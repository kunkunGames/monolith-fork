# Monolith — MonolithAI Module

**Parent:** [SPEC_CORE.md](../SPEC_CORE.md)
**Engine:** Unreal Engine 5.7+
**Version:** 0.14.7 (Beta)

---

## MonolithAI

**Dependencies:** Core, CoreUObject, Engine, MonolithCore, UnrealEd, AIModule, GameplayTasks, NavigationSystem, Json, JsonUtilities
**Namespace:** `ai` | **Tool:** `ai_query(action, params)` | **Actions:** 221 (Phase J F8: +`add_perception_to_actor`, +`get_bt_graph`)
**Conditional:** State Trees (`#if WITH_STATETREE`) and Smart Objects (`#if WITH_SMARTOBJECTS`) are required dependencies. Mass Entity (`#if WITH_MASSENTITY`) and Zone Graph (`#if WITH_ZONEGRAPH`) are optional extensions. When required deps are absent, the module compiles to an empty stub (0 actions registered).
**Settings toggle:** `bEnableAI` (default: True)

MonolithAI provides comprehensive MCP coverage of Unreal Engine's AI framework. It covers Behavior Trees, Blackboards, State Trees, Environment Query System (EQS), Smart Objects, AI Controllers, AI Perception, Navigation, runtime/PIE control, scaffolding templates, discovery, and advanced AI operations.

### Action Categories

Counts below are the **actual** registrations from `Source/MonolithAI/Private/MonolithAI*Actions.cpp` (verified 2026-04-26 post-Phase-J). Pre-J the spec listed `~N` aspirational figures that summed to 249, exceeding the 219 actually shipped. Audit corrected the table to literal counts.

| Category | Actions | Source file | Description |
|----------|---------|-------------|-------------|
| Behavior Trees | 32 | `MonolithAIBehaviorTreeActions.cpp` | BT CRUD, node management, decorator/service/task creation, composite nodes, spec-based generation. **F1** crash-hardening + **F8** `get_bt_graph` + **F15** invalid-vs-unknown GUID error split |
| Blackboards | 12 | `MonolithAIBlackboardActions.cpp` | BB CRUD, key management, key types, inheritance, inspection |
| State Trees | 35 | `MonolithAIStateTreeActions.cpp` | ST CRUD, state/transition management, conditions, tasks, spec-based generation. Conditional on `#if WITH_STATETREE` |
| EQS | 20 | `MonolithAIEQSActions.cpp` | EQS query CRUD, generator/test management, contexts, debugging |
| Smart Objects | 16 | `MonolithAISmartObjectActions.cpp` | SO definition CRUD, slot configuration, behavior binding. Conditional on `#if WITH_SMARTOBJECTS` |
| AI Controllers | 10 | `MonolithAIControllerActions.cpp` | Controller configuration, team assignment, focus management |
| Perception | 11 | `MonolithAIPerceptionActions.cpp` | Sight/hearing/damage/team sense configuration, stimulus management (AIController-only) |
| Perception Scaffold | 1 | `MonolithAIPerceptionScaffoldActions.cpp` | **F8** `add_perception_to_actor` — accepts ANY actor BP (not just AIControllers) plus a `senses` array |
| Navigation | 24 | `MonolithAINavigationActions.cpp` | NavMesh queries, path finding, nav link management, nav modifier volumes |
| Runtime/PIE | 14 | `MonolithAIRuntimeActions.cpp` | Runtime BT/ST inspection, active task queries, blackboard value read/write in PIE |
| Scaffolding | 23 | `MonolithAIScaffoldActions.cpp` | Pre-built AI patterns: patrol, guard, investigate, flee, horror stalker, search area |
| Discovery | 11 | `MonolithAIDiscoveryActions.cpp` | AI asset overview, explain, compare, validate, search |
| Advanced | 12 | `MonolithAIAdvancedActions.cpp` | Mass Entity + Zone Graph cross-module integration (conditional `#if WITH_MASSENTITY`, `#if WITH_ZONEGRAPH`) |

**Total:** 32 + 12 + 35 + 20 + 16 + 10 + 11 + 1 + 24 + 14 + 23 + 11 + 12 = **221**.

### Phase J fixes touching this module

- **F1 (2026-04-26)** — BT crash hardening: 5 `add_bt_*` actions + `build_behavior_tree_from_spec` reject Task-under-Root parenting via `ValidateParentForChildTask` + schema-checked `ConnectParentChild`.
- **F8 (2026-04-26)** — Two new actions: `add_perception_to_actor` (any actor BP, `senses` array) and `get_bt_graph` (flat node_id/parent_id/children GUID dump).
- **F15 (2026-04-26)** — `MonolithAIBehaviorTreeActions.cpp` 16 sites hoisted into `RequireBtNodeByGuid` helper — invalid-GUID and unknown-GUID errors now distinct.

See [SPEC_CORE.md §11 Recent Fixes](../SPEC_CORE.md#recent-fixes-phase-j--shipped-in-0147) for the long-form descriptions.

### Key Actions

> **`build_behavior_tree_from_spec` (power action).** Creates a complete behavior tree from a JSON specification. Handles composite/decorator/service/task node creation, wiring, and compilation in a single call.
>
> **`build_state_tree_from_spec` (power action).** Creates a complete state tree from a JSON specification. Handles state/transition/condition/task creation and compilation.
>
> **Scaffolding templates.** Pre-built AI patterns for common game scenarios including patrol routes, guard behavior, investigation, flee response, and horror-specific stalker AI.

### Notes

> **24K lines of C++ across 30 files.** MonolithAI is the largest domain module by code volume.
>
> **Multi-plugin conditional compilation.** Unlike single-guard modules (GAS, ComboGraph, LogicDriver), MonolithAI uses multiple compile-time guards. State Trees and Smart Objects are required; Mass Entity and Zone Graph are optional extensions that unlock additional actions when present.

---
