---
name: unreal-dataflow
description: Use when inspecting Unreal Dataflow node graphs through Monolith MCP — discover UDataflow assets, read bounded graph snapshots, inspect exact registered node schemas, validate graph integrity, and inspect variables or editor comments. The namespace is strictly read-only; it does not create, wire, evaluate, regenerate, or save Dataflow graphs.
---

# unreal-dataflow

Use the `dataflow` namespace for bounded, read-only inspection of Unreal
Dataflow graphs. It exposes exactly eight actions. It never creates nodes,
changes pins or variables, evaluates or regenerates a graph, marks a package
dirty, or saves an asset.

For Geometry Collection fracture operations use `unreal-chaos-fracture`. For a
PCG graph use `unreal-pcg`. For GeometryScript mesh editing use `unreal-mesh`.

## Discovery

Always confirm the live action and schema before calling an action:

```text
monolith_discover({ namespace: "dataflow", mode: "actions" })
monolith_discover({ namespace: "dataflow", action: "get_dataflow_graph", mode: "schema" })
```

## Action reference

`asset_path` is always an exact, case-sensitive object path below `/Game/`,
including the object name. For example:
`/Game/Dataflow/DF_Rock.DF_Rock`.

| Action | Parameters | Purpose |
| --- | --- | --- |
| `get_status` | none | Report the Dataflow plugin, linked engine modules, exact eight-action roster, and read-only capability flags. |
| `list_assets` | `package_path?="/Game"`, `limit?=100` | Enumerate a bounded AssetRegistry slice of `UDataflow` assets without loading them. |
| `get_dataflow_graph` | `asset_path`; `node_limit?=128`, `connection_limit?=1000`, `pin_limit?=128`, `property_limit?=128`, `include_properties?=false` | Read independent bounded node and connection slices, per-node pin/property summaries, and package-dirty postconditions. |
| `list_dataflow_node_types` | `filter?`, `common_only?=true`, `limit?=200`, `include_pins?=false`, `pin_limit?=64` | List case-exact registered factory types in deterministic category/type order, optionally with default-node pin schemas. |
| `get_dataflow_node_schema` | `type_name`; `include_properties?=true`, `pin_limit?=256`, `property_limit?=256` | Read one case-exact registered factory type and its bounded default pin/property schemas. |
| `validate_dataflow_graph` | `asset_path`; `node_scan_limit?=10000`, `connection_scan_limit?=50000`, `issue_limit?=500` | Validate bounded node and connection slices for null entries, invalid or duplicate names/GUIDs, missing endpoints/pins, and connection type mismatches. |
| `list_dataflow_variables` | `asset_path`; `limit?=200` | Read bounded property-bag descriptors and scalar values with explicit omission status for container, struct, and fixed-array values. |
| `list_dataflow_comments` | `asset_path`; `comment_limit?=200`, `node_limit?=128`, `graph_node_scan_limit?=5000` | Read bounded editor comment boxes and bounded contained-node membership hints. |

## Common workflows

### Inspect and validate one graph

```text
dataflow_query("get_status", {})

dataflow_query("list_assets", {
  "package_path": "/Game/Dataflow",
  "limit": 100
})

dataflow_query("get_dataflow_graph", {
  "asset_path": "/Game/Dataflow/DF_Rock.DF_Rock",
  "node_limit": 128,
  "connection_limit": 1000,
  "pin_limit": 128,
  "property_limit": 128,
  "include_properties": true
})

dataflow_query("validate_dataflow_graph", {
  "asset_path": "/Game/Dataflow/DF_Rock.DF_Rock",
  "node_scan_limit": 10000,
  "connection_scan_limit": 50000,
  "issue_limit": 500
})

dataflow_query("list_dataflow_variables", {
  "asset_path": "/Game/Dataflow/DF_Rock.DF_Rock",
  "limit": 200
})

dataflow_query("list_dataflow_comments", {
  "asset_path": "/Game/Dataflow/DF_Rock.DF_Rock",
  "comment_limit": 200,
  "node_limit": 128,
  "graph_node_scan_limit": 5000
})
```

