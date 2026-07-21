# Monolith — MonolithScene Module

**Parent:** [../SPEC_CORE.md](../SPEC_CORE.md)
**Engine:** Unreal Engine 5.7+
**Owner module:** `MonolithScene`
**Namespace:** `scene`
**MCP tool:** `scene_query`
**Status:** Implemented (2026-05-20 namespace split)

---

## 1. Scope

``MonolithScene`` is the primary owner of the ``scene`` namespace — actor/scene manipulation, spatial queries, volumes, lighting, decals, lighting-capture helpers, storytelling patterns, and editor debug views. Split out of the former monolithic ``mesh`` namespace on 2026-05-20 along with ``worldgen``, ``leveldesign``, and ``modelgen``.

## 2. Namespace ownership

Primary action implementations live in `Source/MonolithScene` and export through `MONOLITHSCENE_API`. `MonolithScene::StartupModule` registers the scene-owned action families and `MonolithScene::ShutdownModule` unregisters only actions owned by `MonolithScene`, preserving other owner-scoped registrations in shared namespaces. `MonolithEditor` contributes editor/map metadata actions and `MonolithLevelDesign` contributes level-design placement/editing actions that operate on editor-world actors but belong under the `scene_query` runtime surface.

## 3. Registered actions

Always-on:
- `FMonolithMeshSceneActions` — spawn/duplicate/delete/select/group actors, set actor properties, scene bounds/stats, `batch_execute` runtime dispatcher.
- `FMonolithMeshSpatialActions` — raycasts, sweeps, overlaps, nearest, line-of-sight, navmesh, scene bounds.
- `FMonolithMeshVolumeActions` — spawn/scan volumes.
- `FMonolithMeshLightingActions` — place/inspect lights, sample light levels, suggest light placement.
- `FMonolithMeshDecalActions` — place decals.

Shared registrations:
- `MonolithEditor` — world context, streaming-level, layer, and level-metadata rows.
- `MonolithLevelDesign` — `place_light`, `set_light_properties`, actor material override helpers, component property reflection, sublevel management, Blueprint actor placement, spline placement, transform randomization, actor enumeration, and distance measurement.

Gated by `bEnableProceduralTownGen`:
- `FMonolithMeshSpatialRegistry` — hierarchical spatial descriptor / BFS pathfinding.
- `FMonolithMeshAutoVolumeActions` — auto NavMesh/blocking/audio/trigger volume generation.
- `FMonolithMeshDebugViewActions` — Daredevil debug view (section clip, floor-plan capture, camera bookmarks).

## 4. Build.cs dependencies

Public: `Core`, `CoreUObject`, `Engine`
Private: `MonolithCore`, `MonolithMesh` (for exported shared mesh-family helpers), `MonolithIndex`, `SQLiteCore`, `UnrealEd`, `EditorSubsystem`, `MeshDescription`, `StaticMeshDescription`, `MeshConversion`, `PhysicsCore`, `NavigationSystem`, `RenderCore`, `RHI`, `EditorScriptingUtilities`, `Json`, `JsonUtilities`, `Slate`, `SlateCore`, `AssetRegistry`, `AssetTools`, `MeshReductionInterface`, `MeshMergeUtilities`, `LevelInstanceEditor`, `ImageCore`.

## 5. Exact actor-deletion contract

`scene.delete_actors` is an exact, current-map operation rather than a best-effort selection command.

| Phase | Contract |
|---|---|
| Resolve | `actor_names` contains 1–1000 exact actor object paths or unique internal names/outliner labels. Each entry must resolve to a different live actor in the active editor world. Duplicate aliases that resolve to the same actor are rejected. |
| Scope | Every target must belong to `World->GetCurrentLevel()` and that level's current map package. Actors in another streamed/sublevel package are rejected before mutation. |
| Preflight | The complete target set passes `UUnrealEdEngine::CanDeleteActor` per actor and `UUnrealEdEngine::ShouldAbortActorDeletion` as one set before the first deletion. A preflight error returns `status=rejected_preflight`, `mutation_started=false`, and `deleted_count=0`. |
| Commit | The exact actor array is sent directly to `UUnrealEdEngine::DeleteActors`; selection state does not choose or widen the deletion target. The commit is enclosed in one action transaction, or the enclosing `scene.batch_execute` transaction. |
| Readback | Success requires every captured `FObjectKey` identity to be absent and every captured actor path to remain unoccupied in the live editor world. `DeleteActors`' Boolean return alone is never treated as proof because UE can skip an individual undeletable actor. |
| Failure | A survivor or engine failure returns structured `actor_results`/`survivors`, counts, `partial_failure`, `exact_deletion_verified=false`, `requires_manual_recovery`, and `rollback_performed=false`. `requires_manual_undo` is true only when a real transaction opened. Completed deletion remains applied and is retained for explicit Undo when `undo_available=true`; Monolith does not claim an automatic rollback. |

`scene.batch_execute` stops at the first failed action and always **ends** its outer transaction. It never calls `CancelTransaction` as a rollback substitute. Its error data preserves the failing action's nested `error_data`, reports `transaction_status=ended_after_failure_for_undo`, and tells the caller whether prior actions or the failing action may have retained mutations.

The shared scene-action transaction helper follows the same rule: a failure closes the transaction to preserve any already-applied mutation for Undo. Empty failed transactions are harmless; discarding a transaction after mutation is not.

## 6. Notes

``scene.batch_execute`` reads an optional per-item ``namespace`` JSON field (defaults to ``scene``) so an agent can dispatch across mesh-family namespaces in a single batch. Cross-namespace execution still goes through the registry; there is no implicit cross-namespace resolution and no registry-level legacy alias.

## 7. Per-action reference

Current static registration audit reports 76 `scene` actions: 58 from `MonolithScene`, 7 from `MonolithEditor`, and 11 from `MonolithLevelDesign`. For exact params and live ownership, call `monolith_discover({ "namespace": "scene", "mode": "actions" })`.
