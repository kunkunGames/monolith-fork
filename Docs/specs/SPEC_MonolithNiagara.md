# Monolith — MonolithNiagara Module

**Parent:** [SPEC_CORE.md](../SPEC_CORE.md)
**Engine:** Unreal Engine 5.7+
**Version:** 0.14.9 (Beta)

---

## MonolithNiagara

**Dependencies:** Core, CoreUObject, Engine, MonolithCore, UnrealEd, Niagara, NiagaraCore, NiagaraEditor, Json, JsonUtilities, AssetTools, Slate, SlateCore

### Classes

| Class | Responsibility |
|-------|---------------|
| `FMonolithNiagaraModule` | Registers 109 Niagara actions (108 baseline in `MonolithNiagaraActions.cpp` + 1 layout in `MonolithNiagaraLayoutActions.cpp`) |
| `FMonolithNiagaraActions` | Static handlers + extensive private helpers |
| `FMonolithNiagaraLayoutActions` | `auto_layout` Blueprint Assist bridge for Niagara graphs |
| `MonolithNiagaraHelpers` | 6 reimplemented NiagaraEditor functions (non-exported APIs) |

### Reimplemented NiagaraEditor Helpers

These exist because Epic's `FNiagaraStackGraphUtilities` functions lack `NIAGARAEDITOR_API`:

1. `GetOrderedModuleNodes` — Module execution order
2. `GetStackFunctionInputOverridePin` — Override pin lookup
3. `GetModuleIsEnabled` — Module enabled state
4. `RemoveModuleFromStack` — Module removal
5. `GetParametersForContext` — System user store params
6. `GetStackFunctionInputs` — Full input enumeration via engine's `FNiagaraStackGraphUtilities::GetStackFunctionInputs` with `FCompileConstantResolver`. Returns all input types (floats, vectors, colors, data interfaces, enums, bools) — not just static switch pins

### Actions (109 — namespace: "niagara")

> **Audit note (2026-04-26):** detailed per-category tables below sum to roughly 96 — the remainder are post-design-doc additions (NPC, effect types, scalability, layout, advanced query helpers) that have not yet been threaded into the per-category tables. The header count is the source-of-truth.

> **Note:** All Niagara actions accept `asset_path` (preferred) or `system_path` (backward compatible) for the system asset path parameter.
>
> **Input name conventions:** `get_module_inputs` returns short names (no `Module.` prefix). All write actions that accept input names (`set_module_input_value`, `set_module_input_binding`, `set_module_input_di`, `set_curve_value`) accept both short names and `Module.`-prefixed names. For CustomHlsl modules, `get_module_inputs` and `set_module_input_value` fall back to reading/writing the FunctionCall node's typed input pins directly (CustomHlsl inputs don't appear in the ParameterMap history).
>
> **Param name aliases:** The canonical param names registered in schemas are `module_node` and `input`. All module write actions also accept these aliases: `module_node` → `module_name`, `module`; `input` → `input_name`. Use the canonical names when possible — aliases exist for backward compatibility.
>
> **Emitter selector matching:** Niagara emitter selectors, including `auto_layout`, do NOT auto-select a single emitter when a specific non-matching name is passed. If a selector is provided it must resolve by GUID, exact/case-insensitive display name, unique instance name, or numeric index string (`"0"`, `"1"`, etc.).

**System (22)**
| Action | Description |
|--------|-------------|
| `add_emitter` | Add an emitter (UE 5.7: takes FGuid VersionGuid) |
| `remove_emitter` | Remove an emitter |
| `duplicate_emitter` | Duplicate an emitter within a system. Accepts `emitter` as alias for `source_emitter` |
| `set_emitter_enabled` | Enable/disable an emitter |
| `reorder_emitters` | Reorder emitters (direct handle assignment + PostEditChange + MarkPackageDirty for proper change notifications) |
| `set_emitter_property` | Set property: SimTarget, bLocalSpace, bDeterminism, RandomSeed, AllocationMode, PreAllocationCount, bRequiresPersistentIDs, MaxGPUParticlesSpawnPerFrame, CalculateBoundsMode |
| `set_system_property` | Set a system-level property (WarmupTime, bDeterminism, etc.) |
| `request_compile` | Request system compilation. Params: `force` (bool), synchronous (bool) |
| `create_system` | Create new system (blank or from template via DuplicateAsset) |
| `list_emitters` | List all emitters with name, index, enabled, sim_target, renderer_count, GUID |
| `list_renderers` | List all renderers across emitters with class (short `type` name), index, enabled, material |
| `list_module_scripts` | Search available Niagara module scripts by keyword. Returns matching script asset paths |
| `list_renderer_properties` | List editable properties on a renderer. Params: `asset_path`, `emitter`, `renderer` |
| `get_system_diagnostics` | Compile errors, warnings, renderer/SimTarget incompatibility, GPU+dynamic bounds warnings, per-script stats (op count, registers, compile status). Added 2026-03-13 |
| `duplicate_system` | Clone an entire Niagara system to a new path |
| `get_system_property` | Read a system-level property (WarmupTime, bDeterminism, RandomSeed, etc.) |
| `get_system_summary` | One-call overview of an entire Niagara system (emitters, params, renderers, module counts) |
| `list_systems` | Search/list Niagara system assets in the project |
| `save_system` | Save a Niagara asset (system, script, NPC, effect type) to disk |
| `set_effect_type` | Assign a UNiagaraEffectType for scalability and cull distance |
| `set_fixed_bounds` | Set explicit bounds on system or emitter for GPU performance |
| `validate_system` | Pre-compile validation: check for common misconfigurations |

