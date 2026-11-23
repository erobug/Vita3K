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

#include <ngs/modules/filter.h>
#include <util/log.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace ngs {

// [ngs-audio] Biquad filter module.
//
// The hardware accepts either a descriptive parameter set (SceNgsParamFilter: mode / frequency /
// resonance / gain) or pre-computed coefficients (SceNgsFilterParamsCoEff). Both are handled;
// the coefficient form is used verbatim, the descriptive form is turned into RBJ-style biquads.
//
// Direct form I, matching the formula documented next to SceNgsParamCoEff:
//     y[0] = fB0*x[0] + fB1*x[-1] + fB2*x[-2] - fA1*y[-1] - fA2*y[-2]
// Per-channel history lives in scratch_data so it persists across process() calls.
namespace {
constexpr float kTwoPi = 6.28318530717958647692f;

struct FilterHistory {
    float x1 = 0.0f;
    float x2 = 0.0f;
    float y1 = 0.0f;
    float y2 = 0.0f;
};

void design_filter(SceNgsParamFilterMode mode, float freq, float resonance, float gain,
    float sample_rate, SceNgsParamCoEff &out) {
    out = { 1.0f, 0.0f, 0.0f, 0.0f, 0.0f };

    if (mode == SCE_NGS_FILTER_MODE_OFF || sample_rate <= 0.0f) {
        return;
    }

    const float nyquist = sample_rate * 0.5f;
    const float f = std::clamp(freq, 10.0f, nyquist * 0.98f);

    const float w0 = kTwoPi * f / sample_rate;
    const float cosw0 = std::cos(w0);
    const float sinw0 = std::sin(w0);

    float q = resonance > 0.0f ? resonance : 0.7071f;
    q = std::clamp(q, 0.1f, 20.0f);
    const float alpha = sinw0 / (2.0f * q);

    float b0 = 1.0f, b1 = 0.0f, b2 = 0.0f, a0 = 1.0f, a1 = 0.0f, a2 = 0.0f;
    const float A = std::pow(10.0f, gain / 40.0f);

    switch (mode) {
    case SCE_NGS_FILTER_LOWPASS_RESONANT:
    case SCE_NGS_FILTER_LOWPASS_RESONANT_NORMALIZED:
        b0 = (1.0f - cosw0) * 0.5f;
        b1 = 1.0f - cosw0;
        b2 = (1.0f - cosw0) * 0.5f;
        a0 = 1.0f + alpha;
        a1 = -2.0f * cosw0;
        a2 = 1.0f - alpha;
        break;
    case SCE_NGS_FILTER_HIGHPASS_RESONANT:
        b0 = (1.0f + cosw0) * 0.5f;
        b1 = -(1.0f + cosw0);
        b2 = (1.0f + cosw0) * 0.5f;
        a0 = 1.0f + alpha;
        a1 = -2.0f * cosw0;
        a2 = 1.0f - alpha;
        break;
    case SCE_NGS_FILTER_BANDPASS_PEAK:
        b0 = alpha;
        b1 = 0.0f;
        b2 = -alpha;
        a0 = 1.0f + alpha;
        a1 = -2.0f * cosw0;
        a2 = 1.0f - alpha;
        break;
    case SCE_NGS_FILTER_BANDPASS_ZERO:
        b0 = 0.0f;
        b1 = 1.0f;
        b2 = 0.0f;
        a0 = 1.0f + alpha;
        a1 = -2.0f * cosw0;
        a2 = 1.0f - alpha;
        break;
    case SCE_NGS_FILTER_NOTCH:
        b0 = 1.0f;
        b1 = -2.0f * cosw0;
        b2 = 1.0f;
        a0 = 1.0f + alpha;
        a1 = -2.0f * cosw0;
        a2 = 1.0f - alpha;
        break;
    case SCE_NGS_FILTER_PEAK:
        b0 = 1.0f + alpha * A;
        b1 = -2.0f * cosw0;
        b2 = 1.0f - alpha * A;
        a0 = 1.0f + alpha / A;
        a1 = -2.0f * cosw0;
        a2 = 1.0f - alpha / A;
        break;
    case SCE_NGS_FILTER_LOWSHELF: {
        const float sqA = std::sqrt(A);
        const float beta = 2.0f * sqA * alpha;
        b0 = A * ((A + 1.0f) - (A - 1.0f) * cosw0 + beta);
        b1 = 2.0f * A * ((A - 1.0f) - (A + 1.0f) * cosw0);
        b2 = A * ((A + 1.0f) - (A - 1.0f) * cosw0 - beta);
        a0 = (A + 1.0f) + (A - 1.0f) * cosw0 + beta;
        a1 = -2.0f * ((A - 1.0f) + (A + 1.0f) * cosw0);
        a2 = (A + 1.0f) + (A - 1.0f) * cosw0 - beta;
        break;
    }
    case SCE_NGS_FILTER_HIGHSHELF: {
        const float sqA = std::sqrt(A);
        const float beta = 2.0f * sqA * alpha;
        b0 = A * ((A + 1.0f) + (A - 1.0f) * cosw0 + beta);
        b1 = -2.0f * A * ((A - 1.0f) + (A + 1.0f) * cosw0);
        b2 = A * ((A + 1.0f) + (A - 1.0f) * cosw0 - beta);
        a0 = (A + 1.0f) - (A - 1.0f) * cosw0 + beta;
        a1 = 2.0f * ((A - 1.0f) - (A + 1.0f) * cosw0);
        a2 = (A + 1.0f) - (A - 1.0f) * cosw0 - beta;
        break;
    }
    case SCE_NGS_FILTER_LOWPASS_ONEPOLE: {
        const float k = std::exp(-w0);
        b0 = 1.0f - k;
        b1 = 0.0f;
        b2 = 0.0f;
        a0 = 1.0f;
        a1 = -k;
        a2 = 0.0f;
        break;
    }
    case SCE_NGS_FILTER_HIGHPASS_ONEPOLE: {
        const float k = std::exp(-w0);
        b0 = (1.0f + k) * 0.5f;
        b1 = -(1.0f + k) * 0.5f;
        b2 = 0.0f;
        a0 = 1.0f;
        a1 = -k;
        a2 = 0.0f;
        break;
    }
    case SCE_NGS_FILTER_ALLPASS:
        b0 = 1.0f - alpha;
        b1 = -2.0f * cosw0;
        b2 = 1.0f + alpha;
        a0 = 1.0f + alpha;
        a1 = -2.0f * cosw0;
        a2 = 1.0f - alpha;
        break;
    default:
        return;
    }

    const float inv_a0 = (std::fabs(a0) > 1e-9f) ? (1.0f / a0) : 0.0f;
    out.fB0 = b0 * inv_a0;
    out.fB1 = b1 * inv_a0;
    out.fB2 = b2 * inv_a0;
    out.fA1 = a1 * inv_a0;
    out.fA2 = a2 * inv_a0;
}
} // namespace

