# PCG PR #8 Review Hardening Verification

**Date:** 2026-07-30
**Pull request:** `kunkunGames/monolith-fork#8`
**Scope:** `MonolithPCG` exact-path graph/component actions, review feedback, strict scalar contracts, rollback behavior, docs, and focused automation
**Stacked base:** `7d6fb362fc9a7608b105058e7792b96528696d0f` (`jules/codex/source-control/provider-actions`)
**Engines:** Unreal Engine 5.7 and 5.8

---

## 1. Goal

Re-review the complete stacked PCG change against its updated source-control base, address the open high-priority review finding, remove nearby contract drift, and prove that the resulting 28-action surface is safe to request another merge review.

The acceptance boundary is stronger than compilation: save failures must not leave an unreported live component mutation, native JSON scalar types and documented numeric bounds must be enforced without coercion or clamping, rollback must preserve truthful dirty state, the documented action roster must match the live catalog, and the same source must pass focused automation on UE 5.7 and UE 5.8.

---

## 2. Findings and Fixes

| Finding | Risk before the fix | Resolution |
|---|---|---|
| Owning-level save failure after a component mutation | `create_component`, `set_component_graph`, `set_component_settings`, or `set_component_user_parameters` could return an error after leaving the editor-world object changed and dirty | Capture the exact prior state, roll back after save failure, re-resolve and verify the exact component state, restore the previous dirty bit only after complete rollback, and keep packages dirty when rollback is incomplete |
| Rollback result ambiguity | A caller could not distinguish a failed save with complete rollback from a surviving partial mutation | Structured failure data now reports `mutation_attempted`, `rollback_complete`, `changed`, `save_error`, and optional `rollback_error` |
| User-parameter rollback verification was too narrow | Verifying only touched values could miss an unrelated descriptor, value, or override-set change | A shared exact property-bag comparator verifies descriptor identity/order, type metadata, serialized values, and the complete override-ID set on UE 5.7 and UE 5.8 |
| Graph rollback dirty-state handling | A failed graph/settings/subgraph/parameter rollback could restore a clean bit and hide a surviving partial mutation | Complete rollback restores the captured dirty bit; incomplete rollback deliberately leaves the graph package dirty |
| Scalar and integer coercion | String booleans, wrong-type paths, fractional integers, near-integral doubles, huge doubles, or out-of-range values could be accepted or clamped | Required and optional strings/booleans now require native JSON types; bounded integers require a finite, exactly integral number inside the declared range before conversion |
| Stale status and skill guidance | `pcg.edit_graph_user_parameter_schema` was still advertised as future work even though `set_pcg_graph_user_parameters` implements graph schema/default authoring | `get_status`, API/spec text, and `Skills/unreal-pcg/SKILL.md` now distinguish graph schema/default ownership from component-instance overrides |

The save-failure regression uses a consumed-once, exact-actor test fault under `WITH_DEV_AUTOMATION_TESTS`; production behavior has no runtime fallback or injected failure branch.

---

## 3. Verification Environment

| Engine | Engine association | Host project | Plugin source |
|---|---|---|---|
| UE 5.7 | `5.7` | `D:\P4\MonolithPCGUE57Host\MonolithPCGUE57Host.uproject` | Junction to `D:\P4\MonolithForkPCG` |
| UE 5.8 | `5.8` | `D:\P4\MonolithPCGUE58Host\MonolithPCGUE58Host.uproject` | Junction to `D:\P4\MonolithForkPCG` |

Each host resolved its engine root from `EngineAssociation`. The two hosts share the plugin source through junctions, which also means the plugin-local `Binaries` and `Intermediate` directories are shared. UE 5.7 and UE 5.8 builds were therefore serialized, and each engine's generated outputs were isolated before switching engines. Concurrent cross-version compilation is invalid because one engine can consume the other engine's generated UHT state.

Final review builds used each host's `RunBuild.ps1`, which resolves the associated engine and invokes the matching editor target build. Full clean review builds succeeded on both engines before the final incremental source audit; the exact final source then rebuilt and linked successfully on both engines.

---

## 4. Build and Automation Results

