---
name: unreal-scene
description: Use when querying or manipulating the live editor scene/level via Monolith MCP (scene namespace) — spawn/move/align/group actors, spatial queries (raycast/overlap/nearest/line-of-sight/navmesh-path), room registry, volumes, navmesh build and nav links, lighting analysis, decals, section/floor-plan views, sublevels, level metadata. For a mesh ASSET (tris/LOD/collision/UVs/GeometryScript) use unreal-mesh; for procedural blockout/town generation use unreal-worldgen; for horror/accessibility/audio analysis on these primitives use unreal-leveldesign; for persistent AI navmesh/navlink behavior and BT/EQS use unreal-ai; to pack/commit/break instanced level content use unreal-level-instance. Triggers on actor, spawn actor, spawn an enemy, move actor, align actors, group actors, outliner folder, raycast, overlap, nearest, line of sight, navmesh path, scene bounds, scene statistics, volume, trigger volume, build navmesh, nav link, light coverage, dark corners, decal, section view, floor plan, camera bookmark, sublevel.
---

# unreal-scene

**56 always-on actions + ~20 feature-gated actions** via `scene_query(action, params)`. The 56 always-on actions (Scene, Spatial, Volume, Level Design, Lighting, Decal, Editor Level Metadata, Editor Map groups) are always registered and carry full signatures below. The ~20 feature-gated actions (Spatial Registry, Debug View, Auto Volume groups) register only when their owning feature is enabled and are **absent from the default catalog dump**; confirm their params via `monolith_discover` once enabled. Action names below are the live registry surface; call `monolith_discover` for exact parameter schemas.

## Discovery

```
monolith_discover({ namespace: "scene" })                      # all actions in this namespace
monolith_discover({ namespace: "scene", action: "<action>", mode: "schema" })  # exact params
```

**Param notation:** `name*` required, `name?` optional, `name=val` default, `a/b/c` allowed values, `[w]` mutates (transaction-wrapped). Signatures below are a snapshot of the live catalog — for the exact full schema call `monolith_discover` with `mode: "schema"` (the discover block above is the authority).

## When to use

Use **unreal-scene** for live-level work: placing/moving/aligning/grouping actors, raw spatial primitives (raycast/overlap/nearest/line-of-sight/navmesh-path/radial-sweep), volumes, navmesh build, nav links, lighting analysis, decals, debug section/floor-plan views, and level/map metadata.

Use a different skill for:

- **Mesh ASSET** inspection/editing (tris/LOD/collision/UVs/GeometryScript) → `unreal-mesh`. This skill places and moves mesh *actors*; it does not edit the mesh asset.
- **Procedural blockout / town / building** generation that populates a level → `unreal-worldgen`. This skill does manual spawn/move/align, not procedural generation. (Building/room **registration** lives here in `scene`, though — see Spatial Registry.)
- **Higher-level horror / accessibility / audio analysis** built on these spatial primitives (sightlines, tension, stealth, RT60, hospice) → `unreal-leveldesign`. Use `scene` for the raw raycast/overlap/navmesh queries those passes consume.
- **Persistent AI navmesh/navlink BEHAVIOR** and BT/EQS/perception setup → `unreal-ai`. Use `scene` for a one-off navmesh-path / line-of-sight query on live actors and to `build_navmesh` / `spawn_nav_link`.
- **Packing/committing/breaking instanced level content** (Level Instances, Packed Level Actors) → `unreal-level-instance`. Use `scene` to place ordinary actors in the level.

## Action Reference

### Scene (11)

