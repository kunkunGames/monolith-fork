---
name: unreal-blueprints
description: Use when reading, creating, modifying, or compiling actor/component Blueprint graphs via Monolith MCP — variables, components, functions, nodes, pins, interfaces, graph management, Blueprint-defined DataTable/struct/enum assets, templates, layout, timelines, level blueprints, CDO properties, graph export/import. For the backing C++ use unreal-cpp; to map a BP node to its native symbol use unreal-bridge; for Widget Blueprints/UMG use unreal-ui; for Gameplay Ability/Effect Blueprints use unreal-gas; for generic asset lifecycle on a DataTable/struct asset use unreal-asset. Triggers on Blueprint, BP, BP function, event graph, add node, wire pins, blueprint variable, function graph, component, component hierarchy, reparent blueprint, construction script, macro, event dispatcher, actor blueprint, blueprint interface, compile, interface, DataTable, struct, enum, template, layout, timeline, level blueprint, CDO.
---

# Unreal Blueprint Workflows

Drives the **`blueprint`** namespace via `blueprint_query()`: read, author, and compile actor/component Blueprint graphs, plus the Blueprint-defined DataTable/struct/enum assets. The ~86 actions enumerated below are a snapshot — discover first so you never call a stale or guessed name:

```
monolith_discover({ namespace: "blueprint" })                       // list live actions
describe_query("action_schema", { namespace: "blueprint", action: "add_node" })  // exact params for one action
```

Also works on: Level Blueprints (map path or `$current`), Widget Blueprints.

## When to use / Use a different skill for

- **unreal-cpp** — the work is the backing C++ rather than the Blueprint graph.
- **unreal-bridge** — you need to map a Blueprint node to its backing native symbol.
- **unreal-ui** — the Blueprint is a Widget Blueprint / UMG.
- **unreal-gas** — the Blueprint is a Gameplay Ability or Gameplay Effect.
- **unreal-asset** — the work is generic asset lifecycle (import/save/rename/delete) on a DataTable or struct asset rather than graph editing.

## Key Parameters

- `asset_path` -- Blueprint path (NOT `asset`). Level BPs: map path or `"$current"`
- `graph_name` -- from `list_graphs`
- `node_id` -- from `get_graph_data` or `add_node` response
- `component_name` -- from `get_components`

## Action Reference

**Param notation:** `name*` required, `name?` optional, `name=val` default, `a/b/c` allowed values, `[w]` mutates (transaction-wrapped). Signatures are a snapshot of the live catalog — for the exact, full, current schema of any action call `monolith_discover` with mode `schema` (or `describe_query("action_schema", {namespace:"blueprint", action:"<name>"})`).

Full per-action parameter signatures — grouped by category (Read 19, CDO 4, Discovery 1, Variable CRUD 8, Component CRUD 7, Graph Management 17, Node & Pin 9, Compile & Create 5, Timeline 4, Struct/Enum/DataTable 6, Build from Spec 1, Graph Export 3, Diff/Template/Layout/Batch/Events 10) — live in [`references/actions.md`](references/actions.md). The Discovery block above stays the authority; do not call from that snapshot alone if param names, aliases, or ranges are load-bearing. Type strings and the `add_node` Types reference stay inline below.

**Type strings:** `bool`, `int`, `int64`, `float`, `double`, `string`, `name`, `text`, `byte`, `object:Class`, `class:Class`, `struct:Struct`, `enum:Enum`, `exec`, `wildcard`, `array:T`, `set:T`, `map:K:V`

### Component CRUD (7)

Full signatures: [`references/actions.md`](references/actions.md).

### Graph Management (17)

Full signatures: [`references/actions.md`](references/actions.md).

### Node & Pin (9)

Full signatures: [`references/actions.md`](references/actions.md).

#### `add_node` Types (~25)

