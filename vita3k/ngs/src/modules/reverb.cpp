// Vita3K emulator project
// Copyright (C) 2026 Vita3K team
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License along
// with this program; if not, write to the Free Software Foundation, Inc.,
// 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.

#include <ngs/modules/reverb.h>
#include <util/log.h>

#include <algorithm>
#include <cmath>
#include <vector>

namespace ngs {

namespace {
constexpr int kCombCount = 4;

// Comb delay lengths (samples at 48 kHz), mutually prime to avoid a metallic ring.
constexpr int kCombTuning[kCombCount] = { 1116, 1188, 1277, 1356 };
constexpr int kAllpassTuning[2] = { 556, 441 };
constexpr float kAllpassFeedback = 0.5f;

float clamp01(float v) {
    return std::clamp(v, 0.0f, 1.0f);
}
} // namespace

void ReverbModuleState::Comb::init(int length, float fb, float dampening) {
    buffer.assign(static_cast<size_t>(length), 0.0f);
    index = 0;
    feedback = fb;
    damp = dampening;
    filter_store = 0.0f;
}

float ReverbModuleState::Comb::process(float input) {
    if (buffer.empty()) {
        return input;
    }

    const float output = buffer[index];
    filter_store = output * (1.0f - damp) + filter_store * damp;
    buffer[index] = input + filter_store * feedback;

    if (++index >= static_cast<int>(buffer.size())) {
        index = 0;
    }

    return output;
}

void ReverbModuleState::Allpass::init(int length) {
    buffer.assign(static_cast<size_t>(length), 0.0f);
    index = 0;
}

float ReverbModuleState::Allpass::process(float input) {
    if (buffer.empty()) {
        return input;
    }

    const float buffered = buffer[index];
    const float output = -input + buffered;
    buffer[index] = input + buffered * kAllpassFeedback;

    if (++index >= static_cast<int>(buffer.size())) {
        index = 0;
    }

    return output;
}

std::unique_ptr<ModuleRuntimeState> ReverbModule::create_runtime_state() const {
    return std::make_unique<ReverbModuleState>();
}

void ReverbModule::cleanup_voice_state(ModuleData &data) {
    data.runtime_state.reset();
}

// [ngs-audio] Combined room reverb.
//
// The guest hands us an EAX/OpenAL-style descriptor (fRoom, fDecayTime, fReflections, fReverb,
// fDiffusion, ...). Reproducing the hardware exactly is out of scope, so this is a compact
// Schroeder network: four parallel comb filters feeding two series allpasses.
//
// The dry signal is always preserved and the wet gain is bounded, so a title that expects reverb
// but receives a slightly different tail still sounds correct instead of silent or distorted.
//
// The delay lines own std::vector storage, so the state is a host object (ModuleData::
// runtime_state) with real construction and destruction. It used to be raw bytes inside
// scratch_data reached through reinterpret_cast and memset, which never constructed the vectors
// and wiped their internal pointers whenever the network was re-tuned.
bool ReverbModule::process(KernelState &kern, const MemState &mem, const SceUID thread_id, ModuleData &data, std::unique_lock<std::recursive_mutex> &scheduler_lock, std::unique_lock<std::mutex> &voice_lock) {
    if (!data.parent || !data.parent->products[0].data) {
        return false;
    }

    float *samples = reinterpret_cast<float *>(data.parent->products[0].data);
    if (!samples) {
        return false;
    }

    if (data.is_bypassed) {
        return false;
    }

    const int32_t granularity = data.parent->rack->system->granularity;
    if (granularity <= 0) {
        return false;
    }

    const float sample_rate = static_cast<float>(data.parent->rack->system->sample_rate > 0
            ? data.parent->rack->system->sample_rate
            : 48000);

    const SceNgsParamsDescriptor *desc = data.get_parameters<SceNgsParamsDescriptor>(mem);
    if (!desc || (desc->id != SCE_NGS_REVERB_PARAMS_STRUCT_ID && desc->id != SCE_NGS_REVERB_PARAMS_STRUCT_ID_V2)) {
        return false;
    }

    const SceNgsReverbParams *params = data.get_parameters<SceNgsReverbParams>(mem);
    if (!params) {
        return false;
    }

    // Wet/dry balance. fReverb is the wet level; fDryMB trims the dry path.
    const float wet_db = std::clamp(params->fReverb, -10000.0f, 0.0f);
    const float wet = (wet_db <= -10000.0f) ? 0.0f : std::pow(10.0f, wet_db / 20.0f) * 0.35f;
    const float dry = clamp01(1.0f - std::clamp(params->fDryMB, 0.0f, 1.0f) * 0.5f);

    if (wet <= 0.0001f) {
        return false;
    }

    if (!data.runtime_state) {
        data.runtime_state = std::make_unique<ReverbModuleState>();
        if (!data.runtime_state) {
            return false;
        }
    }

    auto *state = static_cast<ReverbModuleState *>(data.runtime_state.get());

    if (!state->initialised || state->sample_rate != sample_rate) {
        // Re-tuning reallocates every delay line. The vectors are constructed members here, so
        // assign() is well defined -- unlike memset-ing the whole object as before.
        state->sample_rate = sample_rate;

        // Decay time drives comb feedback; keep it inside a stable range.
        const float decay = std::clamp(params->fDecayTime, 0.1f, 20.0f);
        const float scale = sample_rate / 48000.0f;
        const float feedback = std::clamp(std::pow(10.0f, -3.0f * 1.0f / (decay * 10.0f)), 0.0f, 0.98f);
        const float damping = clamp01(1.0f - std::clamp(params->fDiffusion, 0.0f, 1.0f) * 0.6f);

        for (int c = 0; c < kCombCount; ++c) {
            for (int ch = 0; ch < 2; ++ch) {
                const int len = std::max(8, static_cast<int>(static_cast<float>(kCombTuning[c]) * scale));
                state->combs[c][ch].init(len, feedback, damping);
            }
        }
        for (int a = 0; a < 2; ++a) {
            for (int ch = 0; ch < 2; ++ch) {
                const int len = std::max(8, static_cast<int>(static_cast<float>(kAllpassTuning[a]) * scale));
                state->allpasses[a][ch].init(len);
            }
        }
        state->initialised = true;
    }

    for (int ch = 0; ch < 2; ++ch) {
        for (int32_t i = 0; i < granularity; ++i) {
            const float input = samples[i * 2 + ch];

            float summed = 0.0f;
            for (int c = 0; c < kCombCount; ++c) {
                summed += state->combs[c][ch].process(input);
            }
            summed /= static_cast<float>(kCombCount);

            summed = state->allpasses[0][ch].process(summed);
            summed = state->allpasses[1][ch].process(summed);

            samples[i * 2 + ch] = std::clamp(input * dry + summed * wet, -1.0f, 1.0f);
        }
    }

    return false;
}

} // namespace ngs
