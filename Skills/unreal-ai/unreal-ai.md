---
name: unreal-ai
description: "Use when working with Unreal Engine AI via Monolith MCP: Behavior Trees, StateTree, Blackboard, EQS, navigation/navmesh and nav links, Smart Objects, Mass/ZoneGraph crowds, AI perception, scaffolding new AI, and runtime AI debugging. Triggers on AI, behavior tree, BT, StateTree, blackboard, EQS, environment query, navmesh, nav link, nav modifier, smart object, mass, zonegraph, crowd, perception, AI controller, pawn sensing, patrol, navigation, AI scaffold."
---

# unreal-ai

**243 actions** via `ai_query(action, params)`. Action names below are the live registry surface; call `monolith_discover` for exact parameter schemas.

## Discovery

```
monolith_discover({ namespace: "ai" })                      # all actions in this namespace
monolith_discover({ namespace: "ai", action: "<action>", mode: "schema" })  # exact params
```

## Action Reference

### State Tree (35)

| Action | Purpose |
|--------|---------|
| `add_st_consideration` | Add a utility consideration to a state |
| `add_st_enter_condition` | Add an enter condition to a state |
| `add_st_extension` | Add an extension to a StateTree asset |
| `add_st_property_binding` | Wire a property binding between source and target paths |
| `add_st_state` | Add a state to the StateTree. Omit parent_state_id (or pass null) to add at root level (SubTree). If parent_state_id is supplied but doesn't match a state, returns an error. |
| `add_st_task` | Add a task (FInstancedStruct) to a state |
| `add_st_transition` | Add a transition to a state |
| `add_st_transition_condition` | Add a condition to an existing transition on a state |
| `auto_arrange_st` | Auto-layout a StateTree graph using built-in state processing. Blueprint Assist is disabled because this action has a built-in formatter. |
| `build_state_tree_from_spec` | Declarative full-tree creation from a JSON spec: creates states, adds tasks, transitions, and compiles |
| `compile_state_tree` | Compile a StateTree via UStateTreeEditingSubsystem. MANDATORY after any edits. |
| `configure_st_consideration` | Configure a consideration's properties on a state (by index) |
| `create_state_tree` | Create a new UStateTree asset. Optionally set schema class. |
| `delete_state_tree` | Delete a StateTree asset |
| `duplicate_state_tree` | Deep copy a StateTree asset to a new path |
| `export_st_spec` | Export a StateTree as a JSON spec that can be fed back into build_state_tree_from_spec |
| `generate_st_diagram` | Generate a Mermaid state diagram from a StateTree |
| `get_st_bindable_properties` | List available bindable properties, optionally scoped to a state/task |
| `get_st_bindings` | List all property bindings in a StateTree |
| `get_state_tree` | Full tree structure as JSON: states (recursive), tasks, conditions, transitions, considerations |
| `list_st_condition_types` | List all available StateTree condition struct types |
| `list_st_extension_types` | List all available UStateTreeExtension subclasses |
| `list_st_task_types` | List all available FStateTreeTaskBase subclasses |
| `list_state_trees` | List all UStateTree assets in the project |
| `move_st_state` | Reparent a state under a different parent state |
| `remove_st_enter_condition` | Remove an enter condition from a state by index |
| `remove_st_property_binding` | Remove a property binding by index |
| `remove_st_state` | Remove a state and its children from the StateTree |
| `remove_st_task` | Remove a task from a state by index |
| `remove_st_transition` | Remove a transition from a state by index |
| `rename_st_state` | Rename a state in the StateTree |
| `set_st_schema` | Set schema class and optional context actor class on a StateTree |
| `set_st_state_properties` | Set state properties like weight, selection_behavior, tag, enabled |
| `set_st_task_property` | Set a property on a task via ImportText_Direct reflection |
| `validate_state_tree` | Validate a StateTree: check unbound inputs, dead-end states, infinite loops, missing tasks |

### Behavior Tree (32)

