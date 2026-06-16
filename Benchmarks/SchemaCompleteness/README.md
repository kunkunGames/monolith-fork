# Monolith Schema Completeness Benchmark

Tests full catalog coverage of schema quality across all 51 namespaces and
1766 actions.  Unlike the ActionGuidance benchmark (which samples 161 tasks),
SchemaCompleteness calls `monolith_discover` once per action and scores five
quality dimensions for every action in the live catalog.

## Files

| File | Description |
| --- | --- |
| `README.md` | This document |
| `METRICS.md` | Metric definitions and score formula |
| `RESULTS.md` | Baseline and historical scan results |
| `manifest.json` | Static seed manifest with expected namespace/action counts and scoring formula |

## Quality Dimensions

| Dimension | What it measures | Weight in score |
| --- | --- | --- |
| `param_types_declared` | All parameters in the schema have a `"type"` field | 0.30 |
| `required_params_marked` | At least one parameter carries `"required": true`, OR the action has no parameters | 0.25 |
| `planning_signals_present` | `"planning_signals"` key exists and is a non-empty list | 0.20 |
| `skill_routing_present` | `"skill"` key exists and is a non-empty string | 0.15 |
| `output_contract_declared` | `"output_contract_status"` is explicitly `"declared"` or `"not_declared"` (not absent) | 0.10 |

## Running the Benchmark

### Scan (full catalog)

```powershell
python Plugins\Monolith\Scripts\schema_completeness_benchmark.py scan `
  --mcp-url http://localhost:9316/mcp `
  --output-dir Saved\Monolith\Benchmarks\SchemaCompleteness\run-001 `
  --label "v0.21.0-baseline"
```

### Scan (smoke test — first 100 actions)

Recommended for CI because a full scan takes ~1 call per action x 1766 actions.

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
| `summary.json` | Aggregate metrics including `schema_completeness_score` and per-namespace breakdown |
| `per_action.jsonl` | One JSON line per action with all 5 quality flags and `schema_score` |
| `partial_summary.json` | Updated every 25 actions for progress monitoring during long scans |
| `namespace_breakdown.json` | Per-namespace score breakdown (also embedded in `summary.json`) |

## Output Files (compare)

| File | Description |
| --- | --- |
| `comparison.json` | Full baseline + current + delta objects |
| `comparison.md` | Markdown table: Metric | Baseline | Current | Delta |

## Performance Note

A full scan issues one `monolith_discover` call per action.  At 1766 actions and
a default 8-second timeout this can take 10-30 minutes depending on editor load.
Use `--max-actions 100` for a quick CI smoke run.  The `partial_summary.json`
file is refreshed every 25 actions so progress is visible during long scans.
