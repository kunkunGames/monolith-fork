---
name: unreal-scene
description: "Use when querying or manipulating the live editor scene/level via Monolith MCP: actors (spawn/move/duplicate/delete/align/group), spatial queries (raycast/overlap/nearest/line-of-sight/navmesh/radial sweep), spatial relationships and registry, volumes, lighting analysis, decals & path props, auto-volumes, debug section/floor-plan views, and level/map metadata. Triggers on actor, spawn, move actor, duplicate, raycast, overlap, nearest, line of sight, navmesh path, radial sweep, scene bounds, scene statistics, spatial, volume, trigger volume, light coverage, dark corners, decal, blood trail, section view, floor plan, camera bookmark, sublevel, map, navlink."
---

# unreal-scene

**76 actions** via `scene_query(action, params)`. Action names below are the live registry surface; call `monolith_discover` for exact parameter schemas.

## Discovery

```
monolith_discover({ namespace: "scene" })                      # all actions in this namespace
monolith_discover({ namespace: "scene", action: "<action>", mode: "schema" })  # exact params
```

## Action Reference

### Scene (11)

| Action | Purpose |
|--------|---------|
| `align_actors` | Align multiple actors along an axis. Modes: min/max (snap to extremes), center (average), distribute (spread evenly between min/max). |
| `batch_execute` | Execute multiple scene actions in a single undo transaction. Max 200 actions. No nested batch_execute allowed. |
| `delete_actors` | Delete one or more actors from the editor world. Validates ALL exist before deleting ANY. Does NOT delete asset files. |
| `duplicate_actor` | Duplicate an actor in the editor world with an optional position offset |
| `get_actor_info` | Get comprehensive info for an actor in the editor world (class, transform, mesh, materials, tags, components, bounds) |
| `group_actors` | Move actors into an outliner folder (creates folder hierarchy automatically) |
| `manage_folders` | Manage World Outliner folders: list, delete (move actors to root), rename, or move folders. |
| `move_actor` | Move/rotate/scale an actor. Set relative=true to offset from current transform. |
| `set_actor_properties` | Set properties on an actor (mobility, physics, collision, shadows, tags, mass) |
| `snap_to_floor` | Snap actors to the floor by tracing downward. Places the bottom of each actor on the hit surface. |
| `spawn_actor` | Spawn an actor in the editor world. Path starting with '/' spawns StaticMeshActor with that mesh; otherwise spawns by class name. |

### Spatial (11)

| Action | Purpose |
|--------|---------|
| `get_actors_in_volume` | Get all actors inside a named BlockingVolume. Checks Monolith.Owner tags and spatial containment. |
| `get_scene_bounds` | Compute the enclosing axis-aligned bounding box for all actors (or filtered by class). |
| `get_scene_statistics` | Get scene statistics: actor counts by class, total triangles, light count, volume count, navmesh status. Optional region filter. |
| `get_spatial_relationships` | Analyze spatial relationships between an actor and its neighbors (on_top_of, inside, adjacent, above, below, etc). Thresholds scale with actor bounds. |
| `query_line_of_sight` | Check line of sight between two points. Returns visibility status and blocking info. |
| `query_multi_raycast` | Fire a multi-hit raycast. Returns all hits sorted by distance. |
| `query_navmesh` | Find a navigation path between two points. Returns path points and total distance. Errors if navmesh not built. |
| `query_nearest` | Find nearest actors using physics broadphase (OverlapMultiByObjectType). Filter by class and/or tag, sorted by distance. |
| `query_overlap` | Perform a shape overlap test (box, sphere, or capsule) at a location. Returns overlapping actors. |
| `query_radial_sweep` | Fire rays in a radial pattern from origin. Returns summary by compass direction. Hard cap: ray_count * vertical_angles <= 512. |
| `query_raycast` | Fire a single raycast in the editor world. Returns hit data including actor, location, normal, distance, and physical material. |

### Spatial Registry (10)

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

### Debug View (7)

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

