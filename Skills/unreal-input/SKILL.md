---
name: unreal-input
description: Use for read-only discovery, bounded inspection, and validation of Enhanced Input Action and Input Mapping Context assets through the Monolith input namespace. Routes asset authoring to unreal-blueprints or a purpose-built writer instead of guessing mutation actions.
---

# unreal-input

Use the `input` namespace to inventory and audit `UInputAction` and `UInputMappingContext` assets without modifying, compiling, saving, transacting, or dirtying them.

## Discovery

```text
monolith_discover({ namespace: "input" })
describe_query({ action: "action_schema", params: {
  target_namespace: "input", target_action: "<action>"
}})
```

The live catalog is authoritative. The five actions below are the current read-only surface.

## Routing

Use this skill for:

- finding Input Action and Input Mapping Context assets
- reading Input Action value type, behavior flags, triggers, and modifiers
- reading mapping rows, keys, actions, triggers, modifiers, and player-mappable metadata
- preflighting missing actions, invalid keys, duplicate-key assignments, and scan completeness

Use a different workflow for:

- `unreal-config` — `DefaultInput.ini` or other project configuration
- `unreal-gas` — binding Input Actions to GAS ability activation
- `unreal-blueprints` — wiring Enhanced Input events into Blueprint graphs
- asset mutation — the `input` namespace is intentionally read-only; do not guess create, set, add, or remove action names

## Actions

| Action | Parameters | Result |
|---|---|---|
| `list_input_actions` | `path=/Game`, `offset=0`, `limit=200` (1–1000), `include_details=false` | Stable page of Input Action identities; detailed rows include bounded trigger/modifier classes |
| `get_input_action` | `asset_path` | Value type, description, behavior flags, accumulation policy, trigger/modifier classes, counts, and truncation flags |
| `list_input_mapping_contexts` | `path=/Game`, `offset=0`, `limit=200` (1–1000), `include_details=false`, `mapping_limit=100` (1–500) | Stable context page; optional details load only the returned assets and bound each mapping list |
| `get_input_mapping_context` | `asset_path`, `mapping_offset=0`, `mapping_limit=100` (1–500) | Context metadata and a stable bounded page of mapping rows |
| `validate_input_mappings` | `context_paths` or `path=/Game`, `offset=0`, `limit=200` (1–1000), `mapping_scan_limit=4096` (1–10000) | Per-context issues plus `valid`, `complete`, error/warning counts, and pagination evidence |

## Contract

- Asset inputs accept canonical mounted Unreal package paths (`/Game/Input/IA_Jump`) or matching top-level object paths (`/Game/Input/IA_Jump.IA_Jump`). Filesystem paths, subobjects, whitespace aliases, and mismatched package/object leaves are rejected.
- Package filters are canonical roots such as `/Game/Input`; an omitted filter defaults to `/Game`.
- List results are sorted by object path before pagination. `total`, `offset`, `limit`, `count`, and `has_more` make continuation deterministic.
- Input Action trigger/modifier arrays and mapping rows have independent hard caps. Truncation is explicit and never presented as a complete result.
- `validate_input_mappings` treats a missing action, invalid key, load failure, or scan cutoff as an error. A key assigned to multiple actions is a warning because Enhanced Input can use that layout intentionally.
- `page_complete` reports whether every mapping in the returned context page was scanned. `all_contexts_covered` reports whether pagination covered the full selected context set. Global `complete` requires both; `valid=true` additionally requires zero errors. Always check `valid`, `complete`, and `has_more`.
- The namespace stays available when Monolith GAS authoring is disabled because these are Enhanced Input asset reads, not GAS mutations.

## Workflows

### Inventory actions without loading every asset

```text
input_query({ action: "list_input_actions", params: {
  path: "/Game/Input", offset: 0, limit: 200
}})
```

Set `include_details: true` only when the returned page needs trigger/modifier metadata.

### Page through a mapping context

```text
input_query({ action: "get_input_mapping_context", params: {
  asset_path: "/Game/Input/IMC_Default",
  mapping_offset: 0,
  mapping_limit: 100
}})
```

If `has_more_mappings` is true, advance `mapping_offset` by `mappings_returned`.

### Validate selected contexts

```text
input_query({ action: "validate_input_mappings", params: {
  context_paths: [
    "/Game/Input/IMC_Default",
    "/Game/Input/IMC_Menu"
  ],
  mapping_scan_limit: 4096
}})
```

`context_paths` and `path` are mutually exclusive. An explicit empty or duplicate path list is rejected instead of being treated as an all-project scan.
