---
name: unreal-logicdriver
description: "Use for LogicDriver state machines via Monolith MCP: author/inspect SM assets, states, transitions, conduits, nodes and node Blueprints, scaffold common SM patterns (quest/dialogue/weapon/interactable/game-flow), build from spec/text, components, and runtime SM control/inspection. Triggers on logic driver, logicdriver, state machine, SM, state, transition, conduit, node blueprint, scaffold quest, dialogue, game flow, runtime state, switch state, build from spec, text graph."
---

# unreal-logicdriver

**66 actions** via `logicdriver_query(action, params)`. Action names below are the live registry surface; call `monolith_discover` for exact parameter schemas.

## Discovery

```
monolith_discover({ namespace: "logicdriver" })                      # all actions in this namespace
monolith_discover({ namespace: "logicdriver", action: "<action>", mode: "schema" })  # exact params
```

## Action Reference

### Graph (20)

| Action | Purpose |
|--------|---------|
| `add_any_state_node` | Add an Any State node to a Logic Driver state machine graph |
| `add_conduit` | Add a conduit node to a Logic Driver state machine graph |
| `add_state` | Add a state node to a Logic Driver state machine graph |
| `add_state_machine_node` | Add a nested state machine node to a Logic Driver state machine graph |
| `add_transition` | Add a transition between two nodes in a Logic Driver state machine |
| `auto_arrange_graph` | Auto-arrange all nodes in a state machine graph using built-in BFS layout. Blueprint Assist is disabled for asset mutation. |
| `compile_state_machine` | Compile a Logic Driver State Machine Blueprint and return success/failure with error messages |
| `find_nodes_by_class` | Find all nodes whose class name matches a given string (full or partial match) |
| `find_nodes_by_type` | Find all nodes of a given type (state/transition/conduit/any_state/state_machine) in the SM |
| `get_node_connections` | List all inbound and outbound transitions for a node |
| `get_node_details` | Get detailed info for a specific node including all UPROPERTY values and connections |
| `get_sm_statistics` | Get statistics for a state machine: state/transition/conduit/nested SM counts, max depth, total nodes |
| `get_sm_structure` | Get hierarchical JSON structure of an entire state machine: states, transitions, conduits, nested SMs, GUIDs |
| `move_node` | Move a node to a specific position in the graph editor |
| `remove_node` | Remove a node from a Logic Driver state machine graph (breaks all connections first) |
| `rename_node` | Rename a node in a Logic Driver state machine |
| `set_end_state` | Set or clear the end state flag on a state node |
| `set_initial_state` | Set a state as the initial state by rewiring the entry node |
| `set_node_class` | Set the custom node class (NodeInstanceClass) on a Logic Driver node via reflection |
| `set_node_properties` | Set UPROPERTY values on a Logic Driver node via reflection |

### Asset (8)

| Action | Purpose |
|--------|---------|
| `create_node_blueprint` | Create a new Logic Driver Node Blueprint (custom state, transition, conduit, or state machine node class) |
| `create_state_machine` | Create a new Logic Driver State Machine Blueprint via USMBlueprintFactory |
| `delete_state_machine` | Delete a Logic Driver State Machine Blueprint asset |
| `duplicate_state_machine` | Deep copy a Logic Driver State Machine Blueprint to a new path |
| `get_node_blueprint` | Get info about a Logic Driver Node Blueprint: class hierarchy, node type, properties |
| `get_state_machine` | Get full JSON dump of a state machine's structure: states, transitions, conduits, nested SMs |
| `list_node_blueprints` | List all Logic Driver Node Blueprints in the project |
| `list_state_machines` | List all Logic Driver State Machine Blueprints in the project via AssetRegistry |

### Node (8)

| Action | Purpose |
|--------|---------|
| `configure_conduit` | Set conduit properties (eval_with_transitions, conduit_as_state) via reflection |
| `configure_state` | Set state node configuration flags (always_update, disable_tick_transition, exclude_from_any_state) via reflection |
| `configure_state_machine_node` | Configure a nested state machine node: reuse behavior, independent tick, and other settings |
| `configure_transition` | Set transition properties (priority, color, eval_mode, can_eval_with_start_state) via reflection |
| `get_exposed_properties` | Read all exposed graph properties on SM nodes — FSMGraphProperty variables visible in the graph editor |
| `set_exposed_property` | Set an exposed property value on an SM node by name via reflection |
| `set_state_tags` | Set gameplay tags on a state node. Clears existing tags and applies the provided array. |
| `set_transition_condition` | Set transition condition type: always_true, time_delay, event_based, or tag_check. Sets properties via reflection (no graph rewiring). |

### Runtime (7)

| Action | Purpose |
|--------|---------|
| `runtime_evaluate_transitions` | Force transition evaluation on a live SM instance during PIE |
| `runtime_get_sm_state` | Get the active state(s) of a live SM instance in PIE — state name, GUID, time in state |
| `runtime_get_state_history` | Get state transition history from a live SM instance during PIE |
| `runtime_restart_sm` | Restart a live SM instance during PIE (stop + initialize + start) |
| `runtime_start_sm` | Initialize and start a live SM instance during PIE |
| `runtime_stop_sm` | Stop a live SM instance during PIE |
| `runtime_switch_state` | Force-switch to a specific state by GUID during PIE |

