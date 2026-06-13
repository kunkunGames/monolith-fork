# GAS Action Reference (full parameter signatures)

Detailed per-action parameter signatures for the Monolith **gas** namespace, called via `gas_query({ action, params })`.

**Param notation:** `name*` required, `name?` optional, `name=val` default, `a/b/c` allowed values, `[w]` mutates (transaction-wrapped write). Signatures are a snapshot of the live catalog — for the exact full schema call `monolith_discover({ namespace: "gas", action: "<action>", mode: "schema" })` (or `gas_query({ action, mode: "schema" })`). The discover-first block in `../SKILL.md` is the authority; do not call from this snapshot alone if param names, aliases, or ranges are load-bearing.

**130+ GAS actions across 12 categories** — the tables below are a curated snapshot, not the full live catalog.

## Abilities (28)

| Action | Params | Purpose |
|--------|--------|---------|
| `create_ability` | `[w] save_path* parent_class=GameplayAbility display_name?` | Create GA Blueprint |
| `get_ability_info` | `asset_path*` | Read tags, costs, cooldowns, flags |
| `list_abilities` | `path_filter? tag_filter? parent_class_filter?` | List all GA assets |
| `compile_ability` | `[w] asset_path*` | Compile GA Blueprint |
| `set_ability_tags` | `[w] asset_path* container* tags* mode=set` | Set cancel/block/owned/required tags |
| `get_ability_tags` | `asset_path* container?` | Read all tag containers |
| `set_ability_policy` | `[w] asset_path* instancing_policy? net_execution_policy? net_security_policy?` | Set instancing/net policies |
| `set_ability_cost` | `[w] asset_path* cost_effect_class*` | Assign cost GE |
| `set_ability_cooldown` | `[w] asset_path* cooldown_effect_class*` | Assign cooldown GE |
| `set_ability_triggers` | `[w] asset_path* triggers*` | Configure trigger events |
| `set_ability_flags` | `[w] asset_path* replicate_input_directly? retrigger_instanced_ability? server_respects_remote_ability_cancellation?` | Server-only, retry-on-fail, etc. |
| `add_ability_task_node` | `[w] asset_path* task_class* factory_function? position?` | Add Ability Task node |
| `add_commit_and_end_flow` | `[w] asset_path* graph_name? position?` | Scaffold CommitAbility->logic->EndAbility |
| `add_effect_application` | `[w] asset_path* effect_class* target=self position?` | Add ApplyGE node |
| `add_gameplay_cue_node` | `[w] asset_path* cue_tag* type* position?` | Add ExecuteGameplayCue node |
| `create_ability_from_template` | `[w] save_path* template* overrides?` | Create from preset |
| `build_ability_from_spec` | `[w] save_path* spec*` | Declarative one-shot builder |
| `batch_create_abilities` | `[w] abilities*` | Create multiple at once |
| `duplicate_ability` | `[w] asset_path* new_path* rename_tags?` | Duplicate |
| `list_ability_tasks` | `category_filter?` | Available AT classes |
| `get_ability_task_pins` | `task_class*` | AT input/output pins |
| `wire_ability_task_delegate` | `[w] asset_path* node_id* delegate_name* target_node_id* target_pin=execute` | Wire AT delegate |
| `get_ability_graph_flow` | `asset_path* graph_name?` | Trace execution flow |
| `validate_ability` | `asset_path*` | Lint: missing cost, orphaned tasks, tag conflicts |
| `find_abilities_by_tag` | `tag* match_type=exact` | Find by tag |
| `get_ability_tag_matrix` | `asset_paths?` | Cross-reference tag usage |
| `validate_ability_blueprint` | `asset_path* release_input_supported=false` | Deep validation: graph, tasks, delegates |
| `scaffold_custom_ability_task` | `[w] class_name* parameters* delegates*` | Scaffold custom AT |

## Attributes (20)

