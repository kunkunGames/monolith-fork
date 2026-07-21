# Schema Completeness Benchmark Metrics

## Metric Definitions

| Metric | Direction | Definition |
| --- | --- | --- |
| `schema_completeness_score` | Higher is better | Weighted aggregate of the six per-dimension rates (see formula below). Range 0.0–1.0. |
| `param_types_declared_rate` | Higher is better | Fraction of **param-bearing** actions where **every** parameter carries a non-empty `"type"` field. Param-less actions are N/A and excluded from the denominator (not counted as 1.0). |
| `required_params_marked_rate` | Higher is better | Fraction of **param-bearing** actions where **every** parameter carries a boolean `"required"` flag (the required set is fully, correctly specified). Param-less actions are N/A and excluded. |
| `value_domain_rate` | Higher is better | Fraction of **param-bearing** actions where every parameter is typed, carries a non-empty `"description"`, and a boolean `"required"` flag, **and** every constrained parameter documents its value domain (a non-empty `"enum"` list, and a numeric range via `"minimum"`/`"maximum"` for `integer`/`number` types). Param-less actions are N/A and excluded. |
| `planning_signals_present_rate` | Higher is better | Fraction of scanned actions where the schema contains a `"planning_signals"` key that is a non-empty list. |
| `skill_routing_present_rate` | Higher is better | Fraction of scanned actions where the schema contains a `"skill"` key that is a non-empty string. |
| `output_contract_declared_rate` | Higher is better | Fraction of scanned actions where `"output_contract_status"` is explicitly set to either `"declared"` or `"not_declared"`. Absent key does not satisfy this dimension. |
| `mean_schema_score` | Higher is better | Average of per-action `schema_score` values (mean of the **applicable** boolean flags, unweighted). Range 0.0–1.0. |
| `quality_pass_action_count` | Higher is better | Actions for which every applicable dimension is explicitly `true`; after strict row validation this is equivalent to `schema_score == 1.0`. |
| `quality_fail_action_count` | Lower is better | Successfully fetched or attempted score rows with at least one applicable dimension `false`. This is schema-contract quality, not transport/fetch validity. |
| `failed_action_count` | Lower is better; execution diagnostic | Number of schema fetch/execution failure rows included in an aggregate. It is not the schema-quality failure count. A valid full-scan summary is exact-complete and therefore always has 0; failed full-scan attempts are excluded from score rows and counted in checkpoint provenance instead. Probe aggregates retain their hard-fail rows. |
| `param_bearing_action_count` | Informational | Number of scanned actions that declare at least one parameter (the denominator for the three param-gated rates). |
| `param_less_action_count` | Informational | Number of scanned actions that declare no parameters (N/A on the three param-gated dimensions). |
| `scanned_action_count` | Informational | Total actions scored in this run (may be less than `total_expected_action_count` when `--max-actions` is used). |
| `declared_probe_count` | Informational | Valid unique rows declared by `probe_set.jsonl` and matched by `manifest.json`. |
| `catalog_present_probe_count` | Informational | Declared probes present in the complete live catalog and therefore eligible for schema scoring. |
| `skipped_probe_count` | Informational | Explicit optional/feature-gated probes absent from the complete live catalog; reported but excluded from score and fetch-failure denominators. |
| `stale_probe_count` | Lower is better; hard gate | Required probes absent from the complete live catalog. Any nonzero value aborts before schema fetch and prevents a baseline summary. |
| `transport_failure_count` | Lower is better; hard gate | Schema requests in the current segment that failed at HTTP/SSE transport before a Monolith tool result. Three consecutive failures, or more than 5% after 20 segment attempts, abort immediately. Historical failures remain visible as checkpoint `outages` after resume. |
| `transport_failed_fraction` | Lower is better; hard gate | Current segment `transport_failure_count / segment_attempted_action_count`; separate from semantic `schema_not_returned` failures. |
| `last_transport_item_id` | Diagnostic | The action that produced the most recent transport failure. It remains paired with `last_transport_status` / `last_transport_error_raw` even when a later successful request triggers the cumulative-fraction gate. |
| `run_valid` | Must be `true` for a baseline | Present and true only in a valid `summary.json`; invalid runs are written to `run_failure.json` and never emit a normal summary. |
| `completed_valid_action_count` | Informational / completion gate | Unique catalog actions with a validated `failure_kind: "ok"` checkpoint row. A comparable full-scan summary requires this to equal `total_action_count`. |
| `remaining_action_count` | Lower is better / completion gate | Enumerated catalog action IDs without a reusable valid row. Must be 0 for a comparable full-scan summary. |
| `segment_attempted_action_count` | Diagnostic | Requests attempted in the current editor/MCP segment, including invalid attempts; distinct from completed valid results. |
| `attempted_invalid_action_count` | Diagnostic | Total invalid attempts retained in full-scan checkpoint provenance across segments. Retries may make this nonzero even when the eventual comparable summary is valid. |
| `comparable` | Must be `true` for baseline comparison | True only for an exact-complete full-catalog scan. `--max-actions` diagnostic summaries are explicitly false. |

