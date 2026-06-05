---
name: unreal-asset
description: "Use for generic Unreal asset workflows via Monolith MCP: Texture2D/font ingest, file import, asset save/delete, naming validation, batch rename, fuzzy asset find, and specialized asset inspection. Triggers on asset, import texture, font import, save asset, delete asset, rename assets, inspect asset."
---

# Unreal Asset Skill

Use this skill for cross-domain asset lifecycle work that is not owned by one content system.

## Namespace

- Primary MCP namespace: `asset`
- Tool form: `asset_query("<action>", { ...params })`
- Module/spec: `Docs/specs/SPEC_MonolithAsset.md`

## Workflow

1. Use `monolith_find("<task>")` when the exact asset action is unclear.
2. Use `monolith_discover({ "namespace": "asset", "action": "<action>", "mode": "schema" })` before mutating or importing.
3. Use `project_query("search", ...)` for indexed project discovery and `asset_query("find_assets", ...)` for live AssetRegistry fuzzy lookup.
4. Use dry-run or guarded options when deleting or batch-renaming assets.
5. Verify with `asset_query("inspect_asset", ...)`, `asset_query("validate_specialized_asset", ...)`, or a domain-specific query after import.

## Common Actions

- `import_texture_from_bytes`: import compressed image bytes as `UTexture2D`.
- `import_texture_from_file`: import an external image file with texture settings.
- `import_font_family`: import TTF font families.
- `save_asset`: save a loaded asset package.
- `delete_assets`: guarded asset deletion.
- `validate_naming_conventions`: scan prefix and path hygiene.
- `batch_rename_assets`: preview or apply asset rename operations.
- `find_assets`: fuzzy live AssetRegistry search.
- `inspect_asset`, `inspect_assets_batch`, `validate_specialized_asset`: read and validate asset details.

## Safety

- Asset paths are UE object paths such as `/Game/Folder/Asset`, not `.uasset` filesystem paths.
- Prefer exact schema discovery over examples; import and delete actions are high-impact.
- Do not route material graph authoring, mesh editing, UI widget work, or Paper2D authoring through generic asset actions after import. Hand off to the owning domain skill.
