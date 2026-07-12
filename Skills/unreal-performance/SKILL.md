---
name: unreal-performance
description: Use when analyzing or optimizing Unreal Engine performance via Monolith MCP (config / material / niagara namespaces) — INI/CVar perf auditing, material shader instruction and sample stats, and Niagara emitter/renderer/GPU-HLSL complexity. For per-mesh triangle/LOD/collision/shadow/draw-call budgeting use unreal-mesh; for raw .ini/CVar edits with no profiling intent use unreal-config; for post-failure logs/crashes/hitches use unreal-debugging; for active build/recompile/Live-Coding use unreal-build. Triggers on performance, optimization, FPS, frame time, GPU, draw calls, shader complexity, instruction count, Niagara complexity, CVar perf tuning.
---

# Unreal Performance Analysis Workflows

You have access to **Monolith** with cross-domain performance tools. This skill drives three namespaces with profiling intent — **config** (INI/CVar perf tuning), **material** (shader instruction/sample stats), and **niagara** (emitter/renderer/GPU-HLSL complexity).

## Discovery

```
monolith_discover({ namespace: "config" })                                          # all config actions
monolith_discover({ namespace: "config", action: "resolve_setting", mode: "schema" })   # exact params
monolith_discover({ namespace: "material" })                                        # all material actions
monolith_discover({ namespace: "material", action: "get_all_expressions", mode: "schema" })
monolith_discover({ namespace: "niagara" })                                         # all niagara actions
monolith_discover({ namespace: "niagara", action: "list_emitters", mode: "schema" })
```

## When to use / Use a different skill for

This skill owns cross-domain performance **analysis and tuning with profiling intent** — auditing INI/CVar perf settings, measuring material shader cost, and inspecting Niagara system complexity. Route elsewhere when:

- Triangle-count, LOD, collision, or texel-density budgeting on a specific **mesh asset** → `unreal-mesh`
- Raw `.ini`/CVar edits with no profiling intent (set a value, read a section) → `unreal-config`
- Post-failure logs, crashes, stack traces, or hitch forensics → `unreal-debugging`
- Active build, recompile, or Live-Coding work → `unreal-build`

Reciprocal: `unreal-mesh` defers per-mesh draw-call/triangle/shadow **budgeting that motivates a perf pass** here; `unreal-config` defers profiling-motivated CVar changes here; `unreal-debugging` defers slowness/profiling here; `unreal-build` defers post-build profiling here.

## Asset Path Conventions

All asset paths follow UE content browser format (no .uasset extension):

| Location | Path Format | Example |
|----------|------------|--------|
| Project Content/ | `/Game/Path/To/Asset` | `/Game/Materials/M_Rock` |
| Project Plugins/ | `/PluginName/Path/To/Asset` | `/MassProjectile/Materials/M_Example` |
| Engine Plugins | `/PluginName/Path/To/Asset` | `/Niagara/DefaultAssets/SystemAssets/NS_Default` |

**Note:** For project plugins, the path starts with the plugin name as configured in the .uplugin file's "MountPoint" — which defaults to `/<PluginName>/`. Most plugins mount their Content/ folder there directly.

## Key Tools by Domain

Param notation: `name*` required, `name?` optional, `name=val` default, `a/b/c` allowed, `[w]` mutates. Signatures are a snapshot of the live catalog — for the exact full schema call `monolith_discover` with `mode: "schema"` (the discover-first block above is the authority).

### Config Auditing (`config_query`)
```
monolith_discover({ namespace: "config" })
```

| Action | Purpose | Params |
|--------|---------|--------|
| `resolve_setting` | Get the effective value of any CVar/config setting; omit `file` to search the canonical config categories and return the resolved category plus search diagnostics | `file?` `section*` `key*` |
| `explain_setting` | Understand where a value comes from across Base→Default→User | `file?` `section?` `key?` `setting?` (`setting` = convenience search across common categories) |
| `diff_from_default` | See all project customizations vs engine defaults | `file*` `section?` |
| `search_config` | Find settings by keyword | `query*` `category?` |

Adjacent config-perf reads (same namespace): `get_section` (`file*` `section*`), `get_cvar` (`name*`), `find_cvars` (`query?` `mode=prefix` prefix/contains `limit=100`).

### Material Performance (`material_query`)

`get_all_expressions` lists nodes (count instruction-bearing expressions); the authoritative shader cost numbers come from `get_compilation_stats` / `recompile_material` with `include_stats`.

