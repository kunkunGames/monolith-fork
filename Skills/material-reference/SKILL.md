---
name: material-reference
description: Use when looking up passive material reference knowledge via Monolith MCP context (NOT for editing assets) - PBR value rules, HLSL Custom-node gotchas, sampler/instruction budgets, anti-tiling techniques. This is a read-only doc index; for any create/edit/inspect/optimize-on-an-actual-material action use unreal-materials. Triggers on PBR, PBR values, metallic, roughness, shader budget, instruction budget, sampler limit, HLSL gotcha, Custom HLSL node, anti-tiling, material reference, material budget, blend mode cost, CPD vs DMI, wet surface Lagarde.
---

# Material Reference Library

A read-only knowledge card for material/shader authoring decisions — PBR base values, Custom HLSL node rules, instruction/sampler budgets, and anti-tiling techniques. It drives **no** live namespace: it carries no MCP actions of its own. For any action on an actual material asset, route to the **unreal-materials** skill (material namespace), which owns create/edit/inspect/validate/optimize.

## Use a different skill for

- **unreal-materials** — creating, editing, inspecting, validating, or actually optimizing/debugging a real material or material-instance asset. This skill is reference text only; that skill runs the live `material` namespace actions (`build_material_graph`, `set_material_property`, `get_compilation_stats`, `validate_material`, etc.).
  - *Caveat:* action names here (and in the reference docs, e.g. `render_preview`, `get_all_expressions`) are pointers into unreal-materials, not contracts on this card — confirm the exact schema via `monolith_discover(mode:'schema')` before any WRITE call.
- **niagara-reference** — when the reference lookup is for Niagara VFX/particles rather than materials/shaders.

## Quick Reference

- **Sampler limit:** 16 per material (use Shared:Wrap for more textures).
- **Instruction budgets:** Opaque < 150, Translucent < 200, Post-process < 100.
- **GPU cost (cycles):** MAD=4, Division=20, Pow/Sin/Cos=16, Tan=52.
- **Static switches:** each doubles permutations. Max ~6 per master material.
- **Rust is NOT metallic.** Iron oxide is a dielectric (Metallic=0.0).
- **Non-metal BaseColor:** never below sRGB 30, never above sRGB 240.
- **Wet surfaces (Lagarde):** Roughness * ~0.3, BaseColor squared (darker), saturation boost.
- **MaterialFloat = half.** Use it for platform portability in Custom HLSL.
- **clip() disabled on Nanite passes.** ddx/ddy return 0 in compute.
- **MPC limit:** 2 Material Parameter Collections per material.
- **CPD over DMI:** Custom Primitive Data keeps draw-call batching; Dynamic Material Instances break it.
- **Alpha gotcha:** NEVER use BLEND_Translucent for render-target alpha. Use BLEND_AlphaComposite.

## Reference Documents

Read these on demand when the topic is relevant. Paths are relative to this skill folder, so they survive the per-skill junction/symlink install.

| File | Content |
|------|---------|
| [references/hlsl-custom-node-guide.md](references/hlsl-custom-node-guide.md) | Custom HLSL node authoring — FMaterialPixelParameters / FMaterialVertexParameters fields, View/ResolvedView uniforms, helper functions, type rules, Nanite/compute gotchas, and recipes (noise, fresnel, dissolve, triplanar, flow map, panner). |
| [references/pbr-values.md](references/pbr-values.md) | PBR base values by category — metals, corrosion/degradation, organics/horror (blood, bone, flesh), building materials, environment — plus the key metal-vs-dielectric rules and a quick checklist. |
| [references/material-patterns.md](references/material-patterns.md) | Effect recipe card — wet surface (Lagarde), subsurface blood/skin, dissolve, emissive pulse, damage overlay, decals, masked vs translucent, degradation, dark environment, hologram, POM. |
| [references/material-performance.md](references/material-performance.md) | Budgets and cost tradeoffs — instruction budgets, GPU cycle costs, sampler budget, static-switch math, MPC limit, blend-mode costs, CPD vs DMI, texture-vs-math, and the get_compilation_stats / batch_recompile measurement workflow. |
| [references/material-systems.md](references/material-systems.md) | Architecture — master material design, MIC vs MID (vs MFI), MPC layout, material function libraries, material layers, static vs dynamic branch, and a debugging checklist. |
| [references/anti-tiling.md](references/anti-tiling.md) | Hiding tiled repetition — Iq 2-sample offset (MF_AntiTile_IqOffset), macro variation, detail overlay, hex tiling, FluidNinja noise textures, UV breaking, and an anti-tiling checklist. |

## Rules

- This card is **passive context only**. It snapshots authoring rules and budgets; it does not call any MCP action. Do not treat any name in these docs as a live action contract — confirm exact `material` namespace actions through **unreal-materials** and its discover-first block.
- Numbers here (16 samplers, instruction budgets, GPU cycles, static-switch and MPC limits) are guidance ceilings for UE 5.7; always confirm real counts on the asset with `get_compilation_stats` via unreal-materials before trusting an estimate.
