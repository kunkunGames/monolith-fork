# unreal-ai — Behavior Tree & StateTree actions

Action names + params are a snapshot of the live `ai` registry. Always confirm exact params with
`monolith_discover({ namespace: "ai", action: "<action>", mode: "schema" })`.

**Param notation:** `name*` required, `name?` optional, `name=val` default, `a/b/c` allowed values, `[w]` mutates (wraps a transaction). Signatures are a snapshot of the live catalog — for the exact, full, current schema of any action call `monolith_discover` with `mode: "schema"`.

## State Tree (35)

| Action | Purpose | Params (req* opt? =default) |
|--------|---------|-----------------------------|
| `add_st_consideration` | Add a utility consideration to a state | `[w] asset_path*, state_id*, consideration_class*, properties?` |
| `add_st_enter_condition` | Add an enter condition to a state | `[w] asset_path*, state_id*, condition_class*, properties?` |
| `add_st_extension` | Add an extension to a StateTree asset | `[w] asset_path*, extension_class*, properties?` |
| `add_st_property_binding` | Wire a property binding between source and target paths | `[w] asset_path*, source_path*, target_path*` |
| `add_st_state` | Add a state. Omit parent_state_id (or null) to add at root (SubTree). Bad parent_state_id errors. | `[w] asset_path*, name*, parent_state_id?, type?, selection_behavior?, linked_asset_path?` |
| `add_st_task` | Add a task (FInstancedStruct) to a state | `[w] asset_path*, state_id*, task_class*, properties?` |
| `add_st_transition` | Add a transition to a state | `[w] asset_path*, state_id*, trigger*, target_state*, priority?, delay?` |
| `add_st_transition_condition` | Add a condition to an existing transition on a state | `[w] asset_path*, state_id*, transition_index*, condition_class*, properties?` |
| `auto_arrange_st` | Auto-layout a StateTree graph (built-in formatter; blueprint_assist disabled) | `[w] asset_path*, formatter?` |
| `build_state_tree_from_spec` | Declarative full-tree creation from a JSON spec: states, tasks, transitions, then compiles | `[w] save_path*, spec*, strict_mode=false` |
| `compile_state_tree` | Compile a StateTree via UStateTreeEditingSubsystem. MANDATORY after any edits. | `[w] asset_path*` |
| `configure_st_consideration` | Configure a consideration's properties on a state (by index) | `[w] asset_path*, state_id*, consideration_index*, properties?, instance_properties?` |
| `create_state_tree` | Create a new UStateTree asset. Optionally set schema class. | `[w] save_path*, name?, schema_class?` |
| `delete_state_tree` | Delete a StateTree asset | `[w] asset_path*` |
| `duplicate_state_tree` | Deep copy a StateTree asset to a new path | `[w] source_path*, dest_path*` |
| `export_st_spec` | Export a StateTree as a JSON spec for build_state_tree_from_spec | `[w] asset_path*` |
| `generate_st_diagram` | Generate a Mermaid state diagram from a StateTree | `[w] asset_path*, format?` |
| `get_st_bindable_properties` | List available bindable properties, optionally scoped to a state/task | `asset_path*, state_id?, task_index?` |
| `get_st_bindings` | List all property bindings in a StateTree | `asset_path*` |
| `get_state_tree` | Full tree structure as JSON: states (recursive), tasks, conditions, transitions, considerations | `asset_path*` |
| `list_st_condition_types` | List all available StateTree condition struct types | `(none)` |
| `list_st_extension_types` | List all available UStateTreeExtension subclasses | `(none)` |
| `list_st_task_types` | List all available FStateTreeTaskBase subclasses | `(none)` |
| `list_state_trees` | List all UStateTree assets in the project | `path_filter?` |
| `move_st_state` | Reparent a state under a different parent state | `[w] asset_path*, state_id*, new_parent_id*, index?` |
| `remove_st_enter_condition` | Remove an enter condition from a state by index | `[w] asset_path*, state_id*, condition_index*` |
| `remove_st_property_binding` | Remove a property binding by index | `[w] asset_path*, binding_index*` |
| `remove_st_state` | Remove a state and its children from the StateTree | `[w] asset_path*, state_id*` |
| `remove_st_task` | Remove a task from a state by index | `[w] asset_path*, state_id*, task_index*` |
| `remove_st_transition` | Remove a transition from a state by index | `[w] asset_path*, state_id*, transition_index*` |
| `rename_st_state` | Rename a state in the StateTree | `[w] asset_path*, state_id*, new_name*` |
| `set_st_schema` | Set schema class and optional context actor class on a StateTree | `[w] asset_path*, schema_class*, context_actor_class?` |
| `set_st_state_properties` | Set state properties: weight, selection_behavior, tag, enabled | `[w] asset_path*, state_id*, weight?, selection_behavior?, tag?, enabled?` |
| `set_st_task_property` | Set a property on a task via ImportText_Direct reflection | `[w] asset_path*, state_id*, task_index*, property_name*, value*` |
| `validate_state_tree` | Validate a StateTree: unbound inputs, dead-end states, infinite loops, missing tasks | `asset_path*` |

