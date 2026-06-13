# unreal-ai — Navigation, EQS & Smart Object actions

Action names + params are a snapshot of the live `ai` registry. Always confirm exact params with
`monolith_discover({ namespace: "ai", action: "<action>", mode: "schema" })`.

**Param notation:** `name*` required, `name?` optional, `name=val` default, `a/b/c` allowed values, `[w]` mutates (wraps a transaction). Signatures are a snapshot of the live catalog — for the exact, full, current schema of any action call `monolith_discover` with `mode: "schema"`.

For one-off navmesh-path / raycast queries on live actors prefer the `unreal-scene` skill;
these `ai` navigation actions configure AI navigation data and nav volumes.

## Navigation (26)

| Action | Purpose | Params (req* opt? =default) |
|--------|---------|-----------------------------|
| `add_nav_bounds_volume` | Spawn ANavMeshBoundsVolume at a location with given extents | `[w] location*, extent*, folder_path?` |
| `add_nav_invoker_component` | Add UNavigationInvokerComponent for dynamic nav generation around the actor | `[w] blueprint_path*, generation_radius?, removal_radius?` |
| `add_nav_link_proxy` | Spawn ANavLinkProxy connecting two points (`link_type?` = point (default) / smart) | `[w] start_location*, end_location*, link_type?, area_class?, folder_path?` |
| `add_nav_modifier_volume` | Spawn ANavModifierVolume overriding the nav area class within its bounds | `[w] location*, extent*, area_class*, folder_path?` |
| `analyze_navigation_coverage` | Grid-sample + project to navmesh — coverage %, gaps, stats | `sample_spacing?, bounds?` |
| `build_navigation` | Trigger UNavigationSystemV1::Build() — kicks off generation, returns immediately | `[w] (none)` |
| `configure_nav_agent` | Modify a supported nav agent (radius, height, step height) by SupportedAgents index | `[w] agent_index*, radius?, height?, step_height?` |
| `configure_nav_link` | Configure an existing ANavLinkProxy (`direction?` = both / left_to_right / right_to_left) | `[w] actor_path*, enabled?, area_class?, direction?` |
| `create_nav_area` | Create a new UNavArea Blueprint with custom cost and color | `[w] save_path*, name*, default_cost?, fixed_area_entering_cost?, color?` |
| `find_path` | FindPathSync between two points — returns path points, total cost, total distance | `start*, end*` |
| `get_crowd_manager_config` | Read UCrowdManager settings: max agents, max avoidance agents, avoidance config | `(none)` |
| `get_nav_build_status` | Check navigation build status: is building, tiles remaining, locked state | `(none)` |
| `get_nav_system_config` | Read UNavigationSystemV1 properties (enabled, client-side nav, dirty-area freq) | `(none)` |
| `get_navmesh_config` | Full ARecastNavMesh generation params incl. multi-resolution (Low/Default/High) | `(none)` |
| `get_navmesh_stats` | NavMesh stats: tile count, nav data size, build status, registered bounds count | `(none)` |
| `get_random_navigable_point` | Random navigable point (radius required if origin given) | `origin?, radius?` |
| `list_nav_areas` | List all UNavArea subclasses with costs, colors, flags | `(none)` |
| `list_nav_bounds_volumes` | Enumerate all ANavMeshBoundsVolume actors in the level | `(none)` |
| `list_nav_links` | Enumerate all ANavLinkProxy actors in the level | `level?` |
| `navigation_raycast` | NavMesh line-of-sight test — whether the ray hits a nav boundary | `[w] start*, end*` |
| `project_point_to_navigation` | Project a world point onto the navmesh (`extent?` default 50,50,250) | `[w] point*, extent?` |
| `rebuild_navigation` | Rebuild current world nav, wait for async tiles, optionally save packages | `[w] save_after?, timeout_seconds?` |
| `set_crowd_manager_config` | Modify UCrowdManager: max agents/radius, avoidance counts, intervals, separation | `[w] max_agents?, max_avoidance_agents?, max_agent_radius?, max_avoided_walls?, navmesh_check_interval?, path_optimization_interval?, separation_dir_clamp?, path_offset_radius_multiplier?, resolve_collisions?` |
| `set_navmesh_config` | Modify ARecastNavMesh gen params (agent radius/height, cell/tile size, multi-res) | `[w] agent_radius?, agent_height?, cell_size?, cell_height?, tile_size?, agent_max_slope?, agent_max_step_height?, resolution_params?` |
| `test_path` | Fast reachability test between two nav points — returns bool | `[w] start*, end*` |
| `validate_nav_points` | Project named points to navmesh + optionally confirm paths between named pairs | `points*, agent_class_or_actor?, require_path_pairs?, extent?` |

## EQS (20)

