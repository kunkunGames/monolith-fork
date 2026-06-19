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
| `tasks.jsonl` | Static seed fixture set covering all five task categories. |
| `manifest.json` | Catalog metadata and score formula. |
| `METRICS.md` | Metric definitions, score formula, and interpretation. |
| `RESULTS.md` | Latest checked-in benchmark result summary. |
| `Plugins/Monolith/Scripts/project_index_benchmark.py` | Generator, runner, and comparison tool. |
| `Plugins/Monolith/Scripts/test_project_index_benchmark.py` | Offline scoring unit test (no live editor needed). |

Checked-in corpus size: **330 tasks**.

## Task Categories

| Category | Request type | Scored evidence |
| --- | --- | --- |
| `asset_search` | `project_query` with `action=search` and various query strings | Valid non-error response (lenient for `min_results:0`; strict `>=1` result for `require_results`); results checked for `match_object_path`, `match_value`, `match_source`. |
| `gameplay_tag_lookup` | `project_query` with `action=list_gameplay_tags` or `action=search_gameplay_tags` | Valid non-error response (lenient: empty tag list OK if response is valid JSON). |
| `known_answer` | `project_query` with `action=search` for a distinctive asset name | HIT only when the response's `match_object_path` values contain the fixture's exact `/Game` `expected_object_path` AND `min_results>=1`. Ground-truth recall an empty/broken index cannot fake. |
| `health_check` | `project_query` with `action=health` | Response contains `status` field; checked for stale/error/degraded state. |
| `stats_check` | `project_query` with `action=get_stats` | Response has `success=true`, `indexing` present, and `stats` as an object. |
| `schema_field_presence` | `monolith_discover` with `namespace=project`, `mode=schema` | Schema has `planning_signals` (non-empty list) and `skill` (non-empty string). |

All tasks are read-only and safe to run against any live Monolith MCP endpoint.

## Empty / Broken Index Guard

An empty or broken index used to score a perfect `1.000`. Now:

- `known_answer` tasks require the index to actually return the expected `/Game` asset
  path, so an empty index scores `known_answer_hit_rate = 0.0`.
- `field_completeness_rate` is computed over expected-nonempty tasks and is `0.0`
  (not a vacuous `1.0`) when those tasks return no rows.
- When every result-bearing task returns zero results the run is flagged
  `all_empty=true`, a `[ALL-EMPTY]` warning is printed to stderr, and
  `project_index_score` is capped at `0.30`.

## Primary Score

`project_index_score = 0.25 * search_hit_rate + 0.20 * known_answer_hit_rate + 0.20 * field_completeness_rate + 0.15 * schema_adherence_rate + 0.10 * (1 - stale_rate) + 0.10 * error_free_rate`

## Score Dimensions

| Dimension | Weight | Direction | Description |
| --- | ---: | --- | --- |
| `search_hit_rate` | 0.25 | higher is better | Fraction of asset_search and gameplay_tag_lookup tasks returning a valid non-error response (strict `>=1` result for `require_results` rows). |
| `known_answer_hit_rate` | 0.20 | higher is better | Fraction of known_answer tasks whose response contained the expected `/Game` object path. |
| `field_completeness_rate` | 0.20 | higher is better | Fraction of returned result items with at least 2 of 3 required fields (`match_object_path`, `match_value`, `match_source`), over expected-nonempty tasks. `0.0` (not vacuous) when those tasks return no rows. |
| `schema_adherence_rate` | 0.15 | higher is better | Fraction of schema_field_presence tasks where the discovered schema contains non-empty `planning_signals`. |
| `stale_rate` | 0.10 | lower is better | Fraction of health_check tasks where the response reports stale, error, or degraded status. |
| `error_free_rate` | 0.10 | higher is better | Fraction of all scored rows without transport errors or MCP `isError` responses. |

## Generate

```powershell
python Plugins\Monolith\Scripts\project_index_benchmark.py generate `
  --tasks Benchmarks\ProjectIndex\tasks.jsonl `
  --manifest Benchmarks\ProjectIndex\manifest.json
```

The generator produces 330 static tasks: 176 `asset_search`, 67
`gameplay_tag_lookup`, 30 `known_answer`, 16 `health_check`, 1 `stats_check`, and 40
`schema_field_presence`.

## Offline Scoring Test

The scoring branches (including the `known_answer` HIT/MISS logic and the empty-index
guard) have an offline unit test that needs no live editor or network:

```powershell
python Plugins\Monolith\Scripts\test_project_index_benchmark.py
```

It monkeypatches `mcp_call` with fabricated MCP responses and asserts an all-empty
mock run scores `<= 0.30` (capped, `all_empty=true`) while a healthy mock run scores
above `0.9`.

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
