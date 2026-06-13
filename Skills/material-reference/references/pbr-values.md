# PBR Values Reference

PBR base values by material category for UE 5.7. Use as authoring targets, then break up with texture variation. All sRGB values below assume a color picker in sRGB/0-255 space.

## Key PBR Rules

- **Metallic is binary in authoring.** A surface is either a raw metal (Metallic = 1.0) or a dielectric (Metallic = 0.0). Use intermediate values only on a mask edge between the two (e.g. a metal worn through paint), never as a "kind of metallic" dial.
- **Rust is NOT metallic.** Iron oxide (rust) is a dielectric: Metallic = 0.0. Same for paint, dirt, and most corrosion on top of metal.
- **Non-metal BaseColor range:** never below sRGB 30, never above sRGB 240. Real dielectrics are not pure black or pure white; clamping to this range keeps lighting believable.
- **Metal color lives in BaseColor.** For Metallic = 1.0 surfaces, BaseColor IS the specular/reflected color; albedo/diffuse is effectively black.
- **Roughness drives the look more than color.** Most material identity (wet, polished, dusty, worn) reads through Roughness variation, not BaseColor.
- **Wet surfaces (Lagarde):** multiply Roughness by ~0.3, square the BaseColor (darken), and boost saturation. Water in cavities makes them smoother and darker.
- **Default dielectric Specular = 0.5** (≈4% F0). Leave it at 0.5 unless authoring a specific known F0; do not use Specular as a brightness knob.

## Metals

Metals: Metallic = 1.0, color in BaseColor. Polished metal is smooth (low Roughness); oxidized/brushed metal is rougher.

| Material | BaseColor (sRGB) | Metallic | Roughness | Notes |
|----------|------------------|----------|-----------|-------|
| Iron / steel (polished) | 196, 199, 199 | 1.0 | 0.10-0.25 | Neutral grey, slightly desaturated |
| Worn / brushed steel | 180, 182, 185 | 1.0 | 0.35-0.55 | Roughen and add directional streaks |
| Aluminum | 233, 235, 236 | 1.0 | 0.10-0.30 | Bright neutral, cool |
| Gold | 255, 215, 130 | 1.0 | 0.05-0.20 | Strong warm tint |
| Copper | 250, 170, 120 | 1.0 | 0.10-0.30 | Orange-pink |
| Brass | 215, 185, 115 | 1.0 | 0.15-0.35 | Muted warm yellow |
| Chrome (mirror) | 200, 200, 200 | 1.0 | 0.02-0.08 | Near-mirror reflections |

## Corrosion / Degradation

Corrosion sits ON TOP of metal but is itself dielectric. Author as a blend driven by a mask: metal underneath (Metallic 1.0) transitioning to the corrosion layer (Metallic 0.0).

| Material | BaseColor (sRGB) | Metallic | Roughness | Notes |
|----------|------------------|----------|-----------|-------|
| Rust (iron oxide) | 110, 55, 30 | 0.0 | 0.7-0.95 | Dielectric; orange-brown, rough and matte |
| Patina (copper) | 90, 150, 130 | 0.0 | 0.6-0.85 | Green-blue verdigris over copper |
| Tarnish / oxidation film | 60, 60, 65 | 0.3-0.8 | 0.45-0.7 | Thin film; partial metallic on the edge mask only |
| Dirt / grime | 50, 42, 35 | 0.0 | 0.85-1.0 | Pure dielectric overlay, very rough |

## Organics / Horror

Blood, bone, and flesh. Wet versions use subsurface/translucent shading and lower Roughness; dried versions are dielectric and rough. See material-patterns for subsurface blood/skin setups.

| Material | BaseColor (sRGB) | Metallic | Roughness | Notes |
|----------|------------------|----------|-----------|-------|
| Fresh blood (wet) | 90, 5, 5 | 0.0 | 0.10-0.25 | Dark red; wet sheen, optional thin SSS |
| Pooled blood (deep) | 50, 3, 3 | 0.0 | 0.05-0.15 | Square BaseColor in cavities; very low Roughness |
| Dried blood | 70, 25, 20 | 0.0 | 0.7-0.9 | Brown-red, matte, slightly crusted |
| Flesh / skin | 200, 140, 130 | 0.0 | 0.35-0.55 | Subsurface shading; warm, soft |
| Exposed muscle / viscera | 150, 40, 45 | 0.0 | 0.25-0.45 | Wet, saturated red; subsurface |
| Bone (clean) | 225, 215, 190 | 0.0 | 0.45-0.65 | Off-white, slightly warm |
| Bone (old / stained) | 180, 160, 120 | 0.0 | 0.55-0.75 | Yellowed, dustier and rougher |

## Building Materials

Architectural dielectrics. Metallic = 0.0 across the board (except metal fixtures). Roughness carries most of the variation.

| Material | BaseColor (sRGB) | Metallic | Roughness | Notes |
|----------|------------------|----------|-----------|-------|
| Concrete | 130, 128, 125 | 0.0 | 0.7-0.9 | Neutral grey; break up with stains/cracks |
| Brick | 140, 70, 55 | 0.0 | 0.7-0.9 | Mortar lighter and rougher than brick face |
| Plaster / drywall | 200, 198, 192 | 0.0 | 0.6-0.85 | Light, matte; near upper BaseColor clamp |
| Wood (painted) | per paint | 0.0 | 0.4-0.7 | Sheen depends on paint finish |
| Wood (bare / weathered) | 120, 95, 65 | 0.0 | 0.6-0.85 | Grain direction in Roughness/Normal |
| Glass | 230, 235, 240 | 0.0 | 0.0-0.05 | Translucent; very smooth, high specular |
| Ceramic tile (glazed) | per tile | 0.0 | 0.05-0.20 | Glaze is smooth and reflective |

## Environment

Natural ground and terrain surfaces, all dielectric.

| Material | BaseColor (sRGB) | Metallic | Roughness | Notes |
|----------|------------------|----------|-----------|-------|
| Rock / stone | 110, 105, 100 | 0.0 | 0.7-0.9 | Desaturated; tint per rock type |
| Dirt / soil | 85, 65, 45 | 0.0 | 0.85-1.0 | Very rough, matte |
| Mud (wet) | 55, 42, 30 | 0.0 | 0.2-0.4 | Darkened + smoothed by water |
| Sand | 200, 178, 140 | 0.0 | 0.6-0.8 | Light warm; subtle sparkle optional |
| Grass / foliage | 70, 110, 45 | 0.0 | 0.5-0.7 | Often two-sided; subsurface for backlight |
| Snow | 235, 238, 240 | 0.0 | 0.4-0.6 | Near upper clamp; slight subsurface/sparkle |
| Standing water | 40, 50, 55 | 0.0 | 0.0-0.1 | Translucent; flat smooth surface |

## Quick Checklist

1. Decide metal vs dielectric first — set Metallic to 1.0 or 0.0, not in between.
2. Clamp non-metal BaseColor to sRGB 30-240.
3. Get the Roughness range right before fine-tuning color.
4. Layer corrosion/dirt as a dielectric mask over metal, not by lowering Metallic globally.
5. For wet/horror surfaces, drop Roughness and darken BaseColor (square it) where water pools.
