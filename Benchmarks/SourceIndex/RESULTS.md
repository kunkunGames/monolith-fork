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

| Run label | Date | Formula | source_index_score | symbol_hit_rate | field_completeness_rate | schema_adherence_rate | stale_rate | negative_recovery_rate |
| --- | --- | --- | ---: | ---: | ---: | ---: | ---: | ---: |
| `fix-final-confirm` | 2026-06-17 | 5-term (pre-loophole-fix) | 0.93253 | 0.807229 | 1.0 | 1.0 | 0.0 | n/a |

Notes:

- **Score formula changed (2026-06-18).** The composite is now the 6-term formula in `METRICS.md`/`manifest.json` (`0.30 symbol_hit + 0.20 field_completeness + 0.15 schema_adherence + 0.10 (1-stale) + 0.10 ergonomics + 0.15 negative_recovery`). The `0.93253` row above was produced under the previous 5-term formula and before the empty-response loophole and `negative_recovery` category existed; it is retained for history only and is **not comparable** to runs after this change. A fresh live baseline must be captured and added as a new row.
- Two structural loopholes are now closed: (1) `require_results` lookups (28 curated symbols KNOWN to have a definition / callers / callees) score 0 on an empty or sentinel response, and (2) `field_completeness_rate` is averaged only over those expected-nonempty lookups, so an all-empty run can no longer earn a vacuous `1.0`. The earlier `symbol_hit_rate=0.807` was inflated by empty `find_callers`/`find_callees` returns counting as hits; the new score is expected to drop until the index actually serves call edges for those symbols.
- The new `negative_recovery` category (13 tasks) measures self-correcting behaviour on bad input — `get_include_path`/`get_signature`/`verify_symbols` etc. must return a structured error that names the offending identifier with a did-you-mean / qualified-symbol hint, not a crash or silent empty.
- `health_check` tasks pass when `status` is healthy; a symbol count is only required for `include_counts=true` requests because the `source health` action only emits `row_counts.symbols` in that mode.
