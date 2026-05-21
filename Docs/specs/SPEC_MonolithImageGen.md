# Monolith — MonolithImageGen Module

**Parent:** [../SPEC_CORE.md](../SPEC_CORE.md)
**Engine:** Unreal Engine 5.7+
**Owner module:** `MonolithImageGen`
**Namespace:** `imagegen`
**MCP tool:** `imagegen_query`
**Status:** Implemented (2026-05-21 namespace split + ima2 bridge)

---

## 1. Scope

`MonolithImageGen` owns the `imagegen` namespace: generated-image provider discovery, deterministic local placeholder Texture2D generation, ima2/imag2-gen HTTP generation, caller-supplied generated-image import, and redacted provenance metadata.

## 2. Namespace Ownership

Action implementations live in `Source/MonolithImageGen` and export through `MONOLITHIMAGEGEN_API`. `FMonolithImageGenActions` owns image-generation provider/import/provenance registration. `MonolithImageGen::ShutdownModule` unregisters the `imagegen` namespace through owner-scoped action cleanup.

The module depends on `MonolithAsset` for the exported `MonolithAsset::FTextureIngestActions::HandleImportTextureFromBytes` helper, which remains the single Texture2D import path for generated image bytes. HTTP generation is isolated to the ima2/imag2-gen bridge; provider credentials remain outside Monolith. Reference image path/base64 normalization and root-level PNG archival are owned by `MonolithImageGen`.

## 3. Registered Actions

| Action | Purpose |
|--------|---------|
| `list_image_models` | List Monolith-native and external-boundary image generation providers. |
| `get_image_generation_defaults` | Return default provider/model, ima2 bridge settings, destination, aspect ratios, payload cap, texture settings, and prompt policy. |
| `generate_image` | Generate a deterministic local BMP placeholder from a prompt and import it as a Texture2D. |
| `generate_image_via_ima2` | POST a generation request to an external ima2/imag2-gen server, import the first returned image as a Texture2D, and attach redacted provenance metadata. |
| `import_generated_image` | Import external base64 image bytes as a Texture2D and attach redacted provenance metadata. |
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
| `ImageGenBridgeServerUrl` | `http://192.168.0.10:3333` | Base URL for the external ima2/imag2-gen server used by `generate_image_via_ima2`. |
| `ImageGenBridgeProvider` | `oauth` | Provider forwarded to ima2. `oauth` uses the server host's Codex OAuth session; `api` requires the server host to provide `OPENAI_API_KEY`. |
| `ImageGenBridgeDefaultModel` | `gpt-5.5` | Model forwarded to ima2 when `generate_image_via_ima2` omits `model`. |
| `ImageGenBridgeTimeoutSeconds` | `420.0` | Blocking HTTP timeout for a generation request. |

## 6. Provider Boundary

`generate_image` is local deterministic only: provider `local_deterministic`, model `monolith/local-gradient-bmp-v1`, output BMP. It does not call remote providers or read API keys.

`generate_image_via_ima2` calls the configured ima2/imag2-gen `/api/generate` endpoint. By default it targets `http://192.168.0.10:3333` with `provider="oauth"`, `model="gpt-5.5"`, `quality="high"`, `size="1024x1024"`, `format="png"`, `background="auto"`, and `moderation="low"`. Monolith does not read, store, or forward OpenAI API keys; OAuth/API-key ownership stays on the ima2/imag2-gen server host.

External provider results still enter Monolith through the same Texture2D import boundary. `generate_image_via_ima2` and `import_generated_image` both validate compressed base64 payload size, import through `MonolithAsset::FTextureIngestActions::HandleImportTextureFromBytes`, and tag assets with redacted `Monolith.Generated.*` metadata. Prompt text is not persisted; provenance stores `prompt_hash` plus `prompt_redacted=true`. Generated imports default to `texture_role="basecolor"` unless the caller supplies another role.

## 7. Paths, Resolution, and References

| Concern | Contract |
|---------|----------|
| Default asset destination | If neither `destination` nor `asset_path` is supplied, generated Texture2D packages are imported under `/Game/GeneratedImages`. |
| Explicit destination | `destination` still overrides `asset_path` + `asset_name`; otherwise `asset_path` chooses the Unreal folder and `asset_name` chooses the sanitized texture asset name. |
| Resolution | `generate_image_via_ima2` accepts `size` (`"1024x1024"`) or `resolution`; `resolution` accepts a number, `"WIDTHxHEIGHT"` string, `[width,height]`, or `{width,height}` and overrides `size`. The local deterministic placeholder action accepts the same `resolution` shape and uses it instead of `aspect_ratio`. |
| Background | `generate_image_via_ima2` accepts `background` values `transparent`, `opaque`, and `auto`, forwards the value to ima2/OpenAI, stores it in provenance, and rejects `background="transparent"` unless `format` is `png` or `webp`. |
| Texture role | `generate_image`, `generate_image_via_ima2`, and `import_generated_image` accept `texture_role` and forward it to `asset.import_texture_from_bytes`. Supported roles are `ui_icon`, `sprite`, `decal`, `basecolor`, `world_tile`, `normal`, `orm_mask`, `height`, and `emissive`; the action result returns `settings_applied` and `validation`. |
| Reference input | `generate_image_via_ima2` accepts `references`, `reference_images`, and `reference_image_paths`. Items can be local file paths, data URLs, raw base64 strings, or objects with `path`, `file_path`, `bytes_b64`, `data_url`, and optional `format_hint`. |
| Reference archive | Every reference image is decoded, re-encoded as PNG, saved under `<ProjectDir>/GeneratedImages`, and forwarded to ima2 as PNG base64. The action result returns `reference_png_dir` and `reference_png_files`; provenance stores `reference_count` and reference PNG hashes. |
