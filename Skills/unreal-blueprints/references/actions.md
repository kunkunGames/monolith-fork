# unreal-blueprints — Action Reference

Full per-action parameter signatures for the Monolith **blueprint** namespace, called via `blueprint_query({ action, params })`. These are a live-catalog snapshot — confirm the live action set and an action's exact schema via `monolith_discover({ namespace: "blueprint", action: "<name>", mode: "schema" })` (or `describe_query("action_schema", {namespace:"blueprint", action:"<name>"})`) before relying on any param name, alias, default, or range. The discover-first block in `../SKILL.md` is the authority.

**Param notation:** `name*` required, `name?` optional, `name=val` default, `a/b/c` allowed values, `[w]` mutates (transaction-wrapped). Signatures are a snapshot of the live catalog — for the exact, full, current schema of any action call `monolith_discover` with mode `schema` (or `describe_query("action_schema", {namespace:"blueprint", action:"<name>"})`).

### Read (19)

| Action | Params (req* opt? =default) | Purpose |
|--------|-----------|---------|
| `list_graphs` | `asset_path*` | All event/function/macro graphs |
| `get_graph_summary` | `asset_path*`, `graph_name?` | Node id/class/title + exec connections (all graphs when empty) |
| `get_graph_data` | `asset_path*`, `graph_name?`, `node_class_filter?` | Full topology: pins, connections, positions |
| `get_variables` | `asset_path*`, `include_bind_widgets=false` | Variables with types, defaults, replication; widget binds when set |
| `get_execution_flow` | `asset_path*`, `entry_point*` | Trace execution wires from an event/function |
| `search_nodes` | `asset_path*`, `query*` | Find by title / function name |
| `get_components` | `asset_path*` | Component hierarchy |
| `get_component_details` | `asset_path*`, `component_name*` | Full property dump (SCS + inherited native) |
| `get_functions` | `asset_path*` | Functions with I/O, metadata |
| `get_event_dispatchers` | `asset_path*` | Dispatchers with pins |
| `get_parent_class` | `asset_path*` | Parent, type, status |
| `get_interfaces` | `asset_path*` | Implemented interfaces |
| `get_construction_script` | `asset_path*` | Construction script nodes |
| `get_node_details` | `asset_path*`, `node_id*`, `graph_name?` | Full pin dump |
| `search_functions` | `query?`, `class_filter?`, `include_inherited=true`, `pure_only=false`, `limit=50`, `detail_level=minimal` (minimal/standard) | Find BP-callable functions (one of query/class_filter required) |
| `get_interface_functions` | `interface_class*` | Interface required functions |
| `get_function_signature` | `asset_path*`, `function_name*`, `include_inherited=false` | Inputs, outputs, flags, locals |
| `get_blueprint_info` | `asset_path*` | Comprehensive overview |
| `get_event_dispatcher_details` | `asset_path*`, `dispatcher_name*` | Signature + referencing nodes |

### CDO (4)

| Action | Params (req* opt? =default) | Purpose |
|--------|-----------|---------|
| `get_cdo_properties` | `asset_path*`, `category_filter?`, `include_parent_defaults=true`, `owner_class_filter?`, `name_pattern?`, `exclude_categories?` | Read UPROPERTY defaults |
| `describe_cdo_schema` | `asset_path*` | Legal types, ImportText forms, enum/clamp ranges before a CDO write |
| `set_cdo_property` [w] | `asset_path*`, `property_name*`, `value*`, `dry_run=false`, `strict=false` | Write one CDO property (ImportText) |
| `set_cdo_properties` [w] | `asset_path*`, `properties*`, `dry_run=false`, `strict=false` | Bulk-fill CDO props from a JSON tree |

### Discovery (1)

| Action | Params (req* opt? =default) | Purpose |
|--------|-----------|---------|
| `resolve_node` | `node_type*`, `function_name?`, `target_class?`, `variable_name?`, `replication?`, `reliable?`, `asset_path?`, `component_name?`, `delegate_property_name?` | Dry-run: returns resolved type + pins, no mutation |

### Variable CRUD (8)

