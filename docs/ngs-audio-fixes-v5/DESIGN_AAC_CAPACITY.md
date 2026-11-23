# Bounded-Output AAC Decoding in Vita3K

**Status:** Implemented and locally tested in v5  
**Patch base:** `a631dd35d46747fdb27d7715a0524ea67b0b1a89`  
**Relevant files:**
- `codec/src/aac.cpp`
- `codec/include/codec/state.h`
- `modules/SceAudiodec/SceAudiodecUser.cpp`

## Summary

The HE-AAC/SBR voice issue was caused by a mismatch between the channel count reported to the guest and the channel count actually emitted by the resampler. In addition, the receive path needed an explicit output-capacity limit.

The v5 implementation:

1. Reports the resampler's configured output channel count.
2. Uses the actual sample-frame count returned by `swr_convert()`.
3. Passes the remaining destination capacity to the decoder before writing.
4. Supplies each decoded input frame to `swr_convert()` in full, while limiting only the output count.
5. Drains buffered resampler output before accepting another packet, subject to the remaining destination capacity.
6. Clears resampler state when the decoder is flushed.

A 1/4-capacity stress test changed from a measured 75% sample loss in the earlier truncating implementation to zero observed sample loss with the v5 implementation.

## 1. Failure mechanism

The affected voice streams use HE-AAC with Spectral Band Replication (SBR). The guest creates the decoder using its core-stream parameters, while the decoded frame and decoder context may expose different channel information after decoding.

A representative case declared:

- Guest output: 24 kHz, mono.
- Decoded frame/context observed after decoding: 48 kHz, stereo.
- Resampler output: one channel, matching the guest-declared output layout.

For a 2,048-sample frame in this case:

```text
Actual PCM written:  2048 frames × 1 channel × 2 bytes = 4096 bytes
Reported to guest:   2048 frames × 2 channels × 2 bytes = 8192 bytes
```

`DecoderQuery::CHANNELS` previously returned the channel count from `context->ch_layout`. After decoding, libavcodec could update that layout to two channels, although the resampler was still configured to emit one channel.

The guest used the reported size to advance its PCM read/write accounting. It therefore advanced past data that had not been written, producing gaps and the observed half-speed, stuttering and truncated voice playback.

The reported-size mismatch was observed even though the corresponding 4,096-byte PCM write fit the 4,096-byte destination limit. The separate capacity limit is still necessary to prevent writes from exceeding the remaining destination space in smaller-buffer cases.

## 2. Design requirements

The implementation follows four requirements:

- **Keep input consumption separate from output delivery.** The complete decoded frame must be supplied to the resampler; reducing `in_count` discards input that will be lost when the next packet replaces the `AVFrame`.
- **Bound output in bytes.** Convert the remaining byte capacity to sample frames per channel using `out_channels * sizeof(int16_t)`.
- **Use the conversion result.** `swr_convert()` returns the number of output sample frames actually produced. Do not infer the produced count from `frame->nb_samples`.
- **Drain buffered output.** The caller must continue receiving output after a packet until the resampler has no more immediately available samples or the destination buffer is full.

`swresample` buffers input that cannot be emitted because the current output space is insufficient. The implementation relies on this behavior rather than maintaining a second pending-PCM buffer.

## 3. Decoder-side implementation

### 3.1 Output channel reporting

`AacDecoderState` stores the output channel count used to configure the resampler:

```cpp
uint32_t out_channels = 1;
```

`DecoderQuery::CHANNELS` returns `out_channels`, not the potentially changed `context->ch_layout`.

The output byte count is therefore based on the actual configured output layout:

```text
output bytes = converted sample frames × out_channels × sizeof(int16_t)
```

### 3.2 Feed each decoded frame once

`frame_feedable` tracks whether the current decoded `AVFrame` still needs to be supplied to the resampler.

When a frame is available:

- Set `input` to `frame->extended_data`.
- Set `input_frames` to the full `frame->nb_samples`.
- Clear `frame_feedable` so the same frame is not submitted twice.

When there is no new frame to feed, call `swr_convert()` with a null input and zero input frames to drain buffered output.

### 3.3 Limit only the output

The caller supplies `output_capacity` in bytes. The decoder converts that capacity into the maximum number of output sample frames that fit:

```text
fits = output_capacity / (out_channels × sizeof(int16_t))
```

The conversion uses:

```cpp
const int converted = swr_convert(
    swr,
    &data,
    static_cast<int>(fits),
    input,
    input_frames
);
```

`input_frames` remains the complete decoded frame count. `fits` limits only the output side.

Do not cap `fits` at the current frame's sample count. The resampler may already contain samples buffered from earlier input, so the number of samples it can emit in one call can exceed the current frame length.