| node_type | Extra Params | Aliases/Notes |
|-----------|-------------|-------|
| `CallFunction` | `function_name`, `target_class`? | `call`, `function` |
| `VariableGet`/`VariableSet` | `variable_name` | `get`/`set` |
| `CustomEvent` | `event_name`, `replication`?, `reliable`? | `event`. Server/client/multicast RPC |
| `Branch` / `Sequence` | — | |
| `MacroInstance` | `macro_name`, `macro_blueprint`? | `macro` |
| `SpawnActorFromClass` | `actor_class` | `spawn` |
| `DynamicCast` | `cast_class` | `cast` |
| `Self` / `Return` | — | |
| `MakeStruct` / `BreakStruct` | `struct_type` | |
| `SwitchOnEnum` / `SwitchOnInt` / `SwitchOnString` | `enum_type`? | |
| `FormatText` | `format`? | `"Hello {Name}"` creates arg pins |
| `MakeArray` | `num_entries`? | |
| `Select` / `ForEachLoop` / `ForLoop` / `ForLoopWithBreak` | — | |
| `DoOnce` / `FlipFlop` / `Gate` | — | Engine macros |
| `IsValid` / `Delay` / `RetriggerableDelay` | — | |
| `ComponentBoundEvent` | `component_name`, `delegate_property_name` | "+OnClicked" event entry. Rejects duplicate (component, delegate) BP-wide. Component must be SCS or UMG widget; delegate must be `BlueprintAssignable` multicast on the component class |
| `AddDelegate` | `delegate_property_name`, `target_class`? | "Bind Event to..." for `BlueprintAssignable` multicast. Defaults to self-context (BP's class); `target_class` accepts bare or prefixed forms |
| `RemoveDelegate` | `delegate_property_name`, `target_class`? | "Unbind Event from..." — removes one previously bound event. Same params as `AddDelegate` |
| `ClearDelegate` | `delegate_property_name`, `target_class`? | "Unbind all Events from..." — clears every bound listener. Same params as `AddDelegate` |
| `CallDelegate` | `delegate_property_name`, `target_class`? | "Call ..." — broadcasts a BP-resident multicast delegate to all listeners. Spawned node has one input pin per delegate signature parameter |
| *(any UK2Node_ class)* | — | Generic fallback |

### Compile & Create (5)

Full signatures: [`references/actions.md`](references/actions.md).

Use `asset.save_asset` for generic package saves; `blueprint.save_asset` is not registered as a public compatibility alias.

### Timeline (4)

Full signatures: [`references/actions.md`](references/actions.md).

### Struct, Enum & DataTable (6)

Full signatures: [`references/actions.md`](references/actions.md).

### Build from Spec (1)

Full signatures: [`references/actions.md`](references/actions.md).

Nodes use spec IDs (e.g., `"id": "evt"`) mapped to real IDs in connections/pin_defaults.

### Graph Export (3)

Full signatures: [`references/actions.md`](references/actions.md).

### Diff, Template, Layout, Batch, Events (10)

Full signatures: [`references/actions.md`](references/actions.md).

## Common Workflows

Each recipe is a numbered sequence of real `blueprint_query` calls. Read with `get_graph_summary` / `get_variables` / `get_components` before mutating, and always `compile_blueprint` after structural changes. All actions below are from this skill's Action Reference table.

### Recipe — New actor BP end-to-end (create → variable → component → event → wire → compile)
```
// 1. Create the Blueprint asset
blueprint_query({ action: "create_blueprint", params: { save_path: "/Game/Test/BP_Door", parent_class: "Actor" } })
// 2. Add a member variable
blueprint_query({ action: "add_variable", params: { asset_path: "/Game/Test/BP_Door", name: "bIsOpen", type: "bool", default_value: "false" } })
// 3. Add a component (read the hierarchy afterward to get its resolved component_name)
blueprint_query({ action: "add_component", params: { asset_path: "/Game/Test/BP_Door", component_class: "StaticMeshComponent", component_name: "DoorMesh" } })
blueprint_query({ action: "get_components", params: { asset_path: "/Game/Test/BP_Door" } })
// 4. Add an event node and a function-call node (capture the returned node ids)
blueprint_query({ action: "add_node", params: { asset_path: "/Game/Test/BP_Door", node_type: "CustomEvent", event_name: "OnInteract", position: [0, 0] } })
blueprint_query({ action: "add_node", params: { asset_path: "/Game/Test/BP_Door", node_type: "CallFunction", function_name: "PrintString", position: [300, 0] } })
// 5. Wire the event's exec output into the function's exec input (pin names are case-insensitive)
blueprint_query({ action: "connect_pins", params: { asset_path: "/Game/Test/BP_Door", source_node: "<OnInteract node_id>", source_pin: "Then", target_node: "<PrintString node_id>", target_pin: "execute" } })
// 6. Compile and read back any errors (errors include node_id + graph_name)
blueprint_query({ action: "compile_blueprint", params: { asset_path: "/Game/Test/BP_Door" } })
```

### Recipe — DataTable + struct authoring (struct → table → row → verify)
```
// 1. Author the row struct (fields: [{name, type, default_value?}])
blueprint_query({ action: "create_user_defined_struct", params: { save_path: "/Game/Test/S_ItemRow", fields: [ { name: "DisplayName", type: "text" }, { name: "Price", type: "int", default_value: "0" } ] } })
// 2. Create a DataTable backed by that struct
blueprint_query({ action: "create_data_table", params: { save_path: "/Game/Test/DT_Items", row_struct: "/Game/Test/S_ItemRow" } })
// 3. Add a row keyed by row_name (values: {column: value})
blueprint_query({ action: "add_data_table_row", params: { asset_path: "/Game/Test/DT_Items", row_name: "Sword", values: { DisplayName: "Iron Sword", Price: 120 } } })
// 4. Read rows back to confirm
blueprint_query({ action: "get_data_table_rows", params: { asset_path: "/Game/Test/DT_Items" } })
```

### Build from Spec (one call)
```
blueprint_query({ action: "build_blueprint_from_spec", params: {
  asset_path: "/Game/Test/BP_Door",
  nodes: [
    {"id": "evt", "type": "CustomEvent", "event_name": "OnInteract", "position": [0, 0]},
    {"id": "print", "type": "CallFunction", "function_name": "PrintString", "position": [300, 0]}
  ],
  connections: [{"source": "evt", "source_pin": "Then", "target": "print", "target_pin": "execute"}],
  pin_defaults: [{"node_id": "print", "pin_name": "InString", "value": "Door opened!"}],
  auto_compile: true
}})
```

### Server RPC
```
blueprint_query({ action: "add_node", params: {
  asset_path: "/Game/BP_Player", node_type: "CustomEvent",
  event_name: "ServerTakeDamage", replication: "server", reliable: true
}})
```

## Rules

- Pin names are case-insensitive. Wrong names show available pins in error.
- Compile errors include `node_id` + `graph_name` for targeted debugging.
- Any `UK2Node_` subclass name works as `node_type` (generic fallback).
- Use `get_graph_summary` first, then `get_graph_data` with `node_class_filter` for specifics.
- Always compile after structural changes.
