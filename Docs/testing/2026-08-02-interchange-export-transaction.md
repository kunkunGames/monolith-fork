# Monolith Interchange Export Transaction Verification

**Date:** 2026-08-02 (KST)
**Branch:** `jules/codex/interchange/export-atomicity`
**Base:** `contrib/master` at `07faaf0583a8190f5aa021c9c6cc22fe556427c5`
**Engine floor:** Unreal Engine 5.7
**Engine ceiling tested:** Unreal Engine 5.8
**Status:** Passed

---

## 1. Scope and Confirmed Defect

Before this change, `interchange.export_asset` passed the caller's final `file_path` directly to `UExporter::RunAssetExportTask`. A failed exporter could therefore leave a partial final file, and replacement could damage the previous destination before the action knew whether the complete declared output set succeeded.

The defect and the compatible fix were checked against both installed engine sources:

| Contract | UE 5.7 and UE 5.8 evidence |
|----------|----------------------------|
| Direct exporter writes | `Engine\Source\Runtime\Engine\Private\UnrealExporter.cpp`, `UExporter::RunAssetExportTask`, writes `Task->Filename` or each `GetUniqueFilename` result directly. |
| Declared multi-file outputs | `Engine\Source\Runtime\Engine\Classes\Exporters\Exporter.h` exposes `GetFileCount` and `GetUniqueFilename`; texture UDIM/layers and surround WAV exporters override them. |
| Replacement move behavior | `Engine\Source\Runtime\Core\Private\HAL\FileManagerGeneric.cpp`, `FFileManagerGeneric::Move`, deletes an existing destination before `MoveFile` when replacement is requested. |
| Move retry behavior | The same `FFileManagerGeneric::Move` implementation retries failed moves when `bDoNotRetryOrError=false`; the transaction passes `true` so a commit/restore failure returns immediately without sleeping or invoking error handling. |
| Bounded directory enumeration | `Engine\Source\Runtime\Core\Public\HAL\FileManager.h` exposes both immediate `IterateDirectory` and recursive `IterateDirectoryRecursively`. The recursive implementation in `Engine\Source\Runtime\Core\Private\GenericPlatform\GenericPlatformFile.cpp` explicitly accumulates directories to visit, so staging validation uses the immediate callback form and stops it at the entry budget. |
| Recursive cleanup behavior | `IFileManager::DeleteDirectory(..., Tree=true)` routes to recursive platform deletion, which visits discovered directories. Export cleanup therefore uses the same bounded immediate scan as validation, rejects every directory, deletes immediate files only, and removes the empty owned root non-recursively. |
| Cross-version surface | The relevant signatures and control flow are present in both `D:\Engine\UE_5.7` and `D:\Engine\UE_5.8`. |

---

## 2. Implemented Contract

| Concern | Verified behavior |
|---------|-------------------|
| Output planning | Native exporters may declare `1..256` distinct outputs. Every final output must stay in the exact requested directory and pass root, link, directory-shape, and collision checks before mutation. |
| Script exporters | Blueprint/script implementations of `ScriptRunAssetExportTask` fail closed because their arbitrary filesystem side effects cannot be bounded from the native filename contract. |
| Staging | The exporter receives a unique `.monolith-export-<guid>` directory beside the destination. Its resolved outputs must stay in that exact staging directory. |
| Postcondition | A reported exporter success is rejected if any declared output is missing or if an undeclared file or directory appears in staging. |
| Bounded staging inspection | At most 256 immediate staging entries are retained. The scan stops on entry 257 and never descends into a directory because a nested output is already invalid. |
| Commit | Existing destinations move into unique rollback backups inside staging. All staged outputs are then promoted with no implicit replacement, and every transaction move requests immediate no-retry failure. |
| Complete rollback | If any later promotion fails, every already-promoted output is removed and every original destination is restored. |
| Incomplete rollback | `status=partial_export`, `partial_mutation=true`, and exact `retained_paths` are returned for files that actually remain. An absent destination is never advertised as recovery evidence. Staging and backups are preserved instead of being deleted. |
| Success cleanup | A successful commit removes a completely scanned flat staging directory. Cleanup refuses an entry-budget overflow or any directory, does not descend, and reports `staging_cleanup_complete=false` plus the retained staging path. |

The response additionally exposes `output_file_count`, per-file `output_files`, mutation/exporter/commit flags, rollback and partial-mutation flags, promotion/restoration counts, staging cleanup status, and retained recovery paths. Successful dry runs and preflight errors use the same field set with explicit not-attempted defaults.

---

## 3. Current-Byte Identity

Git blob object IDs identify the canonical committed source independently of
checkout line endings. The SHA-256 column is the exact Windows worktree byte
sequence copied into both the UE 5.7 and UE 5.8 verification packages.

| File | Git blob OID | Windows checkout and both packages SHA-256 |
|------|--------------|------------------------------------------|
| `Source\MonolithInterchange\Private\MonolithInterchangeActions.cpp` | `2284edcb237547b8dc68bf42cd599efe800618ad` | `39229BF28C44A3FB5872DAC21CCEC8C85F2EE5109294C72DF433E8A74540A7F0` |
| `Source\MonolithInterchange\Private\MonolithInterchangeExportTransaction.cpp` | `bbab57c52107e11c5a98999882a30aa84e3bff86` | `CB9C7DBA6FB2A8749C44BE24EB1A876AFA980A99C1E181B1A0A9241CFA6BF504` |
| `Source\MonolithInterchange\Private\MonolithInterchangeExportTransaction.h` | `72c0c94d65b134794f415b63335d1ac1de0f2b61` | `3633F6F9C82D119942E381F844A2ACDC0721CC5FC3EB338DDE7CFC361BE90676` |
| `Source\MonolithInterchange\Private\Tests\MonolithInterchangeExportTransactionTests.cpp` | `0624bbffb9c07e5482ec4d12c6782261dcf01d33` | `2AF105F3D51DBC1FAE16465E60EBCDB268FDFCCA41E298AEF3647DEB3EF3387E` |
| `Source\MonolithInterchange\Private\Tests\MonolithInterchangeParamGuardTests.cpp` | `b348fd32318b300680ecdf24920a6ede24064c40` | `3A2DC430F248B57534B67C6DF1AAF4752025E6502DEAE5181850495C618324AF` |

