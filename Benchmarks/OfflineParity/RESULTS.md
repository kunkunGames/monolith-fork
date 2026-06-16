# Offline Parity Benchmark Results

No benchmark runs have been recorded yet.

Run the benchmark and update this file with the results:

```powershell
python Plugins\Monolith\Scripts\offline_parity_benchmark.py run `
  --label current `
  --output-dir Saved\Monolith\Benchmarks\OfflineParity\current
```

## Results Table Format

When a result is available, replace this section with a table in the following
format:

| Metric | Value |
| --- | --- |
| `offline_parity_score` | |
| `match_rate` | |
| `diff_rate` | |
| `error_rate` | |
| `skip_rate` | |
| `version_parity_score` | |
| `total_actions` | |
| `comparable_actions` | |
| `mean_diffs_per_diff_action` | |

## Evidence

| Artifact | Path |
| --- | --- |
| Run output | `Saved\Monolith\Benchmarks\OfflineParity\<label>` |
| Per-action detail | `Saved\Monolith\Benchmarks\OfflineParity\<label>\per_action.jsonl` |
| Comparison | `Saved\Monolith\Benchmarks\OfflineParity\<comparison-dir>\comparison.md` |
