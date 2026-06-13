---
name: unreal-worldgen
description: Use when generating procedural blockout and town/building geometry via Monolith MCP (worldgen namespace) — blockout volumes/primitives, asset match/replace, prop scatter and context props, terrain foundations, arch features (balcony/porch/ramp/railing), floor plans, facades, roofs, city blocks/streets, room furnishing, and genre presets. For an individual StaticMesh asset use unreal-mesh; for manual actor spawn/move and raw spatial queries use unreal-scene; for a reusable PCG sampler graph use unreal-pcg; for horror/accessibility/encounter analysis use unreal-leveldesign; to pack content into a level instance use unreal-level-instance; to merge geometry into proxy/HLOD meshes use unreal-hlod. Triggers on worldgen, blockout, scan volume, scatter, prop kit, disturbance, terrain, foundation, balcony, porch, fire escape, ramp, railing, floor plan, archetype, facade, roof, city block, street furniture, generate a building, generate a town, furnish, room template, preset, validate building.
---

# unreal-worldgen

Generate procedural blockout and town/building geometry, drives the Monolith `worldgen` namespace. **36 always-on actions + ~27 gated** via `worldgen_query(action, params)`. The always-on set (Blockout, Context Prop, Preset, Template) is the live registry surface; the ~27 town-pipeline actions (Terrain, City Block, Facade, Floor Plan, Furnishing, Building, Roof, Validation, Arch Feature) are gated behind `bEnableProceduralTownGen` (off by default). Action names below are the live registry surface; call `monolith_discover` for exact parameter schemas.

**Param notation:** `name*` required, `name?` optional, `name=val` default, `a/b/c` allowed values, `[w]` mutates (transaction-wrapped). Signatures are a snapshot of the live catalog — for the exact full schema call `monolith_discover` with `mode: "schema"`. Gated actions (marked **[gated]**) are absent from the live snapshot; confirm their params via `monolith_discover` once `bEnableProceduralTownGen` is enabled.

## Discovery

```
monolith_discover({ namespace: "worldgen" })                      # all actions in this namespace
monolith_discover({ namespace: "worldgen", action: "<action>", mode: "schema" })  # exact params
```

## When to use / Use a different skill for

- **Procedural blockout, scatter, floor plans, facades, roofs, terrain foundations, city blocks, furnishing, presets** → this skill (the town pipeline spans `worldgen` + `scene`).
- Inspect or edit an **individual StaticMesh asset** the generation produces or replaces (tris/LOD/collision/UVs/GeometryScript/parametric) → **unreal-mesh**, versus the blockout volumes and town/building generation here.
- **Manual actor spawn/move/align and raw spatial queries** (raycast/overlap/nearest/navmesh) → **unreal-scene**; this skill also relies on `scene.register_building` / `register_room` / `auto_volumes_*` for the spatial registry.
- A **reusable PCG graph** (samplers, points, density filters) → **unreal-pcg**, versus the fixed blockout/town/building generation actions here.
- **Horror / accessibility / encounter analysis** that reads generated geometry and returns heuristic scores → **unreal-leveldesign**, versus generating the geometry.
- Pack generated **building/room content into a reusable level instance** or packed level actor → **unreal-level-instance**, versus generating it.
- Merge generated geometry into **proxy/HLOD meshes** for performance → **unreal-hlod**, versus the generation pass.

## Action Reference

All actions in Blockout, Context Prop, Preset, and Template are always-on (present in the live snapshot). Actions tagged **[gated]** below `Template` require `bEnableProceduralTownGen = true`.

### Blockout (16)

