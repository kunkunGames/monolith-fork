# ProjectIndex Benchmark

This benchmark measures whether the Monolith MCP project namespace (asset index)
returns data that is rich enough for agents to find assets, look up gameplay tags,
and understand the project structure.

The benchmark is deterministic.  It does not call an LLM.  Each task sends one
safe MCP request, then the scorer checks whether the response contains the
machine-readable evidence an agent needs to locate assets, navigate the project
graph, and plan asset or gameplay changes.

## Files

| File | Purpose |
| --- | --- |
| `tasks.jsonl` | Generated fixture set covering all six task categories. |
| `manifest.json` | Catalog metadata and score formula. |
| `live_fixtures.json` | Required schema-v2 project fixtures (schema actions + clean-checkout known answers) written by `refresh_live_fixtures`; consumed by `generate`. |
| `METRICS.md` | Metric definitions, score formula, and interpretation. |
| `RESULTS.md` | Latest checked-in benchmark result summary. |
| `Plugins/Monolith/Scripts/project_index_benchmark.py` | Generator, runner, fixture refresher, and comparison tool. |
| `Plugins/Monolith/Scripts/test_project_index_benchmark.py` | Offline scoring unit test (no live editor needed). |

## Project-local fixtures (`refresh_live_fixtures`)

Known-answer fixtures and the schema-action list are **project-specific**: the
hard-coded 20260618 snapshots were captured on a different project and scored
`known_answer_hit_rate 0.0` plus 21 unknown-action schema rows when first run
against Speed (baseline-20260711). Before generating the corpus on a new
project (or after large content/catalog changes), refresh the fixtures from
the live index:

```powershell
python Plugins\Monolith\Scripts\project_index_benchmark.py refresh_live_fixtures `
  --mcp-url http://localhost:9316/mcp
python Plugins\Monolith\Scripts\project_index_benchmark.py generate
```

`refresh_live_fixtures` enumerates the live `project` namespace actions
(paginated discover), derives distinctive asset names from naming-convention
seed prefixes (`DA_`, `DT_`, `BP_`, …), sorts each prefix pool, and selects one
candidate per prefix per round. This makes selection deterministic and
prefix-balanced instead of depending on live search return order.

Every known-answer candidate must pass all of these gates before the existing
fixture file can be overwritten:

- its package path is outside mutable benchmark roots such as
  `/Game/Benchmarks/`;
- `project.get_saved_asset_state` reports `exists_on_disk=true` and a positive
  file size for the exact package path;
- `source_control.get_status` reports one known, current, source-controlled
  state that is not add/delete/ignored/conflicted;
- the default content-inclusive `project.search` call used by the benchmark
  returns the candidate's exact asset identity path.

The refresh fails without writing when the source-control provider is disabled
or unavailable, the live catalog cannot be enumerated, transport/schema
contracts fail, or fewer than `--min-known-answers` stable pairs are found.
`generate` also fails without writing when `live_fixtures.json` is missing,
malformed, pre-schema-v2, or violates the stability policy. Historical
20260618 schema/known-answer snapshots are not executable; neither the CLI nor
the core task builder can fall back to them.

The checked-in `live_fixtures.json` is the canonical output of a successful
live refresh, not a hand-curated overlay. Its `generated_at` and
`catalog_version` identify the editor/catalog instance that performed the disk,
source-control, and identity checks. When the canonical selection changes,
refresh the whole file and then run `generate`; do not splice individual rows
into an older snapshot or update provenance without rerunning live validation.

## Task Categories

| Category | Request type | Scored evidence |
| --- | --- | --- |
| `asset_search` | `project_query` with `action=search` and various query strings | Valid non-error response (lenient for `min_results:0`; strict `>=1` result for `require_results`); results checked for `match_object_path`, `match_value`, `match_source`. |
| `gameplay_tag_lookup` | `project_query` with `action=list_gameplay_tags` or `action=search_gameplay_tags` | Valid non-error response (lenient: empty tag list OK if response is valid JSON). |
| `known_answer` | `project_query` with `action=search` for a distinctive submitted asset name | HIT only when the response's asset-identity paths (`asset_path`, plus legacy `match_object_path` identity rows) contain the fixture's exact `expected_object_path` AND `min_results>=1`. Ground-truth recall an empty/broken index cannot fake. |
| `health_check` | `project_query` with `action=health` | Response contains `status` field; checked for stale/error/degraded state. |
| `stats_check` | `project_query` with `action=get_stats` | Response has `success=true`, `indexing` present, and `stats` as an object. |
| `schema_field_presence` | `monolith_discover` with `namespace=project`, `mode=schema` | Schema has `planning_signals` (non-empty list) and `skill` (non-empty string). |

All tasks are read-only and safe to run against any live Monolith MCP endpoint.

## Empty / Broken Index Guard

An empty or broken index used to score a perfect `1.000`. Now:

- `known_answer` tasks require the index to actually return the expected Unreal
  package path (`/Game/...` or a project plugin mount such as `/SpeedCore/...`),
  so an empty index scores `known_answer_hit_rate = 0.0`.
- `field_completeness_rate` is computed over expected-nonempty tasks and is `0.0`
  (not a vacuous `1.0`) when those tasks return no rows.
- When every result-bearing task returns zero results the run is flagged
  `all_empty=true`, a `[ALL-EMPTY]` warning is printed to stderr, and
  `project_index_score` is capped at `0.30`.

## Primary Score

`project_index_score = 0.25 * search_hit_rate + 0.20 * known_answer_hit_rate + 0.20 * field_completeness_rate + 0.15 * schema_adherence_rate + 0.10 * (1 - stale_rate) + 0.10 * error_free_rate`

## Score Dimensions

| Dimension | Weight | Direction | Description |
| --- | ---: | --- | --- |
| `search_hit_rate` | 0.25 | higher is better | Fraction of asset_search and gameplay_tag_lookup tasks returning a valid non-error response (strict `>=1` result for `require_results` rows). |
| `known_answer_hit_rate` | 0.20 | higher is better | Fraction of known_answer tasks whose response contained the expected `/Game` object path. |
| `field_completeness_rate` | 0.20 | higher is better | Fraction of returned result items with at least 2 of 3 required fields (`match_object_path`, `match_value`, `match_source`), over expected-nonempty tasks. `0.0` (not vacuous) when those tasks return no rows. |
| `schema_adherence_rate` | 0.15 | higher is better | Fraction of schema_field_presence tasks where the discovered schema contains non-empty `planning_signals`. |
| `stale_rate` | 0.10 | lower is better | Fraction of health_check tasks where the response reports stale, error, or degraded status. |
| `error_free_rate` | 0.10 | higher is better | Fraction of all scored rows without transport errors or MCP `isError` responses. |

## Generate

```powershell
python Plugins\Monolith\Scripts\project_index_benchmark.py generate `
  --tasks Benchmarks\ProjectIndex\tasks.jsonl `
  --manifest Benchmarks\ProjectIndex\manifest.json
