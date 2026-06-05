---
name: unreal-worldgen
description: "Use for procedural blockout and town/building generation via Monolith MCP: blockout volumes & primitives, asset matching/replacement, prop scatter & context props (surface/wall/ceiling, disturbance), terrain patches & foundations, architectural features (balcony/porch/fire escape/ramp/railing), floor plans, facades, roofs, city blocks & streets, room furnishing, and genre presets. Triggers on blockout, primitive, scan volume, scatter, prop kit, disturbance, terrain, foundation, balcony, porch, fire escape, ramp, railing, floor plan, archetype, facade, roof, city block, lot, street furniture, building shell, furnish, preset, validate building."
---

# unreal-worldgen

**63 actions** via `worldgen_query(action, params)`. Action names below are the live registry surface; call `monolith_discover` for exact parameter schemas.

## Discovery

```
monolith_discover({ namespace: "worldgen" })                      # all actions in this namespace
monolith_discover({ namespace: "worldgen", action: "<action>", mode: "schema" })  # exact params
```

## Action Reference

### Blockout (16)

| Action | Purpose |
|--------|---------|
| `apply_replacement` | ATOMIC: Replace blockout primitives with real mesh assets. Validates ALL assets first, single undo, pivot adjustment in local space. |
| `clear_blockout` | Delete blockout primitives by Monolith.Owner tag. Respects keep_tagged to preserve replaced actors. |
| `create_blockout_blueprint` | Create the BP_MonolithBlockoutVolume Blueprint asset in the project. One-time setup — creates /Game/Monolith/Blockout/BP_MonolithBlockoutVolume with editable RoomType, BlockoutTags, Density, physics, wall/ceiling properties. Drag into levels for blockout volumes with proper Details panel UX. |
| `create_blockout_grid` | Create a floor grid of box primitives filling a blockout volume. |
| `create_blockout_primitive` | Spawn a scaled BasicShape as a blockout primitive with category-colored material and owner tags. |
| `create_blockout_primitives_batch` | Create multiple blockout primitives in a single undo transaction. Max 200 primitives. Warns if outside volume bounds. |
| `export_blockout_layout` | Export blockout layout as JSON. Positions normalized 0-1 relative to volume, sizes absolute. |
| `get_blockout_volume_info` | Get detailed info for a blockout volume: parsed tags, blockout primitives, and other actors. |
| `get_blockout_volumes` | Find all actors with Monolith.Blockout tag. Warns about misconfigured actors with partial Monolith tags. |
| `import_blockout_layout` | Import a blockout layout into a volume. Scales POSITIONS to fit, keeps SIZES unchanged. Flags overflow. |
| `match_all_in_volume` | Batch match all blockout primitives in a volume against mesh catalog. Returns replacement plan. |
| `match_asset_to_blockout` | Find mesh catalog assets that match a blockout primitive's size. Axis-sorted matching with weighted scoring. |
| `scan_volume` | Multi-origin radial sweep of a volume. Detects walls, floor, ceiling, openings. Semantic JSON output. |
| `scatter_props` | Poisson disk scatter props within a volume. Floor trace, overlap check, random rotation, reproducible seed. |
| `set_actor_tags` | Batch apply tags to multiple actors in a single undo transaction. |
| `setup_blockout_volume` | Apply Monolith blockout tags to a BlockingVolume. Clears existing Monolith tags first. |

### Context Prop (8)

| Action | Purpose |
|--------|---------|
| `configure_physics_props` | Batch-configure SimulatePhysics and sleep state on actors. Auto-sets Mobility to Movable. Optionally set mass and collision profile. |
| `create_prop_kit` | Author a prop kit JSON file: items with relative positions, rotation, scale, spawn chances. Saved to Saved/Monolith/PropKits/. |
| `place_prop_kit` | Spawn a prop kit at a world location. Random item selection based on spawn_chance. Single undo transaction. |
| `scatter_on_ceiling` | Upward traces to find ceiling. Places props hanging from hit points. For chains, cables, lights, pipes. |
| `scatter_on_surface` | Place props ON a specific surface actor (shelf top, table top, cabinet interior). Detects surface top via bounds + downward trace. Poisson disk spacing. |
| `scatter_on_walls` | Horizontal traces outward from volume center to find walls. Places props at hit points aligned to wall normal. For paintings, clocks, signs, sconces. |
| `set_room_disturbance` | Apply disturbance level to placed props in a volume: orderly (aligned), slightly_messy (small offsets), ransacked (large offsets, tipped), abandoned (pushed to edges). Single undo transaction. |
| `settle_props` | Trace-based gravity settle: for each prop, traces downward and snaps to hit surface with small random tilt. No PIE required. Single undo transaction. |

### Preset (8)

