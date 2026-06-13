---
name: unreal-imagegen
description: Use for AI image and texture generation via Monolith MCP (imagegen) - synthesize a PNG/Texture2D from a prompt, call the ima2 provider bridge, import externally generated image bytes, generate and validate SVG source, and bake MSDF textures with provenance. For the sprite-sheet production contract (asset_spec.yaml, candidate selection, postprocess, export) consuming a generated image use unreal-sprite; to ingest the PNG as a Texture2D and do generic save/rename/metadata use unreal-asset; to wire a generated texture into a material graph use unreal-materials; for a 3D mesh/model job rather than a 2D image use unreal-modelgen. Triggers on image gen, imagegen, generate texture, AI texture, generate image, texture synthesis, prompt to texture, text to image, ima2, MSDF, SVG to texture, generate icon art, generation provenance, import generated image.
---

# unreal-imagegen

Synthesizes 2D images, textures, SVG source, and MSDF textures and imports them as Texture2D assets with redacted provenance. Drives the Monolith MCP `imagegen` namespace via `imagegen_query(action, params)`. **10 actions**; the table below is a snapshot of the live registry surface, so call `monolith_discover` for exact parameter schemas before invoking.

## Discovery

```
monolith_discover({ namespace: "imagegen" })                      # all actions in this namespace
monolith_discover({ namespace: "imagegen", action: "<action>", mode: "schema" })  # exact params
```

## When to use / Use a different skill for

- **This skill:** synthesizing the raw image/texture itself - prompt-to-PNG, the ima2 provider bridge, importing externally generated image bytes, SVG source generation/validation, and MSDF baking, all landing as a Texture2D with provenance.
- **unreal-sprite** — the request is the full sprite-sheet production contract (asset_spec.yaml, candidate selection, postprocess, export metadata) that *consumes* a generated image, not the raw generation call.
- **unreal-asset** — ingesting the already-generated PNG as a Texture2D and doing generic save/rename/move/metadata after generation produces the file.
- **unreal-materials** — wiring a generated texture into a material graph; this skill only synthesizes the texture image.
- **unreal-modelgen** — the generative job is a 3D mesh/model rather than a 2D image/texture.

## Action Reference

Param notation: `name*` required, `name?` optional, `name=val` default, `a/b/c` allowed, `[w]` mutates. The cells below show the high-signal params; the **full per-action signature with every optional param, default, and allowed value is in [references/actions.md](references/actions.md)**. Signatures are a snapshot of the live catalog — for the exact full schema call `monolith_discover` with `mode: "schema"`. The Discovery block above stays the authority.

### generation (10)

| Action | Key params | Purpose |
|--------|------------|---------|
| `list_image_models` | (none) | List Monolith-native image generation providers. |
| `get_image_generation_defaults` | (none) | Return default settings, accepted aspect ratios, destination path, and provenance policy. |
| `get_generated_asset_provenance` | `asset_path*` | Read redacted provenance (model, prompt hash, timestamp) from a Texture2D's metadata. |
| `validate_svg` | `svg_text?`/`bytes_b64?`/`file_path?` (one) `profile?=editor` (web/editor/msdf_source) | Validate SVG source without writing files; reports sanitizer removals, topology, and `msdf_ready` blockers. |
| `[w] generate_image` | `prompt*` `aspect_ratio?=1:1` `resolution?` `texture_role?=basecolor` `destination?` `save?=true` | Generate a deterministic local PNG placeholder and import it as a Texture2D; no remote providers or API keys. |
| `[w] generate_image_via_ima2` | `prompt*` `server_url?=http://192.168.1.147:3333` `provider?=oauth` (oauth/api/auto) `model?=gpt-5.5` `size?=1024x1024` `background?=auto` `compose_prompt?` `texture_role?=basecolor` `reference_*_paths?` | Call the ima2/imag2-gen server, import the first PNG as a Texture2D, attach redacted provenance; Monolith sends no API key. |
| `[w] import_generated_image` | `bytes_b64?`/`file_path?` (one) `format_hint?` (png) `texture_role?=basecolor` `destination?` `save?=true` | Import externally generated image bytes/file as a Texture2D with provenance. The safe remote-provider boundary. |
| `[w] generate_svg` | `svg_spec?`/`prompt?` `profile?=editor` (web/editor/msdf_source) `strict?` `save?=true` | Generate sanitized SVG source; writes `.svg` + `.monolith.json` sidecar only, no Texture2D import. |
| `[w] import_generated_svg` | `svg_text?`/`bytes_b64?`/`file_path?` (one) `profile?=editor` `strict?` `save?=true` | Import external SVG text/bytes/file through the sanitizer/provenance boundary. |
| `[w] generate_msdf_from_svg` | `svg_spec?`/`svg_text?`/`file_path?`/`prompt?` (one) `size?=128` `pixel_range?=8` `verify_samples?=true` `create_material?=true` `verify_material_render?=true` | Convert msdf_ready SVG into an MSDF Texture2D, sample channels, optionally create/render a masked unlit preview material. |

