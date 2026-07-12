# Action Guidance Benchmark Results

## 2026-07-11 baseline-20260711 — first run on the 281-task corpus

First live baseline of the expanded, demand-weighted 281-task corpus (the
2026-06-16 scores below used the older 161-task set and are not comparable).
Run alongside the suite-wide baseline refresh (Speed CL 1093,
`Docs/testing/2026-07-11-benchmark-contract-failfast-and-n3-guards.md`), after
the error-recovery enumeration switched to paginated `mode="actions"` discover
(the default 50-row page hid actions past page 1 for large namespaces and
failed recovery scoring through no fault of the server).

| Metric | Value |
| --- | ---: |
| `effectiveness_score` | **0.8721** |
| `task_success_rate` | 0.8990 |
| `first_recovery_success_rate` | 0.8309 |
| `action_selection_accuracy` | 0.8741 |
| `param_correction_accuracy` | 0.8663 |
| `mean_tool_calls_to_success` | 1.258 |
| `invalid_retry_rate` | 0.1691 |
| `hallucinated_workflow_rate` | 0.1193 |

Output: `Saved\Monolith\Benchmarks\ActionGuidance\baseline-20260711`.
Editor 0.20.3, 1831 actions / 61 namespaces, engine `++UE5+Release-5.8`.

## 2026-06-16 Legacy vs Current

This historical run used the then-current 161-task benchmark set against a
legacy MCP build restored from CL 735 `#have` sources and then against the
current MCP build. The checked-in reusable task corpus has since expanded and
deduplicated to 281 tasks, including 18 `needed_action_routing` tasks and a
per-task demand `weight` (see METRICS.md). The 2026-06-16 scores below predate
the routing category and demand weighting; re-run against a live MCP to refresh
the baseline before comparing.

| Metric | Legacy `#have` | Current | Delta | Better |
| --- | ---: | ---: | ---: | --- |
| `effectiveness_score` | 0.426630 | 0.949001 | +0.522371 | Higher |
| `task_success_rate` | 0.683230 | 1.000000 | +0.316770 | Higher |
| `first_recovery_success_rate` | 0.000000 | 0.863636 | +0.863636 | Higher |
| `action_selection_accuracy` | 0.750000 | 1.000000 | +0.250000 | Higher |
| `param_correction_accuracy` | 0.500000 | 0.872881 | +0.372881 | Higher |
| `mean_tool_calls_to_success` | 2.316770 | 1.093168 | -1.223602 | Lower |
| `invalid_retry_rate` | 1.000000 | 0.136364 | -0.863636 | Lower |
| `hallucinated_workflow_rate` | 1.000000 | 0.000000 | -1.000000 | Lower |

## Evidence

| Artifact | Path |
| --- | --- |
| Task set | `Plugins\Monolith\Benchmarks\ActionGuidance\tasks.jsonl` |
| Manifest | `Plugins\Monolith\Benchmarks\ActionGuidance\manifest.json` |
| Legacy run | `Saved\Monolith\Benchmarks\ActionGuidance\legacy_cl735_have_20260616-010112` |
| Current run | `Saved\Monolith\Benchmarks\ActionGuidance\current_20260616-011125` |
| Comparison | `Saved\Monolith\Benchmarks\ActionGuidance\comparison_legacy_cl735_have_vs_current_20260616-011125` |
| Legacy UBT success log | `Saved\BuildLogs\GoGameEditor-UBT-legacy-cl735-have-retry-20260616-003838.log` |
| Current UBT success log | `Saved\BuildLogs\GoGameEditor-UBT-20260615-235802.log` |

## Interpretation

The improvement is measurable because current MCP gives the deterministic
scorer enough first-response evidence to recover from most failures and enough
explicit discovery evidence to avoid inventing workflow edges.

The score does not reward fabricated `outputs` or `next_actions`. Undeclared
workflow data remains explicitly `not_declared`; the measured gain comes from
generated planning signals, skill/action routing, structured failure causes,
candidate actions, validation errors, and parameter metadata.
