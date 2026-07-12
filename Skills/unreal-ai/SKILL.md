---
name: unreal-ai
description: Use when working with Unreal Engine engine-native AI via Monolith MCP — Behavior Trees, StateTree, Blackboard, EQS, navigation/navmesh and nav links, Smart Objects, Mass/ZoneGraph crowds, AI perception, scaffolding NPC/enemy AI, and runtime AI debugging. For LogicDriver state-machine assets use unreal-logicdriver; for one-off navmesh-path/raycast queries on live actors use unreal-scene; if the AI drives GAS abilities use unreal-gas. Triggers on AI, NPC, enemy AI, behavior tree, BT, StateTree, blackboard, blackboard key, EQS query, decorator, service, navmesh, nav mesh bounds, nav link proxy, nav modifier, pathfinding, AI move to, smart object, mass, zonegraph, crowd, perception, AIController logic, pawn sensing, patrol, wander, spawn AI, AI scaffold.
---

# unreal-ai

Authors and inspects Unreal Engine engine-native AI (Behavior Trees, StateTree, Blackboard, EQS, navigation, Smart Objects, Mass/ZoneGraph, perception, AI controllers, runtime debugging) by driving the Monolith **`ai`** namespace. **245 actions** via `ai_query(action, params)`.

**Param notation (used in the reference tables):** `name*` required, `name?` optional, `name=val` default, `a/b/c` allowed values, `[w]` mutates (wraps a transaction). Signatures are a snapshot of the live catalog — for the exact, full, current schema of any action call `monolith_discover` with `mode: "schema"` (the discover-first block below stays authoritative).

## Discover first (do this before calling an action)

The tables below are a snapshot of the live registry. The catalog is the source of truth — always confirm the action exists and read its exact params before calling:

```
monolith_discover({ namespace: "ai" })                                       # all actions in this namespace
monolith_discover({ namespace: "ai", action: "<action>", mode: "schema" })   # exact params for one action
monolith_find("<what you want>")                                             # jump straight to the right action
```

If an action below is missing or renamed, re-run `monolith_discover({ namespace: "ai" })`.

## When to use / Use a different skill for

Use **unreal-ai** for engine-native AI driven by Behavior Trees or StateTree, navmesh/nav-link configuration, EQS, Smart Objects, Mass/ZoneGraph crowds, AI perception, AI controllers, and one-call AI scaffolding.

- **unreal-logicdriver** — the state logic is a LogicDriver SM asset rather than a Behavior Tree / StateTree.
- **unreal-scene** — you need a one-off navmesh-path / raycast / line-of-sight query on live actors (e.g. `scene.build_navmesh`) rather than configuring AI navigation data.
- **unreal-gas** — the AI uses Gameplay Ability System abilities/effects (the `ai` convenience `add_bt_use_ability_task` fires a GAS ability, but ability/effect authoring lives in unreal-gas).
- **unreal-blueprints** — editing the Blueprint logic that backs the AI character/controller.

## Action groups (245 total)

`ai` is the largest namespace. Actions are grouped by engine subsystem; full Action/Purpose tables with callable param signatures live in the reference files below.

| Group | Count | Reference |
|-------|-------|-----------|
| State Tree | 35 | [behavior-tree-statetree.md](references/behavior-tree-statetree.md) |
| Behavior Tree | 32 | [behavior-tree-statetree.md](references/behavior-tree-statetree.md) |
| Blackboard | 12 | [behavior-tree-statetree.md](references/behavior-tree-statetree.md) |
| Navigation | 26 | [navigation-eqs-smartobject.md](references/navigation-eqs-smartobject.md) |
| EQS | 20 | [navigation-eqs-smartobject.md](references/navigation-eqs-smartobject.md) |
| Smart Object | 16 | [navigation-eqs-smartobject.md](references/navigation-eqs-smartobject.md) |
| Mass Zone Graph | 22 | [mass-perception-runtime.md](references/mass-perception-runtime.md) |
| Advanced (Mass/Zone) | 12 | [mass-perception-runtime.md](references/mass-perception-runtime.md) |
| Perception | 11 | [mass-perception-runtime.md](references/mass-perception-runtime.md) |
| Perception Scaffold | 1 | [mass-perception-runtime.md](references/mass-perception-runtime.md) |
| Controller | 10 | [mass-perception-runtime.md](references/mass-perception-runtime.md) |
| Runtime (PIE only) | 14 | [mass-perception-runtime.md](references/mass-perception-runtime.md) |
| Discovery | 11 | [mass-perception-runtime.md](references/mass-perception-runtime.md) |
| Scaffold | 23 | [mass-perception-runtime.md](references/mass-perception-runtime.md) |

