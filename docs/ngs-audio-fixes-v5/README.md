# Vita3K NGS and Audio Compatibility Fixes (v5)

**Status:** Work in progress; submitted for technical discussion  
**Patch base:** Vita3K commit `a631dd35d46747fdb27d7715a0524ea67b0b1a89`  
**Test environment:** Windows, SDL audio backend, NGS enabled

This patch addresses NGS crashes and missing audio in *Harukanaru Toki no Naka de 3 Ultimate*, as well as HE-AAC/SBR voice playback problems in several *Kiniro no Corda* titles. It also includes related NGS lifecycle, DSP, PCM conversion, and audio-output timing fixes.

The patch was tested against the base commit above. That commit identifies the version used for v5; it should not be interpreted as a claim that it is still the current upstream HEAD.

## Tested titles

| Title | Title ID | Observed before the fixes | Result with v5 |
|---|---|---|---|
| *Harukanaru Toki no Naka de 3 Ultimate* | `PCSG00992` | Crash or missing audio with NGS enabled | BGM, voice and sound effects played normally |
| *Kiniro no Corda 2ff* | `PCSG01124` | Voice played at roughly half speed, stuttered, and was cut off | Voice playback was reported as normal |
| *Kiniro no Corda 4* | `PCSH00275` | Same voice playback problems | Voice playback was reported as normal |
| *Kiniro no Corda 3* series | `PCSG01211` / `PCSG01212` | Same voice playback problems | Voice playback was reported as normal |

These are local gameplay tests, not full playthroughs.

## Main changes

### AAC / HE-AAC SBR

- `DecoderQuery::CHANNELS` now reports the channel count configured for the resampler's output, rather than relying on `AVCodecContext::ch_layout` after decoding may have changed it.
- The decoder reports the number of sample frames actually returned by `swr_convert()`.
- The caller passes the remaining PCM output capacity to the decoder before each receive operation.
- Each decoded input frame is supplied to the resampler in full and only the output count is bounded. The resampler retains input that cannot yet be emitted.
- The caller drains pending resampler output after each packet, until no more output is available or the destination buffer is full.
- Decoder flush handling clears stale resampler data and resets frame state.

The AAC-specific design and test details are documented in [DESIGN_AAC_CAPACITY.md](DESIGN_AAC_CAPACITY.md).

### NGS memory layout and lifecycle

Some host-side members embedded in NGS objects allocated with `placement new` in guest memory were observed to be overwritten by the game. The affected state was moved to host-side tables. In the tested `Voice` layout, this reduced `sizeof(Voice)` from 248 to 152 bytes.

Other related fixes include:

- Use `output_count`, rather than `MAX_OUTPUT_PORT`, when calculating patch allocation size.
- Treat an ATRAC9 voice with no buffer yet as not-yet-started, rather than completed. This prevents the scheduler from stopping the voice before the game supplies its first buffer.
- Use a consistent full-width host-pointer key (`uintptr_t`) for host-side state lookup and cleanup.
- Ensure system release follows the module voice-state cleanup path.
- Preserve Filter and Equalizer secondary product outputs, including on early-return paths.
- Clarify `VoicePlay` state handling, including the `FINALIZING` case and active-voice requeue behavior.
- Construct and destroy the Reverb runtime state as a normal host-side object.

### DSP coverage and limitations

The previously unimplemented Envelope, Filter, Equalizer and Reverb paths now have approximate processing implementations:

- **Envelope:** ADSR-style interpolation using the guest-provided `SceNgsEnvelopePoint` list.
- **Filter:** descriptive parameters are converted to RBJ biquad coefficients; coefficient-form parameters are used directly.
- **Equalizer:** four cascaded biquad stages.
- **Reverb:** a Schroeder-style structure using four comb filters and two all-pass filters, with a cap on wet gain.

These are approximations based on the supplied parameters, not verified cycle-accurate reproductions of the original hardware behavior.

**Pauser remains pass-through.** The guest state structure `SceNgsPauserStates` is undocumented; inventing a layout could corrupt guest memory. Voice-level pause behavior is handled by the scheduler and `VoiceState`.

### General audio path

- PCM conversion now uses the actual sample count returned by `swr_convert()`, avoiding stale tail data being treated as valid output.
- Audio output pacing uses an absolute deadline, a short sleep, and a brief spin/yield phase. If the callback falls more than one period behind, the deadline is resynchronized instead of attempting to catch up in a burst.
- The SDL haptic fallback is retained.

The output-pacing measurement is described briefly below. The general audio changes affect a wider set of titles than the NGS-specific changes and have not been exhaustively regression-tested across all games.

## Validation and limitations

- **Platform:** Windows only.
- **Audio backend:** SDL. The tested configuration used `audio-backend: SDL` and `ngs-enable: true`. Cubeb was reported as unstable in some tested titles and was not used for the results above.
- **Game coverage:** the titles in the table were tested locally; no full playthrough validation was performed.
- **Cross-platform coverage:** not tested.
- **Broader regression coverage:** general PCM and output-timing changes may affect titles outside the NGS test set; those titles have not been systematically tested.

### Remaining guest-memory boundary

The changes move the members for which overwrite evidence was observed. They do **not** migrate every host STL container out of guest-allocated objects. The following remain:

| Object | Remaining host-side containers |
|---|---|
| `Voice` | `inputs` (`std::vector<std::vector<uint8_t>>`) |
| `Rack` | `voices` and `modules` vectors |
| `Rack` / `System` | `allocator.blocks` vector inherited through `MempoolObject` |

No overwrite of these remaining regions was observed in the tests described here. They were left in place to avoid an unverified structural change and additional per-sample lookup overhead. This is a documented boundary, not a claim that all host containers have been moved out of guest memory.

### Audio output pacing measurement

On Windows, continuous playback of *Kiniro no Corda 2ff* was measured over 9,728 callbacks (19 measurement blocks), with a nominal block period of 10,666 μs:

| Metric | Measured deviation |
|---|---:|
| Mean | 108 μs |
| P50 | 1 μs |
| P99 | 708 μs |
| Two scheduling outliers | 85,278 μs and 173,483 μs |

The outliers indicate that the host thread can occasionally be descheduled for multiple audio periods. The resynchronization logic prevents a catch-up burst; the trade-off is occasional latency. Playback was reported as normal during this test. No further timer changes are proposed without additional measurements.

### Build note

After switching to a different Git commit, use a clean rebuild rather than reusing stale object files. A previous incremental build reused objects from another commit and produced an ABI-mismatched executable that crashed at startup; a clean rebuild of the same source commit worked normally.

## Suggested review boundaries

The changes span several areas and may be easier to review as separate units:

1. AAC / HE-AAC SBR decoding and the `SceAudiodec` output-capacity path.
2. NGS guest/host state separation, voice scheduling and lifecycle handling.
3. NGS DSP implementations.
4. General PCM conversion and audio-output pacing.

The exact PR split can be discussed with maintainers. This branch is a work-in-progress implementation, not a claim that all of these areas are ready to merge together.

## License

This work is based on Vita3K and is distributed under GPL-2.0, subject to the project’s licensing terms. Vita3K and the games remain the property of their respective copyright holders. No game data is included.
