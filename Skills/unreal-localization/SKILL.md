---
name: unreal-localization
description: Use when managing Unreal StringTable assets and culture discovery via Monolith MCP (localization namespace) - list/validate/create StringTables, set/remove entries and metadata, and import/export project-scoped CSV. For gather-text targets or PO archives use Unreal's supported localization dashboard or commandlet workflow; those operations are not exposed by this namespace. For UMG display text use unreal-ui, reference search use unreal-project-search, and startup culture .ini settings use unreal-config. Triggers on localization, loc, l10n, i18n, string table, StringTable, string entry, namespace key, FText, CSV import, CSV export, culture, locale, language, translate, translation.
---

# unreal-localization

**10 actions** via `localization_query(action, params)`. Action names below are the live registry surface; use `describe_query` for one exact schema.

## Discovery

```
monolith_discover({ namespace: "localization" })  # action names + descriptions
describe_query("action_schema", target_namespace="localization", target_action="<action>")  # one exact schema
monolith_discover({ namespace: "localization", detail: true })  # all schemas, when the full namespace is needed
```

## When to use

Use this skill for the implemented localization data layer — culture discovery plus StringTable assets, entries, per-entry metadata, validation, and project-scoped CSV import/export.

Use a different skill for:

- **unreal-ui** — when setting an `FText` display string directly on a UMG widget (`set_widget_property` text). This skill owns the string-table source the widget binds to, not the widget itself.
- **unreal-project-search** — when finding which assets reference a given StringTable key or `FText`, versus managing the localization targets/cultures and string-table contents here.
- **unreal-config** — when the change is a culture/internationalization setting in project `.ini` (default/startup culture), versus string-table and gather/export operations here.
- **Unreal Localization Dashboard/commandlets** — when gathering source text, managing localization targets, or importing/exporting PO archives. This namespace does not expose those workflows.

## Action Reference

Param notation: `name*` required, `name?` optional, `name=val` default, `a/b/c` allowed, `[w]` mutates. Signatures are a snapshot of the live catalog — for one exact current schema call `describe_query("action_schema", target_namespace="localization", target_action="<action>")`.

### Core (10)

| Action | Params | Purpose |
|--------|--------|---------|
| `list_cultures` | `culture_names?` `include_derived?=true` | List available cultures known to Unreal internationalization. |
| `list_string_tables` | `path?=/Game` `include_entries?=false` `include_metadata?=false` `limit?=100` | List StringTable assets under a project content path; one shared `limit` bounds both table summaries and aggregate returned entry rows. |
| `get_string_table` | `asset_path*` `include_metadata?=true` `limit?=200` | Inspect a StringTable asset and return capped entries. |
| `validate_string_table` | `asset_path*` | Validate a StringTable; returns at most 200 issue rows plus full/returned/truncated counts. |
| `[w] create_string_table` | `asset_path*` `namespace?` `dry_run?=false` `confirm?=false` `save?=false` | Create a StringTable asset under /Game. Requires dry_run=true or confirm=true. |
| `[w] set_string_entry` | `asset_path*` `key*` `source_string*` `metadata?` `dry_run?=false` `confirm?=false` `save?=false` | Add or replace one StringTable entry and optional metadata. Requires dry_run=true or confirm=true. |
| `[w] remove_string_entry` | `asset_path*` `key*` `dry_run?=false` `confirm?=false` `save?=false` | Remove one StringTable entry by key. Requires dry_run=true or confirm=true. |
| `[w] set_string_metadata` | `asset_path*` `key*` `metadata_key*` `metadata_value?` `remove?=false` `dry_run?=false` `confirm?=false` `save?=false` | Add, replace, or remove metadata on one StringTable entry. Requires dry_run=true or confirm=true. |
| `[w] import_string_table_csv` | `asset_path*` `file_path*` `replace_existing?=false` `dry_run?=false` `confirm?=false` `save?=false` | Import required `key`/`source_string` columns plus per-metadata-key columns into a StringTable. Requires dry_run=true or confirm=true. |
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
- `get_string_table` caps returned entries. `list_string_tables` applies one shared entry-row budget across all returned table summaries and reports `available_entry_count`, `returned_entry_count`, and `truncated_entry_count`. For a full audit pair reads with `validate_string_table`, whose `issues` array is capped at 200 but whose count fields report the full result.
- Metadata keys with leading/trailing whitespace are rejected, as are reserved structural names. Treat metadata names as case-insensitive `FName` identities even when a client-side JSON library appears to allow case variants.
- CSV headers must contain `key` and `source_string`. `__monolith_metadata_presence_v1` is a Monolith structural column, not metadata: export writes a per-row JSON array there so an empty metadata value remains distinguishable from an absent field, and import validates and consumes it. Do not rename or remove that column when lossless empty-versus-absent round trips matter. Every other additional header is a metadata key and may not contain edge whitespace.
- This reference is generated from the live `RegisterAction` surface. If an action is missing or renamed, re-run `monolith_discover({ namespace: "localization" })` — the catalog is the source of truth.
- Call `describe_query("action_schema", target_namespace="localization", target_action="<action>")` for required/optional params and types; use `monolith_discover({ namespace: "localization", detail: true })` only when every schema is needed.