| Action | Purpose | Params (req* opt? =default) |
|--------|---------|-----------------------------|
| `add_eqs_generator` | Add a new option with a generator to an EQS query | `[w] asset_path*, generator_class*, properties?` |
| `add_eqs_test` | Add a test to an EQS query option | `[w] asset_path*, option_index*, test_class*, properties?` |
| `build_eqs_query_from_spec` | Declarative full-query creation from a JSON spec (options, generators, tests) | `[w] save_path*, spec*` |
| `configure_eqs_filter` | Configure filter on a test (`filter_type?` = Minimum/Maximum/Range/Match) | `[w] asset_path*, option_index*, test_index*, filter_type?, min?, max?, bool_value?` |
| `configure_eqs_generator` | Set properties on a generator in an EQS query option | `[w] asset_path*, option_index*, properties*` |
| `configure_eqs_scoring` | Configure scoring on a test (purpose, equation, factor, clamp, normalization) | `[w] asset_path*, option_index*, test_index*, purpose?, equation?, factor?, clamp_min_type?, clamp_max_type?, score_clamp_min?, score_clamp_max?, normalization_type?, define_reference_value?, reference_value?` |
| `configure_eqs_test` | Set properties on a test in an EQS query option | `[w] asset_path*, option_index*, test_index*, properties*` |
| `create_eqs_from_template` | Create from preset (`template*` = find_cover/find_flank/find_patrol_point/find_nearest_item) | `[w] save_path*, template*, properties?` |
| `create_eqs_query` | Create an empty UEnvQuery data asset | `[w] save_path*, name?` |
| `delete_eqs_query` | Delete an EQS query asset | `[w] asset_path*` |
| `duplicate_eqs_query` | Deep copy an EQS query asset to a new path | `[w] source_path*, dest_path*` |
| `get_eqs_query` | Full JSON: options, generators, tests, scoring config | `asset_path*` |
| `list_eqs_contexts` | List all available EQS context classes | `(none)` |
| `list_eqs_generator_types` | List all available EQS generator classes | `(none)` |
| `list_eqs_queries` | List all UEnvQuery assets in the project | `path_filter?` |
| `list_eqs_test_types` | List all available EQS test classes | `(none)` |
| `remove_eqs_generator` | Remove an option (generator + its tests) at the given index | `[w] asset_path*, option_index*` |
| `remove_eqs_test` | Remove a test from an EQS query option | `[w] asset_path*, option_index*, test_index*` |
| `reorder_eqs_tests` | Reorder tests within an EQS query option (`new_order*` = array of indices) | `[w] asset_path*, option_index*, new_order*` |
| `validate_eqs_query` | Validate: empty options, missing contexts, item type mismatches | `asset_path*` |

EQS scoring enum hints: `purpose?` = Filter/Score/FilterAndScore; `equation?` = Linear/InverseLinear/Square/Constant; `normalization_type?` = Absolute/RelativeToScores.

## Smart Object (16)

| Action | Purpose | Params (req* opt? =default) |
|--------|---------|-----------------------------|
| `add_smart_object_component` | Add USmartObjectComponent to an actor Blueprint via SCS | `[w] blueprint_path*, definition_path*` |
| `add_so_behavior_definition` | Add a behavior definition to a Smart Object slot | `[w] asset_path*, slot_index*, behavior_class*, properties?` |
| `add_so_slot` | Add a slot to a Smart Object definition | `[w] asset_path*, offset?, rotation?, activity_tags?, user_tags?, enabled?` |
| `configure_so_slot` | Edit properties of an existing slot | `[w] asset_path*, slot_index*, offset?, rotation?, activity_tags?, user_tags?, enabled?, slot_name?` |
| `create_smart_object_definition` | Create a new USmartObjectDefinition data asset | `[w] save_path*, name?` |
| `create_so_from_template` | Create from preset (`template*` = hide_spot/sit_chair/workstation/door_interaction/pickup_item) | `[w] save_path*, template*` |
| `delete_smart_object_definition` | Delete a Smart Object Definition asset | `[w] asset_path*` |
| `duplicate_smart_object_definition` | Deep copy a Smart Object definition to a new path | `[w] source_path*, dest_path*` |
| `find_smart_objects_in_level` | List all placed Smart Object components in the current level | `level?, definition_filter?, tag_filter?` |
| `get_smart_object_definition` | Full dump: slots, tags, behaviors, shapes | `asset_path*` |
| `list_smart_object_definitions` | List all USmartObjectDefinition assets in the project | `path_filter?` |
| `place_smart_object_actor` | Spawn an actor with USmartObjectComponent in the current level | `[w] definition_path*, location*, rotation?, folder_path?` |
| `remove_so_behavior_definition` | Remove a behavior definition from a Smart Object slot | `[w] asset_path*, slot_index*, behavior_index*` |
| `remove_so_slot` | Remove a slot from a Smart Object definition by index | `[w] asset_path*, slot_index*` |
| `set_so_tags` | Set definition-level activity tags and user tag filter | `[w] asset_path*, activity_tags?, user_tags?` |
| `validate_smart_object_definition` | Validate: slots have behaviors, tags valid, etc. | `asset_path*` |
