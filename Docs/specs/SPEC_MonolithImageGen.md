# Monolith — MonolithImageGen Module

**Parent:** [../SPEC_CORE.md](../SPEC_CORE.md)
**Engine:** Unreal Engine 5.7+
**Owner module:** `MonolithImageGen`
**Namespace:** `imagegen`
**MCP tool:** `imagegen_query`
**Status:** Implemented (2026-05-21 namespace split + ima2 bridge + source PNG mirror)

---

## 1. Scope

`MonolithImageGen` owns the `imagegen` namespace: generated-image provider discovery, deterministic local placeholder Texture2D generation, ima2/imag2-gen HTTP generation, caller-supplied generated-image import, postprocessed generated PNG archival, and redacted provenance metadata.

## 2. Namespace Ownership

Action implementations live in `Source/MonolithImageGen` and export through `MONOLITHIMAGEGEN_API`. `FMonolithImageGenActions` owns image-generation provider/import/provenance registration. `MonolithImageGen::ShutdownModule` unregisters the `imagegen` namespace through owner-scoped action cleanup.

The module depends on `MonolithAsset` for the exported `MonolithAsset::FTextureIngestActions::HandleImportTextureFromBytes` helper, which remains the single Texture2D import path for generated image bytes. HTTP generation is isolated to the ima2/imag2-gen bridge; provider credentials remain outside Monolith. Reference image path/base64 normalization, root-level reference PNG archival, and postprocessed generated PNG mirroring are owned by `MonolithImageGen`.

## 3. Registered Actions

| Action | Purpose |
|--------|---------|
| `list_image_models` | List Monolith-native and external-boundary image generation providers. |
| `get_image_generation_defaults` | Return default provider/model, ima2 bridge settings, destination, source/reference PNG directories, aspect ratios, payload cap, texture settings, and prompt policy. |
| `generate_image` | Generate a deterministic local PNG placeholder, import it as a Texture2D, and save a postprocessed PNG mirror when enabled. |
| `generate_image_via_ima2` | POST a generation request to an external ima2/imag2-gen server, import the first returned image as a Texture2D, save a postprocessed PNG mirror when enabled, and attach redacted provenance metadata. |
| `import_generated_image` | Import external base64 image bytes or a local generated image file as a Texture2D, save a postprocessed PNG mirror when enabled, and attach redacted provenance metadata. |
| `get_generated_asset_provenance` | Read `Monolith.Generated.*` metadata from a generated Texture2D asset. |

## 4. Build.cs Dependencies

| Scope | Modules |
|-------|---------|
| Public | `Core`, `CoreUObject`, `Engine`, `MonolithCore` |
| Private | `MonolithAsset`, `UnrealEd`, `HTTP`, `ImageWrapper`, `Json`, `JsonUtilities` |

## 5. Settings

| Setting | Default | Effect |
|---------|---------|--------|
| `bEnableImageGen` | `true` | Enables `MonolithImageGen` startup registration for `imagegen` actions. Restart required after changing. |
| `ImageGenBridgeServerUrl` | `http://192.168.1.147:3333` | Base URL for the external ima2/imag2-gen server used by `generate_image_via_ima2`. |
| `ImageGenBridgeProvider` | `oauth` | Provider forwarded to ima2. `oauth` uses the server host's Codex OAuth session; `api` requires the server host to provide `OPENAI_API_KEY`. |
| `ImageGenBridgeDefaultModel` | `gpt-5.5` | Model forwarded to ima2 when `generate_image_via_ima2` omits `model`. |
| `ImageGenBridgeTimeoutSeconds` | `420.0` | Blocking HTTP timeout for a generation request. |

## 6. Provider Boundary

`generate_image` is local deterministic only: provider `local_deterministic`, model `monolith/local-gradient-png-v1`, output PNG. It does not call remote providers or read API keys. The legacy `monolith/local-gradient-bmp-v1` model name is accepted as an alias for compatibility but still produces PNG.

`generate_image_via_ima2` calls the configured ima2/imag2-gen `/api/generate` endpoint. By default it targets `http://192.168.1.147:3333` with `provider="oauth"`, `model="gpt-5.5"`, `quality="high"`, `size="1024x1024"`, `format="png"`, `background="auto"`, `compose_prompt=true`, and `moderation="low"`. Monolith accepts only PNG bridge output; JPEG, WebP, and other provider formats are rejected before the bridge call so generated assets enter the project through one lossless Texture2D path. Monolith does not read, store, or forward OpenAI API keys; OAuth/API-key ownership stays on the ima2/imag2-gen server host.

External provider results still enter Monolith through the same Texture2D import boundary. `generate_image`, `generate_image_via_ima2`, and `import_generated_image` import through `MonolithAsset::FTextureIngestActions::HandleImportTextureFromBytes`, save the role-postprocessed imported pixels as a PNG mirror by default when the imported package is saved, and tag assets with redacted `Monolith.Generated.*` metadata. `generate_image_via_ima2` and `import_generated_image` validate compressed payload size and accept PNG generated payloads only before import; `import_generated_image` accepts either `bytes_b64` or `file_path`/`path` for local generated PNG files. Prompt text is not persisted; provenance stores prompt hashes plus `prompt_redacted=true`. Generated imports default to `texture_role="basecolor"` unless the caller supplies another role.

