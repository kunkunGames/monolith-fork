# Scene Exact Actor Deletion and Failure-Safe Transaction Verification

| Field | Value |
|---|---|
| Owner | MonolithScene |
| Status | Source, focused automation, spec, and skill implemented; build and automation execution pending parent CL verification |
| Date | 2026-07-19 |
| Changelist | 1207 |
| Target project | Speed / Unreal Engine 5.8 |

---

## 1. Scope

This change replaces `scene.delete_actors`' selection-driven, best-effort behavior with a fail-closed exact-deletion contract. It also removes `CancelTransaction` rollback claims from the shared scene-action and `scene.batch_execute` failure paths. No ProjectMGH map, gameplay asset, or actor is mutated by this source capability change.

---

## 2. UE 5.8 Contract Audit

| API | Verified behavior | Monolith response |
|---|---|---|
| `UUnrealEdEngine::CanDeleteActor` | Rejects non-transactional actors and actor-specific `CanDeleteSelectedActor` failures. | Run for every resolved target before the first mutation and surface the exact reason. |
| `UUnrealEdEngine::ShouldAbortActorDeletion` | Rejects a complete set when actor deletion should be aborted, including locked-level cases. | Run once on the complete exact target array before commit. |
| `UUnrealEdEngine::DeleteActors` | Iterates actors, can silently skip an actor that fails `CanDeleteActor`, and can still return `true`; its Boolean is not an exact postcondition. | Call directly with the preflight array, then independently scan the live world for each captured `FObjectKey` and actor path. |
| `UEditorEngine::CancelTransaction` | Cancels/discards the transaction record; it does not reverse mutations already made by the action. | End failed action/batch transactions so any completed mutation remains explicitly Undo-able; report `rollback_performed=false`. |

---

## 3. Implementation Gates

| Gate | Result | Evidence |
|---|---|---|
| Exact input identity | PASS (static) | Exact object paths are matched case-sensitively in the active editor world. Short inputs retain unique internal-name/outliner-label resolution. Duplicate inputs and distinct aliases that resolve to the same `FObjectKey` fail preflight. |
| Current-map confinement | PASS (static) | Every actor must belong to `World->GetCurrentLevel()` and its level package. Another streamed/sublevel actor is rejected before mutation. |
| Complete preflight | PASS (static) | JSON type, liveness, identity uniqueness, map package, `CanDeleteActor`, and complete-set `ShouldAbortActorDeletion` checks all precede the one engine commit. Preflight errors expose `mutation_started=false` and `deleted_count=0`. |
| Exact commit | PASS (static) | `UUnrealEdEngine::DeleteActors` receives the exact captured actor array and typed-element selection service directly; selected components or unrelated selected actors cannot widen the target. |
| Exact readback | PASS (static) | Success requires every captured `FObjectKey` to be absent and every captured object path to be unoccupied. Results include per-actor status, survivor rows, counts, engine return, and `exact_deletion_verified`. |
| Partial failure | PASS (static) | A survivor or engine failure returns an error with completed/surviving identities, `partial_failure`, `rollback_performed=false`, and `requires_manual_recovery`. `requires_manual_undo` is asserted only when the action observed a real transaction. |
| Batch transaction safety | PASS (static) | `batch_execute` stops at the first failure, preserves nested action `error_data`, and ends the outer transaction. It reports retained-action/mutation state and never presents `CancelTransaction` as rollback. |
| Shared scene transaction safety | PASS (static) | The local scene-action transaction helper ends on failure rather than discarding a potentially non-empty Undo record. |

---

## 4. Focused Automation

| Test | Coverage | Status |
|---|---|---|
| `Monolith.Scene.DeleteActors.PreflightAtomicity` | Duplicate exact identities and a later non-transactional actor reject the complete set; all actors survive and structured error data reports no mutation. | Added, not run |
| `Monolith.Scene.DeleteActors.ExactReadbackAndPartialFailure` | A dev-only exact-target fault seam proves one deleted/one surviving identity is surfaced as partial failure without rollback claims; the normal path proves two exact identities are absent with zero survivors. | Added, not run |
| `Monolith.Scene.BatchExecute.DeleteFailureUndoContract` | The same partial-deletion seam inside `batch_execute` proves the batch stops, retains the completed deletion, ends a real Undo transaction, and preserves the delete action's nested structured error data. | Added, not run |

The root task explicitly defers compilation and execution to the parent clean rebuild/test pass. This record therefore makes no build or runtime-pass claim.

---

## 5. Documentation and Publication

| Artifact | Result |
|---|---|
| `Docs\specs\SPEC_MonolithScene.md` | Defines resolve, scope, preflight, commit, readback, partial-failure, Undo, and batch semantics. |
| `Skills\unreal-scene\SKILL.md` | Prefers exact object paths for destructive deletion and requires survivor/readback inspection. |
| `Docs\API_REFERENCE.md` | Not edited here because it is owned by dependent publication CL 1200; that owner must synchronize the enriched response contract before publication. |

---

## 6. Visual and Discord Verification

| Gate | Result |
|---|---|
| PC 1920x1080 screenshot | N/A — source/API/test behavior only; no visual, gameplay, level, VFX, UI, or asset-presentation change was made. |
| Discord screenshot upload | N/A — no screenshot artifact is relevant, so `Build\BatchFiles\Script\UploadScreenshotTestsToDiscord.bat` was not invoked. |