| Action | Purpose |
|--------|---------|
| `add_bt_decorator` | Add a decorator as a sub-node on a target BT node |
| `add_bt_node` | Add a composite or task node to a Behavior Tree. parent_id=null adds under root. |
| `add_bt_run_eqs_task` | Convenience: add a fully configured RunEQSQuery task node |
| `add_bt_service` | Add a service as a sub-node on a composite or task node |
| `add_bt_smart_object_task` | Convenience: add a FindAndUseSmartObject task node |
| `add_bt_use_ability_task` | Convenience: add a fully configured TryActivateAbility task node that fires a GAS ability on tick |
| `auto_arrange_bt` | Auto-layout a Behavior Tree graph using built-in depth/breadth positioning. Blueprint Assist is disabled for asset mutation. |
| `build_behavior_tree_from_spec` | Create a complete Behavior Tree from a declarative JSON spec — the crown jewel |
| `clone_bt_subtree` | Copy a subtree from one Behavior Tree to another (deep clone with decorators, services, properties) |
| `compare_behavior_trees` | Structural diff between two Behavior Trees: nodes added/removed/moved, property changes |
| `create_behavior_tree` | Create a new Behavior Tree asset, optionally linking a Blackboard |
| `create_bt_decorator_blueprint` | Create a BTDecorator Blueprint (parent defaults to BTDecorator_BlueprintBase) |
| `create_bt_service_blueprint` | Create a BTService Blueprint (parent defaults to BTService_BlueprintBase) |
| `create_bt_task_blueprint` | Create a BTTask Blueprint (parent defaults to BTTaskNode) |
| `delete_behavior_tree` | Delete a Behavior Tree asset |
| `duplicate_behavior_tree` | Deep copy a Behavior Tree asset to a new path |
| `export_bt_spec` | Export an existing Behavior Tree as a JSON spec (inverse of build_behavior_tree_from_spec) |
| `generate_bt_diagram` | Generate a text diagram of a Behavior Tree (ASCII tree or Mermaid graph) |
| `get_behavior_tree` | Full tree structure as JSON — nodes, decorators, services, hierarchy from root |
| `get_bt_graph` | Flat node array with parent_id + children GUIDs. Use when you need to look up a node by ID without walking the full tree (vs get_behavior_tree which returns a nested tree). |
| `get_bt_node_properties` | Read all UPROPERTYs from a BT node instance |
| `import_bt_spec` | Recreate a Behavior Tree from an exported spec (overwrites existing tree structure) |
| `list_behavior_trees` | List all UBehaviorTree assets in the project |
| `list_bt_node_classes` | List all available BT node classes (composites, tasks, decorators, services) with descriptions |
| `move_bt_node` | Reparent a node under a different composite in the Behavior Tree |
| `remove_bt_decorator` | Remove a decorator from a BT node by index |
| `remove_bt_node` | Remove a node and its children from a Behavior Tree |
| `remove_bt_service` | Remove a service from a BT node by index |
| `reorder_bt_children` | Reorder child nodes under a composite by specifying new GUID order |
| `set_bt_blackboard` | Set or change the Blackboard reference on a Behavior Tree |
| `set_bt_node_property` | Set a UPROPERTY on a BT node instance. Special handling for FBlackboardKeySelector. |
| `validate_behavior_tree` | Validate a Behavior Tree: check BB key refs, unreachable branches, empty composites, missing properties |

### Navigation (24)

