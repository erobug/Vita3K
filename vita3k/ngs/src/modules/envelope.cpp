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

#include <ngs/modules/envelope.h>
#include <util/log.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace ngs {

// [ngs-audio] ADSR-style envelope.
//
// The envelope walks the SceNgsEnvelopePoint list supplied by the guest. Each point states how
// long it takes (uMsecsToNextPoint) to reach fAmplitude from the previous point, and whether the
// interpolation is linear or curved. uReleaseMsecs applies once the voice is keyed off and the
// last point is reached; uLoopStart/nLoopEnd allow looping a segment.
//
// State lives in the guest-visible SceNgsEnvelopeStates block so it survives between calls and
// stays consistent with what the title reads back through sceNgsVoiceGetStateData.
//
// Modules inside a single voice chain pass audio through products[0].data (see PlayerModule and
// InputMixerModule); inputs.inputs is the cross-voice path and is not used here.
namespace {
constexpr float kTwoPi = 6.28318530717958647692f;

float interpolate(float from, float to, float t, SceNgsEnvelopeCurveType curve) {
    t = std::clamp(t, 0.0f, 1.0f);
    if (curve == SCE_NGS_ENVELOPE_CURVED) {
        // Smooth cosine ease approximating the hardware's curved segments.
        const float eased = 0.5f - 0.5f * std::cos(t * 3.14159265358979323846f);
        return from + (to - from) * eased;
    }
    return from + (to - from) * t;
}
} // namespace

bool EnvelopeModule::process(KernelState &kern, const MemState &mem, const SceUID thread_id, ModuleData &data, std::unique_lock<std::recursive_mutex> &scheduler_lock, std::unique_lock<std::mutex> &voice_lock) {
    if (data.is_bypassed) {
        return false;
    }

    if (!data.parent->products[0].data) {
        return false;
    }

    float *samples = reinterpret_cast<float *>(data.parent->products[0].data);
    if (!samples) {
        return false;
    }

    const int32_t sample_rate = data.parent->rack->system->sample_rate > 0
        ? data.parent->rack->system->sample_rate
        : 48000;
    const int32_t granularity = data.parent->rack->system->granularity;
    if (granularity <= 0) {
        return false;
    }

    const SceNgsEnvelopeParams *params = data.get_parameters<SceNgsEnvelopeParams>(mem);
    SceNgsEnvelopeStates *state = data.get_state<SceNgsEnvelopeStates>();

    // Without valid parameters forward the audio rather than silencing it.
    if (!params || !state || params->desc.id != SCE_NGS_ENVELOPE_PARAMS_STRUCT_ID
        || params->uNumPoints == 0 || params->uNumPoints > SCE_NGS_ENVELOPE_MAX_POINTS) {
        return false;
    }

    const int32_t num_points = static_cast<int32_t>(params->uNumPoints);
    if (state->nCurrentPoint < 0) {
        state->nCurrentPoint = 0;
    }
    if (state->nCurrentPoint >= num_points) {
        state->nCurrentPoint = num_points - 1;
    }

    const float ms_per_sample = 1000.0f / static_cast<float>(sample_rate);

    for (int32_t i = 0; i < granularity; ++i) {
        const SceNgsEnvelopePoint &point = params->envelopePoints[state->nCurrentPoint];
        const float segment_len = static_cast<float>(point.uMsecsToNextPoint);
        const float target = point.fAmplitude;

        float height;
        if (state->nReleasing && params->uReleaseMsecs > 0) {
            const float step = ms_per_sample / static_cast<float>(params->uReleaseMsecs);
            state->fReleaseScale = std::max(0.0f, state->fReleaseScale - step);
            height = state->fCurrentHeight * state->fReleaseScale;
        } else if (segment_len <= 0.0f) {
            state->fCurrentHeight = target;
            state->fPosition = 0.0f;
            height = state->fCurrentHeight;
        } else {
            const float t = state->fPosition / segment_len;
            height = interpolate(state->fCurrentHeight, target, t, point.eCurveType);
        }

        samples[i * 2] *= height;
        samples[i * 2 + 1] *= height;

        if (!state->nReleasing) {
            state->fPosition += ms_per_sample;
            if (segment_len > 0.0f && state->fPosition >= segment_len) {
                state->fCurrentHeight = target;
                state->fPosition -= segment_len;

                const int32_t next = state->nCurrentPoint + 1;
                if (next < num_points) {
                    state->nCurrentPoint = next;
                } else if (params->nLoopEnd > 0 && params->uLoopStart < params->uNumPoints) {
                    state->nCurrentPoint = static_cast<int32_t>(params->uLoopStart);
                } else {
                    state->fPosition = 0.0f;
                }
            }
        }
    }

    state->fCurrentHeight = std::clamp(state->fCurrentHeight, 0.0f, 1.0f);

    return false;
}
} // namespace ngs