| Action | Signature | Purpose |
|--------|-----------|---------|
| `[w] align_actors` | `actor_names* axis*(X/Y/Z) mode*(min/max/center/distribute)` | Align multiple actors along an axis. Modes: min/max (snap to extremes), center (average), distribute (spread evenly between min/max). |
| `[w] batch_execute` | `actions*` | Execute up to 200 actions in one undo transaction; no nesting. Stops at first failure, ends the transaction for Undo, and does not claim automatic rollback. |
| `[w] delete_actors` | `actor_names*` | Exactly delete 1–1000 distinct actors in the active current map. Accepts exact object paths or unique internal names/labels; preflights the complete set and verifies exact identity/path absence afterward. Does NOT delete asset files. |
| `[w] duplicate_actor` | `actor_name* new_name? offset=[0,0,0]` | Duplicate an actor in the editor world with an optional position offset |
| `get_actor_info` | `actor_name*` | Get comprehensive info for an actor in the editor world (class, transform, mesh, materials, tags, components, bounds) |
| `[w] group_actors` | `actor_names* group_name*` | Move actors into an outliner folder (creates folder hierarchy automatically) |
| `[w] manage_folders` | `sub_action*(list/delete/rename/move) folder? new_folder? include_subfolders=true` | Manage World Outliner folders: list, delete (move actors to root), rename, or move folders. |
| `[w] move_actor` | `actor_name* location? rotation? scale? relative=false` | Persistently move/rotate/scale a non-transient editor actor. Set relative=true to offset from current transform. Fails closed before mutation if the actor has no root component, if the actor/root/owning package is transient, or if the owning package cannot be dirtied. An effective no-op succeeds without opening a transaction and leaves the package clean. |
| `[w] set_actor_properties` | `actor_name* mobility?(Static/Stationary/Movable) simulate_physics? collision_preset? cast_shadow? tags? mass_kg?` | Set properties on an actor (mobility, physics, collision, shadows, tags, mass) |
| `[w] snap_to_floor` | `actor_names* trace_distance=10000` | Snap actors to the floor by tracing downward. Places the bottom of each actor on the hit surface. |
| `[w] spawn_actor` | `class_or_mesh* location* rotation=[0,0,0] scale=[1,1,1] name? folder?` | Spawn an actor in the editor world. Path starting with '/' spawns StaticMeshActor with that mesh; otherwise spawns by class name. |

### Spatial (11)

| Action | Signature | Purpose |
|--------|-----------|---------|
| `get_actors_in_volume` | `volume_name*` | Get all actors inside a named BlockingVolume. Checks Monolith.Owner tags and spatial containment. |
| `get_scene_bounds` | `class_filter?` | Compute the enclosing axis-aligned bounding box for all actors (or filtered by class). |
| `get_scene_statistics` | `region_min? region_max?` | Get scene statistics: actor counts by class, total triangles, light count, volume count, navmesh status. Optional region filter. |
| `get_spatial_relationships` | `actor_name* radius=500 limit=20` | Analyze spatial relationships between an actor and its neighbors (on_top_of, inside, adjacent, above, below, etc). Thresholds scale with actor bounds. |
| `query_line_of_sight` | `from* to* ignore_actors?` | Check line of sight between two points. Returns visibility status and blocking info. |
| `query_multi_raycast` | `start* end* channel=Visibility max_hits=10` | Fire a multi-hit raycast. Returns all hits sorted by distance. |
| `query_navmesh` | `start* end* agent_radius=42` | Find a navigation path between two points. Returns path points and total distance. Errors if navmesh not built. |
| `query_nearest` | `location* class_filter? tag_filter? radius=5000 limit=20` | Find nearest actors using physics broadphase (OverlapMultiByObjectType). Filter by class and/or tag, sorted by distance. |
| `query_overlap` | `location* shape*(box/sphere/capsule) extent* channel=Visibility` | Perform a shape overlap test (box, sphere, or capsule) at a location. extent: [x,y,z] for box, float radius for sphere, [radius, half_height] for capsule. |
| `query_radial_sweep` | `origin* radius=1000 ray_count=36(max72) vertical_angles=3(max8) channel=Visibility` | Fire rays in a radial pattern from origin. Returns summary by compass direction. Hard cap: ray_count * vertical_angles <= 512. |
| `query_raycast` | `start* end* channel=Visibility ignore_actors?` | Fire a single raycast in the editor world. channel: Visibility/Camera/WorldStatic/WorldDynamic/Pawn/PhysicsBody/Vehicle/Destructible/GameTraceChannel1-18. Returns hit data including actor, location, normal, distance, physical material. |

### Spatial Registry (10) — feature-gated

