---
name: unreal-asset
description: Use for generic, cross-domain Unreal asset lifecycle work via the Monolith MCP asset namespace - Texture2D and TTF/OTF font ingest, file import, create/duplicate/move/save/delete asset, asset metadata/tags/thumbnail edits, naming validation, batch rename, fuzzy live asset find, asset-exists checks, and generic untyped asset inspection. For pipeline-stack imports (glTF/FBX/USD/reimport) use unreal-interchange; for AI-generated textures use unreal-imagegen; for sprite-sheet ingest use unreal-sprite; for project-wide FTS asset search use unreal-project-search; for material or material-instance assets use unreal-materials. Triggers on asset, import texture, Texture2D, font import, TTF/OTF font, create asset, duplicate asset, move asset, save asset, delete asset, asset exists, asset metadata, asset tags, thumbnail, rename assets, batch rename, find asset by name, inspect asset.
---

# Unreal Asset Skill

Generic, cross-domain asset lifecycle work that is not owned by one content system. Drives the Monolith MCP `asset` namespace via `asset_query("<action>", { ...params })`.

## Discover First

The live catalog is authoritative for action names and schemas. Discover before mutating or importing instead of guessing action names.

```text
# List every action in the asset namespace.
monolith_discover({ "namespace": "asset" })

# Read the exact parameter schema for one action before calling it.
monolith_discover({ "namespace": "asset", "action": "import_texture_from_bytes", "mode": "schema" })
```

When the right action is unclear, `monolith_find("<task>")` suggests candidates across namespaces.

Module spec: `Docs/specs/SPEC_MonolithAsset.md`

## When To Use / Use A Different Skill For

Use this skill for untyped, cross-domain asset operations: bulk import of textures/fonts, create/duplicate/move/save/delete, metadata/tags/thumbnail, naming hygiene, batch rename, and a quick fuzzy live find.

| Instead, route to | When |
| --- | --- |
| `unreal-interchange` | The import goes through the Interchange framework or a glTF/FBX/USD pipeline stack, or you need reimport. |
| `unreal-imagegen` | The texture must be AI-generated rather than imported from bytes/file. |
| `unreal-sprite` | Ingesting a generated image into the sprite-sheet production pipeline. |
| `unreal-project-search` | You need cross-project FTS asset search rather than a single fuzzy live find. |
| `unreal-materials` | The asset is specifically a material or material instance. |

Typed inspection of a material, mesh, or animation asset belongs to that domain skill (`unreal-materials`, `unreal-mesh`, `unreal-animation`). Use `inspect_asset` here only for generic, untyped asset details.

## Action Reference

Param notation: `name*` required, `name?` optional, `name=val` default, `a/b/c` allowed values, `[w]` mutates (transaction-wrapped). Signatures are a snapshot of the live catalog - for the exact, full, current schema of any action call `monolith_discover` with `mode: "schema"`.

| Action | Params (req* opt? =default) | Purpose |
| --- | --- | --- |
| `[w] import_texture_from_bytes` | `destination*`, `bytes_b64*`, `format_hint*` (png/jpg/jpeg/bmp/exr/tga/hdr/tif/tiff/dds), `texture_role?` (ui_icon/sprite/decal/basecolor/world_tile/normal/orm_mask/height/emissive), `settings?` (obj: compression_settings, srgb, mip_gen_settings, lod_group, address_x, address_y, alpha_bleed, alpha_from_edge_background, tile_seam_harmonize), `save=true`, `return_processed_png=false` | Decode base64 image and import as a `UTexture2D`. |
| `[w] import_texture_from_file` | `source_path*` (aliases: source_file/file_path/path), `destination*` (aliases: dest_path/destination_path), `asset_name?` (then destination is the folder), `settings?` (obj: compression, srgb, tiling, max_size, lod_group), `replace_existing=false`, `overwrite_policy?` (overwrite/replace->true; fail/unique->false) | Import an external image file as a `UTexture2D`. |
| `[w] import_font_family` | `destination*` (output dir), `family_name*`, `faces*` (array of {typeface, source_path absolute TTF}), `loading_policy=LazyLoad` (LazyLoad/Stream/Inline), `hinting=Default` (Default/Auto/AutoLight/Monochrome/None), `save=true` | Import a TTF/OTF font family as a composite `UFont` + per-face `UFontFace`. |
| `[w] save_asset` | `asset_path*` | Save a loaded asset package to disk. |
| `[w] delete_assets` | `asset_paths*` (array), `allowed_prefixes?` (array, restrict targets), `dry_run=false`, `force=false` (delete referenced assets) | Guarded asset deletion. |
| `validate_naming_conventions` | `scan_path=/Game`, `max_results=100`, `custom_rules?` (obj: {"ClassName": "Prefix_"}) | Scan prefix/path hygiene (SM_/SK_/M_/MI_/T_/BP_ defaults). |
| `[w] batch_rename_assets` | `asset_paths*` (array), `find?`, `replace?`, `add_prefix?`, `remove_prefix?`, `add_suffix?`, `remove_suffix?`, `dry_run=false` | Find/replace, prefix, or suffix rename with reference fixup + redirectors. |
| `find_assets` | `query*`, `path=/Game`, `recursive=true`, `class_names?` (array; alias class), `limit=20` (1-100), `threshold?`, `include_tags=false`, `include_score_breakdown=false`, `allow_transposition=true` | Fuzzy, typo-tolerant live AssetRegistry search (sees unsaved assets). |
| `inspect_asset` | `asset_path*`, `include_references=true`, `array_limit=32` | Read generic/untyped asset details with read-only enrichment. |
| `inspect_assets_batch` | `asset_paths*` (array), `include_references=false`, `array_limit=16` | Inspect many assets with per-row success/error. |
| `validate_specialized_asset` | `asset_path*`, `array_limit=32` | Validate a specialized asset; report warnings without mutation. |
| `list_supported_asset_enrichers` | (none) | List read-only specialized asset enrichers Monolith supports. |