The caller accounts for `converted`, not `frame->nb_samples`.

## 4. Caller-side drain loop

`decode_audio_frames()` now performs the following sequence for each compressed packet:

1. Stop if the guest PCM destination is already full.
2. Submit the packet with `send()`.
3. Advance the elementary-stream input accounting for the packet consumed.
4. Repeatedly set `output_capacity` to the remaining PCM bytes and call `receive()`.
5. Stop draining when `receive()` produces zero samples or the destination is full.
6. Do not submit another packet until the current packet's available resampler output has been drained.

This loop is required. Calling `receive()` only once after each `send()` allows buffered samples to accumulate. The observed failure modes during development included voice stopping after its opening segment and audio being delayed until a later menu transition.

### 4.1 `PartlyDecode()` capacity handling

`PartlyDecode()` writes into its own scratch vector rather than directly into the guest PCM block. The guest output-capacity limit therefore does not apply to that path. It resets `output_capacity` to `UINT32_MAX` before using the query-and-write flow, so a limit left by `decode_audio_frames()` cannot unintentionally restrict the scratch-buffer operation.

### 4.2 Packets that produce no frame

A successful packet decode does not always produce an output frame.

The send path now distinguishes:

| Decoder result | Return/State behavior |
|---|---|
| Decode error (`len < 0`) | Return failure |
| Packet consumed, no frame produced (`got_frame == 0`) | Return success so ES accounting can advance; leave `frame_feedable` false |
| Frame produced (`got_frame == 1`) | Return success and set `frame_feedable` true |

This avoids treating a stale or empty `AVFrame` as valid output in release builds where `assert()` may be disabled.

## 5. Resampler lifecycle

`DecoderState::flush()` flushes the codec, but the AAC decoder also owns buffered state inside `SwrContext`.

`AacDecoderState::flush()` therefore:

1. Calls the base decoder flush.
2. Reconfigures the resampler using the decoder's stored output channel count and output sample rate, clearing samples buffered from the previous stream.
3. Resets `frame_feedable`.
4. Unreferences the previous `AVFrame`.
5. Logs a warning if resampler reconfiguration fails.

This prevents buffered samples from a previous stream from leaking into a subsequent decode after `sceAudiodecClearContext()`.

Per-frame resampler reconfiguration based on changing decoder metadata was tested and reverted. Rebuilding `SwrContext` during packet processing frees its internal buffer and can discard samples that have not yet been delivered. The v5 implementation retains the existing input/output layout and sample-rate policy during normal decoding; the `CHANNELS` query correction is a separate change.

## 6. Validation

### 6.1 Reduced-capacity stress test

The guest PCM capacity was reduced to one quarter of its normal value (4,096 → 1,024 bytes) during *Kiniro no Corda 2ff* playback.

| Metric | Earlier truncating implementation | v5 |
|---|---:|---:|
| Process | Remained alive | Remained alive |
| Samples lost per 1,024-sample frame | 768 | 0 observed |
| Loss ratio | 75% | 0% observed |
| Playback | Severely distorted | Reported as normal |

The earlier implementation used the same reduced value for both `in_count` and `out_count`, so only 256 of 1,024 input sample frames were submitted to the resampler on each call. The remaining 768 samples were discarded when the next packet replaced the frame.

The v5 path submits the complete input frame and limits only the output.

### 6.2 Normal-capacity regression test

On *Kiniro no Corda 2ff* (`PCSG01124`):

- Seven decoder instances were created without reported errors.
- Multiple voice dialogue segments were played continuously.
- The process remained stable.
- Voice playback was reported as normal.

The wider v5 game tests also include *Harukanaru Toki no Naka de 3 Ultimate* (`PCSG00992`) and *Kiniro no Corda 4* (`PCSH00275`), but the reduced-capacity measurement above was performed on *Kiniro no Corda 2ff*.

These are Windows local gameplay tests, not a cross-platform or full-playthrough validation.

## 7. Approaches not used in the final implementation

The following points are included to distinguish the final implementation from earlier design experiments:

- **Do not reduce `in_count` to fit the output buffer.** This permanently drops the part of the decoded frame that was not submitted.
- **Do not cap output to the current input frame length.** Buffered output from earlier calls may make more than one frame's worth of samples available.
- **Do not rely on a separate `pending_frames()` API.** The final caller drains `receive()` after each `send()` until it produces no more samples or the destination is full.
- **Do not rebuild the resampler for each observed frame-parameter change.** Rebuilding frees its internal buffer and can discard undelivered samples.

## 8. Scope

This note covers the bounded-output AAC/HE-AAC path and its interaction with `SceAudiodec`. It does not describe the separate NGS guest-memory, ATRAC9 scheduling, DSP, or general audio-output changes included elsewhere in the v5 patch.
