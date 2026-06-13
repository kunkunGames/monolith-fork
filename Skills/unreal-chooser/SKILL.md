---
name: unreal-chooser
description: Use for the Chooser Table selection mechanism via Monolith MCP chooser namespace — create and inspect chooser/proxy tables, rows, columns, and evaluate which asset/animation a context selects. unreal-chooser owns the SELECTION MECHANISM (chooser/proxy tables, rows, columns, evaluate); to edit the animation assets a selection feeds use unreal-animation; if the data-driven logic belongs in a Blueprint or DataTable use unreal-blueprints. Triggers on chooser, chooser table, proxy table, selection table, chooser column, evaluate chooser, chooser proxy, select animation by state, pick asset by context, data-driven anim, selection logic, output objects, lookup table.
---

# unreal-chooser

**16 actions** via `chooser_query(action, params)`. Action names below are the live registry surface; call `monolith_discover` for exact parameter schemas.

## Discovery

```
monolith_discover({ namespace: "chooser" })                      # all actions in this namespace
monolith_discover({ namespace: "chooser", action: "<action>", mode: "schema" })  # exact params
```

The discover-first block is the authority. The inline signatures below are a snapshot of the live catalog (~1,600 actions across all namespaces) and can drift between versions — for the exact, full, current schema of any action call `monolith_discover` with `mode: "schema"`.

## When to use

Use this skill for the **selection mechanism** — chooser/proxy tables, their rows and columns, and evaluating which asset or animation a runtime context selects.

### Use a different skill for

- **unreal-animation** — when the selection result feeds animation but you actually need to edit the anim assets (sequences, montages, blend spaces, ABPs).
- **unreal-blueprints** — when the data-driven logic should live in a Blueprint or DataTable instead of a chooser table.

## Action Reference

Param notation: `name*` required, `name?` optional, `name=val` default, `a/b/c` allowed values, `[w]` mutates (its execution policy wraps a transaction / dirties the package). Signatures are a snapshot of the live catalog — for the exact, full, current schema of any action call `monolith_discover` with `mode: schema`.

### Inspect / list (read-only)

| Action | Params (req* opt? =default) | Purpose |
|--------|-----------------------------|---------|
| `list_chooser_tables` | `path_filter?` | List ChooserTable assets without hard-linking the Chooser plugin. `path_filter` restricts to a package prefix. |
| `get_chooser_table` | `asset_path*`, `include_rows=false`, `row_limit=50` | Inspect a ChooserTable summary: rows, columns, results, context fields, references. |
| `inspect_chooser` | `asset_path*`, `recursive?` | Read-only deep inspection: context-data params (class/struct reqs), result type+class, row/column counts+types, referenced assets, compile/validation status. `recursive=true` emits the full nested tree (asset / soft_asset / evaluate_chooser / nested_chooser kinds), output-object cells, fallback, parent_table/root_chooser. |
| `list_chooser_columns` | `asset_path*` | Reflected columns with input/output type and row-value counts. |
| `list_chooser_rows` | `asset_path*`, `start_row=0`, `limit=100` | Reflected rows, per-column cells, disabled state, and row result data. |
| `list_chooser_references` | `asset_path*` | Object and soft-object references found in a ChooserTable. |
| `validate_chooser_table` | `asset_path*` | Validate row/column consistency and unresolved reflected references. |
| `validate_chooser` | `asset_path*`, `expected_context_class?`, `expected_result_type?` | Compile (`Compile(true)`) and validate: optional expected context class + expected result type (`ObjectResult`/`ClassResult`/`NoPrimaryResult`), plus null/stale result-row refs. Read-only apart from the compile pass. |

### Create / edit (write)

