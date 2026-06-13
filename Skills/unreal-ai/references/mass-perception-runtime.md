# unreal-ai — Mass/ZoneGraph, Perception, Controller, Runtime, Discovery & Advanced actions

Action names + params are a snapshot of the live `ai` registry. Always confirm exact params with
`monolith_discover({ namespace: "ai", action: "<action>", mode: "schema" })`.

**Param notation:** `name*` required, `name?` optional, `name=val` default, `a/b/c` allowed values, `[w]` mutates (wraps a transaction). Signatures are a snapshot of the live catalog — for the exact, full, current schema of any action call `monolith_discover` with `mode: "schema"`.

## Mass Zone Graph (22)

Most `spawn_*` / `set_*` / `pause_*` / `rebuild_*` actions here are availability-only guards: they take a reserved `confirm=false` and do NOT mutate runtime state yet.

| Action | Purpose | Params (req* opt? =default) |
|--------|---------|-----------------------------|
| `despawn_mass_spawner` | Report MassSpawner despawn availability (guarded; no mutation) | `[w] actor*, confirm=false` |
| `find_nearest_zone_lane` | Return the nearest ZoneGraph lane from overlapping candidates | `location*, radius=1000` |
| `find_overlapping_zone_lanes` | Delegate to query_zone_lanes when ZoneGraph support is registered | `location*, radius=1000` |
| `get_crowd_lane_state` | Report MassCrowd lane-state availability | `(none)` |
| `get_mass_simulation_status` | Report Mass/ZoneGraph module availability, world context, runtime actors | `world_context=editor` |
| `get_mass_spawner` | Inspect one MassSpawner-like actor by label, name, or path | `actor*, world_context=editor` |
| `get_zone_shape` | Inspect one ZoneShape/ZoneGraph-like actor | `actor*, world_context=editor` |
| `list_mass_spawners` | List loaded MassSpawner-like actors (`world_context` = editor/pie) | `world_context=editor, limit=100` |
| `list_zone_lane_profiles` | Return conservative lane-profile discovery metadata | `(none)` |
| `list_zone_shapes` | List loaded ZoneShape/ZoneGraph-like actors | `world_context=editor, limit=100` |
| `list_zone_tags` | Tag names found on loaded ZoneShape/ZoneGraph-like actors | `world_context=editor` |
| `pause_mass_simulation` | Report Mass pause availability; refuses ambiguous editor-vs-PIE state | `[w] world_context=pie, confirm=false` |
| `rebuild_zone_graph` | Report ZoneGraph rebuild availability (no long-running rebuild) | `[w] confirm=false` |
| `remove_zone_shape` | Report ZoneShape removal availability (no level mutation) | `[w] actor*, confirm=false` |
| `resume_mass_simulation` | Report Mass resume availability; refuses ambiguous editor-vs-PIE state | `[w] world_context=pie, confirm=false` |
| `set_crowd_lane_state` | Report MassCrowd lane-state mutation availability | `[w] confirm=false` |
| `set_mass_spawner_count` | Report MassSpawner count mutation availability (no mutation) | `[w] actor*, count*, confirm=false` |
| `set_mass_spawner_scale` | Report MassSpawner scale mutation availability (no mutation) | `[w] actor*, scale*, confirm=false` |
| `set_zone_shape_points` | Report ZoneShape point mutation availability (no level mutation) | `[w] actor*, points*, confirm=false` |
| `set_zone_shape_tags` | Report ZoneShape tag mutation availability (no level mutation) | `[w] actor*, tags*, confirm=false` |
| `spawn_mass_spawner` | Report MassSpawner spawn availability (guarded; no mutation) | `[w] confirm=false` |
| `spawn_zone_shape` | Report ZoneShape spawn availability (no level mutation) | `[w] confirm=false` |

## Advanced (12)

