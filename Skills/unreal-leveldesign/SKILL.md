---
name: unreal-leveldesign
description: Use for level-design ANALYSIS and horror/accessibility passes via Monolith MCP (leveldesign namespace) — acoustics scoring, horror spatial analysis, encounter design, and accessibility validation that READ the scene and return heuristic scores rather than placing assets. To author the audio ASSET (Sound Cue/MetaSound/attenuation) use unreal-audio; for raw spatial primitives (raycast/overlap/nearest/navmesh) and actor placement use unreal-scene; to implement patrol/perception/nav BEHAVIOR use unreal-ai; to generate the building/town geometry use unreal-worldgen. Triggers on leveldesign, level design, acoustics, reverb, RT60, sound propagation, footstep, stealth, quiet path, sightline, hiding spot, ambush, choke point, dead end, tension, pacing, encounter, patrol route, spawn point, scare, safe room, monster reveal, co-op, accessibility, wheelchair, contrast, rest point, hospice, escape route.
---

# unreal-leveldesign

**43 actions** via `leveldesign_query(action, params)`. Action names below are the live registry surface; call `monolith_discover` for exact parameter schemas.

## Discovery

```
monolith_discover({ namespace: "leveldesign" })                      # all actions in this namespace
monolith_discover({ namespace: "leveldesign", action: "<action>", mode: "schema" })  # exact params
```

## When to use / Use a different skill for

Use **unreal-leveldesign** for the *analysis pass* — read the existing scene and return heuristic scores, maps, and design specs for acoustics, horror tension, encounters, and accessibility. These actions are mostly read-only; they do not author assets or place gameplay logic.

- **unreal-audio** — author the actual audio ASSET (Sound Cue, MetaSound, attenuation, submix, sound class). This skill only *scores* acoustics/reverb/RT60/footstep propagation as a design pass; it does not create sound assets.
- **unreal-scene** — the raw spatial primitives an analysis pass is built on (raycast/overlap/nearest/line-of-sight/navmesh path, `build_navmesh`) and actor/light/decal placement, versus the horror/accessibility/encounter *scoring* on top.
- **unreal-ai** — implement the patrol/perception/navigation BEHAVIOR (Behavior Tree, EQS, nav links, perception) that an encounter pass designs, versus the patrol-route/spawn-point *scoring* analysis.
- **unreal-worldgen** — generate the blockout/building/town geometry that this analysis then reads, versus the analysis itself.

## Action Reference

Param notation: `name*` required, `name?` optional, `name=val` default, `a/b/c` allowed, `[w]` mutates (transaction-wrapped). Signatures are a snapshot of the live catalog — for the exact full schema call `monolith_discover` with `mode: "schema"` (the discover block above is the authority). All positions are `[x,y,z]` arrays; all distances are in cm unless noted.

### Audio (14)

| Action | Purpose | Params |
|--------|---------|--------|
| `analyze_room_acoustics` | Sample surfaces via raycasts in a volume, area-weighted absorption. Sabine RT60: 0.161 * Volume_m3 / TotalAbsorption. Classify: dead/dry/live/echo. | `volume_name*` `ray_count?=128` |
| `analyze_sound_propagation` | Sound propagation between two points: direct trace + indirect navmesh path. Returns better-audibility path. | `from*` `to*` `include_occlusion?=true` |
| `can_ai_hear_from` | Can AI hear the player? Best of direct trace + navmesh path. Returns yes/faintly/no + detection radius. | `ai_location*` `player_location*` `surface_type?` `ai_hearing_range?=2000` |
| `create_audio_volume` [w] | Spawn an AAudioVolume matching a blocking volume's shape. Undo transaction. | `volume_name*` `reverb_preset?` `priority?=0` `label?` |
| `create_surface_datatable` [w] | Bootstrap the acoustic system: create surface DataTable + register surface types via UPhysicsSettings CDO. | `template?=horror_default` `save_path?` |
| `estimate_footstep_sound` | Downward trace to determine floor surface type and footstep loudness factor. | `location*` |
| `find_loud_surfaces` | Find high-footstep-loudness surfaces (metal/glass/gravel) in a volume or region. | `volume_name?` `region_min?` `region_max?` `loudness_threshold?=0.5` |
| `find_quiet_path` | Score candidate navmesh paths by surface loudness. Returns lowest-loudness route. | `start*` `end*` `max_loudness?=0.3` |
| `find_sound_paths` | Multi-method path finder: direct trace, first-bounce reflections, navmesh indirect. Sorted by attenuation. | `from*` `to*` `max_bounces?=2` (1-3) `candidate_surfaces?=16` |
| `get_audio_volumes` | Enumerate all AAudioVolume actors; reverb/interior settings, bounds, uncovered regions. | `include_details?=true` |
| `get_stealth_map` | Grid-sample a volume: per-cell footstep loudness + AI detection radius heatmap. | `volume_name*` `grid_size?=100` `ai_hearing_range?=2000` |
| `get_surface_materials` | Cast rays in all directions to catalog physical materials in a volume or region. | `volume_name?` `region_min?` `region_max?` `ray_count?=64` |
| `set_surface_type` [w] | Set physical-material surface-type override on a mesh actor component. Undo transaction. | `actor_name*` `surface_type*` |
| `suggest_audio_volumes` [w] | Suggest AAudioVolume reverb settings from RT60 + material classification. | `volume_name*` |