Subtotals: 35 + 32 + 12 + 26 + 20 + 16 + 22 + 12 + 11 + 1 + 10 + 14 + 11 + 23 = **245**.

## High-value entry points

- **Scaffold a new AI fast:** `scaffold_complete_ai_character`, `scaffold_enemy_ai`, `scaffold_boss_ai`, `scaffold_companion_ai`, `scaffold_ambient_npc` (one call wires Controller + BT + Blackboard + Perception) — or `hello_world_ai` to smoke-test.
- **Author a Behavior Tree:** `create_bt_from_template` / `build_behavior_tree_from_spec` → add composites / tasks / decorators / services → `set_bt_blackboard` → bind keys.
- **Author a StateTree:** `create_st_from_template` / `build_state_tree_from_spec` → add states / transitions / tasks / conditions → `compile_state_tree` (mandatory).
- **Perception:** `add_perception_to_actor` (Sight / Hearing / Damage) → debug stimuli at runtime.
- **Validate before commit:** `batch_validate_ai_assets`.

## Common workflows

```text
# 1. Discover, then scaffold a full enemy AI in one call
monolith_discover({ namespace: "ai", action: "scaffold_enemy_ai", mode: "schema" })
ai_query("scaffold_enemy_ai", { name: "BP_GuardAI", path: "/Game/AI/Guard" })

# 2. Build a Behavior Tree from a declarative spec, then validate
ai_query("build_behavior_tree_from_spec", { path: "/Game/AI/BT_Guard", spec: { ... } })
ai_query("validate_behavior_tree", { path: "/Game/AI/BT_Guard" })

# 3. StateTree must be compiled after edits
ai_query("add_st_state", { path: "/Game/AI/ST_Boss", name: "Phase2" })
ai_query("compile_state_tree", { path: "/Game/AI/ST_Boss" })

# 4. Runtime AI debugging (PIE session required)
ai_query("runtime_get_bt_state", { actor: "BP_GuardAI_C_0" })
```

## Gotchas / Rules

- Behavior Tree / StateTree edits operate on the **asset**; `compile_state_tree` is MANDATORY after any StateTree edit, and BTs save/recompile as the schema specifies.
- `add_bt_node` and the specialized BT task adders fail if the Behavior Tree schema rejects the parent-child edge; a success response guarantees that the new graph node is actually connected, not orphaned.
- `delete_blackboard`, `delete_behavior_tree`, and `delete_eqs_query` use verified asset lifecycle deletion. Success means the package, registry row, package files, and source-control cleanup all reached the absent state; inspect `delete_verification` when auditing destructive work.
- Navigation / path queries need a built navmesh — build it first (use `unreal-scene` `scene.build_navmesh` for one-off queries on the live level).
- `runtime_*` actions require a PIE/game session; authoring and scaffold actions work in-editor.
- Mass/ZoneGraph mutation actions (spawn/despawn/set_*) currently report availability and are guarded until editor-vs-PIE world tests exist — they do not mutate runtime state.
- The reference tables are a generated snapshot of the live `RegisterAction` surface. If an action is missing or renamed, the live catalog wins — re-run `monolith_discover`.