| Action | Purpose | Params (req* opt? =default) |
|--------|---------|-----------------------------|
| `add_mass_trait` | Add a trait to a MassEntityConfigAsset | `[w] asset_path*, trait_class*, properties?` |
| `create_mass_entity_config` | Create a new MassEntityConfigAsset | `[w] save_path*, name?, parent_config?` |
| `get_mass_entity_config` | Inspect a MassEntityConfigAsset: traits, fragments, parent config | `asset_path*` |
| `get_mass_entity_stats` | Runtime MassEntity statistics: archetype counts, entity totals (requires PIE) | `(none)` |
| `get_zone_lane_info` | Detailed info about a zone graph lane by handle index | `lane_handle*, data_handle?` |
| `list_mass_entity_configs` | List all UMassEntityConfigAsset assets in the project | `path_filter?` |
| `list_mass_processors` | List all registered UMassProcessor subclasses | `(none)` |
| `list_mass_traits` | List all available UMassEntityTraitBase subclasses | `(none)` |
| `list_zone_graphs` | Enumerate ZoneGraphData actors in loaded levels | `level?` |
| `query_zone_lanes` | Spatial query for zone graph lanes near a world location | `location*, radius?, tag_filter?` |
| `remove_mass_trait` | Remove a trait from a MassEntityConfigAsset | `[w] asset_path*, trait_class*` |
| `validate_mass_entity_config` | Validate: trait compatibility, missing fragments, duplicates | `asset_path*` |

## Perception (11)

| Action | Purpose | Params (req* opt? =default) |
|--------|---------|-----------------------------|
| `add_perception_component` | Add UAIPerceptionComponent to an AI controller BP via SCS (`dominant_sense?` = Sight/Hearing/Damage/Touch/Team/Prediction) | `[w] asset_path*, dominant_sense?` |
| `add_stimuli_source_component` | Add UAIPerceptionStimuliSourceComponent (`register_as_source_for*` = sense array) | `[w] asset_path*, register_as_source_for*, auto_register?` |
| `configure_damage_sense` | Configure damage sense (creates if absent) | `[w] asset_path*, implementation?, max_age?, starts_enabled?` |
| `configure_hearing_sense` | Configure hearing sense (creates if absent) | `[w] asset_path*, range*, affiliation?, max_age?, starts_enabled?` |
| `configure_sight_sense` | Configure sight sense (creates if absent); `affiliation?` = {enemies,neutrals,friendlies} | `[w] asset_path*, radius*, lose_radius?, peripheral_angle?, affiliation?, auto_success_range?, pov_offset?, near_clipping_radius?, auto_register_all_pawns?, max_age?, starts_enabled?` |
| `configure_stimuli_source` | Configure which senses a stimuli source registers for | `[w] asset_path*, sense_types*, auto_register?` |
| `configure_touch_sense` | Configure touch sense (creates if absent; no Implementation UPROPERTY in UE 5.7) | `[w] asset_path*, affiliation?, max_age?, starts_enabled?` |
| `get_ai_system_config` | Read UAISystem global settings (perception aging, stimulus limits, etc.) | `(none)` |
| `get_perception_config` | Read all senses, params, affiliation filters from an AI controller BP | `asset_path*` |
| `remove_sense` | Remove a sense config (`sense_type*` = Sight/Hearing/Damage/Touch/Team/Prediction) | `[w] asset_path*, sense_type*` |
| `validate_perception_setup` | Validate: senses configured, affiliation set, dominant sense assigned | `asset_path*` |

## Perception Scaffold (1)

| Action | Purpose | Params (req* opt? =default) |
|--------|---------|-----------------------------|
| `add_perception_to_actor` | Add perception to ANY Actor BP + configure senses in one call (`senses*` v1 = Sight/Hearing/Damage) | `[w] actor_bp_path*, senses*, sight_radius=1500, hearing_range=3000` |

## Controller (10)

| Action | Purpose | Params (req* opt? =default) |
|--------|---------|-----------------------------|
| `create_ai_controller` | Create an AAIController Blueprint, optionally setting default BT and BB | `[w] save_path*, name?, bt_path?, bb_path?` |
| `get_ai_actors` | List all AI-controlled actors in the current PIE world | `class_filter?` |
| `get_ai_controller` | Read AI controller config: default BT, BB, perception setup, flags | `asset_path*` |
| `get_ai_team` | Read the generic team ID from an AI controller CDO | `asset_path*` |
| `list_ai_controllers` | List all AAIController Blueprint assets in the project | `path_filter?` |
| `set_ai_controller_bt` | Set default Behavior Tree and optionally Blackboard on an AI controller CDO | `[w] asset_path*, bt_path*, bb_path?` |
| `set_ai_controller_flags` | Set boolean config flags on an AI controller CDO | `[w] asset_path*, wants_player_state?, start_ai_on_possess?, skip_extra_los_checks?, allow_strafe?` |
| `set_ai_team` | Set generic team ID (`team_id*` 0-254, 255 = NoTeam) for affiliation-based perception | `[w] asset_path*, team_id*` |
| `set_pawn_ai_controller_class` | Set AIControllerClass on a Pawn or Character Blueprint CDO | `[w] blueprint_path*, controller_class*` |
| `spawn_ai_actor` | Spawn an AI pawn/character BP actor in the editor level | `[w] class_path*, location*, rotation?, label?, folder_path?` |

