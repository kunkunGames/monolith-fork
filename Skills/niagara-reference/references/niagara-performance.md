# Niagara Performance Reference

Optimization budgets and cost drivers for Niagara systems. Profile with `stat GPU`,
`stat Niagara`, and the Niagara Debugger (`fx.Niagara.Debug.Hud 1`) before and after changes.

## Particle Count Budgets

- **CPU sim:** target < 100 particles per emitter. Best for low counts, complex
  behavior, and effects needing the Light or Ribbon renderer.
- **GPU sim:** use for > 1,000 particles per emitter. Best for high counts with
  simple per-particle behavior (fire, sparks, debris).
- **In-between (100–1,000):** test both — the crossover depends on per-particle
  module cost, not just count.
- Set explicit spawn caps. Cap with `Spawn Rate` / `Spawn Burst` plus a particle-count
  scalability override on the effect type rather than relying on lifetime alone.

## Fixed Bounds — #1 Optimization

- **ALWAYS set fixed bounds.** This is the single highest-impact Niagara optimization.
- **GPU sim REQUIRES fixed bounds** — GPU emitters cannot read back particle positions
  to compute dynamic bounds. Dynamic bounds on a GPU emitter is an error.
- Dynamic bounds force a CPU readback / recompute every frame; fixed bounds skip it.
- Set via `set_fixed_bounds` (system or per-emitter) or the `set_emitter_property`
  bounds mode. Size bounds to the real effect extent — oversized bounds defeat culling,
  undersized bounds clip particles.

## Emitter Count Impact

- **1 emitter × 1,000 particles beats 10 emitters × 100 particles.** Each emitter
  carries fixed dispatch/tick overhead (especially on GPU sim), so consolidate where
  the behavior allows.
- Disable rather than delete unused emitters while iterating (`set_emitter_enabled`),
  but ship with dead emitters removed — disabled emitters still load.
- Prefer one parameterized emitter over several near-duplicates.

## Lightweight (Stateless) Emitters

- **Lightweight emitters (UE 5.4+) have near-zero tick cost** for simple ambient
  effects (sparkles, embers, drifting motes).
- They run a stateless, precomputed motion model instead of a full per-frame sim —
  no simulation stages, no events. Use them for decorative loops that never need
  collision, GPU readback, or inter-emitter communication.

## Overdraw Reduction

- Overdraw (translucent pixels stacked on each other) is usually the dominant GPU cost
  for sprite VFX, not particle count.
- Reduce by: fewer/smaller particles, tighter alpha (trim soft-edge falloff), shorter
  lifetimes, and avoiding large camera-filling sprites.
- Cull distant/offscreen instances through the effect type scalability and distance
  culling rather than spawning particles the player never sees.
- Mesh particles with opaque/masked materials avoid translucent overdraw entirely where
  the look allows.

## Sort Mode Costs

- **Additive materials do not need sorting** — order-independent. Set the renderer
  sort mode to **None** for free performance.
- Translucent (alpha-blend) materials need view-depth sorting for correct layering;
  sorting cost scales with particle count.
- Only pay for sorting when blend order is visible. Default unsorted additive sprites
  whenever the art direction permits.

## Mesh vs Sprite

- **Sprites:** cheap per particle, but translucent sprites drive overdraw; good for
  high counts and soft volumetric looks (smoke, fire, glow).
- **Mesh particles:** higher per-particle vertex/shader cost, but support opaque/masked
  materials (no overdraw) and real geometry (debris, shards, projectiles).
- Keep mesh-particle source meshes low-poly with a tight LOD0 — vertex cost multiplies
  by live particle count.
- For dense detail, a single mesh emitter with a low-tri mesh often beats thousands of
  overdrawing sprites.

## Shader / Material Budget

- Particle materials should be **Unlit** shading model — lit particles cost far more
  and rarely read correctly.
- Keep particle material instruction counts low; they execute per pixel × overdraw.
- Use Dynamic Parameters (4 slots × 4 channels = 16 floats) to drive material variation
  from a shared master material instead of authoring many material variants.
- Reuse one master particle material with instances rather than unique materials per
  effect, to keep shader permutations and PSO cost down.

## Scalability Integration

- Assign a `UNiagaraEffectType` (`set_effect_type`) so the system honors project
  scalability and quality levels.
- Configure per-quality-level scalability (`set_scalability_settings`): max distance,
  per-quality spawn-count / instance-count scale, and significance-based culling.
- Use distance culling and significance handling so low-end / far effects spawn fewer
  particles or cull entirely.
- Effect-type culling reactions (cull distance, max instances) cap the worst case when
  many systems play at once.

## Collision Costs

- Particle collision is one of the most expensive per-particle features — enable it
  only on emitters that visibly need it.
- **CPU collision** (scene/analytic) reads the world per particle and scales poorly with
  count; reserve it for low-count emitters.
- **GPU collision** uses the depth buffer (cheaper, but screen-space only — misses
  offscreen and backface geometry).
- Cheaper alternatives: analytic plane collision, a fixed kill-plane, or a short
  lifetime, instead of full scene collision when exact contact is not required.

## Quick Checklist

- [ ] Fixed bounds set (required on GPU sim)
- [ ] Right sim target for the count (CPU < 100, GPU > 1,000)
- [ ] Emitters consolidated; disabled/dead emitters removed
- [ ] Additive renderers sort mode = None
- [ ] Unlit particle materials, low instruction count, shared master + Dynamic Params
- [ ] Effect type assigned with per-quality scalability + distance/significance culling
- [ ] Collision enabled only where visibly needed
