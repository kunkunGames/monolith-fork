# Scene Move Actor Transform Persistence Verification

| Field | Value |
| --- | --- |
| Owner | MonolithScene |
| Status | Passed: the owning-package persistence root fix, focused automation, final protected build, and live SpeedBox dirty/save/reload application are verified; the target diff check is clean and the global static-check exit `1` is explicitly retained for two pre-existing out-of-scope drift findings |
| Date | 2026-07-22 |
| Changelist | `1284` |
| Target project | Speed / Unreal Engine 5.8 |

---

## 1. Scope And Reproduction

The SpeedBox Project_MGH objective restoration applied `36/36` live transforms
through `scene.batch_execute`, but `editor.list_dirty_packages` returned `0` and
`editor.save_packages(dry_run=true)` reported `would_save=false`. The visible
editor-world result was therefore not persistable and would have been lost on
reload. This was a defect in the owning `scene.move_actor` write contract, not a
reason to bypass Monolith or add a map-specific save workaround.

| Reproduction step | Result before fix |
| --- | --- |
| `scene.batch_execute` with `36` `move_actor` actions | `36/36` success and correct live transforms |
| `editor.list_dirty_packages` | `count=0` |
| Exact map save dry-run | `would_save=false` |
| Persistence conclusion | FAIL: successful live mutation had no savable owning package |

---

## 2. Root Cause And Final Persistence Contract

Transform setters alone did not establish the complete editor-authoring contract.
The final handler requires a persistent actor/root/package, matches location and
rotation setter tolerance while retaining exact scale semantics, proves package
dirtiness before mutation, and notifies editor listeners after a completed real
change.

| Concern | Final contract |
| --- | --- |
| Persistent target | Actor, root component, and owning package must exist and be non-transient. A transient target is rejected, remains unmoved, and leaves the package clean. |
| Location/rotation tolerance | Existing absolute/relative request semantics remain unchanged. Requested world targets are converted to effective relative values under attachments before comparison. An unattached exact or engine-suppressed sub-tolerance location/rotation request is a clean no-op, angle-equivalent rotations are unchanged, and a sub-tolerance world delta that becomes meaningful under a scaled parent remains a real change. |
| Exact scale semantics | Scale uses component-wise exact inequality rather than location tolerance. Any requested `TargetScale != PreviousScale`, including a tiny nonzero delta, is applied and treated as a dirty real change. |
| Dirty preflight | Before a real change, `Actor->MarkPackageDirty()` must return true and the owning package must report `IsDirty()`. Failure returns an explicit error while the actor is still untouched. |
| Transaction participation | After dirty preflight, the actor and root component call `Modify(false)` inside the direct action transaction or the enclosing batch transaction. |
| Mutation and notification | Requested engine setters run, omitted fields remain unchanged, and a real change calls `PostEditMove(true)` once after all setters. |
| Package persistence | The preflight covers both conventional map and external-actor owning packages; mutation cannot succeed with an unsavable clean package. |
| Save ownership | `move_actor` never auto-saves. The caller must save the exact dirty package and reload/read back it. |
| Batch ownership | `scene.batch_execute` retains one outer Undo transaction while every changed actor still dirties its own package. |

The effective-change precheck is intentionally split by setter semantics and
attachment space: the action neither dirties a package for a location/rotation
change the component setter would suppress nor drops a world delta made
meaningful by parent scale or a small scale edit that the exact scale setter
would apply. It also never mutates first and discovers afterward that the
package cannot be saved.

---

## 3. Implementation Artifacts

| Artifact | Change |
| --- | --- |
| `Source\MonolithScene\Private\MonolithMeshSceneActions.cpp` | Requires a persistent actor/root/package, compares effective attachment-relative location/rotation with setter tolerance, retains exact effective scale-change semantics, preflights `Actor->MarkPackageDirty()` plus `IsDirty()`, fails closed before mutation when persistence is unavailable, and applies `Modify(false)`, requested setters, and `PostEditMove(true)` only on a real change. |
| `Source\MonolithScene\Private\Tests\MonolithSceneMoveActorTests.cpp` | Covers transient rejection, direct/batch no-op cleanliness, engine-suppressed and above-tolerance location changes, a tiny exact scale change, exact scaled-parent world and relative target coordinates, and direct/batch real-move persistence. |
| `Docs\specs\SPEC_MonolithScene.md` | Defines the exact actor-transform persistence contract and test name. |

No header, reflection, registration, schema, or action-name change is introduced.

---

## 4. Focused Automation

