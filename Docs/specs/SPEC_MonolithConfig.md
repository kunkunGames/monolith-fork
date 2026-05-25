# Monolith — MonolithConfig Module

**Parent:** [SPEC_CORE.md](../SPEC_CORE.md)
**Engine:** Unreal Engine 5.7+
**Version:** 0.14.10 (Beta)

---

## MonolithConfig

**Dependencies:** Core, CoreUObject, Engine, MonolithCore, UnrealEd, AssetTools, Json, JsonUtilities

### Classes

| Class | Responsibility |
|-------|---------------|
| `FMonolithConfigModule` | Registers 10 config actions and 10 localization actions |
| `FMonolithConfigActions` | Static handlers. Helpers: ResolveConfigFilePath, GetConfigHierarchy (5 layers: Base -> Default -> Project -> User -> Saved). Uses GConfig API for reliable resolution |
| `FMonolithLocalizationActions` | Static handlers for culture inspection and guarded StringTable CRUD/import/export operations. |

### Actions (10 — namespace: "config")

| Action | Description |
|--------|-------------|
| `resolve_setting` | Get effective value via `GConfig->GetString`. Params: `file` (category), `section`, `key` |
| `explain_setting` | Show where value comes from across Base->Default->User layers. Auto-searches Engine/Game/Input/Editor if only `setting` given |
| `diff_from_default` | Compare config layers using GConfig API. Supports 5 INI layers (Base, Default, Project, User, Saved). Reports modified + added. Optional `section` filter |
| `search_config` | Full-text search across all config files. Max 100 results. Optional `file` filter |
| `get_section` | Read entire config section from a file |
| `get_config_files` | List all .ini files with hierarchy level and sizes. Optional `category` filter |
| `list_plugins` | List discovered plugins with enabled state and descriptor metadata. Read-only. |
| `get_plugin` | Get descriptor metadata for one discovered plugin. Read-only. |
| `get_cvar` | Get one console variable value and flags. Read-only. |
| `find_cvars` | Find console variables by prefix or substring. Read-only. |

### Actions (10 — namespace: "localization")

| Action | Description |
|--------|-------------|
| `list_cultures` | List available cultures known to Unreal internationalization. |
| `list_string_tables` | List StringTable assets under a project content path. |
| `get_string_table` | Inspect a StringTable asset and return capped entries. |
| `validate_string_table` | Validate a StringTable asset for empty keys, empty strings, duplicate-looking keys, and large output warnings. |
| `create_string_table` | Create a `UStringTable` asset under `/Game`. Requires `dry_run=true` or `confirm=true`; optional `namespace`, `save`. |
| `set_string_entry` | Add or replace one StringTable entry and optional metadata. Requires `asset_path`, `key`, `source_string`, and `dry_run=true` or `confirm=true`. |
| `remove_string_entry` | Remove one StringTable entry by key. Requires `asset_path`, `key`, and `dry_run=true` or `confirm=true`. |
| `set_string_metadata` | Add, replace, or remove metadata on one entry. Requires `asset_path`, `key`, `metadata_key`, and `dry_run=true` or `confirm=true`. |
| `import_string_table_csv` | Import CSV rows into a StringTable from an in-project file path. Requires `dry_run=true` or `confirm=true`; supports `replace_existing`, but refuses `replace_existing=true` when the CSV has zero accepted rows. |
| `export_string_table_csv` | Export StringTable rows to CSV under the project directory. Requires `dry_run=true` or `confirm=true`. |

### Localization Mutation Contract

| Rule | Requirement |
|------|-------------|
| Write gate | Every mutating localization action must reject calls unless `dry_run=true` or `confirm=true` is supplied. |
| Asset scope | StringTable asset paths must resolve under `/Game`; filesystem CSV paths must remain under the project directory unless the action explicitly documents a broader scope. |
| Return shape | Mutating actions return `dry_run`, `changed`, `saved`, `asset_path`, and a StringTable summary or row-level result object where applicable. |
| Package handling | Successful non-dry-run writes call `Modify()`, mark the package dirty, and save only when `save=true` is supplied. |
| CSV shape | CSV import/export uses `key,source_string,<metadata...>` headers; unknown metadata headers are preserved as per-entry metadata keys. |

---