| Gate | UE 5.7 | UE 5.8 |
|---|---|---|
| Full clean editor build | PASS, 455/455 actions | PASS, 455/455 actions |
| Final post-audit incremental build | PASS, compiled and linked `UnrealEditor-MonolithPCG.dll` | PASS, compiled and linked `UnrealEditor-MonolithPCG.dll` |
| `Automation RunTests Monolith.PCG` | PASS, 37/37 | PASS, 37/37 |
| `Automation RunTests Monolith.ParamGuard.MonolithPCG` | PASS, 3/3 | PASS, 3/3 |
| Failures / not-run / binding errors | 0 / 0 / 0 | 0 / 0 / 0 |
| PCG automation log | `D:\P4\MonolithPCGUE57Host\Saved\Logs\PCGAutomation-UE57-PostAudit.log` | `D:\P4\MonolithPCGUE58Host\Saved\Logs\PCGAutomation-UE58-PostAudit.log` |
| Parameter-guard log | `D:\P4\MonolithPCGUE57Host\Saved\Logs\PCGParamGuard-UE57-PostAudit.log` | `D:\P4\MonolithPCGUE58Host\Saved\Logs\PCGParamGuard-UE58-PostAudit.log` |

The new `Monolith.PCG.Component.SaveFailureAtomicity` test injects save failure independently after component creation, graph assignment, settings mutation, and user-parameter mutation. Each case proves exact state restoration, removal of a newly created component where applicable, structured rollback evidence, and restoration of the pre-action package dirty state. The native-scalar regression additionally rejects string booleans, wrong-type paths/remap values, fractional and near-integral doubles, very large doubles, and documented-range violations before asset loading or mutation.

---

## 5. Live MCP and Catalog Results

A final UE 5.8 editor-backed session used the isolated host on port `9448`.

| Gate | Result |
|---|---|
| MCP identity | PASS: project `MonolithPCGUE58Host`, engine 5.8, protocol `2025-03-26`, server `0.21.3` |
| Live discovery | PASS: exactly 28 `pcg` actions |
| Schema-first routing | PASS: focused schemas were read before create/read/list/validate/cleanup/dirty-package actions |
| Strict live rejection | PASS: `list_graph_assets(limit=501)` and `get_graph_asset(asset_path=true)` returned explicit invalid-parameter errors |
| Create/read/idempotence | PASS: `/Game/Tests/Monolith/PR8/PCG_PR8Live` was created and saved, then `existing_policy=return_existing` returned `existing` with `saved=false` |
| Structural read-back | PASS: clean graph, 0 element nodes, 2 special nodes, 0 edges, and validation with 0 errors/0 warnings |
| Registry metadata | PASS: exact list/get-tags read-back found the one test graph |
| Cleanup | PASS: dry-run then confirmed deletion; registry count returned to 0 and the package file was absent |
| Final status | PASS: `action_count=28`; the only future action is `pcg.execute_standalone_graph` |
| Dirty-package audit | PASS: 0 dirty packages under `/Game/Tests/Monolith` |

The regenerated external catalog at `D:\P4\MonolithPCGTargetCatalog-PostAudit.json` reports:

| Metric | Result |
|---|---:|
| Total actions | 1600 |
| Namespaces | 26 |
| `pcg` actions | 28 |
| Duplicate full action names | 0 |
| Partial catalog | `false` |
| Source hash | `1c716aad25c1fcf5d87ace446834e785e6cb78b6d170688411942174d394e3b2` |

No live test asset or dirty test package remained after verification.

---

## 6. Static Checker Scope

The static checker's own self-test passed. Running the fork's current broader configuration reported 9 blockers and 844 advisories: seven pre-existing Niagara registration markers, the fork's absent hosted-static-CI workflow, one proxy-smoke/configuration finding, and repository-wide CRLF/agent-directory advisories. Inspection found no blocker owned by `MonolithPCG`.

Those raw totals are not compared to the normalized 2026-07-29 base-parity result because the configurations differ. The original parity record remains authoritative for that comparison: `Docs/testing/2026-07-29-pcg-graph-component-actions.md` records identical base/target totals under one temporary normalized configuration. This review does not hide or relabel the fork-wide findings as PCG regressions.

---

## 7. Visual and Delivery Scope

| Gate | Result | Reason |
|---|---|---|
| 1920x1080 screenshot | N/A | The change affects editor C++ actions, schemas, automation, specs, and a skill; it does not change gameplay, UI, VFX, materials, assets, or other visual presentation. |
| Discord screenshot upload | N/A | No screenshot verification was relevant, so `Build\BatchFiles\Script\UploadScreenshotTestsToDiscord.bat` was not run. |

---

## 8. Conclusion

PASS. PR #8 now fails atomically when an owning-level save fails, exposes truthful rollback metadata and dirty state, enforces strict native scalar and exact bounded-integer contracts, removes stale action-status guidance, preserves the 28-action catalog without duplicates, and passes the final focused suites on both UE 5.7 and UE 5.8.