> **Feature-gated:** these 10 actions are absent from the default catalog dump and register only when the spatial-registry feature is enabled. Signatures are not snapshotted here; confirm params via `monolith_discover({ namespace: "scene", action: "<action>", mode: "schema" })` once enabled.

| Action | Purpose |
|--------|---------|
| `load_block_descriptor` | Load a previously saved spatial registry from a JSON file. |
| `path_between_rooms` | BFS shortest path between two rooms through the door/stairwell adjacency graph. |
| `query_adjacent_rooms` | Get all rooms connected to a given room via doors or stairwells. |
| `query_building_exits` | Get all exterior doors of a building. |
| `query_room_at` | Find which room contains a given world position (point-in-AABB test). |
| `query_rooms_by_filter` | Query rooms by type, floor, building, tags, or area range. |
| `register_building` | Register a building from its Building Descriptor JSON in the spatial registry. Extracts all rooms, doors, stairwells and builds the adjacency graph. |
| `register_room` | Register a single room in the spatial registry (for manual/incremental registration). |
| `register_street_furniture` | Register street furniture items (lamps, hydrants, benches, etc.) in the spatial registry. |
| `save_block_descriptor` | Save the spatial registry for a block to a JSON file on disk. |

### Debug View (7) — feature-gated

> **Feature-gated:** these 7 actions are absent from the default catalog dump and register only when the debug-view feature is enabled. Confirm params via `monolith_discover(... mode: "schema")` once enabled.

| Action | Purpose |
|--------|---------|
| `capture_building_views` | Multi-angle diagnostic capture of a building for quality review. Produces 6 views: floor_plan (orthographic top-down), north/south/east/west (orthographic elevations), and perspective (45-degree corner view). All saved as PNGs. Uses spatial registry for building bounds. |
| `capture_floor_plan` | Orthographic top-down scene capture of a building floor, saved as PNG. Auto-hides ceilings/roofs before capture and restores afterward. Uses spatial registry for building bounds. |
| `highlight_room` | Spawn a translucent overlay box at a room's world bounds for visual debugging. Tracked by room_id for cleanup. Use clear=true to remove a specific highlight. |
| `load_camera_bookmark` | Restore the editor viewport camera to a previously saved bookmark position. |
| `save_camera_bookmark` | Save the current editor viewport camera position and rotation as a named bookmark. Stored as JSON in Saved/Monolith/CameraBookmarks/. |
| `toggle_ceiling_visibility` | Show or hide actors tagged with BuildingCeiling and BuildingRoof. Useful for top-down inspection of procedural buildings without section clipping. |
| `toggle_section_view` | Section-cut debug view: hide all actors above a Z height to reveal building interiors. Tracks hidden actors for clean restoration. Can resolve clip_height from building_id + floor_index. |

### Volume (7)

| Action | Signature | Purpose |
|--------|-----------|---------|
| `[w] build_navmesh` | `mode=full(full/dirty_only)` | Trigger navigation mesh rebuild. Synchronous — blocks the game thread. Can take seconds on large maps. |
| `[w] copy_actor_properties` | `source_actor* target_actors* properties? component_class?` | Copy UPROPERTY values from a source actor to one or more target actors. Optionally filter to specific properties. |
| `get_actor_properties` | `actor_name* properties? component? include_defaults=false` | Read arbitrary UPROPERTY values from an actor or its components via FProperty reflection. Returns string-serialized values. |
| `[w] select_actors` | `sub_action*(select/deselect/clear/get/focus) actors? filter? add_to_selection=false focus_camera=false` | Control editor actor selection. Select, deselect, clear selection, get current selection, or focus camera on actors. |
| `[w] set_collision_preset` | `actor_name* preset* component?` | Set the collision profile on an actor's root primitive component (or a named component). |
| `[w] snap_to_surface` | `actors* direction=[0,0,-1] trace_length=10000 align_to_normal=true offset=0 channel=WorldStatic` | Drop actors onto geometry via directional trace. Unlike snap_to_floor, supports any direction and surface normal alignment. |
| `[w] spawn_volume` | `type*(trigger/blocking/kill/pain/nav_modifier/audio/post_process) location* extent=[500,500,300] rotation=[0,0,0] name? folder? properties?` | Spawn a volume actor with proper brush geometry. properties: type-specific (e.g. damage_per_sec for pain, reverb_effect for audio). |