| Action | Params | Purpose |
|--------|--------|---------|
| `get_blockout_volumes` | (none) | Find all actors with Monolith.Blockout tag. Warns about misconfigured actors with partial Monolith tags. |
| `get_blockout_volume_info` | `volume_name*` | Get detailed info for a blockout volume: parsed tags, blockout primitives, and other actors. |
| `[w] setup_blockout_volume` | `volume_name*` `room_type*` `tags?` `density=Normal` (Sparse/Normal/Dense/Cluttered) `allow_physics=false` `floor_height=0` | Apply Monolith blockout tags to a BlockingVolume. Clears existing Monolith tags first. |
| `[w] create_blockout_primitive` | `shape*` (box/cylinder/sphere/cone/wedge) `location*` ([x,y,z]) `scale*` ([x,y,z]) `rotation=[0,0,0]` `label?` `volume_name?` `category=default` (furniture/prop/hazard) | Spawn a scaled BasicShape as a blockout primitive with category-colored material and owner tags. |
| `[w] create_blockout_primitives_batch` | `primitives*` (array of {shape,location,rotation,scale,label,category}, max 200) `volume_name?` | Create multiple blockout primitives in a single undo transaction. Max 200 primitives. Warns if outside volume bounds. |
| `[w] create_blockout_grid` | `volume_name*` `cell_size*` (cm) `wall_thickness=10` (cm) | Create a floor grid of box primitives filling a blockout volume. |
| `[w] match_asset_to_blockout` | `blockout_actor*` `category?` `tolerance_pct=20` `top_n=3` | Find mesh catalog assets that match a blockout primitive's size. Axis-sorted matching with weighted scoring. |
| `[w] match_all_in_volume` | `volume_name*` `tolerance_pct=20` `top_n=3` | Batch match all blockout primitives in a volume against mesh catalog. Returns replacement plan. |
| `[w] apply_replacement` | `replacements*` (array of {blockout_actor,replacement_asset}) `volume_name?` | ATOMIC: Replace blockout primitives with real mesh assets. Validates ALL assets first, single undo, pivot adjustment in local space. |
| `[w] set_actor_tags` | `actor_tags*` (array of {actor,tags}) | Batch apply tags to multiple actors in a single undo transaction. |
| `[w] clear_blockout` | `volume_name*` `keep_tagged=false` | Delete blockout primitives by Monolith.Owner tag. Respects keep_tagged to preserve replaced actors. |
| `[w] export_blockout_layout` | `volume_name*` | Export blockout layout as JSON. Positions normalized 0-1 relative to volume, sizes absolute. |
| `[w] import_blockout_layout` | `volume_name*` `layout_json*` (object or string from export_blockout_layout) | Import a blockout layout into a volume. Scales POSITIONS to fit, keeps SIZES unchanged. Flags overflow. |
| `[w] scan_volume` | `volume_name*` `ray_density=medium` (low/medium/high) `vertical_layers=3` | Multi-origin radial sweep of a volume. Detects walls, floor, ceiling, openings. Semantic JSON output. |
| `[w] scatter_props` | `volume_name*` `asset_paths*` `count*` `min_spacing=50` (cm) `random_rotation=true` `random_scale_range=[0.9,1.1]` `seed=0` `surface_align=false` `collision_mode=warn` (none/warn/reject/adjust) | Poisson disk scatter props within a volume. Floor trace, overlap check, random rotation, reproducible seed. |
| `[w] create_blockout_blueprint` | `save_path=/Game/Monolith/Blockout/BP_MonolithBlockoutVolume` `force=false` | Create the BP_MonolithBlockoutVolume Blueprint asset in the project. One-time setup — editable RoomType, BlockoutTags, Density, physics, wall/ceiling properties. Drag into levels for blockout volumes with proper Details panel UX. |

### Context Prop (8)

