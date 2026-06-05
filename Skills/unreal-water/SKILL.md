---
name: unreal-water
description: "Use for the Water system via Monolith MCP: water bodies (ocean/lake/river) and water zones. Triggers on water, ocean, lake, river, water body, water zone, buoyancy, waves, gerstner."
---

# unreal-water

**2 actions** via `water_query(action, params)`. Action names below are the live registry surface; call `monolith_discover` for exact parameter schemas.

## Discovery

```
monolith_discover({ namespace: "water" })                      # all actions in this namespace
monolith_discover({ namespace: "water", action: "<action>", mode: "schema" })  # exact params
```

## Action Reference

### Core (2)

| Action | Purpose |
|--------|---------|
| `get_status` | Report Water/Landscape module availability and reflected Water-like actor counts. Read-only; no Water or Landscape hard dependency. |
| `list_bodies` | List Water-like actors/components in the current editor world using reflected class names only. Does not mutate actors, splines, landscapes, or zones. |

## Notes

- This reference is generated from the live `RegisterAction` surface. If an action is missing or renamed, re-run `monolith_discover({ namespace: "water" })` — the catalog is the source of truth.
- Pass `mode: "schema"` to `monolith_discover` for required/optional params and types before calling an action.
