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

#include <ngs/modules/equalizer.h>
#include <util/log.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace ngs {

// [ngs-audio] Four-band parametric equalizer.
//
// Each of the SCE_NGS_MAX_EQ_FILTERS stages is a biquad, chained in series over the same block.
// A raw description (SceNgsParamEqParams) or ready-made coefficients (SceNgsParamEqParamsCoEff)
// may be supplied; the coefficient form is used as-is.
//
// Per-stage, per-channel history lives in scratch_data so it persists between calls.
namespace {
constexpr float kTwoPi = 6.28318530717958647692f;

struct EqStageHistory {
    float x1 = 0.0f;
    float x2 = 0.0f;
    float y1 = 0.0f;
    float y2 = 0.0f;
};

// RBJ peaking filter: boost or cut around fFrequency by fGain dB with quality factor fResonance.
void design_eq_band(const SceNgsParamFilter &band, float sample_rate, SceNgsParamCoEff &out) {
    out = { 1.0f, 0.0f, 0.0f, 0.0f, 0.0f };

    if (band.eFilterMode == SCE_NGS_FILTER_MODE_OFF || sample_rate <= 0.0f) {
        return;
    }

    const float nyquist = sample_rate * 0.5f;
    const float f = std::clamp(band.fFrequency, 10.0f, nyquist * 0.98f);
    const float w0 = kTwoPi * f / sample_rate;
    const float cosw0 = std::cos(w0);
    const float sinw0 = std::sin(w0);

    float q = band.fResonance > 0.0f ? band.fResonance : 0.7071f;
    q = std::clamp(q, 0.1f, 20.0f);
    const float alpha = sinw0 / (2.0f * q);
    const float A = std::pow(10.0f, band.fGain / 40.0f);

    const float b0 = 1.0f + alpha * A;
    const float b1 = -2.0f * cosw0;
    const float b2 = 1.0f - alpha * A;
    const float a0 = 1.0f + alpha / A;
    const float a1 = -2.0f * cosw0;
    const float a2 = 1.0f - alpha / A;

    const float inv_a0 = (std::fabs(a0) > 1e-9f) ? (1.0f / a0) : 0.0f;
    out.fB0 = b0 * inv_a0;
    out.fB1 = b1 * inv_a0;
    out.fB2 = b2 * inv_a0;
    out.fA1 = a1 * inv_a0;
    out.fA2 = a2 * inv_a0;
}

bool is_unity(const SceNgsParamCoEff &c) {
    return c.fB0 == 1.0f && c.fB1 == 0.0f && c.fB2 == 0.0f && c.fA1 == 0.0f && c.fA2 == 0.0f;
}
} // namespace

bool EqualizerModule::process(KernelState &kern, const MemState &mem, const SceUID thread_id, ModuleData &data, std::unique_lock<std::recursive_mutex> &scheduler_lock, std::unique_lock<std::mutex> &voice_lock) {
    if (!data.parent->products[0].data) {
        return false;
    }

    // An equalizer Voice Definition has four outputs carrying the same signal. Publish the aliases
    // before any early return below: a bypassed equalizer, an unknown parameter block or a stage
    // that is effectively flat all leave the audio untouched, but a Definition that routes ports
    // 1-3 still needs them populated. The scheduler clears every product slot each update.
    data.parent->products[1] = data.parent->products[0];
    data.parent->products[2] = data.parent->products[0];
    data.parent->products[3] = data.parent->products[0];

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

    SceNgsParamCoEff coeffs[SCE_NGS_MAX_EQ_FILTERS] {};

    if (desc->id == SCE_NGS_PARAM_EQ_COEFF_STRUCT_ID) {
        const SceNgsParamEqParamsCoEff *p = data.get_parameters<SceNgsParamEqParamsCoEff>(mem);
        for (int i = 0; i < SCE_NGS_MAX_EQ_FILTERS; ++i) {
            coeffs[i] = p->filterCoEff[i];
        }
    } else if (desc->id == SCE_NGS_PARAM_EQ_STRUCT_ID) {
        const SceNgsParamEqParams *p = data.get_parameters<SceNgsParamEqParams>(mem);
        for (int i = 0; i < SCE_NGS_MAX_EQ_FILTERS; ++i) {
            design_eq_band(p->filter[i], sample_rate, coeffs[i]);
        }
    } else {
        return false;
    }

    data.ensure_scratch_size(sizeof(EqStageHistory) * SCE_NGS_MAX_EQ_FILTERS * 2);
    EqStageHistory *hist = reinterpret_cast<EqStageHistory *>(data.scratch_data.data());
    if (!hist) {
        return false;
    }

    for (int stage = 0; stage < SCE_NGS_MAX_EQ_FILTERS; ++stage) {
        const SceNgsParamCoEff &c = coeffs[stage];
        if (is_unity(c)) {
            continue;
        }

        for (int ch = 0; ch < 2; ++ch) {
            EqStageHistory &h = hist[stage * 2 + ch];
            for (int32_t i = 0; i < granularity; ++i) {
                const float x = samples[i * 2 + ch];
                const float y = c.fB0 * x + c.fB1 * h.x1 + c.fB2 * h.x2
                    - c.fA1 * h.y1 - c.fA2 * h.y2;

                h.x2 = h.x1;
                h.x1 = x;
                h.y2 = h.y1;
                h.y1 = y;

                samples[i * 2 + ch] = std::clamp(y, -1.0f, 1.0f);
            }
        }
    }

    return false;
}
} // namespace ngs
