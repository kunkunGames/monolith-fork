# unreal-mesh — action parameter reference

Full call signatures for the `mesh` namespace, enriched from the live catalog dump.

**Param notation:** `name*` required, `name?` optional, `name=val` default, `a/b/c` allowed values, `[w]` mutates (transaction-wrapped). Signatures are a snapshot of the live catalog — for the exact full schema call `monolith_discover({ namespace: "mesh", action: "<action>", mode: "schema" })`. The live catalog is always authoritative; this file is a convenience snapshot.

`asset_path` style params take a `/Game/...` path (no `.uasset`). `handle` params are GeometryScript editable-mesh handle names. Array params like `region_min`/`viewpoint` are `[x, y, z]`; `screen_sizes` is a float array.

## Inspection (12)

| Action | Signature |
|--------|-----------|
| `get_mesh_info` | `asset_path*` |
| `get_mesh_bounds` | `asset_path*` |
| `get_mesh_materials` | `asset_path*` |
| `get_mesh_lods` | `asset_path*` |
| `get_mesh_collision` | `asset_path*` |
| `get_mesh_uvs` | `asset_path*`; `lod_index?=0`; `uv_channel?=-1` (-1=all) |
| `analyze_mesh_quality` | `asset_path*` (StaticMesh) |
| `analyze_skeletal_mesh` | `asset_path*` (SkeletalMesh) |
| `compare_meshes` | `asset_path_a*`; `asset_path_b*` |
| `get_vertex_data` | `asset_path*`; `lod_index?=0`; `offset?=0`; `limit?=1000` (hard max 5000) |
| `search_meshes_by_size` | `min_bounds*` [x,y,z] cm; `max_bounds*` [x,y,z] cm; `category?`; `limit?=20` (hard max 1000); `exclude_size_class?` |
| `get_mesh_catalog_stats` | (no params) |

## Operation — GeometryScript handles (17)

| Action | Signature |
|--------|-----------|
| `[w] create_handle` | `source*` (asset path or `primitive:box`/`sphere`/`cylinder`/`cone`/`torus`/`plane`); `handle*` (name) |
| `[w] release_handle` | `handle*` |
| `list_handles` | (no params) |
| `[w] save_handle` | `handle*`; `target_path*`; `overwrite?=false`; `collision?=auto` (auto/box/convex/complex_as_simple/none); `max_hulls?=4` |
| `[w] mesh_boolean` | `handle_a*` (target); `handle_b*` (tool); `operation*` (union/subtract/intersect); `result_handle*` |
| `[w] mesh_simplify` | `handle*`; `target_triangles?`; `target_percentage?` (0.0-1.0); `max_deviation?` |
| `[w] mesh_remesh` | `handle*`; `target_edge_length*` cm |
| `[w] generate_collision` | `handle*`; `method?=convex_decomp` (convex_decomp/auto_box/auto_sphere/auto_capsule/simplified); `max_hulls?=4` |
| `[w] generate_lods` | `handle*` (LOD0 source); `lod_count*` (excl. LOD0); `reduction_per_lod?=0.5` (0.0-1.0) |
| `[w] fill_holes` | `handle*` |
| `[w] compute_uvs` | `handle*`; `method?=auto_unwrap` (auto_unwrap/box_project/planar_project/cylinder_project); `uv_channel?=0` |
| `[w] mirror_mesh` | `handle*`; `axis*` (X/Y/Z); `weld?=true` |
| `[w] geometry_plane_cut` | `handle*`; `result_handle?` (omit=in place); `mode?=cut` (cut/slice/mirror); `origin?=[0,0,0]`; `normal?=[0,0,1]`; `fill_holes?=true`; `weld?=true` |
| `[w] geometry_recompute_normals` | `handle*`; `result_handle?`; `mode?=recompute` (recompute/split/per_vertex/per_face); `split_angle?=15` deg |
| `[w] geometry_subdivide` | `handle*`; `result_handle?`; `method?=uniform` (uniform/pn); `level?=1` (clamped 1-5) |
| `[w] geometry_smooth` | `handle*`; `result_handle?`; `iterations?=10` (clamped 1-200); `speed?=0.25` (0.0-1.0) |
| `[w] geometry_material_ids` | `handle*`; `verb*` (info/remap/clear/delete_by_id); `result_handle?`; `from_id?=0`; `to_id?=0`; `material_id?=0`; `clear_value?=0` |

