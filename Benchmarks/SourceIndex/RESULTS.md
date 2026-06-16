# SourceIndex Benchmark Results

Latest verified run against the live `source` namespace (MCP `http://localhost:9316/mcp`, engine `++UE5+Release-5.7`).

## How to populate

```powershell
python Plugins\Monolith\Scripts\source_index_benchmark.py run `
  --mcp-url http://localhost:9316/mcp `
  --tasks Plugins\Monolith\Benchmarks\SourceIndex\tasks.jsonl `
  --label initial `
  --output-dir Saved\Monolith\Benchmarks\SourceIndex\initial `
  --request-timeout-s 12
```

Then copy the key metrics here from `Saved\Monolith\Benchmarks\SourceIndex\initial\summary.json`.

## Results table scaffold

| Run label | Date | source_index_score | symbol_hit_rate | field_completeness_rate | schema_adherence_rate | stale_rate |
| --- | --- | ---: | ---: | ---: | ---: | ---: |
| `fix-final-confirm` | 2026-06-17 | 0.93253 | 0.807229 | 1.0 | 1.0 | 0.0 |

Notes:

- The remaining `symbol_hit_rate` gap is from `find_callers`/`find_callees` tasks that pass class symbols (for example `UGameplayStatics`, `ACharacter`) where those actions resolve functions and correctly return `No function found matching ...`. Those are truthful per-symbol results, not scoring defects.
- `health_check` tasks pass when `status` is healthy; a symbol count is only required for `include_counts=true` requests because the `source health` action only emits `row_counts.symbols` in that mode.
