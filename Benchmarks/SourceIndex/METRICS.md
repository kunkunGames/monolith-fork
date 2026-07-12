# Metrics

The benchmark compares a baseline source index against the current index with
the same task set and deterministic scorer.

## Primary Score

```text
SourceIndex Score =
  0.30 * symbol_hit_rate
+ 0.20 * field_completeness_rate
+ 0.15 * schema_adherence_rate
+ 0.10 * (1 - stale_rate)
+ 0.10 * ergonomics_success_rate
+ 0.15 * negative_recovery_rate
```

The six coefficients sum to **1.00**.  This formula is the single source of
truth and matches `aggregate()` in
`Plugins/Monolith/Scripts/source_index_benchmark.py` and the `scoring` block in
`manifest.json`.

## Metrics

| Metric | Direction | Definition |
| --- | --- | --- |
| `source_index_score` | Higher is better | Weighted composite of the six component metrics below. |
| `symbol_hit_rate` | Higher is better | Fraction of lookup tasks (`symbol_lookup` + `review_context_lookup` + `impact_radius_lookup`) with `direct_success`. For a `require_results` task (a symbol KNOWN to have a definition / callers / callees) an empty or "No direct C++ callers found…" sentinel response is a **miss**, not a hit — this closes the `min_results:0` loophole. |
| `field_completeness_rate` | Higher is better | Mean per-task field-completeness computed **only over expected-nonempty (`require_results`) lookups**: a required lookup that returned nothing contributes `0`. This closes the divide-by-returned-results loophole where an empty run scored a vacuous `1.0`. |
| `schema_adherence_rate` | Higher is better | Fraction of `schema_field_presence` tasks where the discovered schema contains `planning_signals`, `skill`, and both `output_contract_status`/`next_actions_status` explicitly declared. |
| `stale_rate` | Lower is better | Fraction of `health_check` tasks whose response signals a stale, error, or missing-fields condition. Scored as `1 - stale_rate`. |
| `ergonomics_success_rate` | Higher is better | Fraction of `ergonomics_text` tasks (`get_include_path`, `get_signature`, `verify_symbols`, …) returning non-empty, non-error text. |
| `negative_recovery_rate` | Higher is better | Mean RESPONSE-QUALITY score (`0..1`) over `negative_recovery` tasks (deliberately bad input — nonexistent symbols, missing params, unqualified names). A transport crash or a silent empty success scores `0`; a structured error that names the offending identifier scores `0.7`; the same error plus a did-you-mean / qualified-symbol / retry hint scores `1.0`. For unqualified-resolution tasks (`expect_error:false`) a populated answer is the pass and a not-found rejection is the failure. |
| `mean_results_per_lookup` | Higher is better (in range) | Average number of results returned per lookup task. Informational only; not included in the score. |

## Run-integrity fields and gates

Run integrity is not another score component. A baseline is valid only when the complete artifact has
`run_valid=true`, `metrics_valid=true`, `metrics_scope=complete_run`, and
`completion_status=completed`.

| Field | Meaning |
| --- | --- |
| `transport_error` | The task's HTTP/SSE request failed before a tool result |
| `transport_status` | HTTP status for the transport failure when available |
| `transport_error_raw` | Bounded raw transport diagnostic |
| `protocol_error` / `protocol_error_raw` | Invalid JSON, non-object JSON, top-level JSON-RPC error, or missing result envelope; invalidates the run |
| `failure_kind` | Empty for a valid protocol response; otherwise `protocol_error` or `runner_exception` on a triggering task row |
| `transport_failure_count` | Number of attempted task rows with `transport_error=true` |
| `transport_failed_fraction` | `transport_failure_count / attempted_task_count` |
| `last_transport_item_id` | Last actual transport-failed task, preserved when a later success triggers the fraction gate |

Three consecutive transport failures abort immediately. At 20 attempted tasks, a transport-failure
fraction strictly above `0.05` aborts; exactly `0.05` remains valid. `finalize()` applies the same
fraction limit to a shorter completed corpus. Status transport/protocol failures, task protocol
failures, and runner exceptions write `run_failure.json` and no `summary.json`; task-level evidence is
retained in `per_task.jsonl` and `partial_summary.json` when execution had started.

## Interpretation

A `source_index_score` at or above **0.80** indicates the source index is
reliably serving agents with symbol location, call-graph edges, schema planning
signals, **and self-correcting errors on bad input**.

Key improvement vectors:

1. **symbol_hit_rate** — more golden symbols indexed, and the curated
   `require_results` symbols (e.g. `AActor::BeginPlay` callers) must actually
   resolve to non-empty call edges, not return the "may be called via delegates"
   sentinel.
2. **field_completeness_rate** — result rows should include `kind` and
   `file_path` (or equivalent `location`) alongside `name`.
3. **schema_adherence_rate** — source actions should declare
   `planning_signals`, `skill`, `output_contract_status`, and
   `next_actions_status` in their `monolith_discover` schema responses.
4. **stale_rate** — `source health` should return a non-stale status with a
   populated `symbol_count` field even when the editor is not running (offline
   DB mode).
5. **ergonomics_success_rate** — plain-text helpers should return usable text,
   not `Error` strings, on valid input.
6. **negative_recovery_rate** — on bad input, source actions should fail with a
   structured error that echoes the offending identifier and offers a
   did-you-mean / qualified-symbol / "run search_source first" hint, never a
   transport crash or a silent empty success.
