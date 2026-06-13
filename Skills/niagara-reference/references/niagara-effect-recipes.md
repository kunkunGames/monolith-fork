# Niagara Effect Recipes

Build-block recipes for common game VFX. Each lists emitter layout, sim type, key
modules (by stage), and material. Confirm exact action params with
`monolith_discover({ namespace: "niagara", action: "<action>", mode: "schema" })`.

## Shared rules (apply to every recipe)

- **Module stages:** `Emitter Spawn` -> `Emitter Update` -> `Particle Spawn` -> `Particle Update` -> `Render`.
- **Execution order matters:** Position -> SolveForcesAndVelocity -> Collision/Readers -> Visual.
- **ALWAYS set fixed bounds** (`set_fixed_bounds`). #1 optimization; GPU sim *requires* it.
- **Sim choice:** CPU < 100 particles; GPU > 1,000. GPU is best for fire/sparks/debris; CPU for low counts, complex behavior, Light/Ribbon renderers.
- **Light Renderer & Ribbon Renderer are CPU only.** Events are CPU only (GPU alternative: Attribute Reader, source emitter must run first).
- **Additive materials need no sorting** — set sort mode to None for free perf.
- **Materials are Unlit.** Multiply by the `Particle Color` node so `Particles.Color` drives tint. Fire/glow/sparks = Additive; smoke/dust/fog = Translucent (DepthFade 50-100u).
- For sprite-sheet effects set the SpriteRenderer `SubImageSize` and add a `SubUV Animation` module.

## Fire (5 emitters)

| Emitter | Sim | Renderer | Key modules | Material |
|---------|-----|----------|-------------|----------|
| Flames | GPU | Sprite (SubUV) | ShapeLocation (cone up), AddVelocity (up), CurlNoiseForce, Color over Life (white->orange->black), Scale by Life | Additive, emissive x3-5 |
| Inner core | GPU | Sprite | tight ShapeLocation, short Lifetime, bright additive Color | Additive, hot tint |
| Embers/sparks | GPU | Sprite | low SpawnRate, Gravity Force, Drag, flicker Color | Additive |
| Smoke cap | CPU/GPU | Sprite (SubUV) | rising velocity, large Scale by Life, low opacity | Translucent |
| Light | CPU | Light | low SpawnRate (1-3 lights), Color matched to flame, radius pulse | n/a |

Keep the Light Renderer on its **own CPU emitter** — never on the GPU flame emitter.

## Blood splatter (5 emitters)

| Emitter | Sim | Renderer | Key modules | Material |
|---------|-----|----------|-------------|----------|
| Burst droplets | CPU | Sprite/Mesh | one-shot Spawn Burst Instantaneous, cone velocity, Gravity Force, Drag | Translucent, dark red |
| Spray mist | CPU/GPU | Sprite (SubUV) | high SpawnRate burst, short Lifetime, fade out | Translucent |
| Trails | CPU | Ribbon | spawn from droplets, Ribbon width by life | Translucent |
| Ground decal | CPU | Decal/Sprite | Collision -> kill, spawn pooled splat | Translucent, masked |
| Fine spray | GPU | Sprite | high count, tiny size, gravity | Translucent |

Use Collision (CPU) or GPU collision DI to convert in-flight droplets into ground hits.

## Smoke (1-2 emitters)

- **Sim:** CPU (low count, soft) or GPU for dense columns.
- **Renderer:** SpriteRenderer with SubUV flipbook, camera-facing.
- **Modules:** ShapeLocation (disc/cone), AddVelocity (rising) + CurlNoiseForce for turbulence, Drag, large Scale by Life, Color over Life fading opacity, Rotate sprite (random spin).
- **Material:** Translucent, low opacity (0.3-0.5), DepthFade 50-100u, soft edges.

## Muzzle flash (2-3 emitters)

- **Sim:** CPU (one-shot, short-lived).
- **Emitters:** (1) Flash sprite — Spawn Burst Instantaneous, very short Lifetime (~0.03-0.05s), bright additive, random roll; (2) Smoke puff — Translucent, brief rise; (3) optional Sparks — radial velocity.
- **Modules:** Spawn Burst Instantaneous, InitializeParticle (short lifetime), Color (bright), Scale Sprite Size.
- **Material:** Additive, emissive, often a SubUV flash sheet (set `SubImageSize`, random SubImage index).
- Trigger via a User parameter / Spawn Burst; loop behavior `Once`.

