# Monolith — MonolithScene Module

**Parent:** [../SPEC_CORE.md](../SPEC_CORE.md)
**Engine:** Unreal Engine 5.7+
**Owner module:** `MonolithScene`
**Namespace:** `scene`
**MCP tool:** `scene_query`
**Status:** Implemented (2026-05-20 namespace split)

---

## 1. Scope

``MonolithScene`` owns the ``scene`` namespace — actor/scene manipulation, spatial queries, volumes, lighting, decals, and editor debug views. Split out of the former monolithic ``mesh`` namespace on 2026-05-20 along with ``worldgen``, ``leveldesign``, and ``modelgen``.

## 2. Namespace ownership

The module is a thin registration shim: action class implementations live in MonolithMesh DLL (decorated with ``MONOLITHMESH_API``), and ``MonolithScene::StartupModule`` calls their ``RegisterActions`` cross-DLL. ``MonolithScene::ShutdownModule`` unregisters the ``scene`` namespace. The shim pattern keeps the .cpp files where their MonolithMesh helper dependencies live while giving the namespace its own MCP-discoverable module.

## 3. Registered actions

Always-on:
- `FMonolithMeshSceneActions` — spawn/duplicate/delete/select/group actors, set actor properties, scene bounds/stats, `batch_execute` runtime dispatcher.
- `FMonolithMeshSpatialActions` — raycasts, sweeps, overlaps, nearest, line-of-sight, navmesh, scene bounds.
- `FMonolithMeshVolumeActions` — spawn/scan volumes.
- `FMonolithMeshLightingActions` — place/inspect lights, sample light levels, suggest light placement.
- `FMonolithMeshDecalActions` — place decals.

Gated by `bEnableProceduralTownGen`:
- `FMonolithMeshSpatialRegistry` — hierarchical spatial descriptor / BFS pathfinding.
- `FMonolithMeshAutoVolumeActions` — auto NavMesh/blocking/audio/trigger volume generation.
- `FMonolithMeshDebugViewActions` — Daredevil debug view (section clip, floor-plan capture, camera bookmarks).

## 4. Build.cs dependencies

Public: `Core`, `CoreUObject`, `Engine`
Private: `MonolithCore`, `MonolithMesh` (for shared mesh-family helpers and `MONOLITHMESH_API`-decorated action classes), `MonolithIndex`, `SQLiteCore`, `UnrealEd`, `EditorSubsystem`, `MeshDescription`, `StaticMeshDescription`, `MeshConversion`, `PhysicsCore`, `NavigationSystem`, `RenderCore`, `RHI`, `EditorScriptingUtilities`, `Json`, `JsonUtilities`, `Slate`, `SlateCore`, `AssetRegistry`, `AssetTools`, `MeshReductionInterface`, `MeshMergeUtilities`, `LevelInstanceEditor`, `ImageCore`.

## 5. Notes

``scene.batch_execute`` reads an optional per-item ``namespace`` JSON field (defaults to ``scene``) so an agent can dispatch across mesh-family namespaces in a single batch. ``FMonolithMeshCityBlockActions::TryExecuteAction`` takes a positional ``Namespace`` argument from internal callers. No implicit cross-namespace resolution and no registry-level legacy alias.

## 6. Per-action reference

See the namespace section in [SPEC_MonolithMesh.md](SPEC_MonolithMesh.md) for the per-action params/descriptions (the split table at the top of that spec is authoritative for namespace attribution; row params/descriptions remain in-place pending the per-action re-tabulation backlog item in [../TODO.md](../TODO.md)).