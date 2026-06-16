# Schema Completeness Benchmark Metrics

## Metric Definitions

| Metric | Direction | Definition |
| --- | --- | --- |
| `schema_completeness_score` | Higher is better | Weighted aggregate of the five per-dimension rates (see formula below). Range 0.0–1.0. |
| `param_types_declared_rate` | Higher is better | Fraction of scanned actions where every parameter carries a `"type"` field. Actions with no parameters count as 1.0 (vacuously satisfied). |
| `required_params_marked_rate` | Higher is better | Fraction of scanned actions where at least one parameter has `"required": true`. Actions with no parameters count as 1.0 (vacuously satisfied). |
| `planning_signals_present_rate` | Higher is better | Fraction of scanned actions where the schema contains a `"planning_signals"` key that is a non-empty list. |
| `skill_routing_present_rate` | Higher is better | Fraction of scanned actions where the schema contains a `"skill"` key that is a non-empty string. |
| `output_contract_declared_rate` | Higher is better | Fraction of scanned actions where `"output_contract_status"` is explicitly set to either `"declared"` or `"not_declared"`. Absent key does not satisfy this dimension. |
| `mean_schema_score` | Higher is better | Average of per-action `schema_score` values (simple mean of the 5 boolean flags, unweighted). Range 0.0–1.0. |
| `failed_action_count` | Lower is better | Number of actions for which `monolith_discover` returned no schema or raised a transport error. Failed actions score 0.0 on all five dimensions. |
| `scanned_action_count` | Informational | Total actions scored in this run (may be less than `total_expected_action_count` when `--max-actions` is used). |

## Per-Action Fields (per_action.jsonl)

| Field | Type | Description |
| --- | --- | --- |
| `namespace` | string | Action namespace |
| `action` | string | Action name |
| `param_types_declared` | boolean | All params have a `"type"` field (or no params) |
| `required_params_marked` | boolean | At least one param is `"required": true` (or no params) |
| `planning_signals_present` | boolean | `"planning_signals"` is a non-empty list |
| `skill_routing_present` | boolean | `"skill"` is a non-empty string |
| `output_contract_declared` | boolean | `"output_contract_status"` is `"declared"` or `"not_declared"` |
| `schema_score` | float | Simple mean of the 5 boolean flags (0.0–1.0, unweighted) |
| `error` | string | Empty string on success; error message if schema fetch failed |

## Score Formula

```
schema_completeness_score =
    0.30 * param_types_declared_rate
  + 0.25 * required_params_marked_rate
  + 0.20 * planning_signals_present_rate
  + 0.15 * skill_routing_present_rate
  + 0.10 * output_contract_declared_rate
```

Weights reflect the relative importance of each dimension for agent reliability:

- **param_types (0.30)**: Type information is foundational — without it agents cannot validate or auto-fill parameters.
- **required_params (0.25)**: Required-param marking enables first-call success; missing it forces retry loops.
- **planning_signals (0.20)**: Enables deterministic multi-step workflow construction without hallucination.
- **skill_routing (0.15)**: Routes agents to the correct skill SKILL.md documentation.
- **output_contract (0.10)**: Explicit output contract status prevents agents from guessing response shape.

## Relationship to ActionGuidance Benchmark

| Property | ActionGuidance | SchemaCompleteness |
| --- | --- | --- |
| Scope | 161 sampled tasks | All actions in catalog (1766) |
| Namespaces | All 51 (1 representative action each) | All 51 (every action) |
| Measures | Agent task success, recovery, param correction | Schema structural quality |
| Primary metric | `effectiveness_score` | `schema_completeness_score` |
| LLM calls | None | None |
| Typical duration | ~5 minutes | ~10–30 minutes (full scan) |
| CI smoke mode | N/A | `--max-actions 100` (~5 minutes) |