### Level Design Placement (6)

| Action | Signature | Purpose |
|--------|-----------|---------|
| `get_level_actors` | `class_filter? tag_filter? sublevel_filter? mesh_wildcard? name_wildcard? volume_name? radius? center? limit=200` | Enumerate actors in the editor world with multi-filter AND logic. Returns name, class, location, mesh, sublevel, tags. |
| `[w] manage_sublevel` | `sub_action*(create/add/remove/move_actors) level_path? streaming_class=LevelStreamingDynamic(/LevelStreamingAlwaysLoaded) actor_names? dest_level?` | Create/load/unload streaming sublevels or move actors between levels. level_path required for create/add/remove. |
| `[w] measure_distance` | `from* to* include_navmesh_path=false` | Measure distance between two actors or world points. Returns euclidean, horizontal, height difference, and optional navmesh path distance. |
| `[w] place_blueprint_actor` | `blueprint* location* rotation=[0,0,0] scale=[1,1,1] properties? label? folder?` | Spawn a Blueprint actor in the world with optional property configuration via reflection. |
| `[w] place_spline` | `points*(min2) mesh_path? forward_axis=X(X/Y/Z) point_type=Curve(Linear/Curve/Constant/CurveClamped/CurveCustomTangent) scale=[1,1] close_loop=false label? location=[0,0,0]` | Spawn an actor with a spline component. Optionally places mesh segments along the spline (pipes, cables, railings). |
| `[w] randomize_transforms` | `actor_names* offset_range=[0,0] yaw_range=[0,360] pitch_range=[0,0] roll_range=[0,0] scale_range=[1,1] seed=0` | Apply random offset/rotation/scale variation to actors for an organic feel. Deterministic with seed. |

### Level Design Editing (5)

| Action | Signature | Purpose |
|--------|-----------|---------|
| `get_actor_component_properties` | `actor_name* component_name? properties? component_class?` | Read arbitrary component properties via FProperty reflection. Returns typed values for any UPROPERTY. |
| `[w] set_actor_component_properties` | `actor_name* properties*(object {name:value}) component_name? component_class? save=false` | Write arbitrary component properties on a live level actor via FProperty reflection (ImportText). Values string/number/bool. Reports applied/skipped; optional save persists the owning level. Write counterpart to get_actor_component_properties. |
| `[w] place_light` | `type*(point/spot/rect/directional) location* rotation=[0,0,0] intensity=5000 color=[1,1,1] attenuation_radius=1000 cast_shadows=true temperature=6500 use_temperature=false source_radius=0 source_width=64 source_height=64 inner_cone_angle=25 outer_cone_angle=44 name? folder? mobility=Stationary(Static/Stationary/Movable)` | Spawn a light actor with full property configuration. |
| `[w] set_actor_material` | `actor_name* material* slot=0 slot_name? component_name?` | Assign a material to an actor's mesh component by slot index or slot name. SetMaterial creates override array — setting slot 2 without 0-1 fills them with defaults. |
| `[w] set_light_properties` | `actor_name* intensity? color? attenuation_radius? cast_shadows? temperature? use_temperature? source_radius? source_width? source_height? inner_cone_angle? outer_cone_angle? mobility?` | Modify properties on an existing light actor (intensity, color, shadows, temperature, cone angles, etc.) |
| `[w] swap_material_in_level` | `source_material* target_material* actors? preview=false` | Replace all instances of material X with material Y across actors or entire level. preview=true reports changes without modifying. |

### Lighting (5)