| Action | Purpose |
|--------|---------|
| `build_navmesh` | Trigger navigation mesh rebuild. Synchronous — blocks the game thread. Can take seconds on large maps. |
| `copy_actor_properties` | Copy UPROPERTY values from a source actor to one or more target actors. Optionally filter to specific properties. |
| `get_actor_properties` | Read arbitrary UPROPERTY values from an actor or its components via FProperty reflection. Returns string-serialized values. |
| `select_actors` | Control editor actor selection. Select, deselect, clear selection, get current selection, or focus camera on actors. |
| `set_collision_preset` | Set the collision profile on an actor's root primitive component (or a named component). |
| `snap_to_surface` | Drop actors onto geometry via directional trace. Unlike snap_to_floor, supports any direction and surface normal alignment. |
| `spawn_volume` | Spawn a volume actor (trigger, blocking, kill, pain, nav_modifier, audio, post_process) with proper brush geometry. |

### Level Design Placement (6)

| Action | Purpose |
|--------|---------|
| `get_level_actors` | Enumerate actors in the editor world with multi-filter AND logic. Returns name, class, location, mesh, sublevel, tags. |
| `manage_sublevel` | Create/load/unload streaming sublevels or move actors between levels. sub_action: create, add, remove, move_actors. |
| `measure_distance` | Measure distance between two actors or world points. Returns euclidean, horizontal, height difference, and optional navmesh path distance. |
| `place_blueprint_actor` | Spawn a Blueprint actor in the world with optional property configuration via reflection. |
| `place_spline` | Spawn an actor with a spline component. Optionally places mesh segments along the spline (pipes, cables, railings). |
| `randomize_transforms` | Apply random offset/rotation/scale variation to actors for an organic feel. Deterministic with seed. |

### Level Design Editing (5)

| Action | Purpose |
|--------|---------|
| `get_actor_component_properties` | Read arbitrary component properties via FProperty reflection. Returns typed values for any UPROPERTY. |
| `place_light` | Spawn a light actor (point/spot/rect/directional) with full property configuration |
| `set_actor_material` | Assign a material to an actor's mesh component by slot index or slot name. SetMaterial creates override array — setting slot 2 without 0-1 fills them with defaults. |
| `set_light_properties` | Modify properties on an existing light actor (intensity, color, shadows, temperature, cone angles, etc.) |
| `swap_material_in_level` | Replace all instances of material X with material Y across actors or entire level |

### Lighting (5)

| Action | Purpose |
|--------|---------|
| `analyze_light_transitions` | Sample light levels along a path and flag harsh bright-to-dark transitions (>4:1 ratio over <200cm). Critical for hospice: harsh transitions cause discomfort for light-sensitive patients. |
| `find_dark_corners` | Find contiguous dark regions in a volume. Uses orthographic scene capture + flood-fill. Returns dark zones with area and average luminance. |
| `get_light_coverage` | Room-level lighting audit. Orthographic capture for floor coverage percentages (lit/shadow/dark). Light inventory with type, intensity, color, shadow-casting flag. |
| `sample_light_levels` | Sample light levels at specified points. Modes: capture (scene capture w/ Lumen GI), analytic (inverse-square from light actors), both. Returns luminance, dominant light, color temperature, shadow state per point. Hard cap 50 points. |
| `suggest_light_placement` | Suggest light placements for a mood. Analytic only: target luminance per mood, inverse-square backward-solve, avoids existing light overlap. Moods: horror_dim, safe_room, clinical, ambient. |

### Decal (4)

| Action | Purpose |
|--------|---------|
| `analyze_prop_density` | Analyze prop/actor density within a volume using a grid. Returns per-cell counts, identifies sparse/dense areas, and scores against a target density. |
| `place_along_path` | Place decals or props along a smooth path using Catmull-Rom interpolation. Built-in patterns: blood_drips (30-80cm spacing), footprints (60cm alternating L/R), drag_marks (10-20cm dense). |
| `place_decals` | Place decal actors aligned to surfaces. Provide explicit locations OR a region+count for Poisson-disk scattered placement. Validates material has DeferredDecal domain. |
| `place_storytelling_scene` | Place a parameterized horror storytelling scene. Patterns: violence (radial blood splatter), abandoned_in_haste (scattered items), dragged (linear trail), medical_emergency (triage scene), corruption (organic growth). Intensity 0-1 scales density and radius. Returns placed actor names for manual material assignment. |

