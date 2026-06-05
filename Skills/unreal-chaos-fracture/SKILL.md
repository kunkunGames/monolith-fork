---
name: unreal-chaos-fracture
description: "Use for Chaos destruction via Monolith MCP: Geometry Collections and fracturing (Voronoi/cluster), and destruction setup. Triggers on chaos, fracture, geometry collection, destruction, destructible, voronoi, cluster, shatter, rigid body."
---

# unreal-chaos-fracture

**3 actions** via `chaos_fracture_query(action, params)`. Action names below are the live registry surface; call `monolith_discover` for exact parameter schemas.

## Discovery

```
monolith_discover({ namespace: "chaos_fracture" })                      # all actions in this namespace
monolith_discover({ namespace: "chaos_fracture", action: "<action>", mode: "schema" })  # exact params
```

## Action Reference

### Core (3)

| Action | Purpose |
|--------|---------|
| `get_status` | Report optional Geometry Collection / Fracture module and reflected type availability without mutating assets |
| `list_geometry_collection_assets` | List Geometry Collection-like assets using AssetRegistry class paths without loading Fracture modules |
| `list_geometry_collection_components` | List Geometry Collection-like components in the current editor world using reflected class names |

## Notes

- This reference is generated from the live `RegisterAction` surface. If an action is missing or renamed, re-run `monolith_discover({ namespace: "chaos_fracture" })` — the catalog is the source of truth.
- Pass `mode: "schema"` to `monolith_discover` for required/optional params and types before calling an action.
