# Niagara Action Reference (full parameter signatures)

Detailed per-action parameter signatures for the Monolith **niagara** namespace, called via `niagara_query({ action, params })`.

**Param notation:** `name*` required, `name?` optional, `name=val` default, `a/b/c` allowed values, `[w]` mutates (transaction-wrapped write). Signatures are a snapshot of the live catalog — for the exact full schema call `monolith_discover({ namespace: "niagara", action: "<action>", mode: "schema" })`. The discover-first block in `../SKILL.md` is the authority; do not call from this snapshot alone if param names, aliases, or ranges are load-bearing.

> **Not-yet-implemented (Phase 0 stubs):** `create_stateless_emitter`, `get_system_timing`, `set_warmup_profile`, `set_fixed_tick_delta`, `set_require_current_frame_data` are registered but described in the catalog as "Phase 0 stub … Not yet implemented." Their signatures are listed for completeness; confirm availability with discover before relying on them.

## System Management
| Action | Params | Purpose |
|--------|--------|---------|
| [w] `create_system` | `save_path*`, `template?` | Create a new Niagara system (`save_path`, NOT `asset_path`) |
| [w] `add_emitter` | `asset_path*`, `emitter_asset*`, `name?` | Add an emitter from an emitter asset path (`emitter_asset` is the source asset, `name` overrides the handle name) |
| [w] `remove_emitter` | `asset_path*`, `emitter*` | Remove an emitter by name |
| [w] `duplicate_emitter` | `asset_path*`, `source_emitter*`, `new_name?` | Duplicate an emitter (`source_emitter`, NOT `emitter`) |
| [w] `create_emitter` | `asset_path*`, `name*`, `sim_target?` | Add minimal empty emitter. `sim_target`: cpu/gpu (default cpu) |
| [w] `set_emitter_enabled` | `asset_path*`, `emitter*`, `enabled*` | Enable/disable an emitter |
| [w] `reorder_emitters` | `asset_path*`, `order*` | `order`: array of emitter names in desired order |
| [w] `set_emitter_property` | `asset_path*`, `emitter*`, `property*`, `value*` | Modify emitter settings |
| [w] `duplicate_system` | `asset_path*`, `save_path*` | Clone entire system asset to `save_path` |
| [w] `set_fixed_bounds` | `asset_path*`, `min*`, `max*`, `emitter?`, `enabled?` | `min`/`max`: `[x,y,z]` arrays. Omit `emitter` for system-level. `enabled` default true (false re-enables dynamic) |
| [w] `set_effect_type` | `asset_path*`, `effect_type*` | `effect_type`: asset path, or `none` to clear |
| [w] `export_system_spec` | `asset_path*`, `include_values?` | `include_values` default true. Reverse-engineer to create_system_from_spec JSON |
| `get_system_property` | `asset_path*`, `property*` | `property` aliases: warmup_time/determinism/random_seed/max_pool_size/etc. |
| [w] `set_system_property` | `asset_path*`, `property*`, `value*` | `property`: WarmupTime/WarmupTickCount/WarmupTickDelta/bFixedTickDelta/FixedTickDeltaTime/bDeterminism/RandomSeed/bSupportLargeWorldCoordinates/bNeedsSortedSignificanceHandling/SignificanceHandlerLink/MaxPoolSize |
| [w] `request_compile` | `asset_path*`, `force=false`, `synchronous=false` | Force recompile the system |
| `get_module_graph` | `script_path*` | Get a module SCRIPT's node graph (`script_path` only — NOT asset_path/emitter/module_node) |

