---
name: unreal-pcg
description: "Use for Procedural Content Generation (PCG) via Monolith MCP: create/edit PCG graphs, add and wire nodes, set settings, and run generation. Triggers on PCG, procedural content, PCG graph, PCG node, generate PCG, spline sampler, surface sampler, points, PCG component."
---

# unreal-pcg

**4 actions** via `pcg_query(action, params)`. Action names below are the live registry surface; call `monolith_discover` for exact parameter schemas.

## Discovery

```
monolith_discover({ namespace: "pcg" })                      # all actions in this namespace
monolith_discover({ namespace: "pcg", action: "<action>", mode: "schema" })  # exact params
```

## Action Reference

### Core (4)

| Action | Purpose |
|--------|---------|
| `get_graph_asset` | Inspect bounded AssetRegistry metadata for one PCG graph-like asset without loading PCG or mutating packages |
| `get_status` | Report optional PCG module/type availability without loading PCG or mutating the level |
| `list_components` | List PCG-like components in the current editor world using reflected class names |
| `list_graph_assets` | List PCG graph-like assets using AssetRegistry class paths without hard PCG dependencies |

## Notes

- This reference is generated from the live `RegisterAction` surface. If an action is missing or renamed, re-run `monolith_discover({ namespace: "pcg" })` — the catalog is the source of truth.
- Pass `mode: "schema"` to `monolith_discover` for required/optional params and types before calling an action.
