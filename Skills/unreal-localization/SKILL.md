---
name: unreal-localization
description: Use when localizing Unreal text via Monolith MCP (localization namespace) - StringTable assets, set/remove string entries and metadata, CSV import/export, validation, gather/import/export text, localization targets, and culture management. For setting FText display strings on UMG widgets use unreal-ui; to find which assets reference a string-table key or FText use unreal-project-search; for culture/internationalization settings in project .ini use unreal-config. Triggers on localization, loc, l10n, i18n, string table, StringTable, string entry, namespace key, FText, gather text, import text, export text, CSV import, culture, locale, language, translate, translation, localization target.
---

# unreal-localization

**10 actions** via `localization_query(action, params)`. Action names below are the live registry surface; call `monolith_discover` for exact parameter schemas.

## Discovery

```
monolith_discover({ namespace: "localization" })                      # all actions in this namespace
monolith_discover({ namespace: "localization", action: "<action>", mode: "schema" })  # exact params
```

## When to use

Use this skill for the localization data layer — StringTable assets, string entries and per-entry metadata, CSV import/export, gather/import/export text, validation, localization targets, and culture/locale management.

Use a different skill for:

- **unreal-ui** — when setting an `FText` display string directly on a UMG widget (`set_widget_property` text). This skill owns the string-table source the widget binds to, not the widget itself.
- **unreal-project-search** — when finding which assets reference a given StringTable key or `FText`, versus managing the localization targets/cultures and string-table contents here.
- **unreal-config** — when the change is a culture/internationalization setting in project `.ini` (default/startup culture), versus string-table and gather/export operations here.

## Action Reference

Param notation: `name*` required, `name?` optional, `name=val` default, `a/b/c` allowed, `[w]` mutates. Signatures are a snapshot of the live catalog — for the exact full schema call `monolith_discover` with `mode: "schema"`. The Discovery block above stays the authority.

### Core (10)

| Action | Params | Purpose |
|--------|--------|---------|
| `list_cultures` | `culture_names?` `include_derived?=true` | List available cultures known to Unreal internationalization. |
| `list_string_tables` | `path?=/Game` `include_entries?=false` `include_metadata?=false` `limit?=100` | List StringTable assets under a project content path. |
| `get_string_table` | `asset_path*` `include_metadata?=true` `limit?=200` | Inspect a StringTable asset and return capped entries. |
| `validate_string_table` | `asset_path*` | Validate a StringTable for empty keys, empty strings, duplicate-looking keys, and large output warnings. |
| `[w] create_string_table` | `asset_path*` `namespace?` `dry_run?=false` `confirm?=false` `save?=false` | Create a StringTable asset under /Game. Requires dry_run=true or confirm=true. |
| `[w] set_string_entry` | `asset_path*` `key*` `source_string*` `metadata?` `dry_run?=false` `confirm?=false` `save?=false` | Add or replace one StringTable entry and optional metadata. Requires dry_run=true or confirm=true. |
| `[w] remove_string_entry` | `asset_path*` `key*` `dry_run?=false` `confirm?=false` `save?=false` | Remove one StringTable entry by key. Requires dry_run=true or confirm=true. |
| `[w] set_string_metadata` | `asset_path*` `key*` `metadata_key*` `metadata_value?` `remove?=false` `dry_run?=false` `confirm?=false` `save?=false` | Add, replace, or remove metadata on one StringTable entry. Requires dry_run=true or confirm=true. |
| `[w] import_string_table_csv` | `asset_path*` `file_path*` `replace_existing?=false` `dry_run?=false` `confirm?=false` `save?=false` | Import key,source_string,metadata CSV rows into a StringTable. Requires dry_run=true or confirm=true. |
| `[w] export_string_table_csv` | `asset_path*` `file_path*` `include_metadata?=true` `dry_run?=false` `confirm?=false` | Export a StringTable to CSV under the project directory. Requires dry_run=true or confirm=true. |

## Common Workflows

### List existing StringTable assets, then inspect one
```
localization_query({ action: "list_string_tables", params: { path: "/Game" } })
localization_query({ action: "get_string_table", params: { asset_path: "/Game/Localization/ST_UI" } })
```

### Create a StringTable and add an entry
```
localization_query({ action: "create_string_table", params: { asset_path: "/Game/Localization/ST_UI", dry_run: true } })
localization_query({ action: "set_string_entry", params: { asset_path: "/Game/Localization/ST_UI", key: "MainMenu_Play", source_string: "Play", confirm: true } })
```

### Round-trip through CSV for translators
```
localization_query({ action: "export_string_table_csv", params: { asset_path: "/Game/Localization/ST_UI", file_path: "Saved/Localization/ST_UI.csv", confirm: true } })
localization_query({ action: "import_string_table_csv", params: { asset_path: "/Game/Localization/ST_UI", file_path: "Saved/Localization/ST_UI.csv", confirm: true } })
```

### Validate before shipping, and list known cultures
```
localization_query({ action: "validate_string_table", params: { asset_path: "/Game/Localization/ST_UI" } })
localization_query({ action: "list_cultures", params: {} })
```

## Gotchas / Rules

- Mutating actions (`create_string_table`, `set_string_entry`, `set_string_metadata`, `remove_string_entry`, `import_string_table_csv`, `export_string_table_csv`) require `dry_run=true` or `confirm=true`. Run `dry_run` first to preview, then re-issue with `confirm=true`.
- `get_string_table` caps returned entries; for a full audit pair it with `validate_string_table` rather than relying on the capped list.
- CSV rows are `key,source_string,metadata`; keep keys stable across export/import so translations re-link to the right entry.
- This reference is generated from the live `RegisterAction` surface. If an action is missing or renamed, re-run `monolith_discover({ namespace: "localization" })` — the catalog is the source of truth.
- Pass `mode: "schema"` to `monolith_discover` for required/optional params and types before calling an action.