bool FilterModule::process(KernelState &kern, const MemState &mem, const SceUID thread_id, ModuleData &data, std::unique_lock<std::recursive_mutex> &scheduler_lock, std::unique_lock<std::mutex> &voice_lock) {
    if (!data.parent->products[0].data) {
        return false;
    }

    // Definitions with a filter have two outputs, and both carry the same signal. Publish the
    // alias before any early return below: a bypassed filter, an unknown parameter block or unity
    // coefficients all leave the audio untouched, but a Voice Definition that routes port 1 still
    // needs it to be populated. The scheduler clears every product slot at the start of an update.
    data.parent->products[1] = data.parent->products[0];

    float *samples = reinterpret_cast<float *>(data.parent->products[0].data);
    if (!samples) {
        return false;
    }

    if (data.is_bypassed) {
        return false;
    }

    const float sample_rate = static_cast<float>(data.parent->rack->system->sample_rate);
    const int32_t granularity = data.parent->rack->system->granularity;
    if (granularity <= 0) {
        return false;
    }

    const SceNgsParamsDescriptor *desc = data.get_parameters<SceNgsParamsDescriptor>(mem);
    if (!desc) {
        return false;
    }

    SceNgsParamCoEff coeff {};
    if (desc->id == SCE_NGS_FILTER_PARAMS_COEFF_STRUCT_ID) {
        const SceNgsFilterParamsCoEff *p = data.get_parameters<SceNgsFilterParamsCoEff>(mem);
        coeff = p->params;
    } else if (desc->id == SCE_NGS_FILTER_PARAMS_STRUCT_ID) {
        const SceNgsFilterParams *p = data.get_parameters<SceNgsFilterParams>(mem);
        design_filter(p->params.eFilterMode, p->params.fFrequency, p->params.fResonance,
            p->params.fGain, sample_rate, coeff);
    } else {
        return false;
    }

    // Unity coefficients mean the filter is effectively off.
    if (coeff.fB0 == 1.0f && coeff.fB1 == 0.0f && coeff.fB2 == 0.0f && coeff.fA1 == 0.0f && coeff.fA2 == 0.0f) {
        return false;
    }

    data.ensure_scratch_size(sizeof(FilterHistory) * 2);
    FilterHistory *hist = reinterpret_cast<FilterHistory *>(data.scratch_data.data());
    if (!hist) {
        return false;
    }

    for (int32_t ch = 0; ch < 2; ++ch) {
        FilterHistory &h = hist[ch];
        for (int32_t i = 0; i < granularity; ++i) {
            const float x = samples[i * 2 + ch];
            const float y = coeff.fB0 * x + coeff.fB1 * h.x1 + coeff.fB2 * h.x2
                - coeff.fA1 * h.y1 - coeff.fA2 * h.y2;

            h.x2 = h.x1;
            h.x1 = x;
            h.y2 = h.y1;
            h.y1 = y;

            samples[i * 2 + ch] = std::clamp(y, -1.0f, 1.0f);
        }
    }

    return false;
}
} // namespace ngs