| Action | Purpose |
|--------|---------|
| `add_nav_bounds_volume` | Spawn ANavMeshBoundsVolume at a location with given extents |
| `add_nav_invoker_component` | Add UNavigationInvokerComponent to a Blueprint for dynamic nav generation around the actor |
| `add_nav_link_proxy` | Spawn ANavLinkProxy connecting two navigation points for jumps, drops, etc. |
| `add_nav_modifier_volume` | Spawn ANavModifierVolume that overrides the nav area class within its bounds |
| `analyze_navigation_coverage` | Grid-sample the level and project points to navmesh — reports coverage percentage, gaps, and stats |
| `build_navigation` | Trigger UNavigationSystemV1::Build() — kicks off navmesh generation and returns immediately |
| `configure_nav_agent` | Modify a supported nav agent's properties (radius, height, step height) |
| `configure_nav_link` | Configure an existing ANavLinkProxy: enable/disable, set area class, direction |
| `create_nav_area` | Create a new UNavArea Blueprint with custom cost and color |
| `find_path` | FindPathSync between two points — returns path points, total cost, total distance |
| `get_crowd_manager_config` | Read UCrowdManager settings: max agents, max avoidance agents, avoidance config |
| `get_nav_build_status` | Check navigation build status: is building, tiles remaining, locked state |
| `get_nav_system_config` | Read UNavigationSystemV1 properties: enabled, allow client-side nav, abstract nav data, dirty areas update freq |
| `get_navmesh_config` | Full ARecastNavMesh generation params including multi-resolution settings (Low/Default/High), cell size, tile size, agent params |
| `get_navmesh_stats` | NavMesh statistics: tile count, nav data size, build status, registered nav bounds count |
| `get_random_navigable_point` | Get a random navigable point, optionally within a radius of an origin |
| `list_nav_areas` | List all UNavArea subclasses with their costs, colors, and flags |
| `list_nav_bounds_volumes` | Enumerate all ANavMeshBoundsVolume actors in the level |
| `list_nav_links` | Enumerate all ANavLinkProxy actors in the level |
| `navigation_raycast` | NavMesh line-of-sight test — returns whether the ray hits a nav boundary |
| `project_point_to_navigation` | Project a world point onto the navmesh surface — returns nearest navigable location |
| `set_crowd_manager_config` | Modify UCrowdManager settings: max agents, max agent radius, avoidance counts, intervals, separation, collision resolution |
| `set_navmesh_config` | Modify ARecastNavMesh generation params (agent radius/height, cell size, tile size, multi-resolution) |
| `test_path` | Fast reachability test between two nav points — returns bool |

### Scaffold (23)

| Action | Purpose |
|--------|---------|
| `batch_validate_ai_assets` | Run all validators across AI assets — BTs, STs, EQS, SOs, controllers. If 'asset_paths' is provided, validates only those exact paths. Otherwise scans the project (filtered by 'path_filter' if set). |
| `create_bt_from_template` | Create a Behavior Tree from a named template with standard BB keys |
| `create_st_from_template` | Create a State Tree from a named template (requires StateTree plugin) |
| `hello_world_ai` | ONE-CALL onboarding: creates Character BP + Controller + BT (patrol 3 waypoints) + BB + Perception (sight+hearing) + team. Returns all paths. |
| `scaffold_ai_controller_blueprint` | Full AI controller setup in one call: create controller BP + link BT/BB + perception + team |
| `scaffold_ambient_npc` | Ambient civilian NPC with Smart Object interactions and wander behavior. |
| `scaffold_boss_ai` | Multi-phase boss AI with health-threshold phase transitions. Full AI stack with phase-aware BT. |
| `scaffold_companion_ai` | Friendly companion AI: follows player, optionally fights alongside. Full Character+Controller+BT+BB+Perception stack. |
| `scaffold_complete_ai_character` | Full AI stack: Character BP + Controller + BT + BB + Perception + Team, all wired together |
| `scaffold_enemy_ai` | Basic enemy scaffold with chase+attack behavior. Archetype determines BT structure. |
| `scaffold_eqs_move_sequence` | Convenience: add a RunEQS→store→MoveTo sequence to an existing BT |
| `scaffold_flying_ai` | Flying AI with 3D navigation and altitude management. Uses flying movement mode. |
| `scaffold_group_coordinator` | Squad coordinator AI that assigns tactical roles (flanker, suppressor, rusher) to group members. |
| `scaffold_horror_ambush` | Horror ambush AI: dormant until triggered, burst attack, then retreat. Jump-scare archetype. |
| `scaffold_horror_mimic` | Horror mimic AI: disguises as a static object, attacks when player gets close. Classic mimic. |
| `scaffold_horror_presence` | Invisible horror presence: no physical form, manipulates environment (lights, doors, sounds). Psychological horror. |
| `scaffold_horror_stalker` | Horror stalker AI: follows player at distance, closes in during dark/vulnerability. For survival horror. |
| `scaffold_patrol_investigate_ai` | Guard AI scaffold: patrol→hear→investigate→search→return. Full Character+Controller+BT+BB+Perception stack. |
| `scaffold_perception_to_blackboard` | Wire perception events to blackboard keys (creates BB keys if needed). Note: configures perception senses and BB keys for the perception→BB bridge pattern. |
| `scaffold_stealth_game_ai` | Stealth game AI with detection meter and multi-state alert cascade (unaware→suspicious→searching→alert→combat). |
| `scaffold_team_system` | Full team setup: create team attitude DataTable with specified teams and attitudes |
| `scaffold_turret_ai` | Stationary turret AI with detection cone and engagement range. Does not move. |
| `validate_ai_controller` | Validate an AI controller: check BT/BB refs, perception configured, team set |