| Action | Signature | Purpose |
|--------|-----------|---------|
| `analyze_light_transitions` | `path_points* sample_interval=200 harsh_ratio=4.0 harsh_distance=200` | Sample light levels along a path and flag harsh bright-to-dark transitions (>4:1 ratio over <200cm). Critical for hospice: harsh transitions cause discomfort for light-sensitive patients. |
| `find_dark_corners` | `volume_name? region_min? region_max? threshold=0.05 resolution=128(16-256)` | Find contiguous dark regions in a volume (or region_min/region_max box). Uses orthographic scene capture + flood-fill. Returns dark zones with area and average luminance. |
| `get_light_coverage` | `volume_name* resolution=128 dark_threshold=0.05 bright_threshold=0.2` | Room-level lighting audit. Orthographic capture for floor coverage percentages (lit/shadow/dark). Light inventory with type, intensity, color, shadow-casting flag. |
| `[w] sample_light_levels` | `points* mode=both(capture/analytic/both)` | Sample light levels at specified points. capture = scene capture w/ Lumen GI; analytic = inverse-square from light actors. Returns luminance, dominant light, color temperature, shadow state per point. Hard cap 50 points. |
| `[w] suggest_light_placement` | `volume_name* mood*(horror_dim/safe_room/clinical/ambient) max_lights=8` | Suggest light placements for a mood. Analytic only: target luminance per mood, inverse-square backward-solve, avoids existing light overlap. |

### Decal (4)

| Action | Signature | Purpose |
|--------|-----------|---------|
| `analyze_prop_density` | `volume_name* grid_size=200 target_density=3 summary_only=false` | Analyze prop/actor density within a volume using a grid. Returns per-cell counts, identifies sparse/dense areas, and scores against a target density. |
| `[w] place_along_path` | `path_points*(min2) asset_or_decal* spacing? pattern?(blood_drips/footprints/drag_marks) size? seed=0 folder=PathDecals` | Place decals or props along a smooth path using Catmull-Rom interpolation. Built-in patterns: blood_drips (30-80cm spacing), footprints (60cm alternating L/R), drag_marks (10-20cm dense). |
| `[w] place_decals` | `material* locations? region? count=10 size=[15,80,80] random_rotation=true min_spacing=60 seed=0 folder=Decals` | Place decal actors aligned to surfaces. Provide explicit locations OR a region+count for Poisson-disk scattered placement. Validates material has DeferredDecal domain. |
| `[w] place_storytelling_scene` | `location* pattern*(violence/abandoned_in_haste/dragged/medical_emergency/corruption) intensity=0.5 direction? seed=0 folder=Storytelling` | Place a parameterized horror storytelling scene. Intensity 0-1 scales density and radius. Returns placed actor names for manual material assignment. |

### Editor Level Metadata (4)

| Action | Signature | Purpose |
|--------|-----------|---------|
| `[w] export_metadata` | `output_dir? include_objects=true include_foliage=true max_actors=10000` | Write deterministic JSON level metadata and optional actor NDJSON under the project Saved directory. |
| `get_level_metadata` | `include_foliage=true max_actors=10000` | Return current editor level summary metadata without writing files. |
| `preview_metadata_export` | `include_foliage=true max_actors=10000` | Traverse the current editor level and return metadata counts, warnings, and estimated output without writing files. |
| `validate_metadata_export` | `manifest_path*` | Validate a Monolith level metadata export manifest and referenced files. |

### Auto Volume (3) — feature-gated

> **Feature-gated:** these 3 actions are absent from the default catalog dump and register only when the auto-volume feature is enabled (depends on the spatial registry). Confirm params via `monolith_discover(... mode: "schema")` once enabled.

| Action | Purpose |
|--------|---------|
| `auto_volumes_for_block` | Auto-spawn volumes for ALL buildings in a block, plus a block-level NavMeshBoundsVolume. Iterates buildings in the spatial registry block and calls auto_volumes_for_building for each. |
| `auto_volumes_for_building` | Auto-spawn NavMesh, Audio, Trigger volumes and NavLinkProxies for a building in the spatial registry. Reads room/door/floor data from SP6, delegates to spawn_volume for each volume type. |
| `spawn_nav_link` | Spawn a NavLinkProxy between two world points for AI navigation across disconnected navmesh regions (doors, stairwells, gaps). |

### Editor Map (3)

