# Editor GIF FFmpeg Exact Frame-Count Verification

**Date:** 2026-07-22
**Project:** Speed
**Area:** MonolithEditor / temporal GIF encoding
**Change:** Prevent the ffmpeg concat terminal-duration entry from producing an extra visible GIF frame, then fail-close on an `ffprobe` decoded-frame-count postcondition and report probed output metrics.
**Status:** Passed: protected SpeedEditor build, focused Unreal automation `3/3`, corrected PC 1920x1080 GIF encode, independent `ffprobe`/hash inspection, and sidecar/partial cleanup verification.

---

## 1. Root Cause

The shared ffmpeg encoder emits one `file` plus one `duration` line for every selected PNG and repeats the last `file` so ffmpeg honors the final frame's duration. For a 100-frame, 20 fps input this produces 101 concat packets. The existing `fps=20` filter sampled the terminal bookkeeping packet as a visible frame because the output command had no frame-count cap. The action therefore returned the planned `encoded_frame_count=100` while the generated GIF decoded as 101 frames over 5.05 seconds.

The fix retains the repeated terminal entry, because it carries the last duration contract, and adds `-frames:v <selected-frame-count>` to the ffmpeg output. Successful process exit is no longer sufficient: the handler runs `ffprobe -count_frames`, requires the decoded count to equal the selected count, deletes a mismatched or unprobeable output, and only then reports success. The concat manifest now has a collision-safe GUID suffix and `ON_SCOPE_EXIT` cleanup so success, file-write failure, ffmpeg failure, and probe failure cannot leak a text sidecar into screenshot evidence. The response distinguishes `selected_frame_count` from the probed `encoded_frame_count` and includes the probed duration and verification tool.

---

## 2. Reproduction And Fix Evidence

| Check | Input / Command | Result |
|-------|-----------------|--------|
| Existing concat manifest | `SpeedSwitching_WarmUp_Rings_20fps_PC1080p_ffmpeg_frames.txt` | Confirmed 201 lines: 101 `file` entries and 100 `duration 0.05000000` entries; the final `frame_0099.png` is deliberately repeated. |
| Unbounded ffmpeg control | Existing manifest, `fps=20,scale=320:-1,format=rgb24`, no output frame cap | Independent `ffprobe -count_frames` decoded 101 frames; stream and format duration were both `5.050000` seconds. Diagnostic derivative: `Saved\Monolith\GifExactnessDiagnostic_20260722\baseline_unbounded_320.gif`. |
| Exact frame cap | Same manifest/filter plus `-frames:v 100` | Independent `ffprobe -count_frames` decoded exactly 100 frames; stream and format duration were both `5.000000` seconds. Diagnostic derivative: `Saved\Monolith\GifExactnessDiagnostic_20260722\bounded_100_320.gif`. |
| Production postcondition | `TryEncodeGifWithFFmpeg` | Adds the exact output cap, invokes `ffprobe`, fail-closes unless the decoded count matches, and records decoded frame count/duration for the public response. |
| Protected SpeedEditor build | Run the repository-protected editor build with `P4_BUILD_CHANGELIST=1283` and editor launch disabled. | Passed; the rebuilt `MonolithEditor` includes the exact-frame cap, `ffprobe` postcondition, truthful response fields, scoped concat cleanup, and focused automation. |
| Focused automation | Run `Monolith.Editor.Temporal.EncodeFrameSequenceGif` against the rebuilt editor. | Passed `3/3`, failed `0`; run `automation-20260722T011517Z-8863FDE4`. The new `FFmpegExactFrameCount` case encoded 100 source frames at 20 fps, verified 100 decoded frames over 5.000000 seconds, and found no concat sidecar. |
| Corrected PC proof GIF | Encode `Saved\Screenshots\20260722\SpeedSwitching_WarmUpRings\SpeedSwitching_WarmUp_Rings_20fps_PC1080p.gif`, then inspect with `ffprobe`, file metadata, and SHA-256. | Passed: `1920x1080`, 100 decoded frames, `20/1` fps, `5.000000` seconds, `87,732,966` bytes, SHA-256 `8bb1f3d5c8de7d0cad9ef89000b6002ec69dfe6c54a1f5ac72ab89d9cdee2247`. |
| Evidence-directory hygiene | Enumerate `*_ffmpeg_frames*.txt` and `*.partial` beside the corrected proof. | Passed: sidecar count `0`, partial count `0`; the proof root contains no encoder-work files. |

The rejected 101-frame GIF and its 12,201-byte concat sidecar had their content preserved and were moved together to the recoverable path `Saved\CaptureRecovery\20260722-warmup-gif-101-frame`. The corrected proof remains at the canonical screenshot path above.

---

## 3. Acceptance Gates

- Passed: command-level reproduction isolates the extra terminal sample and proves that the exact frame cap produces 100 frames over 5.00 seconds.
- Passed: both `capture_system_gif` and `encode_frame_sequence_gif` still route through the same shared encoder and expose the same ffmpeg frame-count verification metadata.
- Passed: an ffmpeg process exit without a valid `ffprobe` result or with a mismatched decoded count is not reported as success.
- Passed: the unique concat manifest is owned by scope-exit cleanup on every return path; focused automation and the corrected proof directory both confirm no manifest remains after success.
- Passed: the protected SpeedEditor build for changelist 1283 completed with the changed MonolithEditor module.
- Passed: focused `Monolith.Editor.Temporal.EncodeFrameSequenceGif` automation completed `3/3` in run `automation-20260722T011517Z-8863FDE4`.
- Passed: the corrected PC 1920x1080 proof decodes to exactly 100 frames at 20 fps over 5.000000 seconds, with no sidecar or partial output.

---

## 4. Output Contract

For verified ffmpeg output:

- `selected_frame_count` is the input sampling plan.
- `encoded_frame_count` is the count decoded by `ffprobe` from the output GIF.
- `gif_frame_count_verified` is `true` and `gif_probe_tool` is `ffprobe`.
- `encoded_gif_duration_seconds` is the duration decoded by `ffprobe`.
- `gif_timing_mode` is `ffmpeg_fps_filter_exact_frame_cap`.
- `gif_duration_seconds` / `nominal_gif_duration_seconds` remain the compatibility value `selected_frame_count / fps`; `gif_duration_quantization_error_seconds` is actual minus nominal.