## Procedural (12)

Generators share placement/save params: `handle?`, `save_path?`, `overwrite?=false`, `place_in_scene?=false`, `location?` [x,y,z], `rotation?` [pitch,yaw,roll], `label?`, `use_cache?=true`, `auto_save?=true` (writes /Game/Generated/ when no `save_path`). Listed below as `<shared>`.

| Action | Signature |
|--------|-----------|
| `[w] create_parametric_mesh` | `type*` (chair/table/desk/shelf/cabinet/bed/door_frame/window_frame/stairs/ramp/pillar/counter/toilet/sink/bathtub); `dimensions?` {width,depth,height} cm; `params?` (type-specific: seat_height/back_height/leg_thickness/stair_count/shelf_count/bowl_radius/etc.); `<shared>` |
| `[w] create_horror_prop` | `type*` (barricade/debris_pile/cage/coffin/gurney/broken_wall/vent_grate); `dimensions?` {width,depth,height} cm; `params?` (board_count/bar_count/noise_scale/hole_radius/slot_count/gap_ratio/etc.); `seed?=0`; `<shared>` |
| `[w] create_structure` | `type*` (room/corridor/L_corridor/T_junction/stairwell); `dimensions?` {width,depth,height} cm; `wall_thickness?=20`; `floor_thickness?=3`; `has_ceiling?=true`; `has_floor?=true`; `openings?` (array of {wall:north/south/east/west, type:door/window/vent, width, height, offset_x, offset_z}); `add_trim?=true`; `wall_mode?=sweep` (sweep/box); `<shared>` |
| `[w] create_building_shell` | `footprint*` (array of [x,y] CCW, cm); `floors?=1`; `floor_height?=300`; `wall_thickness?=25`; `floor_thickness?=15`; `stairwell_cutout?` {x,y,width,depth}; `<shared>` |
| `[w] create_maze` | `algorithm?=recursive_backtracker` (recursive_backtracker/prims/binary_tree); `grid_size?=[8,8]` [cols,rows]; `cell_size?=200` cm; `wall_height?=250`; `wall_thickness?=20`; `seed?=0`; `merge_walls?=false`; `<shared>` |
| `[w] create_pipe_network` | `path_points*` (array of [x,y,z], min 2); `radius?=10`; `segments?=12`; `miter_limit?=2.0`; `ball_joints?=false`; `joint_radius_scale?=1.3`; `<shared>` |
| `[w] create_fragments` | `source_handle*` (not modified); `fragment_count?=8`; `noise?=0`; `seed?=0`; `gap_width?=0.5`; `handle_prefix?=frag` |
| `[w] create_terrain_patch` | `size?=[2000,2000]` [x,y] cm; `resolution?=32`; `amplitude?=100` cm; `frequency?=0.01`; `octaves?=4`; `persistence?=0.5`; `lacunarity?=2.0`; `seed?=0`; `<shared>` |
| `list_cached_meshes` | `type?` (e.g. chair/barricade/room); `limit?=100` |
| `[w] clear_cache` | `type?` (omit=clear all) |
| `validate_cache` | (no params) |
| `get_cache_stats` | (no params) |

## Tech Art (8)

