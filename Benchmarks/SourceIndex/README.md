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
| `tasks.jsonl` | Static seed fixture set covering all seven task categories. |
| `manifest.json` | Catalog metadata, golden symbols, score formula, and run-integrity gates. |
| `METRICS.md` | Metric definitions, score formula, and interpretation. |
| `RESULTS.md` | Latest checked-in benchmark result summary. |
| `Plugins/Monolith/Scripts/source_index_benchmark.py` | Generator, runner, and comparison tool. |

Checked-in corpus size: **376 tasks**.

## Task Categories

| Category | Request type | Scored evidence |
| --- | --- | --- |
| `symbol_lookup` | `source_query` with `action=search_source`, `find_callers`, `find_callees`, or `risk_score` against a golden symbol | At least 1 result returned; each result checked for `name`, `kind`, `file_path`/`location` fields. Tasks marked `require_results` (symbols KNOWN to have a definition / callers / callees) **miss** on an empty or "No direct C++ callers found…" sentinel response — closing the `min_results:0` loophole. |
| `review_context_lookup` | `source_query` with `action=review_context` against engine and gameplay symbols | Review context returns a valid risk seed/top-risk symbol row or a truthful empty response. |
| `impact_radius_lookup` | `source_query` with `action=impact_radius` against gameplay, UI, VFX, and asset symbols | Impact radius returns valid seed/impacted symbol rows or a truthful empty response. |
| `ergonomics_text` | Plain-text source helpers such as `get_include_path`, `get_signature`, `verify_symbols`, and `find_example_usage` | Non-empty, non-error text that an agent can use directly. |
| `negative_recovery` | `source_query` with deliberately bad input — nonexistent symbols, missing required params, unqualified-vs-qualified method names | RESPONSE QUALITY on bad input: a structured error (`isError=true`) that **names the offending identifier** plus a did-you-mean / qualified-symbol / "run `search_source` first" hint scores high; a transport crash or a silent empty success scores `0`. |
| `health_check` | `source_query` with `action=health` | Response contains `status` plus `symbol_count` or `total_symbols`, and no stale/error flag. |
| `schema_field_presence` | `monolith_discover` with `namespace=source`, `mode=schema` | Schema has `planning_signals`, `skill`, and both `output_contract_status`/`next_actions_status` declared. |

All tasks are read-only and safe to run against any live Monolith MCP endpoint.
The `negative_recovery` tasks send malformed input but every action is read-only,
so they never mutate the index.

### Database dependency contract

Canonical `source_query` tasks read `Saved/EngineSource.db` through the source
subsystem, including its internal CRG cache used by risk, review-context, and
impact-radius actions. The remaining graph-specific source actions are exercised
only as `monolith_discover` schema probes; the corpus does not execute their
`Saved/graph.db` handlers. SourceIndex input evidence therefore fingerprints only
`Saved/EngineSource.db`, and unrelated derived `graph.db` churn cannot stale a run.

## Generate

```powershell
python Plugins\Monolith\Scripts\source_index_benchmark.py generate `
  --mcp-url http://localhost:9316/mcp `
  --min-tasks 363 `
  --tasks Benchmarks\SourceIndex\tasks.jsonl `
  --manifest Benchmarks\SourceIndex\manifest.json
```

The generator is deterministic but intentionally catalog-bound: it always performs
read-only `monolith_status` + complete paginated `source` discovery + a second
`monolith_status`, and refuses to publish if the catalog changes mid-generation or
if a curated action no longer exists. It never invents generic `query` parameters
for newly discovered actions. Instead, each live source action not already referenced
by the curated corpus receives one exact `monolith_discover(mode=schema)` task in
sorted action-name order. If the resulting corpus is still below `--min-tasks`,
generation fails and requires schema-verified curated tasks.

The current catalog generates 376 tasks: 217 `symbol_lookup` (of which 28 carry
`require_results`), 32 `review_context_lookup`, 32 `impact_radius_lookup`, 40
`ergonomics_text`, 13 `negative_recovery`, 7 `health_check`, and 35
`schema_field_presence` tasks. The 23 appended schema tasks close live source action
identity coverage from 14/37 to 37/37. All 353 curated tasks remain in order; the old
10 generator top-ups that guessed a generic `query` parameter are replaced by exact
schema discovery and no longer distort execution scoring.

## Run

```powershell
python Plugins\Monolith\Scripts\source_index_benchmark.py run `
  --mcp-url http://localhost:9316/mcp `
  --tasks Plugins\Monolith\Benchmarks\SourceIndex\tasks.jsonl `
  --label current `
  --output-dir Saved\Monolith\Benchmarks\SourceIndex\current `
  --request-timeout-s 12
```

A valid completed run writes `summary.json`, `per_task.json`, and incremental
`per_task.jsonl`. Progress is printed as:

```
[N/total] task_id success=True direct=True
```

### Run-integrity contract

`run` strictly validates `monolith_status` before the first task. Invalid JSON, a non-object JSON
envelope, a top-level JSON-RPC error, a missing MCP result, `isError` status, or a payload that does
not declare `server_running=true` writes `run_failure.json`, writes no `summary.json`, and exits
non-zero. Known run outputs are removed first so an invalid rerun cannot expose a stale success.

A canonical run also requires the non-empty `manifest.json` `catalog_version` to exactly match the
live status `catalog_version`. A missing or stale manifest identity aborts before the first benchmark
task call and writes only invalid-run artifacts; regenerate the canonical corpus against the stable
live catalog instead of editing the version by hand. Explicit subsets remain non-comparable
diagnostics and do not claim this canonical identity binding.

Each task row distinguishes a transport failure (`transport_error`, HTTP status, bounded raw text)
from an invalid MCP/JSON-RPC envelope (`protocol_error`). A valid semantic `result.isError=true`
remains scoreable for `negative_recovery`; protocol failures are never allowed through the
empty-result or plain-text success branches and invalidate the run.

The shared transport gate aborts after three consecutive transport-failed tasks or when failures
exceed 5% after 20 attempted tasks. Exactly 5% is allowed. `finalize()` applies the same fraction
budget to a completed corpus shorter than 20 tasks.

| Output | Contract |
| --- | --- |
| `summary.json` | Valid complete run only; includes `run_valid=true`, `metrics_valid=true`, completion status, and transport counters |
| `per_task.json` | Valid complete run only |
| `per_task.jsonl` | Incremental task diagnostics, including the triggering invalid row |
| `partial_summary.json` | In-progress or invalid-run diagnostics; removed after valid completion |
| `run_failure.json` | Invalid status, protocol, transport-budget, or runner-exception record; mutually exclusive with `summary.json` |

Offline contract verification is `python Scripts/test_source_index_benchmark.py` (41 scorer and
run-integrity test functions; no editor or live MCP endpoint).

## Compare

```powershell
python Plugins\Monolith\Scripts\source_index_benchmark.py compare `
  --baseline Saved\Monolith\Benchmarks\SourceIndex\baseline\summary.json `
  --current Saved\Monolith\Benchmarks\SourceIndex\current\summary.json `
  --output-dir Saved\Monolith\Benchmarks\SourceIndex\comparison
```

Produces `comparison.json` and `comparison.md` with a Baseline / Current /
Delta table for each metric.