### Mass Zone Graph (22)

| Action | Purpose |
|--------|---------|
| `despawn_mass_spawner` | Report MassSpawner despawn availability; direct despawn is guarded until PIE/editor-world tests exist. |
| `find_nearest_zone_lane` | Return the nearest ZoneGraph lane from overlapping candidates. |
| `find_overlapping_zone_lanes` | Delegate to ai.query_zone_lanes when ZoneGraph support is registered. |
| `get_crowd_lane_state` | Report MassCrowd lane-state availability. |
| `get_mass_simulation_status` | Report Mass/ZoneGraph module availability, world context, and discovered runtime actors. |
| `get_mass_spawner` | Inspect one MassSpawner-like actor by label, name, or path. |
| `get_zone_shape` | Inspect one ZoneShape/ZoneGraph-like actor. |
| `list_mass_spawners` | List loaded MassSpawner-like actors in an explicit editor or PIE world context. |
| `list_zone_lane_profiles` | Return conservative lane-profile discovery metadata. |
| `list_zone_shapes` | List loaded ZoneShape/ZoneGraph-like actors. |
| `list_zone_tags` | Return tag names found on loaded ZoneShape/ZoneGraph-like actors. |
| `pause_mass_simulation` | Report Mass simulation pause availability; refuses ambiguous editor-vs-PIE state. |
| `rebuild_zone_graph` | Report ZoneGraph rebuild availability without starting long-running rebuilds. |
| `remove_zone_shape` | Report ZoneShape removal availability without mutating levels. |
| `resume_mass_simulation` | Report Mass simulation resume availability; refuses ambiguous editor-vs-PIE state. |
| `set_crowd_lane_state` | Report MassCrowd lane-state mutation availability. |
| `set_mass_spawner_count` | Report MassSpawner count mutation availability without mutating runtime state. |
| `set_mass_spawner_scale` | Report MassSpawner scale mutation availability without mutating runtime state. |
| `set_zone_shape_points` | Report ZoneShape point mutation availability without mutating levels. |
| `set_zone_shape_tags` | Report ZoneShape tag mutation availability without mutating levels. |
| `spawn_mass_spawner` | Report MassSpawner spawn availability; direct spawning is guarded until class-specific tests exist. |
| `spawn_zone_shape` | Report ZoneShape spawn availability without mutating levels. |

### EQS (20)

| Action | Purpose |
|--------|---------|
| `add_eqs_generator` | Add a new option with a generator to an EQS query |
| `add_eqs_test` | Add a test to an EQS query option |
| `build_eqs_query_from_spec` | Declarative full-query creation from a JSON spec with options, generators, and tests |
| `configure_eqs_filter` | Configure filter on a test: filter type, min/max, bool match |
| `configure_eqs_generator` | Set properties on a generator in an EQS query option |
| `configure_eqs_scoring` | Configure scoring on a test: purpose, equation, factor, clamp, normalization |
| `configure_eqs_test` | Set properties on a test in an EQS query option |
| `create_eqs_from_template` | Create an EQS query from a preset template: find_cover, find_flank, find_patrol_point, find_nearest_item |
| `create_eqs_query` | Create an empty UEnvQuery data asset |
| `delete_eqs_query` | Delete an EQS query asset |
| `duplicate_eqs_query` | Deep copy an EQS query asset to a new path |
| `get_eqs_query` | Full JSON: options, generators, tests, scoring config |
| `list_eqs_contexts` | List all available EQS context classes |
| `list_eqs_generator_types` | List all available EQS generator classes |
| `list_eqs_queries` | List all UEnvQuery assets in the project |
| `list_eqs_test_types` | List all available EQS test classes |
| `remove_eqs_generator` | Remove an option (generator + its tests) at the given index |
| `remove_eqs_test` | Remove a test from an EQS query option |
| `reorder_eqs_tests` | Reorder tests within an EQS query option |
| `validate_eqs_query` | Validate an EQS query: check empty options, missing contexts, item type mismatches |

