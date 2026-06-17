# Monolith Offline Parity Benchmark

This benchmark elevates `Scripts/verify_offline_parity.py` from a CI pass/fail
gate into a scored benchmark that tracks exe-vs-py parity quality over time.

## Relationship to verify_offline_parity.py

| | verify_offline_parity.py | offline_parity_benchmark.py |
| --- | --- | --- |
| Role | CI hard-gate | Trend tracking |
| Output | Exit code 0/1 | `summary.json`, `per_action.jsonl`, `comparison.md` |
| Pass criterion | Zero diffs, zero errors, version match | Scored `offline_parity_score` (0.0–1.0) |
| Used in | CI pipeline | Regression detection across builds |

The verify script is the authoritative CI signal. This benchmark adds a numeric
score so regressions can be detected by magnitude before they become hard
failures: a score drop shows *where* parity is degrading even while individual
actions might still pass.

Current coverage:

- `verify_offline_parity.py`: 137 hard-gate actions. CI still requires zero
  diffs, zero real errors, and matching `parity_spec_rev`.
- `offline_parity_benchmark.py`: 317 scored actions: 96 `cppreflect`, 46
  `network`, 50 `decision`, 62 `risk`, and 63 `source` cases. The benchmark
  includes 100 benchmark-only cases beyond the prior 217-action table, plus
  5 expected-error negative source cases that are tracked separately from real
  tool failures.

For CI thresholding, keep the hard-gate as the binary pass/fail check. Use the
benchmark as a trend gate: healthy runs should stay at or above `0.95`, with
`real_error_count == 0` and `expected_error_problem_count == 0`.

## Files

| File | Purpose |
| --- | --- |
| `README.md` | This file. Overview, usage, and relationship to the CI gate. |
| `METRICS.md` | Metric definitions, score formula, and interpretation guidance. |
| `RESULTS.md` | Latest checked-in benchmark result summary and evidence paths. |
| `Scripts/offline_parity_benchmark.py` | Standalone benchmark runner (run / compare / report). |

## Run

```powershell
python Plugins\Monolith\Scripts\offline_parity_benchmark.py run `
  --label current `
  --output-dir Saved\Monolith\Benchmarks\OfflineParity\current
```

With lenient cursor comparison (mirrors `--ignore-cursor-bytes` from the CI gate):

```powershell
python Plugins\Monolith\Scripts\offline_parity_benchmark.py run `
  --label current `
  --output-dir Saved\Monolith\Benchmarks\OfflineParity\current `
  --ignore-cursor-bytes
```

Override tool paths if needed:

```powershell
python Plugins\Monolith\Scripts\offline_parity_benchmark.py run `
  --label v1.2.3 `
  --exe-path Binaries\monolith_query.exe `
  --py-path Scripts\monolith_offline.py `
  --output-dir Saved\Monolith\Benchmarks\OfflineParity\v1.2.3
```

Each run writes:
- `summary.json` -- aggregate metrics including `offline_parity_score`
- `per_action.jsonl` -- one line per action with label, status, diff_count, warning_count, error, expected_error, error_kind, exe_exit_code, py_exit_code
- `partial_summary.json` -- updated every 5 actions for progress visibility

## Compare

```powershell
python Plugins\Monolith\Scripts\offline_parity_benchmark.py compare `
  --baseline Saved\Monolith\Benchmarks\OfflineParity\baseline\summary.json `
  --current  Saved\Monolith\Benchmarks\OfflineParity\current\summary.json `
  --output-dir Saved\Monolith\Benchmarks\OfflineParity\comparison
```

Writes `comparison.json` and `comparison.md` (Markdown delta table).

## Report

```powershell
python Plugins\Monolith\Scripts\offline_parity_benchmark.py report `
  --summary Saved\Monolith\Benchmarks\OfflineParity\current\summary.json
```

Prints a human-readable summary of a single run to stdout.

## Missing exe handling

If `Binaries/monolith_query.exe` does not exist, all actions are reported as
SKIP with a `tool not found: exe` reason. No actions are comparable, so the
primary `offline_parity_score` is forced to `0.0` instead of awarding partial
credit from absent diff/error rates. This allows the benchmark to run in
environments where only the Python reference tool is present, while making the
missing exe visibly absent from the score trend.
