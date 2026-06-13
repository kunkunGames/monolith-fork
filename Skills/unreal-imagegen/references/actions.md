# unreal-imagegen — Action Parameter Reference

Param notation: `name*` required, `name?` optional, `name=val` default, `a/b/c` allowed, `[w]` mutates. Signatures are a snapshot of the live catalog — for the exact full schema call `monolith_discover({ namespace: "imagegen", action: "<action>", mode: "schema" })`. The Discovery block in `../SKILL.md` stays the authority.

All actions live in the `imagegen` namespace, called via `imagegen_query(action, params)`.

## Read-only

### `list_image_models`
No params. Lists Monolith-native image generation providers; remote providers are not configured here — import external bytes with `import_generated_image`.

### `get_image_generation_defaults`
No params. Returns default image generation settings, accepted aspect ratios, destination path, and provenance policy.

### `get_generated_asset_provenance`
`asset_path*` — Texture package path or object path. Reads redacted provenance (model, prompt hash, timestamp) from a Texture2D's metadata.

### `validate_svg`
`svg_text?` `bytes_b64?` `file_path?` `path?` (alias of file_path) `profile?=editor` (web/editor/msdf_source) `strict?` `return_sanitized_svg?=false` `geometry_policy?=validate` (sanitize_only/validate/normalize) `fill_rule_policy?` (preserve/nonzero/reject_evenodd)

Validate and summarize SVG source without writing files; reports sanitizer removals, geometry topology, and `msdf_ready` blockers. Supply exactly one of `svg_text` / `bytes_b64` / `file_path`(=`path`).

## Mutating (`[w]`)

### `[w] generate_image`
`prompt*` `provider?=local_deterministic` `model?=monolith/local-gradient-png-v1` `aspect_ratio?=1:1` (1:1/16:9/9:16/4:3/3:4/21:9) `resolution?` ([w,h] / "1024x1024" / 1024 / {width,height}, overrides aspect_ratio) `asset_path?=/Game/GeneratedImages` `asset_name?` `destination?` (overrides asset_path+asset_name) `overwrite_policy?=unique` (unique/fail) `texture_role?=basecolor` (ui_icon/sprite/decal/basecolor/world_tile/normal/orm_mask/height/emissive) `settings?` `save?=true` `save_source_png?`

Generate a deterministic local PNG placeholder from a prompt and import it as a Texture2D. Does not call remote providers or read API keys.

### `[w] generate_image_via_ima2`
`prompt*` `server_url?=http://192.168.1.147:3333` `provider?=oauth` (oauth/api/auto) `model?=gpt-5.5` `reasoning_effort?` `quality?=high` (low/medium/high) `size?=1024x1024` `resolution?` ([w,h] / "1024x1024" / 1024 / {width,height}, overrides size) `format?=png` (png only) `background?=auto` (transparent/opaque/auto; transparent forwarded as auto) `moderation?=low` (auto/low) `mode?=auto` `compose_prompt?` (default true) `web_search_enabled?` `references?` `reference_images?` `reference_image_paths?` `reference_png_paths?` `reference_asset_paths?` `request_id?` `session_id?` `client_node_id?` `timeout_seconds?=420.0` `aspect_ratio?` `asset_path?=/Game/GeneratedImages` `asset_name?` `destination?` (overrides asset_path+asset_name) `overwrite_policy?=unique` (unique/fail) `max_bytes?=26214400` `texture_role?=basecolor` (ui_icon/sprite/decal/basecolor/world_tile/normal/orm_mask/height/emissive) `settings?` `save?=true` `save_source_png?`

Call an external ima2/imag2-gen HTTP server, import the first generated image as a Texture2D, and attach redacted provenance. Monolith does not read provider API keys.

### `[w] import_generated_image`
`bytes_b64?` (required unless file_path) `file_path?` (required unless bytes_b64) `path?` (alias of file_path) `format_hint?` (png only) `prompt?` `provider?=external` `model?=unknown` `aspect_ratio?` `asset_path?=/Game/GeneratedImages` `asset_name?` `destination?` (overrides asset_path+asset_name) `overwrite_policy?=unique` (unique/fail) `max_bytes?=26214400` `texture_role?=basecolor` (ui_icon/sprite/decal/basecolor/world_tile/normal/orm_mask/height/emissive) `settings?` `save?=true` `save_source_png?`

Import externally generated image bytes or a local generated image file as a Texture2D with redacted provenance. The safe remote-provider boundary.

### `[w] generate_svg`
`svg_spec?` (required unless prompt placeholder mode) `prompt?` `profile?=editor` (web/editor/msdf_source) `asset_path?=/Game/GeneratedImages/Vector` `asset_name?` `destination?` (overrides asset_path+asset_name) `overwrite_policy?=unique` (unique/fail) `view_box?` `width?` `height?` `return_svg?` (default false when saved, true when save=false) `save?=true` `strict?` (default true for msdf_source writes) `geometry_policy?=validate` (sanitize_only/validate/normalize) `fill_rule_policy?` (preserve/nonzero/reject_evenodd) `margin?`

Generate a deterministic sanitized SVG source from `svg_spec` or prompt placeholder metadata. Writes `.svg` + `.monolith.json` sidecar only; no runtime SVG rendering or Texture2D import.

### `[w] import_generated_svg`
`svg_text?` (required unless bytes_b64 or file_path/path) `bytes_b64?` `file_path?` `path?` (alias of file_path) `format_hint?` (svg/svg+xml) `prompt?` `provider?=external` `model?=unknown` `profile?=editor` (web/editor/msdf_source) `asset_path?=/Game/GeneratedImages/Vector` `asset_name?` `destination?` (overrides asset_path+asset_name) `overwrite_policy?=unique` (unique/fail) `return_svg?` (default false when saved, true when save=false) `save?=true` `strict?` (default true for msdf_source writes) `geometry_policy?=validate` (sanitize_only/validate/normalize) `fill_rule_policy?` (preserve/nonzero/reject_evenodd) `margin?`

Import externally generated SVG text, base64 bytes, or a local `.svg` file through the sanitizer/provenance boundary. Writes source `.svg` + sidecar only.

### `[w] generate_msdf_from_svg`
`svg_spec?` `svg_text?` `bytes_b64?` `file_path?` `path?` (alias of file_path) `prompt?` (supply one source) `size?=128` `resolution?` (alias of size) `pixel_range?=8` `asset_path?=/Game/GeneratedImages/MSDF` `asset_name?` `destination?` (overrides asset_path+asset_name) `overwrite_policy?=unique` (unique/fail) `save?=true` `save_source_png?` `return_png?=false` `verify_samples?=true` `create_material?=true` `verify_material_render?=true` `material_destination?` `material_asset_path?` `material_asset_name?` `material_overwrite_policy?=unique` (unique/fail)

Generate a deterministic MSDF PNG from msdf_ready SVG source, import it as a data Texture2D, sample its channels, and optionally create/render a masked unlit preview material.
