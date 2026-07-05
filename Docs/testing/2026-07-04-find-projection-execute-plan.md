# monolith.find / list_automation_tests / coverage Projection + execute_plan v1 Verification

| Field | Value |
| --- | --- |
| Date | 2026-07-04 |
| Project | `D:\P4\speed\Speed.uproject` |
| Scope | Large-result projection for `monolith.find`, `editor.list_automation_tests`, `monolith.get_action_metadata_coverage` (fresh 2026-07-04 log candidates) and the new `monolith.execute_plan` v1 plan executor (PR E) |

---

## 1. Changes Under Test

1. **`monolith.find`** — match rows default to `planning_detail=compact` (counts replace the
   `precondition_details`/`planning_signals` arrays that drove ~37KB average responses across
   52 calls in the 20260629–20260704 window); additive `offset`/`cursor`/`fields` with the
   common `total`/`returned`/`next_cursor`/`projection` contract and an available-fields
   warning for unknown projections.
2. **`editor.list_automation_tests`** — paged (default 500, max 5000) with `offset`/`cursor`
   and the common contract next to the legacy `count`; a full unpaged listing had measured
   2,077KB in one call.
3. **`monolith.get_action_metadata_coverage`** — additive `detail=summary` keeps `totals` and
   `gate` and replaces `by_namespace`/`by_skill` bucket rows with counts (`by_namespace_count`,
   `by_skill_count`); default remains `full` for the existing CI gate consumers (max observed
   full payload 171KB).
4. **`monolith.execute_plan` v1** (`FMonolithPlanExecutor`) — sequential validated plans of up
   to 25 steps through the normal dispatch pipeline; `"$steps.<id>.result.<field.path>"`
   references; `dry_run` plan classification; `confirm`/`allow_destructive` gates;
   `stop_on_error`; per-step result caps; children logged under the plan trace/span. No
   cross-step rollback in v1 (explicit `partial_state_note`/`rollback_available:false`).

## 2. Results

| Check | Result |
| --- | --- |
| Primary `SpeedEditor Win64 Development` UBT build | Passed (headless MCP editor stopped for the link; watchdog relaunched it). |
| `Monolith.Core.PlanExecutor.*` (5 tests: DryRunValidatesWithoutExecuting, ResolvesStepReferences, ConfirmGateBlocksMutations, RejectsInvalidPlans, StopOnErrorSkipsRemainingSteps) | All passed. Confirm gate provably blocks execution (mutation counter unchanged); dry_run executes nothing; forward/unknown references and nested execute_plan rejected at plan time. |
| `Monolith.Core.FindProjection` (new) | Passed: compact default omits `planning_signals`/`precondition_details` and keeps counts; `planning_detail=full` restores arrays; offset/cursor pages do not overlap; `fields` projection + unknown-field warning. |
| `Monolith.Core.FindWeightedScoring` / `FindGoldenQueries` / `ActionExecutionPolicy.*` (11) | All passed — legacy find ranking and the coverage report/gate behavior unchanged at default `detail=full`. |
| Live `monolith.find {query:"search source symbols", limit:2, fields:"action_id,score,skill"}` | Passed: rows exactly `{action_id, score, skill}`, `total=467`, `next_cursor="2"`, `projection.planning_detail=compact`; response ~0.5KB (was ~37KB average). |
| Live `monolith.execute_plan` dry_run (status → discover(if_version ref)) | Passed: per-step `policy_id`/`mutating` classification (`discover` reports its registry-inferred `transaction_optional` policy), `references:["s1"]`, `requires_confirm=true`. |
| Live `monolith.execute_plan` confirm=true execution | Passed: `succeeded=2`; s2's `if_version` resolved from s1's `catalog_version` and discover returned `{status:"unchanged"}` — the repeated-discover recipe completes in one call. |
| Live `monolith.get_action_metadata_coverage {detail:"summary"}` | Passed: `totals` + `gate` (passed, 5 high-traffic checks) + `by_namespace_count=61`/`by_skill_count=45`, no bucket rows (~4KB vs 21.9KB avg / 171KB max full). |
| Live `editor.list_automation_tests {prefix:"Monolith.Core.PlanExecutor", limit:2, cursor:"1"}` | Passed: `total=5`, `returned=2`, `truncated=true`, `next_cursor="3"`, `limits={default_limit:500, max_limit:5000, offset:1}`. |
| Offline catalog snapshot | Regenerated `Tools\MonolithQuery\Generated\monolith_catalog_snapshot.json` (2,046 parsed registrations, includes `monolith.execute_plan`). The offline `monolith find`/`get_action_metadata_coverage` CLI keeps its own snapshot-backed compact contract and needed no mirroring. |

## 3. Notes

`monolith.discover` classifying as `transaction_optional` (mutating) inside execute_plan
plans comes from the registry's conservative name-based policy inference ("discover" is not
a read-like verb prefix) — pre-existing behavior surfaced by the plan classifier, not a
plan-executor defect; plans that chain discover simply pass `confirm=true` or use
`dry_run` first. Follow-ups recorded in `Docs\TODO.md`: execute_plan v2 (rollback,
array-index reference paths, documented plan recipes) and the remaining low-priority
`audio`/`paper2d` list caps.
