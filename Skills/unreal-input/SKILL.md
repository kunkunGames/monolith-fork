---
name: unreal-input
description: Use when authoring Enhanced Input ASSETS via Monolith MCP (input namespace) - create/inspect Input Action (IA) and Input Mapping Context (IMC) assets, add/remove key mappings, set modifiers and triggers, list IA/IMC, and validate mappings for missing actions or duplicate keys. Owns the IA/IMC asset; for generic DefaultInput.ini section/value edits and project config use unreal-config; to bind an Input Action to a GAS ability activation use unreal-gas; to wire an Input Action event into an actor/pawn Blueprint graph use unreal-blueprints. Triggers on input, enhanced input, input action, IA, mapping context, IMC, key binding, key mapping, add input mapping, remove input mapping, modifier, input trigger, axis, action mapping, input value type, swizzle, dead zone, hold trigger, validate input mappings.
---

# unreal-input

Drives the **`input`** namespace via `input_query(action, params)`: author and inspect Enhanced Input Action (IA) and Input Mapping Context (IMC) assets, their key mappings, modifiers, and triggers. The 10 actions below are a snapshot — discover first so you never call a stale or guessed name.

## Discovery

```
monolith_discover({ namespace: "input" })                      # all actions in this namespace
monolith_discover({ namespace: "input", action: "<action>", mode: "schema" })  # exact params
```

## When to use

Use this skill for the Enhanced Input ASSET layer — creating/inspecting `UInputAction` and `UInputMappingContext` assets, adding or removing key mappings, configuring modifiers and triggers, and validating IMCs for missing actions or duplicate-key conflicts.

Use a different skill for:

- **unreal-config** — generic `DefaultInput.ini` section/value edits and other project config (`.ini` read/edit, cvars, project settings) rather than the IA/IMC asset itself.
- **unreal-gas** — binding an Input Action to a GAS ability activation; the ability input ID / input binding lives on the GAS side.
- **unreal-blueprints** — wiring an Input Action event into an actor/pawn Blueprint graph, versus defining the IA/IMC and its key mappings here.

## Action Reference

Param notation: `name*` required, `name?` optional, `name=val` default, `a/b/c` allowed, `[w]` mutates. Signatures are a snapshot of the live catalog — for the exact full schema call `monolith_discover` with `mode: "schema"`. The Discovery block above stays the authority.

### Enhanced Input Assets (10)

| Action | Params | Purpose |
|--------|--------|---------|
| `get_input_action` | `asset_path*` | Inspect an Enhanced Input UInputAction asset |
| `list_input_actions` | `path?` `include_details?=false` | List Enhanced Input UInputAction assets |
| `get_input_mapping_context` | `asset_path*` | Inspect an Enhanced Input UInputMappingContext asset |
| `list_input_mapping_contexts` | `path?` `include_details?=false` | List Enhanced Input UInputMappingContext assets |
| `validate_input_mappings` | `context_paths?` `path?` `fail_on_unbound?=false` | Validate missing actions and exact duplicate mappings; report legal shared keys and unbound rows separately |
| `[w] create_input_action` | `asset_path*` `value_type?=Boolean` (Boolean/Axis1D/Axis2D/Axis3D) `description?` `consume_input?=true` `trigger_when_paused?=false` `accumulation?` (TakeHighestAbsoluteValue/Cumulative) `overwrite?=false` `save?=true` | Create or update a UInputAction asset |
| `[w] set_input_action_properties` | `asset_path*` `value_type?` (Boolean/Axis1D/Axis2D/Axis3D) `description?` `consume_input?` `consume_legacy_mappings?` `trigger_when_paused?` `reserve_all_mappings?` `accumulation?` (TakeHighestAbsoluteValue/Cumulative) `save?=true` | Update common UInputAction properties |
| `[w] create_input_mapping_context` | `asset_path*` `description?` `registration_tracking_mode?=Untracked|CountRegistrations` `overwrite?=false` `save?=true` | Create or update a UInputMappingContext asset and its ownership policy |
| `[w] add_input_mapping` | `context_path*` `action_path*` `key*` `save?=true` | Add a key mapping to an Input Mapping Context |
| `[w] remove_input_mapping` | `context_path*` `action_path*` `key*` `save?=true` | Remove a key mapping from an Input Mapping Context |

## Common Workflows

### Create an IA + IMC and map a key

```
input_query({ action: "create_input_action", params: {
  asset_path: "/Game/Input/IA_Jump", value_type: "Boolean"
}})
input_query({ action: "create_input_mapping_context", params: {
  asset_path: "/Game/Input/IMC_Default"
}})
input_query({ action: "add_input_mapping", params: {
  context_path: "/Game/Input/IMC_Default",
  action_path: "/Game/Input/IA_Jump", key: "SpaceBar"
}})
```

### Inspect and validate before shipping a context

```
input_query({ action: "get_input_mapping_context", params: {
  asset_path: "/Game/Input/IMC_Default"
}})
input_query({ action: "validate_input_mappings", params: {
  context_paths: ["/Game/Input/IMC_Default"]
}})
```

## Notes

- This reference is generated from the live `RegisterAction` surface. If an action is missing or renamed, re-run `monolith_discover({ namespace: "input" })` — the catalog is the source of truth.
- Pass `mode: "schema"` to `monolith_discover` for required/optional params and types (value type, modifiers, triggers, key) before calling an action.
- `validate_input_mappings` flags missing actions and exact duplicate semantic rows within an IMC. Distinct actions may legally share a key and are reported under `shared_keys`, not as conflicts. `None`/invalid-key rows are reported under `unbound_rows`; pass `fail_on_unbound=true` only when the project contract requires every row to have a concrete key.
- Use `registration_tracking_mode="CountRegistrations"` when more than one runtime system may install the same IMC. Each installer must call `AddMappingContext` once for its own lifetime and match it with one `RemoveMappingContext`; merely observing that the context is already present does not acquire ownership.
