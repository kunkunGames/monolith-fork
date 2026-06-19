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

## Relationship to ActionGuidance Benchmark

| Property | ActionGuidance | SchemaCompleteness |
| --- | --- | --- |
| Scope | 263 sampled tasks | All live catalog actions, plus 385 targeted probes |
| Namespaces | All 51 (1 representative action each) | All 51 (every action) |
| Measures | Agent task success, recovery, param correction | Schema structural quality + value-domain documentation |
| Primary metric | `effectiveness_score` | `schema_completeness_score` |
| LLM calls | None | None |
| Typical duration | ~5 minutes | ~10–30 minutes (full scan) |
| CI smoke mode | N/A | `--max-actions 100` (~5 minutes) |
