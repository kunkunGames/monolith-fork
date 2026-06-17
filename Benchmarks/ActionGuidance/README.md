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

Checked-in corpus size: **508 tasks**. The live catalog generator appends a
fixed 100-task Unreal practical supplement after catalog-derived tasks so the
domain cases survive regeneration.

## Task Categories

| Category | Request type | Scored evidence |
| --- | --- | --- |
| `discovery_planning` | `monolith_discover(..., mode=schema)` | `planning_signals`, `skill`, explicit `output_contract_status`, explicit `next_actions_status`. |
| `unknown_action_recovery` | Typoed namespace action | `failure_cause=unknown_action`, `retryability`, `candidate_actions`. |
| `missing_required_param` | Read-only action called without a required param | `missing_required_params` or `required_params`, plus failure cause. |
| `invalid_param_type` | Read-only action called with a deliberately wrong type | `validation_errors`, `failure_cause=invalid_param`, `retryability`. |

All handler-facing negative tasks are selected from actions that the current
catalog marks as read-only. They should fail during lookup or schema validation
before mutating editor/project state.

## Generate

```powershell
python Plugins\Monolith\Scripts\action_guidance_benchmark.py generate `
  --mcp-url http://localhost:9316/mcp `
  --min-tasks 120 `
  --tasks Plugins\Monolith\Benchmarks\ActionGuidance\tasks.jsonl `
  --manifest Plugins\Monolith\Benchmarks\ActionGuidance\manifest.json
```

## Run

```powershell
python Plugins\Monolith\Scripts\action_guidance_benchmark.py run `
  --mcp-url http://localhost:9316/mcp `
  --tasks Plugins\Monolith\Benchmarks\ActionGuidance\tasks.jsonl `
  --label current `
  --output-dir Saved\Monolith\Benchmarks\ActionGuidance\current `
  --request-timeout-s 8
```

Each run writes `summary.json`, `per_task.json`, incremental
`per_task.jsonl`, and `partial_summary.json`. The incremental files make slow
or non-closing legacy MCP responses visible during a long run.

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
| [SchemaCompleteness](../SchemaCompleteness/README.md) | Scripts/schema_completeness_benchmark.py | schema_completeness_score | Full 1766-action catalog schema quality |
| [OfflineParity](../OfflineParity/README.md) | Scripts/offline_parity_benchmark.py | offline_parity_score | exe-vs-py offline parity trend tracking |