| Action | Purpose |
|--------|---------|
| `create_acoustic_profile` | Author an acoustic property set for a genre. Each surface defines absorption (0-1), transmission_loss_db, and footstep_loudness. Saved to Saved/Monolith/AcousticProfiles/. |
| `create_storytelling_pattern` | Author a new storytelling pattern JSON. Defines element types (decal/prop), radial distribution, size ranges, spawn counts, rotation/scale variance. Saved to Saved/Monolith/Patterns/. |
| `create_tension_profile` | Define tension scoring weights for a genre. Override how sightline_length, ceiling_height, room_volume, exit_count, lighting_level, and audio_reverb contribute to the tension score. Saved to Saved/Monolith/TensionProfiles/. |
| `export_genre_preset` | Bundle all user-created templates + patterns + acoustic profiles + tension profiles + prop kits into a single JSON preset file. Optionally filter by name lists. |
| `import_genre_preset` | Load a genre preset pack JSON. Extracts all sub-presets (patterns, acoustic profiles, tension profiles, templates, prop kits) into their respective directories. |
| `list_acoustic_profiles` | List all acoustic profiles: built-in horror defaults (12 surfaces from MonolithLevelDesignAcoustics) + user-created profiles from Saved/Monolith/AcousticProfiles/. Returns profile name, genre, surface count, and source. |
| `list_genre_presets` | List all available genre preset packs from Saved/Monolith/Presets/. Shows name, genre, version, and content summary (pattern count, profile count, etc.). |
| `list_storytelling_patterns` | List all available storytelling patterns: built-in horror defaults + user-created patterns from Saved/Monolith/Patterns/. Returns name, description, element count, and source (built-in vs user). |

### Arch Feature (5)

| Action | Purpose |
|--------|---------|
| `create_balcony` | Generate a balcony: floor slab + railing extending from an upper floor wall face. Styles: simple (posts + top rail), bars (vertical balusters), solid (solid panel). With building_context: auto-orients to wall normal and emits wall_openings (french_door). |
| `create_fire_escape` | Generate a multi-story fire escape: zigzag exterior stairs between floor landings. Each floor gets a landing platform. Stairs alternate left/right. Optional roof ladder. With building_context: auto-orients to wall, aligns landings to floor heights, emits wall_openings (windows). |
| `create_porch` | Generate a covered porch: floor platform, support columns, roof slab, and optional entry steps with railings. Configurable column count, step geometry, roof overhang. With building_context: auto-orients, aligns porch floor to building floor, emits wall_openings (door). |
| `create_railing` | Generate a railing along an arbitrary path defined by 3D points. Styles: simple (posts + top rail), bars (+ vertical balusters), solid (+ panel infill). With building_context: auto-orients to wall normal for placement. |
| `create_ramp_connector` | Generate an ADA-compliant ramp between two heights. Auto-computes run length from rise and slope ratio. Adds intermediate switchback landings if rise exceeds max_rise_per_run. With building_context: auto-orients to wall and emits wall_openings (door at top). |

### Terrain (5)

| Action | Purpose |
|--------|---------|
| `analyze_building_site` | Given a building footprint polygon and terrain samples, determine the optimal foundation strategy (flat, cut_and_fill, stepped, piers, walkout_basement). Returns strategy, slope, pad Z, and ramp specs if hospice mode. |
| `create_foundation` | Generate foundation geometry for a building on terrain. Supports flat pad, cut-and-fill, stepped, pier, and walkout basement strategies. Saves to StaticMesh and optionally places in scene. |
| `create_retaining_wall` | Generate a retaining wall along a terrain cut edge. Wall height varies along its length based on terrain samples. Tapered profile (thicker at base). |
| `place_building_on_terrain` | Full pipeline: sample terrain under building footprint, analyze site, generate foundation, and adjust building Z. Optionally creates retaining walls and ADA ramps (hospice mode). |
| `sample_terrain_grid` | Sample an NxM grid of terrain heights via downward line traces. Returns a 2D height grid, min/max/avg Z, slope analysis, and roughness. Output feeds into analyze_building_site and create_foundation. |

### City Block (4)

| Action | Purpose |
|--------|---------|
| `create_city_block` | Generate a complete city block: subdivide into lots, build each building (floor plan + walls + facades + roofs), create streets with sidewalks and curbs, place street furniture, apply horror decay, and register everything in the spatial registry. Each step is also available as a standalone action for finer control. |
| `create_lot_layout` | Subdivide a rectangular block into building lots using OBB recursive, grid, or organic subdivision. Returns lot positions and generated street segments. Does NOT generate buildings — use create_city_block for full pipeline. |
| `create_street` | Generate street geometry: road surface, sidewalks, and curbs as a single static mesh. Curbs are 15cm raised edges. Sidewalks are flat raised surfaces on each side. |
| `place_street_furniture` | Place street furniture (lamps, hydrants, benches, mailboxes, trash cans) along a street segment. Items are spawned via create_parametric_mesh through the tool registry. |

### Template (4)

| Action | Purpose |
|--------|---------|
| `apply_room_template` | Apply a room template to a blockout volume. Scales furniture positions to fit, creates blockout primitives. Single undo transaction. |
| `create_room_template` | Save the current blockout layout of a volume as a reusable JSON template. |
| `get_room_template` | Load the full JSON definition of a room template by name. |
| `list_room_templates` | List available room templates from the templates directory. Optionally filter by category. |

### Facade (3)

