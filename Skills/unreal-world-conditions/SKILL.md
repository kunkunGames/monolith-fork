---
name: unreal-world-conditions
description: Use when inspecting the Unreal World Conditions / World State system via Monolith MCP (world_conditions namespace) — describe loaded FWorldConditionBase condition struct types and their reflected property metadata, describe a SmartObjectDefinition WorldCondition query without mutating the asset, list query-owning SmartObjectDefinition assets, and report inspection-feature status. For a condition that gates a GAS ability/effect via gameplay tags use unreal-gas (author the tag/ability there, query world state here); for Behavior Tree / StateTree decorators and EQS that drive AI decisions use unreal-ai; to wire a world-condition check into an actor Blueprint graph use unreal-blueprints. Triggers on world condition, world conditions, world state, WorldCondition, FWorldConditionBase, condition schema, condition type, state query, condition query, describe condition, SmartObjectDefinition condition, smart object condition, query owner, world condition status.
---

# unreal-world-conditions

Inspects the Unreal **World Conditions / World State** system by driving the Monolith **`world_conditions`** namespace. **4 actions** via `world_conditions_query(action, params)`. These actions are read-only inspection — they describe condition types and SmartObjectDefinition queries without mutating assets.

## Discovery

The table below is a snapshot of the live registry. The catalog is the source of truth — always confirm the action exists and read its exact params before calling:

```
monolith_discover({ namespace: "world_conditions" })                                       # all actions in this namespace
monolith_discover({ namespace: "world_conditions", action: "<action>", mode: "schema" })   # exact params for one action
describe_query("action_schema", { namespace: "world_conditions", action: "describe_query" })  # equivalent schema form
monolith_find("describe a world condition query")                                          # jump straight to the right action
```

If an action below is missing or renamed, re-run `monolith_discover({ namespace: "world_conditions" })`.

## When to use / Use a different skill for

Use **unreal-world-conditions** to inspect the project-level World Conditions/World State surface — what condition struct types are loaded, their reflected properties, and which SmartObjectDefinition assets own WorldCondition queries.

- **unreal-gas** — the condition gates a Gameplay Ability or Gameplay Effect via gameplay tags. Author the ability/tag in GAS and query world state here.
- **unreal-ai** — the gate is a Behavior Tree / StateTree decorator or an EQS query that drives an AI decision, rather than a project-level World Conditions/world-state query.
- **unreal-blueprints** — you are wiring a world-condition check into an actor Blueprint graph, as opposed to defining or querying the world condition schema itself.

## Action Reference

Param notation: `name*` required, `name?` optional, `name=val` default, `a/b/c` allowed, `[w]` mutates. Signatures are a snapshot of the live catalog — for the exact full schema call `monolith_discover` with `mode: "schema"`. Keep the discover-first block above as the authority.

| Action | Params | Purpose |
|--------|--------|---------|
| `get_status` | _(none)_ | Report WorldConditions inspection feature state and optional dependency availability. |
| `describe_condition_types` | `limit?=128` | List loaded FWorldConditionBase-derived struct types and reflected property metadata. |
| `list_query_owners` | `path_filter?=/Game` `limit?=100` | List SmartObjectDefinition assets that can own WorldCondition query definitions. `path_filter` is the package path searched recursively. |
| `describe_query` | `asset_path*` `query?=preconditions` `slot_index?` | Describe a SmartObjectDefinition WorldCondition query without mutating the asset. `query` allows `preconditions`/`slot_selection_preconditions`; `slot_index` is required when `query=slot_selection_preconditions`. |

## Common workflows

```text
# 1. Confirm the inspection feature is available before deeper calls
world_conditions_query("get_status", {})

# 2. Enumerate the loaded condition struct types and their reflected properties
world_conditions_query("describe_condition_types", {})

# 3. Find which SmartObjectDefinition assets own WorldCondition queries
world_conditions_query("list_query_owners", {})

# 4. Describe one owner's query without touching the asset
world_conditions_query("describe_query", { "asset_path": "/Game/SmartObjects/SOD_Bench", "query": "preconditions" })
```

## Gotchas / Rules

- All four actions are read-only inspection — they describe condition types and queries; they do not author or mutate WorldCondition assets. To change the gating logic, edit it where it lives (GAS ability/effect, AI BT/StateTree, or the actor Blueprint).
- `describe_query` reads a SmartObjectDefinition WorldCondition query; use `list_query_owners` first to discover which SmartObjectDefinition assets can own one.
- Run `get_status` when a call reports the feature or a dependency as unavailable, before assuming the namespace is broken.
- This reference is a snapshot of the live `RegisterAction` surface. If an action is missing or renamed, re-run `monolith_discover({ namespace: "world_conditions" })` — the catalog wins. Pass `mode: "schema"` for required/optional params and types before calling.