## Runtime (14) — PIE/game session only

All runtime actions resolve the AI by `actor*` (label, name, or path in PIE).

| Action | Purpose | Params (req* opt? =default) |
|--------|---------|-----------------------------|
| `runtime_check_perception` | Check if a target is perceived by an observer, and by which senses | `[w] observer_actor*, target_actor*` |
| `runtime_clear_bb_value` | Clear a Blackboard key on a running AI | `[w] actor*, key_name*` |
| `runtime_find_smart_objects` | Find available Smart Object slots near an actor | `[w] querier_actor*, activity_tags?, radius?` |
| `runtime_get_bb_value` | Read a Blackboard value from a running AI's blackboard component | `[w] actor*, key_name*` |
| `runtime_get_bt_execution_path` | Snapshot of BT execution: active branch, current task, decorator states | `[w] actor*` |
| `runtime_get_bt_state` | BehaviorTree runtime state: active node, tree status, pending aborts | `[w] actor*` |
| `runtime_get_perceived_actors` | List all actors currently perceived (`sense_filter?` = Sight/Hearing/Damage/Touch/Team) | `[w] actor*, sense_filter?` |
| `runtime_get_st_active_states` | Get active StateTree states and task statuses | `[w] actor*` |
| `runtime_report_noise` | Fire a noise event at a location for AI hearing | `[w] location*, loudness?, instigator?, tag?` |
| `runtime_run_eqs_query` | Run an EQS query synchronously and return scored results | `[w] querier_actor*, query_path*, max_results?` |
| `runtime_send_st_event` | Send an FStateTreeEvent to a running StateTree component | `[w] actor*, event_tag*` |
| `runtime_set_bb_value` | Write a Blackboard value on a running AI (auto-converted by key type) | `[w] actor*, key_name*, value*` |
| `runtime_start_bt` | Start/restart a BehaviorTree (`run_mode?` = looped (default) / single_run) | `[w] actor*, bt_path?, run_mode?` |
| `runtime_stop_bt` | Stop BehaviorTree execution on an AI controller | `[w] actor*` |

## Discovery (11)

| Action | Purpose | Params (req* opt? =default) |
|--------|---------|-----------------------------|
| `detect_ai_circular_references` | Check BT RunBehavior chains, ST linked assets, BB parent chains for cycles | `path_filter?` |
| `export_ai_manifest` | Full project AI manifest with cross-refs (`format?` = json (default) / markdown) | `[w] path_filter?, format?` |
| `find_eqs_references` | Find which BTs, STs, and Blueprints reference a given EQS query | `eqs_path*` |
| `find_so_references` | Find which BTs, STs, and levels reference a given Smart Object definition | `so_path*` |
| `get_ai_behavior_summary` | Structured JSON summary of a BT or ST: flow paths, decision points, BB deps | `asset_path*` |
| `get_ai_overview` | Scan project: count BTs, BBs, STs, EQS queries, Smart Objects, AI Controllers | `path_filter?` |
| `lint_behavior_tree` | Style lint: unreachable branches, redundant decorators, single-child composites | `[w] asset_path*` |
| `lint_state_tree` | Style lint: no-task states, self-transitions without delay, dead-end states | `[w] asset_path*` |
| `list_ai_node_types` | Unified type discovery (`system*` = bt/st/eqs; `category?` system-specific) | `system*, category?` |
| `search_ai_assets` | Full-text search across AI asset names (`asset_type?` = bt/bb/st/eqs/so/controller) | `query*, asset_type?` |
| `validate_ai_data_flow` | Trace full data flow for an AI Controller: BB keys vs BT refs, EQS->BB, Perception | `controller_path*` |

