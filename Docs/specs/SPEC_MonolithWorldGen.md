# Monolith — MonolithWorldGen Module

**Parent:** [../SPEC_CORE.md](../SPEC_CORE.md)
**Engine:** Unreal Engine 5.7+
**Owner module:** `MonolithWorldGen`
**Namespace:** `worldgen`
**MCP tool:** `worldgen_query`
**Status:** Implemented (2026-05-20 namespace split)

---

## 1. Scope

``MonolithWorldGen`` owns the ``worldgen`` namespace — procedural building/city/facade/roof/terrain generation, floor-plan generation, furnishing, blockout authoring, room/genre templates, asset presets, and context-prop placement. Split out of ``mesh`` on 2026-05-20.

## 2. Namespace ownership

``worldgen`` is **co-registered** by two modules: ``MonolithWorldGen`` (always-on Blockout/Template/Preset/ContextProp + gated FloorPlan/Furnishing/BuildingValidation) and ``MonolithMesh`` (gated Building/Facade/Roof/CityBlock/Terrain/ArchFeature — these stay in MonolithMesh because they require the ``WITH_GEOMETRYSCRIPT`` HandlePool owned by MonolithMesh). ``MonolithWorldGen::ShutdownModule`` is the sole unregisterer of the ``worldgen`` namespace; MonolithMesh does not touch it on shutdown. Same dual-module pattern as ``ui`` (MonolithUI + 4 MonolithGAS UI-binding aliases).

## 3. Registered actions

Owned by `MonolithWorldGen` (always-on):
- `FMonolithMeshBlockoutActions` — blockout grids, primitives, asset matching, prop scatter.
- `FMonolithMeshTemplateActions` — room template CRUD.
- `FMonolithMeshPresetActions` — genre presets, building archetypes.
- `FMonolithMeshContextPropActions` — prop placement/scatter.

Owned by `MonolithWorldGen` (gated by `bEnableProceduralTownGen`):
- `FMonolithMeshFloorPlanGenerator` — treemap floor-plan generation.
- `FMonolithMeshFurnishingActions` — room/building furnishing.
- `FMonolithMeshBuildingValidationActions` — building validation.

Co-registered by `MonolithMesh` (gated by `WITH_GEOMETRYSCRIPT && bEnableProceduralTownGen`, HandlePool dependent):
- `FMonolithMeshBuildingActions` / `FacadeActions` / `RoofActions` / `CityBlockActions` / `TerrainActions` / `ArchFeatureActions`.

## 4. Build.cs dependencies

Public: `Core`, `CoreUObject`, `Engine`
Private: `MonolithCore`, `MonolithMesh` (for shared mesh-family helpers and `MONOLITHMESH_API`-decorated action classes), `MonolithIndex`, `SQLiteCore`, `UnrealEd`, `EditorSubsystem`, `MeshDescription`, `StaticMeshDescription`, `MeshConversion`, `PhysicsCore`, `NavigationSystem`, `RenderCore`, `RHI`, `EditorScriptingUtilities`, `Json`, `JsonUtilities`, `Slate`, `SlateCore`, `AssetRegistry`, `AssetTools`, `MeshReductionInterface`, `MeshMergeUtilities`, `LevelInstanceEditor`, `ImageCore`.

## 5. Notes

MonolithWorldGen.Build.cs probes for the GeometryScripting plugin and sets ``WITH_GEOMETRYSCRIPT`` accordingly. The probe is implemented locally in the Build.cs so UBT rule compilation does not depend on a shared helper class being present in the rules assembly.

## 6. Per-action reference

See the namespace section in [SPEC_MonolithMesh.md](SPEC_MonolithMesh.md) for the per-action params/descriptions (the split table at the top of that spec is authoritative for namespace attribution; row params/descriptions remain in-place pending the per-action re-tabulation backlog item in [../TODO.md](../TODO.md)).
