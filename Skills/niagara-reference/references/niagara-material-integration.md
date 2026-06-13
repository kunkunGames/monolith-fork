# Niagara Material Integration

How particle materials connect to Niagara renderers (UE 5.7). The material agent creates
materials FIRST; you assign them to renderers afterward with `set_renderer_material`.

## Particle Color

- Material must multiply its color by a **Particle Color** node so Niagara can drive it.
- Set `Particles.Color` via the `Color` module or `set_module_input_value`. The material's
  `Particle Color` node reads this automatically — no renderer binding needed.
- `Particle Color` exposes RGBA; the Alpha channel feeds Opacity on translucent/additive blends.
- On a Sprite/Mesh renderer the `Color` attribute binding maps `Particles.Color` to the node;
  rename it via `set_renderer_binding` only if you store color under a non-default attribute.

## Blend Modes (shading model: always Unlit for particles)

| Use | Blend | Typical setup |
|-----|-------|---------------|
| Fire, glow, sparks, energy | **Additive** | Emissive × 3-5, tight radial gradient (power 2-3) |
| Smoke, fog, dust | **Translucent** | Opacity × 0.3-0.5, DepthFade 50-100u |
| Hard-edged decals/masks | Masked | clip()/OpacityMask; no per-pixel sorting cost |

- **Additive needs no sorting.** Set the renderer sort mode to `None` for free perf.
- Translucent particles DO sort; keep counts down or accept the sort cost.
- Convention: particle materials live at `/Game/VFX/Materials/M_<Effect>Particle`. Verify the
  material exists (`project search M_FireParticle`) before assigning it to a renderer.

## Dynamic Parameters (4×4 slots)

- **4 slots × 4 channels = 16 floats** total for Niagara→material communication.
- In the material, add a `DynamicParameter` node (index 0-3 selects the slot); its R/G/B/A pins
  are the four channels.
- Drive them by binding `Particles.DynamicMaterialParameter` (slot 0) — and the indexed
  variants for slots 1-3 — then writing those attributes from a module.
- Use for per-particle erosion, intensity, dissolve threshold, SubUV blend, mask params, etc.
- Each Dynamic Parameter slot must be enabled in the material and bound on the renderer to be
  evaluated; unused slots cost nothing.

## Sub-UV / Flipbook

- Set the renderer's **`SubImageSize`** property to the sheet grid, e.g. `"4,4"` for a 4×4 sheet
  (`set_renderer_property ... property: "SubImageSize", value: "4,4"`).
- Drive the frame with the **`SubUV Animation`** module (linear playback or curve-driven).
- In the material, the `Particle SubUV` node (or TextureSample fed by SubUV UVs) samples the
  current cell. Sub-UV blending between frames uses an extra Dynamic Parameter channel as the
  lerp alpha when smooth interpolation is required.
- Keep sheets power-of-two; match `SubImageSize` exactly to the authored grid or frames tear.

## Soft Particles (depth fade)

- Soft particles fade where the sprite intersects opaque scene geometry, killing hard edges.
- Use a **DepthFade** node in the material (Translucent blend), fade distance ~50-100u for smoke.
- For **textureless** soft edges, request a procedural radial gradient (Custom HLSL) from the
  material agent instead of an alpha texture — a soft falloff disc with no sampler cost.

## Distortion / Heat Haze

- Author as a refraction/distortion material (Translucent), typically driven by a normal or
  flow texture; the scene behind the particle is offset by the distortion vector.
- Keep distortion subtle and counts low — refraction reads the scene color/depth and is overdraw
  heavy. Pair with a short particle lifetime for impact/heat-shimmer bursts.
- Distortion strength is a good Dynamic Parameter channel so a single material serves many FX.

## Camera Facing Modes (Sprite renderer)

`FacingMode` controls how sprites orient toward the view:

| Mode | Behavior |
|------|----------|
| `FaceCamera` | Full billboard — always faces the camera position |
| `FaceCameraPlane` | Faces the camera plane (no per-sprite tilt; stable for sheets) |
| `Velocity` | Aligns sprite to the particle velocity vector (streaks, sparks) |
| `CustomFacingVector` | Orients to a bound facing vector attribute |
| `FaceCameraPosition` / `FaceCameraDistanceBlend` | Position-based / distance-blended facing |

- For 2D sprite/billboard VFX captured from a fixed camera, prefer a stable plane-facing mode and
  frame the effect from the player-facing camera direction before capture.
- `Alignment` (Unaligned / VelocityAligned / CustomAlignment) sets the sprite's up-axis roll
  independently of facing — combine VelocityAligned + FaceCameraPlane for camera-stable streaks.

## Material Usage Flags

A material only compiles for a renderer if the matching usage flag is set, or it falls back to
the default material and the effect renders wrong:

- **`Used with Niagara Sprites`** — required for SpriteRenderer materials.
- **`Used with Niagara Meshes`** — required for MeshRenderer materials.
- **`Used with Niagara Ribbons`** — required for RibbonRenderer materials.
- Setting an unsupported flag combination, or assigning a material missing the flag, surfaces as a
  validation/compile warning — run `validate_system` to catch missing-material issues pre-compile.

## Quick Checklist

1. Material agent creates `M_<Effect>Particle`: Unlit, correct blend, `Particle Color` multiply,
   the right `Used with Niagara *` flag, and any `DynamicParameter` nodes you need.
2. `set_renderer_material` to assign it to the emitter's renderer.
3. Drive `Particles.Color` for tint/alpha; bind `Particles.DynamicMaterialParameter` for the rest.
4. For sheets: set `SubImageSize` + add the `SubUV Animation` module.
5. Additive → sort `None`; Translucent → DepthFade for soft edges, keep counts low.
6. `validate_system` to confirm no missing materials / flag mismatches before saving.
