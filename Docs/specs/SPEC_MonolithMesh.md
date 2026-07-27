# Monolith — MonolithMesh Module

**Parent:** [SPEC_CORE.md](../SPEC_CORE.md)
**Engine:** Unreal Engine 5.7+
**Version:** 0.21.3 (Beta)

> **Action-count audit (2026-04-26):** source-of-truth count is **195 core + 45 experimental town gen = 240**, not the previously claimed 197 + 45 = 242. The detailed per-action tables below predate the audit and may sum to slightly different numbers per category — they are accurate per-row but the category subtotals have drifted. A full per-action sweep against `Source/MonolithMesh/Private/Monolith*Actions.cpp` is on the audit backlog.

---

## MonolithMesh

**Dependencies:** Core, CoreUObject, Engine, MonolithCore, MonolithIndex, SQLiteCore, UnrealEd, EditorSubsystem, MeshDescription, StaticMeshDescription, MeshConversion, PhysicsCore, NavigationSystem, RenderCore, RHI, Json, JsonUtilities, Slate, SlateCore, AssetRegistry, AssetTools, MeshReductionInterface, MeshMergeUtilities, LevelInstanceEditor, SourceControl. Optional: GeometryScriptingCore, GeometryFramework, GeometryCore (Tier 5 mesh ops, gates `WITH_GEOMETRYSCRIPT`)

**Build.cs notes — conditional GeometryScripting (v0.14.1):** The Build.cs probes `Engine/Plugins/Runtime/GeometryScripting` and adds `GeometryScriptingCore`, `GeometryFramework`, `GeometryCore` + `WITH_GEOMETRYSCRIPT=1` only when found. **Release escape hatch:** setting `MONOLITH_RELEASE_BUILD=1` (env var) short-circuits detection so `WITH_GEOMETRYSCRIPT=0` regardless — the released DLL no longer carries a hard import on `UnrealEditor-GeometryScriptingCore.dll`. This fixes #26 / #30 where users without GeometryScripting enabled in their `.uproject` were hitting `GetLastError=126` at module load. Mirrors the canonical `MonolithBABridge.Build.cs` pattern (and matches the `MonolithUI` CommonUI detection). Source-tree users with GeometryScripting enabled still get full Tier 5 functionality.

