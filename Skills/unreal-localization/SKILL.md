---
name: unreal-localization
description: Use for read-only Unreal culture discovery and bounded StringTable asset inspection or validation through Monolith's localization namespace. Routes StringTable authoring and localization import/export to their owning workflows instead of guessing mutation actions.
---

# unreal-localization

Use the `localization` namespace to inventory cultures and `UStringTable` assets, page through source/metadata safely, and preflight key/source quality without modifying, transacting, dirtying, importing, exporting, or saving content.

## Discovery

```text
monolith_discover({ namespace: "localization" })
describe_query({ action: "action_schema", params: {
  target_namespace: "localization", target_action: "<action>"
}})
```

The live catalog is authoritative. The four actions below are the complete read-only surface.

## Routing

Use this skill for:

- discovering available Unreal cultures or resolving explicit culture roots
- finding StringTable assets without loading the full result set
- reading stable bounded source-string and metadata pages
- validating empty tables, whitespace keys, empty source strings, and scan completeness

Use a different workflow for:

- `unreal-blueprints` — existing StringTable entry authoring actions
- the Unreal localization pipeline — CSV/PO import/export, gather, compile, dashboard, target, and archive workflows
- localization config — `DefaultGame.ini`, `DefaultEditor.ini`, or other INI inspection belongs to `config`
- mutation — the `localization` namespace is intentionally read-only; do not guess create, set, remove, import, export, or save action names

## Actions

| Action | Parameters | Result |
|---|---|---|
| `list_cultures` | `culture_names?` (max 256), `include_derived=true`, `offset=0`, `limit=100` (1–500) | Stable culture page, current culture/language/locale, and unresolved explicit names |
| `list_string_tables` | `path=/Game`, `offset=0`, `limit=200` (1–1000), `include_details=false` | Stable Asset Registry page; details load only returned assets and add namespace/entry count |
| `get_string_table` | `asset_path`, `after_key?`, `entry_limit=200` (1–1000), `include_metadata=false`, `metadata_limit=512` (0–4096), `text_limit=4096` (1–65536) | Exclusive-key entry page with independent entry, metadata, and text bounds |
| `validate_string_table` | `asset_path`, `scan_limit=4096` (1–10000), `issue_offset=0`, `issue_limit=200` (1–1000) | Deterministic issues, error/warning totals, issue pagination, `complete`, and `valid` |

## Contract

- Asset inputs accept canonical mounted package paths such as `/Game/Localization/ST_UI` or matching top-level object paths such as `/Game/Localization/ST_UI.ST_UI`.
- Filesystem paths, subobjects, leading/trailing whitespace, malformed paths, and mismatched object leaves are rejected. The handlers do not widen the search or substitute an alternate asset.
- Asset and culture lists are sorted before pagination. Use `total`, `offset`, `limit`, `count`, and `has_more` to continue.
- `after_key` is an exclusive, lexicographically stable cursor. When `has_more_entries=true`, pass the returned `next_after_key` unchanged into the next request.
- `all_entries_covered` asks whether one call covered the table from its first key through its last key. It is expected to be false on continuation pages even when that page reaches the end.
- `metadata_complete` is independent of entry coverage. The `metadata_limit` budget is shared across the returned entry page; source and metadata values also carry length/truncation fields controlled by `text_limit`.
- `get_string_table.complete` requires both `all_entries_covered` and `metadata_complete`.
- `validate_string_table.complete` requires every entry to fit inside `scan_limit`. A cutoff emits an error and forces `valid=false`; `valid=true` additionally requires zero errors. Warnings alone do not invalidate a complete table.
- Unreal `FTextKey` identity does not preserve simultaneous case-only duplicate rows in a StringTable. Do not promise a case-insensitive duplicate-key audit from this surface.

## Workflows

### Discover tables without loading all assets

```text
localization_query({ action: "list_string_tables", params: {
  path: "/Game/Localization",
  offset: 0,
  limit: 200
}})
```

Set `include_details: true` only when the returned page needs table namespace and entry-count data.

### Read a table incrementally

```text
localization_query({ action: "get_string_table", params: {
  asset_path: "/Game/Localization/ST_UI",
  entry_limit: 200,
  include_metadata: true,
  metadata_limit: 512,
  text_limit: 4096
}})
```

If `has_more_entries` is true, repeat with `after_key: "<next_after_key>"`. If complete metadata is required and `metadata_complete` is false, reduce the entry page or raise `metadata_limit` within its hard cap; do not infer that omitted metadata does not exist.

### Validate with explicit completeness

```text
localization_query({ action: "validate_string_table", params: {
  asset_path: "/Game/Localization/ST_UI",
  scan_limit: 4096,
  issue_offset: 0,
  issue_limit: 200
}})
```

Page `issues` separately through `issue_offset`. The error/warning totals and verdict cover the bounded validation run, not only the returned issue page.
