---
name: unreal-niagara
description: Use when creating, editing, or inspecting Niagara particle systems via Monolith MCP (niagara namespace) — systems, emitters, modules, parameters, renderers, data interfaces, dynamic inputs, event handlers, simulation stages, stateless emitters, and custom HLSL. For a sim rule, particle budget, or module/DI reference lookup use niagara-reference; if the work is the particle MATERIAL use unreal-materials; if the effect is sound use unreal-audio. This skill owns Niagara system/emitter/module editing. Triggers on Niagara, particle, VFX, emitter, particle system, GPU particles, emitter spawn rate, ribbon renderer, mesh particles, niagara module script, data interface, scratch pad, particle attribute, burst spawn.
---

# Unreal Niagara VFX Workflows

Edits and inspects Niagara particle systems through the Monolith **niagara** namespace (129 actions via `niagara_query()`).

## Discovery (discover-first)

Always confirm action names and parameter schemas against the live catalog before calling — these tables are a snapshot.

```
monolith_discover({ namespace: "niagara" })
monolith_discover({ namespace: "niagara", action: "search_by_material", mode: "schema" })
```

Use focused schema discovery before calls when parameter names, aliases, or ranges matter. Broad `monolith_discover({ namespace: "niagara" })` is useful for browsing, but the Niagara surface is large.

## When to use / Use a different skill for

- **This skill** owns Niagara system/emitter/module editing — systems, emitters, renderers, modules, parameters, DI, dynamic inputs, event handlers, sim stages, stateless emitters, custom HLSL.
- **niagara-reference** — for a sim rule, particle-count budget, GPU/CPU compatibility rule, effect recipe, material-integration recipe, or module/DI reference lookup (no editing).
- **unreal-materials** — when the work is the particle's MATERIAL (PBR graph, blend mode, Dynamic Parameters) rather than the Niagara system itself.
- **unreal-audio** — when the "effect" is sound (SFX/MetaSound), not particles.

## Asset Path Conventions

All asset paths follow UE content browser format (no .uasset extension):

| Location | Path Format | Example |
|----------|------------|--------|
| Project Content/ | `/Game/Path/To/Asset` | `/Game/VFX/NS_Sparks` |
| Project Plugins/ | `/PluginName/Path/To/Asset` | `/MassProjectile/VFX/NS_Example` |
| Engine Plugins | `/PluginName/Path/To/Asset` | `/Niagara/DefaultAssets/SystemAssets/NS_Default` |

## Key Parameter Names

- `asset_path` — the Niagara system asset path (NOT `system` or `asset`)
- `emitter` — emitter name (string)
- `module_node` — module GUID returned by `get_ordered_modules` (NOT module display name)

## Cross-Namespace Capabilities

These already-working features span multiple Monolith namespaces and are useful during Niagara workflows:

### Texture/Material Preview
Agents can call `material_query("render_preview", { "asset_path": "/Game/SomeTexture" })` to preview textures or materials before assigning them to renderers. Works with any texture or material asset path.

### Module Stage Info from get_ordered_modules
`get_ordered_modules` already returns a `usage` field per module indicating which stage it belongs to (e.g. `"Emitter Update"`, `"Particle Spawn"`). There is no need for a separate stage query — the stage is included in the standard module listing.