## Quality Pass and Fetch-Failure Semantics

The benchmark maintains two independent classifications:

- **Execution/fetch validity:** whether the live schema request produced a
  reusable `failure_kind: "ok"` row. `failed_action_count` and each namespace's
  `failed_count` belong to this classification.
- **Schema quality:** whether every applicable dimension on that row is
  explicitly `true`. `quality_pass_action_count` and
  `quality_fail_action_count` belong to this classification.

A row with `failure_kind: "ok"` and one false dimension is reusable checkpoint
data, but it is a **quality failure** and the completion inventory classifies it
as `fail`. It is not counted by `failed_action_count`. Conversely, a comparable
full-catalog run requires zero fetch failures but does not hide quality failures;
its quality pass and fail counts still partition every raw action row.

Full-catalog quality pass is equivalent to a validated `schema_score` of `1.0`.
Probe pass is narrower: it considers only the probe's declared
`expected_dimensions`, requires no expected `false`, and requires at least one
applicable expected `true`. An all-N/A probe is inconclusive and therefore not a
pass. `failed_probe_count` records this expected-dimension result, while the
aggregate `quality_*_action_count` fields continue to describe all six schema
dimensions on the underlying score rows.

## N/A Semantics (param-gated dimensions)

`param_types_declared`, `required_params_marked`, and `value_domain` are
**param-gated**: they describe an action's parameter contract. For an action that
declares no parameters there is no contract to evaluate, so the dimension is
scored **N/A** (`null` in `per_action.jsonl`) and excluded from the rate
denominator. Only an absent `params` key or a `params` object with no
user-facing entries is param-less. Internal entries whose names begin with `_`
do not make the parameter contract applicable.

Malformed data is never converted to N/A. A non-object `params` container makes
all three parameter dimensions false and emits a `params_must_be_object`
diagnostic. A non-object user-facing parameter entry likewise makes the three
dimensions false and emits `param_schema_must_be_object`. This prevents an
invalid parameter contract from gaining a perfect score by being filtered into
an apparently empty parameter set.

The N/A treatment closes the prior "vacuous-true" hole where a genuinely
param-less action auto-scored 1.0 on these dimensions. N/A is excluded from each
action's `schema_score`, which is the mean of its **applicable** dimensions only.

A `monolith_discover` fetch failure (no schema returned) is an execution/fetch
failure, not N/A. Probe mode represents it as a failed score row. Full-scan mode keeps it in
`scan_checkpoint.json.invalid_attempts`, leaves the action incomplete, and
requires a later valid retry; it cannot enter a comparable score as six false
dimensions.

## Per-Action Fields (per_action.jsonl)

For a full scan, every row in this file is a reusable successful checkpoint
result and each `namespace.action` appears exactly once. The loader rejects
duplicate JSON member names, duplicate action IDs, actions outside the current
catalog, malformed score fields, non-`ok` failure kinds, and transport/error
payloads. Probe mode still emits attempted catalog-present probe rows under its
separate contract.

