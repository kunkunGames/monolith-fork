---
name: unreal-input
description: "Use for Enhanced Input setup via Monolith MCP: Input Actions, Input Mapping Contexts, key mappings, modifiers, and triggers. Triggers on input, enhanced input, input action, IA, mapping context, IMC, key binding, key mapping, modifier, input trigger, axis, action mapping."
---

# unreal-input

**10 actions** via `input_query(action, params)`. Action names below are the live registry surface; call `monolith_discover` for exact parameter schemas.

## Discovery

```
monolith_discover({ namespace: "input" })                      # all actions in this namespace
monolith_discover({ namespace: "input", action: "<action>", mode: "schema" })  # exact params
```

## Action Reference

### GAS Input Asset (10)

| Action | Purpose |
|--------|---------|
| `add_input_mapping` | Add a key mapping to an Input Mapping Context |
| `create_input_action` | Create or update a UInputAction asset |
| `create_input_mapping_context` | Create or update a UInputMappingContext asset |
| `get_input_action` | Inspect an Enhanced Input UInputAction asset |
| `get_input_mapping_context` | Inspect an Enhanced Input UInputMappingContext asset |
| `list_input_actions` | List Enhanced Input UInputAction assets |
| `list_input_mapping_contexts` | List Enhanced Input UInputMappingContext assets |
| `remove_input_mapping` | Remove a key mapping from an Input Mapping Context |
| `set_input_action_properties` | Update common UInputAction properties |
| `validate_input_mappings` | Validate Enhanced Input Mapping Contexts for missing actions and duplicate key conflicts |

## Notes

- This reference is generated from the live `RegisterAction` surface. If an action is missing or renamed, re-run `monolith_discover({ namespace: "input" })` — the catalog is the source of truth.
- Pass `mode: "schema"` to `monolith_discover` for required/optional params and types before calling an action.
