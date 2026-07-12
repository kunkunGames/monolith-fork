# Schema Completeness Benchmark Results

## 2026-07-11 probe-20260711-valid4 — migrated probe contract completed

The first valid live run of the 330-row schema-v2 probe contract completed
against Monolith 0.20.3 at catalog `sha256:ef52979d7e00a249`. Strict preflight,
status validation, complete catalog enumeration, catalog-version recheck, and
all schema fetches completed in 809 seconds.

| Result | Value |
| --- | ---: |
| Declared probes | 330 |
| Catalog-present / scored | 315 |
| Explicit optional / feature-gated skips | 4 / 11 |
| Stale required probes | 0 |
| Fetch failures / transport failures | 0 / 0 |
| Probe / critical / high pass rate | 1.0000 / 1.0000 / 1.0000 |
| Expected dimension checks | 1462 / 1462 passed |
| `schema_completeness_score` | **0.9460** |
| `value_domain_rate` | 0.7300 |
| All other dimension rates | 1.0000 |

Output:
`Saved\Monolith\Benchmarks\SchemaCompleteness\probe-20260711-valid4`.
The successful directory contains `summary.json`, `namespace_breakdown.json`,
`per_action.jsonl`, `probe_preflight.json`, and `probe_results.jsonl`.
`partial_summary.json` and `run_failure.json` are absent, proving the success
cleanup and invalid-artifact separation contract.

## 2026-07-11 probe-20260711-valid3 — transport-invalid run rejected early

This run exercised the shared transport-failure gate against the live 0.20.3
endpoint after the 330-probe corpus migration. Strict preflight completed with
315 catalog-present probes, 4 optional absences, 11 feature-gated absences, and
zero stale required probes. The editor endpoint then disappeared during schema
fetch. The runner stopped at 21/315 attempted probes when 2 transport failures
exceeded the 5% budget (9.52%).

| Run | Result | Attempted | Transport failures | Gate | Normal summary |
| --- | --- | ---: | ---: | --- | --- |
| `probe-20260711-valid3` | invalid / aborted | 21/315 | 2 (9.52%) | `transport_failed_fraction` | not written |

Diagnostic output:
`Saved\Monolith\Benchmarks\SchemaCompleteness\probe-20260711-valid3`.
The directory contains `run_failure.json`, `partial_summary.json`,
`per_action.jsonl`, `probe_preflight.json`, and `probe_results.jsonl`; it does
not contain `summary.json` or a final namespace breakdown. This is deliberate:
partial prefix metrics are diagnostic only and cannot become a benchmark
baseline. The same endpoint-loss pattern previously ran to 200/315 attempts;
the shared gate now bounds it to either three consecutive transport failures or
the first over-budget sample after 20 requests.

## 2026-07-11 baseline-20260711 — first full-catalog scan on the fixed enumeration

First live baseline after the compact-discover enumeration repair + fail-fast
gates (Speed CL 1093, `Docs/testing/2026-07-11-benchmark-contract-failfast-and-n3-guards.md`).
Scan enumerated the catalog via paginated `mode="actions"` (61 namespaces) and
scored every action.

| Run | Label | Date | Actions | Param-bearing | Failed | schema_completeness_score | value_domain_rate |
| --- | --- | --- | --- | --- | --- | --- | --- |
| baseline-20260711 | baseline-20260711 | 2026-07-11 | 1831/1831 | 1728 | 0 | 0.9395 | 0.6979 |

Dimension rates: `param_types_declared` 1.0000, `required_params_marked`
0.9994, `value_domain` 0.6979, `planning_signals_present` 1.0000,
`skill_routing_present` 1.0000, `output_contract_declared` 1.0000.
Output: `Saved\Monolith\Benchmarks\SchemaCompleteness\baseline-20260711`.

Actionable gaps surfaced by this baseline:

- `monolith.guide` is the single catalog action whose params miss a boolean
  `required` flag (`required_params_marked` 0.9994 = 1727/1728).
- `value_domain` is the weak dimension (~522 actions): constrained params
  (numeric limits without ranges, enum-like strings without `enum` lists, or
  undescribed params). Fully-failing small namespaces: `paper2d`, `pcg`,
  `network`, `artifact`, `ndisplay`, `water`, `metahuman`, `chaos_fracture`,
  `cloth`, `dataflow` (each 1–4 param-bearing actions).

Earlier probe runs the same day (`baseline-20260711-probe`,
`-20260711b-probe`, `-20260711d-probe`) each spanned an editor outage window
(the workstation's interactive editors churned the shared endpoint):
136/200/228 of 388 fetches failed and the completed-run
`--max-failed-fraction` gate exited non-zero. Those historical runners retained
diagnostic summaries. The current runner instead emits only invalid-run
artifacts and stops during the run. The full scan above (0 failures) remains the
day's schema-quality baseline; re-run the migrated probe on a stable endpoint with
`python Scripts\schema_completeness_benchmark.py probe --mcp-url
http://localhost:9316/mcp --output-dir <dir> --label <label>`.

## Historical note — 2026-06-18 pending-live-scan status (superseded)

This section is retained only as historical provenance. The 2026-07-11
full-catalog baseline above superseded its pending status. At the time, the
`value_domain` dimension and the param-less N/A semantics (ROI report A4
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

If the MCP endpoint is unreachable, probe and recover the local Speed checkout
with `powershell -NoProfile -ExecutionPolicy Bypass -File
Plugins\Monolith\Scripts\recover_mcp.ps1 -ProbeOnly`, followed by the same
command without `-ProbeOnly` when recovery is required. The recovery script
launches `Build\BatchFiles\RunHeadlessEditor.bat`; do not use another project's
editor wrapper or Monolith checkout.

Then record results here following the format below.

---

## Result Template

| Run | Label | Date | Actions | Param-bearing | Failed | schema_completeness_score | value_domain_rate |
| --- | --- | --- | --- | --- | --- | --- | --- |
| baseline-value-domain | baseline-value-domain | YYYY-MM-DD | 1766 | ? | 0 | 0.0000 | 0.0000 |

Detailed namespace breakdowns are stored in each run's `namespace_breakdown.json`.
Comparison reports between runs are stored in `comparison.md` under the compare
output directory.
