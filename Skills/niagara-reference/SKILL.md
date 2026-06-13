---
name: niagara-reference
description: Use when looking up passive Niagara VFX reference knowledge via Monolith MCP context (NOT for editing effects) - sim/CPU-vs-GPU rules, particle-count budgets, fixed-bounds and module-order gotchas, particle-material integration. This is a read-only doc index; for any create/edit/inspect/optimize-on-an-actual-Niagara-system action use unreal-niagara. Triggers on particle budget, sim rule, Niagara gotcha, module order, fixed bounds, GPU sim limit, spawn rate budget, data interface reference, CPU vs GPU particles, VFX performance reference, Niagara reference.
---

# Niagara Reference Library

This is a passive Niagara VFX knowledge card. It holds reference text only — sim rules, particle budgets, gotchas, and material-integration notes. It drives NO live Monolith namespace; there is no discover-first block because nothing here calls an action.

## When to use / Use a different skill for

- **Use this skill** to recall a Niagara rule, budget, or gotcha from memory or doc text — no asset is touched.
- **unreal-niagara** — any create, edit, inspect, optimize, or debug action on an actual Niagara system, emitter, module, renderer, parameter, DI, or HLSL node (the live `niagara` namespace lives there).
- **material-reference** — when the reference lookup is for materials/shaders rather than VFX.

## Quick Reference

- **CPU sim:** below 100 particles. **GPU sim:** above 1,000 particles. Test in between.
- **Module execution order matters:** Position -> SolveForcesAndVelocity -> Collision/Readers -> Visual.
- **ALWAYS set fixed bounds.** #1 Niagara optimization. GPU REQUIRES them.
- **Events are CPU only.** GPU alternative: Attribute Reader.
- **Light Renderer is CPU only.** GPU alternative: Component Renderer + PointLight.
- **Map For node is GPU only.** Does NOT work on CPU.
- **Skeletal Mesh GPU sampling is D3D12 only.** Crashes on Vulkan.
- **Attribute Reader:** source emitter must execute BEFORE the reader (top-to-bottom order).
- **1 emitter x 1,000 particles beats 10 emitters x 100 particles.** GPU sim has fixed overhead.
- **Lightweight emitters (UE 5.4+):** zero tick cost for simple ambient effects.
- **Additive materials don't need sorting.** Set sort to None for free perf.
- **Dynamic Parameters:** 4 slots x 4 channels = 16 floats for material communication.
- **Data Channel Write is CPU only.** GPU can only read.
- **No spaces in parameter names.** Breaks HLSL and scripting.

## Reference Documents

Read these on demand when the topic is relevant. Each resolves relative to this skill folder.

| File | Content |
|------|---------|
| [references/niagara-architecture.md](references/niagara-architecture.md) | CPU vs GPU decision rules, Lightweight/Stateless emitters, sim-target compatibility, lifecycle and parent-child communication (events, attribute reader, data channels), data interfaces |
| [references/niagara-effect-recipes.md](references/niagara-effect-recipes.md) | SYNTHESIZED starter recipes (fire, smoke) seeded from material conventions and budgets — not a verified recipe library; treat values as a starting point, not ground truth |
| [references/niagara-performance.md](references/niagara-performance.md) | Particle count budgets, fixed bounds, emitter count impact, sort mode costs, mesh vs sprite, scalability integration |
| [references/niagara-gotchas.md](references/niagara-gotchas.md) | Module order, GPU limitations, attribute reader ordering, Map For CPU restriction, deterministic random, warm-up spikes, parameter naming |
| [references/niagara-material-integration.md](references/niagara-material-integration.md) | Particle Color, blend modes, Dynamic Parameters (4x4 slots), Sub-UV/flipbook, soft particles, camera facing, usage flags |

> **Effect recipes are synthesized, not verified.** `references/niagara-effect-recipes.md` was seeded from material-convention and budget bullets because no recipe bodies exist in the repository and the previously-promised vfx-patterns skill does not exist. Use it as a seed and verify in-editor via unreal-niagara before trusting any parameter value.

## Custom HLSL

Do NOT duplicate Custom HLSL rules here. The authoritative guide lives in the repo doc `Plugins/Monolith/Docs/NIAGARA_HLSL_GUIDE.md` (referenced by repo path, not a skill-relative link, so it survives junction install). Read it before writing or editing any Niagara CustomHlsl node; the unreal-niagara skill drives the actual `get_custom_hlsl_text` / `set_custom_hlsl_text` writes.

> **Caveat:** any live action name mentioned on this card (e.g. `get_custom_hlsl_text`, `set_custom_hlsl_text`) is a pointer into unreal-niagara, not a contract here — confirm its exact schema via `monolith_discover` before calling it. The synthesized recipe doc (`references/niagara-effect-recipes.md`) is guidance only, not a verified call sequence.

## Rules

- This skill is reference text only. To act on a Niagara asset, load **unreal-niagara**.
- The detailed docs above describe behavior and budgets; the live action catalog and exact parameter schemas belong to unreal-niagara's discover-first flow, not this card.
- Effect-recipe values are synthesized seeds — verify any recipe in-editor before relying on it.
