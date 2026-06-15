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

## Metrics

| Metric | Direction | Definition |
| --- | --- | --- |
| `task_success_rate` | Higher is better | Fraction of benchmark tasks where the scorer can recover or plan within the allowed tool-call budget. |
| `first_recovery_success_rate` | Higher is better | Fraction of failure tasks whose first response directly contains enough structured evidence to recover. |
| `action_selection_accuracy` | Higher is better | Fraction of discovery/unknown-action tasks where the target action is selected directly, or partially via deterministic discovery fallback. |
| `param_correction_accuracy` | Higher is better | Fraction of missing/invalid-param tasks where structured evidence identifies the missing or invalid parameter. |
| `mean_tool_calls_to_success` | Lower is better | Average number of MCP calls needed before the deterministic scorer can recover or plan. |
| `invalid_retry_rate` | Lower is better | Fraction of failure tasks whose first response lacks enough structured recovery evidence, making a bad retry likely. |
| `hallucinated_workflow_rate` | Lower is better | Fraction of discovery tasks where output/next-action contract status is absent, leaving room to infer unsupported workflow edges. |

## Interpretation

The intended improvement is not a higher count of manually declared
`outputs`/`next_actions`. The intended improvement is:

1. More generated planning evidence.
2. Clearer first-failure diagnostics.
3. Fewer extra discovery calls needed after failure.
4. Lower risk of inventing workflow edges from missing metadata.