| Test | Coverage | Result |
| --- | --- | --- |
| Transient rejection | A transient actor fails closed, remains unmoved, and leaves its owning level package clean. | PASS |
| Direct and batch no-op | Exact absolute transform and zero relative batch requests succeed while the package stays clean. | PASS |
| Location tolerance boundary | A `UE_KINDA_SMALL_NUMBER * 0.5` location request is engine-suppressed and clean; a `* 2.0` request moves and dirties. | PASS |
| Exact scale boundary | A tiny nonzero scale delta is applied exactly and dirties rather than being discarded by location tolerance. | PASS |
| Attached effective space | A `0.5 * UE_KINDA_SMALL_NUMBER` world target under a `UE_KINDA_SMALL_NUMBER`-scaled parent applies exactly in world space, resolves to relative location `0.5`, and dirties. | PASS |
| Direct real move | Absolute location/rotation/scale apply and dirty the owning level package. | PASS |
| Batched real move | Relative location through `scene.batch_execute` applies and dirties the owning level package. | PASS |

The final focused run `automation-20260721T235428Z-BF97AE20` completed `1/1`
tests with zero failures.

---

## 5. Build And Live Production Proof

| Gate | Result | Evidence |
| --- | --- | --- |
| Initial protected build | PASS | `Saved\Logs\Verification\Build-SpeedEditor-Protected-CL1284-MoveActorPersistence-20260722.log` built the handler and new test through `Build\BatchFiles\BuildGameEditorAndRun.bat`. |
| Setter-semantics patches | PASS Live Coding | The persistent-target/dirty-preflight change and the final split between location/rotation tolerance and exact scale-change semantics compiled and applied with zero errors before the final protected build. |
| Production batch | PASS | SpeedBox applied `36/36` donor-normalized Battery/Beacon transforms. |
| Dirty readback | PASS | Exactly one package: `/SpeedBox/Maps/L_Playground_Box`. |
| Save dry-run | PASS | `would_save=1`, `failed_validation=0`. |
| Exact save | PASS | `saved=1`, `failed_validation=0`. |
| Post-save state | PASS | Dirty package count `0`. |
| Reload readback | PASS | All `36/36` transforms persisted. A later `8/8` Beacon pair-compatibility adjustment repeated the same exact dirty/save/reload chain. |
| Live exact no-op | PASS | Reapplying the exact current transform to `ProjectMGH_BeaconObjective_01` left the actor unchanged and the scoped dirty-package count at `0`. |
| Final protected build | PASS, exit `0` | `Saved\Logs\Verification\Build-SpeedEditor-Protected-CL1284-ObjectivePlacement-MoveAttachedExactProof-Final-20260722.log` records the protected `Build\BatchFiles\BuildGameEditorAndRun.bat` run against the final move-only persistence scope, fail-closed attached-actor setter semantics, and exact attached-target proof under CL `1284`. |

---

## 6. Static Checks And Repository Hygiene

| Gate | Result |
| --- | --- |
| GitHub branch/PR collision guard | PASS: fetched remotes and inspected open PRs `#1855` through `#1863`; no target handler/test/spec path overlap. Other MonolithScene branches touched only `MonolithMeshSpatialActions.cpp` or `MonolithSceneParamGuardTests.cpp`. |
| Existing Git worktree changes | Preserved: unrelated MonolithEditor, `CHANGELOG.md`, `Docs\SPEC_CORE.md`, and `SPEC_MonolithEditor.md` changes are not modified or reverted. |
| `python Scripts/ci_static_checks.py --config .github/monolith-static-ci.json --github check` | The final global command executed to completion and returned exit `1` with two pre-existing out-of-scope blockers: accepted OfflineParity database modification-time drift for `Saved/EngineSource.db`, and generated catalog snapshot drift caused by unrelated MonolithEditor registrations. Its live skill-catalog drift guard also timed out after `180s` as an advisory. This record does not claim a global static-check pass. |
| Target `git diff --check` | PASS: the handler, regression test, MonolithScene spec, and this focused record are whitespace-clean. |
| Perforce | Handler, test, spec, and this record belong only to Speed task CL `1284`; unrelated default-CL files remain untouched. |

---

## 7. Visual And Discord Verification

| Gate | Result |
| --- | --- |
| PC `1920x1080` screenshot | N/A for this editor API change. The resulting SpeedBox map presentation is verified separately in `Docs\testing\2026-07-22-speedbox-project-mgh-objective-placement.md`. |
| Discord screenshot upload | N/A for this Monolith API record. The linked Speed verification record explicitly uploads the exact placement PNG, avoiding a duplicate publication. |

---

## 8. Result

`scene.move_actor` now has a fail-visible, transaction-aware, persistable editor
write contract. Persistent targets and savable owning packages are required;
exact or engine-suppressed location/rotation requests remain clean, while any
exact scale change is applied and dirtied. Effective changes cannot begin before
dirty preflight succeeds. Production no-op plus save/reload proof exercises the
same handler rather than a test-only seam.
