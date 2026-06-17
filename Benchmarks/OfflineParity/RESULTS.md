# Offline Parity Benchmark Results

## 2026-06-17 Definition Snapshot

No full exe-vs-py benchmark result is checked in yet. This update expands and
verifies the benchmark definition and report schema.

| Item | Value |
| --- | ---: |
| Benchmark actions | 317 |
| Added in this update | 100 |
| `cppreflect` actions | 96 |
| `network` actions | 46 |
| `decision` actions | 50 |
| `risk` actions | 62 |
| `source` actions | 63 |
| Expected-error source cases | 5 |
| CI hard-gate actions (`verify_offline_parity.py`) | 137 |

## CI Threshold View

| Gate | Threshold |
| --- | --- |
| Hard gate | `verify_offline_parity.py` passes: zero diffs, zero real errors, matching `parity_spec_rev`. |
| Benchmark trend gate | `offline_parity_score >= 0.95`, `real_error_count == 0`, `expected_error_problem_count == 0`. |
| Regression investigation | `offline_parity_score < 0.85` or a drop greater than `0.05` from the previous checked run. |

## Latest Verification

| Check | Result |
| --- | --- |
| Python compile | `python -m py_compile Plugins\Monolith\Scripts\offline_parity_benchmark.py Plugins\Monolith\Scripts\verify_offline_parity.py` passed. |
| Action count import | `build_actions(...)` returned 317 actions with unique labels: 96 `cppreflect`, 46 `network`, 50 `decision`, 62 `risk`, 63 `source`. |
| Help smoke | `python Plugins\Monolith\Scripts\offline_parity_benchmark.py --help` and `report --help` passed. |
| Missing-exe smoke run | Ran to completion with 317 SKIP rows and `offline_parity_score = 0.0`, confirming no comparable-actions partial credit. |
| Report smoke | `report --summary <temp>\summary.json` printed `expected_error_rate`, `real_error_rate`, `expected_error_count`, `expected_error_problem_count`, and `real_error_count`. |
| Expected-error sample | The 5 expected-error source cases returned `MATCH expected` with exit code `1` from both exe and py. |
| Representative new-action sample | 6 new filter/API cases across `cppreflect`, `network`, `decision`, `risk`, and `source` returned `MATCH none`. |
| Full 317-action exe-vs-py run | Not run during this update: it would launch both tools for every action, about 634 process invocations, and is better reserved for CI or a dedicated benchmark run. |

## Evidence Paths

| Artifact | Path |
| --- | --- |
| Full run output | `Saved\Monolith\Benchmarks\OfflineParity\<label>` |
| Per-action detail | `Saved\Monolith\Benchmarks\OfflineParity\<label>\per_action.jsonl` |
| Comparison | `Saved\Monolith\Benchmarks\OfflineParity\<comparison-dir>\comparison.md` |
| Smoke run output | `%TEMP%\offline_parity_benchmark_smoke_<timestamp>` |
