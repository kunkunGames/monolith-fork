# Metrics

The benchmark compares a baseline source index against the current index with
the same task set and deterministic scorer.

## Primary Score

```text
SourceIndex Score =
  0.40 * symbol_hit_rate
+ 0.30 * field_completeness_rate
+ 0.20 * schema_adherence_rate
+ 0.10 * (1 - stale_rate)
```

## Metrics

| Metric | Direction | Definition |
| --- | --- | --- |
| `source_index_score` | Higher is better | Weighted composite of the four component metrics below. |
| `symbol_hit_rate` | Higher is better | Fraction of `symbol_lookup` tasks where at least one result was returned and the `name` field is present. |
| `field_completeness_rate` | Higher is better | Fraction of all individual symbol results (across all `symbol_lookup` tasks) that contain at least 3 of the 4 required fields: `name`, `kind`, `file_path`, `location`. |
| `schema_adherence_rate` | Higher is better | Fraction of `schema_field_presence` tasks where the discovered schema contains `planning_signals`, `skill`, and both `output_contract_status`/`next_actions_status` explicitly declared. |
| `stale_rate` | Lower is better | Fraction of `health_check` tasks whose response signals a stale, error, or missing-fields condition. |
| `mean_results_per_lookup` | Higher is better (in range) | Average number of results returned per `symbol_lookup` task. Very low values indicate sparse indexing; this metric is informational only and is not included in the score. |

## Interpretation

A `source_index_score` at or above **0.80** indicates the source index is
reliably serving agents with symbol location, call-graph edges, and schema
planning signals.

Key improvement vectors:

1. **symbol_hit_rate** — more golden symbols indexed, or fewer connection
   errors when the live editor is unavailable.
2. **field_completeness_rate** — result rows should include `kind` and
   `file_path` (or equivalent `location`) alongside `name`.
3. **schema_adherence_rate** — source actions should declare
   `planning_signals`, `skill`, `output_contract_status`, and
   `next_actions_status` in their `monolith_discover` schema responses.
4. **stale_rate** — `source health` should return a non-stale status with a
   populated `symbol_count` field even when the editor is not running (offline
   DB mode).