---

## 4. Build Verification

`Speed.uproject` was read as `EngineAssociation="5.8"`. Compatibility-floor validation used a separate 5.7 foreign-plugin package. `MONOLITH_RELEASE_BUILD=1` kept optional external BlueprintAssist integration out of both isolated verification packages.

| Gate | Result | Package artifact | Build log |
|------|--------|------------------|-----------|
| UE 5.7 Editor plugin | Passed, 531/531 actions, `Result: Succeeded`, `BUILD SUCCESSFUL` | `D:\P4\MonolithInterchangeMergeReadyUE57aa1eea3Package\Binaries\Win64\UnrealEditor-MonolithInterchange.dll`; 433,664 bytes; SHA-256 `476D580B96C6396C91F66B3BED65C2A6B1A0C11C67518CE3ACD9001E34E9F171` | AutomationTool exit code 0 |
| UE 5.8 Editor plugin | Passed, 531/531 actions, `Result: Succeeded`, `BUILD SUCCESSFUL` | `D:\P4\MonolithInterchangeMergeReadyUE58aa1eea3Package\Binaries\Win64\UnrealEditor-MonolithInterchange.dll`; 409,600 bytes; SHA-256 `6BF15A19C7DE616EDD153B9AA777C42A63B1F5EBE5325168F88DD524AA1BECA6` | AutomationTool exit code 0 |

---

## 5. Focused Automation

| Engine | Test | Result | Report |
|--------|------|--------|--------|
| UE 5.7 | `Monolith.Interchange.ExportTransaction` | `Result={Success}`; warnings/errors 0 | `D:\P4\MonolithInterchangeMergeReadyUE57Host\Saved\Logs\InterchangeExport_MergeReady_UE57.log:1180`; report SHA-256 `AAFB0E3A1F03ED2A358BBC2A80F5B3CD2593434D4CC08D662218C8D70D1EFA97` |
| UE 5.7 | `Monolith.ParamGuard.MonolithInterchange.ImportRejectsMalformedParams` | Report `state=Success`; warnings/errors 0 | `D:\P4\MonolithInterchangeMergeReadyUE57Host\Saved\Automation\InterchangeParamGuard-MergeReady-UE57\index.json`; SHA-256 `E10558801FD3FD66099C5FC30BC1B735C5C43C104FD62D95802C733E09D24602` |
| UE 5.8 | `Monolith.Interchange.ExportTransaction` | `Result={Success}`; warnings/errors 0 | `D:\P4\MonolithInterchangeMergeReadyUE58Host\Saved\Logs\InterchangeExport_MergeReady_UE58.log:2062`; report SHA-256 `12E58396491820982C14167AD5C06793D15A9D61712D33175F080CB3FBF231D9` |
| UE 5.8 | `Monolith.ParamGuard.MonolithInterchange.ImportRejectsMalformedParams` | `Result={Success}`; warnings/errors 0 | `D:\P4\MonolithInterchangeMergeReadyUE58Host\Saved\Logs\InterchangeParamGuard_MergeReady_UE58.log:2127`; report SHA-256 `BC5EF9C5EA0E92C32EBCB5F086DD39CE296D06EA0F2D5E8718F65B9FCA3DDAB5` |

`ExportTransaction` covers successful replacement, a complete two-file rollback after the second promotion fails, a late no-replace collision, exact reporting of the existing backup (without an absent destination) when promotion and restoration are both injected to fail, non-recursive immediate staging inspection, an enforced entry budget, successful flat cleanup, and fail-closed preservation when an unexpected directory is present. The existing Interchange suite also performs a confirmed real `DefaultTexture` PNG export over a sentinel destination, verifies staged commit/cleanup/non-partial status, and verifies that a successful dry run exposes every transaction field without mutation.

Both automation hosts logged an unrelated failure to bind Monolith port `9316` because another process owned that endpoint during the run. The focused automation reports themselves contain zero warnings and zero errors, and no external process was stopped or reconfigured.

---

## 6. Static and Collision Checks

| Gate | Result |
|------|--------|
| Required repository command | `python Scripts/ci_static_checks.py --config .github/monolith-static-ci.json --github check` could not run because `contrib/master` contains neither referenced path. |
| Latest-checker differential | With `offline_exe_freshness` and `offline_parity_smoke` disabled symmetrically, baseline and branch both reported 36 blockers and 962 advisories; new findings: 0. |
| Open-PR source collision | GitHub open-PR file scans across `kunkunGames/monolith-fork`, `kunkunGames/monolith`, and `tumourlove/monolith` found no overlap under `Source/MonolithInterchange`. Generic `Docs/API_REFERENCE.md` is touched by unrelated open documentation PRs only. |

---

## 7. Visual and Discord Evidence

Screenshot verification was not applicable. This change affects headless editor-side filesystem export, rollback, and structured response behavior; it has no gameplay, UMG, VFX, animation, material, level, or editor-panel presentation change.

Therefore `Build\BatchFiles\Script\UploadScreenshotTestsToDiscord.bat` was not run.
