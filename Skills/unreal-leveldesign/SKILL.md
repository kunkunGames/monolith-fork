---
name: unreal-leveldesign
description: "Use for level-design analysis and horror/accessibility passes via Monolith MCP: audio & acoustics (footsteps, reverb RT60, sound propagation, stealth maps, quiet paths), horror spatial analysis (sightlines, hiding/ambush spots, choke points, tension zones, pacing), encounter design (patrol routes, spawn-point scoring, scare sequences, safe rooms), and accessibility validation (wheelchair paths, visual contrast, rest points, hospice reports). Triggers on acoustics, reverb, RT60, footstep, sound propagation, stealth, sightline, hiding spot, ambush, choke point, tension, pacing, encounter, patrol, scare, safe room, accessibility, wheelchair, contrast, rest point, hospice, escape route."
---

# unreal-leveldesign

**43 actions** via `leveldesign_query(action, params)`. Action names below are the live registry surface; call `monolith_discover` for exact parameter schemas.

## Discovery

```
monolith_discover({ namespace: "leveldesign" })                      # all actions in this namespace
monolith_discover({ namespace: "leveldesign", action: "<action>", mode: "schema" })  # exact params
```

## Action Reference

### Audio (14)

| Action | Purpose |
|--------|---------|
| `analyze_room_acoustics` | Sample surfaces via raycasts in a volume, compute area-weighted absorption. Sabine RT60: 0.161 * Volume_m3 / TotalAbsorption. Classify: dead/dry/live/echo. |
| `analyze_sound_propagation` | Analyzes sound propagation between two points. Direct trace (wall occlusion) + indirect navmesh path (doorway propagation). Returns whichever path has better audibility. |
| `can_ai_hear_from` | Can AI hear the player? Direct trace (wall occlusion) + indirect navmesh path (doorway propagation). Uses best path. Returns yes/faintly/no + detection radius. |
| `create_audio_volume` | Spawn an AAudioVolume matching a blocking volume's shape. Set reverb/interior settings. Undo transaction. |
| `create_surface_datatable` | Bootstrap the acoustic system: create a DataTable with surface properties and register surface types via UPhysicsSettings CDO. |
| `estimate_footstep_sound` | Downward trace at a location to determine floor surface type and footstep loudness factor. |
| `find_loud_surfaces` | Find surfaces with high footstep loudness (metal, glass, gravel) in a volume or region. Returns locations, areas, detection radii. |
| `find_quiet_path` | Sample candidate navmesh paths between two points, score by surface loudness along path. Returns lowest-loudness route. |
| `find_sound_paths` | Multi-method sound path finder: direct trace, first-bounce reflections (image-source), and navmesh indirect path (doorway propagation). Returns all viable paths sorted by attenuation. |
| `get_audio_volumes` | Enumerate all AAudioVolume actors. Returns reverb/interior settings, priority, bounds. Flags uncovered regions. |
| `get_stealth_map` | Grid-sample a volume: per-cell footstep loudness + AI detection radius. Returns heatmap data. |
| `get_surface_materials` | Cast rays in all directions to catalog physical materials in a volume or region. Returns material breakdown with acoustic properties. |
| `set_surface_type` | Set physical material surface type override on a mesh actor's component. Undo transaction. |
| `suggest_audio_volumes` | Given room geometry + surface materials, suggest AAudioVolume reverb settings based on RT60 + material classification. |

### Encounter (8)

| Action | Purpose |
|--------|---------|
| `analyze_ai_territory` | Score a region as AI territory: hiding spot density, patrol route coverage, sightline control, ambush potential, escape routes for AI disengagement. |
| `analyze_level_pacing_structure` | Macro-level tension-to-release rhythm across an entire level path. Identifies encounter zones, safe rooms, exploration areas. Compares to ideal pacing curves. |
| `design_encounter` | Capstone: compose spawn points, patrol routes, player entry/exit, sightline breaks, and audio zones into a scored encounter specification. Returns a complete encounter blueprint JSON. |
| `evaluate_safe_room` | Score a room as a safe room: entrance count, defensibility, lighting quality, sound isolation, size, hospice accessibility. Detects doors via actor tags/class. |
| `generate_hospice_report` | Full level audit for hospice patients: intensity caps, rest spacing (every 2-3 min), cognitive load, input demands, one-handed playability, audio alternatives for visual scares. Profiles: motor_impaired, vision_impaired, cognitive_fatigue. |
| `generate_scare_sequence` | Procedurally generate a sequence of scare events with variety, escalation, and pacing. Output is a specification, not placed actors. |
| `suggest_patrol_route` | Generate navmesh patrol routes per AI archetype. Stalker: stay in earshot but out of sight. Patrol: regular loop hitting checkpoints. Ambusher: concealed wait position with surprise angle. |
| `validate_horror_intensity` | Audit horror intensity for hospice compliance. Checks max tension never exceeds profile ceiling. Verifies generous escape windows. Flags jump scares. |

### Horror (8)

