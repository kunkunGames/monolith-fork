# Monolith Action Guidance Benchmark

This benchmark measures whether Monolith MCP gives agents enough factual
planning and failure-recovery evidence without relying on broad hand-authored
`outputs` or `next_actions` metadata.

The benchmark is deterministic. It does not call an LLM. Each task sends one
safe MCP request, then the scorer checks whether the response contains the
machine-readable evidence an agent needs to recover or plan.

## Files

| File | Purpose |
| --- | --- |
| `tasks.jsonl` | Reusable benchmark task set generated from the live catalog. |
| `manifest.json` | Catalog/task-set metadata, namespace coverage, and score formula. |
| `METRICS.md` | Metric definitions and interpretation. |
| `RESULTS.md` | Latest checked-in benchmark result summary and evidence paths. |
| `Plugins/Monolith/Scripts/action_guidance_benchmark.py` | Generator, runner, and comparison tool. |

Checked-in corpus size: **454 tasks** over the 2026-07-17 verified generation
snapshot (`sha256:32d46c1de26877a1`, 61 namespaces / 1857 actions). The generator parses compact
`structuredContent`, enumerates every namespace through paginated
`mode=actions` with `detail=true`, and rejects namespace or total action-count
mismatches before writing the corpus. Each detailed namespace page also fills
the schema cache used to select safe missing/invalid-param probes; the generator
does not repeat a focused schema request for every representative action. It
then appends a
deduplicated Unreal practical supplement after catalog-derived tasks so the
domain cases survive regeneration without keeping repeated task fingerprints.

Curated action assumptions are validated against the live action and parameter
schemas before either canonical file is written. The 2026-07-17 legacy-action
migration table is also joined across the C++ registry seed, source snapshot,
Python generator, and canonical task corpus by the offline contract test. A
renamed target, reintroduced retired endpoint, duplicate alias, or missing
migration probe therefore blocks static CI instead of silently weakening the
benchmark.

## Task Categories

| Category | Request type | Scored evidence |
| --- | --- | --- |
| `discovery_planning` | `monolith_discover(..., mode=schema)` | `planning_signals`, `skill`, explicit `output_contract_status`, explicit `next_actions_status`. |
| `needed_action_routing` | `monolith_find(query=...)` for a vague/typoed action, or `monolith_discover` for an absent action | A routing candidate (`matches[].action_id`, `candidate_actions`, or `did_you_mean`) that names the REAL action_id. A bare no-candidate error scores LOW. |
| `unknown_action_recovery` | Typoed namespace action | `failure_cause=unknown_action`, `retryability`, `candidate_actions`. |
| `missing_required_param` | Read-only action called without a required param | `missing_required_params` or `required_params`, plus failure cause. |
| `invalid_param_type` | Read-only action called with a deliberately wrong type | `validation_errors`, `failure_cause=invalid_param`, `retryability`. |

`needed_action_routing` is the discovery side of the largest live-ROI bucket
(`needed_action`). It asserts that when an agent does not know the exact action
name, `monolith_find` (or a structured `monolith_discover` hint) names the real
action rather than dead-ending on a bare `Unknown action` error. It feeds
`action_selection_accuracy` and `first_recovery_success_rate`. It never executes
a mutating handler: every routing request is read-only discovery or a
before-handler lookup failure.

All other handler-facing negative tasks are selected from actions that the
current catalog marks as read-only. They should fail during lookup or schema
validation before mutating editor/project state.

## Demand Weighting

Each task carries a `weight` field derived from the invocation-log analyzer
Action Stats (`Saved/Monolith/LogAnalysis/<run>/summary.md`):

```text
weight = 1.0 + log10(1 + count * error_rate)   (for documented high-volume /
                                                 high-error actions; else 1.0)
```

The runner combines every sub-metric with a demand-weighted mean, so a
294-call / 47%-error action (`blueprint.add_variable`) moves the aggregate far
more than a dead 10-call action. The static demand snapshot lives in
`_ACTION_STATS_20260618` inside `action_guidance_benchmark.py`; refresh it from
the latest Action Stats when the demand profile shifts. The
`effectiveness_score` component weights still sum to 1.0 — only the per-task
averaging inside each component is weighted.

The offline unit test for both additions is
`Plugins/Monolith/Scripts/tests/test_action_guidance_routing_weight.py`.

## Generate

```powershell
python Plugins\Monolith\Scripts\action_guidance_benchmark.py generate `
  --mcp-url http://localhost:9316/mcp `
  --min-tasks 454 `
  --tasks Plugins\Monolith\Benchmarks\ActionGuidance\tasks.jsonl `
  --manifest Plugins\Monolith\Benchmarks\ActionGuidance\manifest.json
```

Generation fingerprints `monolith_status` and the compact discovery summary at
both the start and end. Their `catalog_version`, action count, and namespace
count must agree throughout the run; otherwise generation fails before
overwriting either `tasks.jsonl` or `manifest.json`. A successful manifest
records the verified `catalog_version` used to build the corpus. Bulk schema
projection remains paginated at the server's bounded 50-row detail limit, so
large namespaces such as `ai` and `animation` retain complete coverage without
the former hundreds of redundant request/response round trips.

## Run

```powershell
python Plugins\Monolith\Scripts\action_guidance_benchmark.py run `
  --mcp-url http://localhost:9316/mcp `
  --tasks Plugins\Monolith\Benchmarks\ActionGuidance\tasks.jsonl `
  --label current `
  --output-dir Saved\Monolith\Benchmarks\ActionGuidance\current `
  --request-timeout-s 8
```

Each valid completed run writes `summary.json`, `per_task.json`, incremental
`per_task.jsonl`, and `partial_summary.json`. The incremental files make slow
or non-closing legacy MCP responses visible during a long run.

The runner validates `monolith_status` before the first task and aborts without
`summary.json` after three consecutive transport failures (configurable with
`--max-consecutive-transport-failures`) or when the transport-failure fraction
exceeds 5% after 20 tasks (configurable with
`--max-transport-failed-fraction` and `--min-transport-fraction-sample`). A
completed short run applies the same fraction budget at finalization. An invalid
run writes `run_failure.json`
plus the last partial evidence and exits non-zero, so an editor shutdown cannot
be recorded as a low benchmark baseline or consume the rest of the corpus in
timeout retries.
Transport accounting covers every MCP request made by a task, including
deterministic discovery/schema recovery calls; a healthy initial response can
therefore no longer hide a failed recovery request.

## Compare

```powershell
python Plugins\Monolith\Scripts\action_guidance_benchmark.py compare `
  --baseline Saved\Monolith\Benchmarks\ActionGuidance\legacy\summary.json `
  --current Saved\Monolith\Benchmarks\ActionGuidance\current\summary.json `
  --output-dir Saved\Monolith\Benchmarks\ActionGuidance\comparison
```

## Related Benchmarks

| Benchmark | Script | Primary Score | What it tests |
| --- | --- | --- | --- |
| [SourceIndex](../SourceIndex/README.md) | Scripts/source_index_benchmark.py | source_index_score | C++ symbol index data quality and recall |
| [SchemaCompleteness](../SchemaCompleteness/README.md) | Scripts/schema_completeness_benchmark.py | schema_completeness_score | Full live-catalog schema quality plus a strict 330-probe contract |
| [OfflineParity](../OfflineParity/README.md) | Scripts/offline_parity_benchmark.py | offline_parity_score | exe-vs-py offline parity trend tracking |