State Tree enum hints: `add_st_transition` `trigger*` = OnStateCompleted/OnStateSucceeded/OnStateFailed/OnTick/OnEvent; `target_state*` = Succeeded/Failed/NextState/NextSelectableState or a state GUID; `priority?` = Low/Normal/Medium/High/Critical. `add_st_state` `type?` = State/Group/Linked/LinkedAsset/Subtree.

## Behavior Tree (32)

| Action | Purpose | Params (req* opt? =default) |
|--------|---------|-----------------------------|
| `add_bt_decorator` | Add a decorator as a sub-node on a target BT node | `[w] asset_path*, node_id*, decorator_class*, properties?` |
| `add_bt_node` | Add a composite or task node. parent_id=null adds under root. | `[w] asset_path*, node_class*, parent_id?, index?, properties?` |
| `add_bt_run_eqs_task` | Convenience: add a fully configured RunEQSQuery task node | `[w] asset_path*, eqs_path*, bb_result_key*, parent_id?, run_mode?` |
| `add_bt_service` | Add a service as a sub-node on a composite or task node | `[w] asset_path*, node_id*, service_class*, properties?` |
| `add_bt_smart_object_task` | Convenience: add a FindAndUseSmartObject task node | `[w] asset_path*, activity_tags*, parent_id?, search_radius?` |
| `add_bt_use_ability_task` | Convenience: add a TryActivateAbility task that fires a GAS ability on tick | `[w] asset_path*, parent_id?, ability_class?, ability_tags?, wait_for_end=true, succeed_on_blocked=false, event_tag?, node_name?` |
| `auto_arrange_bt` | Auto-layout a Behavior Tree graph (built-in layout; blueprint_assist disabled) | `[w] asset_path*, formatter?` |
| `build_behavior_tree_from_spec` | Create a complete Behavior Tree from a declarative JSON spec — the crown jewel | `[w] save_path*, spec*, strict_mode=false` |
| `clone_bt_subtree` | Deep-clone a subtree from one BT to another (decorators, services, properties) | `[w] source_path*, node_id*, dest_path*, dest_parent_id?` |
| `compare_behavior_trees` | Structural diff: nodes added/removed/moved, property changes | `path_a*, path_b*` |
| `create_behavior_tree` | Create a new Behavior Tree asset, optionally linking a Blackboard | `[w] save_path*, name?, blackboard_path?` |
| `create_bt_decorator_blueprint` | Create a BTDecorator Blueprint (parent defaults to BTDecorator_BlueprintBase) | `[w] save_path*, name*, parent_class?` |
| `create_bt_service_blueprint` | Create a BTService Blueprint (parent defaults to BTService_BlueprintBase) | `[w] save_path*, name*, parent_class?` |
| `create_bt_task_blueprint` | Create a BTTask Blueprint (parent defaults to BTTask_BlueprintBase) | `[w] save_path*, name*, parent_class?` |
| `delete_behavior_tree` | Delete a Behavior Tree asset | `[w] asset_path*` |
| `duplicate_behavior_tree` | Deep copy a Behavior Tree asset to a new path | `[w] source_path*, dest_path*` |
| `export_bt_spec` | Export an existing BT as a JSON spec (inverse of build_behavior_tree_from_spec) | `[w] asset_path*` |
| `generate_bt_diagram` | Generate a text diagram of a BT (`format?` = ascii (default) / mermaid) | `[w] asset_path*, format?` |
| `get_behavior_tree` | Full tree structure as JSON — nodes, decorators, services, hierarchy from root | `asset_path*` |
| `get_bt_graph` | Flat node array with parent_id + children GUIDs (look up a node by ID without walking) | `asset_path*` |
| `get_bt_node_properties` | Read all UPROPERTYs from a BT node instance | `asset_path*, node_id*` |
| `import_bt_spec` | Recreate a BT from an exported spec (overwrites existing structure) | `[w] asset_path*, spec*` |
| `list_behavior_trees` | List all UBehaviorTree assets in the project | `path_filter?` |
| `list_bt_node_classes` | List BT node classes (`category?` = composite/task/decorator/service) | `category?` |
| `move_bt_node` | Reparent a node under a different composite | `[w] asset_path*, node_id*, new_parent_id*, index?` |
| `remove_bt_decorator` | Remove a decorator from a BT node by index | `[w] asset_path*, node_id*, decorator_index*` |
| `remove_bt_node` | Remove a node and its children from a Behavior Tree | `[w] asset_path*, node_id*` |
| `remove_bt_service` | Remove a service from a BT node by index | `[w] asset_path*, node_id*, service_index*` |
| `reorder_bt_children` | Reorder child nodes under a composite by new GUID order | `[w] asset_path*, parent_id*, new_order*` |
| `set_bt_blackboard` | Set/change the Blackboard reference on a BT (empty string clears) | `[w] asset_path*, blackboard_path*` |
| `set_bt_node_property` | Set a UPROPERTY on a BT node (special handling for FBlackboardKeySelector) | `[w] asset_path*, node_id*, property_name*, value*` |
| `validate_behavior_tree` | Validate a BT: BB key refs, unreachable branches, empty composites, missing properties | `asset_path*` |

