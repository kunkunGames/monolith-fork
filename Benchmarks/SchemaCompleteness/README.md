# Monolith Schema Completeness Benchmark

Tests full catalog coverage of schema quality across all live Monolith
namespaces and actions. The checked-in manifest records a 51-namespace,
1766-action snapshot; the 2026-06-17 ActionGuidance generation observed 51
namespaces and 1638 actions. Unlike the ActionGuidance benchmark (which samples
263 tasks), SchemaCompleteness calls `monolith_discover` once per action and
scores five quality dimensions for every action in the live catalog. The
checked-in targeted probe set contains **385 probes**.

## Files

| File | Description |
| --- | --- |
| `README.md` | This document |
| `METRICS.md` | Metric definitions and score formula |
| `RESULTS.md` | Baseline and historical scan results |
| `manifest.json` | Static seed manifest with expected namespace/action counts and scoring formula |
| `probe_set.jsonl` | 385 targeted schema probes for practical Unreal agent workflows |

## Quality Dimensions

| Dimension | What it measures | Weight in score |
| --- | --- | --- |
| `param_types_declared` | **Every** parameter has a non-empty `"type"` field (param-gated: N/A for param-less actions) | 0.25 |
| `required_params_marked` | **Every** parameter carries a boolean `"required"` flag (param-gated: N/A for param-less actions) | 0.20 |
| `value_domain` | Every param is typed + described + required-flagged, **and** constrained params document their allowed values (non-empty `enum`, numeric range) (param-gated: N/A for param-less actions) | 0.20 |
| `planning_signals_present` | `"planning_signals"` key exists and is a non-empty list | 0.15 |
| `skill_routing_present` | `"skill"` key exists and is a non-empty string | 0.10 |
| `output_contract_declared` | `"output_contract_status"` is explicitly `"declared"` or `"not_declared"` (not absent) | 0.10 |

The three **param-gated** dimensions describe an action's parameter contract.
They are scored only for actions that declare parameters; a param-less action is
**N/A** on them (excluded from the rate, never auto-scored 1.0). A `monolith_discover`
fetch failure is a hard fail (`False`), not N/A. See `METRICS.md` for the full
N/A semantics and the `value_domain` definition.

## Running the Benchmark

### Scan (full catalog)

```powershell
python Plugins\Monolith\Scripts\schema_completeness_benchmark.py scan `
  --mcp-url http://localhost:9316/mcp `
  --output-dir Saved\Monolith\Benchmarks\SchemaCompleteness\run-001 `
  --label "v0.21.0-baseline"
```

### Scan (smoke test — first 100 actions)

Recommended for CI because a full scan takes one call per live catalog action.

```powershell
python Plugins\Monolith\Scripts\schema_completeness_benchmark.py scan `
  --mcp-url http://localhost:9316/mcp `
  --output-dir Saved\Monolith\Benchmarks\SchemaCompleteness\smoke `
  --label "smoke-100" `
  --max-actions 100
```

### Compare two runs

```powershell
python Plugins\Monolith\Scripts\schema_completeness_benchmark.py compare `
  --baseline Saved\Monolith\Benchmarks\SchemaCompleteness\run-001\summary.json `
  --current  Saved\Monolith\Benchmarks\SchemaCompleteness\run-002\summary.json `
  --output-dir Saved\Monolith\Benchmarks\SchemaCompleteness\compare-001-vs-002
```

### Print namespace report

```powershell
python Plugins\Monolith\Scripts\schema_completeness_benchmark.py report `
  --summary Saved\Monolith\Benchmarks\SchemaCompleteness\run-001\summary.json
```

## Output Files (scan)

| File | Description |
| --- | --- |
| `summary.json` | Aggregate metrics including `schema_completeness_score`, `value_domain_rate`, `param_bearing_action_count`, and per-namespace breakdown |
| `per_action.jsonl` | One JSON line per action with all 6 quality flags (param-gated flags may be `null` = N/A) and `schema_score` |
| `partial_summary.json` | Updated every 25 actions for progress monitoring during long scans |
| `namespace_breakdown.json` | Per-namespace score breakdown (also embedded in `summary.json`) |

## Output Files (compare)

| File | Description |
| --- | --- |
| `comparison.json` | Full baseline + current + delta objects |
| `comparison.md` | Markdown table: Metric | Baseline | Current | Delta |

## Performance Note

A full scan issues one `monolith_discover` call per action. At roughly
1600-1800 live actions and a default 8-second timeout this can take 10-30
minutes depending on editor load. Use `--max-actions 100` for a quick CI smoke
run. The `partial_summary.json` file is refreshed every 25 actions so progress
is visible during long scans.

## Offline Scoring Unit Test

The scoring branches (including `value_domain` and the param-less N/A handling)
have an offline unit test that needs no live editor or network — it feeds
fabricated `monolith_discover` envelopes through the scorer:

```powershell
python Plugins\Monolith\Scripts\tests\test_schema_completeness_value_domain.py
```

It asserts that an action declaring an untyped/undescribed required param now
fails `value_domain`, a fully-specified action passes, a param-less action is
N/A (not auto-1.0), and the six dimension weights still sum to 1.0.