| Action | Params (req* opt? =default) | Purpose |
|--------|-----------|---------|
| `add_variable` [w] | `asset_path*`, `name*`, `type*`, `default_value?`, `category?`, `instance_editable=true`, `blueprint_read_only=false`, `expose_on_spawn=false`, `replicated=false`, `transient=false` | Add member var |
| `remove_variable` [w] | `asset_path*`, `name*` | Remove member var |
| `rename_variable` [w] | `asset_path*`, `old_name*`, `new_name*` | Rename + update refs |
| `set_variable_type` [w] | `asset_path*`, `name*`, `type*` | Change type |
| `set_variable_defaults` [w] | `asset_path*`, `name*`, `default_value?`, `category?`, `instance_editable?`, `blueprint_read_only?`, `expose_on_spawn?`, `replicated?`, `transient?`, `save_game?` | Update metadata/flags (only provided fields) |
| `add_local_variable` [w] | `asset_path*`, `function_name*`, `name*`, `type*`, `default_value?` | Add function local |
| `remove_local_variable` [w] | `asset_path*`, `function_name*`, `name*` | Remove function local |
| `add_replicated_variable` [w] | `asset_path*`, `variable_name*`, `type*`, `replication_condition=None` (None/InitialOnly/OwnerOnly/SkipOwner/SimulatedOnly/AutonomousOnly/SimulatedOrPhysics/InitialOrOwner/Custom), `create_on_rep=false`, `default_value?`, `category?` | With optional OnRep stub |

### Component CRUD (7)

| Action | Params (req* opt? =default) | Purpose |
|--------|-----------|---------|
| `add_component` [w] | `asset_path*`, `component_class*`, `component_name?`, `parent?`, `attach_socket?` | Add SCS component |
| `remove_component` [w] | `asset_path*`, `component_name*`, `promote_children=true` | Remove |
| `rename_component` [w] | `asset_path*`, `component_name*`, `new_name*` | Rename |
| `reparent_component` [w] | `asset_path*`, `component_name*`, `new_parent*` (empty=root), `attach_socket?` | Reparent |
| `set_component_property` [w] | `asset_path*`, `component_name*`, `property_name*`, `value*` | Set via text import |
| `duplicate_component` [w] | `asset_path*`, `component_name*`, `new_name?` | Duplicate (same parent) |
| `add_engine_component_typed` [w] | `bp_path*`, `component_type*`, `component_name*` | Resolve UActorComponent subclass by friendly name + add |

### Graph Management (17)

| Action | Params (req* opt? =default) | Purpose |
|--------|-----------|---------|
| `add_function` [w] | `asset_path*`, `name*` (alias `function_name`), `is_pure=false`, `is_const=false`, `is_static=false`, `call_in_editor=false`, `category?`, `description?`, `access=Public` (Public/Protected/Private), `replication?` (none/multicast/server/client), `reliable?` | Create function |
| `set_function_thread_safe` [w] | `asset_path*`, `function_name*` (alias `name`), `thread_safe=true` | Set/clear Thread Safe flag |
| `override_parent_function` [w] | `asset_path*`, `parent_function_name*` | Override BlueprintImplementable/NativeEvent (incl. return-value) |
| `remove_function` [w] | `asset_path*`, `name*` | Remove function |
| `rename_function` [w] | `asset_path*`, `old_name*`, `new_name*` | Rename function |
| `add_macro` [w] | `asset_path*`, `name*` | Create macro |
| `remove_macro` [w] | `asset_path*`, `macro_name*` | Remove macro |
| `rename_macro` [w] | `asset_path*`, `old_name*`, `new_name*` | Rename macro |
| `add_event_dispatcher` [w] | `asset_path*`, `name*` | Create dispatcher |
| `remove_event_dispatcher` [w] | `asset_path*`, `dispatcher_name*` | Remove dispatcher |
| `set_event_dispatcher_params` [w] | `asset_path*`, `dispatcher_name*`, `params*` (`[{name,type}]`) | Replace signature |
| `set_function_params` [w] | `asset_path*`, `function_name*`, `inputs?`, `outputs?` (`[{name,type}]`) | Set signature |
| `implement_interface` [w] | `asset_path*`, `interface_class*` | Add interface (no stubs) |
| `remove_interface` [w] | `asset_path*`, `interface_class*`, `preserve_functions=false` | Remove interface |
| `scaffold_interface_implementation` [w] | `asset_path*`, `interface_class*` | Add interface + create stubs |
| `reparent_blueprint` [w] | `asset_path*`, `new_parent_class*` | Change parent |
| `add_property_access` [w] | `member_class*` (alias `target_class`), `member_name*`, `asset_path*`, `graph_name?`, `is_setter=false`, `position?` | VariableGet/Set on a FOREIGN class member (NOT thread-safe) |

### Node & Pin (9)