### Smart Object (16)

| Action | Purpose |
|--------|---------|
| `add_smart_object_component` | Add USmartObjectComponent to an actor Blueprint via SCS |
| `add_so_behavior_definition` | Add a behavior definition to a Smart Object slot |
| `add_so_slot` | Add a slot to a Smart Object definition |
| `configure_so_slot` | Edit properties of an existing slot on a Smart Object definition |
| `create_smart_object_definition` | Create a new USmartObjectDefinition data asset |
| `create_so_from_template` | Create a Smart Object definition from a preset template (hide_spot, sit_chair, workstation, door_interaction, pickup_item) |
| `delete_smart_object_definition` | Delete a Smart Object Definition asset |
| `duplicate_smart_object_definition` | Deep copy a Smart Object definition to a new path |
| `find_smart_objects_in_level` | List all placed Smart Object components in the current level |
| `get_smart_object_definition` | Full dump of a Smart Object definition: slots, tags, behaviors, shapes |
| `list_smart_object_definitions` | List all USmartObjectDefinition assets in the project |
| `place_smart_object_actor` | Spawn an actor with USmartObjectComponent in the current level |
| `remove_so_behavior_definition` | Remove a behavior definition from a Smart Object slot |
| `remove_so_slot` | Remove a slot from a Smart Object definition by index |
| `set_so_tags` | Set definition-level activity tags and user tag filter on a Smart Object |
| `validate_smart_object_definition` | Validate a Smart Object definition: slots have behaviors, tags valid, etc. |

### Runtime (14)

| Action | Purpose |
|--------|---------|
| `runtime_check_perception` | Check if a target actor is perceived by an observer, and by which senses (PIE only) |
| `runtime_clear_bb_value` | Clear a Blackboard key on a running AI (PIE only) |
| `runtime_find_smart_objects` | Find available Smart Object slots near an actor (PIE only) |
| `runtime_get_bb_value` | Read a Blackboard value from a running AI's blackboard component (PIE only) |
| `runtime_get_bt_execution_path` | Snapshot of BT execution: active branch, current task, decorator states (PIE only) |
| `runtime_get_bt_state` | Get BehaviorTree runtime state: active node, tree status, pending aborts (PIE only) |
| `runtime_get_perceived_actors` | List all actors currently perceived by an AI's perception component (PIE only) |
| `runtime_get_st_active_states` | Get active StateTree states and task statuses (PIE only) |
| `runtime_report_noise` | Fire a noise event at a location for AI hearing (PIE only) |
| `runtime_run_eqs_query` | Run an EQS query synchronously and return scored results (PIE only) |
| `runtime_send_st_event` | Send an FStateTreeEvent to a running StateTree component (PIE only) |
| `runtime_set_bb_value` | Write a Blackboard value on a running AI (PIE only) |
| `runtime_start_bt` | Start or restart a BehaviorTree on an AI controller (PIE only) |
| `runtime_stop_bt` | Stop BehaviorTree execution on an AI controller (PIE only) |

### Advanced (12)

