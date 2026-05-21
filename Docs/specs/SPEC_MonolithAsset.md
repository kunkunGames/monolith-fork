# Monolith - MonolithAsset Module

**Parent:** [../SPEC_CORE.md](../SPEC_CORE.md)
**Engine:** Unreal Engine 5.7+
**Owner module:** `MonolithAsset`
**Namespace:** `asset`
**MCP tool:** `asset_query`
**Status:** Implemented (2026-05-21 asset namespace split)

---

## 1. Scope

`MonolithAsset` owns generic asset workflows that are not specific to one content domain: Texture2D/font ingest, file texture import, asset save/delete lifecycle operations, naming/rename hygiene, and specialized read-only asset inspection.

This module keeps asset-level actions out of `MonolithUI`, `MonolithMesh`, and `MonolithMaterial` so those modules can focus on their native domains.

## 2. Namespace Ownership

`FMonolithAssetModule` registers all `asset` actions with owner `MonolithAsset` and unregisters them through owner-scoped cleanup during shutdown. The module is gated by `UMonolithSettings::bEnableAsset`.

`MonolithImageGen` depends on `MonolithAsset` for `MonolithAsset::FTextureIngestActions::HandleImportTextureFromBytes`, so generated-image imports and direct texture-byte imports share one Texture2D creation path. `MonolithBlueprint` depends on `MonolithAsset` for the shared save-asset helper used by its internal batch edit op.

No compatibility aliases are registered for the move from the old `ui` ingest actions or the generic lifecycle actions formerly registered under `editor`/`blueprint`. Callers should use `asset.import_texture_from_bytes`, `asset.import_font_family`, `asset.import_texture_from_file`, `asset.save_asset`, and `asset.delete_assets`.

## 3. Registered Actions

| Action | Owner class | Purpose |
|--------|-------------|---------|
| `import_texture_from_bytes` | `MonolithAsset::FTextureIngestActions` | Decode base64 compressed image bytes and create a `UTexture2D` under `/Game/...`. |
| `import_font_family` | `MonolithAsset::FFontIngestActions` | Import one or more TTF files as a composite `UFont` plus `UFontFace` assets. |
| `import_texture_from_file` | `FMonolithAssetLifecycleActions` | Import an external image file as a `UTexture2D` with optional compression, sRGB, tiling, max-size, and LOD-group settings. |
| `save_asset` | `FMonolithAssetLifecycleActions` | Save any loaded asset package to disk. |
| `delete_assets` | `FMonolithAssetLifecycleActions` | Delete assets by path with optional prefix guard and dry-run validation. |
| `validate_naming_conventions` | `FMonolithAssetHygieneActions` | Scan assets under a content path and report prefix-rule violations. |
| `list_supported_asset_enrichers` | `FMonolithAssetInspectionActions` | List specialized read-only enrichers supported by `inspect_asset`. |
| `inspect_asset` | `FMonolithAssetInspectionActions` | Inspect one asset with specialized enrichment and optional reflected references. |
| `inspect_assets_batch` | `FMonolithAssetInspectionActions` | Inspect multiple assets with per-row success/error results. |
| `validate_specialized_asset` | `FMonolithAssetInspectionActions` | Validate a specialized asset and report warnings without mutation. |
| `batch_rename_assets` | `FMonolithAssetHygieneActions` | Preview or apply batch asset renames through `IAssetTools::RenameAssets`. |

## 4. Build.cs Dependencies

| Scope | Modules |
|-------|---------|
| Public | `Core`, `CoreUObject`, `Engine`, `MonolithCore` |
| Private | `UnrealEd`, `Json`, `JsonUtilities`, `AssetRegistry`, `AssetTools`, `EditorScriptingUtilities`, `ImageWrapper`, `ImageCore`, `RenderCore`, `RHI`, `SlateCore` |

## 5. Settings

| Setting | Default | Effect |
|---------|---------|--------|
| `bEnableAsset` | `true` | Enables `MonolithAsset` startup registration for `asset` actions. Restart required after changing. |

## 6. Action Groups

| Group | Files | Notes |
|-------|-------|-------|
| Texture ingest | `Public/MonolithAssetTextureIngestActions.h`, `Private/MonolithAssetTextureIngestActions.cpp` | Public helper reused by `MonolithImageGen`; supports PNG, JPEG, BMP, EXR, TGA, HDR, TIFF, and DDS through `IImageWrapper`. |
| Font ingest | `Public/MonolithAssetFontIngestActions.h`, `Private/MonolithAssetFontIngestActions.cpp` | Uses UE 5.7-safe `UFont::GetMutableInternalCompositeFont()` for composite-font writes. |
| Lifecycle | `Public/MonolithAssetLifecycleActions.h`, `Private/MonolithAssetLifecycleActions.cpp` | Owns generic file texture import, asset save, and guarded delete operations previously scattered under editor/blueprint. |
| Hygiene | `Public/MonolithAssetHygieneActions.h`, `Private/MonolithAssetHygieneActions.cpp` | Owns naming convention validation and batch rename fixup. |
| Inspection | `Public/MonolithAssetInspectionActions.h`, `Private/MonolithAssetInspectionActions.cpp` | Former specialized asset inspection surface, now independent from `MonolithMaterial`. |

## 7. Verification

| Gate | Requirement |
|------|-------------|
| Source stale scan | No old UI ingest action names, old UI ingest classes, or old specialized-asset inspection class names remain in source. |
| Build | Run the primary `GoGameEditor` UBT command after closing any editor process that locks Monolith DLLs. |
| Runtime discovery | `monolith_discover({ "namespace": "asset" })` should list 11 actions owned by `MonolithAsset`. |
