# Offline Parity Benchmark Results

## 2026-07-21 graph-retirement-20260721-08

| Item | Value |
| --- | ---: |
| `offline_parity_score` | **1.0** |
| Actions | 317 total; 311 comparable; 6 decision-chain skips |
| MATCH / DIFF / real ERROR | 311 / 0 / 0 |
| Expected-error cases | 5 (0 problems) |
| `version_parity_score` | 1.0 (`2026-05-29.1` on both implementations) |
| Input fingerprint | `f2acf3584953af0c3cf89414467ec57c81f31fc1c2768bf93dcf65558e5fb9d6` |
| Accepted evidence | `Benchmarks\OfflineParity\accepted\graph-retirement-20260721-08` |

This full run followed the EngineSource Schema v4 migration and health-directed
CRG cache repair and the final portable Query rebuild. Native Query and Python
matched every comparable action after the separate `graph.db` input was
removed; the accepted bundle fingerprints the 4.66 GB `EngineSource.db` plus
the final `monolith_query.exe` and contains no graph-export dependency.

## 2026-07-11 run-20260711-final — post-CRG-repair final

| Item | Value |
| --- | ---: |
| `offline_parity_score` | **1.0** |
| Actions | 317 total; 311 comparable; 6 decision-chain skips |
| MATCH / DIFF / real ERROR | 311 / 0 / 0 |
| Expected-error cases | 5 (0 problems) |
| `version_parity_score` | 1.0 (`2026-05-29.1` on both implementations) |
| Input fingerprint | `92f6717c9d99e528792dbc2215e9cf822b2358b40a386b5336f6c8b67708ae3e` |
| Output | `Saved\Monolith\Benchmarks\OfflineParity\run-20260711-final` |

This explicit final run followed the health-directed EngineSource CRG rebuild.
The C++ `monolith_query.exe` and Python `monolith_offline.py` implementations
matched on every comparable action with zero diffs, zero real errors, and no
environment-blocked rows. The six skips are the decision-ID-dependent rows;
the current database has no decision to chain, so they are non-comparable by
contract rather than failures.

## 2026-07-11 baseline-20260711 run

| Item | Value |
| --- | ---: |
| `offline_parity_score` | **1.0** |
| Actions | 317 (all comparable) |
| MATCH / DIFF / real ERROR | 317 / 0 / 0 |
| Expected-error cases | 5 (0 problems) |
| `version_parity_score` | 1.0 |
| Output | `Saved\Monolith\Benchmarks\OfflineParity\baseline-20260711` |

Run alongside the 2026-07-11 suite-wide baseline refresh (Speed CL 1093,
`Docs/testing/2026-07-11-benchmark-contract-failfast-and-n3-guards.md`).
Consistent with the 2026-07-10 `parity-20260710` run (score 1.0) — the offline
CLI fallback surface remains healthy.

## 2026-06-18 Externalization + offline-unsupported bucket

This update externalizes the action table to `actions.jsonl` + `manifest.json`
(ITEM 1, so the table gets the same hosted-static-CI line-count validation as the
other five benchmarks) and adds the `offline_unsupported` bucket (ITEM 2, so
"both offline tools agree the surface errors" scores as an `expected_offline`
`MATCH` instead of a `real` `ERROR` that masks the real parity signal).

| Item | Value |
| --- | ---: |
| Benchmark actions | 317 |
| `cppreflect` actions | 96 |
| `network` actions | 46 |
| `decision` actions | 50 |
| `risk` actions | 62 |
| `source` actions | 63 |
| Expected-error source cases (`expected_error`) | 5 |
| Offline-unsupported source rows (`offline_unsupported`) | 58 |
| CI hard-gate actions (`verify_offline_parity.py`) | 137 |

## 2026-06-18 Offline Validation

| Check | Result |
| --- | --- |
| Python compile | `python -m py_compile Scripts/offline_parity_benchmark.py` passed. |
| Table load | `load_action_specs()` returned 317 rows; `manifest.json` `action_count`=317 equals the `actions.jsonl` non-empty line count. |
| Loader parity | `build_actions(chain)` reproduces the prior 317 actions, token substitution (`{{uclass}}`/`{{risk_path}}`) and the 6 `decision_id`-dependent SKIP rows. |
| Hosted-static-CI | `check_benchmark_definitions` on the live config returned 0 findings for the new OfflineParity entry (line-count + manifest path + `DEFAULT_ACTIONS`/`DEFAULT_MANIFEST` attrs). |
| Full exe-vs-py run | 311 MATCH / 0 DIFF / 0 ERROR / 6 SKIP, `offline_parity_score=1.0`, identical to the pre-change baseline — externalization is behavior-preserving. |
| Unit test | `python Scripts/test_offline_parity_benchmark.py` — all 8 test functions / 22 assertions passed. Reclassification raises a both-fail run from `0.4167` (real ERROR) to `1.0` (`expected_offline`); a one-sided disagreement still fails as `offline_parity_break`. |

## CI Threshold View

| Gate | Threshold |
| --- | --- |
| Hard gate | `verify_offline_parity.py` passes: zero diffs, zero real errors, matching `parity_spec_rev`. |
| Benchmark trend gate | `offline_parity_score >= 0.95`, `real_error_count == 0`, `expected_error_problem_count == 0`. |
| Regression investigation | `offline_parity_score < 0.85` or a drop greater than `0.05` from the previous checked run. |

## Prior Verification (2026-06-17 definition snapshot)

| Check | Result |
| --- | --- |
| Python compile | `python -m py_compile Plugins\Monolith\Scripts\offline_parity_benchmark.py Plugins\Monolith\Scripts\verify_offline_parity.py` passed. |
| Action count import | `build_actions(...)` returned 317 actions with unique labels: 96 `cppreflect`, 46 `network`, 50 `decision`, 62 `risk`, 63 `source`. |
| Help smoke | `python Plugins\Monolith\Scripts\offline_parity_benchmark.py --help` and `report --help` passed. |
| Missing-exe smoke run | Ran to completion with 317 SKIP rows and `offline_parity_score = 0.0`, confirming no comparable-actions partial credit. |
| Report smoke | `report --summary <temp>\summary.json` printed `expected_error_rate`, `real_error_rate`, `expected_error_count`, `expected_error_problem_count`, and `real_error_count`. |
| Expected-error sample | The 5 expected-error source cases returned `MATCH expected` with exit code `1` from both exe and py. |
| Representative new-action sample | 6 new filter/API cases across `cppreflect`, `network`, `decision`, `risk`, and `source` returned `MATCH none`. |

## Evidence Paths

| Artifact | Path |
| --- | --- |
| Full run output | `Saved\Monolith\Benchmarks\OfflineParity\<label>` |
| Per-action detail | `Saved\Monolith\Benchmarks\OfflineParity\<label>\per_action.jsonl` |
| Comparison | `Saved\Monolith\Benchmarks\OfflineParity\<comparison-dir>\comparison.md` |
| Smoke run output | `%TEMP%\offline_parity_benchmark_smoke_<timestamp>` |
