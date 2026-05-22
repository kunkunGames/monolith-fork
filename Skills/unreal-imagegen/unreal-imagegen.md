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

## Unreal Sprite Sheet Workflow Notes

Use these rules when generating Paper2D/PaperZD character sheets or other game-ready sprite sheets through MonolithImageGen:

- Route generation through Monolith `imagegen.generate_image_via_ima2` or `imagegen.import_generated_image`; do not call the ima2 CLI directly when the request asks for MonolithImageGen or Monolith MCP.
- Use `reference_asset_paths` for Unreal assets and `reference_png_paths` for local reference PNGs. Asset references should be extracted or archived to the project root `GeneratedImages` folder before forwarding to ima2.
- For sprite animation quality, prefer increasing frames per direction before increasing cell size. A fixed 256x256 cell is usually enough for small in-game Paper2D characters; 6 frames per direction at about 10 FPS is a practical improvement over 4 frames. Move to 8+ frames only if the provider accepts the wider sheet and the runtime asset builder can slice that column count.
- Keep the sheet a strict grid: fixed cell dimensions, fixed row order, transparent gutters, no crop, no text, no watermark, and consistent character scale per cell. For four-direction locomotion, use rows ordered Down, Left, Right, Up unless the consuming commandlet says otherwise.
- Validate the generated PNG visually and structurally before applying it. Check alpha exists, dimensions match the intended grid, no character pixels cross cell boundaries, and no disconnected artifact islands remain near cell edges.
- When removing a background from a generated sprite sheet, only remove background pixels connected to the image or cell border by flood-fill or connected-component logic. Avoid global color deletion because it can erase interior highlights, hair, UI glows, or holes that should remain opaque.
- Save the postprocessed PNG, not the raw provider output, as the mirrored source under the project `GeneratedImages/...` path. Import the same postprocessed PNG into the Unreal Texture2D so the source mirror and asset content match.
- For sprite Texture2D imports, apply sprite-safe settings and verify them: `sRGB=true`, no mipmaps (`TMGS_NoMipmaps`), UI/sprite LOD group as appropriate, nearest filtering for pixel art, and clamp addressing when the sheet should not wrap.
- After importing, verify through Monolith/editor tools: Texture2D size/settings, every PaperSprite source texture, source UV, source dimension, flipbook frame count/FPS, AnimBP assignment, and consuming Blueprint compile status.
- Close or quit any running editor/headless editor before commandlets that rewrite sprite, flipbook, animation, or Blueprint assets. Save failures like `MoveFile ... Error Code 32` usually mean an UnrealEditor process is holding the target `.uasset` files.
- Clean up unused intermediate candidates after the final sheet is applied. Keep only the final applied Texture2D, postprocessed PNG mirror, reference archives that are useful for provenance, and generated runtime assets.
