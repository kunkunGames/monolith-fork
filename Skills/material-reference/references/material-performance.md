# material-reference — Material Performance

Budgets, cycle costs, and cost tradeoffs for UE 5.7 materials. Measure real counts with
`material_query("get_compilation_stats", { asset_path })` (VS/PS instructions, samplers,
compile status) before and after changes; never trust an estimate over the compiler.

## Instruction Budgets (pixel shader)

| Material kind | Target | Notes |
|---------------|--------|-------|
| Opaque | < 150 | Most surface/world materials live here |
| Translucent | < 200 | Lit translucency is more expensive per pixel |
| Post-process | < 100 | Runs full-screen — every instruction is paid per pixel |

These are guidance ceilings, not hard limits. Translucency and post-process pay their cost
over many overlapping/full-screen pixels, so overdraw matters as much as instruction count.

## GPU Cycle Costs (relative, per scalar op)

| Operation | ~Cycles |
|-----------|---------|
| MAD / Add / Mul (`a*b+c`) | 4 |
| Pow, Sin, Cos | 16 |
| Division | 20 |
| Tan | 52 |

Prefer MAD-friendly math. Reciprocals (`1/x`, `Divide`) and transcendentals (`Pow`, `Sin`,
`Tan`) are expensive — fold them into texture lookups or precompute constants where possible.
A `Lerp` is two MADs; cheap. Normalize is a dot + rsqrt + 3 muls.

## Sampler Budget

- **16 texture samplers per material** (the hardware/shader limit).
- Each `TextureSample` with its own sampler counts toward the 16.
- To exceed 16 textures, set sampler source to **Shared: Wrap** / **Shared: Clamp** — shared
  samplers do not consume a per-material sampler slot (they draw from a global pool).
- Channel-packed maps (ORM/ARM/MRA = AO+Roughness+Metallic in RGB) collapse three lookups
  into one sampler — the cheapest way to stay under budget.
- Virtual Textures use one shared sampler stack regardless of layer count.

## Static Switch Math

- Each `StaticSwitchParameter` **doubles** the shader permutation count: N switches → 2^N
  permutations compiled and cached.
- Keep **≤ 6 static switches per master material** (64 permutations) as a working ceiling.
- Static switches are **free at runtime** — the dead branch is compiled out, so cost lives in
  compile time, DDC size, and cook/iteration time, not the GPU.
- `StaticBool` / static switch = compile-time branch (free at runtime, costs permutations).
  Scalar param + `Lerp`/`If` = dynamic branch (both sides may execute; no permutation cost).

## Material Parameter Collections

- **2 Material Parameter Collections (MPC) per material** max.
- MPCs are global uniforms shared across materials — cheap to read, ideal for time-of-day,
  wetness, global wind, gameplay-driven globals. Do not use them for per-instance values.

## Blend Mode Costs (cheap → expensive)

| Blend mode | Cost profile |
|------------|--------------|
| Opaque | Cheapest — writes depth, no sorting, no overdraw blending |
| Masked | Opaque cost + alpha test; `clip()` can disable early-Z on some HW |
| Additive / Modulate | No depth write; overdraw-bound |
| Translucent | Lit translucency, sorted, overdraw-bound; most expensive |
| AlphaComposite | Premultiplied-alpha translucency |

- **Render-target / UI alpha:** never use `BLEND_Translucent` when you need correct stored
  alpha — use **`BLEND_AlphaComposite`** (premultiplied) so alpha composites correctly.
- Masked is usually cheaper than Translucent for foliage/decals because it keeps depth writes
  and avoids per-pixel sorting, but heavy `clip()` use can hurt on TBDR/Nanite passes.

## CPD vs DMI (per-instance variation)

| Approach | Batching | Use when |
|----------|----------|----------|
| **Custom Primitive Data (CPD)** | **Preserved** — instances stay batched | Per-instance scalars/vectors (tint, wetness, health) |
| **Dynamic Material Instance (DMI)** | **Broken** — each DMI is a unique material → its own draw call | You must change textures/static switches, or change > the CPD float budget |

**Prefer CPD.** Creating a `CreateDynamicMaterialInstance` per actor breaks instanced-static-mesh
and draw-call batching. CPD feeds floats straight into the existing material via the primitive,
keeping draws merged. Reach for a DMI only when you need to swap a texture or static switch that
CPD cannot express.

## Texture vs Math Tradeoffs

- **Bake to texture** when math is expensive, static across UVs, and you have VRAM/bandwidth:
  gradients, curves, baked AO, complex masks. One sampler often beats 30+ instructions.
- **Compute in math** when the surface is memory/bandwidth-bound, the value is dynamic, or the
  texture would be near-uniform. Cheap procedural noise/fresnel can beat a sampler fetch.
- Rule of thumb: a texture fetch trades ALU for bandwidth + a sampler slot. On modern GPUs the
  bottleneck is usually bandwidth/overdraw, so do not blindly bake — profile both.
- Lower mip/resolution and good compression (BC5 for normals, BC1/BC7 for color) cut bandwidth
  more than shaving a few instructions.

## HLSL / Custom Node Cost Notes

- **`MaterialFloat` == `half`** — use it (not `float`) in Custom HLSL for platform portability
  and to let mobile/half-precision paths run faster.
- **`clip()` is disabled on Nanite passes**; do not rely on it for Nanite-rendered geometry.
- **`ddx`/`ddy` return 0 in compute/Nanite material passes** — screen-space derivatives are not
  available there, so derivative-based effects (mip bias, edge AA) silently break.

## Debugging & Measurement Tools

| Tool | What it shows |
|------|---------------|
| `material_query("get_compilation_stats", { asset_path })` | VS/PS instruction counts, sampler count, compile status |
| `material_query("batch_recompile", { asset_paths })` | Recompile many, returns instruction counts (max 200) |
| Shader Complexity viewmode | Green→red overdraw/instruction heat map in the viewport |
| `r.ShaderComplexity.*` cvars | Tune the complexity viewmode thresholds |
| Material Editor **Stats** panel | Live instruction/sampler counts while editing the graph |
| GPU Visualizer / `ProfileGPU` (Ctrl+Shift+,) | Per-pass GPU timing to find the real frame cost |

Workflow: snapshot `get_compilation_stats` before edits → make the change → re-run stats and
confirm instruction/sampler deltas → confirm in Shader Complexity / ProfileGPU that the pixel
cost actually dropped, since instruction count alone ignores overdraw and bandwidth.
