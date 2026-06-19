# Metrics

The benchmark compares a baseline MCP surface against the current MCP surface
with the same task set and deterministic scorer.

## Primary Score

```text
Effectiveness Score =
  0.30 * TaskSuccessRate
+ 0.20 * FirstRecoverySuccessRate
+ 0.15 * ActionSelectionAccuracy
+ 0.15 * ParamCorrectionAccuracy
+ 0.10 * (1 - NormalizedToolCalls)
+ 0.10 * (1 - HallucinatedWorkflowRate)
```

`NormalizedToolCalls = clamp((MeanToolCallsToSuccess - 1) / 3, 0, 1)`.

The six component weights sum to 1.0. They are fixed. What changed is the
*per-task averaging inside each component*: every rate is now a demand-weighted
mean (see "Demand Weighting" below), not a uniform mean.

## Metrics

| Metric | Direction | Definition |
| --- | --- | --- |
| `task_success_rate` | Higher is better | Demand-weighted fraction of benchmark tasks where the scorer can recover or plan within the allowed tool-call budget. |
| `first_recovery_success_rate` | Higher is better | Demand-weighted fraction of failure tasks (unknown-action, needed-action routing, missing/invalid-param) whose first response directly contains enough structured evidence to recover. |
| `action_selection_accuracy` | Higher is better | Demand-weighted fraction of discovery / unknown-action / needed-action-routing tasks where the target action is selected or routed-to directly, or partially via deterministic discovery fallback. |
| `param_correction_accuracy` | Higher is better | Demand-weighted fraction of missing/invalid-param tasks where structured evidence identifies the missing or invalid parameter. |
| `mean_tool_calls_to_success` | Lower is better | Demand-weighted average number of MCP calls needed before the deterministic scorer can recover or plan. |
| `invalid_retry_rate` | Lower is better | Demand-weighted fraction of failure tasks whose first response lacks enough structured recovery evidence, making a bad retry likely. |
| `hallucinated_workflow_rate` | Lower is better | Demand-weighted fraction of discovery tasks where output/next-action contract status is absent, leaving room to infer unsupported workflow edges. |

## Demand Weighting

Each task carries a `weight` derived from live invocation volume x error cost
(`weight = 1.0 + log10(1 + count * error_rate)` for documented high-volume /
high-error actions, sourced from the invocation-log analyzer Action Stats; else
`1.0`). Sub-metrics are combined with a weighted mean, so a 294-call/47%-error
action moves the aggregate far more than a dead 10-call action. The aggregate
also reports `weighted_task_mass` (the sum of task weights) for transparency.

## Needed-Action Routing

The `needed_action_routing` category measures the discovery side of the largest
live-ROI bucket: when an agent has an absent, typoed, or vague action name,
`monolith_find` (or a structured `monolith_discover` hint) must name the real
action_id. A candidate-bearing routing response scores HIGH; a bare
`Unknown action` error with no candidates scores LOW. These tasks contribute to
`action_selection_accuracy` and `first_recovery_success_rate`.

## Interpretation

The intended improvement is not a higher count of manually declared
`outputs`/`next_actions`. The intended improvement is:

1. More generated planning evidence.
2. Clearer first-failure diagnostics.
3. Fewer extra discovery calls needed after failure.
4. Lower risk of inventing workflow edges from missing metadata.
