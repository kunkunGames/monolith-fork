# GameFeatures PR #6 Review Hardening Verification

**Date:** 2026-07-30
**Pull request:** `kunkunGames/monolith-fork#6`
**Branch:** `jules/codex/gamefeatures/modular-actions`
**Reviewed base head:** `a55fd4a751d4b6aca27f73c4922b303d8cf96bda`
**Scope:** `MonolithGameFeatures` writer atomicity, exact selectors, reflected type safety, asset discovery, and save diagnostics
**Engines:** Unreal Engine 5.7 and 5.8, resolved from each verification host's `EngineAssociation`

---

## 1. Goal

Turn the guarded GameFeatures action port into an idempotent, fail-closed
authoring surface suitable for merge. A rejected request must leave the target
asset unchanged, exact action selectors must remain exact across repeated
calls, reflected class/asset inputs must satisfy their declared types, and
ambiguous plugin data discovery must require an explicit asset path.

---

## 2. Review Findings and Resolution

| Finding | Resolution | Regression coverage |
|---|---|---|
| A newly attached action survived later validation failure | Prepare every edit on a transient duplicate/new action and commit only after all validation succeeds | Invalid component class keeps action count at `0` and package clean |
| `FScriptArrayHelper::AddValue` storage was initialized twice | Removed explicit `InitializeValue` calls after `AddValue` | UE 5.7/5.8 build and writer automation |
| `action_name` could select an object of a different action class | Require exact name and expected class together | Same-name/different-class request fails explicitly |
| Null cleanup dirtied a package even when no null entries existed | Track actual array mutations before calling `Modify` | Exact repeated request remains clean |
| Ability writer dry-run skipped DataTable/LyraAbilitySet type checks | Resolve and validate expected asset classes during preflight, including dry-run | Shared preflight/type-safety automation |
| Save failures exposed an absolute package filename | Report only the Unreal package name | Static response-path inspection |
| Reflected soft class properties ignored `MetaClass` | Enforce `FSoftClassProperty::MetaClass` assignability | Reject/accept cases cover incompatible and compatible classes |
| Plugin-name discovery chose arbitrary `UGameFeatureData` when several existed | Use descriptor match, otherwise require exactly one candidate or explicit `asset_path` | Unique and ambiguous candidate automation |

Exact named action creation also performs an Outer collision preflight and
uses the requested name directly. It does not use `MakeUniqueObjectName`,
which would produce `<name>_0` and break idempotent exact-name reuse.

---

## 3. Static and Documentation Gates

| Gate | Result | Evidence |
|---|---|---|
| Diff hygiene | PASS | `git diff --check` returned no whitespace errors |
| Array initialization audit | PASS | No explicit `InitializeValue` call remains under `Source\MonolithGameFeatures` |
| Focused tests | PASS | `WriterPreflightAndTypeSafety` covers rollback, idempotency, real update, selector class mismatch, soft-class `MetaClass`, and discovery ambiguity |
| API contract | PASS | `Docs\API_REFERENCE.md` describes atomic prepare/commit behavior and exact selectors |
| Module spec | PASS | `Docs\specs\SPEC_MonolithGameFeatures.md` records mutation, discovery, type, and diagnostic contracts |
| Operator skill | PASS | `Skills\unreal-gamefeatures\SKILL.md` documents exact-name and ambiguity requirements |

---

## 4. UE 5.7 Verification

The isolated host
`D:\P4\MonolithGameFeaturesUE57Host\MonolithGameFeaturesUE57Host.uproject`
enables `Monolith` and `GameFeatures` and resolves Unreal Engine 5.7 from its
project association.

| Gate | Result | Evidence |
|---|---|---|
| Fresh Editor build | PASS | `D:\P4\MonolithGameFeaturesUE57Host\Saved\Logs\GameFeatures-Final-UBT-UE57-20260730-051949.log`; `Result: Succeeded` |
| Focused automation | PASS | `2/2` succeeded, `0` warnings, `0` failures |
| Tests | PASS | `Monolith.GameFeatures.StatusAndReadOnlyGuards`; `Monolith.GameFeatures.WriterPreflightAndTypeSafety` |
| Report | PASS | `D:\P4\MonolithGameFeaturesUE57Host\Saved\Automation\GameFeatures-Final-UE57-20260730-052011\index.json` |

