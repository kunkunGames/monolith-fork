# AICapability Test Fixtures

The standing fixture contract is created by:

```powershell
python Scripts/ai_capability_benchmark.py setup_fixtures --mcp-url http://localhost:9316/mcp
```

All assets live under `/Game/Benchmarks/AI/` and are portable; they do not depend on Speed gameplay
content. Setup is idempotent for the three standing fixtures and saves the seeded Blackboard keys so
an editor restart cannot silently revert the contract.

## Standing assets

| Fixture | Path | Created by | Used by |
| --- | --- | --- | --- |
| Blackboard | `/Game/Benchmarks/AI/BB_BenchAI` | `create_blackboard`, four `add_bb_key` calls, then `asset.save_asset` | Blackboard edit/read/error/discovery tasks; linked from the BT fixture |
| Behavior Tree | `/Game/Benchmarks/AI/BT_BenchAI` | `create_behavior_tree` linked to `BB_BenchAI` | Behavior Tree edit/read/error/discovery tasks |
| EQS query | `/Game/Benchmarks/AI/EQS_BenchAI` | `create_eqs_query` | EQS edit/read/error/discovery tasks |

The readiness preflight checks only these standing assets:

| Fixture | Read action | Required token |
| --- | --- | --- |
| `BB_BenchAI` | `get_blackboard` | `BenchTargetActor` |
| `BT_BenchAI` | `get_behavior_tree` | — |
| `EQS_BenchAI` | `get_eqs_query` | — |

If a standing fixture is missing or the Blackboard token is absent, `preflight` and `run` exit
non-zero with `fixture_contract_missing` (or the relevant transport/protocol failure) before scoring.

## Blackboard keys

`BB_BenchAI` contains the following persisted keys. Their type tokens match
`CreateKeyTypeFromString` in `MonolithAIBlackboardActions.cpp`.

| Key | Type |
| --- | --- |
| `BenchTargetActor` | `Object` |
| `BenchHomeLocation` | `Vector` |
| `BenchIsAlerted` | `Bool` |
| `BenchPatrolIndex` | `Int` |

## Task-owned scratch assets

Mutable test preconditions are not standing fixtures. Each task owns the complete reset → create/edit
→ assertion → cleanup → cleanup-readback lifecycle. Only an explicit `not found` response is accepted
for the initial reset; an arbitrary delete failure aborts the probe before its first scored call.

| Purpose | Scratch path/entity | Lifecycle proof |
| --- | --- | --- |
| Negative compile gate | `/Game/Benchmarks/AI/BT_BenchNegativeGateScratch` | Delete-if-absent → create empty BT → require `valid=false` plus an error issue → verified delete |
| Positive compile gate | `/Game/Benchmarks/AI/BT_BenchPositiveGateScratch` | Delete-if-absent → create BT linked to `BB_BenchAI` → add Selector → require `valid=true` → verified delete |
| Blackboard duplicate rejection | `BenchDupKey` on `BB_BenchAI` | Remove-if-absent → add twice → require duplicate error → remove → `get_blackboard` proves exact key absence |
| Blackboard asset duplicate rejection | `/Game/Benchmarks/AI/BB_BenchDuplicateScratch` | Delete-if-absent → create twice → require duplicate error → verified delete |
| Behavior Tree duplicate rejection | `/Game/Benchmarks/AI/BT_BenchDuplicateScratch` | Delete-if-absent → create twice → require duplicate error → verified delete |
| EQS duplicate rejection | `/Game/Benchmarks/AI/EQS_BenchDuplicateScratch` | Delete-if-absent → create twice → require duplicate error → verified delete |
| BT template edit chain | `/Game/Benchmarks/AI/BT_BenchTemplateScratch` plus derived `BB_BenchTemplateScratch` | Delete-if-absent for both → create patrol template → read back Blackboard linkage → delete both → independently require both public reads to report `not found` |

Delete success is not accepted on the handler return alone. Asset scratch cleanup calls the matching
`get_*` action and requires an input-specific `not found` error that echoes the scratch path; key
cleanup requires an exact parsed-name absence. This catches residual `.uasset` files that a delete
handler might otherwise leave reloadable on disk.

The historical source-controlled `BB_BenchDup`, `BT_BenchDup`, `EQS_BenchDup`,
`BT_BenchEmptyScratch`, and `BT_BenchValidateScratch` assets are deliberately not used by these
lifecycles. A benchmark must not delete or mutate another changelist's persistent fixtures.

## StateTree scope

StateTree is compiled out in this Speed build (`WITH_STATETREE=0`). `create_st_from_template` and
`lint_state_tree` remain registered but return a module-unavailable error when executed, and the
namespace exposes no granular StateTree authoring action. StateTree therefore contributes only the
two schema-presence probes; no StateTree fixture, mutation, error-path, or compile-gate asset exists.

## Operational notes

- `ai` is editor-only. Setup, preflight, and run require the local Speed MCP endpoint at
  `http://localhost:9316/mcp`.
- Use `Plugins\Monolith\Scripts\recover_mcp.ps1`; it launches
  `Build\BatchFiles\RunHeadlessEditor.bat` for this checkout.
- A passing run leaves none of the seven task-owned template, duplicate, or compile-gate packages on
  disk, in the Asset Registry, or in Perforce.
