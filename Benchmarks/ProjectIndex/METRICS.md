# ProjectIndex Benchmark Metrics

## Primary Score

`project_index_score` -- composite quality score in [0.0, 1.0]. Higher is better.

```
project_index_score = 0.40 * search_hit_rate
                    + 0.30 * field_completeness_rate
                    + 0.20 * schema_adherence_rate
                    + 0.10 * (1 - stale_rate)
```

## Metric Definitions

| Metric | Direction | Formula | Notes |
| --- | --- | --- | --- |
| `project_index_score` | higher is better | Weighted composite (see above) | Primary score; range [0.0, 1.0] |
| `search_hit_rate` | higher is better | `count(direct_success) / count(asset_search + gameplay_tag_lookup)` | Lenient: empty result set is OK if the response is a valid non-error JSON object |
| `field_completeness_rate` | higher is better | `count(results with >=2 required fields) / count(all returned result items)` | Required fields: `match_object_path`, `match_value`, `match_source`; vacuously 1.0 when no results returned anywhere |
| `schema_adherence_rate` | higher is better | `count(planning_signals present) / count(schema_field_presence tasks)` | Checks non-empty `planning_signals` list and non-empty `skill` in discovered schema |
| `stale_rate` | lower is better | `count(stale or error health responses) / count(health_check tasks)` | Status values treated as stale: `error`, `stale`, `degraded`, `unavailable`, `unknown`, empty. Counts `health_check` rows only (`project.health`); `get_stats` is excluded |
| `stats_check_rate` | higher is better | `count(direct_success) / count(stats_check tasks)` | `project.get_stats` contract check: `success=true`, `indexing` present, `stats` is a dict. Informational only -- not folded into `project_index_score` |
| `task_count` | informational | Total scored rows | Should match `manifest.json` task_count |
| `error_count` | lower is better | Count of rows with transport_error or isError response | Network/MCP errors unrelated to index quality |

## Score Weights Rationale

| Weight | Metric | Rationale |
| ---: | --- | --- |
| 0.40 | search_hit_rate | Core utility: agents depend on project search returning responses to find assets |
| 0.30 | field_completeness_rate | Richness: result items need `match_object_path`, `match_value`, `match_source` for agents to act on them |
| 0.20 | schema_adherence_rate | Planning quality: schemas with `planning_signals` guide agents toward correct actions |
| 0.10 | 1 - stale_rate | Index freshness: stale or errored health signals risk unreliable search results |

## Task Category Counts

| Category | Count | Tool | Description |
| --- | ---: | --- | --- |
| `asset_search` | 170 | `project_query` | Search for common UE asset/gameplay terms plus GAS, Niagara, UMG, animation, audio, world, and import-oriented terms |
| `gameplay_tag_lookup` | 70 | `project_query` | List and search gameplay tags with combat, ability, UI, input, quest, cue, and status params |
| `health_check` | 19 | `project_query` | `project.health` status checks (the authoritative health endpoint) |
| `stats_check` | 1 | `project_query` | `project.get_stats` success/indexing/stats contract check |
| `schema_field_presence` | 40 | `monolith_discover` | Schema discovery for project namespace actions |

## Required Result Fields

Project search result items are checked for at least 2 of these 3 fields (from the
Monolith project search response contract):

- `match_object_path` -- asset object path in the project (e.g. `/Game/BP_MyActor.BP_MyActor`)
- `match_value` -- the matched content value
- `match_source` -- which FTS table the match came from (e.g. `fts_assets`, `fts_nodes`)
