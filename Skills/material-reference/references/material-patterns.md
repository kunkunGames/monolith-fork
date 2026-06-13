# material-reference — Material patterns

Recipe card for common surface/effect materials. Wiring uses the `material`
namespace (`build_material_graph`, `set_material_property`); confirm exact action
params with `monolith_discover({ namespace: "material", action: "<action>", mode: "schema" })`.

## Wet surface (Lagarde)

From the Lagarde "moving frostbite to PBR" wetness model. Apply a wetness mask `0..1`, then:

| Channel | Operation |
|---------|-----------|
| Roughness | `Roughness * lerp(1.0, 0.3, wetness)` — water fills micro-cavities, smoother |
| BaseColor | `pow(BaseColor, lerp(1.0, 2.0, wetness))` — darken (BaseColor squared at full wet) |
| Saturation | boost slightly with wetness — wet surfaces read more saturated |
| Specular | porous materials gain reflectance; nudge Specular toward 0.5 |

Pooling: drive wetness from a worldspace height/cavity mask so water collects in crevices. Keep Metallic untouched (water is dielectric).

## Subsurface (blood / skin)

- Shading model `Subsurface` or `Preintegrated Skin` (`set_material_property shading_model`). `Subsurface Profile` for skin gives the best falloff.
- Plug a warm interior tint into **Subsurface Color** (skin: desaturated red; blood: deep crimson). Opacity controls SSS thickness for `Subsurface`.
- Blood: low Roughness (wet sheen ~0.15–0.30), dark red BaseColor, thin SSS for translucent edges; thicken Roughness as it dries.

## Dissolve

- Sample noise, `clip(noise - Threshold)` (or feed Opacity Mask). Animate `Threshold` 0→1.
- Burn edge: `smoothstep` a thin band around the threshold into **Emissive** (hot color) for a glowing rim.
- Mask blend modes: `clip()` is disabled on Nanite passes — use `Masked` blend mode opacity, not raw `clip` in a Nanite material.

## Emissive pulse

- `Emissive = Color * (Base + Amplitude * (0.5 + 0.5 * sin(Time * Speed)))`. Keep `Base > 0` to avoid full-black troughs.
- Drive HDR intensity above 1.0 for bloom. Prefer a scalar parameter for `Speed`/`Amplitude` so instances tune it without recompiles.

## Damage overlay

- Blend a damage layer over the base via a mask: `lerp(BaseColor, DamageColor, mask)` on BaseColor, Normal, Roughness together.
- Drive the mask by a scalar `DamageAmount` against a wear/dirt texture (`step`/`smoothstep`) so damage reveals progressively. Use **Custom Primitive Data** for per-instance `DamageAmount` to keep draw-call batching (DMIs break it).

## Decals

- `material_domain: DeferredDecal`, `blend_mode: Translucent`, `opacity_from_alpha: true` (see unreal-materials `create_pbr_material_from_disk`).
- Decal Blend Mode `Translucent` writes BaseColor/Normal/Roughness; `DBuffer` variants are required if the receiving surface needs decals before base-pass lighting. Sort with Decal Sort Order.
- Blood-trail / grime decals: project onto floors via the decal actor; keep textures power-of-two.

## Masked vs Translucent

| | Masked | Translucent |
|---|--------|-------------|
| Edges | hard (1-bit, alpha-test) | soft (alpha blend) |
| Cost | cheaper, writes depth | pricier, sorting/overdraw |
| Lighting | full deferred | limited / forward |
| Use for | foliage, chain-link, cutouts | glass, smoke, fades |

RT alpha gotcha: **never** use `BLEND_Translucent` for render-target alpha — use `BLEND_AlphaComposite`.

## Degradation (wear / corrosion)

- Layer dirt/rust/grime via masks driven by curvature (edges wear first) and worldspace height (dirt settles low).
- **Rust is NOT metallic** — iron oxide is dielectric, set Metallic=0.0 on rusted areas even over metal.
- Keep non-metal BaseColor within sRGB 30–240 (no pure black/white albedo).

## Dark environment

- Avoid Metallic=0 + Roughness=1 pure-black albedo; floor non-metal BaseColor at sRGB ~30 so it reads under low light.
- Lean on subtle Emissive accents and Specular highlights for readability rather than raising ambient. Fresnel rim helps silhouettes in the dark.

## Hologram

- `Unlit` shading model, `Translucent` blend. Emissive = scanline pattern (UV.y vs `frac(Time)`) × tint, modulated by Fresnel for edge glow.
- Add UV jitter/glitch via noise on Opacity; flicker with a `sin`/noise-driven scalar. Keep additive feel by feeding Emissive, low Opacity.

## POM (Parallax Occlusion Mapping)

- Use a heightmap to ray-march UV offset along the view vector (tangent space) for parallax depth without geometry. UE node: `Parallax Occlusion Mapping` material function.
- Cost scales with sample count (`MinSteps`/`MaxSteps`) — budget it; expensive per-pixel. Pair with a real Normal map.
- Silhouette is NOT displaced (flat edges); for true edges use tessellation/Nanite displacement instead.

## Cost reminders

- Instruction budgets: Opaque < 150, Translucent < 200. Pow/Sin/Cos ≈ 16 cycles, Division ≈ 20, Tan ≈ 52, MAD ≈ 4.
- Sampler limit 16/material (use `Shared:Wrap`). Each static switch doubles permutations (max ~6 per master).
- Always `validate_material` and `get_compilation_stats` after graph edits.