| Action | Params | Purpose |
|--------|--------|---------|
| `[w] scatter_on_surface` | `surface_actor*` `asset_paths*` `count=5` (max 100) `surface_side=top` (top/inside) `min_spacing=15` (cm) `random_rotation=true` `seed=0` `random_scale_range=[0.9,1.1]` `collision_mode=warn` (none/warn/reject/adjust) | Place props ON a specific surface actor (shelf top, table top, cabinet interior). Detects surface top via bounds + downward trace. Poisson disk spacing. |
| `[w] scatter_on_walls` | `volume_name*` `asset_paths*` `count=10` (max 100) `wall_offset=2` (cm) `min_spacing=80` (cm) `height_range=[0.3,0.8]` (fractions 0-1) `seed=0` `collision_mode=warn` (none/warn/reject/adjust) | Horizontal traces outward from volume center to find walls. Places props at hit points aligned to wall normal. For paintings, clocks, signs, sconces. |
| `[w] scatter_on_ceiling` | `volume_name*` `asset_paths*` `count=8` (max 100) `ceiling_offset=2` (cm) `min_spacing=100` (cm) `seed=0` `collision_mode=warn` (none/warn/reject/adjust) | Upward traces to find ceiling. Places props hanging from hit points. For chains, cables, lights, pipes. |
| `[w] set_room_disturbance` | `volume_name*` `disturbance*` (orderly/slightly_messy/ransacked/abandoned) `seed=42` `exclude_actors?` `exclude_tags?` | Apply disturbance level to placed props in a volume: orderly (aligned), slightly_messy (small offsets), ransacked (large offsets, tipped), abandoned (pushed to edges). Single undo transaction. |
| `[w] configure_physics_props` | `actor_names?` `volume_name?` `simulate_physics=true` `start_asleep=true` `mass_override?` (kg) `collision_profile=PhysicsActor` | Batch-configure SimulatePhysics and sleep state on actors. Auto-sets Mobility to Movable. Optionally set mass and collision profile. (Pass `actor_names` or `volume_name`.) |
| `[w] settle_props` | `actor_names?` `volume_name?` `max_tilt=5` (deg) `seed=0` | Trace-based gravity settle: for each prop, traces downward and snaps to hit surface with small random tilt. No PIE required. Single undo transaction. (Pass `actor_names` or `volume_name`.) |
| `[w] create_prop_kit` | `name*` `items*` (array of {label,asset_path,offset,rotation,scale,required,spawn_chance}) `description?` `overwrite=false` | Author a prop kit JSON file: items with relative positions, rotation, scale, spawn chances. Saved to Saved/Monolith/PropKits/. |
| `[w] place_prop_kit` | `kit_name*` `location*` ([x,y,z]) `rotation=[0,0,0]` `seed=0` `folder?` `validate_placement=true` | Spawn a prop kit at a world location. Random item selection based on spawn_chance. Single undo transaction. |

### Preset (8)

| Action | Params | Purpose |
|--------|--------|---------|
| `list_storytelling_patterns` | `source_filter?` (built-in/user) | List all available storytelling patterns: built-in horror defaults + user-created patterns from Saved/Monolith/Patterns/. Returns name, description, element count, and source (built-in vs user). |
| `[w] create_storytelling_pattern` | `name*` `description*` `elements*` (array of {label,type:decal/prop,relative_offset,size,radial,radial_min,radial_max,count_min,count_max,rotation_variance,scale_variance,wall_element}) `overwrite=false` | Author a new storytelling pattern JSON. Defines element types (decal/prop), radial distribution, size ranges, spawn counts, rotation/scale variance. Saved to Saved/Monolith/Patterns/. |
| `list_acoustic_profiles` | `source_filter?` (built-in/user) | List all acoustic profiles: built-in horror defaults (12 surfaces from MonolithLevelDesignAcoustics) + user-created profiles from Saved/Monolith/AcousticProfiles/. Returns profile name, genre, surface count, and source. |
| `[w] create_acoustic_profile` | `name*` `surfaces*` (array of {surface_name,absorption:0-1,transmission_loss_db,footstep_loudness:0-1}) `genre=custom` `description?` `overwrite=false` | Author an acoustic property set for a genre. Each surface defines absorption (0-1), transmission_loss_db, and footstep_loudness. Saved to Saved/Monolith/AcousticProfiles/. |
| `[w] create_tension_profile` | `name*` `factors*` ({factor:{weight:0-1,invert:bool}}; factors: sightline_length/ceiling_height/room_volume/exit_count/lighting_level/audio_reverb) `genre=custom` `description?` `thresholds?` ({low,medium,high}) `overwrite=false` | Define tension scoring weights for a genre. Override how sightline_length, ceiling_height, room_volume, exit_count, lighting_level, and audio_reverb contribute to the tension score. Saved to Saved/Monolith/TensionProfiles/. |
| `list_genre_presets` | (none) | List all available genre preset packs from Saved/Monolith/Presets/. Shows name, genre, version, and content summary (pattern count, profile count, etc.). |
| `[w] export_genre_preset` | `name*` `genre=custom` `description?` `include_patterns?` `include_acoustic_profiles?` `include_tension_profiles?` `include_templates?` `include_prop_kits?` `overwrite=false` | Bundle all user-created templates + patterns + acoustic profiles + tension profiles + prop kits into a single JSON preset file. Optionally filter by name lists. |
| `[w] import_genre_preset` | `preset_name*` `merge_mode=skip_existing` (overwrite/skip_existing/rename_conflicts) | Load a genre preset pack JSON. Extracts all sub-presets (patterns, acoustic profiles, tension profiles, templates, prop kits) into their respective directories. |