**Delay-loaded GeometryScripting dependency model (issue #70):** Even on a source tree *with* GeometryScripting present, the load-time hard import on the three GeometryScripting DLLs (`UnrealEditor-GeometryScriptingCore.dll`, `UnrealEditor-GeometryFramework.dll`, `UnrealEditor-GeometryCore.dll`) caused a `CouldNotBeLoadedByOS` (`GetLastError=126`) first-build/load race — `MonolithMesh.dll` failed to load on first editor launch, then loaded fine on subsequent launches. Fix: those three DLL names are added to `PublicDelayLoadDLLs` **inside the same `bHasGeometryScripting` block** as the `PrivateDependencyModuleNames` adds. The Windows loader now binds the import **lazily on first Tier-5 GeometryScript call** rather than at `LoadLibrary`, so `MonolithMesh.dll` loads even when the dependency is momentarily unresolvable at module-load time; the full `WITH_GEOMETRYSCRIPT=1` Tier-5 op surface is preserved (nothing compiled out, deps unchanged). **Release-strip interaction:** because the delay-load adds live inside the `bHasGeometryScripting` block, `MONOLITH_RELEASE_BUILD=1` (which forces `bHasGeometryScripting=false`) excludes BOTH the `PrivateDependencyModuleNames` adds and the `PublicDelayLoadDLLs` adds — the released DLL links nothing against GeometryScripting (delay-bound or otherwise) and there is no stray delay-load entry for an unlinked DLL. The `.uplugin` Plugins array is UNCHANGED by this fix, so no new mandatory end-user dependency is introduced (the rejected "drop `Optional:true`" alternative would have been an issue-#32-class release regression). Authoritative closure check: `dumpbin /imports UnrealEditor-MonolithMesh.dll` shows the three DLLs under the delay-load import section, not the normal import table.

### Classes

| Class | Responsibility |
|-------|---------------|
| `FMonolithMeshModule` | Registers 195 core mesh actions across 30+ action classes (+ GeometryScript ops conditional). 45 additional experimental town gen actions registered only when `bEnableProceduralTownGen = true` (default: false). Total: 240 |
| `FMonolithMeshInspectionActions` | Mesh asset inspection: geometry stats, LODs, UVs, materials, collision, quality analysis, catalog (12 actions) |
| `FMonolithMeshReplacementActions` | Module-private registrar for narrow, exact-path, transactional, name-preserving StaticMesh `MeshDescription`/LOD replacement with explicit named/remapped/strict-single-slot material policies, unsupported-feature, numbered-changelist, package rollback, save/reload, and postcondition gates (1 action) |
| `FMonolithMeshSceneActions` | Scene actor manipulation: spawn, move, duplicate, delete, group, batch execute (8 actions) |
| `FMonolithMeshSpatialActions` | Spatial queries: raycasts, sweeps, overlaps, nearest, line of sight, navmesh, scene bounds/stats (11 actions) |
| `FMonolithMeshBlockoutActions` | Level blockout: volumes, primitives, grids, asset matching, replacement, layout import/export, prop scatter (15 actions) |
| `FMonolithMeshProceduralActions` | Procedural geometry: parametric furniture, structures, mazes, pipes, terrain, horror props, sweep-based walls, auto-collision, human-scale defaults, door/window trim frames (8 actions) |
| `FMonolithMeshCacheActions` | Procedural mesh caching: hash-based manifest, list/clear/validate/stats (4 actions) |
| `FMonolithMeshPrefabActions` | Blueprint prefabs: dialog-free HarvestBlueprintFromActors (1 action) |
| `FMonolithMeshBuildingActions` | Grid-based building construction: grid → geometry + Building Descriptor (2 actions) |
| `FMonolithMeshFloorPlanGenerator` | Automatic floor plan generation: treemap layout, archetype loading, corridor insertion (3 actions) |
| `FMonolithMeshFacadeActions` | Facade & window generation: window placement, trim profiles, horror damage (3 actions) |
| `FMonolithMeshRoofActions` | Roof generation: gable, hip, flat/parapet, shed, gambrel (1 action) |
| `FMonolithMeshCityBlockActions` | City block layout: lot subdivision, street geometry, orchestration (4 actions) |
| `FMonolithMeshSpatialRegistry` | Spatial registry: hierarchical JSON descriptor, adjacency graph, BFS pathfinding (10 actions) |
| `FMonolithMeshAutoVolumeActions` | Auto-volume generation: NavMesh, blocking, audio, trigger volumes (3 actions) |
| `FMonolithMeshTerrainActions` | Terrain adaptation: height sampling, foundations, retaining walls (5 actions) |
| `FMonolithMeshArchFeatureActions` | Architectural features: balconies, porches, fire escapes, railings (5 actions) |
| `FMonolithMeshDebugViewActions` | Daredevil debug view: section clip, floor plan capture, camera bookmarks (6 actions) |
| `FMonolithMeshFurnishingActions` | Room furnishing: room-type furniture mapping, placement rules (3 actions) |
| `FMonolithMeshBuildingTypes` | Shared structs: FBuildingGrid, FRoomDef, FDoorDef, FStairwellDef, FBuildingDescriptor |
| `FMonolithMeshCatalog` | Mesh catalog database for search_meshes_by_size and get_mesh_catalog_stats |
| `FMonolithMeshUtils` | Shared helpers for mesh loading, bounds calculation, actor queries |

### Actions (240 — namespace: "mesh")

> **Note:** 195 core actions (Phases 1-22 + Proc Geo Overhaul) always registered + 45 experimental Procedural Town Generator actions (SP1-SP10 + `validate_building`) registered only when `bEnableProceduralTownGen = true` (default: false). Town gen has known geometry issues (wall misalignment, room separation) — very much a work-in-progress. Unless you're willing to dig in and help improve it, it's best left alone for now. Fix Plans v2-v5 addressed 27+ issues but fundamental geometry problems remain.

**Inspection (12)**
| Action | Params | Description |
|--------|--------|-------------|
| `get_mesh_info` | `asset_path` | Full mesh info: vertex/triangle counts, bounds, LODs, materials, collision |
| `get_mesh_bounds` | `asset_path` | Bounding box dimensions and center |
| `get_mesh_materials` | `asset_path` | Material slot names and assigned materials |
| `get_mesh_lods` | `asset_path` | LOD details: vertex/triangle counts, screen sizes |
| `get_mesh_collision` | `asset_path` | Collision geometry: type, complexity, body count |
| `get_mesh_uvs` | `asset_path` | UV channel info: channel count, bounds per channel |
| `analyze_skeletal_mesh` | `asset_path` | Skeletal mesh analysis: bones, sockets, morph targets, physics bodies |
| `analyze_mesh_quality` | `asset_path` | Quality metrics: degenerate triangles, UV distortion, overdraw estimate |
| `compare_meshes` | `asset_path_a`, `asset_path_b` | Side-by-side comparison of two meshes |
| `get_vertex_data` | `asset_path`, `lod`, `section` | Raw vertex data for a mesh section |
| `search_meshes_by_size` | `min_size`, `max_size`, `limit` | Search indexed meshes by bounding box size range |
| `get_mesh_catalog_stats` | none | Mesh catalog database statistics |

**Scene Manipulation (8)**
| Action | Params | Description |
|--------|--------|-------------|
| `get_actor_info` | `actor_name` | Full actor details: class, transform, components, tags |
| `spawn_actor` | `class_name`, `location`, `rotation`, `label` | Spawn an actor in the current level |
| `move_actor` | `actor_name`, `location`, `rotation`, `scale` | Set actor transform |
| `duplicate_actor` | `actor_name`, `offset` | Duplicate an actor with optional offset |
| `delete_actors` | `actor_names` | Delete one or more actors by name |
| `group_actors` | `actor_names`, `group_name` | Group actors under a folder |
| `set_actor_properties` | `actor_name`, `properties` | Set properties on an actor via reflection |
| `batch_execute` | `operations` | Execute multiple scene operations in a single transaction |

**Spatial Queries (11)**
| Action | Params | Description |
|--------|--------|-------------|
| `query_raycast` | `start`, `end`, `channel` | Single-hit raycast with collision response |
| `query_multi_raycast` | `start`, `end`, `channel` | Multi-hit raycast returning all intersections |
| `query_radial_sweep` | `center`, `radius`, `channel` | Radial sphere sweep around a point |
| `query_overlap` | `location`, `extent`, `channel` | Box overlap test at location |
| `query_nearest` | `location`, `radius`, `class_filter` | Find nearest actor of a given class within radius |
| `query_line_of_sight` | `from`, `to`, `ignore_actors` | Line-of-sight check between two points |
| `get_actors_in_volume` | `volume_name` | Get all actors inside a named volume |
| `get_scene_bounds` | none | Get the total bounds of all actors in the level |
| `get_scene_statistics` | none | Scene stats: actor count, triangle count, draw calls, texture memory |
| `get_spatial_relationships` | `actor_name`, `radius` | Get nearby actors and their spatial relationships |
| `query_navmesh` | `start`, `end` | Query navigation mesh for path between two points |

**Level Blockout (16)**
| Action | Params | Description |
|--------|--------|-------------|
| `get_blockout_volumes` | none | List all blockout volumes in the level |
| `get_blockout_volume_info` | `volume_name` | Detailed info about a blockout volume |
| `setup_blockout_volume` | `location`, `extent`, `name`, `tags` | Create a blockout volume for level design |
| `create_blockout_primitive` | `type`, `location`, `scale`, `material` | Create a blockout primitive (box, cylinder, sphere, etc.) |
| `create_blockout_primitives_batch` | `primitives` | Batch-create multiple blockout primitives |
| `create_blockout_grid` | `origin`, `cell_size`, `rows`, `columns` | Create a grid of blockout primitives |
| `match_asset_to_blockout` | `blockout_actor`, `asset_path` | Match a production asset to replace a blockout primitive |
| `match_all_in_volume` | `volume_name`, `asset_mapping` | Match all blockout primitives in a volume to production assets |
| `apply_replacement` | `blockout_actor`, `asset_path` | Replace a blockout actor with a production mesh |
| `set_actor_tags` | `actor_name`, `tags` | Set tags on an actor for blockout categorization |
| `clear_blockout` | `volume_name` | Remove all blockout primitives in a volume |
| `export_blockout_layout` | `volume_name`, `save_path` | Export blockout layout to JSON |
| `import_blockout_layout` | `file_path` | Import a blockout layout from JSON |
| `scan_volume` | `volume_name` | Scan a volume and report contents |
| `scatter_props` | `volume_name`, `asset_paths`, `density`, `seed` | Scatter props randomly within a volume |
| `create_blockout_blueprint` | `save_path`, `force` | Create the BP_MonolithBlockoutVolume Blueprint asset in the project |

**Procedural Mesh Caching (4)** — Hash-based manifest at `Saved/Monolith/ProceduralCache/manifest.json`
| Action | Params | Description |
|--------|--------|-------------|
| `list_cached_meshes` | `type_filter`?, `limit`? (default 100) | List cached procedural mesh entries with asset_path, action, type, dimensions, triangle_count, created_utc |
| `clear_cache` | `type_filter`? | Clear cached meshes — all or filtered by type. Returns cleared_count |
| `validate_cache` | none | Remove stale cache entries where the asset no longer exists on disk. Returns removed_count |
| `get_cache_stats` | none | Cache statistics: total_entries and per-type breakdown |

**Blueprint Prefabs (1)** — Dialog-free blueprint creation from placed actors
| Action | Params | Description |
|--------|--------|-------------|
| `create_blueprint_prefab` | `*actor_names`, `*save_path`, `center_pivot`? (default true), `keep_source_actors`? (default true) | Create a Blueprint from selected actors via HarvestBlueprintFromActors. Every actor must have a **root component** — rootless actors are rejected by name (see below). Returns blueprint_path, asset_name, source_actor_count, component_count |

### `create_blueprint_prefab` requires a scene root per actor

UE 5.8's `FKismetEditorUtilities::HarvestBlueprintFromActors` dereferences
`AActor::GetRootComponent()` with no null check once it identifies more than one root actor
(`Kismet2.cpp`: `SceneComponentOldRelativeTransforms.Emplace(SceneComponent, SceneComponent->GetRelativeTransform())`).
A bare `AActor` has no root component, so harvesting two of them **crashed the whole editor**
with an access violation instead of failing the call — reproduced 2026-07-12 by AssetEditing
task `BEB-428`, which spawned two plain `Actor`s and took down the headless MCP editor mid-run.

A prefab is a spatial assembly: the action centers the pivot from the actors' world locations and
offsets their scene-component templates, so an actor with no scene root cannot belong to one.
`create_blueprint_prefab` therefore validates its inputs before calling the engine and returns an
explicit error naming every offending actor:

```
Cannot harvest a Blueprint prefab from actor(s) with no root component: <names>.
A prefab needs a scene root per actor — spawn a class that has one (for example
StaticMeshActor, or a mesh path) instead of a bare Actor.
```

Covered by the `Monolith.ParamGuard.LevelDesign.CreateBlueprintPrefabRootlessActors` automation
test (which crashes the process without the guard) and by AssetEditing task `BEB-429`, which
asserts the rejection is named and that no prefab asset is left behind.

> **Procedural Geometry Overhaul (2026-03-28):** The proc gen actions (`create_parametric_mesh`, `create_structure`, `create_horror_prop`, etc.) now feature sweep-based thin walls (`wall_mode: "sweep"` default), auto snap-to-floor (`snap_to_floor` param), auto-collision on all saved meshes (`collision: auto/box/convex/complex_as_simple/none`), human-scale defaults (stairs 90/28/18cm, doors 90cm, floor 3cm), door/window/vent trim frames (`add_trim` param), and vent openings via `create_structure`. Collision-aware prop placement uses `collision_mode: none/warn/reject/adjust` on scatter actions with SweepSingle box traces for floor finding. All proc gen actions support `use_cache` and `auto_save` params for the caching system.

### GeometryScript operation range contracts

These actions register only when `WITH_GEOMETRYSCRIPT=1`. Their wire schemas expose the same inclusive bounds that the handlers enforce, so invalid values fail during parameter validation before a mesh-handle lookup or GeometryScript work begins.

| Action | Parameter | Default | Inclusive range | Handler behavior |
|--------|-----------|---------|-----------------|------------------|
| `mesh.generate_collision` | `max_hulls` | `4` | `1..256` | Rejects values outside the range before collision generation. |
| `mesh.generate_lods` | `lod_count` | required | `1..8` | Rejects zero, negative, and more than eight generated LODs. |
| `mesh.generate_lods` | `reduction_per_lod` | `0.5` | `0.1..0.9` | Rejects ratios outside the range before LOD handle creation. |

### Transactional name-preserving StaticMesh geometry replacement

`mesh.replace_static_mesh_geometry_in_place` is the dedicated path for donor-to-target geometry migration when serialized references must continue to resolve to the existing target object/package identity. It does not call or reuse `save_handle` or `import_mesh`, because those paths create or overwrite assets without this action's identity, policy, rollback, reload, and exact-readback contract.

| Contract | Requirement |
|----------|-------------|
| Identity | `source_asset_path` and `target_asset_path` are exact mounted package/object paths. Relative, filesystem, export-text, subobject, extension, and redirect resolution are rejected. Source and target must be distinct exact `UStaticMesh` packages and both packages must already exist on disk; a clean memory-only donor is not treated as committed input. |
| Execute gate | `dry_run=true` is the default. Mutation requires both `dry_run=false` and `confirm=true`. Dry-run performs asset/policy planning and exposes expected/actual source-control readiness but never checks out, mutates, saves, or reloads. Execute additionally requires `GEditor` and rejects active or queued PIE/SIE before source-control preparation or mutation. |
| Material policy | The target `StaticMaterials` array is retained. `preserve_target_by_name` requires every explicit source polygon-group slot to resolve unambiguously with exact case-sensitive spelling on the target. `explicit_remap` requires its key set to exactly equal the source slots used across all LODs and every target value to exactly match a canonical/imported target-slot spelling; unused, misspelled, case-aliased, missing, or ambiguous names fail. `preserve_target_single_slot` is the only policy that accepts an unnamed source or target polygon group: source and target must each have exactly one `StaticMaterial`, every source LOD must have exactly one polygon group and one render section, and the common exact-layout gate must prove the same one-group/one-section layout on every target LOD. It assigns the source group and canonicalizes target-layout readback to the sole canonical target slot; it never guesses an index in a multi-slot mesh. The live target is not mutated during dry-run. Only the committed replacement polygon-group slot name is rewritten during execution. |
| Stable narrow-v1 input | Source and target packages must be clean, every authored LOD must have committed `MeshDescription` plus render data and at least one render section, neither mesh nor its dependencies may be compiling, and the target StaticMesh editor must be closed. Source or target HiRes `MeshDescription`, enabled/built Nanite data, missing polygon-group slot metadata under the named/remap policies, unsupported strict-single-slot topology, or ambiguous target material aliases fail closed. Target-slot resolution uses the shared `MonolithMeshExactNameUtils` spelling guard from the material-slot authoring path; ordinary case-insensitive `FName` lookup cannot make `Wall` satisfy `wall`. Active compilation is rejected instead of synchronously waiting so snapshot/build/reload cannot race async writes. |
| Geometry/LOD | Every committed source `MeshDescription` LOD is cloned and copied (`lod_policy=copy_all_source_lods`). No target asset duplication, rename, move, redirector, or fallback object is created. |
| Section layout | `section_policy=preserve_target_exact_layout` requires equal LOD counts and, for every LOD, equal render-section count, polygon-group count, and ordered remapped target-slot names. Only then are the target `SectionInfoMap` and `OriginalSectionInfoMap` restored byte-for-logical-entry and verified after reload, preserving custom material indices, collision, shadow, ray tracing, distance-field, and opacity flags. |
| Collision policy | `collision_policy=preserve_target_authored_simple` preserves the target's authored simple primitives and every non-transient reflected `UBodySetup` property. `UseSimpleAsComplex` and `UseSimpleAndComplex` are supported when no external `ComplexCollisionMesh` is assigned; the latter deterministically rebuilds derived complex collision from the copied `MeshDescription`. `UseDefault`, complex-as-simple, external complex meshes, collision copying, and collision generation fail closed. The digest covers `AggGeom`, trace/response semantics, physical material, walkable slope, `FBodyInstance` profile/responses, build scale, and authored support flags while excluding derived cooked caches/GUIDs. |
| UV/lightmap/build policy | UV layers are copied as part of each source `MeshDescription` (`uv_policy=copy_source_mesh_description`). The selected lightmap coordinate index must exist on every planned LOD. Lightmap metadata and per-LOD build/reduction/screen settings each require explicit `preserve_target` or `copy_source`; target import filenames and `bImportWithBaseMesh` remain target-owned and are read back. |
| Source control | `source_control_policy=require_checked_out` plus positive integer `target_changelist` require an available provider, tracked/current/non-conflicted target, no other-user checkout, not added/deleted/ignored, and `GetCheckInIdentifier()->GetIdentifier()` exact equality with the requested non-default changelist. The action is registered as handler-owned source control, so the central guard never auto-checks out the coarse request path before this exact numbered-CL validation. The same force-refreshed contract is checked at planning, handler preparation, immediately before mutation, after save, after reload/readback, and during every rollback. Success and post-prepare errors expose only the canonical `source_control_prepare` object with `mode=handler_owned_pre_mutation`, status `validated_exact_numbered_changelist` or `failed`, the expected CL, and `before_action`; the legacy `source_control_prepared` alias is not emitted. Dry-run reports expected/actual/current values. The handler saves the already-checked-out exact package directly with `UPackage::SavePackage`; it does not invoke an editor convenience saver that can silently issue `MarkForAdd`/checkout operations. The action never checks out or moves files between changelists. |
| Transaction/rollback | The action uses `track_dirty_packages` policy without central transaction wrapping because the handler owns one `FScopedTransaction` spanning the exact target mutation. It fingerprints the original target bytes, creates and verifies a byte-identical temporary backup, and every failure after `PreEditChange` restores backup bytes regardless of the save API's return value, verifies original size/MD5, reloads, and verifies the complete original identity/LOD/settings/material/section/lightmap/collision/streaming policy plus source-control state. If rollback is not fully verified, the sole backup is not deleted and its exact path is returned in the error. |
| Persistence proof | A successful call saves the target, revalidates source control, reloads the package non-interactively, resolves the exact target path again, verifies the source-control invariant again, and verifies class/name/package identity, clean package state, per-LOD serialized `MeshDescription` MD5/counts/UVs/render sections, build/reduction/screen/import metadata including `bImportWithBaseMesh`, target materials, both section-info maps, every built section's material/collision/shadow/ray-tracing/distance-field/force-opaque flags, target `NeverStream`, all-LOD lightmap validity, authored-simple collision digest, and non-empty package size/MD5. Section maps and `NeverStream` are restored before `PostEditChange`; `NeverStream` is explicit because UE 5.8 `BuildFromMeshDescriptions` unconditionally sets it to true. |

Automation coverage: the registry test verifies real `StartupModule` registration, handler-owned source-control routing, required/enum schema, safe defaults, and catalog-visible narrow-v1 gates. Param-guard tests cover confirmation, exact path, policy enum, positive changelist, exact remap keys, wrong-case target-slot lookup, pure PIE/SIE state rejection, and an observed `1204` versus required `1203` source-control mismatch that returns canonical structured error data after exactly two read-only checks with no mutation. Persistence tests inject a partial temp-file write and verify exact backup restore plus backup retention on a failed destination. One GUID-mounted Intermediate-only sphere-to-cube fixture helper is reused by dry-run, destructive execute, and rollback tests. The execute tests call the real registered handler with `dry_run=false`, perform real build/save/reload, prove donor bytes remain unchanged, target bytes/geometry change, logical identity/collision remain stable, and the five source-control boundary reads retain CL `1203`; the `SimpleAndComplex` variant clears both donor and target sole polygon-group names and proves `preserve_target_single_slot` plus the authored collision digest survive rebuild/reload. Three consume-once fault seams run after the real build, after the real save, and before success reload; each proves original target size/MD5 and complete in-memory authored state are restored, the package is clean, and rollback revalidates source control. Cleanup removes AssetRegistry entries, unloads packages without resetting the global undo buffer, unregisters the mount, and deletes files only after successful unload. Its failure-only reachability diagnostic deliberately avoids UE 5.8's broken non-direct `ExternalOnly` filter: it searches the remaining fixture cohort once with `Shortest | FullChain` and applies the public-header `FReferenceChain::IsExternal` algorithm locally to each result (`root !IsIn target`). The local helper is required because UE 5.8 declares `IsExternal()` without exporting its implementation from CoreUObject. Cleanup never clears `RF_Standalone`, mutates GC keep flags, or starts an extra collection to force success; a failed unload retains evidence and fails the test. No repository content asset is mutated.

Focused asynchronous verification passed all 12 tests in four controller runs: Registry `6EC2E2B8` 1/1, ParamGuard `D74CD4B9` 6/6, Persistence `938BB912` 2/2, and Workflow `6E6961CF` 3/3. These are asynchronous controller results for this action and its disposable fixtures; they do not establish or claim a fix for any synchronous runner transaction issue. The production 11-mesh application and its per-asset visual/content proof are isolated in CL1207, not CL1203.

Publication ownership: this action adds one public registry entry. Count-bearing aggregate files and the generated catalog snapshot remain in their existing publication-owner changelist rather than being partially stolen into CL 1203; the live registry, this module spec, the mesh skill, and its action reference contain the complete action contract. The integration submit gate must run the repository static catalog check against that owner state together with CL 1203.

### Mesh Import (`import_mesh` — automated FBX/glTF pipeline)

Handler: `FMonolithMeshTechArtActions::ImportMesh` (`Source/MonolithMesh/Private/MonolithMeshTechArtActions.cpp`). Drives the engine's `IAssetTools::ImportAssetsAutomated` path via `UFbxImportUI` + `UFbxFactory` for FBX inputs (and the engine default factory for glTF / other formats). Backwards-compatible with the original static-mesh-only contract: omitting the new skeletal flags reproduces pre-PR behaviour exactly.

**Schema:**

| Param | Type | Required | Default | Description |
|-------|------|----------|---------|-------------|
| `files` | array<string> | required | — | Absolute file paths to import (FBX, glTF, etc.) |
| `destination` | string | required | — | Content path (e.g. `/Game/Characters/Skeleton/MyCharacter`) |
| `replace_existing` | boolean | optional | `false` | Overwrite assets that already exist at `destination` |
| `combine_meshes` | boolean | optional | `true` | Combine all FBX sub-meshes into a single asset |
| `generate_lightmap_uvs` | boolean | optional | `true` | Generate a lightmap UV channel on import |
| `auto_generate_collision` | boolean | optional | `true` | Generate collision primitives on import |
| `normal_import_method` | string | optional | `ImportNormalsAndTangents` | `ImportNormals` / `ImportNormalsAndTangents` / `ComputeNormals` |
| `material_import` | string | optional | `create_new` | `create_new` (new MIs), `find_existing` (reuse by name), `skip` (no material/texture import) |
| **`import_as_skeletal`** | boolean | optional | `false` | **NEW.** Selects `USkeletalMesh` import path: sets `UFbxImportUI::bImportAsSkeletal=true` + `MeshTypeToImport=FBXIT_SkeletalMesh`. Engine auto-creates a `USkeleton` companion asset under `destination` when no existing skeleton is supplied. |
| **`import_animations`** | boolean | optional | `false` | **NEW.** Imports animation sequences alongside the skeletal mesh via `UFbxImportUI::bImportAnimations=true`. **Forces `import_as_skeletal=true`** if not already set — animation import is not paired with the static-mesh branch. |

**FBX branch behaviour** (when at least one input file ends in `.fbx`):

- **Skeletal path** (`import_as_skeletal=true` OR `import_animations=true`): skeletal mesh + auto-created skeleton; anim sequences imported only when `import_animations=true`.
- **Static path** (default, no skeletal flags): identical to pre-PR behaviour — `bImportAsSkeletal=false`, `MeshTypeToImport=FBXIT_StaticMesh`, `bImportAnimations=false`.

**Response shape:** for each imported `UObject`, returns `{ asset_path, type }`. When the imported object is a `UStaticMesh`, additional fields are populated: `vertex_count`, `triangle_count`, `material_slots[]`, `bounds`. The skeletal-result branch currently emits only `{ asset_path, type }` per imported asset — bone count, skeleton path, and morph-target metadata enrichment is **(WISHLIST)** for the skeletal-result JSON branch.

**Wishlist / v2 follow-ups:**

- **(WISHLIST)** `skeleton_asset` param — supply an existing `USkeleton` asset path to bind imported animations to, rather than auto-creating a new skeleton under `destination`. Enables retargeting onto an existing character rig.
- **(WISHLIST)** Enriched skeletal-result JSON branch — bone count, skeleton asset path, socket count, morph target count, physics asset reference parity with the existing `UStaticMesh` stat block.
- **(WISHLIST)** glTF skeletal/animation parity — confirm glTF factory honors the same boolean semantics, or document the divergence.

**Credit:** integration-test contribution from @4698to (downstream UE 5.7 fork). See `Docs/FEEDBACK_import_mesh_skeletal_params.md` for the upstream contributor's full design rationale (bilingual EN/中文).

### Procedural Town Generator (45 gated + 1 always-registered actions — 11 sub-projects) — WORK-IN-PROGRESS

> **Status:** Work-in-progress, disabled by default (`bEnableProceduralTownGen = false`). Fix Plans v2-v5 addressed 27+ issues but fundamental geometry problems remain (wall misalignment, room separation). Very much a WIP — unless you're willing to dig in and help improve it, it's best left alone.

Procedural city block generation from a single MCP call. 11 sub-projects composing into a pipeline: grid-based buildings with connected rooms, roofs, facades, furniture, lighting, horror dressing, navmesh, and volumes, all adaptive to terrain. The critical interface is the **Building Descriptor** — a JSON contract that SP1 outputs and SP2-SP10 consume. All building specs are generated server-side (not sent over MCP wire).

**Master plan:** `Docs/plans/2026-03-28-proc-town-generator-master-plan.md`

**Building Descriptor Contract (Critical Interface)**

SP1's `create_building_from_grid` returns a JSON descriptor consumed by all downstream SPs. Key fields:
- `building_id`, `asset_path`, `actors[]` — building identity and spawned actors
- `footprint_polygon` — 2D building outline for roof generation
- `floors[]` — per-floor data: `rooms[]` (room_id, room_type, grid_cells, world_bounds), `doors[]` (connects, wall, width), `stairwells[]`
- `exterior_faces[]` — wall segments for facade decoration (normal, width, height, is_exterior)
- `grid_cell_size`, `wall_thickness`, `materials_assigned`, `tags_applied`

**SP1: Grid-Based Building Construction (2 actions)**
| Action | Params | Description |
|--------|--------|-------------|
| `create_building_from_grid` | `grid`, `rooms`, `doors`, `floors`, `cell_size`?, `materials`?, `omit_exterior_walls`? | Grid of room IDs → geometry + Building Descriptor. Auto-detects interior/exterior walls, generates floor/ceiling slabs, stairwell cutouts, trim frames, actor tags. `omit_exterior_walls` (default false) skips exterior wall generation for facade-only workflows |
| `create_grid_from_rooms` | `rooms`, `adjacency` | Room list + adjacency requirements → grid layout |

**SP2: Automatic Floor Plan Generation (3 actions)**
| Action | Params | Description |
|--------|--------|-------------|
| `generate_floor_plan` | `archetype`, `width`, `depth`, `floors`?, `hospice_mode`? | Building archetype + footprint → grid + rooms + doors. Squarified treemap with per-floor room assignment, aspect ratio enforcement, footprint boundary validation, and guaranteed exterior entrance on ground floor. Corridor width min 120cm, door width min 90cm. Accessibility mode (`hospice_mode`): 100cm doors, 180cm corridors, rest alcoves |
| `list_building_archetypes` | none | List available archetype definitions (residential, clinic, police_station, apartment, etc.) |
| `get_building_archetype` | `archetype` | Get archetype JSON: room types, sizes, adjacency requirements |

**SP3: Facade & Window Generation (3 actions)**
| Action | Params | Description |
|--------|--------|-------------|
| `generate_facade` | `building_descriptor`, `style`?, `damage`? | Exterior walls → windows, doors, trim, cornices, storefronts. CGA-style vertical split (base/shaft/cap). Optional horror damage |
| `list_facade_styles` | none | List available facade style presets (Victorian, Colonial, Brutalist, Abandoned) |
| `apply_horror_damage` | `building_descriptor`, `decay` | Apply horror damage to facades: boarded windows, broken glass, rust stains |

**SP4: Roof Generation (1 action)**
| Action | Params | Description |
|--------|--------|-------------|
| `generate_roof` | `building_descriptor`, `roof_type`?, `overhang`? | Footprint polygon → roof geometry (gable, hip, flat/parapet, shed, gambrel). Separate MaterialID for roof surface |

**SP5: City Block Layout (4 actions)**
| Action | Params | Description |
|--------|--------|-------------|
| `create_city_block` | `buildings`, `genre`?, `seed`?, `block_size`?, `decay`? | Top-level orchestrator. Subdivides block → generates buildings → facades → roofs → streets → horror decay. Graceful degradation if SPs unavailable |
| `create_lot_layout` | `block_size`, `lot_count`?, `seed`? | Subdivide block into lots (OBB recursive), return lot positions and footprint shapes |
| `create_street` | `block_bounds`, `lot_positions` | Generate street geometry: sidewalks, curbs, road surface |
| `place_street_furniture` | `street_bounds`, `density`?, `seed`? | Place lamps, hydrants, benches, trash cans along streets |

**SP6: Spatial Registry (10 actions)**
| Action | Params | Description |
|--------|--------|-------------|
| `register_building` | `building_descriptor` | Register a building in the spatial registry |
| `register_room` | `building_id`, `room` | Register an individual room |
| `register_street_furniture` | `block_id`, `actors` | Register street furniture actors |
| `query_room_at` | `position` | Query what room is at a world position |
| `query_adjacent_rooms` | `room_id` | Query rooms adjacent to a given room |
| `query_rooms_by_filter` | `filter` | Query rooms by type, floor, building, or tags |
| `query_building_exits` | `building_id` | Query all exit points from a building |
| `path_between_rooms` | `from_room`, `to_room` | BFS pathfinding between two rooms through the adjacency graph |
| `save_block_descriptor` | `block_id`, `save_path`? | Persist block descriptor to JSON |
| `load_block_descriptor` | `file_path` | Load a persisted block descriptor |

**SP7: Auto-Volume Generation (3 actions)**
| Action | Params | Description |
|--------|--------|-------------|
| `auto_volumes_for_building` | `building_descriptor` | Auto-spawn NavMeshBounds, BlockingVolume, AudioVolume (reverb by room size), TriggerVolume for a building |
| `auto_volumes_for_block` | `block_id` | Auto-volumes for all buildings in a block + navmesh build |
| `spawn_nav_link` | `location`, `left_point`, `right_point` | Spawn a NavLinkProxy between two points |

**SP8a: Terrain + Foundations (5 actions)**
| Action | Params | Description |
|--------|--------|-------------|
| `sample_terrain_grid` | `origin`, `extent`, `resolution` | Sample NxM height grid via downward traces |
| `analyze_building_site` | `footprint`, `terrain_grid` | Analyze site slope and recommend foundation strategy (Flat/CutAndFill/Stepped/Piers/WalkoutBasement) |
| `create_foundation` | `building_descriptor`, `strategy`?, `hospice_mode`? | Generate foundation geometry. ADA-compliant ramps when `hospice_mode` is enabled (1:12 slope, 76cm max rise, 150cm landings) |
| `create_retaining_wall` | `path`, `height`, `material`? | Generate retaining wall geometry along a path |
| `place_building_on_terrain` | `building_descriptor`, `terrain_grid` | Adapt a building to uneven terrain with auto-selected foundation |

**SP8b: Architectural Features (5 actions)**
| Action | Params | Description |
|--------|--------|-------------|
| `create_balcony` | `building_descriptor`, `floor`, `face`, `width`?, `depth`?, `building_context`? | Floor slab + railing extending from upper floor exterior. `building_context` enables collision checks against existing geometry. Returns `wall_openings` for facade integration |
| `create_porch` | `building_descriptor`, `face`, `depth`?, `columns`?, `building_context`? | Ground-level covered entry with columns. `building_context` enables collision checks. Returns `wall_openings` for facade integration |
| `create_fire_escape` | `building_descriptor`, `face`, `floors`?, `building_context`? | Zigzag exterior stairs between floor landings (45-degree angle). `building_context` enables collision checks. Returns `wall_openings` for facade integration |
| `create_ramp_connector` | `start`, `end`, `width`?, `slope`?, `building_context`? | ADA-compliant ramp between two heights with switchback support. Returns `wall_openings` for facade integration |
| `create_railing` | `path`, `height`?, `style`? | Swept profile railing along edge path |

**SP9: Daredevil Debug View (6 actions)**
| Action | Params | Description |
|--------|--------|-------------|
| `toggle_section_view` | `z_height`?, `enabled`? | MPC-based section clip — hide everything above a Z height |
| `toggle_ceiling_visibility` | `visible`?, `floor`? | Toggle ceiling/roof visibility via actor tags (BuildingCeiling, BuildingRoof) |
| `capture_floor_plan` | `building_descriptor`, `floor`?, `output_path`? | Orthographic top-down floor plan capture to PNG |
| `highlight_room` | `room_id`, `color`?, `duration`? | Room highlighting with overlay materials |
| `save_camera_bookmark` | `name`, `location`?, `rotation`? | Save current or specified camera viewpoint |
| `load_camera_bookmark` | `name` | Restore a saved camera viewpoint |

**SP10: Room Furnishing Pipeline (3 actions)**
| Action | Params | Description |
|--------|--------|-------------|
| `furnish_room` | `building_descriptor`, `room_id`, `preset`?, `disturbance`? | Place appropriate furniture per room type. Horror dressing via optional disturbance level (orderly/slightly_messy/ransacked/abandoned) |
| `furnish_building` | `building_descriptor`, `decay`? | Furnish all rooms in a building, applying horror decay per room |
| `list_furniture_presets` | `room_type`? | List available furniture preset configurations per room type |

**Validation (1 action)**
| Action | Params | Description |
|--------|--------|-------------|
| `validate_building` | `building_descriptor` | Post-generation validation: checks room connectivity, door reachability, stair angle limits, wall thickness, exterior entrance existence, floor slab coverage. Returns `valid` bool + `issues[]` with severity, location, and description per problem found |

### Fix Plan v2 Changes (2026-03-28)

20 issues fixed across 3 phases targeting building generation correctness and playability:

**Phase 1 — Geometry Fixes:**
1. Building stairs angle reduced from 70 degrees to 32 degrees (standard residential)
2. Building stairs switchback support for multi-story buildings
3. Fire escape angle reduced from 66 degrees to 45 degrees
4. Ramp connector switchback self-intersection fix
5. Exterior wall omission via `omit_exterior_walls` param on `create_building_from_grid`
6. Wall thickness validation (minimum 10cm, maximum 60cm)

**Phase 2 — Floor Plan Fixes:**
7. Corridor minimum width enforced at 120cm
8. Door minimum width enforced at 90cm
9. Per-floor room assignment in `generate_floor_plan` (bedrooms upstairs, living areas ground floor)
10. Room aspect ratio enforcement (no rooms narrower than 1:4)
11. Footprint boundary validation (rooms cannot exceed building footprint)
12. Guaranteed exterior entrance on ground floor

**Phase 3 — Integration Fixes:**
13. `building_context` param on architectural feature actions (balcony, porch, fire escape, ramp)
14. `wall_openings` output on architectural feature actions for facade coordination
15. Stairwell ceiling cutout geometry correctness
16. Floor slab coverage validation (no gaps between rooms)
17. Room connectivity validation (all rooms reachable)
18. Door placement validation (doors on shared walls only)
19. `validate_building` action added for post-generation integrity checks
20. Graceful error reporting with per-issue severity levels

### Bulk Fill & Describe Surface (2026-05-11)

`MonolithMeshBulkFillAdapter` registers under `FMonolithBulkFillRegistry` for the `mesh` namespace, exposed via the framework-level `bulk_fill_query("apply", ...)` and `describe_query("schema", ...)` dispatchers. Phase 5 of the MCP ergonomics rollout (design spec `Docs/plans/2026-05-11-monolith-mcp-ergonomics-design.md`).

**Surface summary.** `bulk_fill_query("apply", target_namespace="mesh", target="<asset_or_actor>", tree={...})` covers two distinct fanout paths: DataTable row authoring for surface mapping (the `create_surface_datatable` / `create_room_template` / `create_prop_kit` row-by-row pain) and a v1 audit-only wrapper over `set_actor_properties` to surface the Mobility-ordering folklore. `describe_query("schema", target_namespace="mesh", target="<actor_class>")` returns actor properties + nested struct paths (`StaticMeshComponent.OverrideMaterials[N]`, `BodyInstance.CollisionProfileName`).

**fill_kind catalogue (2 — enumerated against `MonolithMeshBulkFillAdapter.cpp`):**

| `fill_kind` | Target shape | Walks |
|---|---|---|
| `SurfaceDataTable` | `UDataTable` | `rows:{}` written as DataTable rows (e.g. `{"Wood":{...},"Metal":{...}}`). Assumes the row struct already exists |
| `ActorProperties` | Spawned actor path | **v1 audit-only** wrapper for `mesh_query("set_actor_properties")`. Surfaces the Mobility-must-be-Movable-before-`bSimulatePhysics=true` dependency. Writes still flow through the existing action |

**Sample tree (SurfaceDataTable):**

```json
{
  "target": "/Game/Audio/DT_FootstepSurfaces",
  "tree": {
    "fill_kind": "SurfaceDataTable",
    "rows": {
      "Wood":   {"FootstepSC": "/Game/Audio/SC_Wood",   "ImpactSC": "/Game/Audio/SC_WoodImpact"},
      "Metal":  {"FootstepSC": "/Game/Audio/SC_Metal",  "ImpactSC": "/Game/Audio/SC_MetalImpact"},
      "Carpet": {"FootstepSC": "/Game/Audio/SC_Carpet"}
    }
  },
  "dry_run": true
}
```

**Adapter-specific quirks.**

- **DT row-struct synthesis is impossible from MCP — `(WISHLIST)`.** The `SurfaceDataTable` fill_kind assumes the row struct already exists on the target DataTable. Authoring a USTRUCT from a JSON shape is reflection-bound but blocked on UE not supporting runtime struct synthesis. Schema returns the existing row struct's settable surface; bulk_fill rejects writes to a DataTable with no row struct attached with `"target DataTable has no row struct — synthesise the struct in editor first or use an existing DT as template"`.
- **Mobility ordering surfaces in describe_schema.** Actor schema descriptors annotate `bSimulatePhysics` with `conditional_on: "Mobility == Movable"`. The `ActorProperties` fill_kind audits the property order — if a tree writes `bSimulatePhysics: true` before `Mobility: "Movable"` (or omits the Mobility write entirely when the current value is Static / Stationary), the dry-run surfaces a `SilentDrops` entry naming the dependency. This is the v1 audit-only behaviour referenced by the cited adapter source.
- **`monolith_reindex` is a silent prerequisite for `search_meshes_by_size`.** Schema descriptors for mesh-catalog-dependent actions annotate `reindex_required: true`. Dry-run reports flag stale catalog state and recommend a `monolith_reindex` call before bulk_fill of catalog-keyed fields.
- **`v1 ActorProperties fill_kind is audit-only (Mobility-guard) — write still through existing `mesh_query("set_actor_properties")`.** Cited verbatim from design spec; adapter refuses to commit actor writes in v1 and points callers at the existing action.
- **`batch_execute` interaction.** Bulk_fill is single-transaction by design and does NOT inherit `batch_execute`'s flat-key shape (which would conflict with nested params). Mesh's existing `batch_execute` action remains available alongside.

**Limitations / v1.1 follow-ups.**

- CSV/JSON-array ingestion on `create_surface_datatable` / `create_room_template` / `create_prop_kit` — covered minimally by SurfaceDataTable's nested-rows shape in v1; full CSV path `(WISHLIST v1.1)` per Q2.
- `apply_replacement` / `match_all_in_volume` / `scatter_props` confidence-score preview — `(WISHLIST v1.1)` — dry_run integration on the existing actions.
- ActorProperties non-audit (real write) fill_kind — `(v1.1)` — blocked until the Mobility-ordering audit surfaces every silent-drop case in the existing `set_actor_properties` action.
- `create_blockout_blueprint` BP logic authoring — `(WISHLIST)` — bulk_fill cannot synthesise Blueprint graphs.

**Data Files (Procedural Town Generator)**
| Directory | Sub-Project | Contents |
|-----------|------------|----------|
| `Saved/Monolith/BuildingArchetypes/` | SP2 | JSON room catalogs per building type (residential_house, clinic, police_station, apartment) |
| `Saved/Monolith/FacadeStyles/` | SP3 | JSON facade presets (Victorian, Colonial, Brutalist, Abandoned) |
| `Saved/Monolith/BlockPresets/` | SP5 | JSON block configuration presets |
| `Saved/Monolith/SpatialRegistry/` | SP6 | Persisted block descriptors |
| `Saved/Monolith/CameraBookmarks/` | SP9 | Saved camera positions |
| `Saved/Monolith/FurniturePresets/` | SP10 | Room-type furniture configs per room type (kitchen, bedroom, bathroom, office, lobby, corridor) |

---