### Encounter (8)

| Action | Purpose | Params |
|--------|---------|--------|
| `analyze_ai_territory` | Score a region as AI territory: hiding density, patrol coverage, sightline control, ambush potential, escape routes. | `region*` `archetype?=stalker` (stalker/patrol/ambusher) `granularity?=200` |
| `analyze_level_pacing_structure` | Macro tension-to-release rhythm across a level path. Identifies encounter zones, safe rooms, exploration areas. | `start*` `end*` `waypoints?` `sample_interval?=500` |
| `design_encounter` [w] | Capstone: compose spawns, patrols, entry/exit, sightline breaks, audio zones into a scored encounter spec JSON. | `region*` `archetype?=stalker` (stalker/patrol/ambusher/swarm) `difficulty?=medium` (low/medium/high) `enemy_blueprint?` `constraints?` `dry_run?=true` |
| `evaluate_safe_room` [w] | Score a room as a safe room: entrances, defensibility, lighting, isolation, size, hospice accessibility. | `region*` |
| `generate_hospice_report` [w] | Full hospice audit: intensity caps, rest spacing, cognitive load, input demands, audio alternatives. | `start*` `end*` `profile?` (motor_impaired/vision_impaired/cognitive_fatigue; empty=all) `walk_speed_cms?=300` |
| `generate_scare_sequence` [w] | Procedurally generate a scare-event sequence spec (not placed actors). | `path_points*` `style?=escalating` (slow_burn/escalating/relentless/single_peak) `intensity_cap?=1.0` `scare_types?` (audio/visual/environmental/entity_spawn) `count?=5` |
| `suggest_patrol_route` [w] | Generate navmesh patrol routes per AI archetype. | `region*` `archetype?=patrol` (stalker/patrol/ambusher) `waypoint_count?=5` `patrol_style?=loop` (loop/back_and_forth/random) `constraints?` `player_path?` |
| `validate_horror_intensity` | Audit horror intensity for hospice compliance: tension ceiling, escape windows, jump scares. | `start*` `end*` `intensity_cap?=50` (0-100) `flag_jump_scares?=true` `min_rest_distance_cm?=800` `min_escape_routes?=2` |

### Horror (8)