| Action | Params (req* opt? =default) | Purpose |
|--------|-----------------------------|---------|
| `[w] create_chooser_table` | `asset_path*`, `output_type=Object`, `output_class?`, `context_class?` | Create a new UChooserTable. `output_type`: `ObjectResult`(=Object, default) / `ClassResult` / `NoPrimaryResult`. `output_class` = Result Class; `context_class` = context object class param. |
| `[w] add_chooser_column` | `asset_path*`, `column_kind*`, `binding_property?`, `enum_class?` | Append a column. `column_kind`: `Bool` / `Enum` / `GameplayTag` / `FloatRange` / `OutputObject`. `binding_property` = dotted input-binding path (input columns only); `enum_class` = UEnum path/short name (Enum columns only). New column's per-row array is grown to current row count. |
| `[w] add_chooser_row` | `asset_path*`, `cells*`, `output_psd*` | Append a row. `cells` = one entry per INPUT column in order (Bool: `true`/`false`/`any`; Enum: int; FloatRange: `{min,max}`; GameplayTag: tag string). `output_psd` = asset this row selects (written as FAssetChooser). All parallel arrays grow by exactly 1 atomically. |
| `[w] set_chooser_cell` | `asset_path*`, `column_index*`, `row_index*`, `bool_value?`, `enum_value?`, `comparison?`, `float_min?`, `float_max?`, `tags?` | Set a typed predicate into `(column_index,row_index)`. Bool→`bool_value` (or `'any'` string); Enum→`enum_value` int [+ `comparison`: `MatchEqual`(default)/`MatchNotEqual`/`MatchAny`]; FloatRange→`float_min`+`float_max`; GameplayTag→`tags` (string or array). |
| `[w] set_context_object_class` | `asset_path*`, `context_name_or_index*`, `class_path*` | Rewrite the Class on a ContextData entry (FContextObjectTypeClass). `context_name_or_index` = 0-based index; non-numeric selects first class-typed entry. Recompiles. |
| `[w] set_result_asset_reference` | `asset_path*`, `row_or_column*`, `asset_path_value*` | Rewrite the Asset ref on a result row (FAssetChooser/FSoftAssetChooser). `row_or_column` = 0-based result row index. Recompiles. |
| `[w] set_evaluate_chooser_result_reference` | `asset_path*`, `row*`, `child_chooser_path*` | Rewrite the child UChooserTable an EvaluateChooser result row points at (FEvaluateChooser); use for root/nested rows that `set_result_asset_reference` cannot set. Recompiles. |
| `[w] duplicate_chooser_tree` | `source_assets*`, `destination_folder*`, `remap_rules?` | Duplicate one or more chooser tables into `destination_folder` (sources untouched). `remap_rules` = map of old-path → new-path applied to RootChooser/ParentTable/NestedChoosers and result asset refs. |

## Common Workflows

Numbered recipes use only the actions in the table above. Run `monolith_discover` with `mode: "schema"` for the exact params before each call. Scaffold a fresh table **columns before rows** — every `[w]` add grows all parallel arrays atomically, so a row added before its input columns has nowhere to put its cells.

### Recipe 1 — Scaffold a new chooser table end-to-end

1. `chooser_query("create_chooser_table", { asset_path, output_type: "ObjectResult", output_class, context_class })` — make the empty `UChooserTable`; set `context_class` to the object the runtime passes in and `output_class` to the asset class each row selects.
2. `chooser_query("add_chooser_column", { asset_path, column_kind: "Enum", binding_property, enum_class })` — add each INPUT column first, one call per predicate (`Bool` / `Enum` / `GameplayTag` / `FloatRange`); `binding_property` is the dotted input-binding path, `enum_class` only for `Enum` columns.
3. `chooser_query("add_chooser_row", { asset_path, cells, output_psd })` — append each row AFTER all input columns exist; `cells` is one entry per input column in order, `output_psd` is the asset that row selects.
4. (optional) `chooser_query("set_chooser_cell", { asset_path, column_index, row_index, ... })` — overwrite a single typed predicate at `(column_index,row_index)` instead of re-adding the row; pass the field matching the column kind (`bool_value` / `enum_value`(+`comparison`) / `float_min`+`float_max` / `tags`).
5. `chooser_query("validate_chooser", { asset_path, expected_context_class, expected_result_type })` — compile via `Compile(true)` and confirm row/column consistency, expected context class, and no null/stale result refs (use `validate_chooser_table` for the lighter consistency-only pass).
6. Save the asset package (a chooser-namespace save action is **not** exposed today — confirm via `monolith_discover` and save through **unreal-asset**), then re-confirm selection wiring with `chooser_query("inspect_chooser", { asset_path, recursive: true })` to read the nested result tree the way a runtime context resolves it. There is no `chooser_query` evaluate-at-runtime action in this namespace; inspection is the read-only equivalent.

## Notes

- This reference is generated from the live `RegisterAction` surface. If an action is missing or renamed, re-run `monolith_discover({ namespace: "chooser" })` — the catalog is the source of truth.
- Pass `mode: "schema"` to `monolith_discover` for required/optional params and types before calling an action.
- `[w]` actions dirty the package and most recompile (`Compile(true)`); save the asset and validate (`validate_chooser`) after a batch of edits. Row/column add actions grow all parallel arrays atomically, so add columns before rows when scaffolding a fresh table.
