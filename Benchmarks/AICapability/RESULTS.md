# AICapability Results

## Status

| Item | State |
| --- | --- |
| Runner | `Scripts/ai_capability_benchmark.py` — implemented, `generate` verified offline |
| Tasks / manifest | `tasks.jsonl` (73 tasks) + `manifest.json` generated; weights sum to 1.0; integrity checks pass; every `ai` action verified against `_ai_catalog.txt` |
| Offline self-test | `Scripts/test_ai_capability_benchmark.py` — **14/14 passed** (scoring branches validated against fabricated MCP responses) |
| Live scored run | One exploratory run scored **0.886** (pre-`WITH_STATETREE=0` fix); the 3 failures were all StateTree stub tasks since removed/replaced (see below). Next scored run pending the shared editor |

A scored run mutates AI assets and needs the live MCP endpoint (the single shared headless editor),
left to a coordinated session to avoid colliding with other editor work.

## Offline validation performed (2026-06-18)

```powershell
# from D:\P4\game\Plugins\Monolith
python Scripts/ai_capability_benchmark.py generate
#   -> task_count 73; category_counts {compile_gate:2, discovery:6, duplicate_reject:4,
#      edit_execute:9, edit_schema:28, error_path:8, read_schema:16}
#   -> weights sum to 1.0; score_formula rendered; integrity checks pass
#   -> every ai action in tasks.jsonl exists in Saved/Monolith/LogAnalysis/_ai_catalog.txt

python Scripts/test_ai_capability_benchmark.py
#   -> 14/14 passed:
#      compile_gate negative (validate_invalid): stub-always-valid FAILS, empty-BT valid==false+error issue PASSES,
#         valid==false-without-error-issue FAILS, isError-not-a-verdict FAILS
#      compile_gate positive (validate_valid): stub-always-invalid FAILS, real valid==true PASSES
#      edit_execute: silent-no-op read-back FAILS, observed-mutation PASSES
#      error_path: generic reject-all FAILS, names-offending-identifier PASSES, silent-success FAILS
#      duplicate_reject: silent-second-success FAILS, second-isError PASSES
#      aggregate: healthy run -> 1.0, broken run -> 0.0
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
D:\P4\game\BatchFiles\RunHeadlessEditor.bat        # or: Scripts\recover_mcp.ps1
#    wait for localhost:9316 to listen, confirm with monolith_status()

# from D:\P4\game\Plugins\Monolith
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
| 2026-06-18 | (none) | — | Offline generate + 14/14 self-test for the post-fix 73-task suite; next live scored run pending the shared editor |