| Action | Purpose | Params |
|--------|---------|--------|
| `get_all_expressions` | List all expression nodes in a base material | `asset_path*` |
| `get_compilation_stats` | Read instruction count, texture samplers, errors (no recompile) | `asset_path*` |
| `validate_material` | Check for errors, unused nodes, broken connections | `asset_path*` `fix_issues=false` |
| [w] `recompile_material` | Force recompile; with `include_stats` returns errors + instruction counts | `asset_path*` `include_stats?` |
| [w] `batch_recompile` | Recompile many materials, per-material instruction counts | `asset_paths*` (array, max 200) |
| [w] `render_preview` | Render preview PNG; UV tiling to check repetition at scale | `asset_path*` `resolution=256` `uv_tiling=1.0` `preview_mesh=sphere` plane/sphere/cube |
| `check_tiling_quality` | Flag missing anti-tiling, no macro variation, direct UV usage | `asset_path*` |

### Niagara Inspection (`niagara_query`)

Use `monolith_discover({ namespace: "niagara" })` to see all available actions. Key ones for performance:

| Action | Purpose | Params |
|--------|---------|--------|
| `list_emitters` | List all emitters in a system — check emitter count | `asset_path*` |
| `list_renderers` | List all renderers on an emitter — check renderer count | `asset_path*` `emitter*` |
| `get_ordered_modules` | Get modules in a script stage — audit module complexity | `asset_path*` `emitter*` `usage?` (particle_update/particle_event/emitter_spawn/particle_simulation_stage) `stage_name?` `usage_id?` `stage_index?` `handler_index?` |
| `get_all_parameters` | Get all parameters — review parameter overhead | `asset_path*` `emitter?` `scope?` |
| `get_compiled_gpu_hlsl` | Get compiled GPU HLSL — inspect shader complexity | `asset_path*` `emitter*` |
| `get_system_summary` | One-call topology/role/module-count overview | `asset_path*` `detail_level?` (compact/full) |
| `get_system_diagnostics` | Compile errors, warnings, renderer issues, script stats | `asset_path*` `compile_first?` (default true) |
| [w] `set_fixed_bounds` | Set explicit bounds to avoid per-frame GPU bounds recompute | `asset_path*` `min*` `max*` `emitter?` `enabled?` |

**Note:** Niagara actions use `asset_path` (not `system`) for the system, and `emitter` (not `emitter_name`) for the emitter.

## Common Workflows

### Audit INI performance settings
```
config_query({ action: "diff_from_default", params: { file: "DefaultEngine" } })
config_query({ action: "resolve_setting", params: { file: "DefaultEngine", section: "/Script/Engine.RendererSettings", key: "r.Lumen.TraceMeshSDFs" } })
config_query({ action: "resolve_setting", params: { section: "/Script/Engine.RendererSettings", key: "r.Lumen.TraceMeshSDFs" } })  # category unknown: inspect category/searched_categories
config_query({ action: "explain_setting", params: { setting: "r.Lumen.TraceMeshSDFs" } })
```

### Check material shader complexity
```
material_query({ action: "get_all_expressions", params: { asset_path: "/Game/Materials/M_Character" } })
material_query({ action: "get_compilation_stats", params: { asset_path: "/Game/Materials/M_Character" } })
material_query({ action: "validate_material", params: { asset_path: "/Game/Materials/M_Character" } })
```

### Audit Niagara effect complexity
```
niagara_query({ action: "list_emitters", params: { asset_path: "/Game/VFX/NS_Blood" } })
niagara_query({ action: "list_renderers", params: { asset_path: "/Game/VFX/NS_Blood", emitter: "Emitter0" } })
niagara_query({ action: "get_compiled_gpu_hlsl", params: { asset_path: "/Game/VFX/NS_Blood", emitter: "Emitter0" } })
```

### Find expensive config settings
```
config_query({ action: "search_config", params: { query: "Lumen" } })
config_query({ action: "search_config", params: { query: "Shadow" } })
config_query({ action: "search_config", params: { query: "TSR" } })
```

## High-Impact INI Settings

These are the most impactful performance CVars to audit:

| Setting | Impact | Notes |
|---------|--------|-------|
| `r.Lumen.TraceMeshSDFs` | ~1-2ms GPU | Set to 0 if not using mesh SDF tracing |
| `r.Shadow.Virtual.SMRT.RayCountDirectional` | ~0.5ms GPU | 8 is default, 4 is often sufficient |
| `gc.IncrementalBeginDestroyEnabled` | Frame spikes | Enable to eliminate GC hitches |
| `r.StochasticInterpolation` | ~0.5ms GPU | Set to 2 for better perf |
| `r.AntiAliasingMethod` | Varies | TSR handles aliasing — MSAA often redundant |
| `r.Lumen.Reflections.AsyncCompute` | White flash | UE-354891 bug, keep at 0 until 5.7.2 |

## Tips

- Use `explain_setting` before changing any unfamiliar CVar
- `diff_from_default` is the fastest way to see all project customizations
- For real pixel-shader cost read `get_compilation_stats` (instruction count + sampler count); `get_all_expressions` only lists the node graph
- Use `list_emitters` + `list_renderers` to audit Niagara system complexity
