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
- `per_action.jsonl` -- one line per action with label, status, diff_count, warning_count, error
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
SKIP with a `tool not found: exe` reason. No actions are comparable, so
`match_rate`, `diff_rate`, and `error_rate` are all 0.0. The score formula
evaluates to `0.20 * (1-0) + 0.20 * (1-0) = 0.40` (the diff and error penalty
terms are absent but the match contribution is also absent). This allows the
benchmark to run in environments where only the Python reference tool is
present, while making the missing exe visibly absent from the score trend.