## Common Workflows

Numbered recipes use only the actions in the table above (and `references/actions.md`). Run `monolith_discover` with `mode: "schema"` for exact params before each call.

### Recipe 1 — Generate a texture, import it, validate provenance, hand off

1. `imagegen_query("get_image_generation_defaults", {})` — read the accepted aspect ratios, default destination, and provenance policy so the destination and `aspect_ratio` you pass next are valid; `imagegen_query("list_image_models", {})` confirms which native provider answers.
2. `imagegen_query("generate_image", { prompt, texture_role: "basecolor", aspect_ratio: "1:1", destination: "/Game/Generated/Tex_Stone", save: true })` `[w]` — synthesize the PNG and import it as a Texture2D in one call. (Swap to `generate_image_via_ima2` with the same `prompt`/`texture_role` to drive the ima2 server, or to `import_generated_image` with `bytes_b64`/`file_path` when the bytes came from an external provider — all three land a Texture2D through the same import boundary.)
3. `imagegen_query("get_generated_asset_provenance", { asset_path: "/Game/Generated/Tex_Stone" })` — read back the redacted provenance (model, prompt hash, timestamp) to confirm the import attached it and the asset is the one you just generated.
4. Hand off the imported Texture2D: use **unreal-asset** for generic save/rename/move/metadata, or **unreal-materials** to wire it into a material graph. This namespace only synthesizes and imports the image — it does not author materials or finalize the asset name.

Pitfall — `destination` vs `asset_path`+`asset_name`: `destination` overrides `asset_path`+`asset_name`; with no destination, color textures land under `/Game/GeneratedImages`. `overwrite_policy` defaults to `unique`, so a re-run writes a new uniquely named asset rather than overwriting — pass `overwrite_policy: "fail"` when you want a collision to error instead. Pick `texture_role` at generation time so the import applies the right sRGB/mip/LOD settings; `import_generated_image` and `generate_image_via_ima2` accept only PNG payloads.

### Recipe 2 — SVG → MSDF variant

1. `imagegen_query("generate_svg", { svg_spec, profile: "msdf_source", strict: true, save: true })` `[w]` — author sanitized SVG source for MSDF; this writes `.svg` + `.monolith.json` only, no Texture2D. (Use `import_generated_svg` with `svg_text`/`bytes_b64`/`file_path` instead when the SVG came from outside.)
2. `imagegen_query("validate_svg", { file_path: "<written .svg>", profile: "msdf_source" })` — confirm `msdf_ready=true` and resolve any reported blockers (text, gradients, strokes, unflattened transforms, `evenodd` ambiguity) before baking.
3. `imagegen_query("generate_msdf_from_svg", { file_path: "<msdf_ready .svg>", size: 128, pixel_range: 8, verify_samples: true, create_material: true, verify_material_render: true })` `[w]` — bake the MSDF Texture2D, sample its channels, and render a masked unlit preview material to prove the graph is non-empty.
4. `imagegen_query("get_generated_asset_provenance", { asset_path: "<baked MSDF asset>" })` — confirm provenance, then hand the MSDF texture/material off to **unreal-materials** or **unreal-ui** for UI use.

