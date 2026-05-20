---
name: unreal-chooser
description: "Use for the Chooser Table system (data-driven asset/animation selection) via Monolith MCP: create chooser/proxy tables, rows, columns, and evaluate selection. Triggers on chooser, chooser table, proxy table, selection table, data-driven selection, lookup table, chooser column."
---

# unreal-chooser

**6 actions** via `chooser_query(action, params)`. Action names below are the live registry surface; call `monolith_discover` for exact parameter schemas.

## Discovery

```
monolith_discover({ namespace: "chooser" })                      # all actions in this namespace
monolith_discover({ namespace: "chooser", action: "<action>", mode: "schema" })  # exact params
```

## Action Reference

### AI Chooser (6)

| Action | Purpose |
|--------|---------|
| `get_chooser_table` | Inspect a ChooserTable summary: rows, columns, results, context fields, and references. |
| `list_chooser_columns` | Return reflected ChooserTable columns with input/output type and row-value counts. |
| `list_chooser_references` | List object and soft-object references found in a ChooserTable. |
| `list_chooser_rows` | Return reflected ChooserTable rows, per-column cells, disabled state, and row result data. |
| `list_chooser_tables` | List ChooserTable assets without hard-linking the Chooser plugin. |
| `validate_chooser_table` | Validate ChooserTable row/column consistency and unresolved reflected references. |

## Notes

- This reference is generated from the live `RegisterAction` surface. If an action is missing or renamed, re-run `monolith_discover({ namespace: "chooser" })` — the catalog is the source of truth.
- Pass `mode: "schema"` to `monolith_discover` for required/optional params and types before calling an action.