## 7. Paths, Resolution, and References

| Concern | Contract |
|---------|----------|
| Default asset destination | If neither `destination` nor `asset_path` is supplied, generated Texture2D packages are imported under `/Game/GeneratedImages`. |
| Explicit destination | `destination` still overrides `asset_path` + `asset_name`; otherwise `asset_path` chooses the Unreal folder and `asset_name` chooses the sanitized texture asset name. |
| Resolution | `generate_image_via_ima2` accepts `size` (`"1024x1024"`) or `resolution`; `resolution` accepts a number, `"WIDTHxHEIGHT"` string, `[width,height]`, or `{width,height}` and overrides `size`. The local deterministic placeholder action accepts the same `resolution` shape and uses it instead of `aspect_ratio`. |
| Output format | `generate_image_via_ima2` accepts only `format="png"` for provider output. `import_generated_image` also accepts only PNG generated payloads. JPEG, WebP, and other formats are rejected before any bridge call or generated import. |
| Background | `generate_image_via_ima2` accepts caller `background` values `transparent`, `opaque`, and `auto`, but never forwards `transparent` to ima2/OpenAI. After validating role compatibility, Monolith sends provider `background="auto"` for omitted or transparent requests, stores both `requested_background` and `provider_background` in provenance, and requires PNG output. Data and tile roles (`world_tile`, `normal`, `orm_mask`, `height`) reject `background="transparent"`. |
| Texture role | `generate_image`, `generate_image_via_ima2`, and `import_generated_image` accept `texture_role` and forward it to `asset.import_texture_from_bytes`. Supported roles are `ui_icon`, `sprite`, `decal`, `basecolor`, `world_tile`, `normal`, `orm_mask`, `height`, and `emissive`; `generate_image_via_ima2` validates the role before calling the bridge. The action result returns `settings_applied` and `validation`. |
| Prompt composition | `generate_image_via_ima2` accepts `compose_prompt`. It defaults to `true`, which appends Unreal Texture2D and `texture_role` constraints to the provider prompt, including strict evenly spaced grid constraints for multi-frame `sprite` output. The provider `background` option remains `auto` for omitted or transparent requests, including alpha roles (`ui_icon`, `sprite`, `decal`), for ima2/gpt-5.5 compatibility; the composed prompt still asks for transparent output and the import path can extract edge-background alpha. Provenance records only prompt hashes (`prompt_hash` for the effective prompt and `caller_prompt_hash` for the caller prompt). Set `compose_prompt=false` to send the caller `prompt` verbatim. |
| Generated source archive | `generate_image`, `generate_image_via_ima2`, and `import_generated_image` accept `save_source_png`. It defaults to the `save` value, so normal saved generations also write a postprocessed PNG under `<ProjectDir>/GeneratedImages` using the imported asset path relative to `/Game/GeneratedImages` when applicable. Example: `/Game/GeneratedImages/basecolor/T_Stone` mirrors to `<ProjectDir>/GeneratedImages/basecolor/T_Stone.png`. For destinations outside `/Game/GeneratedImages`, the path is mirrored relative to `/Game`, e.g. `/Game/Tests/T_Image` -> `<ProjectDir>/GeneratedImages/Tests/T_Image.png`. Provenance stores `source_png_kind="postprocessed"` when the mirror came from imported role-processing output. |
| Reference input | `generate_image_via_ima2` accepts `references`, `reference_images`, `reference_image_paths`, `reference_png_paths`, and `reference_asset_paths`. Image items can be local file paths, data URLs, raw base64 strings, or objects with `path`, `file_path`, `bytes_b64`, `data_url`, and optional `format_hint`; `reference_png_paths` is the explicit local-PNG path alias. |
| Texture reference input | `reference_asset_paths` accepts Unreal Texture2D package paths or object paths, including objects with `asset_path`, `path`, or `package_path`. Monolith loads the Texture2D, reads valid Source art from the top mip, converts TSF_BGRA8/TSF_G8/TSF_G16 source data to PNG, archives that PNG, and forwards it with the prompt as an ima2 reference. |
| Reference archive | Every reference image is decoded or extracted, re-encoded as PNG, saved under `<ProjectDir>/GeneratedImages`, and forwarded to ima2 as PNG base64 before the `/api/generate` call. The action result returns `reference_png_dir` and `reference_png_files`; provenance stores `reference_count` and reference PNG hashes. |
| External import input | `import_generated_image` accepts PNG `bytes_b64` for inline generated bytes or `file_path`/`path` for a local generated PNG file. Local-file imports use provenance `source="external_file"`; inline imports use `source="external_bytes"`. |