| Action | Signature | Purpose |
|--------|-----------|---------|
| `get_world_context` | `(no params)` | Read the active editor world, persistent level, partitioning, actor count, and classic streaming capability. |
| `list_layers` | `include_actors=false actor_limit=20` | List actor layer names in the active editor world, with optional capped actor membership rows. |
| `list_streaming_levels` | `limit=100` | List classic streaming levels for the active editor world and report World Partition capability context. |

## Related skills (former mega-`mesh` namespace was split)

- Mesh **assets** (inspect/edit/GeometryScript/tech art): `unreal-mesh`
- Procedural **blockout & town** generation: `unreal-worldgen`
- Horror / accessibility / audio level **analysis**: `unreal-leveldesign`
- Persistent AI **navmesh/navlink behavior**, BT/EQS: `unreal-ai`
- **Level Instances / Packed Level Actors** (pack/commit/break): `unreal-level-instance`

## Key parameters

- `actor_name` — placed level actor | `class_or_mesh` — class path or `/Game/...` mesh to spawn
- `location` / `rotation` / `scale` — transforms (use `relative: true` for offsets)
- `volume_name` — named volume actor | `start` / `end` — world-space points for trace queries

## Typical workflows

- **Scene overview:** `get_scene_statistics` → `query_radial_sweep` → `get_spatial_relationships`
- **Place & arrange:** `spawn_actor` → `move_actor` / `align_actors` → `group_actors` → `select_actors`
- **Exact destructive edit:** resolve current-map actor object paths → `delete_actors` → require `exact_deletion_verified=true` and `survivor_count=0` before saving
- **Visibility / cover:** `query_line_of_sight` → `query_raycast` / `query_overlap` / `query_nearest`
- **Lighting audit:** `get_light_coverage` → `find_dark_corners` → `analyze_light_transitions` → `suggest_light_placement`
- **Spatial registry (town):** `register_building` / `register_room` → `query_room_at` / `path_between_rooms` → `auto_volumes_for_building`

## Gotchas

- `query_*` run live physics traces; `get_*` read stored data. All spatial queries work in-editor **without** a PIE session.
- For destructive batches, pass exact actor object paths to `delete_actors`. Every actor must belong to the active `World->GetCurrentLevel()` map package; streamed/sublevel actors must be handled only after making that level current.
- `delete_actors` rejects duplicate aliases that resolve to one actor and runs both per-actor `CanDeleteActor` and whole-set `ShouldAbortActorDeletion` checks before mutation. Success requires exact identity and path readback, not just UE's `DeleteActors` Boolean.
- On `delete_actors` partial failure, inspect `actor_results`, `survivors`, `requires_manual_recovery`, and `undo_available`; completed deletions remain applied and are explicitly Undo-able only when `undo_available=true`. `rollback_performed=false` is intentional.
- `batch_execute` caps at 200 and forbids nesting. It stops at first failure and closes the transaction for Undo; earlier successful actions and a partially-mutated failing action are retained, not automatically rolled back. Inspect nested `results[].error_data`.
- `spawn_actor` cannot create `ABlockingVolume` — use `spawn_volume`. Set mobility `Movable` before enabling physics in `set_actor_properties`.
- `query_radial_sweep`: `ray_count * vertical_angles <= 512`.
- `register_building` / `register_room` must run before spatial-registry queries return results.
- Editor actor edits are not autosaved: after spawn/move/delete/property changes, save the open level explicitly (the scene namespace has no save action — use the editor/asset save path or save in-editor before relying on persistence).
- `move_actor` mutates only after `MarkPackageDirty()` succeeds; it never auto-saves. Persist the owning map or external-actor package with `editor.save_packages`. Inside `batch_execute`, the move joins the outer Undo transaction instead of opening its own.
- Actor object paths are `/Game/Maps/Map.Map:PersistentLevel.ActorName`. Most singular `actor_name` params take an outliner label/internal name; `delete_actors.actor_names` additionally accepts and prefers exact object paths for destructive work.

## Notes

- This reference is generated from the live `RegisterAction` surface. If an action is missing or renamed, re-run `monolith_discover({ namespace: "scene" })` — the catalog is the source of truth.
- Pass `mode: "schema"` to `monolith_discover` for required/optional params and types before calling an action.