Pitfall — MSDF readiness gate: `generate_msdf_from_svg` expects msdf_ready source, so always pass `validate_svg` first; keep `verify_samples=true` outside diagnostic-only runs so a degenerate bake fails instead of producing a useless MSDF. SVG actions write under `<ProjectDir>/GeneratedImages/Vector` and never rasterize or parse SVG at gameplay runtime.

## Notes

- This reference is generated from the live `RegisterAction` surface. If an action is missing or renamed, re-run `monolith_discover({ namespace: "imagegen" })` - the catalog is the source of truth.
- Pass `mode: "schema"` to `monolith_discover` for required/optional params and types before calling an action.
- Use `provider="oauth"` for the API-key-free path. `provider="api"` requires `OPENAI_API_KEY` on the ima2/imag2-gen server host, not in Monolith.
- Use PNG for generated image payloads. `generate_image_via_ima2` accepts only `format="png"`, and `import_generated_image` accepts only generated PNG bytes/files. MonolithImageGen rejects JPEG, WebP, and other generated payload formats before the bridge call or import so generated textures enter Unreal through one lossless path.
- Use `texture_role` to make the generated Texture2D game-resource-ready at import time. Supported roles: `ui_icon`, `sprite`, `decal`, `basecolor`, `world_tile`, `normal`, `orm_mask`, `height`, `emissive`. `generate_image_via_ima2` validates the role before the bridge call. The import result returns `settings_applied` and non-blocking `validation` warnings.
- `compose_prompt` defaults to `true`, appending Unreal Texture2D and `texture_role` constraints to the provider prompt; provenance stores prompt hashes only. Set `compose_prompt=false` only when the caller prompt must be sent verbatim. Monolith never forwards `background="transparent"` to ima2/gpt-5.5; omitted or transparent background requests are sent as provider `background="auto"` while the composed prompt asks for transparent output and the Texture2D import path can extract edge-background alpha.
- Do not depend on provider `background="transparent"` for `ui_icon`, `sprite`, or `decal`; use role-aware prompt composition and PNG import postprocessing instead. Do not request transparent backgrounds for `world_tile`, `normal`, `orm_mask`, or `height`.
- For world/material textures, prefer `texture_role="basecolor"` or `texture_role="world_tile"` for generated color maps, `texture_role="normal"` only when the source really is a valid tangent-space normal map, and `texture_role="orm_mask"` for packed AO/Roughness/Metallic-style data. Role validation warns about likely non-tileable edges, suspicious normal data, non-power-of-two dimensions, and wrong data-texture settings.
- If no asset destination is provided, generated Texture2D assets go under `/Game/GeneratedImages`. Reference image inputs are archived as PNG files under the project root `GeneratedImages` folder before being forwarded to ima2.
- Use `generate_svg` and `import_generated_svg` for source/vector workflows. They write sanitized `.svg` files and sidecars under `<ProjectDir>/GeneratedImages/Vector`; they do not import SVG as `UTexture2D`, rasterize previews, or parse SVG at gameplay runtime.
- For SVG intended for MSDF, use `profile="msdf_source"` and call `validate_svg` before depending on the source. `msdf_ready=true` requires closed, non-self-intersecting, non-overlapping filled contours with explicit geometry; text, gradients, strokes, unflattened transforms, `evenodd` ambiguity, and unsupported path grammar become blockers.
- Use `generate_msdf_from_svg` as the explicit SVG-to-runtime-resource conversion boundary. It imports the baked PNG through the generated Texture2D path and applies `TC_Masks`, `sRGB=false`, `TMGS_NoMipmaps`, UI LOD group, clamp addressing, `NeverStream=true`, and max texture size matching the MSDF output.
- Keep `verify_samples=true` for generated MSDF unless you are doing a diagnostic-only run. The action samples representative inside, outside, and edge pixels and fails if median/channel-spread checks indicate the output is not a useful MSDF.
- Use `create_material=true` plus `verify_material_render=true` when validating a new MSDF asset visually; this creates an unlit masked material and decodes a rendered preview PNG to prove the graph is non-empty and non-uniform.
- Keep generated SVG simple: prefer filled paths, `rect`, and `polygon` primitives with stable `viewBox`; avoid filters, masks, CSS, animation, images, external references, `use`/symbols, and decorative stroke semantics unless a later conversion step can flatten and revalidate them.