## Read / Inspection
| Action | Params | Purpose |
|--------|--------|---------|
| `list_emitters` | `asset_path*` | List all emitters (name, index, enabled, sim_target, renderer_count, GUID) |
| `list_renderers` | `asset_path*`, `emitter*` | List renderers (type, index, enabled, material) |
| `list_module_scripts` | `search?`, `usage?`, `limit?`, `include_metadata?` | `usage`: module/dynamic_input/function (default module). `limit` default 50. `include_metadata` default false (slower) |
| `list_systems` | `search?`, `path?`, `limit?` | `path`: content path filter. `limit` default 50 |
| `list_renderer_properties` | `asset_path*`, `emitter*`, `renderer_index*` | List editable properties on a renderer (`renderer_index`, NOT `renderer`) |
| `list_emitter_properties` | `asset_path*`, `emitter*` | Reflection-based discovery of set_emitter_property keys |
| `list_available_renderers` | (none) | List all available Niagara renderer types with descriptions |
| `get_ordered_modules` | `asset_path*`, `emitter*`, `usage?`, `stage_name?`, `usage_id?`, `stage_index?`, `handler_index?` | Modules with GUIDs + `usage` stage field. `usage` e.g. particle_update/particle_event/emitter_spawn/particle_simulation_stage; selector params target event/sim-stage scripts |
| `get_system_diagnostics` | `asset_path*`, `compile_first?` | `compile_first` default true (forces synchronous compile) |
| `get_system_summary` | `asset_path*`, `detail_level?` | `detail_level`: compact (default)/full |
| `get_emitter_summary` | `asset_path*`, `emitter*`, `detail_level?` | `detail_level`: compact (default)/full. `emitter` accepts name or GUID |
| `get_module_input_value` | `asset_path*`, `emitter*`, `module_node*`, `input*` | `input` bare or `Module.` prefixed |
| `get_module_script_inputs` | `script_path*` | Pre-add introspection of a module script's inputs |
| `validate_system` | `asset_path*` | Pre-compile validation incl. event-chain/spawn-location/persistent-id checks |
| `diff_systems` | `asset_path_a*`, `asset_path_b*`, `detail_level?` | `detail_level`: summary/full (default full) |

## Search / Discovery
Search/discovery actions validate JSON types and ranges before running. `limit` is range-checked 1..1000; `threshold` 0.0..1.0. Loaders (`search_by_data_interface`, `search_by_material`) load each system — `limit` + `folder` are cost governors.

| Action | Params | Purpose |
|--------|--------|---------|
| `search_by_parameter` | `parameter_name*`, `parameter_type?`, `folder?`, `limit?` | Find systems exposing a user parameter (case-insensitive partial). `limit` default 50 |
| `search_by_data_interface` | `di_class*`, `folder?`, `limit?` | Find systems using a DI class (substring). `limit` default 50 |
| `query_niagara` | `query_string*`, `folder?`, `limit?` | DSL: `emitters>N`/`emitters=N`, `sim_target=GPU/CPU`, `has_renderer=<name>`, joined by AND/comma. Strict integer parsing. `limit` default 50 |
| `find_similar_systems` | `asset_path*` (alias `system_path`), `folder?`, `threshold?`, `limit?` | `threshold` 0..1 (default 0.5). `limit` default 10 |
| `search_by_material` | `material_path*`, `folder?`, `limit?` | Find systems whose renderers reference a material. `limit` default 50 |
| `find_niagara_references` | `asset_path*`, `limit?` | Asset Registry referencers of a Niagara asset. `limit` default 100 |
| `list_system_data_interfaces` | `asset_path*` (alias `system_path`) | DIs actually used by one system |