**Module (17)**
| Action | Description |
|--------|-------------|
| `get_ordered_modules` | Get ordered modules in a script stage |
| `get_module_inputs` | Get all inputs (floats, vectors, colors, data interfaces, enums, bools) with override values, linked params, and actual DI curve data. Uses engine's `FNiagaraStackGraphUtilities::GetStackFunctionInputs`. Returns short names (no `Module.` prefix). LinearColor/vector defaults deserialized from JSON string if needed |
| `get_module_graph` | Node graph of a module script |
| `add_module` | Add module to script stage (uses FNiagaraStackGraphUtilities) |
| `remove_module` | Remove module from stack |
| `move_module` | Move module to new index (remove+re-add — **loses input overrides**) |
| `set_module_enabled` | Enable/disable a module |
| `duplicate_module` | Duplicate a module within or across emitters (copies script + all overrides) |
| `get_static_switch_value` | Get static switch value(s) on a module — omit input to list all switches |
| `set_module_input_value` | Set input value (float, int, bool, vec2/3/4, color, string) |
| `set_module_input_binding` | Bind input to a parameter |
| `set_module_input_di` | Set data interface on input. Required: `di_class` (class name — `U` prefix optional, e.g. `NiagaraDataInterfaceCurve` or `UNiagaraDataInterfaceCurve`), optional `config` object (supports FRichCurve keys for curve DIs). Validates input exists and is DataInterface type. Accepts both short names and `Module.`-prefixed names |
| `set_static_switch_value` | Set a static switch value on a module |
| `create_module_from_hlsl` | Create a Niagara module script from custom HLSL. Params: `name`, `save_path`, `hlsl` (body), optional `inputs[]`/`outputs[]` (`{name, type}` objects), `description`. **HLSL body rules:** use bare input/output names (no `Module.` prefix — compiler adds `In_`/`Out_` automatically). Write particle attributes via `Particles.X` ParameterMap tokens directly in the body. No swizzle via dot on map variables. |
| `create_function_from_hlsl` | Create a Niagara function script from custom HLSL. Same params as `create_module_from_hlsl`. Script usage is set to `Function` instead of `Module`. |
| `get_module_input_value` | Read the current override value for a specific module input |
| `get_module_script_inputs` | Introspect a module script's inputs WITHOUT adding it to an emitter |

**Parameter (10)**
| Action | Description |
|--------|-------------|
| `get_all_parameters` | All parameters (user + per-emitter rapid iteration) |
| `get_user_parameters` | User-exposed parameters only |
| `get_parameter_value` | Get a parameter value |
| `get_parameter_type` | Type info (size, is_float, is_DI, is_enum, struct) |
| `trace_parameter_binding` | Find all usage sites of a parameter |
| `add_user_parameter` | Add user parameter with optional default |
| `remove_user_parameter` | Remove a user parameter |
| `set_parameter_default` | Set parameter default value |
| `rename_user_parameter` | Rename a user parameter and update all module bindings that reference it |
| `set_curve_value` | Set curve keys on a module input. Params: `emitter`, `module_node`, `input`, `keys` (array of `{time, value}` objects) |

**Renderer (6)**
| Action | Description |
|--------|-------------|
| `add_renderer` | Add renderer (Sprite, Mesh, Ribbon, Light, Component) |
| `remove_renderer` | Remove a renderer |
| `set_renderer_material` | Set renderer material (per-type handling) |
| `set_renderer_property` | Set property via reflection (float, double, int, bool, string, enum, byte, object) |
| `get_renderer_bindings` | Get attribute bindings via reflection |
| `set_renderer_binding` | Set attribute binding (ImportText with fallback format) |

**Batch (7)**
| Action | Description |
|--------|-------------|
| `batch_execute` | Execute multiple operations in one undo transaction (max 500 actions, 23 sub-op types — all write ops including: remove_user_parameter, set_parameter_default, set_module_input_di, set_curve_value, reorder_emitters, duplicate_emitter, set_renderer_binding, request_compile) |
| `import_system_spec` | Overwrite an existing Niagara system with a JSON spec (removes all emitters/params, applies spec fresh) |
| `create_system_from_spec` | Full declarative system builder from JSON spec. Uses `UNiagaraSystemFactoryNew::InitializeSystem` for proper system creation |
| `configure_curve_keys` | Set keys on a DataInterface curve input (NiagaraDataInterfaceCurve/ColorCurve). For plain float inputs use set_curve_value instead |
| `export_system_spec` | Reverse-engineer an existing system into create_system_from_spec-compatible JSON |
| `get_scalability_settings` | Read scalability settings from a NiagaraEffectType asset |
| `set_scalability_settings` | Set scalability settings on a NiagaraEffectType asset |