| Action | Purpose |
|--------|---------|
| `add_mass_trait` | Add a trait to a MassEntityConfigAsset |
| `create_mass_entity_config` | Create a new MassEntityConfigAsset |
| `get_mass_entity_config` | Inspect a MassEntityConfigAsset: traits, fragments, parent config |
| `get_mass_entity_stats` | Get runtime MassEntity statistics: archetype counts, entity totals (requires PIE) |
| `get_zone_lane_info` | Get detailed info about a specific zone graph lane by handle index |
| `list_mass_entity_configs` | List all UMassEntityConfigAsset assets in the project |
| `list_mass_processors` | List all registered UMassProcessor subclasses |
| `list_mass_traits` | List all available UMassEntityTraitBase subclasses |
| `list_zone_graphs` | Enumerate ZoneGraphData actors in loaded levels |
| `query_zone_lanes` | Spatial query for zone graph lanes near a world location |
| `remove_mass_trait` | Remove a trait from a MassEntityConfigAsset |
| `validate_mass_entity_config` | Validate a MassEntityConfigAsset: check trait compatibility, missing fragments, duplicates |

### Blackboard (12)

| Action | Purpose |
|--------|---------|
| `add_bb_key` | Add a key to a Blackboard (Bool/Int/Float/String/Name/Vector/Rotator/Object/Class/Enum/NativeEnum) |
| `batch_add_bb_keys` | Add multiple keys to a Blackboard at once |
| `compare_blackboards` | Diff two Blackboards: added, removed, and changed keys |
| `create_blackboard` | Create a new Blackboard Data asset |
| `delete_blackboard` | Delete a Blackboard Data asset |
| `duplicate_blackboard` | Deep copy a Blackboard Data asset to a new path |
| `get_bb_key_details` | Detailed info for a single blackboard key (type, base class filter, allowed types, etc.) |
| `get_blackboard` | Full JSON dump of all blackboard keys with types; inherited keys marked |
| `list_blackboards` | List all UBlackboardData assets in the project |
| `remove_bb_key` | Remove a key from a Blackboard |
| `rename_bb_key` | Rename a key in a Blackboard |
| `set_bb_parent` | Set or change the parent Blackboard for key inheritance |

### Discovery (11)

| Action | Purpose |
|--------|---------|
| `detect_ai_circular_references` | Check BT RunBehavior chains, ST linked assets, BB parent chains for circular references |
| `export_ai_manifest` | Full project AI manifest with cross-references — JSON or Markdown table |
| `find_eqs_references` | Find which BTs, STs, and Blueprints reference a given EQS query |
| `find_so_references` | Find which BTs, STs, and levels reference a given Smart Object definition |
| `get_ai_behavior_summary` | Structured JSON summary of a BT or ST: flow paths, decision points, key BB dependencies |
| `get_ai_overview` | Scan project: count BTs, BBs, STs, EQS queries, Smart Objects, AI Controllers |
| `lint_behavior_tree` | Style lint a Behavior Tree: unreachable branches, redundant decorators, single-child composites, unnamed nodes |
| `lint_state_tree` | Style lint a State Tree: no-task states, self-transitions without delay, dead-end states |
| `list_ai_node_types` | Unified type discovery for AI systems — enumerate available node classes |
| `search_ai_assets` | Full-text search across AI asset names |
| `validate_ai_data_flow` | Trace full data flow for an AI Controller: BB keys vs BT refs, EQS->BB, Perception config |

### Perception (11)

| Action | Purpose |
|--------|---------|
| `add_perception_component` | Add UAIPerceptionComponent to an AI controller Blueprint via SCS |
| `add_stimuli_source_component` | Add UAIPerceptionStimuliSourceComponent to any actor Blueprint |
| `configure_damage_sense` | Configure damage sense on a perception component (creates if absent). Reflection-writes to UAISenseConfig_Damage + base UAISenseConfig (MaxAge, bStartsEnabled). |
| `configure_hearing_sense` | Configure hearing sense on a perception component (creates if absent). Reflection-writes to UAISenseConfig_Hearing + base UAISenseConfig (MaxAge, bStartsEnabled). |
| `configure_sight_sense` | Configure sight sense on a perception component (creates if absent). Reflection-writes to UAISenseConfig_Sight + base UAISenseConfig (MaxAge, bStartsEnabled). |
| `configure_stimuli_source` | Configure which senses a stimuli source component registers for |
| `configure_touch_sense` | Configure touch sense on a perception component (creates if absent). Reflection-writes to UAISenseConfig_Touch + base UAISenseConfig (MaxAge, bStartsEnabled). Note: UAISenseConfig_Touch has no Implementation UPROPERTY in UE 5.7 — class is hardcoded. |
| `get_ai_system_config` | Read UAISystem global settings (perception aging, stimulus limits, etc.) |
| `get_perception_config` | Read all perception senses, params, and affiliation filters from an AI controller Blueprint |
| `remove_sense` | Remove a sense configuration from a perception component |
| `validate_perception_setup` | Validate perception setup: senses configured, affiliation set, dominant sense assigned |