## Module Editing
| Action | Params | Purpose |
|--------|--------|---------|
| `get_module_inputs` | `asset_path*`, `emitter*`, `module_node*` | List all inputs on a module (returns short names) |
| [w] `add_module` | `asset_path*`, `emitter*`, `usage*`, `module_script*`, `stage_name?`, `usage_id?`, `stage_index?`, `handler_index?`, `index?` | `usage`: particle_spawn/particle_update/particle_event/emitter_update/particle_simulation_stage (REQUIRED). `module_script` is the script asset path. `index`: insert position |
| [w] `remove_module` | `asset_path*`, `emitter*`, `module_node*` | Remove a module |
| [w] `move_module` | `asset_path*`, `emitter*`, `module_node*`, `new_index*` | Reorder a module (preserves overrides) |
| [w] `set_module_enabled` | `asset_path*`, `emitter*`, `module_node*`, `enabled*` | Enable/disable a module |
| [w] `set_module_input_value` | `asset_path*`, `emitter*`, `module_node*`, `input*`, `value*` | Set a literal value (auto-detects CustomHlsl, uses pin defaults) |
| [w] `set_module_input_binding` | `asset_path*`, `emitter*`, `module_node*`, `input*`, `binding*` | Bind an input to a parameter |
| [w] `set_module_input_di` | `asset_path*`, `emitter*`, `module_node*`, `input*`, `di_class*`, `config?` | Set a DI on an input. `config`: DI configuration object. Auto-resolves DI class names |
| [w] `set_static_switch_value` | `asset_path*`, `emitter*`, `module_node*`, `input*`, `value*` | Static switch (`value`: true/false for bool, enum value name, integer) |
| [w] `clear_emitter_modules` | `asset_path*`, `emitter*`, `usage?` | `usage`: particle_update/particle_spawn/emitter_update/emitter_spawn/all (default all) |
| `get_static_switch_value` | `asset_path*`, `emitter*`, `module_node*`, `input?` | Omit `input` to list all switches |
| `get_module_output_parameters` | `asset_path*`, `emitter*`, `module_node*` | What attributes a module writes |

## Parameters
| Action | Params | Purpose |
|--------|--------|---------|
| `get_all_parameters` | `asset_path*`, `emitter?`, `scope?` | `scope`: User/ParticleSpawn/emitter name |
| `get_user_parameters` | `asset_path*` | User-exposed parameters |
| `get_available_parameters` | `asset_path*`, `emitter?`, `usage?` | All bindable parameters. `usage`: user/engine/particle/emitter/system/all (default all). Pass `emitter` to include particle/emitter-scoped attributes |
| `get_parameter_value` | `asset_path*`, `parameter*` | Get a parameter's current value |
| `get_parameter_type` | `type*` | Get info about a Niagara type (`type` is a type NAME — no asset_path) |
| [w] `trace_parameter_binding` | `asset_path*`, `parameter*` | Follow parameter binding chain |
| [w] `add_user_parameter` | `asset_path*`, `name*`, `type*`, `default?` | Add a user parameter |
| [w] `remove_user_parameter` | `asset_path*`, `name*` | Remove a user parameter |
| [w] `set_parameter_default` | `asset_path*`, `parameter*`, `value*` | Set parameter default |
| [w] `rename_user_parameter` | `asset_path*`, `old_name*`, `new_name*` | Rename + update bindings (with/without `User.` prefix; HLSL string refs NOT updated) |
| [w] `set_curve_value` | `asset_path*`, `emitter*`, `module_node*`, `input*`, `keys*` | Set curve keys on a plain float module input. `keys`: array of key objects |

## DI & Curve Configuration
| Action | Params | Purpose |
|--------|--------|---------|
| [w] `configure_curve_keys` | `asset_path*`, `emitter*`, `module_node*`, `input*`, `keys*`, `interp?` | Curve DI keys. `keys`: float `[{time,value}]` / color `[{time,r,g,b,a}]` / vector `[{time,x,y,z}]`. `interp`: linear/cubic/constant (default cubic) |
| [w] `configure_data_interface` | `asset_path*`, `emitter*`, `module_node*`, `input*`, `properties*` | `properties`: name-value object set via reflection |

