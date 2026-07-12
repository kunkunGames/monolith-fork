# Monolith Schema Completeness Benchmark

Tests full catalog coverage of schema quality across all live Monolith
namespaces and actions. The checked-in manifest records the 2026-07-11 live
catalog snapshot of 61 namespaces and 1840 actions. Unlike the ActionGuidance
benchmark (which currently contains 289 tasks), SchemaCompleteness calls
`monolith_discover` once per action and scores six quality dimensions for every
action in the live catalog. The checked-in targeted probe contract contains
**330 unique probes**: 315 required, 11 feature-gated, and 4 optional.

## Files

| File | Description |
| --- | --- |
| `README.md` | This document |
| `METRICS.md` | Metric definitions and score formula |
| `RESULTS.md` | Baseline and historical scan results |
| `manifest.json` | Probe count/availability contract, catalog snapshot metadata, and scoring formula |
| `probe_set.jsonl` | 330 unique targeted schema probes for practical Unreal agent workflows |
| `probe_migration_20260711.json` | Audited mapping for 122 removed legacy IDs, their current replacements, and 15 explicit availability gates |

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
| `summary.json` | Valid completed run only: aggregate metrics including `schema_completeness_score`, `value_domain_rate`, transport counters, and per-namespace breakdown |
| `per_action.jsonl` | One JSON line per completed action with all 6 quality flags (param-gated flags may be `null` = N/A), transport diagnostics, and `schema_score` |
| `partial_summary.json` | Updated every 25 actions for progress monitoring during long scans |
| `namespace_breakdown.json` | Per-namespace score breakdown (also embedded in `summary.json`) |
| `run_failure.json` | Machine-readable invalid-run reason and partial/final diagnostics; mutually exclusive with `summary.json` |

## Output Files (probe)

| File | Description |
| --- | --- |
| `probe_preflight.json` | Declared/runnable/skipped/stale counts plus exact skipped and stale action IDs from the complete-catalog presence gate |
| `probe_results.jsonl` | Successful preflight/run: one row per declared probe in manifest order. Stale-preflight or transport abort: completed/skipped/stale diagnostic rows only. `result_status` is `scored`, `fetch_failed`, `skipped`, or `stale` |
| `per_action.jsonl` | Scored catalog-present probes only; explicit gated skips never enter schema aggregates |
| `summary.json` | Valid completed run only; absent for contract, catalog, required-presence, transport-budget, or fetch-budget failures |
| `partial_summary.json` | Progress snapshot over catalog-present probes only |
| `namespace_breakdown.json` | Per-namespace breakdown over scored probes only |
| `run_failure.json` | Machine-readable invalid-run reason, thresholds, completed count, and transport/fetch diagnostics |

## Output Files (compare)

| File | Description |
| --- | --- |
| `comparison.json` | Full baseline + current + delta objects |
| `comparison.md` | Markdown table: Metric | Baseline | Current | Delta |

## Catalog Enumeration Contract

The scan enumerates the catalog through the compact discover contract
(2026-07): `monolith_discover({})` returns summary rows carrying only
`action_count`, so the scan then pages each namespace with
`monolith_discover(namespace=..., mode="actions", limit=1000, offset=...)`
following `next_offset` until `truncated` is false. The catalog version is
rechecked after pagination, so a registry reload during enumeration aborts
instead of mixing two catalog generations. Legacy summary rows that
still inline an `actions` list are honored without extra calls; namespaces
reporting `action_count: 0` (disabled / not-installed optional modules) are
skipped.

**Fail-fast gates (N5, 2026-07-10):** the scan exits non-zero — and never
records a baseline `summary.json` — when catalog enumeration returns zero
actions or when any namespace fails enumeration or disagrees with its summary
`action_count`. The enumerated `catalog_version` is rechecked after pagination
and must still match the immediately following `monolith_status` response;
catalog drift aborts before the first schema request. During schema fetch,
three consecutive transport failures or a
transport-failure fraction above 5% after 20 completed requests aborts
immediately. A completed run also fails when more than
`--max-failed-fraction` (default 0.05) of all schema fetches failed. Every
invalid path writes `run_failure.json` (and partial diagnostics when work had
started) but never writes `summary.json`; a silent/depressed baseline cannot be
mistaken for a valid result.

## Probe Contract Preflight

`probe` treats `probe_set.jsonl` as the only source of truth; there is no hidden
code-side supplement. Before fetching any schema it strictly validates every
JSON row, rejects duplicate JSON member names, non-canonical action IDs,
duplicate `namespace.action` pairs, duplicate/unknown dimensions, and unproven
availability gates, checks counts against `manifest.json`, then enumerates the
same complete live catalog used by `scan`.

- A missing required action is `stale`: the command writes
  `probe_preflight.json` plus stale diagnostics in `probe_results.jsonl`, exits
  non-zero, and does not write a baseline summary or issue any schema calls.
- A missing action can be skipped only when its row explicitly declares an
  `optional` or `feature_gated` availability object with `gate.kind` and
  `gate.id`. The skip remains visible in the result and is excluded from schema
  score, dimension denominators, and fetch-failure budget.
- If an optional/feature-gated action is present, it is scored exactly like a
  required action; availability metadata never suppresses a live schema defect.

The 2026-07-11 migration contains 122 mappings: 75 direct, 31 partial, 12
composite, and 4 with no equivalent. The mappings contain 140 replacement
occurrences across 112 unique current actions; 64 of those actions were newly
added to `probe_set.jsonl`. The legacy `replacement_actions_added=64` manifest
field is retained for compatibility and means the same thing as
`newly_added_probe_action_count`, not the 112 unique replacements in the map.
The migration also records 15 explicitly gated actions. Four retired intents
had no current action equivalent;
the full-catalog scan continues to cover every live action independently of the
targeted probe contract.

## Performance Note

A full scan issues one paginated `mode="actions"` call per namespace (~60
calls) plus one `monolith_discover` schema call per action. At roughly
1600-1900 live actions and a default 8-second timeout this can take 10-30
minutes depending on editor load. Use `--max-actions 100` for a quick CI smoke
run. The `partial_summary.json` file is refreshed every 25 actions so progress
is visible during long scans; endpoint loss is bounded by the consecutive and
fraction transport gates instead of timing out once for every remaining row.

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

The catalog enumeration and fail-fast gates have their own offline unit test
that fabricates compact discover envelopes (summary rows without inline
actions, paginated `mode="actions"` pages, schema fetches):

```powershell
python Plugins\Monolith\Scripts\tests\test_schema_completeness_enumeration.py
```
