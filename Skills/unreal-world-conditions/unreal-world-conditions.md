---
name: unreal-world-conditions
description: "Use for the World Conditions / World State system via Monolith MCP: define and query world conditions and world state. Triggers on world condition, world state, condition, state query, world conditions, condition schema."
---

# unreal-world-conditions

**4 actions** via `world_conditions_query(action, params)`. Action names below are the live registry surface; call `monolith_discover` for exact parameter schemas.

## Discovery

```
monolith_discover({ namespace: "world_conditions" })                      # all actions in this namespace
monolith_discover({ namespace: "world_conditions", action: "<action>", mode: "schema" })  # exact params
```

## Action Reference

### Core (4)

| Action | Purpose |
|--------|---------|
| `describe_condition_types` | List loaded FWorldConditionBase-derived struct types and reflected property metadata. |
| `describe_query` | Describe a SmartObjectDefinition WorldCondition query without mutating the asset. |
| `get_status` | Report WorldConditions inspection feature state and optional dependency availability. |
| `list_query_owners` | List SmartObjectDefinition assets that can own WorldCondition query definitions. |

## Notes

- This reference is generated from the live `RegisterAction` surface. If an action is missing or renamed, re-run `monolith_discover({ namespace: "world_conditions" })` — the catalog is the source of truth.
- Pass `mode: "schema"` to `monolith_discover` for required/optional params and types before calling an action.
