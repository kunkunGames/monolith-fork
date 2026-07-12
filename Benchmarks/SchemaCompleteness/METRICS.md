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
| `failed_action_count` | Lower is better | Number of actions for which `monolith_discover` returned no schema or raised a transport error. Failed actions score `False` on every dimension (a hard fail, not N/A). |
| `param_bearing_action_count` | Informational | Number of scanned actions that declare at least one parameter (the denominator for the three param-gated rates). |
| `param_less_action_count` | Informational | Number of scanned actions that declare no parameters (N/A on the three param-gated dimensions). |
| `scanned_action_count` | Informational | Total actions scored in this run (may be less than `total_expected_action_count` when `--max-actions` is used). |
| `declared_probe_count` | Informational | Valid unique rows declared by `probe_set.jsonl` and matched by `manifest.json`. |
| `catalog_present_probe_count` | Informational | Declared probes present in the complete live catalog and therefore eligible for schema scoring. |
| `skipped_probe_count` | Informational | Explicit optional/feature-gated probes absent from the complete live catalog; reported but excluded from score and fetch-failure denominators. |
| `stale_probe_count` | Lower is better; hard gate | Required probes absent from the complete live catalog. Any nonzero value aborts before schema fetch and prevents a baseline summary. |
| `transport_failure_count` | Lower is better; hard gate | Schema requests that failed at HTTP/SSE transport before a Monolith tool result. Three consecutive failures, or more than 5% after 20 requests, abort immediately. |
| `transport_failed_fraction` | Lower is better; hard gate | `transport_failure_count / completed_action_count`; separate from semantic `schema_not_returned` failures. |
| `last_transport_item_id` | Diagnostic | The action that produced the most recent transport failure. It remains paired with `last_transport_status` / `last_transport_error_raw` even when a later successful request triggers the cumulative-fraction gate. |
| `run_valid` | Must be `true` for a baseline | Present and true only in a valid `summary.json`; invalid runs are written to `run_failure.json` and never emit a normal summary. |

## N/A Semantics (param-gated dimensions)

`param_types_declared`, `required_params_marked`, and `value_domain` are
**param-gated**: they describe an action's parameter contract. For an action that
declares no parameters there is no contract to evaluate, so the dimension is
scored **N/A** (`null` in `per_action.jsonl`) and excluded from the rate
denominator. This closes the prior "vacuous-true" hole where a param-less action
auto-scored 1.0 on these dimensions and an action could game two dimensions by
declaring nothing. N/A is also excluded from each action's `schema_score`, which
is the mean of its **applicable** dimensions only.

A `monolith_discover` fetch failure (no schema returned) is a **hard fail**
(`False` on every dimension), not N/A.

## Per-Action Fields (per_action.jsonl)

| Field | Type | Description |
| --- | --- | --- |
| `namespace` | string | Action namespace |
| `action` | string | Action name |
| `param_types_declared` | boolean \| null | Every param has a non-empty `"type"`; `null` (N/A) when the action is param-less |
| `required_params_marked` | boolean \| null | Every param carries a boolean `"required"` flag; `null` (N/A) when param-less |
| `value_domain` | boolean \| null | Every param typed + described + required-flagged, and constrained params document their allowed values; `null` (N/A) when param-less |
| `planning_signals_present` | boolean | `"planning_signals"` is a non-empty list |
| `skill_routing_present` | boolean | `"skill"` is a non-empty string |
| `output_contract_declared` | boolean | `"output_contract_status"` is `"declared"` or `"not_declared"` |
| `schema_score` | float | Mean of the applicable (non-N/A) boolean flags (0.0–1.0, unweighted) |
| `error` | string | Empty string on success; error message if schema fetch failed |
| `failure_kind` | string | `ok`, `transport_error`, `protocol_error`, `schema_not_returned`, or `runner_exception` |
| `transport_error` | boolean | True only when the HTTP/SSE request failed before a tool result was returned |
| `transport_status` | integer/null | HTTP status when available for a transport failure |
| `transport_error_raw` | string | Bounded raw transport diagnostic; empty for semantic schema failures and successes |

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
| Scope | 289 sampled tasks | All live catalog actions, plus 330 targeted probes |
| Namespaces | All 61 (at least 1 representative action each) | All 61 live namespaces (every action) |
| Measures | Agent task success, recovery, param correction | Schema structural quality + value-domain documentation |
| Primary metric | `effectiveness_score` | `schema_completeness_score` |
| LLM calls | None | None |
| Typical duration | ~5 minutes | ~10–30 minutes (full scan) |
| CI smoke mode | N/A | `--max-actions 100` (~5 minutes) |