**Data Interface (3)**
| Action | Description |
|--------|-------------|
| `get_di_functions` | Get data interface function signatures |
| `configure_data_interface` | Set arbitrary properties on a DI attached to a module input via reflection |
| `get_di_properties` | Inspect editable properties on a Niagara DataInterface class (CDO reflection) |

**HLSL (1)**
| Action | Description |
|--------|-------------|
| `get_compiled_gpu_hlsl` | Get compiled GPU HLSL for an emitter |

**Dynamic Inputs (8)**
| Action | Description |
|--------|-------------|
| `list_dynamic_inputs` | List all dynamic inputs on a module |
| `get_dynamic_input_tree` | Get the full tree structure of a dynamic input |
| `remove_dynamic_input` | Remove a dynamic input from a module |
| `get_dynamic_input_value` | Get the current value of a dynamic input |
| `get_dynamic_input_inputs` | Get all sub-inputs of a dynamic input |
| `add_dynamic_input` | Attach a dynamic input script to a module input pin |
| `search_dynamic_inputs` | Browse available dynamic input scripts with optional type filtering |
| `set_dynamic_input_value` | Set an input value on a dynamic input node |

**Emitter Management (8)**
| Action | Description |
|--------|-------------|
| `rename_emitter` | Rename an emitter within a system |
| `get_emitter_parent` | Get the parent emitter asset of an emitter in a system (read-only) |
| `get_emitter_property` | Get a property value from an emitter via reflection |
| `list_available_renderers` | List all available renderer classes that can be added |
| `clear_emitter_modules` | Remove all modules from an emitter, optionally filtered by stage |
| `create_emitter` | Add a minimal empty emitter to a system (no template needed) |
| `get_emitter_summary` | Deep view of a single emitter (modules per stage, renderers, properties) |
| `list_emitter_properties` | List all editable properties on FVersionedNiagaraEmitterData with current values |

**Renderer Configuration (3)**
| Action | Description |
|--------|-------------|
| `set_renderer_mesh` | Set the mesh asset on a mesh renderer |
| `configure_ribbon` | Configure ribbon renderer settings (width, facing, tessellation, etc.) |
| `configure_subuv` | Configure SubUV animation settings on a renderer |

**Event Handlers (4)**
| Action | Description |
|--------|-------------|
| `get_event_handlers` | Get all event handlers on an emitter |
| `set_event_handler_property` | Set a property on an event handler |
| `remove_event_handler` | Remove an event handler from an emitter |
| `add_event_handler` | Add an inter-emitter event handler (death, collision, location events) |

**Simulation Stages (4)**
| Action | Description |
|--------|-------------|
| `get_simulation_stages` | Get all simulation stages on an emitter |
| `set_simulation_stage_property` | Set a property on a simulation stage |
| `remove_simulation_stage` | Remove a simulation stage from an emitter |
| `add_simulation_stage` | Add a simulation stage to an emitter |

**Module Outputs (1)**
| Action | Description |
|--------|-------------|
| `get_module_output_parameters` | Get output parameters exposed by a module |

**Niagara Parameter Collections (NPC) (5)**
| Action | Description |
|--------|-------------|
| `create_npc` | Create a Niagara Parameter Collection asset |
| `get_npc` | Get NPC contents (parameters, defaults, namespace) |
| `add_npc_parameter` | Add a parameter to an NPC |
| `remove_npc_parameter` | Remove a parameter from an NPC |
| `set_npc_default` | Set the default value of an NPC parameter |

**Effect Types (3)**
| Action | Description |
|--------|-------------|
| `create_effect_type` | Create a Niagara Effect Type asset |
| `get_effect_type` | Get effect type settings (scalability, significance, budget) |
| `set_effect_type_property` | Set a property on an effect type |

**Utilities (6)**
| Action | Description |
|--------|-------------|
| `get_available_parameters` | List available parameters that can be bound to inputs |
| `preview_system` | Capture a preview image of a Niagara system |
| `diff_systems` | Compare two Niagara systems and return structural differences |
| `save_emitter_as_template` | Save an emitter as a reusable template asset |
| `clone_module_overrides` | Clone input overrides from one module to another |
| `auto_layout` | Auto-arrange nodes in a Niagara module script graph. `formatter`: `"auto"` (default) — uses Blueprint Assist if available, falls back to built-in layout; `"blueprint_assist"` — requires BA; `"builtin"` — built-in only |

### UE 5.7 Compatibility Fixes (6 sites)

All marked with "UE 5.7 FIX" comments:
1. `AddEmitterHandle` takes `FGuid VersionGuid`
2-5. `GetOrCreateStackFunctionInputOverridePin` uses 5-param version (two FGuid params)
6. `RapidIterationParameters` accessed via direct UPROPERTY (no getter)

---

**Composite (1)**
| Action | Description |
|--------|-------------|
| `set_spawn_shape` | Add a spawn shape (Cylinder, Sphere, Box, Cone, Torus) to an emitter with automatic switch setup |