### Editor Level Metadata (4)

| Action | Purpose |
|--------|---------|
| `export_metadata` | Write deterministic JSON level metadata and optional actor NDJSON under the project Saved directory. |
| `get_level_metadata` | Return current editor level summary metadata without writing files. |
| `preview_metadata_export` | Traverse the current editor level and return metadata counts, warnings, and estimated output without writing files. |
| `validate_metadata_export` | Validate a Monolith level metadata export manifest and referenced files. |

### Auto Volume (3)

| Action | Purpose |
|--------|---------|
| `auto_volumes_for_block` | Auto-spawn volumes for ALL buildings in a block, plus a block-level NavMeshBoundsVolume. Iterates buildings in the spatial registry block and calls auto_volumes_for_building for each. |
| `auto_volumes_for_building` | Auto-spawn NavMesh, Audio, Trigger volumes and NavLinkProxies for a building in the spatial registry. Reads room/door/floor data from SP6, delegates to spawn_volume for each volume type. |
| `spawn_nav_link` | Spawn a NavLinkProxy between two world points for AI navigation across disconnected navmesh regions (doors, stairwells, gaps). |

### Editor Map (3)

| Action | Purpose |
|--------|---------|
| `get_world_context` | Read the active editor world, persistent level, partitioning, actor count, and classic streaming capability. |
| `list_layers` | List actor layer names in the active editor world, with optional capped actor membership rows. |
| `list_streaming_levels` | List classic streaming levels for the active editor world and report World Partition capability context. |

## Related skills (former mega-`mesh` namespace was split)

- Mesh **assets** (inspect/edit/GeometryScript/tech art): `unreal-mesh`
- Procedural **blockout & town** generation: `unreal-worldgen`
- Horror / accessibility / audio level **analysis**: `unreal-leveldesign`

## Key parameters

- `actor_name` — placed level actor | `class_or_mesh` — class path or `/Game/...` mesh to spawn
- `location` / `rotation` / `scale` — transforms (use `relative: true` for offsets)
- `volume_name` — named volume actor | `start` / `end` — world-space points for trace queries

## Typical workflows

- **Scene overview:** `get_scene_statistics` → `query_radial_sweep` → `get_spatial_relationships`
- **Place & arrange:** `spawn_actor` → `move_actor` / `align_actors` → `group_actors` → `select_actors`
- **Visibility / cover:** `query_line_of_sight` → `query_raycast` / `query_overlap` / `query_nearest`
- **Lighting audit:** `get_light_coverage` → `find_dark_corners` → `analyze_light_transitions` → `suggest_light_placement`
- **Spatial registry (town):** `register_building` / `register_room` → `query_room_at` / `path_between_rooms` → `auto_volumes_for_building`

## Gotchas

- `query_*` run live physics traces; `get_*` read stored data. All spatial queries work in-editor **without** a PIE session.
- `batch_execute` runs many actions in one undo transaction — caps at 200, no nesting.
- `spawn_actor` cannot create `ABlockingVolume` — use `spawn_volume`. Set mobility `Movable` before enabling physics in `set_actor_properties`.
- `query_radial_sweep`: `ray_count * vertical_angles <= 512`.
- `register_building` / `register_room` must run before spatial-registry queries return results.

## Notes

- This reference is generated from the live `RegisterAction` surface. If an action is missing or renamed, re-run `monolith_discover({ namespace: "scene" })` — the catalog is the source of truth.
- Pass `mode: "schema"` to `monolith_discover` for required/optional params and types before calling an action.