| Action | Params | Purpose |
|--------|--------|---------|
| `create_attribute_set` | `[w] save_path* mode=blueprint parent_class? attributes?` | Create Attribute Set |
| `add_attribute` | `[w] attribute_set* name* default_value? replicated?` | Add attribute |
| `get_attribute_set` | `attribute_set*` | Read attributes, defaults, clamping |
| `set_attribute_defaults` | `[w] attribute_set* defaults*` | Set defaults |
| `list_attribute_sets` | `include_plugins?` | List all |
| `configure_attribute_clamping` | `[w] attribute_set* clamp_rules*` | Set clamping |
| `configure_meta_attributes` | `[w] attribute_set* meta_attributes*` | Meta attribute for damage/heal |
| `create_attribute_set_from_template` | `[w] template* save_path* mode? overrides?` | From preset |
| `create_attribute_init_datatable` | `[w] attribute_set* save_path* rows?` | DataTable for init |
| `duplicate_attribute_set` | `[w] source* save_path* remove_attributes? add_attributes?` | Duplicate |
| `configure_attribute_replication` | `[w] attribute_set* replication*` | Set replication |
| `link_datatable_to_asc` | `[w] asc_blueprint* entries*` | Link init DataTable to ASC |
| `bulk_edit_attributes` | `[w] operations*` | Edit multiple |
| `validate_attribute_set` | `attribute_set*` | Lint: orphans, clamping, replication |
| `find_attribute_modifiers` | `attribute* search_scope?` | Find all GEs modifying attribute |
| `diff_attribute_sets` | `set_a* set_b*` | Compare two sets |
| `get_attribute_dependency_graph` | `attribute_sets* format=json` | Map dependencies |
| `remove_attribute` | `[w] attribute_set* name* check_references=true` | Remove |
| `get_attribute_value` | `actor* attribute*` | Read runtime value (PIE) |
| `set_attribute_value` | `[w] actor* attribute* value* set_base?` | Set runtime value (PIE) |

## Effects (26)

| Action | Params | Purpose |
|--------|--------|---------|
| `create_gameplay_effect` | `[w] save_path* duration_policy* parent_class?` | Create GE |
| `get_gameplay_effect` | `asset_path*` | Read modifiers, duration, stacking, components |
| `list_gameplay_effects` | `path_filter? duration_policy? tag_filter? attribute_filter?` | List all |
| `add_modifier` | `[w] asset_path* attribute* operation* magnitude*` | Add modifier |
| `set_modifier` | `[w] asset_path* modifier_index* attribute? operation? magnitude?` | Edit modifier |
| `remove_modifier` | `[w] asset_path* modifier_index? attribute?` | Remove modifier |
| `list_modifiers` | `asset_path*` | List modifiers |
| `add_ge_component` | `[w] asset_path* component_type* config*` | Add GE Component (5.3+) |
| `set_ge_component` | `[w] asset_path* component_type* config* index?` | Edit GE Component |
| `remove_ge_component` | `[w] asset_path* component_type* index?` | Remove GE Component |
| `set_effect_stacking` | `[w] asset_path* stacking_type* stack_limit? stack_duration_refresh_policy? stack_period_reset_policy? stack_expiration_policy?` | Configure stacking |
| `set_duration` | `[w] asset_path* duration_policy* duration_magnitude?` | Set duration policy |
| `set_period` | `[w] asset_path* period* execute_on_application?` | Set periodic execution |
| `create_effect_from_template` | `[w] save_path* template* overrides?` | From preset |
| `build_effect_from_spec` | `[w] save_path* spec*` | Declarative one-shot builder |
| `batch_create_effects` | `[w] effects*` | Create multiple |
| `add_execution` | `[w] asset_path* calculation_class* scoped_modifiers?` | Add Execution Calculation |
| `duplicate_gameplay_effect` | `[w] source_path* dest_path* overrides?` | Duplicate |
| `delete_gameplay_effect` | `[w] asset_path* force?` | Delete |
| `validate_effect` | `asset_path*` | Lint: missing attrs, stacking conflicts |
| `get_effect_interaction_matrix` | `asset_paths?` | Cross-reference GE interactions |
| `get_active_effects` | `actor* filter_class? filter_tag?` | List active GEs (PIE) |
| `get_effect_modifiers_breakdown` | `actor* attribute*` | Per-attribute modifier breakdown (PIE) |
| `apply_effect` | `[w] actor* effect_class* level? set_by_caller?` | Apply at runtime (PIE) |
| `remove_effect` | `[w] actor* effect_handle? effect_class?` | Remove active GE (PIE) |
| `simulate_effect_stack` | `[w] attribute_state* effects*` | Simulate stacking without PIE |

## ASC Setup (14)

