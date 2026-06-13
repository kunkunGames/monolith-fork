---
name: unreal-mesh
description: Use for static/skeletal mesh ASSET inspection and editing via Monolith MCP (mesh namespace) - mesh info (tris/verts/bounds/materials/LODs/collision/UVs), quality analysis, GeometryScript handle ops, procedural geometry, tech-art import/LOD/texel-density, performance budgeting, validation, and actor/mesh merging. For placing/moving mesh ACTORS in the level and spatial queries use unreal-scene; for blockout and town generation use unreal-worldgen; to import via glTF/FBX/USD first use unreal-interchange; for cloth sim on a skeletal mesh use unreal-cloth; to fracture into a Geometry Collection use unreal-chaos-fracture; for a geometry/Chaos node GRAPH use unreal-dataflow. Triggers on mesh, StaticMesh, SkeletalMesh, tri count, vertex, bounds, LOD, collision, UV, texel density, GeometryScript, handle, boolean, simplify, remesh, parametric mesh, import mesh, mesh quality, proxy mesh, merge meshes.
---

# unreal-mesh

**70 actions** via `mesh_query(action, params)`. Action names below are the live registry surface; call `monolith_discover` for exact parameter schemas.

## Discovery

```
monolith_discover({ namespace: "mesh" })                      # all actions in this namespace
monolith_discover({ namespace: "mesh", action: "<action>", mode: "schema" })  # exact params
```

## When to use / Use a different skill for

This skill owns the mesh **ASSET** (the StaticMesh/SkeletalMesh on disk and its GeometryScript editable handle). Route elsewhere when:

- Placing/moving/duplicating mesh **actors** in the live level, or spatial queries (raycast/overlap/nearest) → `unreal-scene`
- Procedural blockout, town/building generation, facades, streets, furnishing → `unreal-worldgen`
- Importing the mesh through the glTF/FBX/USD pipeline before inspecting/editing it here → `unreal-interchange`
- Cloth simulation set up on the skeletal mesh (this skill edits the underlying geometry) → `unreal-cloth`
- Fracturing a mesh into a **Geometry Collection** for destruction → `unreal-chaos-fracture`
- Authoring a geometry/Chaos **node GRAPH** (versus direct GeometryScript handle ops here) → `unreal-dataflow`

## Action Reference

**Param notation:** `name*` required, `name?` optional, `name=val` default, `a/b/c` allowed values, `[w]` mutates. The tables below list each action's purpose; full call signatures (every param with required/default/allowed values) live in [references/actions.md](references/actions.md). Signatures there are a snapshot of the live catalog — for the exact full schema call `monolith_discover({ namespace: "mesh", action: "<action>", mode: "schema" })`. The discover-first block above stays the authority.

### Operation (17)

| Action | Purpose |
|--------|---------|
| `compute_uvs` | Compute UVs using various projection methods |
| `create_handle` | Create a mesh handle from a StaticMesh asset or primitive (box/sphere/cylinder/cone/torus/plane) |
| `fill_holes` | Automatically detect and fill holes in a mesh |
| `generate_collision` | Generate collision shapes for a mesh handle |
| `generate_lods` | Generate LOD chain by repeated simplification |
| `geometry_material_ids` | Inspect or modify per-triangle material IDs on a mesh handle |
| `geometry_plane_cut` | Apply a direct GeometryScript plane cut/slice/mirror to a mesh handle |
| `geometry_recompute_normals` | Recompute or reset normals on a mesh handle |
| `geometry_smooth` | Apply iterative smoothing to a mesh handle |
| `geometry_subdivide` | Uniform or PN tessellate a mesh handle |
| `list_handles` | List all active mesh handles with source, triangle count, and idle time |
| `mesh_boolean` | Boolean operation (union/subtract/intersect) between two mesh handles |
| `mesh_remesh` | Isotropic remeshing to a target edge length |
| `mesh_simplify` | Simplify a mesh to a target triangle count or percentage |
| `mirror_mesh` | Mirror a mesh across an axis |
| `release_handle` | Release a mesh handle, freeing memory |
| `save_handle` | Save a mesh handle as a new StaticMesh asset with auto-generated collision |