### Scaffold (7)

| Action | Purpose |
|--------|---------|
| `scaffold_dialogue_sm` | Create a dialogue state machine with speaker/text states wired in sequence, with optional branching choices |
| `scaffold_game_flow_sm` | Create a game flow state machine: MainMenu->Loading->Gameplay->Pause->Results->Credits with loops |
| `scaffold_hello_world_sm` | Create a ready-to-use SM Blueprint with 3 states (Idle->Active->Complete) and transitions — a quick-start template |
| `scaffold_horror_encounter_sm` | Create a horror encounter state machine: Dormant->Lurking->Stalking->Chasing->Attacking->Retreating->Despawned |
| `scaffold_interactable_sm` | Create an interactable state machine with custom states (default: locked/unlocked/open/closed) |
| `scaffold_quest_sm` | Create a quest state machine: Inactive -> Active -> [objectives] -> Complete/Failed |
| `scaffold_weapon_sm` | Create an FPS weapon state machine: Idle->Drawing->Ready->Firing->Cooldown->Reloading with transitions |

### Discovery (6)

| Action | Purpose |
|--------|---------|
| `explain_state_machine` | Generate a structured explanation of a state machine: purpose, states, flow paths, key decisions, complexity rating |
| `find_node_class_usages` | Search all SM Blueprints in the project for nodes that use a specific Node Blueprint class |
| `find_sm_references` | Find all Blueprints in the project that reference a given SM Blueprint (via dependencies) |
| `get_sm_overview` | Project scan: count SM Blueprints, Node Blueprints, and component usage across the project |
| `validate_state_machine` | Validate a state machine for common issues: missing initial state, orphaned states, unreachable nodes |
| `visualize_sm_as_text` | Generate a text diagram of a state machine in ASCII, Mermaid, or DOT format |

### Spec (5)

| Action | Purpose |
|--------|---------|
| `build_sm_from_spec` | Create a complete state machine from a JSON spec in one call. The crown jewel — states, transitions, conduits, nested SMs, initial/end markers, all wired and compiled |
| `compare_state_machines` | Compare two state machines structurally: diff states, transitions, and topology by name |
| `export_sm_json` | Export a state machine's full structure as JSON. Optionally write to a file on disk |
| `export_sm_spec` | Export a state machine as a spec JSON (same format as build_sm_from_spec input). Inverse of build_sm_from_spec |
| `import_sm_json` | Import a state machine from a JSON spec — either a file path or inline JSON string. Parses and delegates to build_sm_from_spec logic |

### Component (3)

| Action | Purpose |
|--------|---------|
| `add_sm_component` | Add a Logic Driver SM component to an actor Blueprint via SimpleConstructionScript |
| `configure_sm_component` | Set SM component properties on an actor Blueprint: auto_start, tick_interval, network_config via reflection |
| `get_sm_component_config` | Read SM component configuration on an actor Blueprint: state machine class, auto-start, tick interval, network config, and all SM-specific properties |

### Text Graph (2)

| Action | Purpose |
|--------|---------|
| `get_dialogue_flow` | Walk entire SM and extract dialogue flow: speakers, lines, choices, branching paths |
| `get_text_graph_content` | Read FSMTextGraphProperty content (dialogue text, speaker names) from state nodes |

## Typical workflows

- **Author an SM:** `create_state_machine` → `add_state` / `add_transition` / `add_conduit` → `configure_state` / `configure_transition` → `set_initial_state` → `compile_state_machine` → `validate_state_machine`
- **Scaffold a pattern (one call):** `scaffold_quest_sm` / `scaffold_dialogue_sm` / `scaffold_weapon_sm` / `scaffold_interactable_sm` / `scaffold_game_flow_sm` / `scaffold_horror_encounter_sm` (or `scaffold_hello_world_sm` to smoke-test)
- **Spec / text round-trip:** `build_sm_from_spec` ← `export_sm_spec`; inspect with `visualize_sm_as_text` / `get_text_graph_content` / `get_dialogue_flow`
- **Node Blueprints:** `create_node_blueprint` → `set_node_class` / `set_node_properties` → `get_node_blueprint`
- **Runtime (PIE):** `runtime_start_sm` → `runtime_get_sm_state` / `runtime_switch_state` / `runtime_evaluate_transitions` → `runtime_stop_sm`

## Gotchas

- Action names were **renamed** from earlier versions (e.g. `get_all_states`→`get_sm_overview`, `export_sm_to_json`→`export_sm_json`, `scaffold_quest`→`scaffold_quest_sm`). Always confirm with `monolith_discover`.
- `runtime_*` actions need a live PIE/game session; authoring actions work in-editor.
- `compile_state_machine` before `validate_state_machine`; `auto_arrange_graph` after structural edits keeps the graph readable.

## Notes

- This reference is generated from the live `RegisterAction` surface. If an action is missing or renamed, re-run `monolith_discover({ namespace: "logicdriver" })` — the catalog is the source of truth.
- Pass `mode: "schema"` to `monolith_discover` for required/optional params and types before calling an action.