| Action | Params (req* opt? =default) | Purpose |
|--------|-----------|---------|
| `add_node` [w] | `asset_path*`, `node_type*`, `graph_name?`, `position?`, + type-specific (see below) | Add node |
| `remove_node` [w] | `asset_path*`, `node_id*`, `graph_name?` | Remove node |
| `connect_pins` [w] | `asset_path*`, `source_node*`, `source_pin*`, `target_node*`, `target_pin*`, `graph_name?` | Wire pins (case-insensitive) |
| `disconnect_pins` [w] | `asset_path*`, `node_id*`, `pin_name*`, `target_node?`, `target_pin?`, `graph_name?` | Break one or all connections on a pin |
| `set_pin_default` [w] | `asset_path*`, `node_id*`, `pin_name*`, `value*`, `graph_name?` | Set default (class/object pins accept native names or paths) |
| `set_node_position` [w] | `asset_path*`, `node_id*`, `position*` (`[x,y]`), `graph_name?` | Move node |
| `promote_pin_to_variable` [w] | `asset_path*`, `node_id*`, `pin_name*`, `variable_name?`, `graph_name?` | Scalar pin to member var (no containers) |
| `add_property_access_node` [w] | `asset_path*`, `path*` (string[]), `graph_name?` (aliases `target_graph`/`function_name`), `context_id?`, `position?` | Genuine thread-safe Property Access node (AnimBP) |

### Compile & Create (5)

| Action | Key Params | Purpose |
|--------|-----------|---------|
| `compile_blueprint` | `asset_path` | Errors include node_id + graph_name |
| `validate_blueprint` | `asset_path` | Lint: unused vars, disconnected nodes |
| `create_blueprint` | `save_path`, `parent_class`, `blueprint_type`? | Create new BP |
| `duplicate_blueprint` | `asset_path`, `new_path` | Duplicate |
| `get_dependencies` | `asset_path`, `direction`? | Asset deps |

### Timeline (4)

| Action | Key Params | Purpose |
|--------|-----------|---------|
| `add_timeline` | `asset_path`, `timeline_name`?, `auto_play`?, `loop`? | Create timeline |
| `get_timeline_data` | `asset_path`, `timeline_name`? | Read tracks, keys |
| `add_timeline_track` | `asset_path`, `timeline_name`, `track_name`, `track_type`? | float/vector/event/color track |
| `set_timeline_keys` | `asset_path`, `timeline_name`, `track_name`, `keys` | `[{time, value, interp_mode?}]` |

### Struct, Enum & DataTable (6)

| Action | Key Params | Purpose |
|--------|-----------|---------|
| `create_user_defined_struct` | `save_path`, `fields` | `[{name, type, default_value?}]` |
| `create_user_defined_enum` | `save_path`, `values` | `["Value1", "Value2"]` |
| `create_data_table` | `save_path`, `row_struct` | DataTable for struct |
| `create_data_asset` | `save_path`, `class_name`, `skip_save`? | Raw UObject (DataAssets, MPCs, etc.) |
| `add_data_table_row` | `asset_path`, `row_name`, `values` | `{column: value}` |
| `get_data_table_rows` | `asset_path`, `row_name`? | Read rows |

### Build from Spec (1)

| Action | Key Params | Purpose |
|--------|-----------|---------|
| `build_blueprint_from_spec` | `asset_path`, `graph_name`?, `variables`?, `components`?, `nodes`, `connections`?, `pin_defaults`?, `auto_compile`? | One-shot declarative builder |

### Graph Export (3)

| Action | Key Params | Purpose |
|--------|-----------|---------|
| `export_graph` | `asset_path`, `graph_name`? | Export to JSON (build_from_spec compatible) |
| `copy_nodes` | `source_asset`, `source_graph`, `node_ids`, `target_asset`, `target_graph` | Copy via T3D |
| `duplicate_graph` | `asset_path`, `graph_name`, `new_name` | Duplicate within BP |

### Diff, Template, Layout, Batch, Events (10)

| Action | Key Params | Purpose |
|--------|-----------|---------|
| `compare_blueprints` | `asset_path_a`, `asset_path_b` | Structural diff |
| `list_templates` | — | Available templates |
| `apply_template` | `template_name`, `asset_path`, `params`? | Apply template |
| `auto_layout` | `asset_path`, `graph_name`?, `layout_mode`?, `formatter`? | Auto-arrange. Modes: `all`/`new_only`/`selected`. Formatter: `monolith`/`blueprint_assist` |
| `add_event_node` | `asset_path`, `event_name`, `replication`?, `reliable`? | Parent or implemented-interface override; otherwise custom event with RPC |
| `add_comment_node` | `asset_path`, `text`, `node_ids`?, `color`? | Comment box |
| `batch_execute` | `asset_path`, `operations`, `compile_on_complete`? | Multiple ops, one round-trip |
| `add_nodes_bulk` | `asset_path`, `nodes` (with `temp_id`) | Place multiple, returns ID map |
| `connect_pins_bulk` | `asset_path`, `connections` | Wire multiple |
| `set_pin_defaults_bulk` | `asset_path`, `defaults` | Set multiple defaults |