| Action | Params | Purpose |
|--------|--------|---------|
| `add_asc_to_actor` | `[w] asset_path* asc_class? location?` | Add ASC to Blueprint |
| `configure_asc` | `[w] asset_path* replication_mode? default_abilities? default_effects? default_attribute_sets?` | Configure ASC |
| `setup_asc_init` | `[w] asset_path* location*` | Scaffold InitAbilityActorInfo |
| `setup_ability_system_interface` | `[w] asset_path* asc_location* class_name? parent_class?` | IAbilitySystemInterface (C++ only) |
| `apply_asc_template` | `[w] actor_path* template* overrides?` | Apply ASC template (player/AI/boss) |
| `set_default_abilities` | `[w] actor_path* abilities* mode?` | Abilities granted on init |
| `set_default_effects` | `[w] actor_path* effects* mode?` | Effects applied on init |
| `set_default_attribute_sets` | `[w] actor_path* attribute_sets* mode?` | Attribute sets created on init |
| `set_asc_replication_mode` | `[w] actor_path* mode*` | Full/Minimal/Mixed |
| `validate_asc_setup` | `actor_path*` | Lint: missing interface, init, avatar |
| `grant_ability` | `[w] actor* ability_class* level? input_id?` | Grant at runtime (PIE) |
| `revoke_ability` | `[w] actor* ability_class*` | Revoke at runtime (PIE) |
| `get_asc_snapshot` | `actor* include_abilities? include_effects? include_attributes? include_tags? include_cooldowns?` | Full ASC state dump (PIE) |
| `get_all_ascs` | `class_filter? tag_filter?` | List all actors with ASCs |

## Tags (10)

| Action | Params | Purpose |
|--------|--------|---------|
| `add_gameplay_tags` | `[w] tags* table_path?` | Register tags (INI or DataTable) |
| `get_tag_hierarchy` | `root? depth? include_usage=false` | Display tag tree |
| `search_tag_usage` | `tag* match_type=exact` | Find all assets using tag |
| `scaffold_tag_hierarchy` | `[w] preset* save_path?` | Generate tag hierarchy from preset |
| `rename_tag` | `[w] old_tag* new_tag* dry_run=false` | Rename across all assets |
| `remove_gameplay_tags` | `[w] tags* check_references=true` | Remove from registry |
| `validate_tag_consistency` | `path_filter?` | Find orphan tags, naming violations |
| `audit_tag_naming` | `[w] conventions?` | Audit naming conventions |
| `export_tag_hierarchy` | `[w] format=json output_path?` | Export as JSON/CSV |
| `import_tag_hierarchy` | `[w] source_path* merge_mode=merge dry_run=false` | Import from file |

## Cues (10)

| Action | Params | Purpose |
|--------|--------|---------|
| `create_gameplay_cue_notify` | `[w] save_path* cue_tag* type=burst` | Create GCN (burst/looping/static) |
| `link_cue_to_effect` | `[w] effect_path* cue_tag*` | Add cue to GE |
| `unlink_cue_from_effect` | `[w] effect_path* cue_tag*` | Remove cue from GE |
| `get_cue_info` | `asset_path*` | Read cue details |
| `list_gameplay_cues` | `tag_prefix? type_filter=all` | List all cues |
| `set_cue_parameters` | `[w] asset_path* burst_particle? burst_sound? loop_particle? loop_sound? camera_shake?` | Configure cue params |
| `find_cue_triggers` | `cue_tag*` | Find GEs/abilities triggering cue |
| `validate_cue_coverage` | `path_filter? include_registered_tags_without_notifies=false` | Check for GEs missing GCN assets |
| `batch_create_cues` | `[w] cues*` | Create multiple |
| `scaffold_cue_library` | `[w] preset* save_path_prefix*` | Generate starter cue set |

## Targeting (5)

| Action | Params | Purpose |
|--------|--------|---------|
| `create_target_actor` | `[w] save_path* targeting_type* trace_channel? max_range? radius?` | Create TargetActor BP |
| `configure_target_actor` | `[w] asset_path* trace_channel? max_range? radius? start_location_type? should_produce_target_data_on_server? debug_draw?` | Set range, radius, shape |
| `add_targeting_to_ability` | `[w] ability_path* target_actor_class? confirm_type=instant` | Wire WaitTargetData task |
| `scaffold_fps_targeting` | `[w] ability_path* mode* range? radius? save_path?` | FPS line trace targeting |
| `validate_targeting` | `ability_path*` | Lint targeting setup |

