# MonolithAsset Texture Ingest Bounds Verification

**Date:** 2026-08-02
**Branch:** `jules/codex/asset/texture-ingest-bounds`
**Fork base:** `kunkunGames/monolith-fork@07faaf0583a8190f5aa021c9c6cc22fe556427c5`
**Engine floor:** Unreal Engine 5.7
**Additional engine:** Unreal Engine 5.8
**Status:** PASS

---

## 1. Purpose

Verify that `asset.import_texture_from_bytes` rejects oversized compressed and decoded image payloads before expensive allocations or asset mutation. The change also narrows failed-new-texture garbage collection to Unreal's normal keep flags so rollback does not collect unrelated standalone editor objects.

## 2. Unreal Contract Review

The UE 5.7 and UE 5.8 engine sources expose the same relevant contracts:

| Engine API | Verified behavior | Consequence for Monolith |
|---|---|---|
| `FBase64::GetDecodedDataSize` | Removes trailing padding, divides before multiplying, and returns the exact decoded byte count used by `FBase64::Decode` to size its destination array | The importer can enforce the 256 MiB compressed-byte cap before allocation without the overflow and padded-input overestimate of `GetMaxDecodedDataSize` |
| `IImageWrapper::SetCompressed` | Parses enough metadata for width and height queries; raw pixel decompression is deferred to `GetRaw` | The importer can enforce dimension and BGRA8 byte limits before decompression |
| `FImageCoreUtils::IsImageImportPossible` | Bounds dimensions to `int32` and total pixels to `uint32`, which is looser than this action's editor allocation budget | Monolith still needs its own 16,384-axis and 512 MiB BGRA8 limits |
| `CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS)` | Preserves the engine's normal garbage-collection keep flags | Failed texture rollback can collect its detached object without sweeping unrelated standalone assets |

## 3. Implemented Bounds

| Stage | Limit | Failure contract |
|---|---:|---|
| Base64 preflight | Exact decoded compressed image bytes must be at most 256 MiB | `-32602`, before `FBase64::Decode` allocates its output array |
| Image header | Width and height must each be in `1..16,384` | `-32602`, after `SetCompressed` and before `GetRaw` |
| Decoded surface | Expected `width * height * 4` BGRA8 bytes must be at most 512 MiB | `-32602`, with overflow-safe division before multiplication |
| Raw result | `GetRaw` must return exactly the expected BGRA8 byte count | `-32603`, before package creation or replacement mutation |

The automation fixture starts from a valid 2x2 PNG, rewrites its IHDR to 16,384x16,384, recomputes the PNG CRC, and proves that the action rejects the valid header before the deliberately mismatched tiny IDAT can reach `GetRaw`.

## 4. Build Results

Both hosts resolve their installed engine from a `.uproject` `EngineAssociation`; no alternate engine checkout is hard-coded.

| Engine | Isolation | Result | Evidence |
|---|---|---|---|
| UE 5.7 | `RunUAT BuildPlugin` copied the current plugin sources into an independent `HostProject`; SHA256 for the two changed C++ files, the new internal header, and both updated docs matched the source worktree | PASS, independent UHT plus 529/529 Editor actions; `MonolithAssetTextureIngestActions.cpp` compiled and `UnrealEditor-MonolithAsset.dll` linked; `Result: Succeeded` | `C:\Users\12336\AppData\Roaming\Unreal Engine\AutomationTool\Logs\D+Engine+UE_5.7\UBA-UnrealEditor-Win64-Development.txt` |
| UE 5.8 | Clean 529/529 Editor build, followed by a final incremental build of both changed C++ translation units and relink after the exact base64-size correction | PASS, final 5/5 actions; `Result: Succeeded` | `D:\P4\MonolithFollowupUE58Host\Saved\Logs\UBT_20260802_UE58_Final.log` |

The UE 5.7 BuildPlugin command later attempted an additional `UnrealGame` packaging target and encountered an externally owned UBT mutex. That post-Editor packaging step is not part of this editor-only plugin verification; the independent Editor target had already completed successfully. A shared source-directory UE 5.7 rebuild was also discarded because UE 5.8 UHT products remained under the shared plugin `Intermediate` directory. The accepted UE 5.7 evidence is only the independent copied HostProject build.

## 5. Automation Results

Both engines ran the same focused filter:

```text
MonolithAsset.ImportTextureFromBytes
```

| Engine | Success | Failed | Queue result | Evidence |
|---|---:|---:|---|---|
| UE 5.7 | 13 | 0 | `Automation Test Queue Empty 13 tests performed` | `D:\P4\MonolithAssetBoundsUE57Package\HostProject\Saved\Logs\Automation_20260802_ImportTextureFromBytes_Retry.log` |
| UE 5.8 | 13 | 0 | `Automation Test Queue Empty 13 tests performed` | `D:\P4\MonolithFollowupUE58Host\Saved\Logs\Automation_20260802_ImportTextureFromBytes_Final.log` |

The filter covers basic creation, invalid settings, normal/role preset behavior, exact-path conflict handling, save-failure cleanup, replacement identity and rollback, aliased cooked-data rejection, pure decode-bound arithmetic, and the oversized-IHDR action path.

## 6. Static and Repository Checks

The fork base does not contain `Scripts/ci_static_checks.py` or `.github/monolith-static-ci.json`, so the prescribed command cannot execute in this branch. Loading the current Monolith checker externally also revealed an existing helper-contract mismatch: the current checker expects `SRC_PATHS`, while this fork's `Scripts/check_offline_exe_fresh.py` exports `SRC_PATH`.

For a non-regression comparison, the current checker/config was run against both detached fork base `07faaf05` and this branch with only the two incompatible offline-executable gates disabled symmetrically:

| Tree | Blockers | Advisories | New findings |
|---|---:|---:|---:|
| Detached fork base | 36 | 962 | — |
| Texture bounds branch | 36 | 962 | **0** |

`python Scripts/test_proxy_seed_parity.py` passes and reports all 19 Python/native offline proxy dispatchers in parity. `git diff --check` also passes.

## 7. Visual and Discord Evidence

Screenshot verification is **N/A**. This change affects a headless asset-ingest action, allocation validation, rollback garbage-collection flags, automation, and documentation; it does not change gameplay, runtime UI, editor UI, VFX, animation, materials, or another visual presentation path.

Discord screenshot upload is therefore **N/A**. No `Build\BatchFiles\Script\UploadScreenshotTestsToDiscord.bat` command was run because there is no meaningful PC 1920x1080 visual artifact for this API-only change.

## 8. Result

PASS. Current source bytes compile and link on UE 5.7 and UE 5.8, all 13 focused tests pass on both engines, the valid oversized PNG header is rejected before raw decompression or package mutation, and the change introduces no new finding relative to the exact fork base under the available current static checker surface.
