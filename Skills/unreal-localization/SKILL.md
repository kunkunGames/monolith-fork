---
name: unreal-localization
description: "Use for localization and text via Monolith MCP: localization targets, string tables, gather/import/export text, and culture management. Triggers on localization, loc, string table, culture, language, translate, gather text, FText, namespace key, localization target."
---

# unreal-localization

**10 actions** via `localization_query(action, params)`. Action names below are the live registry surface; call `monolith_discover` for exact parameter schemas.

## Discovery

```
monolith_discover({ namespace: "localization" })                      # all actions in this namespace
monolith_discover({ namespace: "localization", action: "<action>", mode: "schema" })  # exact params
```

## Action Reference

### Core (10)

| Action | Purpose |
|--------|---------|
| `create_string_table` | Create a StringTable asset under /Game. Requires dry_run=true or confirm=true. |
| `export_string_table_csv` | Export a StringTable to CSV under the project directory. Requires dry_run=true or confirm=true. |
| `get_string_table` | Inspect a StringTable asset and return capped entries. |
| `import_string_table_csv` | Import key,source_string,metadata CSV rows into a StringTable. Requires dry_run=true or confirm=true. |
| `list_cultures` | List available cultures known to Unreal internationalization. |
| `list_string_tables` | List StringTable assets under a project content path. |
| `remove_string_entry` | Remove one StringTable entry by key. Requires dry_run=true or confirm=true. |
| `set_string_entry` | Add or replace one StringTable entry and optional metadata. Requires dry_run=true or confirm=true. |
| `set_string_metadata` | Add, replace, or remove metadata on one StringTable entry. Requires dry_run=true or confirm=true. |
| `validate_string_table` | Validate a StringTable asset for empty keys, empty strings, duplicate-looking keys, and large output warnings. |

## Notes

- This reference is generated from the live `RegisterAction` surface. If an action is missing or renamed, re-run `monolith_discover({ namespace: "localization" })` — the catalog is the source of truth.
- Pass `mode: "schema"` to `monolith_discover` for required/optional params and types before calling an action.