### Inspection (12)

| Action | Purpose |
|--------|---------|
| `analyze_mesh_quality` | Deep quality analysis for a StaticMesh (non-manifold edges, degenerate tris, loose verts, UV overlap) |
| `analyze_skeletal_mesh` | Quality analysis for a SkeletalMesh (bone weights, degenerate tris, UV quality, LOD delta) |
| `compare_meshes` | Compare two meshes side-by-side (triangle, vertex, bounds, material, LOD deltas) |
| `get_mesh_bounds` | Get detailed bounding box info for a mesh (AABB, volume, surface area, sphere radius) |
| `get_mesh_catalog_stats` | Get aggregate statistics from the mesh catalog (total count, category breakdown, size distribution) |
| `get_mesh_collision` | Get collision setup details (simple shapes, complex, trace flag) |
| `get_mesh_info` | Get comprehensive info for a StaticMesh or SkeletalMesh (tri count, bounds, materials, LODs, collision, Nanite) |
| `get_mesh_lods` | Get LOD details (tri/vert counts, section count, screen size per LOD) |
| `get_mesh_materials` | Get material slot info with per-section triangle counts |
| `get_mesh_uvs` | Get UV channel info with island count and overlap detection |
| `get_vertex_data` | Get raw vertex positions and normals (paginated, max 5000 per call) |
| `search_meshes_by_size` | Search the mesh catalog for meshes within a size range (requires indexer to have run) |

### Procedural (12)

| Action | Purpose |
|--------|---------|
| `clear_cache` | Clear the procedural mesh cache. Removes manifest entries (assets remain on disk). Optionally filter by type. |
| `create_building_shell` | Generate a multi-story building shell from a 2D footprint polygon. Extrudes walls per floor, adds floor/ceiling slabs. |
| `create_fragments` | Fragment a mesh via iterative plane slicing. Each cut adds a random plane through the mesh interior. Produces N fragments as separate handles. Seed for reproducibility. Uses ApplyMeshPlaneSlice + SplitMeshByComponents. |
| `create_horror_prop` | Generate horror-specific procedural props: barricade, debris_pile, cage, coffin, gurney, broken_wall, vent_grate |
| `create_maze` | Generate a grid-based maze with 3 algorithms: recursive_backtracker, prims, binary_tree. Returns maze layout JSON for AI pathfinding. Seed for reproducibility. |
| `create_parametric_mesh` | Generate blockout-quality parametric furniture/props from boolean operations on primitives. Types: chair, table, desk, shelf, cabinet, bed, door_frame, window_frame, stairs, ramp, pillar, counter, toilet, sink, bathtub |
| `create_pipe_network` | Sweep a circular cross-section along 3D path points to create pipes/ducts. Configurable radius, segments, elbow handling via MiterLimit. Supports ball joints at junctions. |
| `create_structure` | Generate room/corridor/junction structures with walls, floor, ceiling, and door/window openings via boolean subtract. Types: room, corridor, L_corridor, T_junction, stairwell |
| `create_terrain_patch` | Generate a terrain patch as a subdivided grid with Perlin noise heightmap displacement. Multi-octave noise via repeated ApplyPerlinNoiseToMesh2 passes. |
| `get_cache_stats` | Get cache statistics: entry counts by type, total entries. |
| `list_cached_meshes` | List all procedural meshes in the cache manifest with metadata (type, path, triangle count, age). |
| `validate_cache` | Validate the cache manifest against disk. Removes entries whose assets no longer exist. |

### Tech Art (8)