| Action | Purpose |
|--------|---------|
| `analyze_choke_points` | Find narrow passages along a navmesh path. Returns choke points with width, flank possibility, and bypass routes. |
| `analyze_escape_routes` | Find and score escape routes from a location to tagged exit actors. Critical for hospice: ensures no inescapable encounters. |
| `analyze_pacing_curve` | Sample tension at intervals along a path. Identifies monotonous stretches, optimal scare placement, and false-calm opportunities. |
| `analyze_sightlines` | Fan-of-rays sightline analysis from a location. Returns claustrophobia score 0-100, blocked percentages at distance thresholds, longest clear sightline. |
| `classify_zone_tension` | Composite tension analysis: sightline distance + ceiling height + room volume + exit count. Returns calm/uneasy/tense/dread/panic. |
| `find_ambush_points` | Find ambush positions lateral to a path. Scores concealment + surprise angle (180 degrees from player forward = perfect ambush). |
| `find_dead_ends` | Navmesh flood-fill to find single-exit (dead-end) regions. Returns depth, width, exit direction for each. |
| `find_hiding_spots` | Grid-sample a region and score each point for concealment from given viewpoints. Returns spots sorted by quality. |

### Accessibility (6)

| Action | Purpose |
|--------|---------|
| `analyze_visual_contrast` | Analyze visual contrast of interactable actors against their backgrounds using scene capture. WCAG-inspired thresholds. |
| `find_rest_points` | Walk a path and inventory safe rooms/calm zones. Flag gaps exceeding max_gap (default 30m). Hospice patients need frequent rest opportunities. |
| `generate_accessibility_report` | Comprehensive accessibility report combining path width, navigation complexity, visual contrast, rest points, and interactive reach. Profile-specific thresholds. |
| `validate_interactive_reach` | Check interactable actors for height, navmesh distance, and obstructions. Flag items requiring jumping or precision movement. |
| `validate_navigation_complexity` | Score cognitive difficulty of navigation between two points: turn count, sharp corners, backtracking, elevation changes. |
| `validate_path_width` | Validate path width for wheelchair accessibility (default 120cm min). Returns pinch points with exact obstruction actors. |

### Horror Design (4)

| Action | Purpose |
|--------|---------|
| `evaluate_encounter_pacing` | Analyze spacing and intensity of multiple encounter positions along a level path. Flags back-to-back encounters, insufficient rest periods, and intensity curve issues. |
| `evaluate_spawn_point` | Composite score for an enemy spawn location. Evaluates visibility delay, lighting, audio cover, escape proximity, and path commitment from player paths. |
| `predict_player_paths` | Generate weighted navmesh paths between two points using multiple strategy heuristics: shortest, safest, curious, cautious. Returns path points, distance, estimated time, and per-strategy scores. |
| `suggest_scare_positions` | Find optimal positions for scripted scare events along a player path. Scores anticipation buildup, player visibility, timing, and player agency. Supports hospice mode. |

### Quality (3)

| Action | Purpose |
|--------|---------|
| `analyze_co_op_balance` | Analyze spatial design for co-op play: coverage blind spots, separation opportunities, communication distances. Given multiple player positions, evaluate the level's co-op balance. |
| `analyze_framing` | Camera composition scoring: rule of thirds placement, depth layering, leading lines. Projects actors to screen space from a camera viewpoint and analyzes composition. |
| `evaluate_monster_reveal` | Score a monster reveal moment: silhouette quality (screen coverage), backlight potential, distance rating, partial visibility, player camera alignment. Uses traces and sightline analysis. |

## Related skills

- Live scene actors, volumes, navmesh build, lighting: `unreal-scene`
- Procedural blockout / town generation: `unreal-worldgen`
- Mesh assets: `unreal-mesh`

## Typical workflows

- **Audio & acoustics:** `create_surface_datatable` → `get_surface_materials` → `analyze_room_acoustics` → `get_stealth_map` → `can_ai_hear_from` → `find_quiet_path`
- **Horror spatial:** `analyze_sightlines` → `find_hiding_spots` / `find_ambush_points` → `analyze_escape_routes` → `classify_zone_tension` → `analyze_pacing_curve`
- **Encounter design:** `design_encounter` → `suggest_patrol_route` → `evaluate_spawn_point` → `generate_scare_sequence`
- **Accessibility (hospice mission):** `generate_accessibility_report` (or individual `validate_*`) → `find_rest_points` → `generate_hospice_report`

## Gotchas

- Most actions are **read-only analysis**. They need scene context built first: a navmesh (`scene.build_navmesh`) for path-based queries and `AAudioVolume`s for room acoustics.
- Acoustic actions read physical-material → surface mappings; bootstrap them once with `create_surface_datatable`.
- Tension / pacing / encounter scores are heuristics for authoring guidance, not runtime gameplay values.

## Notes

- This reference is generated from the live `RegisterAction` surface. If an action is missing or renamed, re-run `monolith_discover({ namespace: "leveldesign" })` — the catalog is the source of truth.
- Pass `mode: "schema"` to `monolith_discover` for required/optional params and types before calling an action.