---

## 5. UE 5.8 Verification

The clean host
`D:\P4\MonolithGameFeaturesUE58ReviewHost\MonolithGameFeaturesUE58ReviewHost.uproject`
enables only `Monolith` and `GameFeatures`, resolves Unreal Engine 5.8 from its
project association, and uses a junction from `Plugins\Monolith` to the review
worktree.

| Gate | Result | Evidence |
|---|---|---|
| Fresh full Editor build | PASS | `439` actions; `Result: Succeeded`; `263.61` seconds |
| Build log | PASS | `D:\P4\MonolithGameFeaturesUE58ReviewHost\Saved\Logs\GameFeatures-Final-UBT-UE58-20260730-052155.log` |
| Focused automation | PASS | `2/2` succeeded, `0` warnings, `0` failures |
| Tests | PASS | `Monolith.GameFeatures.StatusAndReadOnlyGuards`; `Monolith.GameFeatures.WriterPreflightAndTypeSafety` |
| Report | PASS | `D:\P4\MonolithGameFeaturesUE58ReviewHost\Saved\Automation\GameFeatures-Final-UE58-20260730-052653\index.json` |
| Final module binary | PASS | `474624` bytes; SHA-256 `853E7DACA2A7D27508689BF72759E8ADCBD672022C2980F99BA7A4CFA7BCF285` |

The build emitted pre-existing non-Windows SDK availability messages and
deprecation warnings outside the changed GameFeatures code. The focused
automation report itself contains zero warnings and zero errors.

---

## 6. Live MCP Verification

The UE 5.8 host ran a live stateless MCP endpoint on review-only port `9437`.
The server reported Monolith `0.21.3`, Unreal Engine 5.8, `1342` actions, and
exactly `15` actions in the `gamefeatures` namespace.

| Scenario | Result |
|---|---|
| Invalid component class | Explicit type error; action count stayed `0` |
| Valid dry-run | Reported the proposed addition; actual action count stayed `0` |
| First apply | Created exact action `LiveComponents`; count became `1` |
| Identical repeat | `created=false`, `added=false`, `updated=false`, `changed=false`, `saved=false` |
| Real update | `addition_flags=1` produced `updated=true`, `changed=true`, `saved=true` |
| Readback | One `GameFeatureAction_AddComponents` action named exactly `LiveComponents`; Actor, ActorComponent, and `AdditionFlags=1` matched |
| Cleanup | Temporary `/Game/MonolithReview/DA_PR6_GameFeatureData` deleted; file absent and both find/describe returned not found |
| Shutdown | `QUIT_EDITOR` completed; validation PID exited and port `9437` closed |

Live log:
`D:\P4\MonolithGameFeaturesUE58ReviewHost\Saved\Logs\GameFeatures-Final-LiveMCP-UE58-20260730-052752.log`.
It contains no fatal error, assertion failure, ensure failure, or unhandled
exception. A single startup error notes that the intentionally minimal host
does not configure a production Asset Manager rule for `GameFeatureData`; it
does not affect direct asset authoring or the verified mutation contract.

---

## 7. Visual and Delivery Scope

| Gate | Result | Reason |
|---|---|---|
| PC 1920x1080 screenshot | N/A | This review changes headless editor action authoring, validation, and diagnostics only; it has no visual, gameplay, UI, VFX, animation, material, or asset-presentation behavior. |
| Discord screenshot upload | N/A | No screenshot artifact is relevant, so `Build\BatchFiles\Script\UploadScreenshotTestsToDiscord.bat` was not invoked. |

---

## 8. Result

All eight review findings are closed by production-path changes with focused
regression coverage. Both supported engine versions compile and pass the same
writer suite, and the UE 5.8 live endpoint proves rollback, dry-run, exact-name
idempotency, real update, readback, cleanup, and clean shutdown.