| Action | Purpose |
|--------|---------|
| `analyze_lightmap_density` | Lightmap texel density analysis and resolution recommendations for actors in the scene. |
| `analyze_material_cost_in_region` | Cross-module: spatial query + shader instruction count per material. Identifies shader cost hotspots in a region. |
| `analyze_texel_density` | Calculate texels/cm for meshes. Reports UV space usage vs world-space area combined with texture resolution. Supports single actor or region mode. |
| `auto_generate_lods` | One-shot LOD generation: simplify via GeometryScript and write back to UStaticMesh source models with screen sizes. |
| `export_mesh` | Export a UStaticMesh or USkeletalMesh asset to FBX on disk via UAssetExportTask + the engine's built-in FBX exporter. |
| `fix_mesh_quality` | Auto-fix mesh quality issues: weld edges, remove degenerates, remove small components, fix normals. GeometryScript required. |
| `import_mesh` | Import FBX/glTF mesh files via automated import. Configure static vs skeletal, collision, material import, scale. |
| `set_mesh_collision` | Set collision on a static mesh asset. Supports simple shapes, complex-as-simple, and auto-convex decomposition. |

### Actor Merge (5)

| Action | Purpose |
|--------|---------|
| `bake_actor_materials` | Preview material bake inputs for actors; dedicated bake output generation is unavailable until bake tests exist. |
| `create_proxy_mesh_from_actors` | Preview or invoke mesh.generate_proxy_mesh from explicit actor_names. |
| `merge_actors` | Preview or create a side-by-side proxy mesh from actors. Requires confirm=true and keep_sources policy. |
| `merge_actors_to_instances` | Preview actor candidates for future instancing conversion; does not mutate scene structure yet. |
| `preview_actor_merge` | Resolve actors and estimate bounds, materials, and LOD0 triangles before actor merge/proxy operations. |

### Performance (5)

| Action | Purpose |
|--------|---------|
| `analyze_shadow_cost` | Audit shadow-casting actors and lights in a region. Flags small props casting shadows unnecessarily and lights with high shadow resolution on non-hero objects. |
| `estimate_placement_cost` | Pre-placement budgeting: estimate triangle and draw call cost for a set of meshes without spawning. Loads mesh assets to read render data. |
| `find_overdraw_hotspots` | Detect overdraw hotspots from translucent/additive material actors. Projects bounds to screen-space AABBs and counts overlap in a tile grid. |
| `get_region_performance` | Analyze performance metrics for a world region: triangle count, draw call estimate, light count, shadow caster count. Conservative estimates (no occlusion culling). |
| `get_triangle_budget` | LOD-aware triangle budget check from a viewpoint. Builds a view frustum, tests actor visibility, selects LOD by screen size. Returns count vs budget. Conservative (no occlusion culling). |

### Level Design Editing (4)

| Action | Purpose |
|--------|---------|
| `convert_to_hism` | Convert grouped StaticMeshActors into a single HISM actor. Deletes originals. Single undo transaction. |
| `find_instancing_candidates` | Identify meshes used many times that could benefit from HISM conversion. Groups by mesh and material set. |
| `find_replace_mesh` | Swap all instances of static mesh X with mesh Y. Essential for blockout-to-art pass. |
| `set_lod_screen_sizes` | Set per-LOD screen size thresholds on a static mesh asset. Sizes must be monotonically decreasing. |

### Validation (4)

| Action | Purpose |
|--------|---------|
| `batch_validate` | Batch validate meshes: fast SQL pre-filter from mesh_catalog, then deep asset-load on flagged assets. Sorted by severity. |
| `compare_lod_chain` | Compare LOD chain quality: per-step reduction ratio, screen size gaps, section mismatches. Flags unhealthy transitions. |
| `suggest_lod_strategy` | Suggest LOD strategy based on triangle count. Returns ready-to-execute params for generate_lods. |
| `validate_game_ready` | Run a game-readiness checklist on a StaticMesh: collision, LODs, lightmap UV, degenerate geo, material count, pivot, scale. Returns pass/fail per check with severity. |

### Quality (3)

