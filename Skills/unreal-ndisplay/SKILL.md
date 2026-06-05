---
name: unreal-ndisplay
description: "Use for nDisplay multi-display / LED-wall virtual production config via Monolith MCP: clusters, viewports, and config. Triggers on ndisplay, LED wall, virtual production, ICVFX, cluster, viewport, multi-display, nDisplay config."
---

# unreal-ndisplay

**2 actions** via `ndisplay_query(action, params)`. Action names below are the live registry surface; call `monolith_discover` for exact parameter schemas.

## Discovery

```
monolith_discover({ namespace: "ndisplay" })                      # all actions in this namespace
monolith_discover({ namespace: "ndisplay", action: "<action>", mode: "schema" })  # exact params
```

## Action Reference

### Core (2)

| Action | Purpose |
|--------|---------|
| `get_status` | Report read-only nDisplay/DisplayCluster config authoring support without hard DisplayCluster dependencies. |
| `list_config_assets` | List nDisplay/DisplayCluster config-like assets under /Game using AssetRegistry metadata only. Does not load, save, or mutate configs. |

## Notes

- This reference is generated from the live `RegisterAction` surface. If an action is missing or renamed, re-run `monolith_discover({ namespace: "ndisplay" })` — the catalog is the source of truth.
- Pass `mode: "schema"` to `monolith_discover` for required/optional params and types before calling an action.