### Controller (10)

| Action | Purpose |
|--------|---------|
| `create_ai_controller` | Create an AAIController Blueprint, optionally setting default BT and BB |
| `get_ai_actors` | List all AI-controlled actors in the current PIE world |
| `get_ai_controller` | Read AI controller config: default BT, BB, perception setup, flags |
| `get_ai_team` | Read the generic team ID from an AI controller CDO |
| `list_ai_controllers` | List all AAIController Blueprint assets in the project |
| `set_ai_controller_bt` | Set default Behavior Tree and optionally Blackboard on an AI controller CDO |
| `set_ai_controller_flags` | Set boolean config flags on an AI controller CDO (wants_player_state, start_ai_on_possess, etc.) |
| `set_ai_team` | Set the generic team ID (0-254) on an AI controller CDO for affiliation-based perception |
| `set_pawn_ai_controller_class` | Set AIControllerClass on a Pawn or Character Blueprint CDO |
| `spawn_ai_actor` | Spawn an AI pawn/character Blueprint actor in the editor level |

### Perception Scaffold (1)

| Action | Purpose |
|--------|---------|
| `add_perception_to_actor` | Add UAIPerceptionComponent to ANY Actor BP (not just AIControllers) and configure senses in one call. Senses v1: Sight, Hearing, Damage. Optional sight_radius (default 1500) for Sight, hearing_range (default 3000) for Hearing. |

## Navigating 243 actions

This is the largest namespace. **Lead with `monolith_find("<what you want>")`** to jump straight to the right action, then `monolith_discover({ namespace: "ai", action: "...", mode: "schema" })` for params. The group headings above map to engine subsystems: Behavior Tree, StateTree, Blackboard, EQS, Navigation, Smart Object, Mass/ZoneGraph, Perception, Controller, Runtime, and Scaffold (one-call AI authoring).

## Related skills

- Place / inspect AI actors and build navmesh in the level: `unreal-scene` (`scene.build_navmesh`)
- Abilities/attributes the AI uses: `unreal-gas`
- Blueprint logic backing the AI: `unreal-blueprints`

## Typical workflows

- **Scaffold a new AI fast:** `scaffold_complete_ai_character` / `scaffold_boss_ai` / `scaffold_companion_ai` / `scaffold_ambient_npc` (one call wires Controller + BT + Blackboard) — or `hello_world_ai` to smoke-test.
- **Author a Behavior Tree:** `create_bt_from_template` → add composites / tasks / decorators / services → bind blackboard keys.
- **Author a StateTree:** `create_st_from_template` → add states / transitions / tasks / conditions.
- **Perception:** `add_perception_to_actor` (Sight / Hearing / Damage) → debug stimuli at runtime.
- **Validate:** `batch_validate_ai_assets` before committing generated AI assets.

## Gotchas

- Behavior Tree / StateTree edits operate on the **asset**; save/recompile as the schema specifies.
- Navigation/path queries need a built navmesh — run `scene.build_navmesh` first.
- Runtime AI actions require a PIE/game session; authoring/scaffold actions work in-editor.

## Notes

- This reference is generated from the live `RegisterAction` surface. If an action is missing or renamed, re-run `monolith_discover({ namespace: "ai" })` — the catalog is the source of truth.
- Pass `mode: "schema"` to `monolith_discover` for required/optional params and types before calling an action.