| Action | Purpose |
|--------|---------|
| `analyze_texture_budget` | Analyze texture memory usage: pool size, used, top textures, by-format breakdown. Identifies budget hogs and gives recommendations. |
| `generate_proxy_mesh` | Merge selected static mesh actors into a single simplified proxy mesh. Uses IMeshMergeUtilities for LOD-aware merging with optional material merging. |
| `setup_hlod` | Create or configure a UHLODLayer asset with type and settings. |

## Related skills (the old mega-`mesh` namespace was split)

The former 268-action `mesh` namespace was refactored into focused namespaces. Use the right one:

| Need | Namespace | Skill |
|------|-----------|-------|
| Mesh **asset** inspect/edit, GeometryScript, procedural geo, tech art | `mesh` | this skill |
| **Scene/level** actors, spatial queries, volumes, lighting, decals, debug views | `scene` | `unreal-scene` |
| **Horror/accessibility/audio** level analysis & encounter design | `leveldesign` | `unreal-leveldesign` |
| **Blockout & procedural town** generation (buildings, facades, streets, furnishing) | `worldgen` | `unreal-worldgen` |

Adjacent mesh-related skills outside the split:

- **Import** the mesh via the glTF/FBX/USD pipeline first → `unreal-interchange`
- **Cloth** simulation on a skeletal mesh (this skill edits the underlying geometry) → `unreal-cloth`
- **Fracture** a mesh into a Geometry Collection for destruction → `unreal-chaos-fracture`
- Geometry/Chaos **node GRAPH** authoring (versus direct GeometryScript handle ops here) → `unreal-dataflow`
- **HLOD layer** creation/build/clear orchestration around the proxy/merged mesh (this skill still owns the `setup_hlod` / `generate_proxy_mesh` actions themselves) → `unreal-hlod`
- **Cross-domain perf profiling** (material shader stats, Niagara complexity, CVar perf tuning) that a per-mesh budget pass feeds into → `unreal-performance`

## Key parameters

- `asset_path` — mesh asset (`/Game/...`, no `.uasset`) | `handle` — GeometryScript editable-mesh handle
- `region_min`/`region_max` — world-space AABB for performance/tech-art region modes

## Typical workflows

- **Inspect a mesh:** `get_mesh_info` → `analyze_mesh_quality` → `get_mesh_lods` / `compare_meshes`
- **Edit a mesh (GeometryScript):** `create_handle` → [`mesh_boolean` | `mesh_simplify` | `mesh_remesh` | `fill_holes` | `compute_uvs`] → `save_handle` → `release_handle`
- **Game-ready pass:** `validate_game_ready` → `fix_mesh_quality` → `auto_generate_lods` → `set_mesh_collision` → `compare_lod_chain`
- **Optimize a region:** `get_region_performance` → `get_triangle_budget` → `find_instancing_candidates` → `convert_to_hism` / `generate_proxy_mesh`

## Gotchas

- `create_handle` opens a server-side editable mesh; always `release_handle` (or rely on idle eviction). `list_handles` shows leaks.
- `save_handle` writes a new `StaticMesh` asset (auto-collision); it does NOT place a level actor — use `scene` to spawn it.
- GeometryScript ops require the engine built `#if WITH_GEOMETRYSCRIPT`; they error out if the plugin is disabled.
- `search_meshes_by_size` / `get_mesh_catalog_stats` need the mesh catalog indexed first (`monolith_reindex()`).
- `merge_actors` / `create_proxy_mesh_from_actors` require `confirm=true` and a `keep_sources` policy; previews are read-only.
- `set_lod_screen_sizes` screen sizes must be **monotonically decreasing**.

## Notes

- This reference is generated from the live `RegisterAction` surface. If an action is missing or renamed, re-run `monolith_discover({ namespace: "mesh" })` — the catalog is the source of truth.
- Pass `mode: "schema"` to `monolith_discover` for required/optional params and types before calling an action.
