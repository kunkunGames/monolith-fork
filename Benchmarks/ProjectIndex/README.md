# ProjectIndex Benchmark

This benchmark measures whether the Monolith MCP project namespace (asset index)
returns data that is rich enough for agents to find assets, look up gameplay tags,
and understand the project structure.

The benchmark is deterministic.  It does not call an LLM.  Each task sends one
safe MCP request, then the scorer checks whether the response contains the
machine-readable evidence an agent needs to locate assets, navigate the project
graph, and plan asset or gameplay changes.

## Files

| File | Purpose |
| --- | --- |
| `tasks.jsonl` | Static seed fixture set covering all four task categories. |
| `manifest.json` | Catalog metadata and score formula. |
| `METRICS.md` | Metric definitions, score formula, and interpretation. |
| `RESULTS.md` | Latest checked-in benchmark result summary. |
| `Plugins/Monolith/Scripts/project_index_benchmark.py` | Generator, runner, and comparison tool. |

## Task Categories

| Category | Request type | Scored evidence |
| --- | --- | --- |
| `asset_search` | `project_query` with `action=search` and various query strings | Valid non-error response (lenient: empty results OK if response is valid JSON); results checked for `match_object_path`, `match_value`, `match_source`. |
| `gameplay_tag_lookup` | `project_query` with `action=list_gameplay_tags` or `action=search_gameplay_tags` | Valid non-error response (lenient: empty tag list OK if response is valid JSON). |
| `health_check` | `project_query` with `action=health` or `action=get_stats` | Response contains `status` field; checked for stale/error/degraded state. |
| `schema_field_presence` | `monolith_discover` with `namespace=project`, `mode=schema` | Schema has `planning_signals` (non-empty list) and `skill` (non-empty string). |

All tasks are read-only and safe to run against any live Monolith MCP endpoint.

## Primary Score

`project_index_score = 0.40 * search_hit_rate + 0.30 * field_completeness_rate + 0.20 * schema_adherence_rate + 0.10 * (1 - stale_rate)`

## Score Dimensions

| Dimension | Weight | Direction | Description |
| --- | ---: | --- | --- |
| `search_hit_rate` | 0.40 | higher is better | Fraction of asset_search and gameplay_tag_lookup tasks returning a valid non-error response. |
| `field_completeness_rate` | 0.30 | higher is better | Fraction of returned result items with at least 2 of 3 required fields (`match_object_path`, `match_value`, `match_source`). Vacuously 1.0 when no results returned. |
| `schema_adherence_rate` | 0.20 | higher is better | Fraction of schema_field_presence tasks where the discovered schema contains non-empty `planning_signals`. |
| `stale_rate` | 0.10 | lower is better | Fraction of health_check tasks where the response reports stale, error, or degraded status. |

## Generate

```powershell
python Plugins\Monolith\Scripts\project_index_benchmark.py generate `
  --tasks Plugins\Monolith\Benchmarks\ProjectIndex\tasks.jsonl `
  --manifest Plugins\Monolith\Benchmarks\ProjectIndex\manifest.json
```

The generator produces 100 static tasks: 50 asset_search, 20 gameplay_tag_lookup,
5 health_check, and 25 schema_field_presence.

## Run

```powershell
python Plugins\Monolith\Scripts\project_index_benchmark.py run `
  --mcp-url http://localhost:9316/mcp `
  --tasks Plugins\Monolith\Benchmarks\ProjectIndex\tasks.jsonl `
  --label current `
  --output-dir Saved\Monolith\Benchmarks\ProjectIndex\current `
  --request-timeout-s 12
```

Each run writes `summary.json`, `per_task.json`, incremental `per_task.jsonl`,
and `partial_summary.json`.  Progress is printed as:

```
[N/total] task_id success=True
```

## Compare

```powershell
python Plugins\Monolith\Scripts\project_index_benchmark.py compare `
  --baseline Saved\Monolith\Benchmarks\ProjectIndex\baseline\summary.json `
  --current Saved\Monolith\Benchmarks\ProjectIndex\current\summary.json `
  --output-dir Saved\Monolith\Benchmarks\ProjectIndex\comparison
```

Produces `comparison.json` and `comparison.md` with a Baseline / Current /
Delta table for each metric.

## See Also

- [Benchmarks/README.md](../README.md) -- master benchmark index
