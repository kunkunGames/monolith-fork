---
name: unreal-logicdriver
description: Use for LogicDriver state-machine ASSETS via Monolith MCP (logicdriver namespace) — author/inspect SM assets, states, transitions, conduits, Any-State, nested SMs, node Blueprints, scaffold SM patterns (quest/dialogue/weapon/interactable/game-flow/horror), build/round-trip from JSON spec or text graph, SM actor components, and runtime SM control in PIE. For an AnimBP's internal AnimGraph state machine use unreal-animation; for a melee/attack combo chain use unreal-combograph; for engine-native StateTree/Behavior Tree AI logic use unreal-ai; when SM states activate GAS abilities/effects author those in unreal-gas. Triggers on logic driver, logicdriver, state machine, SM, FSM, state, transition, conduit, any state, nested state machine, node blueprint, scaffold quest, dialogue, game flow, weapon SM, interactable, horror encounter, runtime state, switch state, build from spec, text graph, mermaid SM.
---

# unreal-logicdriver

**66 actions** (prior documentation) via `logicdriver_query(action, params)`. Call `monolith_discover` for exact action names and parameter schemas — see the callout below.

> **Plugin not currently loaded — re-verify when enabled.** There is no `logicdriver` namespace dump in this editor — LogicDriver is a Fab/marketplace plugin and the namespace is not registered here. The live schema is therefore UNAVAILABLE, so every action name and parameter below is **unverified prior documentation, not invented** — nothing here was guessed or fabricated, but none of it has been checked against a live catalog. Before relying on any action once the LogicDriver plugin is enabled, CONFIRM the exact names and schemas via `monolith_discover({ namespace: "logicdriver" })` (with `mode: "schema"` per action) and reconcile this file against the live catalog with the drift checker `Scripts/check_skill_catalog_drift.ps1`. Do not treat these action names or signatures as verified.

## Discovery

```
monolith_discover({ namespace: "logicdriver" })                      # all actions in this namespace
monolith_discover({ namespace: "logicdriver", action: "<action>", mode: "schema" })  # exact params
```

If an action below is missing or renamed, re-run `monolith_discover({ namespace: "logicdriver" })` — the live catalog is the source of truth. Because the namespace is not loaded in this editor, expect `monolith_discover` to return no `logicdriver` actions until the plugin is enabled.

## When to use / Use a different skill for

Use **unreal-logicdriver** for LogicDriver SM **assets** — the standalone state-machine Blueprint (states, transitions, conduits, Any-State, nested SMs, node Blueprints) used for quest/dialogue/game-flow/interactable/weapon/horror logic, plus its runtime control in PIE.

- **unreal-animation** — "state machine" means the state machine **inside an AnimBP's AnimGraph** (anim states/transitions/notifies), not a LogicDriver SM asset.
- **unreal-combograph** — the state machine is specifically a melee/attack **combo chain**; use the dedicated ComboGraph authoring instead of a generic SM.
- **unreal-ai** — the decision logic is engine-native **StateTree / Behavior Tree** AI, versus a LogicDriver SM asset.
- **unreal-gas** — SM states **activate GAS abilities/effects**; author those abilities/effects in GAS and drive them from the state machine.

## Action Reference

Param notation: `name*` required, `name?` optional, `name=val` default, `a/b/c` allowed values, `[w]` mutates project state. **No per-action signatures are listed here because the `logicdriver` namespace is not loaded in this editor and there is no live-catalog dump to source them from** — inventing parameters is not allowed. The action names below are prior documentation; confirm both names and full schemas with `monolith_discover` (`mode: "schema"`) once the LogicDriver plugin is enabled. The discover-first block above is the authority.

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

## Common Workflows

