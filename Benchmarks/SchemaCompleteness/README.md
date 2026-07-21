# Monolith Schema Completeness Benchmark

Tests full catalog coverage of schema quality across all live Monolith
namespaces and actions. The checked-in manifest records the 2026-07-11 live
catalog snapshot of 61 namespaces and 1840 actions. Unlike the ActionGuidance
benchmark (which currently contains 454 tasks), SchemaCompleteness calls
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
They are scored only for actions that declare parameters; an absent `params`
key or a `params` object with no user-facing entries is **N/A** on them
(excluded from the rate, never auto-scored 1.0). Malformed data is not
param-less: a non-object `params` value or a non-object user-facing parameter
entry fails all three parameter dimensions and emits a
`params_must_be_object` or `param_schema_must_be_object` diagnostic. A
`monolith_discover` fetch failure is a separate fetch/execution failure, not
N/A. See `METRICS.md` for the full N/A, quality-classification, and
`value_domain` contracts.

## Quality Classification vs Run Validity

Schema quality and benchmark execution validity are deliberately separate.
A successfully fetched schema is reusable checkpoint evidence even when its
schema contract is incomplete; it becomes a quality failure rather than a
fetch failure.

| Classification | Exact meaning |
| --- | --- |
| Full-catalog quality pass | Every applicable dimension is explicitly `true`; equivalently, the validated per-action `schema_score` is `1.0` |
| Full-catalog quality fail | The schema fetch succeeded with `failure_kind: "ok"`, but at least one applicable dimension is `false` |
| Probe pass | No declared expected dimension is `false`, and at least one expected dimension is applicable and `true`; an all-N/A probe is not a pass |
| Fetch/execution failure | The schema was not returned through a valid MCP result. `failed_action_count` and per-namespace `failed_count` record these rows, not schema-quality failures |
| Expected skip | A declared optional or feature-gated probe is absent from the complete live catalog; required probes cannot be skipped |

`quality_pass_action_count` and `quality_fail_action_count` report the
all-applicable-dimensions classification in aggregate and namespace metrics.
For targeted probes, `failed_probe_count` separately reports failure against
that probe's declared `expected_dimensions`. A valid comparable full scan has
`failed_action_count: 0`, but it may still contain quality failures that must
remain visible as inventory `fail` rows rather than being credited as completed
quality coverage.

## Running the Benchmark

### Scan (full catalog)

```powershell
python Plugins\Monolith\Scripts\schema_completeness_benchmark.py scan `
  --mcp-url http://localhost:9316/mcp `
  --output-dir Saved\Monolith\Benchmarks\SchemaCompleteness\run-001 `
  --label "v0.21.0-baseline"
```

The full scan creates schema-version-2 `scan_checkpoint.json` and appends only
valid `failure_kind: "ok"` rows to `per_action.jsonl`. A reusable row preserves
the canonical `raw_schema`, declares the type-exact canonical JSON hash kind,
and carries its SHA-256; the loader reruns `score_schema_quality(raw_schema)`
before accepting it. If the editor/MCP endpoint stops, recover the endpoint and
continue the **same output directory**:

```powershell
python Plugins\Monolith\Scripts\schema_completeness_benchmark.py scan `
  --mcp-url http://localhost:9316/mcp `
  --output-dir Saved\Monolith\Benchmarks\SchemaCompleteness\run-001 `
  --label "v0.21.0-baseline" `
  --resume
