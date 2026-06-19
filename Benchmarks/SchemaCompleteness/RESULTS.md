# Schema Completeness Benchmark Results

## Status: pending live scan (2026-06-18)

The `value_domain` dimension and the param-less N/A semantics (ROI report A4
items 1 and 2) are wired into the scorer, aggregate, namespace breakdown,
compare, and report paths. A **real baseline for the new formula requires a
live editor scan** and could not be produced offline:

- A full scan issues one `monolith_discover ... mode=schema` call per catalog
  action; that endpoint is editor-only.
- The most recent cached scan
  (`Saved/Monolith/Benchmarks/SchemaCompleteness/ci-audit/per_action.jsonl`,
  185 actions) stores only the previously-computed boolean flags, **not** the
  raw per-action schemas, so `value_domain` cannot be back-computed from it.
- This run did not start the shared editor (single shared editor; live scan is
  intentionally deferred per task constraints).

### Offline validation that WAS performed

- `python Plugins\Monolith\Scripts\tests\test_schema_completeness_value_domain.py`
  — 11/11 pass. Asserts: an untyped/undescribed required param fails
  `value_domain`; a fully-specified action passes; a param-less action is N/A
  (not auto-1.0); param-less rows are excluded from param-gated rates; a
  `value_domain` failure lowers the aggregate; the six weights sum to 1.0.
- Offline replay of the cached `ci-audit` rows through `aggregate_metrics`,
  `build_namespace_breakdown`, and `cmd_report` (the `report` subcommand) ran
  clean with the new `value_domain_rate`, `param_bearing_action_count`, and
  `param_less_action_count` fields present.

## Running the live baseline scan

```powershell
python Plugins\Monolith\Scripts\schema_completeness_benchmark.py scan `
  --mcp-url http://localhost:9316/mcp `
  --output-dir Saved\Monolith\Benchmarks\SchemaCompleteness\baseline-value-domain `
  --label "baseline-value-domain"
```

CI smoke (first 100 actions, ~5 minutes):

```powershell
python Plugins\Monolith\Scripts\schema_completeness_benchmark.py scan `
  --mcp-url http://localhost:9316/mcp `
  --output-dir Saved\Monolith\Benchmarks\SchemaCompleteness\smoke-value-domain `
  --label "smoke-100-value-domain" `
  --max-actions 100
```

If the MCP endpoint is unreachable, bring it up first with
`D:\P4\game\BatchFiles\RunHeadlessEditor.bat` (see `Plugins\Monolith\CLAUDE.md`
§14), wait for `localhost:9316` to listen, then re-run the scan.

Then record results here following the format below.

---

## Result Template

| Run | Label | Date | Actions | Param-bearing | Failed | schema_completeness_score | value_domain_rate |
| --- | --- | --- | --- | --- | --- | --- | --- |
| baseline-value-domain | baseline-value-domain | YYYY-MM-DD | 1766 | ? | 0 | 0.0000 | 0.0000 |

Detailed namespace breakdowns are stored in each run's `namespace_breakdown.json`.
Comparison reports between runs are stored in `comparison.md` under the compare
output directory.
