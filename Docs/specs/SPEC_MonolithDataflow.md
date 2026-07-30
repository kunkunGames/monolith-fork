# Monolith — MonolithDataflow Module

| Field | Value |
| --- | --- |
| Parent | [SPEC_CORE.md](../SPEC_CORE.md) |
| Module | `MonolithDataflow` |
| Namespace | `dataflow` |
| Type | Editor |
| Engine floor | Unreal Engine 5.7 |
| Version | 0.21.3 (Beta) |
| Status | Current |

---

## 1. Purpose

`MonolithDataflow` provides eight bounded, read-only actions for discovering
`UDataflow` assets, inspecting graph/node schemas, validating graph integrity,
and reading variables or editor comments. It exposes useful Dataflow structure
without authoring, evaluating, regenerating, dirtying, or saving a graph.

---

## 2. Module boundary

| Area | Contract |
| --- | --- |
| Public dependencies | `Core`, `CoreUObject`, `Engine` |
| Private dependencies | `MonolithCore`, `UnrealEd`, `AssetRegistry`, `Json`, `Projects`, `DataflowCore`, `DataflowEngine` |
| Engine contract | `DataflowCore` and `DataflowEngine` are explicit engine-source dependencies in both UE 5.7 and UE 5.8, independent of the editor plugin's install location. The `Dataflow` editor plugin is an optional enabled plugin reference for factory/editor-node coverage; actions do not attempt optional module loading or substitute another graph system. |
| Registration | `FMonolithDataflowModule::StartupModule` registers exactly eight `dataflow` actions; shutdown unregisters the namespace. |
| Mutability | Every action is read-only. The module contains no graph authoring, evaluation, regeneration, transaction, package-save, or dirty-marking path. |
| Portability | The implementation contains no Speed-, Lyra-, or project-specific class dependency. |

---

## 3. Actions

| Action | Params | Behavior |
| --- | --- | --- |
| `dataflow.get_status` | none | Reports the Dataflow plugin descriptor, `DataflowCore`/`DataflowEngine` module state, exact action roster, and explicit read-only capability flags. |
| `dataflow.list_assets` | optional `package_path=/Game`, `limit=100` | Enumerates a bounded AssetRegistry slice of exact `UDataflow` rows without loading assets. |
| `dataflow.get_dataflow_graph` | `asset_path`; optional `node_limit=128`, `connection_limit=1000`, `pin_limit=128`, `property_limit=128`, `include_properties=false` | Reads independent bounded node and connection slices, bounded per-node pins/properties, and package-dirty postconditions. |
| `dataflow.list_dataflow_node_types` | optional `filter`, `common_only=true`, `limit=200`, `include_pins=false`, `pin_limit=64` | Lists registered factory types in deterministic case-exact category/type order, optionally with bounded default-node pin schemas. |
| `dataflow.get_dataflow_node_schema` | `type_name`; optional `include_properties=true`, `pin_limit=256`, `property_limit=256` | Resolves one case-exact registered factory type and returns bounded default pin/property schemas. |
| `dataflow.validate_dataflow_graph` | `asset_path`; optional `node_scan_limit=10000`, `connection_scan_limit=50000`, `issue_limit=500` | Checks bounded node/connection slices for null entries, invalid or duplicate names/GUIDs, missing endpoints/pins, and case-exact pin type mismatches. |
| `dataflow.list_dataflow_variables` | `asset_path`; optional `limit=200` | Reads bounded property-bag descriptors and bounded scalar values; container, struct, and fixed-array values are explicitly omitted. |
| `dataflow.list_dataflow_comments` | `asset_path`; optional `comment_limit=200`, `node_limit=128`, `graph_node_scan_limit=5000` | Reads bounded editor comment boxes and bounded geometric membership hints under an explicit comparison-work budget. |

---

## 4. Input and identity contracts

| Contract | Requirement |
| --- | --- |
| Unknown keys | Every action rejects undeclared keys with JSON-RPC `-32602`; there is no soft-warning path. |
| JSON scalar types | Strings, booleans, and integers must arrive as their declared JSON type. String coercion is not accepted. Integers must be finite whole numbers. |
| Integer ranges | Values outside the inclusive action limit are rejected and never clamped. `FParamSchemaBuilder::Range` publishes the same bounds through discovery. |
| Package directory | `package_path` is exactly `/Game` or a canonical long package directory below `/Game/`; `/GameX`, dots, backslashes, colons, and trailing slashes are rejected. |
| Asset identity | `asset_path` is an exact, case-sensitive `/Game/.../Asset.Asset` object path. Shorthand names, extensions, backslashes, subobjects, redirects, and resolved-path substitutions are rejected. |
| Object type | The exact object must be `UDataflow`; a missing object and a wrong asset type return distinct structured errors. |
| Node type identity | `type_name` must match a registered factory type exactly. Case-only mismatch returns `node_type_case_mismatch`; unknown identity returns `unknown_node_type`. |
| Comment work | `comment_limit * graph_node_scan_limit` must not exceed 1,000,000. Excessive work is rejected instead of silently reduced. |

### 4.1 Inclusive numeric limits

