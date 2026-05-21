---
name: unreal-imagegen
description: Use for AI image/texture generation workflows exposed by Monolith MCP. Triggers on image gen, imagegen, generate texture, AI texture, generate image, texture synthesis.
---

# unreal-imagegen

**6 actions** via `imagegen_query(action, params)`. Action names below are the live registry surface; call `monolith_discover` for exact parameter schemas.

## Discovery

```
monolith_discover({ namespace: "imagegen" })                      # all actions in this namespace
monolith_discover({ namespace: "imagegen", action: "<action>", mode: "schema" })  # exact params
```

## Action Reference

### generation (6)

| Action | Purpose |
|--------|---------|
| `generate_image` | Generate a deterministic local placeholder image from a prompt and import it as a Texture2D. Supports optional explicit `resolution` and `texture_role`; does not call remote providers or read API keys. |
| `generate_image_via_ima2` | Call the configured ima2/imag2-gen server, import the first generated image as a Texture2D, and attach redacted provenance. Defaults to `http://192.168.0.10:3333` with `provider="oauth"` and `model="gpt-5.5"`; Monolith sends no API key. Supports `size`/`resolution`, `background=transparent|opaque|auto`, `texture_role`, and reference image paths/base64. |
| `get_generated_asset_provenance` | Read redacted generation provenance (model, prompt hash, timestamp) from a Texture2D asset's metadata. |
| `get_image_generation_defaults` | Return default image generation settings, accepted aspect ratios, destination path, ima2 bridge settings, and provenance policy. |
| `import_generated_image` | Import externally generated image bytes as a Texture2D and attach redacted generation provenance. This is the safe remote-provider boundary. |
| `list_image_models` | List Monolith-native, ima2 bridge, and external import providers. |

## Notes

- This reference is generated from the live `RegisterAction` surface. If an action is missing or renamed, re-run `monolith_discover({ namespace: "imagegen" })` - the catalog is the source of truth.
- Pass `mode: "schema"` to `monolith_discover` for required/optional params and types before calling an action.
- Use `provider="oauth"` for the API-key-free path. `provider="api"` requires `OPENAI_API_KEY` on the ima2/imag2-gen server host, not in Monolith.
- Use `texture_role` to make the generated Texture2D game-resource-ready at import time. Supported roles: `ui_icon`, `sprite`, `decal`, `basecolor`, `world_tile`, `normal`, `orm_mask`, `height`, `emissive`. The import result returns `settings_applied` and non-blocking `validation` warnings.
- Use `background="transparent"` with `texture_role="ui_icon"` or `texture_role="sprite"` when the upstream model should produce real alpha. Monolith forwards it as an API option through ima2 and rejects transparent backgrounds for JPEG output; the Texture2D import path preserves alpha when the returned PNG/WebP contains it and applies alpha bleed for UI/sprite/decal roles.
- For world/material textures, prefer `texture_role="basecolor"` or `texture_role="world_tile"` for generated color maps, `texture_role="normal"` only when the source really is a valid tangent-space normal map, and `texture_role="orm_mask"` for packed AO/Roughness/Metallic-style data. Role validation warns about likely non-tileable edges, suspicious normal data, non-power-of-two dimensions, and wrong data-texture settings.
- If no asset destination is provided, generated Texture2D assets go under `/Game/GeneratedImages`. Reference image inputs are archived as PNG files under the project root `GeneratedImages` folder before being forwarded to ima2.
