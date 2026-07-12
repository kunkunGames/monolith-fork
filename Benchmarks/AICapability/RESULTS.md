# AICapability Results

## 2026-07-11 run-20260711-final-isolated — cleanup-complete canonical final

| Metric | Value |
| --- | ---: |
| `ai_capability_score` | **1.0** |
| `edit_execute_rate` (0.34) | 1.0 |
| `error_path_rate` (0.16) | 1.0 |
| `compile_gate_rate` (0.12) | 1.0 |
| `duplicate_reject_rate` (0.10) | 1.0 |
| `edit_schema_rate` / `discovery_rate` / `read_schema_rate` | 1.0 / 1.0 / 1.0 |
| Tasks / transport failures | 74/74 / 0 |
| Status identity | Stable: Monolith 0.20.3, catalog `sha256:4af1375172e8818a`, Speed, UE 5.8 |

The final run used the seven-path scratch ownership contract and executed with no
other asset-mutating benchmark. Every create/duplicate/compile scratch package
was reset, cleaned through the public delete action, and independently read back
as absent. All seven paths were absent from disk and Perforce after the run; the
default changelist contained no AICapability asset.

| Reproducibility item | Value |
| --- | --- |
| Task SHA-256 | `AE898D3FDFADAC2FBB07371C3A409EADCCE8FBA855FBD34187B93A6874A446BB` |
| Manifest SHA-256 | `4C0610BE988BF8984852504BBB0595C5D60EFC33909E3428088C03B731AD5CF6` |
| Input fingerprint | `b3bfb12bcbdf3f86611f7a4a431d64af556ec356778c656a056c126da0cdc24b` |
| Output | `Saved\Monolith\Benchmarks\AICapability\run-20260711-final-isolated` |

The preceding concurrent diagnostic `run-20260711-final-clean` scored 0.966
because another four-job AssetEditing request garbage-collected an unsaved
Blackboard between `add_bb_key` and its readback. That failure was not hidden or
accepted as noise: it led to the production `GARBAGE_COLLECTION_KEEPFLAGS` fix
and the passing
`MonolithAsset.DeleteAssets.PreservesUnrelatedStandaloneDirtyAsset` automation
test. The isolated rerun above is the canonical baseline after that root fix.

## 2026-07-11 baseline-20260711d — first fully-scored live run (74 tasks)

| Metric | Value |
| --- | ---: |
| `ai_capability_score` | **1.0** |
| `edit_execute_rate` (0.34) | 1.0 — includes the new `create_bt_from_template` scaffold-linkage guard chain |
| `error_path_rate` (0.16) | 1.0 |
| `compile_gate_rate` (0.12) | 1.0 |
| `duplicate_reject_rate` (0.10) | 1.0 |
| `edit_schema_rate` / `discovery_rate` / `read_schema_rate` | 1.0 / 1.0 / 1.0 |

Run context: Speed CL 1093 suite refresh
(`Docs/testing/2026-07-11-benchmark-contract-failfast-and-n3-guards.md`), editor
0.20.3, 74-task corpus. Two runner repairs unblocked this run the same day:
(1) token checks moved to `searchable_text` (the live server returns terse
`content[0].text` "OK; see structuredContent." with the JSON in
`structuredContent`, which false-failed the fixture preflight and would have
false-failed every edit_execute read-back), and (2) `setup_fixtures` now saves
the Blackboard after seeding keys (`add_bb_key` edits memory only, so editor
restarts reverted `BB_BenchAI` to SelfActor-only and failed the preflight).
Output: `Saved\Monolith\Benchmarks\AICapability\baseline-20260711d`.

## Status

| Item | State |
| --- | --- |
| Runner | `Scripts/ai_capability_benchmark.py` — implemented, `generate` verified offline |
| Tasks / manifest | `tasks.jsonl` (74 tasks) + `manifest.json` generated; weights sum to 1.0; integrity checks pass; every `ai` action verified against `_ai_catalog.txt` |
| Offline self-test | `Scripts/test_ai_capability_benchmark.py` — **39/39 passed** (scoring, transport/protocol aborts, isolated scratch reset/cleanup, and exact absence readbacks validated against fabricated MCP responses) |
| Live scored run | **1.0**, 74/74 canonical isolated final (2026-07-11, above). The concurrent 0.966 diagnostic exposed and drove the unrelated-dirty-asset GC fix. A prior exploratory run scored **0.886** (pre-`WITH_STATETREE=0` fix); its 3 StateTree stub tasks were removed/replaced (see below). |

A scored run mutates AI assets and needs the live MCP endpoint (the single shared headless editor).
Run it in an isolated mutation window: an unrelated mutating benchmark can legitimately expose
cross-request asset-lifecycle bugs, but such an adversarial diagnostic is not a canonical baseline.

## Offline validation performed (updated 2026-07-11)

```powershell
# from D:\P4\speed\Plugins\Monolith
python Scripts/ai_capability_benchmark.py generate
#   -> task_count 74; category_counts {compile_gate:2, discovery:6, duplicate_reject:4,
#      edit_execute:10, edit_schema:28, error_path:8, read_schema:16}
#   -> weights sum to 1.0; score_formula rendered; integrity checks pass
#   -> every ai action in tasks.jsonl exists in Saved/Monolith/LogAnalysis/_ai_catalog.txt

python Scripts/test_ai_capability_benchmark.py
#   -> 39/39 passed, including compile-gate truth tables, silent-no-op detection,
#      exact duplicate/reset/cleanup contracts, create-chain multi-package cleanup,
#      MCP protocol validation, task-level transport attribution, and invalid-run gates.
```

## Reconciliation against the live `ai` catalog (2026-06-18)