## Dynamic Inputs
| Action | Params | Purpose |
|--------|--------|---------|
| [w] `add_dynamic_input` | `asset_path*`, `emitter*`, `module_node*`, `input*`, `dynamic_input_script*` | Attach dynamic input script to a pin. Returns node GUID + inputs |
| [w] `set_dynamic_input_value` | `asset_path*`, `emitter*`, `dynamic_input_node*`, `input*`, `value*` | Set value on a dynamic input node |
| `search_dynamic_inputs` | `query?`, `input_type?`, `limit?` | `input_type`: float/LinearColor/Vector. `limit` default 20 |
| `list_dynamic_inputs` | `asset_path*`, `emitter*`, `module_node*` | List dynamic inputs attached to a module |
| `get_dynamic_input_tree` | `asset_path*`, `emitter*`, `module_node*`, `max_depth?` | Recursive input tree. `max_depth` default 10 |
| [w] `remove_dynamic_input` | `asset_path*`, `emitter*`, `module_node?`, `input?`, `dynamic_input_node?` | Pass `module_node`+`input` OR `dynamic_input_node` |
| `get_dynamic_input_value` | `asset_path*`, `emitter*`, `dynamic_input_node*`, `input*` | Read a value on a dynamic input sub-pin |
| `get_dynamic_input_inputs` | `script_path*` | Discover inputs on an unattached dynamic input script |

## Event Handlers
| Action | Params | Purpose |
|--------|--------|---------|
| [w] `add_event_handler` | `asset_path*`, `emitter*`, `event_name*`, `source_emitter?`, `execution_mode?`, `max_events_per_frame?`, `spawn_number?` | `event_name`: CollisionEvent/DeathEvent/LocationEvent. `source_emitter` required for inter-emitter (unresolved=rejected). `execution_mode`: every_particle/spawned_particles/single_particle (default every_particle). `max_events_per_frame` default 0=unlimited. Does NOT auto-add Receive<Event> modules |
| `get_event_handlers` | `asset_path*`, `emitter*` | Read all handlers with full properties |
| [w] `set_event_handler_property` | `asset_path*`, `emitter*`, `property*`, `value*`, `handler_index?`, `usage_id?` | `property`: ExecutionMode/SpawnNumber/MaxEventsPerFrame/SourceEventName/bRandomSpawnNumber/MinSpawnNumber/UpdateAttributeInitialValues. Select with `handler_index` OR `usage_id` |
| [w] `remove_event_handler` | `asset_path*`, `emitter*`, `handler_index?`, `usage_id?` | Select with `handler_index` OR `usage_id` |

## Simulation Stages
| Action | Params | Purpose |
|--------|--------|---------|
| [w] `add_simulation_stage` | `asset_path*`, `emitter*`, `name*`, `iteration_source?`, `num_iterations?` | `iteration_source`: particles/data_interface (default particles). `num_iterations` default 1 |
| `get_simulation_stages` | `asset_path*`, `emitter*` | Read all sim stages with properties |
| [w] `set_simulation_stage_property` | `asset_path*`, `emitter*`, `property*`, `value*`, `stage_index?`, `stage_name?` | Select stage with `stage_index` OR `stage_name` |
| [w] `remove_simulation_stage` | `asset_path*`, `emitter*`, `stage_index?`, `stage_name?` | Select with `stage_index` OR `stage_name` |

## Temporal Control
Composite, intent-named writers over EmitterState / InitializeParticle.

| Action | Params | Purpose |
|--------|--------|---------|
| `get_system_timing` | `asset_path*` | **Phase 0 stub (not yet implemented).** Bundle read of warmup/tick fields |
| [w] `set_warmup_profile` | `asset_path*`, `warmup_time*`, `warmup_tick_delta?` | **Phase 0 stub.** `warmup_tick_delta` default ~1/15s |
| [w] `set_fixed_tick_delta` | `asset_path*`, `enabled*`, `fixed_delta_time?` | **Phase 0 stub.** `fixed_delta_time` default 1/60s when enabled |
| [w] `set_require_current_frame_data` | `asset_path*`, `require*` | **Phase 0 stub.** Param is `require` (bool), NOT `enabled` |
| [w] `set_emitter_loop_profile` | `asset_path*`, `emitter?`, `loop_behavior?`, `loop_duration_mode?`, `loop_duration?`, `loop_delay?`, `loop_count?`, `loop_delay_enabled?` | `loop_behavior`: Once/Multiple/Infinite. **Stateless-aware:** standalone stateless asset → omit `emitter` (response `stateless: true`). `loop_duration_mode` (Fixed/Infinite) is stateless-only. Provide at least one payload field |
| `get_emitter_timing_summary` | `asset_path*`, `emitter?` | Aggregator: loop topology + sim stages + InitializeParticle lifetime. Omit `emitter` for all. Stateless-aware |
| [w] `set_sim_stage_iteration_count` | `asset_path*`, `emitter*`, `iterations*`, `stage_index?`, `stage_name?` | Alias for NumIterations. Param is `iterations`. Provide exactly one of `stage_index`/`stage_name` |
| [w] `set_sim_stage_execute_behavior` | `asset_path*`, `emitter*`, `behavior*`, `stage_index?`, `stage_name?` | Param is `behavior`: Always/OnSimulationReset/NotOnSimulationReset. One of `stage_index`/`stage_name` |
| [w] `set_particle_lifetime` | `asset_path*`, `emitter*`, `min*`, `max?` | `min` only → Direct constant; `min`+`max` → Random mode |

