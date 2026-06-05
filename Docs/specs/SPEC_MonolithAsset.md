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
| `import_texture_from_bytes` | `MonolithAsset::FTextureIngestActions` | Decode base64 compressed image bytes and create a `UTexture2D` under `/Game/...`; optional `texture_role` applies Unreal texture-role import presets, role post-processing, validation metadata, and optional postprocessed PNG return. |
| `import_font_family` | `MonolithAsset::FFontIngestActions` | Import one or more TTF files as a composite `UFont` plus `UFontFace` assets. |
| `import_texture_from_file` | `FMonolithAssetLifecycleActions` | Import an external image file as a `UTexture2D` with optional compression, sRGB, tiling, max-size, and LOD-group settings. |
| `save_asset` | `FMonolithAssetLifecycleActions` | Save any loaded asset package to disk. |
| `delete_assets` | `FMonolithAssetLifecycleActions` | Delete assets by path with optional prefix guard, dry-run validation, non-interactive editor closing, and optional force deletion. |
| `validate_naming_conventions` | `FMonolithAssetHygieneActions` | Scan assets under a content path and report prefix-rule violations. |
| `list_supported_asset_enrichers` | `FMonolithAssetInspectionActions` | List specialized read-only enrichers supported by `inspect_asset`. |
| `inspect_asset` | `FMonolithAssetInspectionActions` | Inspect one asset with specialized enrichment and optional reflected references. |
| `inspect_assets_batch` | `FMonolithAssetInspectionActions` | Inspect multiple assets with per-row success/error results. |
| `validate_specialized_asset` | `FMonolithAssetInspectionActions` | Validate a specialized asset and report warnings without mutation. |
| `batch_rename_assets` | `FMonolithAssetHygieneActions` | Preview or apply batch asset renames through `IAssetTools::RenameAssets`. |
| `find_assets` | `FMonolithAssetFindActions` | Fuzzy, scored, typo-tolerant search over the live `AssetRegistry`; ranks by asset name/path/class (and optional tags) via the shared MonolithCore `FMonolithFuzzyMatch` engine, with `allow_transposition` controlling Damerau adjacent-swap tolerance. |

## 4. Build.cs Dependencies

| Scope | Modules |
|-------|---------|
| Public | `Core`, `CoreUObject`, `Engine`, `MonolithCore` |
| Private | `UnrealEd`, `Json`, `JsonUtilities`, `AssetRegistry`, `AssetTools`, `EditorScriptingUtilities`, `ImageWrapper`, `ImageCore`, `RenderCore`, `RHI`, `SlateCore` |

## 5. Lifecycle Delete Contract

`asset.delete_assets` runs non-interactively. For each loaded target it clears the package dirty flag and closes open asset editors before deleting under an unattended-script guard. `force=false` uses `ObjectTools::DeleteObjects`; `force=true` uses `ObjectTools::ForceDeleteObjects` for referenced assets. Failed deletion attempts are reported in `failed_to_delete[]` without aborting the response.

## 6. Settings

| Setting | Default | Effect |
|---------|---------|--------|
| `bEnableAsset` | `true` | Enables `MonolithAsset` startup registration for `asset` actions. Restart required after changing. |

## 7. Action Groups