### Resolve a node type before reasoning about pins

```text
dataflow_query("list_dataflow_node_types", {
  "filter": "Fracture",
  "common_only": true,
  "limit": 200,
  "include_pins": true,
  "pin_limit": 64
})

dataflow_query("get_dataflow_node_schema", {
  "type_name": "<exact type_name returned above>",
  "include_properties": true,
  "pin_limit": 256,
  "property_limit": 256
})
```

## Input contract

- JSON strings, booleans, and integers must use their declared JSON scalar
  types. String-encoded booleans or integers, fractional integers, non-finite
  numbers, out-of-range values, and unknown keys are rejected with `-32602`.
- `package_path` must be exactly `/Game` or a canonical directory below
  `/Game/`. `/GameX`, trailing slashes, filesystem paths, and dotted object
  paths are rejected.
- `asset_path` must be a canonical object path below `/Game/`. Shorthand
  package names, `.uasset` filenames, backslashes, subobjects, redirects, case
  substitutions, and alternate resolved objects are rejected.
- `type_name` is case-sensitive. A case-only mismatch returns the distinct
  `node_type_case_mismatch` error instead of silently selecting another type.
- `comment_limit * graph_node_scan_limit` may not exceed 1,000,000 membership
  comparisons. The action rejects an excessive budget; it does not clamp it.

## Completeness and read-only checks

- Every bounded result reports returned counts, limits, and explicit
  truncation or completeness fields. Never infer absence from a partial slice.
- `validate_dataflow_graph` reports `validity_status="incomplete"` and omits
  `valid` when either scan is incomplete. Only a complete scan can report
  `valid=true` or `valid=false`.
- Node and connection slices in `get_dataflow_graph` are independent. A
  connection can therefore report an endpoint outside the returned node slice
  without being treated as a broken graph edge.
- Pin rows report whether their source is `registered` or `declared`; the
  action does not silently substitute one representation for the other.
- Editable node/default properties and property-bag variables expose bounded
  scalar values only. Dynamic containers, structs, fixed arrays, and
  unsupported types report an explicit `value_read_status` and omit `value`;
  they never enter a generic unbounded export path.
- Asset-backed reads report `package_dirty_before`, `package_dirty_after`, and
  `package_dirty_state_preserved`. A clean package becoming dirty is a hard
  read-only postcondition failure.
- Free-form text is bounded, and each response reports
  `truncated_text_field_count`.

## Limits

| Parameter | Inclusive range |
| --- | --- |
| `list_assets.limit` | 1..500 |
| `get_dataflow_graph.node_limit` | 1..500 |
| `get_dataflow_graph.connection_limit` | 1..5,000 |
| Graph/schema `pin_limit` | 1..500 |
| Graph/schema `property_limit` | 1..500 |
| `list_dataflow_node_types.limit` | 1..1,000 |
| `validate_dataflow_graph.node_scan_limit` | 1..100,000 |
| `validate_dataflow_graph.connection_scan_limit` | 1..250,000 |
| `validate_dataflow_graph.issue_limit` | 1..1,000 |
| `list_dataflow_variables.limit` | 1..1,000 |
| `list_dataflow_comments.comment_limit` | 1..1,000 |
| `list_dataflow_comments.node_limit` | 1..500 |
| `list_dataflow_comments.graph_node_scan_limit` | 1..50,000 |

## Non-capabilities

The namespace intentionally does not expose graph creation, node creation,
wiring, input writes, variable writes, evaluation, regeneration, or save
operations. Do not substitute editor Python, commandlets, or another namespace
for those missing write contracts. If authoring becomes necessary, first add a
schema-first, transactional, validated Monolith action and verify it through
the live catalog.