## Stateless Emitters
`UNiagaraStatelessEmitter` (Lightweight Emitter) is a standalone asset; pass its path directly to the stateless-aware actions (`set_emitter_loop_profile`, `get_emitter_timing_summary`).

| Action | Params | Purpose |
|--------|--------|---------|
| [w] `create_stateless_emitter` | `save_path*` | **Phase 0 stub (not yet implemented).** Create a standalone Lightweight Emitter |

## NPC (Niagara Parameter Collections)
| Action | Params | Purpose |
|--------|--------|---------|
| [w] `create_npc` | `save_path*`, `namespace*` | Create an NPC asset. `namespace`: FName |
| `get_npc` | `asset_path*` | Read NPC parameters and defaults |
| [w] `add_npc_parameter` | `asset_path*`, `name*`, `type*` | `type`: float/int/bool/vec2/vec3/vec4/color/position |
| [w] `remove_npc_parameter` | `asset_path*`, `name*` | Remove parameter from NPC |
| [w] `set_npc_default` | `asset_path*`, `name*`, `value*` | `value`: number/bool/`{x,y,z}`/`{r,g,b,a}` |

## Effect Types & Scalability
| Action | Params | Purpose |
|--------|--------|---------|
| [w] `create_effect_type` | `save_path*`, `cull_reaction?`, `update_frequency?`, `max_distance?` | `cull_reaction`: Deactivate/DeactivateImmediate/DeactivateResume/PauseResume (default Deactivate). `update_frequency`: Continuous/Low/Medium/High. `max_distance` default 0=no limit |
| `get_effect_type` | `asset_path*` | Read all scalability settings |
| [w] `set_effect_type_property` | `asset_path*`, `property*`, `value*` | `property`: CullReaction/UpdateFrequency/SignificanceHandler/etc. |
| `get_scalability_settings` | `asset_path*` | Read per-quality-level configs |
| [w] `set_scalability_settings` | `asset_path*`, `settings*` | `settings`: array `[{quality_levels:[…], max_distance, max_instances, max_system_instances}]` |