| Action | Purpose | Params |
|--------|---------|--------|
| `analyze_choke_points` | Find narrow passages along a navmesh path: width, flank possibility, bypass routes. | `start*` `end*` `agent_radius?=45` |
| `analyze_escape_routes` | Find and score escape routes from a location to tagged exit actors. | `location*` `exit_tags?` `max_routes?=5` |
| `analyze_pacing_curve` | Sample tension along a path: monotonous stretches, optimal scare placement, false-calm. | `path_points*` `sample_interval?=200` |
| `analyze_sightlines` | Fan-of-rays sightline analysis: claustrophobia 0-100, blocked % at thresholds, longest clear line. | `location*` `forward?` (default +X) `fov?=90` `ray_count?=36` `max_distance?=5000` |
| `classify_zone_tension` [w] | Composite tension: sightline + ceiling height + volume + exit count. Returns calm/uneasy/tense/dread/panic. | `location*` `radius?=500` |
| `find_ambush_points` | Find ambush positions lateral to a path; scores concealment + surprise angle. | `path_points*` `lateral_range?=500` `concealment_threshold?=0.7` |
| `find_dead_ends` | Navmesh flood-fill for single-exit regions: depth, width, exit direction. | `region_min?` `region_max?` (default whole navmesh) `grid_size?=200` |
| `find_hiding_spots` | Grid-sample a region, score concealment from viewpoints. Sorted by quality. | `region_min*` `region_max*` `viewpoints*` `grid_size?=100` `min_concealment?=0.6` |

### Accessibility (6)

| Action | Purpose | Params |
|--------|---------|--------|
| `analyze_visual_contrast` | Visual contrast of interactable actors vs backgrounds via scene capture. WCAG-inspired. | `location*` `forward?` `fov?=90` `tags?` |
| `find_rest_points` | Walk a path, inventory safe rooms/calm zones, flag gaps exceeding max_gap. | `start*` `end*` `max_gap?=3000` |
| `generate_accessibility_report` [w] | Combined path width, nav complexity, contrast, rest points, interactive reach. | `start*` `end*` `profile?` (motor_impaired/vision_impaired/cognitive_fatigue) |
| `validate_interactive_reach` | Check interactable actors for height, navmesh distance, obstructions. | `region_min?` `region_max?` `tags?` |
| `validate_navigation_complexity` | Score cognitive nav difficulty: turn count, sharp corners, backtracking, elevation. | `start*` `end*` |
| `validate_path_width` | Validate path width for wheelchair accessibility. Returns pinch points with obstruction actors. | `start*` `end*` `min_width?=120` |

### Horror Design (4)

| Action | Purpose | Params |
|--------|---------|--------|
| `evaluate_encounter_pacing` [w] | Spacing/intensity of multiple encounters along a path: back-to-back, rest gaps, curve issues. | `path_points*` `encounters*` (`[{location,type,intensity,duration_s}]`) `target_pacing?=horror_standard` (horror_standard/hospice_gentle/action) `walk_speed_cms?=400` |
| `evaluate_spawn_point` [w] | Composite enemy-spawn score: visibility delay, lighting, audio cover, escape proximity, path commitment. | `location*` `player_paths?` `player_location?` `weights?` (`{visibility_delay,lighting,audio_cover,escape_proximity,path_commitment}`) |
| `predict_player_paths` [w] | Weighted navmesh paths via strategy heuristics + per-strategy scores. | `start*` `end*` `strategies?` (shortest/safest/curious/cautious) `agent_radius?=45` `agent_height?=180` `waypoints?` `sample_density?=200` `max_samples?=500` `max_paths_per_strategy?=3` `walk_speed_cms?=400` |
| `suggest_scare_positions` [w] | Optimal scripted-scare positions along a path: anticipation, visibility, timing, agency. | `path_points*` `scare_type?=visual` (audio/visual/entity_spawn/environmental) `count?=5` `min_spacing_cm?=1000` `intensity_curve?=escalating` (escalating/wave/random) `hospice_mode?=false` |

### Quality (3)

| Action | Purpose | Params |
|--------|---------|--------|
| `analyze_co_op_balance` | Co-op spatial design: blind spots, separation opportunities, communication distances. | `player_positions*` `region_min?` `region_max?` |
| `analyze_framing` | Camera composition: rule of thirds, depth layering, leading lines (screen-space projection). | `camera_location*` `camera_rotation*` (`[pitch,yaw,roll]`) `focal_actor?` `fov?=90` `aspect_ratio?=1.777` |
| `evaluate_monster_reveal` [w] | Score a monster reveal: silhouette, backlight, distance, partial visibility, camera alignment. | `player_location*` `player_rotation*` (`[pitch,yaw,roll]`) `monster_actor*` `fov?=90` |

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
