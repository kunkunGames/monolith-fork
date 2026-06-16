# Schema Completeness Benchmark Results

Placeholder — run the benchmark to populate this file.

## Running a Baseline Scan

```powershell
python Plugins\Monolith\Scripts\schema_completeness_benchmark.py scan `
  --mcp-url http://localhost:9316/mcp `
  --output-dir Saved\Monolith\Benchmarks\SchemaCompleteness\run-001 `
  --label "baseline"
```

Then record results here following the format below.

---

## Result Template

| Run | Label | Date | Actions | Failed | schema_completeness_score |
| --- | --- | --- | --- | --- | --- |
| run-001 | baseline | YYYY-MM-DD | 1766 | 0 | 0.0000 |

Detailed namespace breakdowns are stored in each run's `namespace_breakdown.json`.
Comparison reports between runs are stored in `comparison.md` under the compare
output directory.
