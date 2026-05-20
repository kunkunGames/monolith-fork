---
name: unreal-metahuman
description: Use for MetaHuman setup and layout via Monolith MCP. Triggers on metahuman, MHC, digital human, face, groom, metahuman layout.
---

# unreal-metahuman

**2 actions** via `metahuman_query(action, params)`. Action names below are the live registry surface; call `monolith_discover` for exact parameter schemas.

## Discovery

```
monolith_discover({ namespace: "metahuman" })                      # all actions in this namespace
monolith_discover({ namespace: "metahuman", action: "<action>", mode: "schema" })  # exact params
```

## Action Reference

### Layout (2)

| Action | Purpose |
|--------|---------|
| `get_status` | Report read-only MetaHuman capability status without hard MetaHuman plugin dependencies or service calls. |
| `list_character_assets` | List MetaHuman-like assets under /Game using AssetRegistry metadata only. Does not load characters, build, rig, conform, or call services. |

## Notes

- This reference is generated from the live `RegisterAction` surface. If an action is missing or renamed, re-run `monolith_discover({ namespace: "metahuman" })` — the catalog is the source of truth.
- Pass `mode: "schema"` to `monolith_discover` for required/optional params and types before calling an action.
