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
| Cross-version surface | The relevant signatures and control flow are present in both `D:\Engine\UE_5.7` and `D:\Engine\UE_5.8`. |

---

## 2. Implemented Contract

| Concern | Verified behavior |
|---------|-------------------|
| Output planning | Native exporters may declare `1..256` distinct outputs. Every final output must stay in the exact requested directory and pass root, link, directory-shape, and collision checks before mutation. |
| Script exporters | Blueprint/script implementations of `ScriptRunAssetExportTask` fail closed because their arbitrary filesystem side effects cannot be bounded from the native filename contract. |
| Staging | The exporter receives a unique `.monolith-export-<guid>` directory beside the destination. Its resolved outputs must stay in that exact staging directory. |
| Postcondition | A reported exporter success is rejected if any declared output is missing or if an undeclared file or directory appears in staging. |
| Commit | Existing destinations move into unique rollback backups inside staging. All staged outputs are then promoted with no implicit replacement. |
| Complete rollback | If any later promotion fails, every already-promoted output is removed and every original destination is restored. |
| Incomplete rollback | `status=partial_export`, `partial_mutation=true`, and exact `retained_paths` are returned. Staging and backups are preserved instead of being deleted. |
| Success cleanup | A successful commit removes staging and backups. A cleanup failure keeps `status=exported` but reports `staging_cleanup_complete=false` and the retained staging path. |

The response additionally exposes `output_file_count`, per-file `output_files`, exporter/commit flags, rollback and partial-mutation flags, promotion/restoration counts, staging cleanup status, and retained recovery paths.

---

## 3. Current-Byte Identity

The source files copied by each foreign-plugin build matched the worktree byte-for-byte before the accepted build results were recorded.

| File | SHA-256 |
|------|---------|
| `Source\MonolithInterchange\Private\MonolithInterchangeActions.cpp` | `8E5608C239C83EFD33945BA76C04FFC022A6CD8F03D3D86129091682920DC51E` |
| `Source\MonolithInterchange\Private\MonolithInterchangeExportTransaction.cpp` | `E29265209979E50EC605E08E708237F2DCCCBD13E339B9E95A85407A7F62810A` |
| `Source\MonolithInterchange\Private\MonolithInterchangeExportTransaction.h` | `069AB22F12E18271957B0C135816F83F35C6E19566BDA2B67E6B0F2ED06DFEB4` |
| `Source\MonolithInterchange\Private\Tests\MonolithInterchangeExportTransactionTests.cpp` | `F483CB64D1FE85AEEAEFC505D126F8B61D0C275FCEC7DB1C46F19280921E8631` |
| `Source\MonolithInterchange\Private\Tests\MonolithInterchangeParamGuardTests.cpp` | `7AB523667A62A928D8383995B64E706699440CD17F8668996DA9E98322D965C8` |

---

## 4. Build Verification

`Speed.uproject` was read as `EngineAssociation="5.8"`. Compatibility-floor validation used a separate 5.7 foreign-plugin package. `MONOLITH_RELEASE_BUILD=1` kept optional external BlueprintAssist integration out of both isolated verification packages.

| Gate | Result | Package artifact | Build log |
|------|--------|------------------|-----------|
| UE 5.7 Editor plugin | Passed, 531/531 actions, `BUILD SUCCESSFUL` | `D:\P4\MonolithInterchangeExportUE57CurrentPackage\Binaries\Win64\UnrealEditor-MonolithInterchange.dll`; 414,208 bytes; SHA-256 `C8B5F7AC3F5D046BFDDEEE0C3C66C81AABEEE1822CCC767B9849DACD90AB74A2` | `C:\Users\12336\AppData\Roaming\Unreal Engine\AutomationTool\Logs\D+Engine+UE_5.7\Log.txt`; SHA-256 `E9CDCB5DD0B3AD08BFE789AF9E16B42F453AC81EBA063671FB77CAE4909093C3` |
| UE 5.8 Editor plugin | Passed, 531/531 actions, `BUILD SUCCESSFUL` | `D:\P4\MonolithInterchangeExportUE58CurrentPackage\Binaries\Win64\UnrealEditor-MonolithInterchange.dll`; 392,192 bytes; SHA-256 `EB8713E11609DF82C4DFA70E115A24DFDE1ED94DFE4B888498305BCF95CD37E7` | `C:\Users\12336\AppData\Roaming\Unreal Engine\AutomationTool\Logs\D+Engine+UE_5.8\Log.txt`; SHA-256 `0FBD320914C5E9158F5405D543F834F463F2DD70EAF3ACFE8CE93288FAF90682` |

---

## 5. Focused Automation

| Engine | Test | Result | Report |
|--------|------|--------|--------|
| UE 5.7 | `Monolith.Interchange.ExportTransaction` | 1 succeeded, 0 warnings, 0 errors | `D:\P4\MonolithInterchangeExportUE57CurrentAutomationHost\Saved\AutomationReports\ExportTransaction\index.json`; SHA-256 `C0A80E55E18D4ED8B1FC51ABBD447D861EFB1E256CF21CCAA3EC17E7EF1234EC` |
| UE 5.7 | `Monolith.ParamGuard.MonolithInterchange.ImportRejectsMalformedParams` | 1 succeeded, 0 warnings, 0 errors | `D:\P4\MonolithInterchangeExportUE57CurrentAutomationHost\Saved\AutomationReports\ParamGuard\index.json`; SHA-256 `B1578546F5CDE78CF4D9B595BBF8C1FE7852900453EFAB62691512E7E86262E2` |
| UE 5.8 | `Monolith.Interchange.ExportTransaction` | 1 succeeded, 0 warnings, 0 errors | `D:\P4\MonolithInterchangeExportUE58CurrentAutomationHost\Saved\AutomationReports\ExportTransaction\index.json`; SHA-256 `3EE31A506FE019DF2ED720132C50146ECD3D339F3263B23F6638886AA17368F8` |
| UE 5.8 | `Monolith.ParamGuard.MonolithInterchange.ImportRejectsMalformedParams` | 1 succeeded, 0 warnings, 0 errors | `D:\P4\MonolithInterchangeExportUE58CurrentAutomationHost\Saved\AutomationReports\ParamGuard\index.json`; SHA-256 `A0A1C845A6E2F2E5ECB8DB875C961FFA92675A474A292DE41DAFF6BED309B694` |

`ExportTransaction` covers successful replacement, a complete two-file rollback after the second promotion fails, a late no-replace collision, and preservation/reporting of the original backup when both promotion and restoration are injected to fail. The existing Interchange suite now also performs a confirmed real `DefaultTexture` PNG export over a sentinel destination and verifies staged commit, cleanup, and non-partial status.

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
