# Editor GIF Zero-Alpha Normalization Verification

**Date:** 2026-07-12
**Project:** Speed
**Area:** MonolithEditor / temporal GIF encoding
**Change:** Make `editor.capture_system_gif` and `editor.encode_frame_sequence_gif` produce opaque RGB visual-review GIFs by normalizing UE/Slate zero or undefined alpha at the encoder boundary, without modifying the source PNG frames; replace scalar Python/Pillow delays with cumulative nearest-centisecond scheduling so 20/30/60 fps remain drift-free within GIF's 10 ms time unit.
**Status:** Passed: resolver-derived editor build, focused automation, real 24-frame encoding, independent alpha/timing/hash inspection, and PC visual review; Discord mirror is recorded in the Tag Chase verification record

---

## 1. Scope

This record covers the shared GIF encoder used by `capture_system_gif` and `encode_frame_sequence_gif`. The intended contract is that ordered PNG frames remain the durable, unchanged evidence while the derived GIF is an opaque RGB review artifact. GIF delays are whole centiseconds: Python therefore schedules rounded cumulative boundaries, yielding six-frame sequences of `20 fps -> [50,50,50,50,50,50] ms`, `30 fps -> [30,40,30,30,40,30] ms`, and `60 fps -> [20,10,20,20,10,20] ms` rather than truncating every frame independently.

---

## 2. Commands And Results

| Check | Command / Action | Result |
|-------|------------------|--------|
| SpeedEditor build | Resolve the engine from `Speed.uproject`, then run the project primary `SpeedEditor Win64 Development` UBT command. | Passed. The final build compiled `MonolithEditorSystemGifTests.cpp` and `MonolithEditorActions.cpp`, linked `UnrealEditor-MonolithEditor.dll`, and ended `Result: Succeeded`; evidence: `Saved\Logs\TagChase_LobbyBeacon_CaptureNiagara_MonolithGif_CadenceFinalBuild_20260712.log`. |
| Focused automation | Run `Monolith.Editor.Temporal.EncodeFrameSequenceGif`, covering the dependency-free cumulative schedule plus Python/Pillow mixed-alpha serialization, decoded timing, duration metadata, and source-PNG byte preservation. | Passed `2/2`, failed `0`, skipped `0`; run `automation-20260712T031542Z-297DE223`. The pure C++ test bounded every cumulative boundary to `<=5 ms`; the six-frame Pillow decode reported 20 fps `[50,50,50,50,50,50]` (`300 ms`), 30 fps `[30,40,30,30,40,30]` (`200 ms`), and 60 fps `[20,10,20,20,10,20]` (`100 ms`), with six opaque frames, preserved RGB, and byte-identical source PNGs. Report: `Saved\Monolith\AutomationReports\20260712_MonolithEditorGifCadence_Final\AutomationReport_20260712T031548Z.json`. |
| Python encoder output | Invoke `editor.encode_frame_sequence_gif` with the real ordered Tag Chase PNG sequence, `encoder="python"`, and `fps=20`. | Passed with the final binary: `encoder_used=python`, `gif_timing_mode=cumulative_centisecond_rounding`, `gif_delay_unit_ms=10`, nominal/encoded duration `1.2 s`, quantization error `0`, `24` input/encoded frames, `1920x1080`, `fps=20`, and no FPS adjustment; output: `Saved\Screenshots\20260712\TagChase_CaptureNiagara_Final_PC1080p_20fps.gif`. |
| Opaque RGB inspection | Decode every output GIF frame and verify no transparent pixels remain while non-black RGB stored under zero-alpha source pixels remains visible. | Passed. Independent Pillow decode found alpha extrema `[255,255]` on every frame, no transparency metadata, and `255..256` visible RGB colors per frame. The final GIF is `17,077,172` bytes with SHA-256 `F3D0F2D6629FFD9ABAF8BC5BE83D8AD117F40691FE2492D8AAC5193321CAA21D`. |
| Frame timing inspection | Read the per-frame GIF delays and verify every frame reports 50 ms at 20 fps. | Passed. All `24` delays are exactly `50 ms`; total decoded duration is `1200 ms`. |
| Source PNG preservation | Compare the input PNG files before and after encoding. | Passed. The ordered `24`-file name/size/SHA-256 manifest remains `47DF9385A9681938BDBFE49C852E62DE893816914BF64DAE646107D280285A7C` before and after encoding. |
| Visual review | Inspect the representative PNG and generated GIF at the PC `1920x1080` baseline. | Passed. The GIF renders the full arena, UI, and blue Runner instead of a transparent/flat field; `TagChase_CaptureNiagara_Representative_PC1080p.png` shows the visible blue capture plume while the Runner remains blue. |
| Discord screenshot mirror | Upload the final Tag Chase and GIF proof set with one explicit `--files` list. | Passed. After final-code GIF regeneration, the official `--no-attach-files` command again mirrored all `29` files to `\\VR11\Mac_Workspace\Screenshots\20260712`, sent the shared-folder notification, and exited `0`. The earlier attachment aggregate HTTP `413` and successful no-attachment recovery are retained in the Tag Chase record. Full command/result: `Docs\testing\2026-07-12-tagchase-lobby-beacon-role-selection-capture-niagara.md`. |

---

## 3. Acceptance Gates

- Passed: both public editor actions reach the same shared `EncodeGifFromPngFrames` helper and compile with the opaque-RGB contract.
- Passed: the dependency-free timing test validates the shared production helper at 20/30/60 fps even if Python is unavailable, and the Python/Pillow E2E decodes the same six-frame schedules from real GIF files.
- Passed: the mixed-alpha regression test and real PIE frame sequence remain visible instead of becoming transparent or flat-colored.
- Passed: six generated test PNGs remain byte-for-byte identical after three encodes; the real 24-frame PNG manifest is also unchanged.
- Passed at implementation/compile level for both encoders: ffmpeg appends `format=rgb24`, while the available Python/Pillow path passed functional runtime validation. No ffmpeg executable was available for a second local runtime encode.
- Passed: Python reports nominal duration separately from the exact scheduled GIF duration and quantization error; decoded 20/30/60 fps totals are `300/200/100 ms` for six frames. The real 24-frame 20 fps output reports/decodes to `1.2 s`.

---

## 4. Verification Notes

The first focused test execution exposed a test-fixture path issue: `FPaths::ProjectSavedDir()` returned a relative path that the public action correctly treated as project-relative, producing a doubled comparison string. The test normalizes its unique temporary directory with `FPaths::ConvertRelativePathToFull`. A later final-scope audit then found that scalar `duration=1000/FPS` happened to be correct at 20 fps but Pillow truncated it to `30 ms` at 30 fps and `10 ms` at 60 fps, causing cumulative playback drift. The encoder now uses the shared `MonolithEditorGifTiming` helper's integer cumulative-boundary schedule, and the rebuilt `2/2` final run validates both the dependency-free schedule and actual Pillow output. No local ffmpeg executable was available, so this record does not claim runtime array parity for the ffmpeg branch; ordered PNGs remain the authoritative source evidence.
