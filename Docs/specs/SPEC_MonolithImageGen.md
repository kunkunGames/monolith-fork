# Monolith — MonolithImageGen Module

**Parent:** [../SPEC_CORE.md](../SPEC_CORE.md)
**Engine:** Unreal Engine 5.7+
**Owner module:** `MonolithImageGen`
**Namespace:** `imagegen`
**MCP tool:** `imagegen_query`
**Status:** Implemented (2026-05-21 namespace split)

---

## 1. Scope

`MonolithImageGen` owns the `imagegen` namespace: generated-image provider discovery, deterministic local placeholder Texture2D generation, caller-supplied generated-image import, and redacted provenance metadata.

## 2. Namespace Ownership

Action implementations live in `Source/MonolithImageGen` and export through `MONOLITHIMAGEGEN_API`. `FMonolithImageGenActions` owns image-generation provider/import/provenance registration. `MonolithImageGen::ShutdownModule` unregisters the `imagegen` namespace through owner-scoped action cleanup.

The module depends on `MonolithAsset` for the exported `MonolithAsset::FTextureIngestActions::HandleImportTextureFromBytes` helper, which remains the single Texture2D import path for generated image bytes.

## 3. Registered Actions

| Action | Purpose |
|--------|---------|
| `list_image_models` | List Monolith-native and external-boundary image generation providers. |
| `get_image_generation_defaults` | Return default provider/model, destination, aspect ratios, payload cap, texture settings, and prompt policy. |
| `generate_image` | Generate a deterministic local BMP placeholder from a prompt and import it as a Texture2D. |
| `import_generated_image` | Import external base64 image bytes as a Texture2D and attach redacted provenance metadata. |
| `get_generated_asset_provenance` | Read `Monolith.Generated.*` metadata from a generated Texture2D asset. |

## 4. Build.cs Dependencies

| Scope | Modules |
|-------|---------|
| Public | `Core`, `CoreUObject`, `Engine`, `MonolithCore` |
| Private | `MonolithAsset`, `UnrealEd`, `Json`, `JsonUtilities` |

## 5. Settings

| Setting | Default | Effect |
|---------|---------|--------|
| `bEnableImageGen` | `true` | Enables `MonolithImageGen` startup registration for `imagegen` actions. Restart required after changing. |

## 6. Provider Boundary

`generate_image` is local deterministic only: provider `local_deterministic`, model `monolith/local-gradient-bmp-v1`, output BMP. It does not call remote providers or read API keys.

Remote image generation is caller-owned. External provider results enter Monolith through `import_generated_image`, where base64 image bytes are validated, imported, and tagged with provenance. Prompt text is not persisted; provenance stores `prompt_hash` plus `prompt_redacted=true`.
