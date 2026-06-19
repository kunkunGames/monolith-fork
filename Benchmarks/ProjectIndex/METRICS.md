# ProjectIndex Benchmark Metrics

## Primary Score

`project_index_score` -- composite quality score in [0.0, 1.0]. Higher is better.

```
project_index_score = 0.25 * search_hit_rate
                    + 0.20 * known_answer_hit_rate
                    + 0.20 * field_completeness_rate
                    + 0.15 * schema_adherence_rate
                    + 0.10 * (1 - stale_rate)
                    + 0.10 * error_free_rate
```

## Integrity Guard (empty / broken index)

An empty or broken index used to score a perfect `1.000`: every `asset_search` task
passed leniently on a valid empty response, and `field_completeness_rate` returned a
vacuous `1.0` when no results existed. Two changes close that loophole:

- **`known_answer` tasks** carry a ground-truth `expected_object_path` and require
  `min_results >= 1`. They are a HIT only when the response's `match_object_path`
  values actually contain the expected `/Game` asset path, so an empty index scores
  `known_answer_hit_rate = 0.0`.
- **`field_completeness_rate` is computed over expected-nonempty tasks** (every
  `known_answer` task plus any `require_results` search). If those tasks returned no
  result rows at all, the rate is `0.0` (penalized), not a vacuous `1.0`.
- **`all_empty` cap.** When every result-bearing task returns zero results, the run is
  flagged `all_empty=true`, a loud `[ALL-EMPTY]` warning is printed to stderr, and
  `project_index_score` is capped at `0.30` (`all_empty_score_cap` in the manifest).

With these, an all-empty run scores `<= 0.30` instead of `~1.0`.

## Metric Definitions

| Metric | Direction | Formula | Notes |
| --- | --- | --- | --- |
| `project_index_score` | higher is better | Weighted composite (see above) | Primary score; range [0.0, 1.0]; capped at `0.30` when `all_empty` |
| `search_hit_rate` | higher is better | `count(direct_success) / count(asset_search + gameplay_tag_lookup)` | Lenient for `min_results:0` rows (empty OK if valid non-error JSON); strict for `require_results` rows (needs >=1 result) |
| `known_answer_hit_rate` | higher is better | `count(direct_success) / count(known_answer tasks)` | Ground-truth recall: a HIT requires the expected `/Game` `expected_object_path` to appear in the response's `match_object_path` values AND `min_results>=1`. An empty/broken index scores 0.0 |
| `field_completeness_rate` | higher is better | `count(complete results) / count(results) over expected-nonempty tasks` | Required fields: `match_object_path`, `match_value`, `match_source`. Computed over `known_answer` + `require_results` tasks; `0.0` (not vacuous `1.0`) when those tasks returned no rows |
| `all_empty` | lower is better | `true` iff every result-bearing task returned 0 results | Loud integrity flag; prints `[ALL-EMPTY]` warning and caps `project_index_score` at `0.30` |
| `schema_adherence_rate` | higher is better | `count(planning_signals present) / count(schema_field_presence tasks)` | Checks non-empty `planning_signals` list and non-empty `skill` in discovered schema |
| `stale_rate` | lower is better | `count(stale or error health responses) / count(health_check tasks)` | Status values treated as stale: `error`, `stale`, `degraded`, `unavailable`, `unknown`, empty. Counts `health_check` rows only (`project.health`); `get_stats` is excluded |
| `stats_check_rate` | higher is better | `count(direct_success) / count(stats_check tasks)` | `project.get_stats` contract check: `success=true`, `indexing` present, `stats` is a dict. Informational only -- not folded into `project_index_score` |
| `error_free_rate` | higher is better | `1 - error_count / task_count` | Penalizes transport errors and MCP `isError` responses across all scored rows |
| `task_count` | informational | Total scored rows | Should match `manifest.json` task_count |
| `error_count` | lower is better | Count of rows with transport_error or isError response | Network/MCP errors unrelated to index quality |

## Score Weights Rationale

| Weight | Metric | Rationale |
| ---: | --- | --- |
| 0.25 | search_hit_rate | Core utility: agents depend on project search returning responses to find assets |
| 0.20 | known_answer_hit_rate | Ground truth: the index must actually return known assets; this is the dimension an empty/broken index cannot fake |
| 0.20 | field_completeness_rate | Richness: result items need `match_object_path`, `match_value`, `match_source` for agents to act on them |
| 0.15 | schema_adherence_rate | Planning quality: schemas with `planning_signals` guide agents toward correct actions |
| 0.10 | 1 - stale_rate | Index freshness: stale or errored health signals risk unreliable search results |
| 0.10 | error_free_rate | Reliability: repeated live endpoint or handler errors should reduce the overall quality score |

Weights sum to `1.00`.

## Task Category Counts

| Category | Count | Tool | Description |
| --- | ---: | --- | --- |
| `asset_search` | 176 | `project_query` | Search for common UE asset/gameplay terms plus GAS, Niagara, UMG, animation, audio, world, import-oriented terms, and session/log-derived Monolith workflow terms |
| `gameplay_tag_lookup` | 67 | `project_query` | List and search gameplay tags with combat, ability, UI, input, quest, cue, and status params |
| `known_answer` | 30 | `project_query` | Ground-truth recall: search a distinctive, project-unique asset name and assert the response contains its exact `/Game` object path (`require_results`, HIT-checked) |
| `health_check` | 16 | `project_query` | `project.health` status checks (the authoritative health endpoint) |
| `stats_check` | 1 | `project_query` | `project.get_stats` success/indexing/stats contract check |
| `schema_field_presence` | 40 | `monolith_discover` | Schema discovery for project namespace actions |

Total: **330** tasks.

## Required Result Fields

Project search result items are checked for at least 2 of these 3 fields (from the
Monolith project search response contract):

- `match_object_path` -- asset object path in the project (e.g. `/Game/BP_MyActor.BP_MyActor`)
- `match_value` -- the matched content value
- `match_source` -- which FTS table the match came from (e.g. `fts_assets`, `fts_nodes`)
