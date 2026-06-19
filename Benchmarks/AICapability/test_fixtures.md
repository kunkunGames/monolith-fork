# AICapability Test Fixtures

The fixture contract created by
`python Scripts/ai_capability_benchmark.py setup_fixtures --mcp-url http://localhost:9316/mcp`.
All assets live under `/Game/Benchmarks/AI/` and are portable (do not depend on GO content). Setup
is idempotent: an already-existing asset/key/state is tolerated and reported as `already existed`.

## Assets

| Fixture | Path | Created by | Used by |
| --- | --- | --- | --- |
| Blackboard | `/Game/Benchmarks/AI/BB_BenchAI` | `create_blackboard` | blackboard edit_execute, error_path, discovery; linked from BT |
| Behavior Tree | `/Game/Benchmarks/AI/BT_BenchAI` | `create_behavior_tree` (linked to BB_BenchAI) | behavior_tree edit_execute, error_path, discovery |
| Empty-scratch Behavior Tree | `/Game/Benchmarks/AI/BT_BenchEmptyScratch` | `create_behavior_tree` (NO nodes) | compile_gate (negative `validate_invalid` probe) |
| Validate-scratch Behavior Tree | `/Game/Benchmarks/AI/BT_BenchValidateScratch` | `create_behavior_tree` (linked to BB_BenchAI) + `add_bt_node` Selector | compile_gate (positive `validate_valid` probe) |
| EQS query | `/Game/Benchmarks/AI/EQS_BenchAI` | `create_eqs_query` | eqs edit_execute, error_path, discovery |

**StateTree is compiled out on this build (`WITH_STATETREE=0`)**, so **no StateTree fixture is
seeded**. `create_st_from_template` and `lint_state_tree` are runtime stubs that return `isError`
"StateTree module not available (WITH_STATETREE=0)"; the `ai` namespace also exposes no granular
StateTree authoring action. StateTree is covered only by schema-presence tasks (one `edit_schema`,
one `read_schema`) that don't touch a fixture, so there is no StateTree edit_execute, error_path, or
gate task and no StateTree scratch asset.

`duplicate_reject` creates and self-deletes its own throwaway assets per run
(`BB_BenchDup` / `BT_BenchDup` / `EQS_BenchDup`), so they are not part of the standing fixture set.
There is no `ST_BenchDup` — the `ai` namespace has no `create_state_tree` action whose duplicate
wording the category can assert.

## Blackboard keys (seeded on BB_BenchAI)

`add_bb_key` with the verified key-type tokens (`CreateKeyTypeFromString`,
`MonolithAIBlackboardActions.cpp:55-170`: Bool/Int/Float/String/Name/Vector/Rotator/Object/Class/Enum/NativeEnum).

| Key | Type |
| --- | --- |
| `BenchTargetActor` | `Object` |
| `BenchHomeLocation` | `Vector` |
| `BenchIsAlerted` | `Bool` |
| `BenchPatrolIndex` | `Int` |

The first key (`BenchTargetActor`) is asserted present by the readiness preflight.

## Behavior Tree validate fixtures (the compile_gate probes)

`BT_BenchEmptyScratch` is created with `create_behavior_tree` and **no nodes**. Its root has no
children, so `validate_behavior_tree` emits an `error`-severity issue "Root has no children — empty
Behavior Tree" and reports `valid == false` (VERIFIED LIVE: `{valid:false, issue_count:1}`) — exactly
what the compile_gate negative `validate_invalid` probe asserts (`valid==false` plus the error
issue). It must stay empty: setup_fixtures intentionally adds no `add_bt_node` to it.

`BT_BenchValidateScratch` is created with `create_behavior_tree` (linked to `BB_BenchAI`) and gets a
`BTComposite_Selector` root child via `add_bt_node`. A Selector-rooted BT with a linked Blackboard
has no `error`-severity issues, so `validate_behavior_tree` reports `valid == true` (VERIFIED LIVE:
`{valid:true, issue_count:2 (warnings)}`) — exactly what the compile_gate positive `validate_valid`
probe asserts.

These two Behavior Tree probes replace the StateTree lint gate, which is unavailable on
`WITH_STATETREE=0`.

## Readiness preflight

`preflight` (and the implicit preflight before `run`) confirms each fixture is reachable via its
canonical read action and, where listed, contains the seed token:

| Fixture | Read action | Required token |
| --- | --- | --- |
| `BB_BenchAI` | `get_blackboard` | `BenchTargetActor` |
| `BT_BenchAI` | `get_behavior_tree` | — |
| `EQS_BenchAI` | `get_eqs_query` | — |
| `BT_BenchEmptyScratch` | `get_behavior_tree` | — |
| `BT_BenchValidateScratch` | `get_behavior_tree` | — |

If any fixture is missing or the contract token is absent, `preflight` and `run` exit non-zero with
`failure_kind=fixture_contract_missing` (or `transport_error` if the endpoint is unreachable). Run
`setup_fixtures` first.

## Notes / known constraints

- `ai` is an **editor-only** namespace — there is no offline `monolith_query.exe` path. `setup_fixtures`,
  `preflight`, and `run` all require the live MCP endpoint at `http://localhost:9316/mcp`
  (`BatchFiles\RunHeadlessEditor.bat` / `Scripts/recover_mcp.ps1` to bring it up).
- StateTree is **compiled out on this build (`WITH_STATETREE=0`)**: `create_st_from_template` and
  `lint_state_tree` are registered in the catalog but execute as runtime stubs returning `isError`
  "StateTree module not available (WITH_STATETREE=0)". The benchmark therefore exercises StateTree
  only through schema-presence tasks and never runs a StateTree execute/lint task on this build. The
  `ai` namespace also has no granular StateTree authoring or `compile_state_tree` action.
- Heavy mutation under the `-nullrhi` headless editor is a known instability; the benchmark is
  designed for a single coordinated run per editor boot (re-run after a fresh `setup_fixtures`).