```

Resume is fail-closed. Every existing result must still pass raw-schema hash
verification and exact rescoring. The label, endpoint, runner/manifest fingerprints,
server/project/engine metadata, `catalog_version`, action count, exact action-id
set hash, hash kind, timeout, and failure-gate configuration must match the
original run. The action-set identity uses
`sorted_json_namespace_dot_action_v1`: sort the unique `namespace.action`
strings, serialize the JSON array canonically, and SHA-256 the resulting bytes.
`--resume` cannot be combined with `--max-actions`. A changed catalog starts a
new output directory; it is never merged into the old checkpoint.

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
| `summary.json` | Valid completed run only. A full-catalog summary is `comparable: true` only after every enumerated action has exactly one valid result and the catalog/input identity passes a final recheck. Accepted metrics and the embedded namespace breakdown are rederived from `per_action.jsonl` |
| `per_action.jsonl` | Full scan checkpoint result set: exactly one reusable `failure_kind: "ok"` row per completed action. Each row preserves canonical `raw_schema`, its declared `sha256_utf8_canonical_json_sorted_keys_compact_ascii_type_exact_v1` hash and SHA-256, and the exact output of rerunning all six dimensions, `schema_score`, and `value_domain_diagnostics`. Failed attempts are not score rows and remain in checkpoint provenance |
| `scan_checkpoint.json` | Schema-version-2 full-scan identity, lifecycle state, ordered segment history, transport outages, invalid attempts, recovery events, exact action-set hash kind/hash, and completed-valid count used by `--resume` |
| `partial_summary.json` | Updated every 25 segment attempts and on interruption; metrics cover only the validated checkpoint subset and are always `metrics_valid: false`, `comparable: false` |
| `namespace_breakdown.json` | Per-namespace score and quality pass/fail breakdown. Accepted evidence requires this file and the embedded summary object to exactly equal the raw-row derivation, including JSON value types |
| `run_failure.json` | Machine-readable invalid-run reason and partial/final diagnostics; mutually exclusive with `summary.json` |

## Output Files (probe)

| File | Description |
| --- | --- |
| `probe_preflight.json` | Declared/runnable/skipped/stale counts plus exact skipped and stale action IDs from the complete-catalog presence gate |
| `probe_results.jsonl` | Successful preflight/run: one row per declared probe in manifest order. Stale-preflight or transport abort: completed/skipped/stale diagnostic rows only. `result_status` is `scored`, `fetch_failed`, `skipped`, or `stale` |
| `per_action.jsonl` | Attempted catalog-present probes only; successful rows use the same canonical raw-schema/SHA/exact-rescoring contract as full scan, failed fetches carry explicit null raw evidence and canonical hard-failure dimensions, and gated skips never enter schema aggregates |
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
`action_count`. A transport failure while paging one namespace stops catalog
enumeration immediately because requests for later namespaces cannot make the
required complete catalog valid; non-transport contract mismatches remain
collected for diagnosis. The enumerated `catalog_version` is rechecked after pagination
and must still match the immediately following `monolith_status` response;
catalog drift aborts before the first schema request. During schema fetch,
three consecutive transport failures or a
transport-failure fraction above 5% after 20 completed requests aborts
immediately. A full scan never converts a tolerated fetch failure into a score:
only successful schema results enter `per_action.jsonl`, and every missing or
failed action remains pending for a later `--resume`. Before publication, the
runner rechecks the live catalog version, status identity, and benchmark input
fingerprint, then requires the result action-id set to equal the enumerated
catalog set exactly under the declared
`sorted_json_namespace_dot_action_v1` hash contract. Every invalid path writes
`run_failure.json` (and partial diagnostics when work had started) but never
writes `summary.json`; a
silent/depressed baseline cannot be mistaken for a valid result. The
`--max-failed-fraction` tolerance remains a probe/legacy diagnostic setting and
cannot weaken this full-scan exact-completeness gate.

## Full-Scan Resume Contract

Each full scan is a sequence of segments. A fresh command creates segment 1;
every accepted `--resume` creates the next segment and records the editor/server
identity seen at its start. Successful rows are appended immediately and the
checkpoint count is refreshed after each attempt. If the process exits between
the result append and metadata flush, the next resume validates the row and
records a `recovery_events` entry instead of silently discarding or duplicating
it.

The checkpoint separates score data from failure provenance:

Version-1 checkpoints are rejected rather than upgraded in place. Existing
diagnostic evidence has no accepted completion credit, so the next probe/full
run starts with the version-2 raw-evidence contract; no migration path can
silently bless rows that never preserved their source schema.

- `segments` records start/end time, starting valid count, attempts, new valid
  results, invalid results, transport failures, last action, and completion
  status for every editor lifetime.
- `outages` records each transport failure with segment/action/status/bounded
  raw diagnostics and the gate reason when it triggered an abort.
- `invalid_attempts` records transport, protocol, missing-schema, and runner
  failures. Those action IDs remain pending and must later produce a valid row.
- `per_action.jsonl` is loaded with duplicate-JSON-key, catalog-membership,
  row-shape, success-state, raw-schema hash, exact raw rescoring, and
  duplicate-action-id validation before any resume schema call is issued.

Even if the last schema was valid, endpoint loss during the completion identity
recheck leaves the checkpoint interrupted and emits no summary. A later resume
may have zero pending schema calls; it publishes only after the identical live
identity can be confirmed. Publication uses atomic result/summary replacements
and a distinct `publishing` checkpoint state. If the process stops after the
summary replacement but before the final `completed` state flip, `--resume`
removes and republishes that uncommitted summary after the same identity checks.

## Accepted Full-Catalog Evidence Contract

`benchmark_inventory.py` does not trust self-reported summary scores or counts.
It accepts a full-catalog result only when all of the following are true:

- JSON objects have no duplicate members, numbers are finite, and boolean and
  numeric types are not treated as interchangeable.
- `run_valid`, `metrics_valid`, `completion_status`, `metrics_scope`, and
  `comparable` identify one completed, comparable full run.
- `per_action.jsonl` contains exactly one reusable row for every action. Each
  row has canonical `raw_schema`, the exact declared raw-schema hash kind, and a
  matching lowercase SHA-256. The inventory reruns
  `score_schema_quality(raw_schema)` and type-exact-compares all six dimensions,
  the float `schema_score`, and the complete ordered
  `value_domain_diagnostics`; missing, tampered, or rehashed-but-forged derived
  evidence is rejected.
- Aggregate metrics, the embedded namespace breakdown, and the standalone
  `namespace_breakdown.json` are independently rederived from the raw rows and
  match exactly, including JSON value types.
- Scanned/completed/valid/full-catalog counts equal the raw action set,
  `remaining_action_count` is zero, and full-scan fetch failure count is zero.
- The sorted unique `namespace.action` set reproduces the declared
  `sorted_json_namespace_dot_action_v1` SHA-256 in `benchmark_inputs`, the
  resume identity, and checkpoint provenance; `catalog_version` and the full
  benchmark input fingerprint also agree.
- The actual `scan_checkpoint.json` exists with schema version 2,
  `benchmark: "SchemaCompleteness"`, `scan_scope: "full_catalog"`,
  `state: "completed"`, `results_file: "per_action.jsonl"`, an exact result
  count, identical benchmark inputs, matching provenance-array counts, and a
  final segment whose `completion_status` is `completed`.

A summary left beside a checkpoint in `publishing` state is intentionally
uncommitted and cannot be accepted. Likewise, a valid fetch row with incomplete
schema quality remains an inventory quality failure even though it is safe to
reuse during resume.

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
is visible during long scans. Every valid row and failure event is checkpointed
immediately, so endpoint loss is bounded by the consecutive/fraction transport
gates and the next editor lifetime continues only the missing action IDs.

## Offline Scoring Unit Test

The scoring branches (including `value_domain` and the param-less N/A handling)
have an offline unit test that needs no live editor or network — it feeds
fabricated `monolith_discover` envelopes through the scorer:

```powershell
python Plugins\Monolith\Scripts\tests\test_schema_completeness_value_domain.py
```

It asserts that an action declaring an untyped/undescribed required param fails
`value_domain`, a fully-specified action passes, a genuinely param-less action
is N/A (not auto-1.0), malformed `params` containers/entries fail instead of
being hidden as N/A, and the six dimension weights still sum to 1.0.

The catalog enumeration and fail-fast gates have their own offline unit test
that fabricates compact discover envelopes (summary rows without inline
actions, paginated `mode="actions"` pages, schema fetches):

```powershell
python Plugins\Monolith\Scripts\tests\test_schema_completeness_enumeration.py
```

The enumeration test also runs multi-segment resume fixtures: transport abort
then successful continuation, catalog-identity rejection, duplicate checkpoint
result rejection, and completion-recheck outage followed by a zero-refetch
resume.