### Template (4)

| Action | Params | Purpose |
|--------|--------|---------|
| `list_room_templates` | `category?` (e.g. residential/commercial/medical) | List available room templates from the templates directory. Optionally filter by category. |
| `get_room_template` | `template_name*` (without .json) | Load the full JSON definition of a room template by name. |
| `[w] apply_room_template` | `volume_name*` `template_name*` `mirror=false` (mirror along X) `rotate=0` (0/90/180/270) | Apply a room template to a blockout volume. Scales furniture positions to fit, creates blockout primitives. Single undo transaction. |
| `[w] create_room_template` | `volume_name*` `template_name*` `category=custom` `description?` | Save the current blockout layout of a volume as a reusable JSON template. |

---

The remaining sections are **gated** behind `bEnableProceduralTownGen = true` (off by default — they are absent from the live snapshot, so no param signatures are listed; confirm params via `monolith_discover({ namespace: "worldgen", action: "<action>", mode: "schema" })` once the setting is enabled).

### Arch Feature (5) — **[gated]**

| Action | Purpose |
|--------|---------|
| `create_balcony` | Generate a balcony: floor slab + railing extending from an upper floor wall face. Styles: simple (posts + top rail), bars (vertical balusters), solid (solid panel). With building_context: auto-orients to wall normal and emits wall_openings (french_door). |
| `create_fire_escape` | Generate a multi-story fire escape: zigzag exterior stairs between floor landings. Each floor gets a landing platform. Stairs alternate left/right. Optional roof ladder. With building_context: auto-orients to wall, aligns landings to floor heights, emits wall_openings (windows). |
| `create_porch` | Generate a covered porch: floor platform, support columns, roof slab, and optional entry steps with railings. Configurable column count, step geometry, roof overhang. With building_context: auto-orients, aligns porch floor to building floor, emits wall_openings (door). |
| `create_railing` | Generate a railing along an arbitrary path defined by 3D points. Styles: simple (posts + top rail), bars (+ vertical balusters), solid (+ panel infill). With building_context: auto-orients to wall normal for placement. |
| `create_ramp_connector` | Generate an ADA-compliant ramp between two heights. Auto-computes run length from rise and slope ratio. Adds intermediate switchback landings if rise exceeds max_rise_per_run. With building_context: auto-orients to wall and emits wall_openings (door at top). |

### Terrain (5) — **[gated]**

| Action | Purpose |
|--------|---------|
| `analyze_building_site` | Given a building footprint polygon and terrain samples, determine the optimal foundation strategy (flat, cut_and_fill, stepped, piers, walkout_basement). Returns strategy, slope, pad Z, and ramp specs if hospice mode. |
| `create_foundation` | Generate foundation geometry for a building on terrain. Supports flat pad, cut-and-fill, stepped, pier, and walkout basement strategies. Saves to StaticMesh and optionally places in scene. |
| `create_retaining_wall` | Generate a retaining wall along a terrain cut edge. Wall height varies along its length based on terrain samples. Tapered profile (thicker at base). |
| `place_building_on_terrain` | Full pipeline: sample terrain under building footprint, analyze site, generate foundation, and adjust building Z. Optionally creates retaining walls and ADA ramps (hospice mode). |
| `sample_terrain_grid` | Sample an NxM grid of terrain heights via downward line traces. Returns a 2D height grid, min/max/avg Z, slope analysis, and roughness. Output feeds into analyze_building_site and create_foundation. |

### City Block (4) — **[gated]**

| Action | Purpose |
|--------|---------|
| `create_city_block` | Generate a complete city block: subdivide into lots, build each building (floor plan + walls + facades + roofs), create streets with sidewalks and curbs, place street furniture, apply horror decay, and register everything in the spatial registry. Each step is also available as a standalone action for finer control. |
| `create_lot_layout` | Subdivide a rectangular block into building lots using OBB recursive, grid, or organic subdivision. Returns lot positions and generated street segments. Does NOT generate buildings — use create_city_block for full pipeline. |
| `create_street` | Generate street geometry: road surface, sidewalks, and curbs as a single static mesh. Curbs are 15cm raised edges. Sidewalks are flat raised surfaces on each side. |
| `place_street_furniture` | Place street furniture (lamps, hydrants, benches, mailboxes, trash cans) along a street segment. Items are spawned via create_parametric_mesh through the tool registry. |