The original runner draft referenced **invented** StateTree actions (`create_state_tree`,
`compile_state_tree`, `add_st_state`, etc.) that do not exist; those were removed and the StateTree
surface was reduced to the real `create_st_from_template` + `lint_state_tree`.

A subsequent live run scored **0.886 with exactly 3 failures**, ALL StateTree tasks, because
**StateTree is compiled out of this build (`WITH_STATETREE=0`)** — even the real `create_st_from_template`
and `lint_state_tree` actions are runtime stubs that return `isError` "StateTree module not available
(WITH_STATETREE=0)". The 3 failing tasks were: (1) the StateTree `create_st_from_template`+`lint`
`edit_execute` chain, (2) the `lint_state_tree`-on-missing-asset `error_path`, and (3) the negative
`compile_gate` StateTree lint probe. None can pass on a `WITH_STATETREE=0` build. Fix applied:

- **Removed** the StateTree `edit_execute` chain (and its `ST_BenchTemplateScratch` fixture).
- **Replaced** the StateTree `error_path` with a `validate_behavior_tree`-on-missing-asset probe
  (`/Game/Benchmarks/AI/NONEXISTENT_BT_ZZZZ`), which executes and echoes the offending path.
- **Replaced** the negative `compile_gate` (StateTree lint) with a Behavior Tree `validate_invalid`
  gate: an EMPTY BT (`BT_BenchEmptyScratch`, `create_behavior_tree` with no nodes) must make
  `validate_behavior_tree` report `valid==false` with an `error`-severity issue "Root has no children
  — empty Behavior Tree" (VERIFIED LIVE: `{valid:false, issue_count:1}`). The positive gate
  (`validate_valid` on the Selector-rooted clean BT; VERIFIED LIVE `{valid:true, issue_count:2}`)
  is unchanged.
- **Kept** `create_st_from_template` (`edit_schema`) and `lint_state_tree` (`read_schema`) — these
  are schema-presence tasks that only assert the action is registered in the catalog (true even when
  stubbed), so they pass on `WITH_STATETREE=0`.

`edit_execute` dropped 10→9; total dropped 74→73; `error_path` (8) and `compile_gate` (2) counts are
unchanged. Behavior Tree / Blackboard / EQS coverage was unchanged.

## Live run commands (require the shared editor)

Bring the MCP endpoint up first (`http://localhost:9316/mcp`):

```powershell
# 0. start / recover the headless editor + MCP endpoint
D:\P4\speed\Build\BatchFiles\RunHeadlessEditor.bat # or: Plugins\Monolith\Scripts\recover_mcp.ps1
#    wait for localhost:9316 to listen, confirm with monolith_status()

# from D:\P4\speed\Plugins\Monolith
# 1. seed the AI fixtures at /Game/Benchmarks/AI/
python Scripts/ai_capability_benchmark.py setup_fixtures --mcp-url http://localhost:9316/mcp

# 2. confirm fixture readiness (exit 0 = ready)
python Scripts/ai_capability_benchmark.py preflight --mcp-url http://localhost:9316/mcp

# 3. score a run (writes summary.json / per_task.json / per_task.jsonl under --output-dir)
python Scripts/ai_capability_benchmark.py run `
    --mcp-url http://localhost:9316/mcp `
    --output-dir Saved/Monolith/Benchmarks/AICapability/baseline-v1 `
    --label baseline-v1

# 4. (later) compare two runs
python Scripts/ai_capability_benchmark.py compare `
    --baseline Saved/Monolith/Benchmarks/AICapability/baseline-v1/summary.json `
    --current  Saved/Monolith/Benchmarks/AICapability/<next>/summary.json `
    --output-dir Saved/Monolith/Benchmarks/AICapability/compare-v1-vs-next
```

`run` runs a fixture-readiness preflight first and refuses to score if the fixtures are missing
(use `--skip-preflight` only as a compatibility escape hatch). One coordinated run per editor boot,
after a fresh `setup_fixtures`, is the intended cadence (heavy mutation under `-nullrhi` is a known
instability).

## Expected shape of a healthy first run

The composite is `ai_capability_score`. The seven dimensions, in scored order, are
`edit_execute_rate, error_path_rate, compile_gate_rate, duplicate_reject_rate, edit_schema_rate,
discovery_rate, read_schema_rate`. `summary.json` also carries a `subsystem_breakdown`
(blackboard / behavior_tree / eqs over the adversarial rows; StateTree has no adversarial rows on
this `WITH_STATETREE=0` build) so a regression localized to one subsystem is visible. Per the ROI
report this is preventive insurance on a large cold surface; the value is that any future `ai`
capability regression (a broken edit, a swallowed bad identifier, a lost duplicate guard, a validator
that stops flagging empty trees, or one that stops passing clean trees) drops the headline
immediately.

## Run history

| Date | Label | ai_capability_score | Notes |
| --- | --- | --- | --- |
| 2026-06-18 | (exploratory) | 0.886 | 3 failures, all StateTree stub tasks (`WITH_STATETREE=0`); since removed/replaced with Behavior Tree validate tasks |
| 2026-06-18 | (none) | — | Offline generate + 14/14 self-test for the post-fix 73-task suite |
| 2026-07-11 | `baseline-20260711d` | 1.0 | First fully-scored 74-task live run before the seven-path cleanup contract |
| 2026-07-11 | `run-20260711-final-clean` | 0.966 | Concurrent adversarial diagnostic; unrelated `asset.delete_assets` GC removed an unsaved Blackboard before readback |
| 2026-07-11 | `run-20260711-final-isolated` | **1.0** | Canonical 74/74 final after GC and cleanup fixes; 0 transport failures, stable identity, all scratch absent |