## Renderers
| Action | Params | Purpose |
|--------|--------|---------|
| [w] `add_renderer` | `asset_path*`, `emitter*`, `class*` | `class`: Sprite/Mesh/Ribbon/etc. (param is `class`, NOT `type`) |
| [w] `remove_renderer` | `asset_path*`, `emitter*`, `renderer_index*` | `renderer_index` (NOT `renderer`) |
| [w] `set_renderer_material` | `asset_path*`, `emitter*`, `renderer_index*`, `material*` | `renderer_index` + `material` asset path |
| [w] `set_renderer_property` | `asset_path*`, `emitter*`, `renderer_index*`, `property*`, `value*` | `renderer_index` (NOT `renderer`) |
| `get_renderer_bindings` | `asset_path*`, `emitter*`, `renderer_index*` | Get a renderer's attribute bindings |
| [w] `set_renderer_binding` | `asset_path*`, `emitter*`, `renderer_index*`, `binding_name*`, `attribute*` | `binding_name` = binding property; `attribute` = particle attribute to bind (NOT `binding`/`value`) |
| [w] `set_renderer_mesh` | `asset_path*`, `emitter*`, `renderer_index*`, `mesh*`, `mesh_index?`, `scale?`, `rotation?`, `pivot_offset?` | Assign StaticMesh to a MeshRenderer slot. `mesh_index` default 0. `scale`/`rotation`/`pivot_offset` objects |
| [w] `configure_ribbon` | `asset_path*`, `emitter*`, `renderer_index*`, `preset?`, `facing_mode?`, `shape?`, `tessellation_mode?`, `tessellation_factor?`, `tube_subdivisions?`, `uv_mode?`, `tiling_length?`, `width_binding?`, `link_order_binding?`, `ribbon_id_binding?` | `preset`: trail/beam/lightning/tube. `shape`: Plane/MultiPlane/Tube/Custom. `tessellation_factor` 1-16; `tube_subdivisions` 3-16 |
| [w] `configure_subuv` | `asset_path*`, `emitter*`, `renderer_index*`, `columns*`, `rows*`, `blend?`, `add_animation_module?`, `playback_mode?`, `start_frame?`, `end_frame?` | SubUV/flipbook on a sprite renderer. `blend` default false; `add_animation_module` default false |

## Composite Helpers
| Action | Params | Purpose |
|--------|--------|---------|
| [w] `set_spawn_shape` | `asset_path*`, `emitter*`, `shape*`, `params?`, `replace_existing?` | `shape`: Cylinder/Sphere/Box/Cone/Torus. `params`: shape object (radius, height, surface_only…). `replace_existing` default true |

## Advanced
| Action | Params | Purpose |
|--------|--------|---------|
| [w] `rename_emitter` | `asset_path*`, `emitter*`, `name*` | Rename an emitter (`emitter` accepts name or handle ID) |
| `get_emitter_property` | `asset_path*`, `emitter*`, `property*` | `property` aliases: sim_target/local_space/determinism/bounds_mode/random_seed/allocation_mode/pre_allocation_count/requires_persistent_ids/max_gpu_particles_spawn_per_frame |
| [w] `save_emitter_as_template` | `asset_path*`, `emitter*`, `save_path*` | Extract emitter to standalone asset |
| [w] `clone_module_overrides` | `asset_path*`, `source_emitter*`, `source_module*`, `target_emitter*`, `target_module*` | Copy overrides between modules (same script) |
| [w] `duplicate_module` | `asset_path*`, `source_emitter*`, `source_module_node*`, `target_emitter?`, `target_usage?`, `target_stage_name?`, `usage_id?`, `stage_index?`, `target_index?` | Duplicate a module with overrides. `target_emitter`/`target_usage` default to source |
| `get_emitter_parent` | `asset_path*`, `emitter*` | Parent emitter asset path (read-only) |
| `preview_system` | `asset_path*`, `seek_time?`, `resolution?`, `output_path?`, `camera_angle?`, `background_color?`, `auto_fit?` | `seek_time` default 1.0. `resolution` WxH (default 512x512). `camera_angle`: front/top/three_quarter/side. `background_color`: `[R,G,B,A]` 0-1. `auto_fit` default true |
| [w] `save_system` | `asset_path*`, `only_if_dirty?` | `only_if_dirty` default true. Saves any Niagara asset |
| [w] `auto_layout` | `asset_path*`, `emitter?`, `script_usage?`, `formatter=auto` | **Experimental.** Auto-layout via Blueprint Assist (asset must be open). `script_usage`: system/emitter/particle. `formatter`: auto/blueprint_assist (monolith unsupported for Niagara) |
| [w] `audit_cross_asset_refs` | `limit=50`, `cursor?` | Audit Niagara systems whose native class edge points at a class missing from reflect_uclasses. `limit` cap 200; `cursor` pagination |