### Facade (3) — **[gated]**

| Action | Purpose |
|--------|---------|
| `apply_horror_damage` | Apply procedural horror damage to a building facade: boarded windows, broken glass, cracks. Operates on an existing facade actor by generating damage overlay geometry. |
| `generate_facade` | Generate a building facade with windows, doors, trim, and cornices from a Building Descriptor's exterior faces. CGA-style vertical split (base/shaft/cap) with even window placement. Returns facade mesh + element metadata. |
| `list_facade_styles` | List available facade style JSON presets from the FacadeStyles/ directory. |

### Floor Plan Generator (3) — **[gated]**

| Action | Purpose |
|--------|---------|
| `generate_floor_plan` | Generate a complete floor plan from a building archetype. Returns grid, rooms, and doors in the exact format consumed by create_building_from_grid. Algorithm: archetype loading -> room resolution -> squarified treemap layout -> adjacency validation -> privacy gradient -> corridor insertion -> door placement -> horror post-processing -> Space Syntax scoring. WP-2: Supports adjacency_matrix (MUST/MUST_NOT), privacy gradient, wet wall clustering, and circulation patterns (double_loaded, hub_spoke, racetrack, enfilade). WP-6: Horror subversion (door locking, dead-end control, loop breaking, wrong-room injection), Space Syntax metrics (integration, connectivity, depth), tension curve metadata per room, and hospice safety caps. |
| `get_building_archetype` | Return the full JSON definition of a specific building archetype. |
| `list_building_archetypes` | List all available building archetype JSON files in the archetypes directory. |

### Furnishing (3) — **[gated]**

| Action | Purpose |
|--------|---------|
| `furnish_building` | Furnish all rooms in a building from the spatial registry. Iterates each room, loads the appropriate preset, and delegates to furnish_room. |
| `furnish_room` | Furnish a single room with parametric furniture based on room type. Places items using create_parametric_mesh via the tool registry. Collision-aware: items that would clip walls, doors, or other furniture are skipped. |
| `list_furniture_presets` | List available furniture preset configurations. Returns preset names and summaries. |

### Building (2) — **[gated]**

| Action | Purpose |
|--------|---------|
| `create_building_from_grid` | Generate a multi-room building from a 2D grid of room IDs. Walls placed only at room-ID boundaries (no shared-wall duplication). Doors as boolean subtracts at grid edges. Returns the Building Descriptor JSON consumed by all downstream SPs (facades, roofs, furnishing, etc). |
| `create_grid_from_rooms` | Helper: takes a list of room rectangles and generates a 2D grid + room definitions. Output feeds directly into create_building_from_grid. |

### Building Validation (1) — **[gated]**

| Action | Purpose |
|--------|---------|
| `validate_building` | Post-generation validation of a procedural building. Checks door passability (capsule sweeps), room connectivity (BFS from entrance), window openings (raycasts), and stair angles. Returns a per-check breakdown with an overall playability score. |

### Roof (1) — **[gated]**

| Action | Purpose |
|--------|---------|
| `generate_roof` | Generate roof geometry from a building footprint polygon. Types: gable, hip, flat (parapet), shed, gambrel. Consumes footprint_polygon from the Building Descriptor (SP1). MaterialID 0 = roof surface, 1 = soffit, 2 = fascia/parapet. |

## Common Workflows

The numbered recipes below use only **always-on** actions (Blockout, Context Prop, Preset, Template). The town-pipeline recipes that follow them are **[gated]** behind `bEnableProceduralTownGen = true` and call gated actions (annotated). Confirm any param via `monolith_discover({ namespace: "worldgen", action: "<action>", mode: "schema" })`.

### Blockout → scatter → inspect (always-on, end-to-end)

There is no always-on `validate_building` (that action is **[gated]**), so this flow validates by **inspecting** the result with always-on `get_blockout_volume_info` / `export_blockout_layout`. Tag a `BlockingVolume` first.

