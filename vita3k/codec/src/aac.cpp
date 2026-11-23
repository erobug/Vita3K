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

#include <codec/state.h>

#define DEBUG

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>

#include <libavcodec/codec_internal.h>
}

#include <util/log.h>

AacDecoderState::AacDecoderState(uint32_t sample_rate, uint32_t channels)
    : out_channels(channels) {
    codec = avcodec_find_decoder(AV_CODEC_ID_AAC);
    assert(codec);

    context = avcodec_alloc_context3(codec);
    assert(context);

    frame = av_frame_alloc();

    context->codec_type = AVMEDIA_TYPE_AUDIO;
    av_channel_layout_default(&context->ch_layout, channels);
    context->sample_rate = sample_rate;

    int err = avcodec_open2(context, codec, nullptr);
    assert(err == 0);

    swr = nullptr;
    int ret = swr_alloc_set_opts2(&swr,
        &context->ch_layout, AV_SAMPLE_FMT_S16, sample_rate,
        &context->ch_layout, AV_SAMPLE_FMT_FLTP, sample_rate,
        0, nullptr);
    assert(ret == 0);

    ret = swr_init(swr);
    assert(ret == 0);
}

AacDecoderState::~AacDecoderState() {
    av_frame_free(&frame);
    swr_free(&swr);
}

uint32_t AacDecoderState::get(DecoderQuery query) {
    switch (query) {
    // Report the layout swr actually emits, not the layout libavcodec decoded to. swr was
    // configured from the guest's declared layout when the decoder was created, and it keeps
    // that layout even after a HE-AAC/SBR stream switches the decoder context to twice the
    // channels. Answering with context->ch_layout made the caller size the PCM it received as
    // twice the bytes that were really written, so its read pointer ran ahead of the data.
    case DecoderQuery::CHANNELS: return out_channels;
    case DecoderQuery::BIT_RATE: return context->bit_rate;
    case DecoderQuery::SAMPLE_RATE: return context->sample_rate;
    default:
        return 0;
    }
}

bool AacDecoderState::send(const uint8_t *data, uint32_t size) {
    AVPacket *packet = av_packet_alloc();
    packet->data = const_cast<uint8_t *>(data);
    packet->size = size;

    av_frame_unref(frame);
    frame_feedable = false;

    const FFCodec *ff_codec = ffcodec(codec);
    int got_frame = 0;
    const int len = ff_codec->cb.decode(context, frame, &got_frame, packet);

    av_packet_free(&packet);

    if (len < 0) {
        LOG_WARN("Error sending Aac packet: {}.", codec_error_name(len));
        return false;
    }

    es_size_used = static_cast<uint32_t>(len);

    // The decoder consumed the packet but produced no frame. That is not a valid frame to hand
    // to receive(): doing so would push whatever the AVFrame still holds. Report success so the
    // caller advances the ES pointer, but leave frame_feedable false so receive() has nothing
    // to convert.
    if (!got_frame || frame->nb_samples <= 0) {
        LOG_WARN_ONCE("AAC packet produced no frame (len={}, got_frame={}).", len, got_frame);
        return true;
    }

    // A complete frame is available for the next receive().
    frame_feedable = true;

    return true;
}

void AacDecoderState::flush() {
    DecoderState::flush();

    // libavcodec is now reset, but the resampler still holds samples that were decoded from the
    // previous stream. swr_init() drops that backlog.
    if (swr)
        swr_init(swr);

    frame_feedable = false;
    av_frame_unref(frame);
}

bool AacDecoderState::receive(uint8_t *data, DecoderSize *size) {
    // swr_convert's out_count counts sample *frames* (one sample per channel), not interleaved
    // bytes, so the bytes a frame occupies are frames * channels * sizeof(int16_t).
    const uint32_t channels = out_channels ? out_channels : 1;
    const uint32_t bytes_per_frame = sizeof(int16_t) * channels;

    // A feedable frame is the only case where the AVFrame describes real audio. send() calls
    // av_frame_unref() before decoding, which resets format to -1, so the assertion below belongs
    // inside this branch -- check it unconditionally and a perfectly normal drain call (no new
    // frame, waiting for the resampler's backlog) would trip it in a debug build.
    const bool has_frame = frame_feedable && frame->nb_samples > 0;
    if (has_frame) {
        assert(frame->format == AV_SAMPLE_FMT_FLTP);
    }

    const uint32_t frame_frames = has_frame ? static_cast<uint32_t>(frame->nb_samples) : 0;

    // How many sample frames the caller's buffer can take. This limits only the output side, and
    // it must NOT be clamped to this frame's own length: when the resampler still holds audio from
    // earlier frames it can legitimately produce more than one frame in a single call. Clamping to
    // frame_frames there left the backlog un-drainable, so a voice played only its opening.
    uint32_t fits;
    if (output_capacity == UINT32_MAX) {
        // Unknown capacity: allow the frame plus whatever is already queued.
        const int64_t pending = swr_get_delay(swr, context->sample_rate > 0 ? context->sample_rate : 48000);
        fits = frame_frames + (pending > 0 ? static_cast<uint32_t>(pending) : 0) + 0x100;
    } else {
        fits = bytes_per_frame ? (output_capacity / bytes_per_frame) : 0;
    }

    uint32_t produced = 0;
    bool ok = true;

    if (data) {
        // swr_convert's in_count and out_count are independent: supplying more input than the
        // output can take makes the resampler buffer the remainder and return it later. So the
        // *whole* frame is handed over every time -- reducing in_count to match out_count would
        // silently drop the tail, because the next send() replaces the frame.
        //
        // A null input with in_count 0 asks the resampler to emit what it still holds. That is how
        // the tail of a stream is drained once no more packets arrive.
        const uint8_t **input = nullptr;
        int input_frames = 0;
        if (has_frame) {
            input = const_cast<const uint8_t **>(frame->extended_data);
            input_frames = static_cast<int>(frame_frames);
            frame_feedable = false;
        }

        if (fits > 0) {
            const int converted = swr_convert(swr, &data, static_cast<int>(fits), input, input_frames);
            if (converted < 0) {
                // A real conversion failure. Report it rather than returning "no more audio",
                // which the caller cannot tell apart from a normal end of data.
                LOG_WARN("AAC resample failed: {}.", log_hex(static_cast<uint32_t>(converted)));
                ok = false;
            } else {
                produced = static_cast<uint32_t>(converted);
            }
        }
    }
    else if (size) {
        // Query path: report the frame's own sample count without consuming it.
        produced = frame_frames;
    }

    if (size) {
        size->samples = produced;
    }

    return ok;
}

uint32_t AacDecoderState::get_es_size() {
    return es_size_used;
}
