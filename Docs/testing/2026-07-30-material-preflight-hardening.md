# Monolith Material Preflight Hardening Verification

**Date:** 2026-07-30
**Scope:** Strict complex-parameter handling, material creation collision safety, material-function preflight, and atomic expression moves
**Result:** Submit-ready: current linked outputs, all 25 focused live tests, line endings, and final ownership audit pass

---

## 1. Contract

| Surface | Required behavior |
|---|---|
| Registry schema dispatch | A schema-declared JSON array or object accepts only a native JSON value of that kind. String-encoded JSON is rejected without rewriting caller input. |
| Material creation | A disk-backed Asset Registry entry fails as an existing asset. A loaded unsaved object or stale unindexed object fails as an explicit collision after package load. |
| PBR creation/replacement | Disk-backed registry collisions, stale disk packages, and loaded unsaved objects are distinguished before texture import. Replacement may delete only a disk-backed registry entry, must check the delete result, and must not delete or reuse an unindexed object. |
| Material-function creation | `type`, `description`, `expose_to_library`, and every `library_categories` entry are validated before package or object creation. |
| Material-function metadata | At least one writable field is required. Every supplied field and category entry is validated before the target asset is loaded or dirtied. |
| Batch material arrays | `batch_set_material_property` and `batch_recompile` accept only a native JSON string array for `asset_paths`; string-encoded JSON and mixed-type arrays are rejected before asset loading or transaction creation. |
| Expression move | Exactly one single or batch mode is accepted. Coordinates are finite `int32` values, aliases are unambiguous, duplicate/missing targets and relative overflow are rejected before mutation, and the accepted batch is one editor transaction. |

---

## 2. Verification Gates

| Gate | Required result | Current result |
|---|---|---|
| Protected editor build | Coordinator-owned protected `Build\BatchFiles\BuildGameEditorAndRun.bat` evidence; this review must not start another build | **PASS.** `UnrealEditor-MonolithCore.dll` is newer than both CL 1344 Core source files, and `UnrealEditor-MonolithMaterial.dll` is newer than all three CL 1344 Material source/test files. The subsequent protected build in `Saved\Logs\Codex\20260730_OpenCLReview\ProtectedBuild_CL1357_20260730_final2.stdout.log` accepted both modules as up to date and ended `Result: Succeeded` / `[BuildSpeedEditorAndRun] Build succeeded.`; the latest coordinator protected build in `C:\Users\12336\AppData\Local\UnrealBuildTool\Log.txt` also ended `Result: Succeeded` after 24 actions in 37.56 seconds. This review did not run another build or modify the CL 1357 surface. |
| Security automation | `Monolith.Security.Material` | **PASS.** Current linked-binary run `automation-20260730T151522Z-093F66AD`: `4/4` passed, zero errors and warnings. |
| Core schema automation | `Monolith.ParamValidation` | **PASS.** Current linked-binary run `automation-20260730T151526Z-9E31190D`: `12/12` passed and zero errors, including native-versus-string complex values. The new `StringEncodedComplexParamsRejected` test itself emitted zero warnings; the wider suite's expected fixture warnings are deliberate unknown-param and temporary action-registration diagnostics, not validation failures. |
| Material parameter guards | `Monolith.ParamGuard.Material` | **PASS.** Current linked-binary run `automation-20260730T151532Z-14620D77`: `9/9` passed, zero errors and warnings, including strict function metadata and atomic move rejection. |
| Perforce | Implementation, tests, specs, and this record are in CL `1344`; unrelated/default files remain outside it. | **PASS.** Exactly eight cohesive files are open in CL 1344; `p4 fstat -Ol` reports no `otherOpen`, `otherLock`, or `unresolved`; `p4 resolve -n -c 1344` reports nothing to resolve; `p4 revert -n -a -c 1344` reports no unchanged file. |
| Line endings | Every CL 1344 text-family file uses CRLF | **PASS.** `TestSourceLineEndings.ps1 -ProjectRoot D:\P4\speed -Changelist 1344` verified `8/8`; no bare LF remains. |
| Screenshot verification | N/A | No runtime visual, gameplay, UI, VFX, animation, or asset-presentation result changes. |
| Discord screenshot upload | N/A | `UploadScreenshotTestsToDiscord.bat` is not run because screenshot verification is not relevant. |

---

## 3. Acceptance

Every non-N/A gate above records passing evidence from the current source bytes
and linked editor binary. CL `1344` is submit-ready.
