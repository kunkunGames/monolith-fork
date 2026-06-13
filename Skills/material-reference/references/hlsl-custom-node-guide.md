# HLSL Custom Node Guide

Reference for authoring **Custom expression** (HLSL) nodes in UE 5.7 materials. The Custom node injects raw HLSL into the generated shader; its body must `return` a value of the declared `output_type`. Add it via `material_query` `create_custom_hlsl_node` / `update_custom_hlsl_node`, or inline in `build_material_graph` `graph_spec.custom_hlsl_nodes[]` as `{ id, code, description?, output_type?, inputs?, additional_outputs?, pos? }`.

## Type rules

- **`MaterialFloat` = `half`.** Use it for platform portability in Custom HLSL (mobile maps it to half precision; PC promotes to float).
- Use `MaterialFloat`/`MaterialFloat2..4` for portable scalars/vectors; reserve full `float` for positions and values needing 32-bit precision.
- Output type must match the node's declared `output_type` (`float`/`float2`/`float3`/`float4`); extra outputs come from `additional_outputs`.
- Inputs declared on the node become local variables of the same name inside the body.

## `FMaterialPixelParameters` (pixel/fragment shader)

Accessible as `Parameters` in the pixel stage.

| Field | Meaning |
|-------|---------|
| `Parameters.TexCoords[i].xy` | UV set `i` (UV0 = `[0]`). Index must be a literal |
| `Parameters.AbsoluteWorldPosition` | World-space position (large-world float3) |
| `Parameters.WorldNormal` | Per-pixel world normal (after normal map) |
| `Parameters.TangentToWorld` | 3x3 tangent->world basis (row [2] = vertex normal) |
| `Parameters.VertexColor` | Interpolated vertex color RGBA |
| `Parameters.CameraVector` | Unit vector from pixel toward camera |
| `Parameters.ScreenPosition` | Clip-space position; `.xy/.w` for screen UV |
| `Parameters.LightVector` | Light direction (light-function/deferred-decal domains) |
| `Parameters.UnMirrored` | +1/-1 mirroring sign for tangent-space fixups |

## `FMaterialVertexParameters` (vertex shader / WPO)

Accessible as `Parameters` in the vertex stage (World Position Offset, Displacement).

| Field | Meaning |
|-------|---------|
| `Parameters.WorldPosition` | Pre-offset world position |
| `Parameters.TangentToWorld` | Vertex tangent basis |
| `Parameters.VertexColor` | Raw vertex color |
| `Parameters.PrevFrameLocalToWorld` | Previous-frame transform (velocity) |

> Pixel-only fields (e.g. `CameraVector`, `ScreenPosition`) are **not** valid in the vertex stage and vice versa.

## `View` / `ResolvedView` uniforms

Prefer `ResolvedView` over `View` so the node works under instanced stereo (VR).

| Uniform | Meaning |
|---------|---------|
| `ResolvedView.GameTime` | Seconds since start (animation driver) |
| `ResolvedView.RealTime` | Wall-clock seconds (ignores pause/dilation) |
| `ResolvedView.ViewSizeAndInvSize` | `(w, h, 1/w, 1/h)` render target size |
| `ResolvedView.WorldCameraOrigin` | Camera world position |
| `ResolvedView.ViewForward / ViewUp / ViewRight` | Camera basis vectors |
| `ResolvedView.PrevFrameGameTime` | Previous-frame time |

## Helper functions

- `Texture2DSample(Tex, TexSampler, UV)` — sample a texture input (the input's `TexSampler` companion is auto-generated).
- `dot`, `cross`, `normalize`, `reflect`, `refract`, `lerp`, `saturate`, `frac`, `pow`, `step`, `smoothstep` — standard HLSL intrinsics.
- `DDX(x)` / `DDY(x)` — screen-space derivatives (pixel stage only).
- `TransformTangentVectorToWorld(Parameters.TangentToWorld, v)` — tangent->world for normal-map style data.

## Gotchas

- **`clip()` disabled on Nanite passes.** Do masked clipping through Opacity Mask, not raw `clip()`, on Nanite meshes.
- **`ddx`/`ddy` return 0 in compute** (and on Nanite/VSM passes) — never rely on derivatives for hashing or branching there.
- Texture-coordinate index inside `TexCoords[i]` must be a compile-time literal, not a runtime variable.
- Custom nodes are opaque to the material editor's instruction estimate; verify real cost with `get_compilation_stats`.
- **GPU cost (cycles):** MAD=4, Division=20, `pow`/`sin`/`cos`=16, `tan`=52 — fold constants and prefer MAD chains.
- Each texture input counts toward the **16-sampler limit** (use Shared:Wrap samplers for many textures).
- No spaces in input/output names; an output `float3` returned where `float` is declared silently truncates.

## Common recipes

```hlsl
// Value noise (cheap, hash-based)
float2 i = floor(UV); float2 f = frac(UV);
float2 u = f*f*(3-2*f);
float a = frac(sin(dot(i,             float2(127.1,311.7)))*43758.5453);
float b = frac(sin(dot(i+float2(1,0), float2(127.1,311.7)))*43758.5453);
float c = frac(sin(dot(i+float2(0,1), float2(127.1,311.7)))*43758.5453);
float d = frac(sin(dot(i+float2(1,1), float2(127.1,311.7)))*43758.5453);
return lerp(lerp(a,b,u.x), lerp(c,d,u.x), u.y); // output_type float
```

| Recipe | Core math |
|--------|-----------|
| **Fresnel** | `pow(saturate(1 - dot(Parameters.WorldNormal, Parameters.CameraVector)), Power)` |
| **Dissolve** | `clip(NoiseTex - Threshold)`; edge glow = `smoothstep(T, T+W, NoiseTex)` (use Opacity Mask on Nanite) |
| **Triplanar** | Blend 3 axis samples by `pow(abs(WorldNormal), Sharpness)`, normalized so weights sum to 1; UVs from `AbsoluteWorldPosition` swizzles |
| **Flow map** | Sample flow vector, advect UV by `flow * frac(GameTime*Speed + phase)`, cross-fade two half-cycle phases to hide the reset |
| **Panner** | `UV + ResolvedView.GameTime * Speed` |

## See also

- `pbr-values.md` — PBR constants for BaseColor/Roughness/Metallic.
- `material-performance.md` — instruction budgets, sampler budget, blend-mode costs.
- `anti-tiling.md` — Iq 2-sample offset and macro variation for noise-driven recipes.
