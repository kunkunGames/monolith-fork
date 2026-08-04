# Monolith — MonolithConfig Module

**Parent:** [SPEC_CORE.md](../SPEC_CORE.md)
**Engine:** Unreal Engine 5.7+
**Version:** 0.22.0 (Beta)

---

## MonolithConfig

**Dependencies:** Core, CoreUObject, Engine, MonolithCore, UnrealEd, AssetRegistry, Json, JsonUtilities, DeveloperSettings
**Namespaces:** `config` (7 actions) and `localization` (4 actions)
**Tools:** `config_query(action, params)` and `localization_query(action, params)`
**Settings toggle:** `bEnableConfig` gates only `config`; `localization` always registers because it is read-only asset inspection

### Classes

| Class | Responsibility |
|---|---|
| `FMonolithConfigModule` | Registers `localization` before the config toggle, conditionally registers `config`, reports live registry counts, and unregisters both namespaces on shutdown |
| `FMonolithConfigActions` | Resolves the five-layer INI hierarchy and owns the editor-only reflected `UDeveloperSettings` setter |
| `FMonolithLocalizationActions` | Discovers cultures and StringTable assets, reads bounded entry/metadata pages, and validates deterministic bounded key/source issues without mutation |

---

## Config namespace

### Actions (7 — namespace: `config`)

| Action | Description |
|---|---|
| `resolve_setting` | Get an effective value through `GConfig->GetString`. Parameters: `file`, `section`, `key` |
| `explain_setting` | Explain the value across Base → Default → Project → User → Saved layers; can search common categories from `setting` |
| `diff_from_default` | Compare resolved project layers with engine defaults; optional section filter |
| `search_config` | Full-text search across config files with a bounded result set |
| `get_section` | Read an entire config section |
| `get_config_files` | List INI files with hierarchy level and size; optional category filter |
| `set_developer_setting` | Editor-only reflected `UDeveloperSettings` CDO write; optional `save_config` persists it |

The first six actions are reads. `set_developer_setting` is the only write action in this module and therefore prevents the mixed `config` dispatcher from advertising a read-only hint.

---

## Localization namespace

### Ownership and registration

`FMonolithLocalizationActions::RegisterActions` runs before the `bEnableConfig` check. Turning off config authoring cannot remove localization preflight. The dispatcher publishes `readOnlyHint=true` and `idempotentHint=true`; no localization handler calls `Modify`, starts a transaction, marks a package dirty, creates/deletes an asset, or saves a package.

`AssetRegistry` is a direct private dependency because StringTable discovery queries `UStringTable` assets without loading the whole result set. Only a returned discovery page is loaded when `include_details=true`.

### Actions (4 — namespace: `localization`)

| Action | Input bounds | Contract |
|---|---|---|
| `list_cultures` | `culture_names` max 256; `offset >= 0`; `limit` 1–500, default 100 | Resolve all known cultures or explicit roots plus optional derived cultures; sort by culture name; return current culture/language/locale and unresolved explicit names |
| `list_string_tables` | canonical `path`, default `/Game`; `offset >= 0`; `limit` 1–1000, default 200 | Query and sort StringTable asset identities before pagination; optional details load only the returned page and add table id, namespace, internal flag, and entry count |
| `get_string_table` | canonical `asset_path`; `after_key` max 4096 chars; `entry_limit` 1–1000, default 200; `metadata_limit` 0–4096, default 512; `text_limit` 1–65536, default 4096 | Return the lexicographically smallest keys after an exclusive cursor without materializing an unbounded sorted table; bound source/metadata text and the aggregate metadata page independently |
| `validate_string_table` | `scan_limit` 1–10000, default 4096; `issue_offset >= 0`; `issue_limit` 1–1000, default 200 | Inspect a deterministic smallest-key prefix; empty table and scan cutoff are errors, key-edge whitespace and empty source strings are warnings; paginate the stable issue list |

### Path and failure contract

Asset inputs accept mounted package paths such as `/Game/Localization/ST_UI` and matching top-level object paths such as `/Game/Localization/ST_UI.ST_UI`. Filesystem paths, subobjects, leading/trailing whitespace, malformed mounted paths, and object leaves that do not match the package leaf return invalid-parameter errors. A resolved asset must be exactly a `UStringTable`; no alternate asset, widened search root, or legacy fallback is substituted.

Every list response includes `total`, `offset`, `limit`, `count`, and `has_more`. Discovery results are sorted by object path before slicing.

### Readback completeness

`get_string_table` uses `after_key` as an exclusive stable cursor and returns `next_after_key` when another page exists. The response separates three questions:

| Field | Meaning |
|---|---|
| `has_more_entries` | More keys exist after the returned page |
| `all_entries_covered` | This call started at the beginning and reached the final key |
| `metadata_complete` | The shared metadata budget covered every metadata row on the returned entries |
| `complete` | Both `all_entries_covered` and `metadata_complete` are true |

Each source string and metadata value also returns original length and a truncation flag. Metadata counts are exposed per entry and for the whole page. A caller can therefore distinguish entry continuation, metadata budget exhaustion, and text clipping.

### Validation completeness

Validation emits stable `{code, severity, message, key?}` rows sorted by key, code, then message. `errors`, `warnings`, and `issue_total` cover the whole scanned result even when the returned issue page is smaller. `has_more_issues` applies only to issue pagination.

`complete=true` requires the entire table to fit inside `scan_limit`. A cutoff emits `scan_limit_exceeded` as an error. `valid=true` requires both completeness and zero errors; warnings alone do not invalidate a complete table. Case-only duplicate detection is intentionally absent because Unreal's `FTextKey` identity does not preserve simultaneous case-only duplicate rows in a `FStringTable`.

---

## Extension points

| Need | Route |
|---|---|
| Add another read-only localization audit | Extend `FMonolithLocalizationActions`, retain explicit bounds/completeness, and add focused tests |
| Author StringTable entries | Use the existing `blueprint` StringTable actions; do not add mutations to the read-only localization dispatcher |
| Import/export CSV or PO data | Use the established localization pipeline or add a separately reviewed authoring namespace with transaction/save semantics |
| Inspect live schemas | Use `monolith_discover("localization")` and `describe_query("action_schema", ...)` |

Focused verification is recorded in [2026-08-04-string-table-discovery-validation.md](../testing/2026-08-04-string-table-discovery-validation.md). Workflow routing lives in `Skills/unreal-localization/SKILL.md`.