## Blackboard (12)

| Action | Purpose | Params (req* opt? =default) |
|--------|---------|-----------------------------|
| `add_bb_key` | Add a key (`key_type*` = Bool/Int/Float/String/Name/Vector/Rotator/Object/Class/Enum/NativeEnum) | `[w] asset_path*, key_name*, key_type*, description?, base_class?, enum_type?, instance_synced=false` |
| `batch_add_bb_keys` | Add multiple keys at once (`keys*` = array of {name,type,description?,base_class?,enum_type?}) | `[w] asset_path*, keys*` |
| `compare_blackboards` | Diff two Blackboards: added, removed, and changed keys | `path_a*, path_b*` |
| `create_blackboard` | Create a new Blackboard Data asset | `[w] save_path*, name?, parent_bb?` |
| `delete_blackboard` | Delete a Blackboard Data asset | `[w] asset_path*` |
| `duplicate_blackboard` | Deep copy a Blackboard Data asset to a new path | `[w] source_path*, dest_path*` |
| `get_bb_key_details` | Detailed info for a single key (type, base class filter, allowed types) | `asset_path*, key_name*` |
| `get_blackboard` | Full JSON dump of all keys with types; inherited keys marked | `asset_path*` |
| `list_blackboards` | List all UBlackboardData assets in the project | `path_filter?` |
| `remove_bb_key` | Remove a key from a Blackboard | `[w] asset_path*, key_name*` |
| `rename_bb_key` | Rename a key in a Blackboard | `[w] asset_path*, old_name*, new_name*` |
| `set_bb_parent` | Set/change the parent Blackboard for key inheritance (empty parent_path clears) | `[w] asset_path*, parent_path*` |
