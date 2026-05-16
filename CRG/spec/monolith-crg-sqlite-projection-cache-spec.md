---
doc_type: "spec_source"
schema_version: "2"
status: "implemented"
stage: "verified"
topic_slug: "monolith-crg-sqlite-projection-cache"
linked_prd: "./monolith-crg-index-navigation-prd.md"
traceability_mode: "req-task-test"
---

# SPEC SOURCE: Monolith CRG SQLite Projection Cache

## Goal

Add a SQLite-native, rebuildable CRG projection/cache on top of the existing
Monolith indexes.

The existing domain indexes remain the only source of truth:

- `ProjectIndex.db`: `assets`, `dependencies`, graph/detail tables, FTS.
- `EngineSource.db`: `symbols`, `"references"`, `inheritance`, source FTS.

The CRG tables are derived materialized projection tables. They can be dropped
and rebuilt from the source indexes at any time. They must not become an
independent parser, source DB, or alternate authority.

## Verified Current State

- `project.impact_radius` and `source.impact_radius` traverse the native
  dependency/reference tables directly.
- `project.risk_score` and `source.risk_score` read `crg_node_metrics` after a
  successful `repair_crg_cache` rebuild and fall back to query-time scoring when
  the derived cache is absent.
- `project.review_context` and `source.review_context` use the same risk helpers,
  so their `risk` objects share cache-hit/cache-miss behavior.
- Measured offline CLI latency showed `review_context` is already about 2.6x
  faster than an agent chaining three separate actions, but repeated health/risk
  and broad source checks still benefit from materialized metrics.
- CRG's useful schema ideas are generic `nodes`, `edges`, metadata, FTS, flow
  summaries, and metric/risk projections. Monolith should adopt the projection
  and metric pattern, not the Python parser/runtime or generic source-of-truth
  replacement.

## Non-Goals

- Do not merge `ProjectIndex.db` and `EngineSource.db`.
- Do not add a Python CRG runtime dependency.
- Do not make CRG projection tables authoritative.
- Do not add embeddings, communities, flows, wiki export, daemon ports, or D3
  visualization in this change.
- Do not change existing direct action output shapes.

## Schema Contract

Each DB owns its own CRG projection. The schema names are shared so offline and
live tooling can query them consistently.

```sql
CREATE TABLE IF NOT EXISTS crg_nodes (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  domain TEXT NOT NULL,              -- project | source
  native_table TEXT NOT NULL,        -- assets | symbols
  native_id INTEGER NOT NULL,
  stable_key TEXT NOT NULL,          -- /Game/... or qualified symbol plus id
  kind TEXT NOT NULL,
  name TEXT NOT NULL,
  path TEXT DEFAULT '',
  module TEXT DEFAULT '',
  source_revision TEXT DEFAULT '',
  extra TEXT DEFAULT '{}',
  updated_at INTEGER NOT NULL,
  UNIQUE(domain, native_table, native_id),
  UNIQUE(domain, stable_key)
);

CREATE TABLE IF NOT EXISTS crg_edges (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  domain TEXT NOT NULL,
  source_node_id INTEGER NOT NULL,
  target_node_id INTEGER NOT NULL,
  edge_kind TEXT NOT NULL,           -- dependency | call | type | inheritance
  edge_subkind TEXT DEFAULT '',      -- Hard | Soft | ref_kind
  weight REAL NOT NULL DEFAULT 1.0,
  native_table TEXT NOT NULL,
  native_id INTEGER NOT NULL,
  updated_at INTEGER NOT NULL
);

CREATE TABLE IF NOT EXISTS crg_node_metrics (
  node_id INTEGER PRIMARY KEY,
  fan_in INTEGER NOT NULL DEFAULT 0,
  fan_out INTEGER NOT NULL DEFAULT 0,
  hard_in INTEGER NOT NULL DEFAULT 0,
  descendants INTEGER NOT NULL DEFAULT 0,
  risk_score REAL NOT NULL DEFAULT 0.0,
  risk_tier TEXT NOT NULL DEFAULT 'low',
  reasons_json TEXT NOT NULL DEFAULT '[]',
  raw_counts_json TEXT NOT NULL DEFAULT '{}',
  scoring_version TEXT NOT NULL DEFAULT '1',
  computed_at INTEGER NOT NULL
);

CREATE TABLE IF NOT EXISTS crg_meta (
  key TEXT PRIMARY KEY,
  value TEXT NOT NULL
);
```

Required indexes:

- `crg_nodes(domain, native_table, native_id)`
- `crg_nodes(domain, stable_key)`
- `crg_edges(domain, source_node_id)`
- `crg_edges(domain, target_node_id)`
- `crg_edges(domain, edge_kind, edge_subkind)`
- `crg_node_metrics(risk_score DESC)`

## Rebuild Contract

`project.repair_crg_cache` and `source.repair_crg_cache` are explicit repair
actions.

Parameters:

- `execute=false` by default. When false, return a plan and counts only.
- `scope=all` for now. Future scopes may add `nodes`, `edges`, `metrics`.

Behavior:

- Dry-run never mutates.
- Execute creates the CRG tables if missing.
- Execute deletes only CRG projection rows, never source tables.
- Execute repopulates `crg_nodes`, `crg_edges`, `crg_node_metrics`, and
  `crg_meta`.
