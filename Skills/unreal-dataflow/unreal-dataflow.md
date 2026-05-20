---
name: unreal-dataflow
description: "Use for Dataflow graphs (geometry/Chaos node graphs) via Monolith MCP: create graphs, add and connect nodes, set inputs, and evaluate. Triggers on dataflow, dataflow graph, geometry node graph, dataflow node, evaluate dataflow, dataflow asset."
---

# unreal-dataflow

**8 actions** via `dataflow_query(action, params)`. Action names below are the live registry surface; call `monolith_discover` for exact parameter schemas.

## Discovery

```
monolith_discover({ namespace: "dataflow" })                      # all actions in this namespace
monolith_discover({ namespace: "dataflow", action: "<action>", mode: "schema" })  # exact params
```

## Action Reference

### Core (8)

| Action | Purpose |
|--------|---------|
| `get_dataflow_graph` | Read a bounded Dataflow graph summary from a UDataflow asset. Does not mutate, evaluate, regenerate, or mark packages dirty. |
| `get_dataflow_node_schema` | Return schema details for one registered Dataflow node type. |
| `get_status` | Report read-only Dataflow/Chaos graph discovery support without adding hard Dataflow link dependencies. |
| `list_assets` | List Dataflow asset metadata under /Game using AssetRegistry only. Does not load, evaluate, regenerate, or mutate assets. |
| `list_dataflow_comments` | List Dataflow editor comment boxes with bounded node membership hints without mutation. |
| `list_dataflow_node_types` | List registered Dataflow node factory types with optional filtering and pin summaries. |
| `list_dataflow_variables` | List UDataflow property bag variables with descriptor metadata and serialized values without mutation. |
| `validate_dataflow_graph` | Validate a Dataflow graph for duplicate node identifiers and broken connection references without mutation. |

## Notes

- This reference is generated from the live `RegisterAction` surface. If an action is missing or renamed, re-run `monolith_discover({ namespace: "dataflow" })` — the catalog is the source of truth.
- Pass `mode: "schema"` to `monolith_discover` for required/optional params and types before calling an action.
