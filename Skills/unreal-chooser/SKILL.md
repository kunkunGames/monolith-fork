---
name: unreal-chooser
description: Use for the Unreal Engine Chooser Table selection mechanism via the Monolith chooser namespace: discover, inspect, validate, create, and edit ChooserTable assets, rows, columns, context bindings, and result references.
---

# unreal-chooser

Use this skill for the selection mechanism itself: ChooserTable assets, rows,
columns, context bindings, nested chooser references, and selected result
assets. Use `unreal-animation` when the selected result is an animation asset
that must be edited, and `unreal-blueprints` when the logic belongs in a
Blueprint or DataTable instead.

The namespace exposes **16 actions** through `chooser_query(action, params)`.
The live catalog and per-action schema are authoritative:

```text
monolith_discover({ namespace: "chooser" })
monolith_discover({ namespace: "chooser", action: "<action>", mode: "schema" })
```

## Path contract

The six discovery/readback/validation actions accept only canonical mounted
Unreal package or top-level object paths:

- accepted package form: `/Game/Choosers/CHT_Locomotion`
- accepted object form:
  `/Game/Choosers/CHT_Locomotion.CHT_Locomotion`
- rejected: filesystem paths, relative paths, subobjects, redirectors,
  case-only aliases, and object names that do not match the package asset name

`path_filter` is also exact: use a mounted long package prefix such as
`/Game/Choosers`. The read actions never compile, mutate, transact, save, or
dirty a package.

Reflected payloads have an independent serialization budget: depth 3, 128
fields per full struct (16 for compact column summaries), and 256 entries per
full container (8 compact). A container object always reports its full `count`
and adds `truncated_after` when the returned `items` or `entries` array is only
the bounded prefix. At the depth boundary, structs and containers return
`serialization=depth_limit` metadata without expanding their contents.
String, localized-text, and otherwise unsupported export values cap at 4,096
characters; a longer value becomes an object with the bounded `value`,
`serialization`, `original_char_count`, and `truncated_after`.

## Discovery and readback

| Action | Parameters | Result and contract |
|---|---|---|
| `list_chooser_tables` | `path_filter?`, `offset=0`, `limit=200` | Deterministic AssetRegistry listing. `limit` is 1–1000; returns `count`, `total`, and `has_more`. |
| `get_chooser_table` | `asset_path*`, `include_rows=false`, `row_limit=50` | Bounded summary of row/column/result/context counts, reflected columns, references, fallback result, and optional row/cell readback. `row_limit` is 1–500. |
| `list_chooser_columns` | `asset_path*` | Reflected column type, input/output classification, disabled state, row-value property/type/count, and bounded fields. |
| `list_chooser_rows` | `asset_path*`, `start_row=0`, `limit=100` | Bounded row page with result payload, disabled state, and per-column cell values. `limit` is 1–500. |
| `list_chooser_references` | `asset_path*`, `offset=0`, `limit=200` | Bounded hard/soft reference page with reflected source locations and package-level existence checks. Returns `scan_truncated`; a truncated scan is never treated as complete validation. |
| `validate_chooser_table` | `asset_path*` | Non-mutating structural validation: result/disabled/column row-array alignment, valid column structs, soft-reference package resolution, and bounded-scan completeness. Warnings do not make `valid=false`; errors do. |

The read layer is reflection-only and does not hard-link new Chooser internals.
Actions remain visible when the optional Chooser plugin is disabled.
Asset-dependent calls then return an explicit optional-dependency error;
`list_chooser_tables` can still report any registry metadata that is visible.

## Deep inspection and reference editing

These actions use the existing `WITH_CHOOSER`-gated implementation:

| Action | Parameters | Purpose |
|---|---|---|
| `inspect_chooser` | `asset_path*`, `include_cells=false`, `recursive=false` | Deep inspection of context data, result type/class, columns, referenced assets, compile status, and optionally the complete nested chooser tree. |
| `duplicate_chooser_tree` | `source_assets*`, `destination_folder*`, `remap_rules?` | Duplicate tables, then remap root/parent/nested chooser and result references in a two-pass operation. |
| `set_context_object_class` | `asset_path*`, `context_name_or_index*`, `class_path*` | Rewrite a class-typed context entry and recompile. |
| `set_result_asset_reference` | `asset_path*`, `row_or_column*`, `asset_path_value*` | Rewrite an asset or soft-asset result row and recompile. |
| `set_evaluate_chooser_result_reference` | `asset_path*`, `row*`, `child_chooser_path*` | Rewrite the child table referenced by an EvaluateChooser result and recompile. |
| `validate_chooser` | `asset_path*`, `expected_context_class?`, `expected_result_type?` | Compile and validate expected context/result type plus result references. This differs from the non-mutating `validate_chooser_table`. |

## Authoring

| Action | Parameters | Purpose |
|---|---|---|
| `create_chooser_table` | `asset_path*`, `output_type=Object`, `output_class?`, `context_class?` | Create an empty UChooserTable and configure its result/context contract. |
| `add_chooser_column` | `asset_path*`, `column_kind*`, `binding_property?`, `enum_class?` | Append Bool, Enum, GameplayTag, FloatRange, or OutputObject column; back-fill its row-value array to the current row count. |
| `add_chooser_row` | `asset_path*`, `cells*`, `output_psd*` | Append one result row and grow all parallel row arrays atomically. |
| `set_chooser_cell` | `asset_path*`, `column_index*`, `row_index*`, typed value fields | Replace one typed predicate cell while preserving table alignment. |

## Safe end-to-end workflow

1. Call `list_chooser_tables` or use a known exact object path.
2. Call `get_chooser_table`, `list_chooser_columns`, and
   `validate_chooser_table` before mutation.
3. For a new table, create it and add every input column before adding rows.
4. Add rows or make focused cell/reference edits.
5. Call `validate_chooser` when a compile pass is required.
6. Save through the `asset` namespace; Chooser has no separate save action.
7. Re-read with `get_chooser_table`, `list_chooser_rows`,
   `list_chooser_references`, and `validate_chooser_table`.

Do not infer success from an action's mutation response alone. The final
readback must show the intended row/column counts and references, and both
validation actions must be interpreted according to their contracts:
`validate_chooser` compiles, while `validate_chooser_table` is strictly
non-mutating.