- Execute is rejected while the owning subsystem is indexing.
- `crg_meta.cache_version` is `1`.
- `crg_meta.scoring_version` is `2` for projection-backed risk scoring.
- `crg_meta.built_at` is written from SQLite `datetime('now')`.

## Projection Mapping

### ProjectIndex.db

- `assets` -> `crg_nodes(domain='project', native_table='assets')`
- `dependencies` -> `crg_edges(edge_kind='dependency')`
- `dependency_type` -> `edge_subkind`
- `saved_hash` -> `source_revision`
- risk metrics:
  - `fan_in`: inbound dependencies
  - `fan_out`: outbound dependencies
  - `hard_in`: inbound hard dependencies
  - `raw_counts_json`: inbound/outbound/hard/nodes/variables/parameters/tags/class weight
  - `risk_score`: same factor family as query-time scoring

### EngineSource.db

- `symbols` -> `crg_nodes(domain='source', native_table='symbols')`
- `"references"` -> `crg_edges(edge_kind=ref_kind)`
- `inheritance` -> `crg_edges(edge_kind='inheritance')`
- `files.path` -> `path`
- risk metrics:
  - `fan_in`: references to symbol
  - `fan_out`: references from symbol
  - `descendants`: direct inheritance children
  - `raw_counts_json`: callers/callees/descendants/ancestors/caller_files/is_ue_macro
  - `risk_score`: same factor family as query-time scoring

## Query Contract

Existing actions keep their names.

- `risk_score` prefers `crg_node_metrics` when cache status is `ok`.
- `risk_score` falls back to query-time scoring when the cache is missing,
  stale, corrupt, or the seed is absent from projection tables.
- `review_context` may keep using bounded traversal directly, but its `risk`
  object must come from the same `risk_score` helper so cache/fallback behavior
  is consistent.
- `health` includes CRG projection checks:
  - required CRG tables/indexes
  - node count parity against `assets` or `symbols`
  - orphan CRG edges
  - `crg_meta.cache_version`
  - `crg_meta.scoring_version`
- outputs include `cache` metadata when using cached risk:
  - `cache.status`: `hit|miss|stale|rebuilt|unavailable`
  - `cache.version`
  - `cache.scoring_version`

## Requirements

- [REQ-001] Add CRG projection tables to both SQLite databases as derived cache.
- [REQ-002] Add execute-gated repair actions for project/source CRG cache rebuild.
- [REQ-003] Add CRG projection health checks to project/source health.
- [REQ-004] Make risk scoring prefer cached metrics and fall back safely.
- [REQ-005] Keep direct action behavior and existing output shapes compatible.
- [REQ-006] Keep offline `monolith_query.exe` behavior compatible; offline cache
  use can be read-only in this change, while offline rebuild may follow later if
  not needed for acceptance.

## Tasks

- [TSK-001] ProjectIndex: create/rebuild `crg_*` tables from `assets` and `dependencies`.
- [TSK-002] Source: create/rebuild `crg_*` tables from `symbols`, `"references"`, and `inheritance`.
- [TSK-003] Register `project.repair_crg_cache` and `source.repair_crg_cache`.
- [TSK-004] Extend `health` to report CRG cache status.
- [TSK-005] Update risk helpers to use cached metrics when present.
- [TSK-006] Add automation tests for dry-run, execute rebuild, health warning,
  and risk cache hit/fallback.
- [TSK-007] Update API/spec/testing docs and PR body.

## Tests

- [TEST-001] Project temp DB: `repair_crg_cache execute=false` returns a plan and
  leaves `crg_nodes` absent or unchanged.
- [TEST-002] Project temp DB: `repair_crg_cache execute=true` creates rows equal
  to source asset/dependency counts and health reports CRG ok.
- [TEST-003] Project risk cache hit: after rebuild, `project.risk_score` includes
  cache metadata and returns the expected tier/score field.
- [TEST-004] Source temp DB: `repair_crg_cache execute=true` creates rows equal
  to source symbol/reference/inheritance counts and health reports CRG ok.
- [TEST-005] Source risk cache hit: after rebuild, `source.risk_score` includes
  cache metadata and returns the expected tier/score field.
- [TEST-006] Regression: existing `Monolith.IndexGuard.*` tests still pass.

## Traceability

- REQ-001 -> TSK-001, TSK-002 -> TEST-002, TEST-004
- REQ-002 -> TSK-003 -> TEST-001, TEST-002, TEST-004
- REQ-003 -> TSK-004 -> TEST-002, TEST-004
- REQ-004 -> TSK-005 -> TEST-003, TEST-005
- REQ-005 -> TSK-006 -> TEST-006
- REQ-006 -> TSK-007 -> TEST-006

## Implementation Notes

- Use CRG's `nodes`/`edges` naming because it is familiar to agent tooling, but
  add `domain`, `native_table`, and `native_id` so the rows remain projections
  of Monolith-native tables.
- Store JSON reasons/raw counts as text to avoid adding new UObject structs or
  changing public action output shapes.
- Do not create triggers for CRG tables in this change. Rebuild after indexing
  and explicit repair are simpler and safer than trying to mirror every source
  write path.
- A missing cache is not an error for `risk_score`; it is a cache miss.