For selector-based shared-graph stages, pass an explicit selector when needed (PR #65):
- `usage: "particle_simulation_stage"` with `usage_id`, `stage_name`, or `stage_index`
- `usage: "particle_event"` with `usage_id` or `handler_index`

These selectors (`usage_id`/`stage_name`/`stage_index`/`handler_index`) are exposed on `get_ordered_modules`, `add_module`, and `duplicate_module`. `move_module` takes only `module_node` + `new_index` (no selector params in the live catalog).

`add_event_handler` only creates the handler and its `ParticleEventScript` container — it does **not** auto-add `ReceiveDeathEvent` / `ReceiveLocationEvent`, and it rejects an inter-emitter handler whose `source_emitter` cannot be resolved. For event-driven effects like fireworks, add the matching `Receive<Event>` module to the `particle_event` script (target it with the selector above) and set required payload fields such as `Position` to `Apply`.

### Available Parameters with Usage Filter
`get_available_parameters` with `usage: "particle"` lists all particle attributes including compiled emitter attributes. This is the fastest way to discover what bindings are available without inspecting individual modules.

### CustomHlsl Module Input Fallback
`set_module_input_value` automatically detects CustomHlsl modules and uses pin defaults directly as a fallback path. This means you can set inputs on custom HLSL modules the same way as built-in modules — no special handling required.

## Action Reference

**Param notation:** `name*` required, `name?` optional, `name=val` default, `a/b/c` allowed values, `[w]` mutates (transaction-wrapped write). Signatures are a snapshot of the live catalog — for the exact full schema call `monolith_discover({ namespace: "niagara", action: "<action>", mode: "schema" })`. The discover-first block above is the authority; do not call from this snapshot alone if param names, aliases, or ranges are load-bearing.

Full per-action parameter signatures — corrected against the live catalog and grouped by category — live in [`references/niagara-actions.md`](references/niagara-actions.md). Common-name traps to remember:

- `create_system` / `create_system_from_spec` / `create_stateless_emitter` / `create_npc` / `create_effect_type` use `save_path`, NOT `asset_path`.
- `add_emitter` takes `emitter_asset` (source asset path) + optional `name`, NOT `emitter`. `duplicate_emitter` takes `source_emitter` + `new_name`.
- `add_renderer` takes `class` (Sprite/Mesh/Ribbon), NOT `type`. The whole renderer family addresses a renderer by `renderer_index` (integer), NOT `renderer`. `set_renderer_binding` uses `binding_name` + `attribute`.
- `add_module` requires `usage` + `module_script` (NOT `stage` / `module_path`). `get_module_graph` takes only `script_path`.
- `batch_execute` takes `operations`, NOT `commands`.
- `set_require_current_frame_data` takes `require` (bool); `set_sim_stage_iteration_count` takes `iterations`; `set_sim_stage_execute_behavior` takes `behavior`.
- **Phase 0 stubs (not yet implemented):** `create_stateless_emitter`, `get_system_timing`, `set_warmup_profile`, `set_fixed_tick_delta`, `set_require_current_frame_data`. They are registered but return not-implemented — confirm with discover before relying on them.

Undocumented elsewhere but present in the catalog: `list_available_renderers`, `set_renderer_mesh`, `configure_ribbon`, `configure_subuv`, `auto_layout` (experimental), `audit_cross_asset_refs` — all detailed in the reference file.

### Custom HLSL — body rules

Custom HLSL creation/read/overwrite actions (`create_module_from_hlsl`, `create_function_from_hlsl`, `get_custom_hlsl_text`, `set_custom_hlsl_text`) and the full body-rule checklist are in [`references/niagara-actions.md`](references/niagara-actions.md).
**Input/output format:** `[{"name": "InValue", "type": "float"}, {"name": "Velocity", "type": "vec3"}]`; **supported types:** `float`, `int`, `bool`, `vec2`, `vec3`, `vec4`, `color`, `position`, `quat`, `matrix`.
**CRITICAL: Before writing custom HLSL, read the repo doc `Plugins/Monolith/Docs/NIAGARA_HLSL_GUIDE.md` (referenced by repo path, not a skill-relative link, so it survives junction install) for the complete body rules.** Key CPU/GPU difference: GPU simulation CAN directly write `Particles.Velocity`/`Particles.Position` (wrap in `#if GPU_SIMULATION ... #endif`); CPU simulation does NOT support `Particles.*` — use output parameters. Use bare input/output names (`InColor`, not `Module.InColor`).

## Common Workflows

### Inspect a system
```
niagara_query({ action: "list_emitters", params: { asset_path: "/Game/VFX/NS_Sparks" } })
niagara_query({ action: "get_ordered_modules", params: { asset_path: "/Game/VFX/NS_Sparks", emitter: "Fountain" } })
niagara_query({ action: "get_module_inputs", params: { asset_path: "/Game/VFX/NS_Sparks", emitter: "Fountain", module_node: "<GUID from get_ordered_modules>" } })
```

### Create a system and add an emitter
```
niagara_query({ action: "create_system", params: { save_path: "/Game/VFX/NS_Sparks" } })
// add_emitter copies from an EXISTING emitter asset; `name` overrides the handle name
niagara_query({ action: "add_emitter", params: { asset_path: "/Game/VFX/NS_Sparks", emitter_asset: "/Niagara/DefaultAssets/Templates/Fountain.Fountain", name: "Fountain" } })
// Or add a minimal empty emitter with no source asset:
niagara_query({ action: "create_emitter", params: { asset_path: "/Game/VFX/NS_Sparks", name: "Fountain", sim_target: "cpu" } })
```

### Set a module input value
```
niagara_query({ action: "set_module_input_value", params: {
  asset_path: "/Game/VFX/NS_Sparks", emitter: "Fountain",
  module_node: "<GUID>", input: "Lifetime", value: 2.0
}})
```

### Add a renderer with material
```
// `class` (Sprite/Mesh/Ribbon), NOT `type`. add_renderer returns the new renderer_index.
niagara_query({ action: "add_renderer", params: { asset_path: "/Game/VFX/NS_Sparks", emitter: "Fountain", class: "Sprite" } })
// Material is assigned by integer renderer_index, NOT a renderer name.
niagara_query({ action: "set_renderer_material", params: {
  asset_path: "/Game/VFX/NS_Sparks", emitter: "Fountain",
  renderer_index: 0, material: "/Game/Materials/M_Particle"
}})
```

### Find and add a module script
```
niagara_query({ action: "list_module_scripts", params: { query: "ShapeLocation" } })
// Space-separated terms also work — "Gravity Force" matches "GravityForce"
niagara_query({ action: "list_module_scripts", params: { query: "Gravity Force" } })
// add_module takes `usage` (particle_spawn/particle_update/emitter_update/...) + `module_script`, NOT `stage`/`module_path`
niagara_query({ action: "add_module", params: { asset_path: "/Game/VFX/NS_Sparks", emitter: "Fountain", usage: "particle_spawn", module_script: "<path from list_module_scripts>" } })
```

### Inspect renderer properties before setting them
```
// Renderers are addressed by integer renderer_index (from list_renderers), NOT a renderer name
niagara_query({ action: "list_renderer_properties", params: { asset_path: "/Game/VFX/NS_Sparks", emitter: "Fountain", renderer_index: 0 } })
niagara_query({ action: "set_renderer_property", params: { asset_path: "/Game/VFX/NS_Sparks", emitter: "Fountain", renderer_index: 0, property: "SubImageSize", value: "4,4" } })
```

### Preview a texture before using it
```
material_query({ action: "render_preview", params: { asset_path: "/Game/VFX/Textures/T_Smoke" } })
```

### Temporal control — configure an emitter loop in one call

Goal: make `Fountain` loop 3 times over 2.5 seconds with a 0.5-second delay between iterations.

```
niagara_query({ action: "set_emitter_loop_profile", params: {
  asset_path: "/Game/VFX/NS_Sparks",
  emitter: "Fountain",
  loop_behavior: "Multiple",
  loop_count: 3,
  loop_duration: 2.5,
  loop_delay: 0.5,
  loop_delay_enabled: true
}})
```

One call replaces five: two `set_static_switch_value` (Loop Behavior, UseLoopDelay) + three `set_module_input_value` (Loop Duration, Loop Delay, Loop Count) against the EmitterState module.

Before changing topology, inspect it:
```
niagara_query({ action: "get_emitter_timing_summary", params: {
  asset_path: "/Game/VFX/NS_Sparks", emitter: "Fountain"
}})
```
Returns loop topology + sim stages + InitializeParticle lifetime fields in one response — no need to walk `get_ordered_modules` → `get_module_inputs` per emitter.

For a fire-and-forget burst over 0.4s with no looping:
```
niagara_query({ action: "set_emitter_loop_profile", params: {
  asset_path: "/Game/VFX/NS_Impact",
  emitter: "Sparks",
  loop_behavior: "Once",
  loop_duration: 0.4
}})
```

### Create + configure a Lightweight Emitter end-to-end

`UNiagaraStatelessEmitter` (Lightweight Emitter) is a separate asset class — no `UNiagaraSystem` wrapper needed. Pass the standalone asset path directly to the stateless-aware temporal-control actions; they detect the class and route to a reflection-based write/read against the protected `EmitterState` (`FNiagaraEmitterStateData`) UPROPERTY.

```
// 1. Create the asset — NOTE: create_stateless_emitter is a Phase 0 stub (not yet
//    implemented in the live catalog); confirm with monolith_discover before relying on it.
niagara_query({ action: "create_stateless_emitter", params: {
  save_path: "/Game/MyEmitters/NSE_Foo"
}})

// 2. Configure loop topology — no `emitter` param needed for standalone stateless assets
niagara_query({ action: "set_emitter_loop_profile", params: {
  asset_path: "/Game/MyEmitters/NSE_Foo",
  loop_behavior: "Multiple",
  loop_duration: 2.5,
  loop_count: 3,
  loop_delay: 0.5,
  loop_delay_enabled: true,
  loop_duration_mode: "Fixed"
}})
// Response includes `stateless: true`

// 3. Round-trip verification
niagara_query({ action: "get_emitter_timing_summary", params: {
  asset_path: "/Game/MyEmitters/NSE_Foo"
}})
// Response: `stateless: true`, loop topology populated, lifetime fields null, sim_stages: []
```

`loop_duration_mode` (`"Fixed"` / `"Infinite"` — maps to `ENiagaraLoopDurationMode`) is stateless-only. The stateful `EmitterState` module has no equivalent input; supplying it on a stateful path emits a warning.

## Particle Materials, GPU/CPU Rules, and Known Issues

For particle-material recipes (blend modes, Particle Color, Dynamic Parameters, soft particles, SubUV), GPU-vs-CPU compatibility rules (fixed bounds, Light/Ribbon renderer restrictions), and known-gotcha details (emitter display-name vs handle-ID, `set_curve_value` for DI curves, `UseVelDistribution`), use the **niagara-reference** skill — those references now live in its `references/` docs (`niagara-material-integration.md`, `niagara-architecture.md`, `niagara-gotchas.md`). Assign a material to a renderer with `set_renderer_material` once the material agent has created it.

## Rules

- Use focused `monolith_discover({ namespace: "niagara", action: "...", mode: "schema" })` to see exact per-action param schemas
- The primary asset param is `asset_path`, NOT `system` or `asset`
- Search/discovery actions validate JSON types and ranges before running; fix the payload instead of relying on implicit coercion
- Module actions require `module_node` (a GUID) — get it from `get_ordered_modules`
- Module stages: `Emitter Spawn`, `Emitter Update`, `Particle Spawn`, `Particle Update`, `Render`
- User parameters are the main interface for Blueprint/C++ control of effects
- Parameter actions now accept the `User.` prefix (e.g. `User.MyParam`) in addition to bare names
- `di_class` for `set_module_input_di` accepts both `UNiagaraDataInterfaceCurve` and `NiagaraDataInterfaceCurve` — U prefix is optional (auto-resolved)
- **HLSL modules:** before writing custom HLSL, ALWAYS read the repo doc `Plugins/Monolith/Docs/NIAGARA_HLSL_GUIDE.md` (referenced by repo path, not a skill-relative link, so it survives junction install) for the complete rules. **Key CPU/GPU difference:** GPU simulation CAN directly write `Particles.Velocity` / `Particles.Position` etc. (must wrap in `#if GPU_SIMULATION`); CPU simulation does NOT support `Particles.*` — use output parameters instead. Use bare input/output names (`InColor`, not `Module.InColor`); inputs stay overridable via `set_module_input_value`. Read/overwrite existing `CustomHlsl` nodes with `get_custom_hlsl_text` / `set_custom_hlsl_text`.
- When creating VFX, always dispatch material agent FIRST, then assign materials after they're created
- Verify materials exist before assigning them to renderers