| Group | Files | Notes |
|-------|-------|-------|
| Texture ingest | `Public/MonolithAssetTextureIngestActions.h`, `Private/MonolithAssetTextureIngestActions.cpp` | Public helper reused by `MonolithImageGen`; supports PNG, JPEG, BMP, EXR, TGA, HDR, TIFF, and DDS through `IImageWrapper`. Optional `texture_role` values are `ui_icon`, `sprite`, `decal`, `basecolor`, `world_tile`, `normal`, `orm_mask`, `height`, and `emissive`. |
| Font ingest | `Public/MonolithAssetFontIngestActions.h`, `Private/MonolithAssetFontIngestActions.cpp` | Uses UE 5.7-safe `UFont::GetMutableInternalCompositeFont()` for composite-font writes. |
| Lifecycle | `Public/MonolithAssetLifecycleActions.h`, `Private/MonolithAssetLifecycleActions.cpp` | Owns generic file texture import, asset save, and guarded delete operations previously scattered under editor/blueprint. |
| Hygiene | `Public/MonolithAssetHygieneActions.h`, `Private/MonolithAssetHygieneActions.cpp` | Owns naming convention validation and batch rename fixup. |
| Inspection | `Public/MonolithAssetInspectionActions.h`, `Private/MonolithAssetInspectionActions.cpp` | Former specialized asset inspection surface, now independent from `MonolithMaterial`. |
| Find | `Public/MonolithAssetFindActions.h`, `Private/MonolithAssetFindActions.cpp` | Fuzzy live-`AssetRegistry` search (`asset.find_assets`); thin consumer of MonolithCore `FMonolithFuzzyMatch`, owns its own corpus/fields/weights and the `allow_transposition` option. Distinct from exact-name `FMonolithAssetUtils::FindAssetCandidates` and offline `project` FTS search. |

## 8. Texture Role Pipeline

`asset.import_texture_from_bytes` accepts `texture_role` (or `role`) for Unreal-specific import behavior. Role presets apply before explicit `settings`; caller-provided `settings` fields remain the final override.

| Role | Import / post-processing contract |
|------|-----------------------------------|
| `ui_icon`, `sprite` | `sRGB=true`, `TEXTUREGROUP_UI`, `TMGS_NoMipmaps`, clamp addressing, edge-connected background alpha extraction for opaque-background generated images, and transparent-pixel RGB alpha bleed. |
| `decal` | `sRGB=true`, `TEXTUREGROUP_Effects`, group mips, clamp addressing, edge-connected background alpha extraction, alpha bleed, power-of-two warning. |
| `basecolor` | `sRGB=true`, `TC_Default`, `TEXTUREGROUP_World`, group mips, wrap addressing. |
| `world_tile` | Base-color world settings plus wrap addressing, one-pixel opposite-edge seam harmonization, power-of-two check, and opposite-edge seam validation. |
| `normal` | `sRGB=false`, `TC_Normalmap`, `TEXTUREGROUP_WorldNormalMap`, wrap addressing, power-of-two and tangent-space normal plausibility validation. |
| `orm_mask` | `sRGB=false`, `TC_Masks`, `TEXTUREGROUP_WorldSpecular`, wrap addressing, channel-range validation. |
| `height` | `sRGB=false`, `TC_Grayscale`, `TEXTUREGROUP_World`, wrap addressing, channel-range validation. |
| `emissive` | `sRGB=true`, `TEXTUREGROUP_Effects`, group mips, clamp addressing. |

For opaque-background generated images, alpha extraction first classifies pixels similar to the sampled edge background color, then flood-fills only those candidate pixels that are connected to an image edge. Disconnected interior pixels that resemble the background color remain opaque so highlights, pale hair, and enclosed details are not erased by global color keying.

The action result includes `texture_role`, `settings_applied`, and `validation`. Validation is non-destructive and returns warnings instead of failing the import unless normal parameter validation fails. `settings.alpha_from_edge_background=false` disables generated alpha extraction, `settings.tile_seam_harmonize=false` disables world-tile seam harmonization, and `return_processed_png=true` returns the imported post-processing result as `processed_png_b64`. `asset.inspect_asset` and `asset.validate_specialized_asset` recognize `Texture2D` assets and report generated texture-role settings from `Monolith.Generated.texture_role` metadata.

## 9. Verification

| Gate | Requirement |
|------|-------------|
| Source stale scan | No old UI ingest action names, old UI ingest classes, or old specialized-asset inspection class names remain in source. |
| Build | Run the primary `GoGameEditor` UBT command after closing any editor process that locks Monolith DLLs. |
| Runtime discovery | `monolith_discover({ "namespace": "asset" })` should list 12 actions owned by `MonolithAsset`. |
| Find engine reuse | `asset.find_assets` consumes `FMonolithFuzzyMatch` (MonolithCore); it must not duplicate edit-distance/tokenization, `allow_transposition` must flow into `ScoreCandidate`, and `FMonolithAssetUtils::FindAssetCandidates` stays exact-name. |
