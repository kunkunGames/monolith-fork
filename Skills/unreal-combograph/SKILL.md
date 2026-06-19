---
name: unreal-combograph
description: Use when working with the ComboGraph plugin via Monolith MCP (combograph namespace) — creating and editing the combo GRAPH (nodes/edges/wiring), effects, cues, and scaffolding combo abilities. unreal-combograph owns the ComboGraph graph; to edit the underlying montage/anim asset use unreal-animation, if the combo triggers a GAS ability/effect use unreal-gas, and for a generic state-machine attack chain use unreal-logicdriver. Triggers on combo, combo system, combo graph, ComboGraph asset, combo node, combo edge, combo transition, combo window, combo ability, combo montage, melee chain, attack chain, input buffer combo, next attack, branch attack, hit sequence.
---

# Unreal ComboGraph Workflows

Edits the ComboGraph plugin combo graph (nodes/edges/wiring, effects, cues, scaffolded abilities) via the **combograph** namespace. **13 ComboGraph actions** via `combograph_query()`.

> **Plugin not currently loaded — re-verify when enabled.** The `combograph` namespace dump reports `status: not_installed` / `actions: 0` in this editor (ComboGraph is a Fab marketplace plugin, gated by `#if WITH_COMBOGRAPH`). The live schema is therefore UNAVAILABLE, so every action name and parameter below is **unverified prior documentation, not invented** — nothing here was guessed or fabricated, but none of it has been checked against a live catalog. Before relying on any action once the ComboGraph plugin is enabled, CONFIRM the exact names and schemas via `monolith_discover({ namespace: "combograph" })` (with `mode: "schema"` per action) and reconcile this file against the live catalog with the drift checker `Scripts/check_skill_catalog_drift.ps1`. Do not treat these signatures as verified.

## Discover first

Always confirm live action names and schemas before calling — these tables are a snapshot, and (since the plugin is not loaded here) the params are unverified prior documentation. The discover-first block is the authority.

```
monolith_discover({ namespace: "combograph" })
combograph_query({ action: "get_combo_graph_info", mode: "schema" })
```

## When to use / Use a different skill for

- **unreal-combograph** (this skill) — the ComboGraph graph itself: nodes, edges, combo wiring, node effects/cues, and combo-ability scaffolding.
- **unreal-animation** — editing the underlying montage/anim sequence asset (notifies, sync markers, sections) rather than the combo graph wiring.
- **unreal-gas** — when the combo triggers a Gameplay Ability or Gameplay Effect and you need to author the ability/effect/attribute side.
- **unreal-logicdriver** — when the attack chain is modeled as a generic LogicDriver state machine instead of a ComboGraph asset.

## Key Parameters

- `asset_path` -- ComboGraph asset path (e.g. `/Game/Combos/CG_LightAttack`)
- `node_id` -- node identifier | `montage_path` -- animation montage path
- `save_path` -- destination for new assets | `ability_path` -- gameplay ability path

## Action Reference

Param notation: `name*` required, `name?` optional, `name=val` default, `a/b/c` allowed values, `[w]` mutates project state. Signatures here are PRIOR DOCUMENTATION (the plugin is not loaded, so they are not a live-catalog snapshot) — for the exact, full, current schema of any action call `monolith_discover` with `mode: "schema"` once ComboGraph is enabled.

| Action | Key Params | Purpose |
|--------|-----------|---------|
| **Read (4)** | | |
| `list_combo_graphs` | `path_filter`? | List all ComboGraph assets |
| `get_combo_graph_info` | `asset_path` | Full graph: nodes, edges, entry points, effects |
| `get_combo_node_effects` | `asset_path`, `node_id` | Gameplay effects on a node |
| `validate_combo_graph` | `asset_path` | Lint: orphans, missing montages, broken edges, unreachable nodes |
| **Create (5)** | | |
| `create_combo_graph` | `save_path` | Create new ComboGraph |
| `add_combo_node` | `asset_path`, `montage_path`, `node_name`? | Add node with montage |
| `add_combo_edge` | `asset_path`, `source_node`, `target_node`, `input_type`? | Add transition edge |
| `set_combo_node_effects` | `asset_path`, `node_id`, `effects` | Set gameplay effects |
| `set_combo_node_cues` | `asset_path`, `node_id`, `cues` | Set gameplay cues |
| **Scaffold (3)** | | |
| `create_combo_ability` | `save_path`, `combo_graph`?, `initial_input`?, `parent_class`? | Create Gameplay Ability for ComboGraph |
| `link_ability_to_combo_graph` | `ability_path`, `combo_graph` | Link existing ability |
| `scaffold_combo_from_montages` | `save_path`, `montages`, `input_action`?, `transition_behavior`? | Full graph from ordered montage list |
| **Layout (1)** | | |
| `layout_combo_graph` | `asset_path`, `horizontal_spacing`?, `vertical_spacing`? | Auto-layout nodes (BFS tree) |

## Technical Notes

- **Reflection-only** -- interacts via UObject reflection + Asset Registry, no direct C++ API linkage. Works with any ComboGraph version.
- **EdGraph sync** -- action handlers update runtime graph AND reconstruct EdGraph automatically.
- **`#if WITH_COMBOGRAPH`** -- probes Plugins/ and Plugins/Marketplace/. Absent = empty stub (0 actions).
- **Settings toggle** -- `bEnableComboGraph` in UMonolithSettings (default: true).
- **GAS integration** -- `create_combo_ability` and `link_ability_to_combo_graph` require both ComboGraph AND GameplayAbilities plugins.

## Common Workflows

### Create combo from scratch
```
combograph_query({ action: "create_combo_graph", params: { save_path: "/Game/Combos/CG_LightAttack" }})
combograph_query({ action: "add_combo_node", params: { asset_path: "/Game/Combos/CG_LightAttack", montage_path: "/Game/Animations/AM_Slash_1", node_name: "Slash1" }})
combograph_query({ action: "add_combo_node", params: { asset_path: "/Game/Combos/CG_LightAttack", montage_path: "/Game/Animations/AM_Slash_2", node_name: "Slash2" }})
combograph_query({ action: "add_combo_edge", params: { asset_path: "/Game/Combos/CG_LightAttack", source_node: "Slash1", target_node: "Slash2", input_type: "LightAttack" }})
```

### Quick scaffold from montage list
```
combograph_query({ action: "scaffold_combo_from_montages", params: {
  save_path: "/Game/Combos/CG_HeavyCombo",
  montages: ["/Game/Animations/AM_Heavy_1", "/Game/Animations/AM_Heavy_2", "/Game/Animations/AM_Heavy_Finisher"]
}})
```

## Anti-Patterns

- **Orphan nodes** -- no edges (except entry). `validate_combo_graph` catches these.
- **Missing montages** -- deleted montage refs. Flagged by validation.
- **Circular edges without exit** -- infinite combo chains.
- **Unlinked ability** -- graph exists but no ability references it.
- **Effect on entry node** -- damage before attack animation plays. Usually unintended.

## Tips

- Use `scaffold_combo_from_montages` for quick setup, then customize with effects/cues
- Always `validate_combo_graph` after editing
- Pair graphs with abilities via `create_combo_ability` or `link_ability_to_combo_graph`
- Open asset in ComboGraph editor after MCP edits to visually verify
