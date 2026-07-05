# monolith.execute_plan v2 Verification — array-index refs + transaction rollback

| Field | Value |
| --- | --- |
| Date | 2026-07-05 |
| Project | `D:\P4\speed\Speed.uproject` |
| Scope | `FMonolithPlanExecutor` v2: numeric array-index reference segments and the `transaction=auto` outermost editor transaction (cancel-on-`stop_on_error`) |

---

## 1. Changes Under Test

1. **Array-index references** — a whole-string param `"$steps.<id>.result.<path>"` now accepts
   all-digit path segments that index into arrays (`"$steps.s1.result.items.0.name"`), in
   addition to object-field segments. Out-of-range indices fail with an explicit message.
2. **`transaction` param (`auto`/`off`)** — with `auto` (default) a mutating plan runs inside
   one outermost `GEditor` transaction. On a `stop_on_error` halt the transaction is
   **cancelled** (undoable edits roll back; each executed mutating step row gets
   `rolled_back:"editor_transaction"`, and no `partial_state_note` is emitted). On full success
   it **commits**. The response `transaction.{mode,state}` reports the outcome with a `caveat`
   that saves/disk/source-control/external effects are not undoable. `transaction=off` (and
   `stop_on_error=false`) restores the v1 partial-state markers.

## 2. Results

| Check | Result |
| --- | --- |
| Primary `SpeedEditor Win64 Development` UBT build | Passed (`Result: Succeeded`). |
| `Monolith.Core.PlanExecutor.*` (7 tests) | All passed: v1 5 (`DryRunValidatesWithoutExecuting`, `ResolvesStepReferences`, `ConfirmGateBlocksMutations`, `RejectsInvalidPlans`, `StopOnErrorSkipsRemainingSteps`) + v2 `ResolvesArrayIndexReferences` and `TransactionWrapsAndCancels`. `TEST COMPLETE. EXIT CODE: 0`. |
| Live array-index resolve | Passed: `s2.params.action = "$steps.s1.result.actions.0.action"` resolved to a real action name from `discover(source, mode=actions)`, and s2 returned that action's schema. |
| Live array-index out-of-range | Passed: `...actions.99.action` failed with `array index 99 out of range (array has 3 element(s))`. |
| Live transaction commit | Passed: two mutating (`transaction_optional`) discover steps succeeded → `transaction.state="committed"` with the non-undoable `caveat`. |
| Live transaction cancel | Passed: mutating s1 ok, s2 fails (out-of-range ref) under default `stop_on_error` → `transaction.state="cancelled"`, s1 row `rolled_back:"editor_transaction"`, no `partial_state_note`. |
| Live per-step result cap | Passed: `max_result_bytes_per_step` (2048 / 1024) summarized both step results with `result_truncated`/`result_bytes`/`result_top_keys`. |
| Live read-only plan | `transaction.state="none"` — a plan with no mutating step opens no transaction. |

Verified against the headless MCP editor at `http://localhost:9316/mcp` (pid 48340) after the UBT rebuild.

## 3. Notes

`monolith.discover` classifies as `transaction_optional` (mutating) via the registry's
name-based execution-policy inference ("discover" is not a read-like verb prefix), which is
why the discover-chained live plans exercise the transaction path and require `confirm=true`.
This is pre-existing inference behavior surfaced by the plan classifier, not a v2 defect.

Test runs must stop the live watchdog first: the watchdog's recover path kills a `-NullRHI`
editor when `9316` is unhealthy and cannot distinguish a non-MCP automation/commandlet run
from an unhealthy MCP editor (it killed an in-progress automation editor during this task).
Filed as a `Docs\TODO.md` follow-up (watchdog `-NullRHI` over-kill).

Follow-ups (v3 / recipes) recorded in `Docs\TODO.md`: documented `monolith.guide` plan
recipes for common authoring chains, and per-step nested transaction policy.
