---
name: unreal-input
description: Use when authoring Enhanced Input ASSETS via Monolith MCP (input namespace) - create/inspect Input Action (IA) and Input Mapping Context (IMC) assets, add/remove key mappings, set or clone modifiers and triggers, list IA/IMC, and validate mappings for missing actions or duplicate keys. Owns the IA/IMC asset; for generic DefaultInput.ini section/value edits and project config use unreal-config; to bind an Input Action to a GAS ability activation use unreal-gas; to wire an Input Action event into an actor/pawn Blueprint graph use unreal-blueprints. Triggers on input, enhanced input, input action, IA, mapping context, IMC, key binding, key mapping, add input mapping, remove input mapping, modifier, input trigger, axis, action mapping, input value type, swizzle, dead zone, hold trigger, validate input mappings.
---

# unreal-input

Drives the **`input`** namespace via `input_query(action, params)`: author and inspect Enhanced Input Action (IA) and Input Mapping Context (IMC) assets, their key mappings, modifiers, and triggers. The 10 actions below are a snapshot — discover first so you never call a stale or guessed name.

## Discovery

```
monolith_discover({ namespace: "input" })
describe_query({ action: "action_schema", params: {
  target_namespace: "input", target_action: "<action>"
}})
```

## When to use

Use this skill for the Enhanced Input ASSET layer — creating/inspecting `UInputAction` and `UInputMappingContext` assets, adding or removing key mappings, configuring or cloning modifiers and triggers, and validating IMCs for missing actions or duplicate-key conflicts.

Use a different skill for:

- **unreal-config** — generic `DefaultInput.ini` section/value edits and other project config (`.ini` read/edit, cvars, project settings) rather than the IA/IMC asset itself.
- **unreal-gas** — binding an Input Action to a GAS ability activation; the ability input ID / input binding lives on the GAS side.
- **unreal-blueprints** — wiring an Input Action event into an actor/pawn Blueprint graph, versus defining the IA/IMC and its key mappings here.

## Action Reference

Param notation: `name*` required, `name?` optional, `name=val` default, `a/b/c` allowed, `[w]` mutates. Signatures are a snapshot of the live catalog — call `describe_query` with `action_schema` for one action's exact schema, or use `monolith_discover({ namespace: "input", detail: true })` for all 10 schemas. The Discovery block above stays authoritative.

### Enhanced Input Assets (10)

| Action | Params | Purpose |
|--------|--------|---------|
| `get_input_action` | `asset_path*` | Inspect an Enhanced Input UInputAction asset |
| `list_input_actions` | `path?` `include_details?=false` | List Enhanced Input UInputAction assets |
| `get_input_mapping_context` | `asset_path*` | Inspect an Enhanced Input UInputMappingContext asset |
| `list_input_mapping_contexts` | `path?` `include_details?=false` | List Enhanced Input UInputMappingContext assets |
| `validate_input_mappings` | `context_paths?` `path?` | Validate IMCs for missing actions and duplicate key conflicts |
| `[w] create_input_action` | `asset_path*` `value_type?=Boolean` (Boolean/Axis1D/Axis2D/Axis3D) `description?` `consume_input?=true` `trigger_when_paused?=false` `accumulation?` (TakeHighestAbsoluteValue/Cumulative) `overwrite?=false` `dry_run?=false` `confirm?=false` `save?=false` | Create or update a UInputAction asset |
| `[w] set_input_action_properties` | `asset_path*` `value_type?` (Boolean/Axis1D/Axis2D/Axis3D) `description?` `consume_input?` `consume_legacy_mappings?` `trigger_when_paused?` `reserve_all_mappings?` `accumulation?` (TakeHighestAbsoluteValue/Cumulative) `dry_run?=false` `confirm?=false` `save?=false` | Update common UInputAction properties |
| `[w] create_input_mapping_context` | `asset_path*` `description?` `overwrite?=false` `dry_run?=false` `confirm?=false` `save?=false` | Create or update a UInputMappingContext asset |
| `[w] add_input_mapping` | `context_path*` `action_path*` `key*` `source_context_path?` `source_action_path?` `source_key?` `modifier_classes?` `trigger_classes?` `allow_duplicate?=false` `dry_run?=false` `confirm?=false` `save?=false` | Add or update a key mapping; optionally clone or instantiate modifiers/triggers |
| `[w] remove_input_mapping` | `context_path*` `action_path*` `key*` `dry_run?=false` `confirm?=false` `save?=false` | Remove matching key mappings without dirtying the package when none exist |

## Mutation Contract

- Every write requires either `dry_run: true` or `confirm: true`. Calls with neither are rejected before an asset is created, loaded for mutation, or dirtied.
- `dry_run: true` predicts `would_create`, `would_update`, or `would_change` with `preview_state: "proposed"` without creating/loading modifier or trigger packages/classes/CDOs, constructing UObjects, creating packages, or modifying assets. It takes precedence if `confirm` is also present.
- `save` defaults to `false`. A confirmed in-memory change marks the package dirty; `save: true` additionally writes it immediately. If that save fails, inspect structured `error.data`: the in-memory mutation is already committed, `retry_safe` is false, and a blind retry can duplicate intent. Dry-runs and semantic no-ops never dirty or save a package.
- Asset paths and list roots must resolve under `/Game`. Omitted list roots default to `/Game`; malformed or non-`/Game` paths are rejected rather than redirected to a fallback asset or widened to Engine/plugin content.
- `add_input_mapping` reuses the existing action+key mapping by default. Set `allow_duplicate: true` only when a deliberate duplicate row is required.
- `source_context_path`, `source_action_path`, and `source_key` form one clone selector and must be supplied together.
- When `modifier_classes` or `trigger_classes` is present, the array replaces cloned/existing entries; an empty array explicitly clears that side. Dry-run validates soft path syntax and already-loaded class compatibility, returns `class_resolution: "deferred_until_confirm"`, and performs full class loading/validation only on confirmation.

## Common Workflows

### Preview, create, and map an IA + IMC

```
input_query({ action: "create_input_action", params: {
  asset_path: "/Game/Input/IA_Jump", value_type: "Boolean", dry_run: true
}})
input_query({ action: "create_input_action", params: {
  asset_path: "/Game/Input/IA_Jump", value_type: "Boolean", confirm: true
}})
input_query({ action: "create_input_mapping_context", params: {
  asset_path: "/Game/Input/IMC_Default", confirm: true
}})
input_query({ action: "add_input_mapping", params: {
  context_path: "/Game/Input/IMC_Default",
  action_path: "/Game/Input/IA_Jump",
  key: "SpaceBar",
  trigger_classes: ["/Script/EnhancedInput.InputTriggerHold"],
  confirm: true
}})
```

### Clone modifiers and triggers from another mapping

```
input_query({ action: "add_input_mapping", params: {
  context_path: "/Game/Input/IMC_Default",
  action_path: "/Game/Input/IA_Confirm",
  key: "Enter",
  source_context_path: "/Game/Input/IMC_Default",
  source_action_path: "/Game/Input/IA_Jump",
  source_key: "SpaceBar",
  confirm: true
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

- This reference follows the live `RegisterAction` surface. If an action is missing or renamed, re-run `monolith_discover({ namespace: "input" })`; the catalog is the source of truth.
- Call `describe_query({ action: "action_schema", params: { target_namespace: "input", target_action: "<action>" }})` for exact required/optional params and types before calling an action.
- `validate_input_mappings` flags missing actions and duplicate key conflicts within an IMC; run it after a batch of `add_input_mapping` edits.
