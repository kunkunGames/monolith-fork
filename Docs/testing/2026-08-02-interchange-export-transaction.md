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
| Read-only rollback deletion | `FFileManagerGeneric::Delete` in both engines clears the read-only attribute through the low-level platform file when `EvenReadOnly=true`, then calls `DeleteFile`. Rollback uses that contract before restoring a backup. |
| Link detection | `FWindowsPlatformFile::IsSymlink` in both engines returns `ESymlinkResult::Symlink` for every `FILE_ATTRIBUTE_REPARSE_POINT`, covering symbolic links and junctions. The post-export guard checks every component through the expected staged-output leaf. |
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
| Postcondition | A reported exporter success is rejected if any declared output is missing, traverses a symlink/junction, or if an undeclared file or directory appears in staging. |
| Bounded staging inspection | At most 256 immediate staging entries are retained. The scan stops on entry 257 and never descends into a directory because a nested output is already invalid. |
| Commit | Existing destinations move into unique rollback backups inside staging. All staged outputs are then promoted with no implicit replacement, and every transaction move requests immediate no-retry failure. |
| Complete rollback | If any later promotion fails, every already-promoted output is force-removed even when read-only and every original destination is restored. |
| Incomplete rollback | `status=partial_export`, `partial_mutation=true`, and exact `retained_paths` are returned for files that actually remain. An absent destination is never advertised as recovery evidence. Staging and backups are preserved instead of being deleted. |
| Success cleanup | A successful commit removes a completely scanned flat staging directory. Cleanup refuses an entry-budget overflow or any directory, does not descend, and reports `staging_cleanup_complete=false`; `retained_paths` includes the staging directory only while it still exists. |

The response additionally exposes `output_file_count`, per-file `output_files`, mutation/exporter/commit flags, rollback and partial-mutation flags, promotion/restoration counts, staging cleanup status, and retained recovery paths. Successful dry runs and preflight errors use the same field set with explicit not-attempted defaults.

---

## 3. Current-Byte Identity

Git blob object IDs identify the canonical committed source independently of
checkout line endings. The SHA-256 column is the exact Windows worktree byte
sequence copied into both the UE 5.7 and UE 5.8 verification packages.

| File | Git blob OID | Windows checkout and both packages SHA-256 |
|------|--------------|------------------------------------------|
| `Source\MonolithInterchange\Private\MonolithInterchangeActions.cpp` | `543d6170244498d6068755889e68d89ce8772152` | `B7A6CA87A869E22E8204C786C55470E5FB7058A1DBF4C52CE26D24AF419329C0` |
| `Source\MonolithInterchange\Private\MonolithInterchangeExportTransaction.cpp` | `dbd14443ede4b239a5e679062bdf6a64fcbc5e1d` | `0771622D95902329C707B7C9783BB185EAE0810D7ADDC869B26FA96EB1AC2130` |
| `Source\MonolithInterchange\Private\MonolithInterchangeExportTransaction.h` | `f44b28770a5814b2b94ebc61ff606a4d8a33493e` | `3BA6A55B0C5F7C0F780C466153F64E1FA1C375B1229F28CDB68BE47DB34F2DD6` |
| `Source\MonolithInterchange\Private\Tests\MonolithInterchangeExportTransactionTests.cpp` | `17660dc20326aa7bf3982def45edfdaac2ec92cb` | `C9531465B45B331AAFD6F0B4EA6D58B2778427E950F24834344EB8F9D0C9600B` |
| `Source\MonolithInterchange\Private\Tests\MonolithInterchangeParamGuardTests.cpp` | `b348fd32318b300680ecdf24920a6ede24064c40` | `3A2DC430F248B57534B67C6DF1AAF4752025E6502DEAE5181850495C618324AF` |

---

## 4. Build Verification

`Speed.uproject` was read as `EngineAssociation="5.8"`. Compatibility-floor validation used a separate 5.7 foreign-plugin package. `MONOLITH_RELEASE_BUILD=1` kept optional external BlueprintAssist integration out of both isolated verification packages.

| Gate | Result | Package artifact | Build log |
|------|--------|------------------|-----------|
| UE 5.7 Editor plugin | Passed, 531/531 actions, `Result: Succeeded`, `BUILD SUCCESSFUL` | `D:\P4\MonolithInterchangeMergeReadyReview2UE57Package\Binaries\Win64\UnrealEditor-MonolithInterchange.dll`; 438,784 bytes; SHA-256 `0D8B33EC2FC582B3A5B3AE39913E122DB82AC980569ECCE08AC0B1587E210F51` | AutomationTool exit code 0 |
| UE 5.8 Editor plugin | Passed, 531/531 actions, `Result: Succeeded`, `BUILD SUCCESSFUL` | `D:\P4\MonolithInterchangeMergeReadyReview2UE58Package\Binaries\Win64\UnrealEditor-MonolithInterchange.dll`; 415,232 bytes; SHA-256 `F717760123C84508CD811C63AE3859A56E8394095553956EB9DCF56E023532CD` | AutomationTool exit code 0 |

---

## 5. Focused Automation

| Engine | Test | Result | Report |
|--------|------|--------|--------|
| UE 5.7 | `Monolith.Interchange.ExportTransaction` | `Result={Success}` | `D:\P4\MonolithInterchangeMergeReadyReview2UE57Host\Saved\Logs\InterchangeExport_Review2_UE57.log:1193`; report SHA-256 `56399824BDC9483161A76BA85834C912992EF885EE1175C3C3A980C347EB61AE` |
| UE 5.7 | `Monolith.ParamGuard.MonolithInterchange.ImportRejectsMalformedParams` | `Result={Success}` | `D:\P4\MonolithInterchangeMergeReadyReview2UE57Host\Saved\Logs\InterchangeParamGuard_Review2_UE57.log:1190`; report SHA-256 `0DBB050EA809A7A2A852014FE2F86CEE26CFBEA827B69B4136AF3761AA75D4A4` |
| UE 5.8 | `Monolith.Interchange.ExportTransaction` | `Result={Success}` | `D:\P4\MonolithInterchangeMergeReadyReview2UE58Host\Saved\Logs\InterchangeExport_Review2_UE58.log:2061`; report SHA-256 `E5FE7A68E51F2EA04458313DA1B4AD7B483EE58C14A34D56956C87468E653F20` |
| UE 5.8 | `Monolith.ParamGuard.MonolithInterchange.ImportRejectsMalformedParams` | `Result={Success}` | `D:\P4\MonolithInterchangeMergeReadyReview2UE58Host\Saved\Logs\InterchangeParamGuard_Review2_UE58.log:2102`; report SHA-256 `3861A5A2B8DF32C9E974416A92D50B14A131ACB4AC2D6D04177CC7143E03F9DC` |

`ExportTransaction` covers successful replacement, a complete two-file rollback after the second promotion fails, forced deletion of a promoted read-only staged file before both originals are restored, deterministic rejection of a symlink at the exact expected staged-output leaf, a late no-replace collision, exact reporting of the existing backup (without an absent destination) when promotion and restoration are both injected to fail, non-recursive immediate staging inspection, an enforced entry budget, successful flat cleanup, and fail-closed preservation when an unexpected directory is present. The existing Interchange suite also performs a confirmed real `DefaultTexture` PNG export over a sentinel destination, verifies staged commit/cleanup/non-partial status, and verifies that a successful dry run exposes every transaction field without mutation.

Both automation hosts logged an unrelated failure to bind Monolith port `9316` because another process owned that endpoint during the run; UE startup also emitted ordinary host/editor warnings. Every focused test nevertheless completed with `Result={Success}`, and no external process was stopped or reconfigured.

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