## Sparks (1-2 emitters)

- **Sim:** GPU (high count, simple).
- **Renderer:** SpriteRenderer (stretched/velocity-aligned) or Ribbon (CPU only) for trails.
- **Modules:** Spawn Burst Instantaneous, ShapeLocation (point/cone), AddVelocity (random cone, high speed), Gravity Force, Drag, Color over Life (hot white -> orange fade), Scale by speed.
- **Material:** Additive, emissive, velocity-stretched alignment.
- Note: with `UseVelDistribution=true` the Velocity vector is ignored — speed comes from `Velocity Speed`.

## Explosions (8 emitters)

| Emitter | Sim | Renderer | Role |
|---------|-----|----------|------|
| Core flash | CPU | Sprite | instant bright additive pop |
| Fireball | GPU | Sprite (SubUV) | expanding flame ball, Scale by Life |
| Shockwave | CPU | Mesh/Sprite | single expanding ring, distortion material |
| Smoke plume | GPU | Sprite (SubUV) | rising lingering smoke, translucent |
| Sparks | GPU | Sprite | radial high-speed, gravity + drag |
| Debris | GPU | Mesh | chunks, gravity, collision, angular velocity |
| Embers | GPU | Sprite | slow drifting glow, flicker |
| Light flash | CPU | Light | one bright pulse, fast falloff |

Drive all with Spawn Burst Instantaneous, `Once` loop. Heat-haze on the shockwave uses a distortion/refraction material.

## Dust / debris (2-3 emitters)

- **Sim:** GPU for debris chunks; CPU/GPU for dust.
- **Emitters:** (1) Dust cloud — Sprite SubUV, rising/spreading, Translucent, large Scale by Life; (2) Debris — MeshRenderer, Gravity Force, Drag, Collision, random angular velocity; (3) optional settling dust.
- **Modules:** ShapeLocation (disc on ground), AddVelocity (outward + slight up), Gravity Force, Drag, Curl Noise (dust drift), Color over Life.
- **Material:** Dust Translucent (DepthFade); debris uses the source surface material.

## Rain (1-2 emitters)

- **Sim:** GPU (high count, simple fall).
- **Emitters:** (1) Falling streaks — Sprite velocity-stretched or Ribbon; (2) Splash — spawned on collision (Collision module + event, CPU) or a separate looping ground-impact emitter.
- **Modules:** ShapeLocation (box volume above camera, often bound to camera position via User param), constant downward Velocity, Drag minimal, Collision -> spawn splash, large fixed bounds.
- **Material:** Translucent, faint, velocity-aligned streak; splash uses a small SubUV ring.

## Magic / energy (2-4 emitters)

- **Sim:** GPU for swirling particles; CPU if Ribbon trails are needed.
- **Emitters:** (1) Core glow — additive Sprite, pulsing Scale/Color; (2) Orbiting particles — CurlNoiseForce or VortexForce around origin; (3) Ribbon trails (CPU) following particles; (4) optional Light.
- **Modules:** ShapeLocation (sphere/ring), CurlNoiseForce / Vortex Velocity, Color over Life (saturated emissive), Scale by Life, sprite rotation.
- **Material:** Additive, emissive x3-5, optional erosion/distortion via Dynamic Parameter channels (4 slots x 4 = 16 floats) bound to `Particles.DynamicMaterialParameter`.

## Quick build flow

1. `create_system` -> `add_emitter` per layer.
2. Per emitter: set sim target, `set_fixed_bounds`, `set_spawn_shape`, add force/color/scale modules via `add_module`, set inputs with `set_module_input_value`.
3. Material agent creates materials FIRST, then `set_renderer_material` per renderer.
4. For bursts use `set_emitter_loop_profile` (`Once`); for ambient loops use `Infinite` and consider a Lightweight (stateless) emitter for zero tick cost.
5. `validate_system` -> `request_compile` -> `preview_system`.