> **Documented from prior knowledge — params unverified.** The `logicdriver` namespace is **not loaded** in this editor, so the recipes below are **representative sequences built only from the prior-documentation Action Reference tables above**; nothing here is invented, but no action name or param has been checked against a live catalog. Treat every `{ ...param }` placeholder as illustrative, not authoritative. Before running any step once the LogicDriver plugin is enabled, CONFIRM the action name and full schema with `monolith_discover({ namespace: "logicdriver", action: "<action>", mode: "schema" })`. Every action named below appears in this file's tables.

### Author an SM step-by-step (create → add → configure → wire → compile → validate)

```
1. logicdriver_query("create_state_machine", { ...save_path })          # Asset: new SM Blueprint via USMBlueprintFactory
2. logicdriver_query("add_state", { ...sm + state_name })               # Graph: add each state node (repeat per state)
3. logicdriver_query("add_transition", { ...sm + from + to })           # Graph: wire transitions between nodes (repeat per edge)
4. logicdriver_query("set_initial_state", { ...sm + state })            # Graph: rewire the entry node to the start state
5. logicdriver_query("configure_transition", { ...transition + priority/eval_mode }) # Node: set transition props via reflection
6. logicdriver_query("set_transition_condition", { ...transition + type }) # Node: always_true/time_delay/event_based/tag_check
7. logicdriver_query("auto_arrange_graph", { ...sm })                    # Graph: BFS layout after structural edits
8. logicdriver_query("compile_state_machine", { ...sm })                # Graph: compile, returns success/failure + errors
9. logicdriver_query("validate_state_machine", { ...sm })               # Discovery: missing-initial / orphaned / unreachable checks (compile first)
```

### Build from a JSON spec, then round-trip and inspect (one-call authoring)

```
1. logicdriver_query("build_sm_from_spec", { ...spec_json })            # Spec: states+transitions+conduits+nested, all wired and compiled in one call
2. logicdriver_query("validate_state_machine", { ...sm })               # Discovery: validate the generated SM
3. logicdriver_query("visualize_sm_as_text", { ...sm + format })        # Discovery: ASCII/Mermaid/DOT diagram to eyeball the topology
4. logicdriver_query("export_sm_spec", { ...sm })                       # Spec: inverse of build_sm_from_spec — re-export the editable spec
```

### Scaffold a pattern in one call, then explain it

```
1. logicdriver_query("scaffold_quest_sm", { ...path })                  # Scaffold: or scaffold_dialogue_sm / scaffold_weapon_sm / scaffold_interactable_sm / scaffold_game_flow_sm / scaffold_horror_encounter_sm / scaffold_hello_world_sm
2. logicdriver_query("explain_state_machine", { ...sm })                # Discovery: purpose, states, flow paths, key decisions, complexity rating
```

### Drive a live SM in PIE (runtime — needs an active PIE/game session)

```
1. logicdriver_query("runtime_start_sm", { ...instance })               # Runtime: initialize + start the live SM instance
2. logicdriver_query("runtime_get_sm_state", { ...instance })           # Runtime: active state name/GUID, time in state
3. logicdriver_query("runtime_switch_state", { ...instance + guid })    # Runtime: force-switch by GUID
4. logicdriver_query("runtime_stop_sm", { ...instance })                # Runtime: stop the live SM instance
```

## Gotchas

- Action names were **renamed** from earlier versions (e.g. `get_all_states`→`get_sm_overview`, `export_sm_to_json`→`export_sm_json`, `scaffold_quest`→`scaffold_quest_sm`). Always confirm with `monolith_discover`.
- `runtime_*` actions need a live PIE/game session; authoring actions work in-editor.
- `compile_state_machine` before `validate_state_machine`; `auto_arrange_graph` after structural edits keeps the graph readable.

## Notes

- These tables are **prior documentation, not a live-catalog snapshot** — the `logicdriver` namespace is not registered in this editor (LogicDriver is a marketplace plugin). Treat the live catalog as the source of truth once the plugin is enabled (see Discovery above).
- Pass `mode: "schema"` to `monolith_discover` for required/optional params and types before calling an action; expect no `logicdriver` actions to resolve until the plugin is loaded.
