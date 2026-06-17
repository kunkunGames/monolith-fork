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
| `tasks.jsonl` | Static seed fixture set covering all six task categories. |
| `manifest.json` | Catalog metadata, golden symbols, and score formula. |
| `METRICS.md` | Metric definitions, score formula, and interpretation. |
| `RESULTS.md` | Latest checked-in benchmark result summary. |
| `Plugins/Monolith/Scripts/source_index_benchmark.py` | Generator, runner, and comparison tool. |

Checked-in corpus size: **319 tasks**.

## Task Categories

| Category | Request type | Scored evidence |
| --- | --- | --- |
| `symbol_lookup` | `source_query` with `action=search_source`, `find_callers`, `find_callees`, or `risk_score` against a golden symbol | At least 1 result returned; each result checked for `name`, `kind`, `file_path`/`location` fields. |
| `review_context_lookup` | `source_query` with `action=review_context` against engine and gameplay symbols | Review context returns a valid risk seed/top-risk symbol row or a truthful empty response. |
| `impact_radius_lookup` | `source_query` with `action=impact_radius` against gameplay, UI, VFX, and asset symbols | Impact radius returns valid seed/impacted symbol rows or a truthful empty response. |
| `ergonomics_text` | Plain-text source helpers such as `get_include_path`, `get_signature`, `verify_symbols`, and `find_example_usage` | Non-empty, non-error text that an agent can use directly. |
| `health_check` | `source_query` with `action=health` | Response contains `status` plus `symbol_count` or `total_symbols`, and no stale/error flag. |
| `schema_field_presence` | `monolith_discover` with `namespace=source`, `mode=schema` | Schema has `planning_signals`, `skill`, and both `output_contract_status`/`next_actions_status` declared. |

All tasks are read-only and safe to run against any live Monolith MCP endpoint.

## Generate

```powershell
python Plugins\Monolith\Scripts\source_index_benchmark.py generate `
  --mcp-url http://localhost:9316/mcp `
  --min-tasks 319 `
  --tasks Benchmarks\SourceIndex\tasks.jsonl `
  --manifest Benchmarks\SourceIndex\manifest.json
```

The generator starts from deterministic source-index fixtures and includes the
checked-in practical Unreal extension: 192 `symbol_lookup`, 33
`review_context_lookup`, 33 `impact_radius_lookup`, 40 `ergonomics_text`, 9
`health_check`, and 12 `schema_field_presence` tasks.

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