## Batch & Spec Operations
| Action | Params | Purpose |
|--------|--------|---------|
| [w] `batch_execute` | `asset_path*`, `operations*` | `operations`: array of operation objects (param is `operations`, NOT `commands`) |
| [w] `create_system_from_spec` | `save_path*`, `spec*` | Create a full system from JSON. Requires `save_path` AND `spec` (emitters/modules/renderers/user_parameters) |
| [w] `import_system_spec` | `asset_path*`, `spec*`, `mode?` | Overwrite existing system. `mode`: overwrite/merge |

## Data Interface & HLSL
| Action | Params | Purpose |
|--------|--------|---------|
| `get_di_functions` | `di_class*` | List functions on a DI class |
| `get_di_properties` | `di_class*` | Editable properties + signatures via CDO reflection |
| `get_compiled_gpu_hlsl` | `asset_path*`, `emitter*` | Compiled GPU HLSL for an emitter (auto-compiles if needed) |

## Custom HLSL Module/Function Creation
| Action | Params | Purpose |
|--------|--------|---------|
| [w] `create_module_from_hlsl` | `name*`, `save_path*`, `hlsl*`, `inputs?`, `outputs?`, `description?` | Standalone module from custom HLSL. `inputs`/`outputs`: array of `{name,type}`. Strict type validation (unknown types hard-fail) |
| [w] `create_function_from_hlsl` | `name*`, `save_path*`, `hlsl*`, `inputs?`, `outputs?`, `description?` | Reusable function from custom HLSL (same validation) |
| `get_custom_hlsl_text` | `script_path*`, `node_guid?` | Read HLSL from a CustomHlsl node. `node_guid` disambiguates multi-node scripts |
| [w] `set_custom_hlsl_text` | `script_path*`, `hlsl*`, `node_guid?` | Overwrite a CustomHlsl node under Modify()+transaction with recompile |

**Input/output format:** `[{"name": "InValue", "type": "float"}, {"name": "Velocity", "type": "vec3"}]`
**Supported types:** `float`, `int`, `bool`, `vec2`, `vec3`, `vec4`, `color`, `position`, `quat`, `matrix`

**CRITICAL: Before writing custom HLSL, read [`../../../Docs/NIAGARA_HLSL_GUIDE.md`](../../../Docs/NIAGARA_HLSL_GUIDE.md) for the complete body rules.**

### HLSL body rules (summary)
1. Use bare input/output names (e.g. `InValue` / `OutValue`, NOT `Module.InValue`) — the compiler generates `In_X` / `Out_X` internally.
2. Custom HLSL is injected inside an existing function body — global vars, bare functions, namespaces, and `::` static calls are invalid.
3. Functions must be wrapped in a struct; instantiate the struct explicitly before calling methods.
4. Struct methods cannot reach outer-scope variables — pass all inputs, constants, and Data Interfaces explicitly as parameters.
5. **GPU ONLY:** in GPU simulation you CAN directly read/write `Particles.Velocity`, `Particles.Position`, etc., but MUST wrap in `#if GPU_SIMULATION ... #endif`. CPU simulation does NOT support `Particles.*` — use output parameters instead.
6. CAN access Data Interface functions if a DI is passed as input (e.g. a Grid3D input enables `SamplePreviousGridVector3Value`).
7. Grid attribute names are strings — `"Velocity"` in a Grid API call is a channel name, NOT an output variable reference.
8. DI sampling behaves like GPU resource access — `SamplePrevious*` reads previous-frame state, not current-frame writes.
9. Wrap all GPU-only APIs with `#if GPU_SIMULATION ... #endif`.
10. Assign output defaults BEFORE any `#if GPU_SIMULATION` block so the CPU fallback path stays valid.
11. Niagara HLSL requires strict type matching — avoid implicit casts between `float` / `float2` / `float3` / `int` / `bool`.
12. Use unique struct names (avoid generic `FMath`) to prevent generated-shader symbol collisions across Custom nodes.
- Can't swizzle ParameterMap variables directly (`Particles.Color.xyz` is one token) — assign to a local first: `float4 C = Particles.Color;`
