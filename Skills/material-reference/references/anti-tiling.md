# Anti-Tiling Reference

Techniques to hide repetition in tiled/terrain/organic materials. Apply the cheapest
technique that solves the visible repeat; layer only when one is not enough. Always
preview at high repetition with `render_preview` (`uv_tiling: 3` or `5`) before finalizing.

## Technique Selection

| Technique | Cost | Best for | Trade-off |
|-----------|------|----------|-----------|
| Macro variation | ~5-10 instr + 1 sampler | Any tiled surface (first thing to add) | Hides large-scale repeat, not the tile seam itself |
| Detail overlay | ~10 instr + 1 sampler | Close-up surfaces lacking fine grain | Adds a sampler; pick a non-correlated detail map |
| Iq 2-sample offset (`MF_AntiTile_IqOffset`) | ~15 instr, 2 taps | Organic / terrain / noise-like textures | Doubles texture taps; blurs sharp features slightly |
| Hex (Voronoi) tiling | High (3 taps + blend) | Large surfaces with strong directional patterns | 3x sampling cost; overkill for subtle textures |

## Macro Variation (do this first)

Break up large-scale repetition with a low-frequency, world-space noise overlay so the
pattern does not read as a uniform grid at distance.

- BaseColor: multiply/overlay noise, strength **0.1-0.3**.
- Roughness: multiply by a noise remapped to a **0.8-1.2** range.
- Drive noise UVs from `WorldPosition * 0.0003-0.001` so variation is independent of the
  tiling UV scale and stays stable across mesh instances.
- Recommended textures (in-project FluidNinja set):
  - Color/BaseColor: `/Game/FluidNinjaLive/Textures/T_LowResBlurredNoise_sRGB`
  - Roughness: `/Game/FluidNinjaLive/Textures/T_MultilevelNoise1`

## Detail Overlay

Add high-frequency detail at close range by blending a second, finer texture (or detail
normal) at a higher UV scale than the base. Choose a detail map uncorrelated with the base
so the two repeats do not align into a new visible pattern. Fade detail contribution by
camera distance to avoid aliasing far away.

## Iq 2-Sample Offset

Inigo Quilez's technique: sample the texture twice at UVs offset by a low-frequency noise
lookup, then blend the two samples. This is the cheapest *proper* anti-tiling for organic
and terrain textures and is packaged as `MF_AntiTile_IqOffset` (~15 instructions).

- Use a smooth noise source to drive the per-pixel UV offset; large offset = more variation
  but more feature smearing.
- Best on noise-like content (dirt, rock, moss); avoid on textures with sharp man-made
  features where blended duplicates look like ghosting.
- Pair with macro variation: Iq removes the seam, macro variation removes the distant grid.

## Hex (Voronoi) Tiling

Partitions UV space into a hexagonal grid, samples the texture with a per-cell random
rotation/offset in 3 overlapping cells, and blends by distance weight. Effectively removes
all visible repetition, including strong directional patterns, at ~3x sampling cost. Reserve
for large hero surfaces (terrain, big walls) where cheaper methods still show a pattern.

## UV Breaking

Base UVs must not feed a `TextureSample` untransformed on a tiled material. Break them with
a low-frequency noise offset or a world-position blend so identical UV neighborhoods do not
sample identical texels. This is the precondition that makes the techniques above effective.

## Texture Recommendations

- Keep tiling source textures power-of-two; pick resolution to hold detail at the in-world
  texel density, not higher.
- Use a tileable (seamless) base texture — anti-tiling hides *repetition*, not hard seams
  baked into a non-tileable source.
- Reuse one shared low-frequency noise texture for macro variation across materials to keep
  the look consistent and save samplers.
- Watch the **16-sampler** per-material limit: each technique above adds taps. Use
  `Shared:Wrap` sampler source when you exceed the dedicated-sampler budget.

## Anti-Tiling Checklist

Verify ALL before finalizing a material that uses tiling textures:

1. **Macro variation applied?** World-space noise overlay on BaseColor (0.1-0.3) and
   Roughness (x0.8-1.2), UVs from `WorldPosition * 0.0003-0.001`.
2. **UVs broken?** Base UVs pass through a noise offset or world-position blend, not straight
   into `TextureSample`.
3. **Previewed at 3x?** `render_preview` with `uv_tiling: 3` — the grid must not be obvious.
4. **Iq offset for organic/terrain?** Apply `MF_AntiTile_IqOffset` (~15 instr) on noise-like
   surfaces; escalate to hex tiling on large surfaces with stubborn patterns.
5. **FluidNinja noise textures wired?** Color via `T_LowResBlurredNoise_sRGB`, roughness via
   `T_MultilevelNoise1`.
6. **Sampler budget OK?** Total taps within 16; switch overflow textures to `Shared:Wrap`.

Run `check_tiling_quality` to flag missing anti-tiling / macro variation automatically.