```

The generator produces 314 tasks from the checked-in schema-v2 fixtures: 176
`asset_search`, 67 `gameplay_tag_lookup`, 30 `known_answer`, 16 `health_check`,
1 `stats_check`, and 24
`schema_field_presence`.

## Offline Scoring Test

The scoring branches, strict fixture loading, mutable-root rejection,
disk/source-control gates, provider-unavailable no-overwrite behavior,
deterministic prefix-balanced selection, status/fixture-provenance preflight,
strict MCP response shape, transport gates, and success/failure artifact lifecycle have 23 offline
tests that need no live editor or network:

```powershell
python Plugins\Monolith\Scripts\test_project_index_benchmark.py
```

It monkeypatches `mcp_call` with fabricated MCP responses and asserts an all-empty
mock run scores `<= 0.30` (capped, `all_empty=true`) while a healthy mock run scores
above `0.9`. The runner tests also cover the third consecutive transport failure,
the 20th-sample 5% fraction gate, short-run finalization, success resetting the
consecutive counter, triggering-row preservation, stale-output cleanup, and
runner/protocol failure artifacts.

## Run

```powershell
python Plugins\Monolith\Scripts\project_index_benchmark.py run `
  --mcp-url http://localhost:9316/mcp `
  --tasks Plugins\Monolith\Benchmarks\ProjectIndex\tasks.jsonl `
  --label current `
  --output-dir Saved\Monolith\Benchmarks\ProjectIndex\current `
  --max-transport-failed-fraction 0.05 `
  --max-consecutive-transport-failures 3 `
  --min-transport-fraction-sample 20 `
  --request-timeout-s 12
```

Before scoring, the runner removes stale known outputs and requires a valid
`monolith_status` response with `server_running=true`. Raw MCP JSON must have an
object at the top level; parse failures, top-level lists/scalars, protocol errors,
and runner exceptions invalidate the run immediately instead of becoming index
quality scores.

For a canonical comparable run, the checked-in `live_fixtures` schema version,
project name, and catalog version must exactly match the live status identity.
A stale catalog version aborts at `fixture_provenance_preflight` before the first
task and writes only failure/partial diagnostics. Refresh against a healthy live
endpoint and regenerate; do not rewrite the provenance field by hand when the
endpoint is unavailable.

Transport failures use the shared benchmark policy:

- abort on the third consecutive task transport failure;
- after 20 attempted tasks, abort when the transport-failed fraction exceeds 5%;
- for runs shorter than 20 tasks, apply the same 5% fraction at finalization;
- a successful task resets the consecutive-failure counter;
- failure provenance records the last actual transport failure's task ID, HTTP
  status, and raw diagnostic even when a later success triggers the fraction gate.

Artifact publication is fail-closed:

| Run result | Written | Not written |
| --- | --- | --- |
| Valid completion | `summary.json`, final `per_task.json`, incremental `per_task.jsonl` | `run_failure.json`, `partial_summary.json` (the in-progress partial is removed) |
| Invalid status/config/protocol/runner/transport run | `run_failure.json`, invalid `partial_summary.json`; incremental `per_task.jsonl` only when tasks were attempted | normal `summary.json`, final `per_task.json` |

Invalid artifacts set `run_valid=false`, `metrics_valid=false`, and a bounded
`metrics_scope`. Protocol and runner failures preserve the triggering task row in
`per_task.jsonl`; task transport rows preserve `transport_status` and
`transport_error_raw`. Progress is printed as:

```
[N/total] task_id success=True
```

## Compare

```powershell
python Plugins\Monolith\Scripts\project_index_benchmark.py compare `
  --baseline Saved\Monolith\Benchmarks\ProjectIndex\baseline\summary.json `
  --current Saved\Monolith\Benchmarks\ProjectIndex\current\summary.json `
  --output-dir Saved\Monolith\Benchmarks\ProjectIndex\comparison
```

Produces `comparison.json` and `comparison.md` with a Baseline / Current /
Delta table for each metric.

## See Also

- [Benchmarks/README.md](../README.md) -- master benchmark index