## Unreal Sprite Sheet Workflow Notes

Use these rules when generating Paper2D/PaperZD character sheets or other game-ready sprite sheets through MonolithImageGen:

- Route generation through Monolith `imagegen.generate_image_via_ima2` or `imagegen.import_generated_image`; do not call the ima2 CLI directly when the request asks for MonolithImageGen or Monolith MCP.
- Use `reference_asset_paths` for Unreal assets and `reference_png_paths` for local reference PNGs. Asset references should be extracted or archived to the project root `GeneratedImages` folder before forwarding to ima2.
- For sprite animation quality, prefer increasing frames per direction before increasing cell size. A fixed 256x256 cell is usually enough for small in-game Paper2D characters; 6 frames per direction at about 10 FPS is a practical improvement over 4 frames. Move to 8+ frames only if the provider accepts the wider sheet and the runtime asset builder can slice that column count.
- Keep the final sprite-sheet Texture2D dimensions power-of-two when practical. With 256x256 cells and four directional rows, prefer `4x4=1024x1024` or `8x4=2048x1024` over non-power-of-two sheet widths such as `6x4=1536x1024`, unless the consuming runtime or source art explicitly requires that frame count.
- Keep the sheet a strict evenly spaced grid: identical cell dimensions, fixed row order, transparent gutters, aligned baselines, no overlap, no crop, no text, no watermark, and consistent character scale per cell. With `texture_role=sprite` and `compose_prompt=true`, Monolith appends this grid constraint when multiple poses or frames are requested. For four-direction locomotion, use rows ordered Down, Left, Right, Up unless the consuming commandlet says otherwise.
- Validate the generated PNG visually and structurally before applying it. Check alpha exists, dimensions match the intended grid, no character pixels cross cell boundaries, and no disconnected artifact islands remain near cell edges.
- When removing a background from a generated sprite sheet, only remove background pixels connected to the image or cell border by flood-fill or connected-component logic. Avoid global color deletion because it can erase interior highlights, hair, UI glows, or holes that should remain opaque.
- Save the postprocessed PNG, not the raw provider output, as the mirrored source under the project `GeneratedImages/...` path. Import the same postprocessed PNG into the Unreal Texture2D so the source mirror and asset content match.
- For sprite Texture2D imports, apply sprite-safe settings and verify them: `sRGB=true`, no mipmaps (`TMGS_NoMipmaps`), UI/sprite LOD group as appropriate, nearest filtering for pixel art, and clamp addressing when the sheet should not wrap.
- After importing, verify through Monolith/editor tools: Texture2D size/settings, every PaperSprite source texture, source UV, source dimension, flipbook frame count/FPS, AnimBP assignment, and consuming Blueprint compile status.
- Close or quit any running editor/headless editor before commandlets that rewrite sprite, flipbook, animation, or Blueprint assets. Save failures like `MoveFile ... Error Code 32` usually mean an UnrealEditor process is holding the target `.uasset` files.
- Clean up unused intermediate candidates after the final sheet is applied. Keep only the final applied Texture2D, postprocessed PNG mirror, reference archives that are useful for provenance, and generated runtime assets.
