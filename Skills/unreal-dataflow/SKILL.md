---
name: unreal-dataflow
description: Use to INSPECT Dataflow node graphs (geometry/Chaos node graphs) via Monolith MCP — list, read, and validate Dataflow graphs, nodes, node types, variables, and comments, plus report module status. This namespace is read-only inspection today; authoring (create graph, connect/wire nodes, set input, evaluate) is not exposed here — confirm via monolith_discover, and if it lands route by name. For fracturing/Geometry Collections use unreal-chaos-fracture; if the procedural node graph is PCG use unreal-pcg; for GeometryScript mesh editing use unreal-mesh. This skill owns Dataflow graph inspection. Triggers on dataflow, dataflow graph, geometry node graph, dataflow node, inspect dataflow, validate dataflow, list dataflow nodes, dataflow asset, dataflow variables, dataflow comments, node graph geometry, chaos dataflow, procedural geometry graph.
---

# unreal-dataflow

Drives the Monolith **dataflow** namespace for **inspecting** Dataflow node graphs (geometry/Chaos node graphs). **8 actions** via `dataflow_query(action, params)`, all read-only (list / read / validate / status). Authoring a Dataflow graph (create graph, connect or wire nodes, set input pins, evaluate/regenerate) is **not exposed by this namespace today** — verify the live catalog with `monolith_discover` before assuming a write action exists, and if one lands route to it by name. Action names below are the live registry surface; call `monolith_discover` for exact parameter schemas.

## Discovery

```
monolith_discover({ namespace: "dataflow" })                      # all actions in this namespace
monolith_discover({ namespace: "dataflow", action: "<action>", mode: "schema" })  # exact params
```

## When to use / Use a different skill for

- **Use this skill** to inspect Dataflow node graphs — listing, reading, and validating Dataflow graphs, node types, variables, and comments, plus reporting module status. Inspection / read-only only.
- **Authoring is not exposed here.** Creating a graph, connecting/wiring nodes, setting input pins, or evaluating/regenerating a Dataflow graph is not in this namespace today — confirm with `monolith_discover({ namespace: "dataflow" })` before assuming a write action exists.
- **unreal-chaos-fracture** when the goal is fracturing or Geometry Collections, with Dataflow just the graph host.
- **unreal-pcg** when the procedural node graph is PCG, not Dataflow.
- **unreal-mesh** when you need GeometryScript mesh editing rather than a Dataflow graph.

## Action Reference

Param notation: `name*` required, `name?` optional, `name=val` default, `a/b/c` allowed values, `[w]` mutates. Signatures are a snapshot of the live catalog — for the exact, full, current schema of any action call `monolith_discover` with `mode: "schema"`. All 8 dataflow actions below are read-only (none mutate state).

### Core (8)

| Action | Params (req* opt? =default) | Purpose |
|--------|------------------------------|---------|
| `get_dataflow_graph` | `asset_path*`, `node_limit=128`, `connection_limit=1000`, `include_properties=false` | Read a bounded Dataflow graph summary from a UDataflow asset. Does not mutate, evaluate, regenerate, or mark packages dirty. |
| `get_dataflow_node_schema` | `type_name*`, `include_properties=true` | Return schema details for one registered Dataflow node type. |
| `get_status` | _(none)_ | Report read-only Dataflow/Chaos graph discovery support without adding hard Dataflow link dependencies. |
| `list_assets` | `package_path=/Game`, `limit=100` | List Dataflow asset metadata under /Game using AssetRegistry only. Does not load, evaluate, regenerate, or mutate assets. |
| `list_dataflow_comments` | `asset_path*`, `node_limit=128` | List Dataflow editor comment boxes with bounded node membership hints without mutation. |
| `list_dataflow_node_types` | `filter?`, `common_only=true`, `limit=200`, `include_pins=false` | List registered Dataflow node factory types with optional filtering and pin summaries. |
| `list_dataflow_variables` | `asset_path*` | List UDataflow property bag variables with descriptor metadata and serialized values without mutation. |
| `validate_dataflow_graph` | `asset_path*` | Validate a Dataflow graph for duplicate node identifiers and broken connection references without mutation. |

## Common Workflows

### 1. Inspect and validate a Dataflow graph end-to-end (read-only)

```
# 1. Confirm Dataflow/Chaos graph discovery support is present (no hard link dep)
dataflow_query("get_status", {})

# 2. Find Dataflow assets under a content path
dataflow_query("list_assets", { "package_path": "/Game", "limit": 100 })

# 3. Read the bounded graph summary for one asset (raise limits / pull properties as needed)
dataflow_query("get_dataflow_graph", { "asset_path": "/Game/Dataflow/DF_Rock", "node_limit": 128, "connection_limit": 1000, "include_properties": true })

# 4. Validate that graph for duplicate node ids and broken connection refs
dataflow_query("validate_dataflow_graph", { "asset_path": "/Game/Dataflow/DF_Rock" })

# 5. List the property-bag variables and the editor comment boxes for context
dataflow_query("list_dataflow_variables", { "asset_path": "/Game/Dataflow/DF_Rock" })
dataflow_query("list_dataflow_comments", { "asset_path": "/Game/Dataflow/DF_Rock", "node_limit": 128 })
```

### 2. Resolve a node type before reasoning about a graph

```
# 1. List registered Dataflow node factory types (filter + pin summaries)
dataflow_query("list_dataflow_node_types", { "filter": "Fracture", "common_only": true, "limit": 200, "include_pins": true })

# 2. Read the full schema for one registered node type
dataflow_query("get_dataflow_node_schema", { "type_name": "FDataflowVoronoiFractureNode", "include_properties": true })
```

## Gotchas

- **List/read truncation is silent at the bound.** The list and read actions cap results at their default ceilings: `get_dataflow_graph` stops at `node_limit=128` nodes and `connection_limit=1000` connections, `list_dataflow_comments` at `node_limit=128`, `list_dataflow_node_types` at `limit=200`, and `list_assets` at `limit=100`. A large graph or content tree can be truncated to those bounds, so a short result is not proof of a small graph — raise the relevant `*_limit` and re-query before concluding a node, connection, comment, type, or asset is absent.
- **`get_dataflow_node_schema` wants the registered factory type name, not a display label.** Pass `type_name` exactly as it appears in `list_dataflow_node_types` (the registered Dataflow node factory type, e.g. `FDataflowVoronoiFractureNode` from the workflow above) — resolve the type with `list_dataflow_node_types` first rather than guessing a friendly node name.
- **Module-not-loaded shows up as a `get_status` capability, not a thrown error.** `get_status` reports Dataflow/Chaos graph discovery support without taking a hard Dataflow link dependency, so when the Dataflow module is unavailable the inspection surface degrades gracefully instead of crashing. Call `get_status` first and read its support flags before trusting `get_dataflow_graph` / `list_*` output; empty results may mean the module is not present rather than that no graphs exist.

## Notes

- This reference is generated from the live `RegisterAction` surface. If an action is missing or renamed, re-run `monolith_discover({ namespace: "dataflow" })` — the catalog is the source of truth.
- Pass `mode: "schema"` to `monolith_discover` for required/optional params and types before calling an action.