| Action | Signature |
|--------|-----------|
| `[w] import_mesh` | `files*` (abs paths); `destination*` (/Game/...); `replace_existing?=false`; `combine_meshes?=true`; `generate_lightmap_uvs?=true`; `auto_generate_collision?=true`; `normal_import_method?=ImportNormalsAndTangents` (ImportNormals/ImportNormalsAndTangents/ComputeNormals); `material_import?=create_new` (create_new/find_existing/skip); `import_as_skeletal?=false`; `import_animations?=false` (forces skeletal) |
| `[w] export_mesh` | `asset_path*` (StaticMesh or SkeletalMesh); `file_path*` (abs, .fbx); `replace_existing?=true` |
| `[w] fix_mesh_quality` | `asset_path*`; `operations?` (remove_degenerates/weld_edges/remove_small_components/fix_normals; default all); `weld_tolerance?=0.01` cm |
| `[w] auto_generate_lods` | `asset_path*`; `lod_count?=3` (excl. LOD0); `reduction_per_lod?=0.5` (0.0-1.0); `screen_sizes?` [LOD0,LOD1,...]; `preserve_uv_borders?=true` |
| `analyze_texel_density` | `actor_name?`; `region_min?` [x,y,z]; `region_max?`; `target_density?=5.12` texels/cm; `uv_channel?=0` (single-actor OR region mode) |
| `analyze_lightmap_density` | `actor_name?`; `region_min?`; `region_max?`; `target_density?=4.0` texels/cm |
| `analyze_material_cost_in_region` | `center?` [x,y,z]; `radius?` cm; `region_min?`; `region_max?`; `actors?` (use center+radius OR region OR actors) |
| `[w] set_mesh_collision` | `asset_path*`; `collision_type*` (simple_box/simple_sphere/simple_capsule/complex_as_simple/use_complex/use_default); `auto_convex?` {hull_count,max_verts} |

## Performance (5)

| Action | Signature |
|--------|-----------|
| `get_region_performance` | `region_min*` [x,y,z]; `region_max*` [x,y,z] |
| `estimate_placement_cost` | `assets*` (array of {asset_path, count}) |
| `find_overdraw_hotspots` | `viewpoint*` [x,y,z]; `view_direction?` [x,y,z] (default +X); `fov?=90` deg |
| `analyze_shadow_cost` | `region_min*` [x,y,z]; `region_max*` [x,y,z] |
| `get_triangle_budget` | `viewpoint*` [x,y,z]; `view_direction?` (default +X); `fov?=90` deg; `budget?=500000` tris |

## Actor Merge (5)

| Action | Signature |
|--------|-----------|
| `preview_actor_merge` | `actor_names*`; `save_path?` (candidate output) |
| `[w] merge_actors` | `actor_names*`; `save_path*`; `confirm?=false` (required true to generate); `source_policy?=keep_sources` (keep_sources/side_by_side); `merge_materials?=true`; `texture_size?=1024`; `screen_size?=300` |
| `[w] create_proxy_mesh_from_actors` | `actor_names*`; `save_path*`; `confirm?=false` (required true); `merge_materials?=true`; `texture_size?=1024`; `screen_size?=300` |
| `[w] merge_actors_to_instances` | `actor_names*` (preview only; does not mutate scene yet) |
| `[w] bake_actor_materials` | `actor_names*`; `texture_size?=1024` (preview only; bake output unavailable) |

## Level Design Editing (4)

| Action | Signature |
|--------|-----------|
| `find_replace_mesh` | `source_mesh*`; `target_mesh*`; `actors?` (default all); `match_mode?=exact` (exact/contains); `preview?=false` |
| `find_instancing_candidates` | `min_count?=5`; `region_min?` [x,y,z]; `region_max?`; `include_materials?=true` |
| `[w] convert_to_hism` | `mesh*` (asset path); `actors*`; `name?`; `folder?`; `preserve_materials?=true` |
| `[w] set_lod_screen_sizes` | `asset_path*`; `screen_sizes*` (float array, monotonically decreasing, e.g. [1.0, 0.4, 0.15]) |

## Validation (4)

| Action | Signature |
|--------|-----------|
| `validate_game_ready` | `asset_path*` (StaticMesh) |
| `compare_lod_chain` | `asset_path*` |
| `[w] suggest_lod_strategy` | `asset_path*` |
| `[w] batch_validate` | `class?=StaticMesh`; `path_filter?` (e.g. /Game/Environment/); `severity_min?=HIGH` (CRITICAL/HIGH/MEDIUM/LOW) |

## Quality (3)

| Action | Signature |
|--------|-----------|
| `analyze_texture_budget` | `scan_path?` (content path filter; empty=all); `top_count?=20` |
| `[w] generate_proxy_mesh` | `actor_names*`; `save_path*`; `screen_size?=300` px; `merge_materials?=true`; `texture_size?=1024` |
| `[w] setup_hlod` | `save_path*`; `layer_type?=MeshSimplify` (MeshMerge/MeshSimplify/MeshApproximate/Custom); `cell_size?=25600`; `loading_range?=2.0` |