| Action | Purpose |
|--------|---------|
| `apply_horror_damage` | Apply procedural horror damage to a building facade: boarded windows, broken glass, cracks. Operates on an existing facade actor by generating damage overlay geometry. |
| `generate_facade` | Generate a building facade with windows, doors, trim, and cornices from a Building Descriptor's exterior faces. CGA-style vertical split (base/shaft/cap) with even window placement. Returns facade mesh + element metadata. |
| `list_facade_styles` | List available facade style JSON presets from the FacadeStyles/ directory. |

### Floor Plan Generator (3)

| Action | Purpose |
|--------|---------|
| `generate_floor_plan` | Generate a complete floor plan from a building archetype. Returns grid, rooms, and doors in the exact format consumed by create_building_from_grid. Algorithm: archetype loading -> room resolution -> squarified treemap layout -> adjacency validation -> privacy gradient -> corridor insertion -> door placement -> horror post-processing -> Space Syntax scoring. WP-2: Supports adjacency_matrix (MUST/MUST_NOT), privacy gradient, wet wall clustering, and circulation patterns (double_loaded, hub_spoke, racetrack, enfilade). WP-6: Horror subversion (door locking, dead-end control, loop breaking, wrong-room injection), Space Syntax metrics (integration, connectivity, depth), tension curve metadata per room, and hospice safety caps. |
| `get_building_archetype` | Return the full JSON definition of a specific building archetype. |
| `list_building_archetypes` | List all available building archetype JSON files in the archetypes directory. |

### Furnishing (3)

| Action | Purpose |
|--------|---------|
| `furnish_building` | Furnish all rooms in a building from the spatial registry. Iterates each room, loads the appropriate preset, and delegates to furnish_room. |
| `furnish_room` | Furnish a single room with parametric furniture based on room type. Places items using create_parametric_mesh via the tool registry. Collision-aware: items that would clip walls, doors, or other furniture are skipped. |
| `list_furniture_presets` | List available furniture preset configurations. Returns preset names and summaries. |

### Building (2)

| Action | Purpose |
|--------|---------|
| `create_building_from_grid` | Generate a multi-room building from a 2D grid of room IDs. Walls placed only at room-ID boundaries (no shared-wall duplication). Doors as boolean subtracts at grid edges. Returns the Building Descriptor JSON consumed by all downstream SPs (facades, roofs, furnishing, etc). |
| `create_grid_from_rooms` | Helper: takes a list of room rectangles and generates a 2D grid + room definitions. Output feeds directly into create_building_from_grid. |

### Building Validation (1)

| Action | Purpose |
|--------|---------|
| `validate_building` | Post-generation validation of a procedural building. Checks door passability (capsule sweeps), room connectivity (BFS from entrance), window openings (raycasts), and stair angles. Returns a per-check breakdown with an overall playability score. |

### Roof (1)

| Action | Purpose |
|--------|---------|
| `generate_roof` | Generate roof geometry from a building footprint polygon. Types: gable, hip, flat (parapet), shed, gambrel. Consumes footprint_polygon from the Building Descriptor (SP1). MaterialID 0 = roof surface, 1 = soffit, 2 = fascia/parapet. |

## Related skills (the town pipeline spans `worldgen` + `scene`)

- Spatial registry, auto-volumes, navmesh, scene actors: `unreal-scene`
- Horror / accessibility / audio analysis of the result: `unreal-leveldesign`
- Mesh asset cleanup of generated geometry: `unreal-mesh`

## Typical workflows

- **Blockout → art pass:** `get_blockout_volumes` → `scan_volume` → `create_blockout_primitives_batch` → `match_all_in_volume` → `apply_replacement`
- **Single building (spans `scene`):** `list_building_archetypes` → `generate_floor_plan` → `create_building_from_grid` → `generate_facade` → `generate_roof` → `scene.register_building` → `scene.auto_volumes_for_building` → `furnish_building` → `validate_building`
- **City block:** `create_city_block` (all-in-one) — or per lot + `create_street` → `place_street_furniture` → `scene.auto_volumes_for_block`
- **Building on terrain:** `sample_terrain_grid` → `analyze_building_site` → `create_foundation` → `place_building_on_terrain`
- **Context props:** `scatter_props` / `scatter_on_surface` / `scatter_on_walls` → `set_room_disturbance` → `settle_props`

## Gotchas

- Procedural town gen is **experimental**, gated by `bEnableProceduralTownGen = true` (off by default — known geometry issues).
- Building/room **registration lives in `scene`** (`scene.register_building` / `register_room`); call it before furnishing or spatial queries.
- Use `omit_exterior_walls: true` on `create_building_from_grid` when you will add `generate_facade` (avoids double walls).
- Run `validate_building` after generation (capsule sweep, BFS connectivity, stair angles, window raycasts).
- Stairwells need ≥ 4×6 cells at 270 cm floor height for switchback stairs.

## Notes

- This reference is generated from the live `RegisterAction` surface. If an action is missing or renamed, re-run `monolith_discover({ namespace: "worldgen" })` — the catalog is the source of truth.
- Pass `mode: "schema"` to `monolith_discover` for required/optional params and types before calling an action.
