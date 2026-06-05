---
name: unreal-cloth
description: "Use for cloth simulation (Chaos Cloth) via Monolith MCP: cloth assets, config, and painting. Triggers on cloth, chaos cloth, cloth sim, clothing, cloth paint, cloth config, cloth LOD."
---

# unreal-cloth

**2 actions** via `cloth_query(action, params)`. Action names below are the live registry surface; call `monolith_discover` for exact parameter schemas.

## Discovery

```
monolith_discover({ namespace: "cloth" })                      # all actions in this namespace
monolith_discover({ namespace: "cloth", action: "<action>", mode: "schema" })  # exact params
```

## Action Reference

### Pose Search (2)

| Action | Purpose |
|--------|---------|
| `get_status` | Report read-only Chaos Cloth/Outfit workflow support without hard Chaos Outfit dependencies. |
| `list_clothing_assets` | List cloth/clothing/outfit-like assets under /Game using AssetRegistry metadata only. Does not load vertex data, weight maps, or mutate assets. |

## Notes

- This reference is generated from the live `RegisterAction` surface. If an action is missing or renamed, re-run `monolith_discover({ namespace: "cloth" })` — the catalog is the source of truth.
- Pass `mode: "schema"` to `monolith_discover` for required/optional params and types before calling an action.