## Input Binding (5)

| Action | Params | Purpose |
|--------|--------|---------|
| `setup_ability_input_binding` | `[w] actor_path* binding_mode*` | Configure binding approach |
| `bind_ability_to_input` | `[w] actor_path* ability_class* input_action* trigger_event=started` | Bind to Enhanced Input |
| `batch_bind_abilities` | `[w] actor_path* bindings*` | Bind multiple |
| `get_ability_input_bindings` | `actor_path*` | List bindings |
| `scaffold_input_binding_component` | `[w] actor_path* input_config*` | Scaffold binding component |

## Inspect / Debug (10)

| Action | Params | Purpose |
|--------|--------|---------|
| `snapshot_gas_state` | `[w] class_filter? include_modifiers=false` | Full runtime state by class (PIE) |
| `get_tag_state` | `actor*` | Current tag container (PIE) |
| `get_cooldown_state` | `actor*` | Active cooldowns (PIE) |
| `trace_ability_activation` | `[w] actor* ability_class*` | Activation trace (can/cannot + why) |
| `compare_gas_states` | `snapshot_a* snapshot_b*` | Diff two prior `snapshot_gas_state` outputs |
| `get_runtime_summary` | `class_filter? include_actor_samples=true max_actors=20` | Aggregate runtime ASC summary (PIE) |
| `start_event_cue_probe` | `[w] actor* event_tags? event_tag? cue_tags? cue_tag? max_events=128` | Begin recording gameplay events/cues (PIE) |
| `stop_event_cue_probe` | `[w] probe_id*` | Stop a running event/cue probe (PIE) |
| `expect_event_cue` | `[w] actor* event_tag? cue_tag? trigger_action? max_events=128` | Assert an event/cue fires (PIE) |
| `export_gas_manifest` | `[w] format=json include_relationships=true include_data_asset_profiles=true data_asset_path_filter? data_asset_profile? max_data_asset_profiles=500 output_path? path_filter?` | Full project GAS manifest |

## Scaffold / Bootstrap (7)

| Action | Params | Purpose |
|--------|--------|---------|
| `bootstrap_gas_foundation` | `[w] project_name?` | Full bootstrap: ASC, attrs, GEs, tags |
| `validate_gas_setup` | `(no params)` | Project-wide validation |
| `scaffold_gas_project` | `[w] preset* actor_paths?` | Scaffold complete project structure |
| `scaffold_damage_pipeline` | `[w] save_path* damage_types*` | Damage pipeline: meta attrs, exec calc |
| `scaffold_status_effect` | `[w] save_path* name* config*` | Status effect (DOT, buff, debuff) |
| `scaffold_weapon_ability` | `[w] save_path* weapon_type* fire_mode=single` | Weapon ability with targeting + cues |
| `grant_ability_to_pawn` | `[w] pawn_bp_path* ability_class_path* level=1 input_id=-1` | Author-time grant on a pawn Blueprint |

## Data Asset GAS Profile (3)

| Action | Params | Purpose |
|--------|--------|---------|
| `describe_data_asset_gas_profile` | `asset_path* profile?` | Inspect a data asset's GAS-shaped fields |
| `validate_data_asset_gas_profile` | `path_filter=/Game profile? include_content=true include_source_scan=true required_roles? max_assets=200` | Validate data-asset GAS roles/coverage |
| `set_data_asset_gas_fields` | `[w] asset_path* fields* profile? dry_run=true strict=true save=false` | Write GAS fields on a data asset (dry-run by default) |

## Widget Attribute Binding (5)

| Action | Params | Purpose |
|--------|--------|---------|
| `bind_widget_to_attribute` | `[w] wbp_path* widget_name* target_property* attribute* max_attribute? owner_resolver=owning_player_pawn format=auto update_policy=on_change replace_existing=true` | Bind a UMG widget property to an attribute |
| `unbind_widget_attribute` | `[w] wbp_path* widget_name* target_property*` | Remove one widget-attribute binding |
| `list_attribute_bindings` | `wbp_path*` | List widget-attribute bindings on a WBP |
| `clear_widget_attribute_bindings` | `[w] wbp_path*` | Remove all bindings on a WBP |
