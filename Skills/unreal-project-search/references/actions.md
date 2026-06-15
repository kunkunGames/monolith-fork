# unreal-project-search — Action Reference

Detailed per-action parameter signatures for the Monolith **project** namespace, called via `project_query({ action, params })`. These are a snapshot of the live catalog — confirm the exact full schema via `monolith_discover({ namespace: "project", action: "<action>", mode: "schema" })`. The discover-first block in `../SKILL.md` is the authority.

Param notation: `name*` required, `name?` optional, `name=val` default, `a/b/c` allowed, `[w]` mutates (transaction-wrapped — for `impact_radius`/`risk_score`/`pre_merge_check`/`snapshot`/`repair_*` this is index-cache bookkeeping, not asset edits). Signatures are a snapshot of the live catalog — for the exact full schema call `monolith_discover` with `mode: "schema"` (the discover block above is the authority).

### Search & inspect

| Action | Params | Purpose |
|--------|--------|---------|
| `search` | `query*` `limit?=50` `include_content?=true` | Full-text search across indexed assets plus graph/content signals. `include_content=false` for asset/node-only search |
| `find_references` | `asset_path*` | Assets that reference OR are referenced by the given asset |
| `find_by_type` | `asset_type*` (aliases: `asset_class`/`type`) `module?` `limit?=100` `offset?=0` | List all assets of a type, optionally filtered by plugin/module |
| `get_asset_details` | `asset_path*` | Deep details: nodes, variables, parameters, dependencies |
| `get_saved_asset_state` | `asset_path*` | Disk-backed state: class, package, disk path, size, mtime, deps, referencers |
| `get_stats` | (none) | Index statistics — counts by table and asset-class breakdown |
| `impact_radius` [w] | `asset_path*` `direction?=both` (in/out/both) `max_depth?=2` `max_results?=200` `dependency_type?` (Hard/Soft/...) | Bounded-BFS dependency blast radius |
| `refresh_assets` [w] | `asset_paths*` `wait_for_asset_registry?=true` `wait_for_disk?=false` | Force a synchronous asset-registry rescan of paths (post-save freshness) |

### Gameplay tags

| Action | Params | Purpose |
|--------|--------|---------|
| `list_gameplay_tags` | `prefix?` `limit?=100` `offset?=0` | List indexed GameplayTags, optionally filtered by prefix |
| `search_gameplay_tags` | `query*` `limit?=100` `offset?=0` | Search GameplayTags by substring, with referencing assets |

### Review & risk (use before code/asset-review claims)

| Action | Params | Purpose |
|--------|--------|---------|
| `risk_score` [w] | `asset_path?` (alias `seed`; omit to rank top fan-in) `limit?=20` `min_tier?=low` (low/medium/high) | Change-risk score (fan-in, hard deps, class weight, graph density) |
| `review_context` | `asset_path*` `direction?=both` (in/out/both) `max_depth?=2` `max_results?=200` `detail_level?=minimal` (minimal/standard) | Token-efficient review package: seed + impact + risk + next actions |
| `review_hotspots` | `kind?=all` (fan_in/fan_out/risk/large/all) `limit?=50` `min_lines?=100` `include_questions?=true` | Project-wide review hotspots |
| `find_unused` | `kind?=all` `limit?=100` `min_confidence?=low` (low/medium/high) | Advisory orphan-asset candidates with confidence + reasons |
| `audit_orphan_assets` [w] | `asset_class_filter?` `limit?=50` (cap 200) `cursor?` | Strictest orphan signal: zero registry referencers AND zero cpp_asset_edges |
| `detect_changes` | `changed_paths?` (array or comma string; alias `paths`) `max_results?=200` `detail_level?=minimal` (minimal/standard) | Map changed paths to impact + risk-ranked review priorities |
| `pre_merge_check` [w] | `changed_paths?` (alias `paths`) `max_results?=200` `unused_limit?=20` `detail_level?=minimal` (minimal/standard) `include_unused?=true` | Pre-merge decision: health + change risk + impact + optional unused |
| `cleanup_generated_assets` [w] | `paths*` (allowlist `/Game/Tests/Monolith/` only) `dry_run?=true` `require_no_referencers?=true` `remove_empty_folders?=false` | Safely delete throwaway test assets with reference checks |

### Snapshots & maintenance

| Action | Params | Purpose |
|--------|--------|---------|
| `snapshot` [w] | `label?` (default `project-<utc_ticks>`) `execute?=false` | Capture an index CRG projection snapshot for later diffing |
| `diff_snapshots` | `before*` `after?=current` `limit?=100` | Diff two index snapshots |
| `health` | `include_counts?=true` | Index diagnostics: schema, triggers, FTS parity, orphans, journal mode |
| `repair_fts` [w] | `target?=all` (all/assets/nodes/variables/parameters/datatable_rows/actors/asset_search_values) `execute?=false` | Rebuild project FTS tables when search looks stale |
| `repair_crg_cache` [w] | `scope?=all` `execute?=false` | Rebuild project CRG projection/cache |