| Field | Type | Description |
| --- | --- | --- |
| `namespace` | string | Action namespace |
| `action` | string | Action name |
| `raw_schema_hash_kind` | string | Exact constant `sha256_utf8_canonical_json_sorted_keys_compact_ascii_type_exact_v1`; it defines UTF-8 JSON with sorted object keys, compact separators, ASCII escaping, finite numbers only, and preserved JSON scalar types |
| `raw_schema` | object \| null | Canonical raw schema returned by live discovery. Required for `failure_kind: "ok"`; explicitly `null` for a failed fetch |
| `raw_schema_sha256` | string \| null | Lowercase SHA-256 of the canonical raw-schema bytes on success; explicitly `null` for a failed fetch |
| `param_types_declared` | boolean \| null | Every param has a non-empty `"type"`; `null` (N/A) when the action is param-less |
| `required_params_marked` | boolean \| null | Every param carries a boolean `"required"` flag; `null` (N/A) when param-less |
| `value_domain` | boolean \| null | Every param typed + described + required-flagged, and constrained params document their allowed values; `null` (N/A) when param-less |
| `planning_signals_present` | boolean | `"planning_signals"` is a non-empty list |
| `skill_routing_present` | boolean | `"skill"` is a non-empty string |
| `output_contract_declared` | boolean | `"output_contract_status"` is `"declared"` or `"not_declared"` |
| `schema_score` | float | Mean of the applicable (non-N/A) boolean flags (0.0–1.0, unweighted) |
| `value_domain_diagnostics` | array | One unique, named object per user-facing parameter, with strict boolean `ok` and a non-empty `reason`. Empty only when `value_domain` is N/A; otherwise `value_domain` must equal `all(diagnostic.ok)` |
| `error` | string | Empty string on success; error message if schema fetch failed |
| `failure_kind` | string | `ok`, `transport_error`, `protocol_error`, `schema_not_returned`, or `runner_exception` |
| `transport_error` | boolean | True only when the HTTP/SSE request failed before a tool result was returned |
| `transport_status` | integer/null | HTTP status when available for a transport failure |
| `transport_error_raw` | string | Bounded raw transport diagnostic; empty for semantic schema failures and successes |

Reusable full-scan rows must contain every dimension key explicitly. Missing
keys are not interpreted as N/A, non-parameter dimensions cannot be null, and
`schema_score` must be a finite JSON float. Self-consistency is insufficient:
the validator verifies the raw-schema hash, reruns
`score_schema_quality(raw_schema)`, and type-exact-compares all six dimensions,
`schema_score`, and the complete ordered `value_domain_diagnostics` output.
Duplicate/unnamed/malformed diagnostics, missing raw evidence, stale hashes,
and rehashed raw schemas paired with forged derived fields invalidate the row.

## Full-Scan Checkpoint and Comparability

`scan_checkpoint.json` is schema version 2 and binds a run to
`schema-completeness-full-catalog-v2`. Resume requires an exact match for the
runner/manifest input fingerprint, stable MCP server/project/engine metadata,
label, endpoint, timeout/gate settings, `catalog_version`, catalog action count,
the declared action-ID hash kind, and SHA-256 of the exact catalog action-id
set. The required hash kind is `sorted_json_namespace_dot_action_v1`: sorted
unique `namespace.action` strings encoded as a canonical JSON array. The kind
and hash must agree across benchmark inputs, resume identity, and checkpoint
provenance. Live schema scans do not read
`EngineSource.db` or `ProjectIndex.db`; their volatile signatures
are deliberately marked not applicable so a normal editor restart does not
invalidate otherwise identical schema inputs.

Version-1 checkpoints/evidence are rejected and are not migrated. The existing
diagnostic run carries zero completion credit; a new accepted probe/full run
must capture its source schemas under the version-2 contract.

The checkpoint persists four provenance arrays:

| Field | Contract |
| --- | --- |
| `segments` | One row per process/editor lifetime with start/end, server identity, starting valid count, attempts, new valid results, invalid results, transport failures, last action, and terminal status |
| `outages` | Every transport failure, its segment/action/status/bounded raw diagnostic, plus a gate reason when it aborts a segment |
| `invalid_attempts` | Every non-reusable transport, protocol, missing-schema, or runner result; the corresponding action remains pending |
| `recovery_events` | Explicit provenance when a valid JSONL append survived but its checkpoint count update did not |

A normal comparable summary is emitted only when the validated checkpoint
result IDs exactly equal the current catalog IDs, every result has
`failure_kind: "ok"`, and a final live catalog/status/input recheck matches the
original identity. Endpoint loss during that final recheck remains an invalid
interrupted segment; a later `--resume` may publish without refetching already
valid action rows. Result/summary files are atomically replaced under a
checkpoint `publishing` state; a process stop before the final `completed`
state flip causes the uncommitted summary to be removed and deterministically
republished on resume.

