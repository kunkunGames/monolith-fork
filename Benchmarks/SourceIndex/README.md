# Monolith SourceIndex Quality Benchmark

This benchmark measures whether the Monolith MCP source namespace (C++ symbol
index) returns data that is rich enough for agents to get useful code context.

The benchmark is deterministic.  It does not call an LLM.  Each task sends one
safe MCP request, then the scorer checks whether the response contains the
machine-readable evidence an agent needs to locate symbols, navigate call
graphs, and plan C++ changes.

## Files

| File | Purpose |
| --- | --- |
| `tasks.jsonl` | Static seed fixture set covering all three task categories. |
| `manifest.json` | Catalog metadata, golden symbols, and score formula. |
| `METRICS.md` | Metric definitions, score formula, and interpretation. |
| `RESULTS.md` | Latest checked-in benchmark result summary. |
| `Plugins/Monolith/Scripts/source_index_benchmark.py` | Generator, runner, and comparison tool. |

## Task Categories

| Category | Request type | Scored evidence |
| --- | --- | --- |
| `symbol_lookup` | `source_query` with `action=search_source`, `find_callers`, `find_callees`, or `risk_score` against a golden symbol | At least 1 result returned; each result checked for `name`, `kind`, `file_path`/`location` fields. |
| `health_check` | `source_query` with `action=health` | Response contains `status` plus `symbol_count` or `total_symbols`, and no stale/error flag. |
| `schema_field_presence` | `monolith_discover` with `namespace=source`, `mode=schema` | Schema has `planning_signals`, `skill`, and both `output_contract_status`/`next_actions_status` declared. |

All tasks are read-only and safe to run against any live Monolith MCP endpoint.

## Generate

```powershell
python Plugins\Monolith\Scripts\source_index_benchmark.py generate `
  --mcp-url http://localhost:9316/mcp `
  --min-tasks 30 `
  --tasks Plugins\Monolith\Benchmarks\SourceIndex\tasks.jsonl `
  --manifest Plugins\Monolith\Benchmarks\SourceIndex\manifest.json
```

The generator starts from the static golden-symbol set (6 symbols x 4 actions
= 24 symbol_lookup tasks, 2 health_check tasks, 4 schema tasks = 30 tasks) and
tops up from the live catalog if `--min-tasks` exceeds 30.

## Run

```powershell
python Plugins\Monolith\Scripts\source_index_benchmark.py run `
  --mcp-url http://localhost:9316/mcp `
  --tasks Plugins\Monolith\Benchmarks\SourceIndex\tasks.jsonl `
  --label current `
  --output-dir Saved\Monolith\Benchmarks\SourceIndex\current `
  --request-timeout-s 12
```

Each run writes `summary.json`, `per_task.json`, incremental `per_task.jsonl`,
and `partial_summary.json`.  Progress is printed as:

```
[N/total] task_id success=True direct=True
```

## Compare

```powershell
python Plugins\Monolith\Scripts\source_index_benchmark.py compare `
  --baseline Saved\Monolith\Benchmarks\SourceIndex\baseline\summary.json `
  --current Saved\Monolith\Benchmarks\SourceIndex\current\summary.json `
  --output-dir Saved\Monolith\Benchmarks\SourceIndex\comparison
```

Produces `comparison.json` and `comparison.md` with a Baseline / Current /
Delta table for each metric.