| Parameter | Minimum | Maximum |
| --- | ---: | ---: |
| `list_assets.limit` | 1 | 500 |
| `get_dataflow_graph.node_limit` | 1 | 500 |
| `get_dataflow_graph.connection_limit` | 1 | 5,000 |
| Graph/schema `pin_limit` | 1 | 500 |
| Graph/schema `property_limit` | 1 | 500 |
| `list_dataflow_node_types.limit` | 1 | 1,000 |
| `validate_dataflow_graph.node_scan_limit` | 1 | 100,000 |
| `validate_dataflow_graph.connection_scan_limit` | 1 | 250,000 |
| `validate_dataflow_graph.issue_limit` | 1 | 1,000 |
| `list_dataflow_variables.limit` | 1 | 1,000 |
| `list_dataflow_comments.comment_limit` | 1 | 1,000 |
| `list_dataflow_comments.node_limit` | 1 | 500 |
| `list_dataflow_comments.graph_node_scan_limit` | 1 | 50,000 |

---

## 5. Bounded output and completeness

| Surface | Completeness contract |
| --- | --- |
| Asset discovery | Reports `observed_match_count`, `returned_count`, `truncated`, `count_complete`, and `asset_registry_scan_in_progress`; `total_count` appears only for a complete enumeration. A registry still performing its initial scan reports `count_complete=false` because enumeration can only observe the assets discovered so far. |
| Graph snapshot | Reports exact total/returned counts and separate truncation flags for nodes and connections. Connection slicing is independent of the returned node slice. |
| Pins | Reports `source=registered` or `source=declared` and availability flags; it never silently substitutes an unavailable representation. |
| Node types | Sorts by case-sensitive category then type, reports registered/valid/matched/returned counts, and exposes truncation. |
| Validation | Reports separate node/connection scan completeness. An incomplete scan returns `validity_status=incomplete` and omits `valid`; only a complete scan can report `valid=true` or `valid=false`. |
| Node/default properties | Reports bounded scalar values with `value_read_status`, `value_available`, and `value_truncated`; dynamic containers, structs, fixed arrays, and unsupported types are explicitly omitted instead of entering an unbounded generic export path. |
| Variables | Reports property-bag availability, total/returned counts, and the same bounded scalar value/read/omission/truncation contract. |
| Comments | Reports graph-scan, comment-count, returned-comment, per-comment membership, and comparison-budget completeness independently. Membership candidates include comment nodes, so a comment box nested inside another is reported as a contained node; `considered_non_comment_node_count` counts non-comment nodes and `considered_membership_candidate_count` counts every candidate compared. |
| Aggregate output | Every dynamic response shares a hard 4,096-row budget across top-level and nested arrays. Reflected/free-form strings passed through the bounded reader additionally share 1,048,576 characters. `output_returned_row_count`, `output_rows_truncated`, `output_returned_bounded_text_character_count`, `output_bounded_text_truncated`, and `output_budget_exhausted` make aggregate truncation explicit; fixed-size GUID and contract metadata remain bounded by the row ceiling. |
| Text | Each free-form field is capped at 4,096 characters; the aggregate character budget applies after that per-field cap and each result reports `truncated_text_field_count`. Truncation never splits a UTF-16 surrogate pair, so bounded values stay serializable as UTF-8. |

---

## 6. Read-only postcondition

Asset-backed reads capture whether the package was already loaded and dirty.
They return:

- `package_loaded_before`;
- `package_dirty_before`;
- `package_dirty_after`;
- `package_dirty_state_preserved`.

Every object returned by `StaticLoadObject`, including a case-mismatched,
redirector, or wrong-type object that will be rejected, has its package
captured before the identity/type decision returns. Any dirty-state transition
during load or inspection fails with `read_only_load_dirtied_package`,
`read_only_load_changed_package_dirty_state`, or
`read_only_postcondition_failed`. Pre-existing dirty state is reported, never
masked or reset.

---

## 7. Non-capabilities

The namespace does not expose graph creation, node creation/removal, pin
connection/disconnection, input or variable writes, evaluation, regeneration,
transactions, dirty marking, or package saves. `get_status.capabilities`
reports `authoring=false`, `evaluation=false`, and `regeneration=false`.

---

## 8. Verification gates

| Gate | Required result |
| --- | --- |
| Registry | Exactly eight `dataflow` actions with required/default schemas and discoverable numeric bounds. |
| Param guards | Wrong scalar types, fractional/out-of-range integers, unknown keys, `/GameX`, shorthand/file asset paths, excessive comment work, and direct aggregate row/text budget overflow checks fail closed. |
| Identity | Case-exact registered node schema succeeds; a case-only type substitution returns `node_type_case_mismatch`. |
| Read-only behavior | AssetRegistry discovery does not load assets; successful and rejected graph loads preserve package dirty state; long strings are capped at 4,096 characters; aggregate output stops at 4,096 rows/1,048,576 characters with explicit metadata; container values are explicitly omitted without generic serialization. |
| Validation | Complete empty-graph validation reports `validity_status=valid`; incomplete validation cannot emit `valid`. |
| UE 5.7 build/test | `UnrealEditor-MonolithDataflow.dll` links and `Monolith.Dataflow` passes 3/3. |
| UE 5.8 build/test | `UnrealEditor-MonolithDataflow.dll` links and `Monolith.Dataflow` passes 3/3. |
| Catalog | Generated catalog adds exactly eight actions under one new namespace and removes none. |
| Mutation audit | The module has no authoring, transaction, dirty-mark, save, evaluate, or regenerate call path. |
| Exclusions | No security, benchmark, invocation-log, metadata/RL, execution-policy, or search-planning capability is added. |

Verification evidence is recorded in
[2026-07-30-dataflow-read-validation-actions.md](../testing/2026-07-30-dataflow-read-validation-actions.md).