## Accepted-Evidence Raw Derivation

The completion inventory accepts a full-catalog summary only after rebuilding
its claims from the raw artifacts. It uses strict JSON parsing that rejects
duplicate object members and non-finite numbers, and exact type-sensitive JSON
comparison so `true` cannot stand in for numeric `1` and `false` cannot stand in
for `0`.

From `per_action.jsonl`, the validator rechecks row reuse eligibility, verifies
each canonical `raw_schema_sha256`, reruns `score_schema_quality(raw_schema)`,
and exact-compares all six dimension values, `schema_score`, and ordered
diagnostics. It then rederives unique action IDs, quality classification,
aggregate metrics, and namespace metrics. Both the embedded
`summary.namespace_breakdown` and standalone
`namespace_breakdown.json` must exactly match that derivation. Scanned,
expected, completed, valid, total, and full-catalog counts must equal the raw
action set; remaining and full-scan fetch-failure counts must be zero.

The same exact action set must reproduce the declared
`sorted_json_namespace_dot_action_v1` hash in `benchmark_inputs`,
`checkpoint_provenance`, and `scan_checkpoint.json.resume_identity`. Accepted
evidence also requires the actual checkpoint to be schema version 2, full
catalog, `state: "completed"`, bound to `per_action.jsonl`, equal to the
summary's benchmark inputs and catalog version, and finished by a final
completed segment. Its `segments`, `outages`, `invalid_attempts`, and
`recovery_events` array lengths must equal the published provenance counts. A
summary next to a checkpoint in `publishing` state is uncommitted and is not
accepted.

## Score Formula

```
schema_completeness_score =
    0.25 * param_types_declared_rate
  + 0.20 * required_params_marked_rate
  + 0.20 * value_domain_rate
  + 0.15 * planning_signals_present_rate
  + 0.10 * skill_routing_present_rate
  + 0.10 * output_contract_declared_rate
```

Weights reflect the relative importance of each dimension for agent reliability:

- **param_types (0.25)**: Type information is foundational — without it agents cannot validate or auto-fill parameters.
- **required_params (0.20)**: A complete, correct required-param marking enables first-call success; missing it forces retry loops.
- **value_domain (0.20)**: The #1 production-failure cause is a wrong/undocumented param contract that the structural booleans cannot see — undocumented enums, unbounded numerics, untyped or undescribed params. This dimension scores the *correctness and documentation* of the contract, not merely that a `type` key exists.
- **planning_signals (0.15)**: Enables deterministic multi-step workflow construction without hallucination.
- **skill_routing (0.10)**: Routes agents to the correct skill SKILL.md documentation.
- **output_contract (0.10)**: Explicit output contract status prevents agents from guessing response shape.

## Namespace-Score Renormalization (2026-07-11)

The per-namespace `schema_completeness_score` in `namespace_breakdown` excludes
dimensions that are N/A for every action in the namespace and renormalizes the
remaining weights. Before this, a namespace of only param-less actions folded
the three param-gated dimensions in as `0.0` and capped at `0.35` even when
every applicable dimension passed (observed on `slate`/`reflect` in
baseline-20260711). Each namespace row now carries `param_gated_applicable`;
when it is `false` the three param-gated `*_rate` fields are vacuous `0.0`
placeholders excluded from the score. The TOP-LEVEL aggregate formula is
unchanged (every dimension has applicable rows over the full catalog).

## Relationship to ActionGuidance Benchmark

| Property | ActionGuidance | SchemaCompleteness |
| --- | --- | --- |
| Scope | 454 sampled tasks | All live catalog actions, plus 329 targeted probes |
| Namespaces | All 61 (at least 1 representative action each) | All 61 live namespaces (every action) |
| Measures | Agent task success, recovery, param correction | Schema structural quality + value-domain documentation |
| Primary metric | `effectiveness_score` | `schema_completeness_score` |
| LLM calls | None | None |
| Typical duration | ~5 minutes | ~10–30 minutes (full scan) |
| CI smoke mode | N/A | `--max-actions 100` (~5 minutes) |