## Scaffold (23) — one-call AI authoring

| Action | Purpose | Params (req* opt? =default) |
|--------|---------|-----------------------------|
| `batch_validate_ai_assets` | Run all validators across AI assets (explicit `asset_paths?` skips full scan) | `[w] path_filter?, asset_paths?` |
| `create_bt_from_template` | Create a BT from a named template (`template*` = patrol/chase_attack/flee/search_area/guard_post) | `[w] save_path*, template*` |
| `create_st_from_template` | Create a ST from a named template (`template*` = patrol/combat/investigation/ambient) | `[w] save_path*, template*` |
| `hello_world_ai` | ONE-CALL onboarding: Character + Controller + BT (patrol) + BB + Perception + team | `[w] save_path*, name?, location?` |
| `scaffold_ai_controller_blueprint` | Full controller setup: BP + link BT/BB + perception + team | `[w] save_path*, bt_path*, bb_path*, perception_preset?, team_id?` |
| `scaffold_ambient_npc` | Ambient civilian NPC with Smart Object interactions and wander | `[w] save_path*, name*, smart_objects?, wander_radius?` |
| `scaffold_boss_ai` | Multi-phase boss AI with health-threshold phase transitions | `[w] save_path*, name*, phases?` |
| `scaffold_companion_ai` | Friendly companion AI: follows player, optionally fights (`combat_behavior?` = passive/defensive/aggressive) | `[w] save_path*, name*, follow_distance?, combat_behavior?` |
| `scaffold_complete_ai_character` | Full AI stack: Character + Controller + BT + BB + Perception + Team | `[w] save_path*, name*, mesh?, bt_template?, perception_preset?, team_id?` |
| `scaffold_enemy_ai` | Basic enemy chase+attack (`archetype*` = melee/ranged/charger) | `[w] save_path*, name*, archetype*, team_id?` |
| `scaffold_eqs_move_sequence` | Add a RunEQS→store→MoveTo sequence to an existing BT | `[w] bt_path*, eqs_path*, bb_key*, parent_id?` |
| `scaffold_flying_ai` | Flying AI with 3D navigation (`altitude_range?` = 'min,max' cm, default 500,2000) | `[w] save_path*, name*, altitude_range?` |
| `scaffold_group_coordinator` | Squad coordinator assigning tactical roles (default flanker/suppressor/rusher) | `[w] save_path*, name*, roles?` |
| `scaffold_horror_ambush` | Horror ambush: dormant until triggered, burst attack, retreat (jump-scare) | `[w] save_path*, name*, trigger_type?, attack_pattern?` |
| `scaffold_horror_mimic` | Horror mimic: disguises as a static object, attacks when player is close | `[w] save_path*, name*, disguise_mesh?, reveal_conditions?` |
| `scaffold_horror_presence` | Invisible horror presence manipulating environment (lights, doors, sounds) | `[w] save_path*, name*, effects?` |
| `scaffold_horror_stalker` | Horror stalker: follows at distance, closes in during dark/vulnerability | `[w] save_path*, name*, stalk_distance?, attack_conditions?` |
| `scaffold_patrol_investigate_ai` | Guard scaffold: patrol→hear→investigate→search→return (`patrol_type?` = loop/pingpong/random) | `[w] save_path*, name*, patrol_type?, investigation_radius?` |
| `scaffold_perception_to_blackboard` | Wire perception events to BB keys (`mappings?` = [{sense, bb_key}]) | `[w] controller_path*, bb_path*, mappings?` |
| `scaffold_stealth_game_ai` | Stealth AI with detection meter + alert cascade (`alert_states?` = simple/standard/full) | `[w] save_path*, name*, detection_meter?, alert_states?` |
| `scaffold_team_system` | Full team setup: team attitude DataTable (`teams*` = [{id, name}]) | `[w] save_path*, teams*, attitudes?` |
| `scaffold_turret_ai` | Stationary turret with detection cone and engagement range | `[w] save_path*, name*, detection_cone?, engagement_range?` |
| `validate_ai_controller` | Validate an AI controller: BT/BB refs, perception configured, team set | `asset_path*` |
