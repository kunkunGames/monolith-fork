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

`worldgen` is registered by `MonolithWorldGen`. The former MonolithMesh-owned town-generation classes (Building/Facade/Roof/CityBlock/Terrain/ArchFeature) moved into `Source/MonolithWorldGen`, and `MonolithWorldGen` now owns the `WITH_GEOMETRYSCRIPT` handle-pool lifecycle for those actions. `MonolithWorldGen::ShutdownModule` unregisters only actions owned by `MonolithWorldGen`, with no cross-module co-registration required for the `worldgen` namespace.

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

Owned by `MonolithWorldGen` (gated by `WITH_GEOMETRYSCRIPT && bEnableProceduralTownGen`, WorldGen-owned HandlePool dependent):
- `FMonolithMeshBuildingActions` / `FacadeActions` / `RoofActions` / `CityBlockActions` / `TerrainActions` / `ArchFeatureActions`.

## 4. Build.cs dependencies

Public: `Core`, `CoreUObject`, `Engine`
Private: `MonolithCore`, `MonolithMesh` (for exported shared mesh-family helpers and `UMonolithMeshHandlePool`), `MonolithScene` (storytelling/spatial helper types), `MonolithLevelDesign` (acoustic helper types), `MonolithIndex`, `SQLiteCore`, `UnrealEd`, `EditorSubsystem`, `MeshDescription`, `StaticMeshDescription`, `MeshConversion`, `PhysicsCore`, `NavigationSystem`, `RenderCore`, `RHI`, `EditorScriptingUtilities`, `Json`, `JsonUtilities`, `Slate`, `SlateCore`, `AssetRegistry`, `AssetTools`, `MeshReductionInterface`, `MeshMergeUtilities`, `LevelInstanceEditor`, `ImageCore`.

## 5. Notes

MonolithWorldGen.Build.cs gates GeometryScripting on the plugin being enabled for the target, not merely present on disk, and sets ``WITH_GEOMETRYSCRIPT`` accordingly. The gate is implemented locally in the Build.cs so UBT rule compilation does not depend on a shared helper class being present in the rules assembly. When both `WITH_GEOMETRYSCRIPT` and `bEnableProceduralTownGen` are true, the module creates and roots its own `UMonolithMeshHandlePool`, wires it into the town-generation action families, and cleans it up on pre-exit. Tests that name GeometryScripting-only action classes, including the `create_foundation` parameter guard, use the same `WITH_GEOMETRYSCRIPT` compile gate so disabled-plugin targets remain a supported build configuration.

## 6. Per-action reference

See the namespace section in [SPEC_MonolithMesh.md](SPEC_MonolithMesh.md) for the legacy per-action params/descriptions that have not yet been retabulated into this spec. The source-of-truth ownership is this module spec plus the live `monolith_discover({ "namespace": "worldgen", "mode": "actions" })` registry.