```
1. worldgen_query("get_blockout_volumes", {})                                           # find Monolith.Blockout-tagged actors; warns about partial-tag actors
2. worldgen_query("setup_blockout_volume", { "volume_name": "<vol>", "room_type": "<type>", "density": "Normal" })  # [w] apply Monolith blockout tags (clears existing)
3. worldgen_query("scan_volume", { "volume_name": "<vol>", "ray_density": "medium", "vertical_layers": 3 })          # [w] radial sweep → walls/floor/ceiling/openings JSON
4. worldgen_query("create_blockout_primitives_batch", { "primitives": [ { "shape": "box", "location": [x,y,z], "scale": [x,y,z], "category": "furniture" } ], "volume_name": "<vol>" })  # [w] batch blockout, single undo (max 200)
5. worldgen_query("scatter_props", { "volume_name": "<vol>", "asset_paths": ["<mesh>"], "count": 12, "min_spacing": 50, "seed": 0, "collision_mode": "warn" })  # [w] Poisson-disk scatter, reproducible seed
6. worldgen_query("match_all_in_volume", { "volume_name": "<vol>", "tolerance_pct": 20, "top_n": 3 })                # [w] match primitives to mesh catalog → replacement plan
7. worldgen_query("apply_replacement", { "replacements": [ { "blockout_actor": "<a>", "replacement_asset": "<mesh>" } ], "volume_name": "<vol>" })  # [w] ATOMIC swap to real meshes
8. worldgen_query("get_blockout_volume_info", { "volume_name": "<vol>" })                 # inspect: parsed tags + primitives + other actors (verify result)
9. worldgen_query("export_blockout_layout", { "volume_name": "<vol>" })                   # [w] export normalized JSON layout for diff / reuse
```

### Dress a room from a template, then disturb + settle (always-on)

```
1. worldgen_query("list_room_templates", { "category": "residential" })                  # available templates (optional category filter)
2. worldgen_query("apply_room_template", { "volume_name": "<vol>", "template_name": "<name>", "rotate": 0 })  # [w] scale furniture to fit + create primitives
3. worldgen_query("scatter_on_walls", { "volume_name": "<vol>", "asset_paths": ["<mesh>"], "count": 10, "height_range": [0.3,0.8], "seed": 0 })  # [w] paintings/signs aligned to wall normal
4. worldgen_query("set_room_disturbance", { "volume_name": "<vol>", "disturbance": "ransacked", "seed": 42 })  # [w] orderly/slightly_messy/ransacked/abandoned, single undo
5. worldgen_query("settle_props", { "volume_name": "<vol>", "max_tilt": 5, "seed": 0 })   # [w] trace-based gravity settle, no PIE
6. worldgen_query("create_room_template", { "volume_name": "<vol>", "template_name": "<new>", "category": "custom" })  # [w] save the dressed layout back as a reusable template
```

### Town pipeline (**[gated]** — needs `bEnableProceduralTownGen = true`)

- **Single building (spans `scene`):** `list_building_archetypes` → `generate_floor_plan` → `create_building_from_grid` → `generate_facade` → `generate_roof` → `scene.register_building` → `scene.auto_volumes_for_building` → `furnish_building` → `validate_building`
- **City block:** `create_city_block` (all-in-one) — or per lot + `create_street` → `place_street_furniture` → `scene.auto_volumes_for_block`
- **Building on terrain:** `sample_terrain_grid` → `analyze_building_site` → `create_foundation` → `place_building_on_terrain`

## Gotchas

- Procedural town gen is **experimental**, gated by `bEnableProceduralTownGen = true` (off by default — known geometry issues).
- Building/room **registration lives in `scene`** (`scene.register_building` / `register_room`); call it before furnishing or spatial queries.
- Use `omit_exterior_walls: true` on `create_building_from_grid` when you will add `generate_facade` (avoids double walls).
- Run `validate_building` after generation (capsule sweep, BFS connectivity, stair angles, window raycasts).
- Stairwells need ≥ 4×6 cells at 270 cm floor height for switchback stairs.

## Notes

- This reference is generated from the live `RegisterAction` surface. If an action is missing or renamed, re-run `monolith_discover({ namespace: "worldgen" })` — the catalog is the source of truth.
- Pass `mode: "schema"` to `monolith_discover` for required/optional params and types before calling an action.