## Common Workflows

```text
# Import a texture from raw bytes, then verify it.
asset_query("import_texture_from_bytes", { "destination": "/Game/UI/Icons/IconSkill", "bytes_b64": "<base64>", "format_hint": "png", "texture_role": "ui_icon" })
asset_query("inspect_asset", { "asset_path": "/Game/UI/Icons/IconSkill" })

# Find a live asset by fuzzy name.
asset_query("find_assets", { "query": "IconSkill", "limit": 10 })

# Dry-run a delete before applying it.
asset_query("delete_assets", { "asset_paths": ["/Game/UI/Icons/IconOld"], "dry_run": true })

# Preview a batch rename (add a prefix), then apply by dropping dry_run.
asset_query("batch_rename_assets", { "asset_paths": ["/Game/UI/Icons/IconSkill"], "add_prefix": "T_", "dry_run": true })
```

### Recipe: ingest a texture, then organize it to the naming contract

End-to-end import-and-organize pass. Imports a file as a `UTexture2D`, confirms the imported result, fixes naming hygiene with a previewed batch rename, then saves the package. (For a glTF/FBX/USD pipeline-stack import use `unreal-interchange`; once the texture is wired into a material graph hand off to `unreal-materials`.)

```text
# 1. Import the source file (asset_name set, so destination is the folder).
asset_query("import_texture_from_file", { "source_path": "C:/Art/icon_skill.png", "destination": "/Game/UI/Icons", "asset_name": "icon_skill", "settings": { "srgb": true }, "replace_existing": false })

# 2. Confirm the imported asset and read its generic details.
asset_query("inspect_asset", { "asset_path": "/Game/UI/Icons/icon_skill", "include_references": true })

# 3. Scan naming hygiene to see whether the new asset matches prefix rules.
asset_query("validate_naming_conventions", { "scan_path": "/Game/UI/Icons", "max_results": 50 })

# 4. Preview a prefix fix (Texture2D -> T_), then apply by dropping dry_run.
asset_query("batch_rename_assets", { "asset_paths": ["/Game/UI/Icons/icon_skill"], "add_prefix": "T_", "dry_run": true })
asset_query("batch_rename_assets", { "asset_paths": ["/Game/UI/Icons/icon_skill"], "add_prefix": "T_", "dry_run": false })

# 5. Save the renamed package to disk.
asset_query("save_asset", { "asset_path": "/Game/UI/Icons/T_icon_skill" })
```

## Gotchas / Rules

- Asset paths are UE object paths such as `/Game/Folder/Asset`, not `.uasset` filesystem paths.
- Prefer exact schema discovery over examples; import and delete actions are high-impact - use `dry_run`/guarded options when deleting or batch-renaming.
- Do not route material graph authoring, mesh editing, UI widget work, or Paper2D authoring through generic asset actions after import. Hand off to the owning domain skill.
- For indexed, content-inclusive project discovery use `unreal-project-search`; `find_assets` here is a live AssetRegistry fuzzy lookup only.
