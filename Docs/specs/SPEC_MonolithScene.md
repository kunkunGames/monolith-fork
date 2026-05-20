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

## 5. Notes

``scene.batch_execute`` reads an optional per-item ``namespace`` JSON field (defaults to ``scene``) so an agent can dispatch across mesh-family namespaces in a single batch. Cross-namespace execution still goes through the registry; there is no implicit cross-namespace resolution and no registry-level legacy alias.

## 6. Per-action reference

Current static registration audit reports 76 `scene` actions: 58 from `MonolithScene`, 7 from `MonolithEditor`, and 11 from `MonolithLevelDesign`. For exact params and live ownership, call `monolith_discover({ "namespace": "scene", "mode": "actions" })`.
