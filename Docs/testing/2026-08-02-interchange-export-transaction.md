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
| Bounded directory enumeration | `Engine\Source\Runtime\Core\Public\HAL\FileManager.h` exposes both immediate `IterateDirectory` and recursive `IterateDirectoryRecursively`. The recursive implementation in `Engine\Source\Runtime\Core\Private\GenericPlatform\GenericPlatformFile.cpp` explicitly accumulates directories to visit, so staging validation uses the immediate callback form and stops it at the entry budget. |
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
| Commit | Existing destinations move into unique rollback backups inside staging. All staged outputs are then promoted with no implicit replacement. |
| Complete rollback | If any later promotion fails, every already-promoted output is removed and every original destination is restored. |
| Incomplete rollback | `status=partial_export`, `partial_mutation=true`, and exact `retained_paths` are returned. Staging and backups are preserved instead of being deleted. |
| Success cleanup | A successful commit removes staging and backups. A cleanup failure keeps `status=exported` but reports `staging_cleanup_complete=false` and the retained staging path. |

The response additionally exposes `output_file_count`, per-file `output_files`, mutation/exporter/commit flags, rollback and partial-mutation flags, promotion/restoration counts, staging cleanup status, and retained recovery paths. Successful dry runs and preflight errors use the same field set with explicit not-attempted defaults.

---

## 3. Current-Byte Identity

The source files copied by each foreign-plugin build matched the worktree byte-for-byte before the accepted build results were recorded.

| File | SHA-256 |
|------|---------|
| `Source\MonolithInterchange\Private\MonolithInterchangeActions.cpp` | `FEB52E86AC2DF05FCC264F951BAD2D412C0AFCB4100917FF8D43325D1F8F70EB` |
| `Source\MonolithInterchange\Private\MonolithInterchangeExportTransaction.cpp` | `B528A26F4343829E6E4FF043E31E8E8DB8B296C9FBB0B77936269CE08A95AC82` |
| `Source\MonolithInterchange\Private\MonolithInterchangeExportTransaction.h` | `B9E90D68091453736BCF21D614C2A58FD19AF727980DF3F45F23950037641545` |
| `Source\MonolithInterchange\Private\Tests\MonolithInterchangeExportTransactionTests.cpp` | `A74CE0D45434D45C1452C79C51D4198F48D2B5AAB291A20316FB4672CDE7847B` |
| `Source\MonolithInterchange\Private\Tests\MonolithInterchangeParamGuardTests.cpp` | `3A2DC430F248B57534B67C6DF1AAF4752025E6502DEAE5181850495C618324AF` |

---

## 4. Build Verification

`Speed.uproject` was read as `EngineAssociation="5.8"`. Compatibility-floor validation used a separate 5.7 foreign-plugin package. `MONOLITH_RELEASE_BUILD=1` kept optional external BlueprintAssist integration out of both isolated verification packages.

| Gate | Result | Package artifact | Build log |
|------|--------|------------------|-----------|
| UE 5.7 Editor plugin | Passed, 531/531 actions, `BUILD SUCCESSFUL` | `D:\P4\MonolithInterchangeReviewUE57Package\Binaries\Win64\UnrealEditor-MonolithInterchange.dll`; 427,008 bytes; SHA-256 `13D5C67A8A9FF075CFEB4979B4658D9FB55280B72C84A23AD5EE563352946190` | `C:\Users\12336\AppData\Roaming\Unreal Engine\AutomationTool\Logs\D+Engine+UE_5.7\Log.txt`; SHA-256 `8FF2D949D0760E17F3DF39B77F2AAC0C2B385399FFAE87F81D205CDCCC8447D4` |
| UE 5.8 Editor plugin | Passed, 531/531 actions, `BUILD SUCCESSFUL` | `D:\P4\MonolithInterchangeReviewUE58Package\Binaries\Win64\UnrealEditor-MonolithInterchange.dll`; 402,944 bytes; SHA-256 `CB72F98A297F1A4D775903042F2B4686A603CAC8E799E466E374241B03AC0B94` | `C:\Users\12336\AppData\Roaming\Unreal Engine\AutomationTool\Logs\D+Engine+UE_5.8\Log.txt`; SHA-256 `D7EDB4B27A1A3B9335AD2F26EF1DD5BBE232F6EBB6E089CB13EF58CF0257D7E6` |

---

## 5. Focused Automation

| Engine | Test | Result | Report |
|--------|------|--------|--------|
| UE 5.7 | `Monolith.Interchange.ExportTransaction` | `Result={Success}` | `D:\P4\MonolithInterchangeReviewUE57Host\Saved\Logs\Automation_20260802_ExportTransaction_UE57_Review.log:1172`; SHA-256 `49272686754B47A42EC322F5CC7DA68D5CB042F90EB0B27EFAC5A0DBB39300B9` |
| UE 5.7 | `Monolith.ParamGuard.MonolithInterchange.ImportRejectsMalformedParams` | `Result={Success}` | `D:\P4\MonolithInterchangeReviewUE57Host\Saved\Logs\Automation_20260802_ParamGuard_UE57_Review.log:1184`; SHA-256 `02B01ED3AA01C6E2D1802C24741ED48BFD551D58D37571DBCB3732EA6FFF72B2` |
| UE 5.8 | `Monolith.Interchange.ExportTransaction` | `Result={Success}` | `D:\P4\MonolithInterchangeReviewUE58Host\Saved\Logs\Automation_20260802_ExportTransaction_UE58_Review.log:2054`; SHA-256 `90B36C35970A612E2993FC388445CC50B6CC767F17A6620F3EAFDB68ACFDB58D` |
| UE 5.8 | `Monolith.ParamGuard.MonolithInterchange.ImportRejectsMalformedParams` | `Result={Success}` | `D:\P4\MonolithInterchangeReviewUE58Host\Saved\Logs\Automation_20260802_ParamGuard_UE58_Review_Retry.log:2044`; SHA-256 `C65828ABE3BE24F9A683FC049242B98B19D7B4A307E61D5DCD2ABCC37620C1FE` |

`ExportTransaction` covers successful replacement, a complete two-file rollback after the second promotion fails, a late no-replace collision, preservation/reporting of the original backup when both promotion and restoration are injected to fail, non-recursive immediate staging inspection, and an enforced entry budget. The existing Interchange suite also performs a confirmed real `DefaultTexture` PNG export over a sentinel destination, verifies staged commit/cleanup/non-partial status, and verifies that a successful dry run exposes every transaction field without mutation.

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
