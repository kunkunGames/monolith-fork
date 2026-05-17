# Monolith — MonolithIndex Module

**Parent:** [SPEC_CORE.md](../SPEC_CORE.md)
**Engine:** Unreal Engine 5.7+
**Version:** 0.14.9 (Beta)

---

## MonolithIndex

**Dependencies:** Core, CoreUObject, Engine, MonolithCore, UnrealEd, AssetRegistry, Json, JsonUtilities, SQLiteCore, Slate, SlateCore, BlueprintGraph, KismetCompiler, EditorSubsystem

### Classes

| Class | Responsibility |
|-------|---------------|
| `FMonolithIndexModule` | Registers 13 project actions |
| `FMonolithIndexDatabase` | RAII SQLite wrapper. 13 tables + 2 FTS5 + 6 triggers + 1 meta. DELETE journal mode, 64MB cache. Schema v2: `saved_hash` column (Blake3 `FIoHash` hex), `schema_version` meta key |
| `FMonolithIndexReview` | CRG-inspired navigation/review over the existing `dependencies` graph plus rebuildable CRG projection/cache tables. Provides bounded BFS impact radius, read-only health, execute-gated FTS/CRG repair, cached risk with query-time fallback, and review-context packaging. Uses only the public `FMonolithIndexDatabase` surface — the DB impl file is untouched (additive, REQ-009) |
| `UMonolithIndexSubsystem` | UEditorSubsystem. 3-layer indexing (startup delta, live AR callbacks, full fallback). Hash-based startup catch-up. Live batched AR delegates on 2s timer. Deep asset indexing with game-thread batching. Batches every 100 assets. Progress notifications |
| `IMonolithIndexer` | Pure virtual interface: GetSupportedClasses(), IndexAsset(), GetName(), IsSentinel(), SupportsIncrementalIndex(), IndexScoped() |
| `FBlueprintIndexer` | Blueprint, WidgetBlueprint, AnimBlueprint — graphs, nodes, variables |
| `FMaterialIndexer` | Material, MaterialInstanceConstant, MaterialFunction — expressions, params, connections |
| `FAnimationIndexer` | AnimSequence, AnimMontage, BlendSpace, AnimBlueprint — tracks, notifies, slots, state machines |
| `FNiagaraIndexer` | NiagaraSystem, NiagaraEmitter — emitters, modules, parameters, renderers |
| `FDataTableIndexer` | DataTable — row names, struct type, column info |
| `FLevelIndexer` | World/MapBuildData — actors, components, sublevel references. **Editor-world skip invariant (v0.14.1, PR #28):** `IndexAsset` skips WorldPartition `Uninitialize` + `TryUnloadPackage` when the asset being indexed is the world currently open in the editor (`GEditor->GetEditorWorldContext().World()`). Prevents the indexer from tearing down the live editor WP world mid-session (fixes #27). |
| `FGameplayTagIndexer` | GameplayTag containers — tag hierarchies and references |
| `FConfigIndexer` | Config/INI files — sections, keys, values across config hierarchy |
| `FCppIndexer` | C++ source files — classes, functions, includes (project-level source) |
| `FGenericAssetIndexer` | StaticMesh, SkeletalMesh, Texture2D, SoundWave, etc. — metadata nodes |
| `FDependencyIndexer` | Hard + Soft package dependencies (runs after all other indexers) |
| `FMonolithIndexNotification` | Slate notification bar with throbber + percentage |

### Actions (13 — namespace: "project")

| Action | Params | Description |
|--------|--------|-------------|
| `search` | `query` (required), `limit` (50) | FTS5 full-text search across all indexed assets, nodes, variables, parameters |
| `find_references` | `asset_path` (required) | Bidirectional dependency lookup |
| `find_by_type` | `asset_type` (required), `limit` (100), `offset` (0) | Filter assets by class with pagination |
| `get_stats` | none | Row counts for all 13 tables + asset class breakdown (top 20) |
| `get_asset_details` | `asset_path` (required) | Deep inspection: nodes, variables, references for a single asset |
| `list_gameplay_tags` | `prefix`, `limit`, `offset` (optional) | List indexed gameplay tags, optionally filtered by prefix |
| `search_gameplay_tags` | `query` (required) | Search gameplay tags and return referencing assets |
| `impact_radius` | `asset_path` (required), `direction` (both), `max_depth` (2), `max_results` (200), `dependency_type` | Bounded BFS over `dependencies`: who is impacted within N hops (cycle-safe, `truncated` flag) |
| `health` | `include_counts` (true) | Read-only diagnostics: v2 schema, 6 triggers, FTS row parity, orphan deps, CRG projection table/index/parity checks, journal mode |
| `repair_fts` | `target` (all\|assets\|nodes), `execute` (false) | Rebuild `fts_assets`/`fts_nodes`. Dry-run unless `execute=true` (sole write gate); refused while `IsIndexing()` |
| `repair_crg_cache` | `scope` (all), `execute` (false) | Rebuild derived `crg_nodes`/`crg_edges`/`crg_node_metrics`/`crg_meta` from `assets` and `dependencies`. Dry-run unless `execute=true`; refused while `IsIndexing()` |
| `risk_score` | `asset_path`/`seed`, `limit` (20), `min_tier` (low) | Cached risk `{score,tier,reasons[],raw_counts,cache}` from CRG projection when present; safe query-time fallback on cache miss |
| `review_context` | `asset_path` (required), `direction` (both), `max_depth` (2), `max_results` (200), `detail_level` (minimal) | Token-efficient package: seed + impact + risk reasons + next actions; `minimal` omits full asset details |

### Database Schema

**13 Tables:** assets, nodes, connections, variables, parameters, dependencies, actors, tags, tag_references, configs, cpp_symbols, datatable_rows, meta

**2 FTS5 Virtual Tables:**
- `fts_assets` — content=assets, tokenize='porter unicode61', columns: asset_name, asset_class, description, package_path
- `fts_nodes` — content=nodes, tokenize='porter unicode61', columns: node_name, node_class, node_type

**DB Location:** `Plugins/Monolith/Saved/ProjectIndex.db`

**Derived CRG Projection Cache:** `crg_nodes`, `crg_edges`, `crg_node_metrics`, `crg_meta`.
These tables are rebuildable projections over `assets` and `dependencies`, not
source-of-truth tables. `project.repair_crg_cache execute=true` recreates the
projection, and `project.risk_score` reads `crg_node_metrics` first before
falling back to query-time scoring.

### Incremental Indexing

The project indexer uses a 3-layer architecture to keep `ProjectIndex.db` in sync without costly full rebuilds:

**Layer 1 — Startup Catch-Up (hash-based delta)**

On editor startup, `UMonolithIndexSubsystem` runs a fast delta engine:
1. `EnumerateAllPackages()` collects all discoverable `.uasset` packages with their `FIoHash` (Blake3).
2. Hash comparison against the `saved_hash` column in the `assets` table identifies added, removed, and changed assets. Move detection uses a `TMultiMap<FIoHash, FString>` to match removed→added pairs with identical hashes.
3. Delta application (inserts, updates, deletes, renames) executes in a single SQLite transaction.
4. Hash updates are deferred until after commit for crash recovery — if the editor crashes mid-index, the next startup re-detects the delta.

Performance: ~14K assets compared in ~20ms. <1s total startup time with no changes.

**Layer 2 — Live Asset Registry Callbacks**

Four AR delegates are registered at startup:
- `OnAssetsAdded` — new assets
- `OnAssetsRemoved` — deleted assets
- `OnAssetRenamed` — moved/renamed assets
- `OnAssetsUpdatedOnDisk` — externally modified assets

Events are batched into a pending queue and drained on a 2-second timer tick. The drain deduplicates entries (same asset touched multiple times within the window) and applies changes in a single transaction.

**Layer 3 — Forced Full Reindex (fallback)**

`monolith_reindex()` defaults to incremental mode (Layer 1 logic). Passing `force=true` triggers a full wipe-and-rebuild: drops all table data, re-enumerates, and re-indexes every asset. Used when the DB is suspected corrupt or after schema migrations.

**Schema v2 Migration**

Schema v2 adds:
- `saved_hash TEXT` column on the `assets` table (stores Blake3 `FIoHash` as hex string)
- `schema_version` key in the `meta` table
- Index on `saved_hash` for fast lookup

Migration is automatic: on startup, `PRAGMA table_info(assets)` checks for the `saved_hash` column. If missing, `ALTER TABLE assets ADD COLUMN saved_hash TEXT` runs followed by index creation.

**IMonolithIndexer Interface Additions**

| Method | Purpose |
|--------|---------|
| `IsSentinel()` | Returns true if this indexer acts as a sentinel for a specific asset type (used by incremental path to decide which indexers to invoke) |
| `SupportsIncrementalIndex()` | Returns true if the indexer can process individual asset changes without a full rebuild |
| `IndexScoped()` | Index a specific set of assets (subset of full index). Default implementation falls back to `IndexAsset()` per asset |

**Plugin Content Scope Fix**

The `bInstalled` filter on plugin content paths was replaced with explicit path enumeration. This fixes discovery of project-local plugins (e.g., DrawCallReducer, NiagaraDestructionDriver) that previously reported `bInstalled=false` and were excluded from indexing. The `MeshCatalogIndexer` paths were also corrected to use the new enumeration.

### CRG-Inspired Navigation + Projection Cache — IMPLEMENTED (P0, 2026-05-16; cache 2026-05-17)

The CRG-inspired review/navigation surface is **implemented** as 6 additive `project`
actions (`impact_radius`, `health`, `repair_fts`, `repair_crg_cache`, `risk_score`,
`review_context`) over the **existing** `dependencies` graph. The CRG `nodes`/`edges`
idea is adopted only as a derived SQLite projection/cache (`crg_*` tables), while
`assets` and `dependencies` remain authoritative. There is no Python runtime or
generic parser replacement (monolith-native: asset-domain, lexical/local). Logic lives in
`FMonolithIndexReview` (`Private/MonolithIndexReview.{h,cpp}`) using only the public
`FMonolithIndexDatabase` surface; the DB impl file is unchanged (REQ-009). Spec source:
`Plugins/Monolith/CRG/spec/monolith-crg-index-navigation-{prd,spec}.md`. Tests:
`Monolith.IndexGuard.Project.*` in `Private/Tests/MonolithIndexQueryTests.cpp`,
including cycle/truncation guards, orphan-dependency health warnings, repair dry-run/execute,
CRG cache rebuild/cache-hit coverage, and minimal review-context output-contract coverage.

Invariants honored by the implementation:

- `ProjectIndex.db` is **Schema v2** (`schema_version` meta key + `assets.saved_hash` column/index, `PRAGMA table_info` migration). `project.health` must validate v2, not generic v1.
- 6 FTS triggers (`fts_assets`/`fts_nodes` × ai/ad/au) are external-content FTS5 → `'rebuild'` is valid for `repair_fts`.
- `FMonolithIndexDatabase` exposes a raw `FSQLiteDatabase*` (`GetRawDatabase()`) with **no DB-internal lock**; writes are caller-serialized. `repair_fts` and `repair_crg_cache` must gate on `UMonolithIndexSubsystem::IsIndexing()` and run inside transaction-scoped helpers.
- CRG projection rows are disposable: `crg_nodes` maps one row per asset, `crg_edges` maps one row per dependency, and `crg_node_metrics` stores `risk_score`, tier, reasons JSON, and raw count JSON. Missing projection rows are cache misses, not action failures.
- Direct lookup helpers to build bounded traversal on: `GetDependenciesForAsset` (out / `source_asset_id`), `GetReferencersOfAsset` (in / `target_asset_id`).
- P0 action outputs expose a stable review contract: `input`, `limits`, `truncated`, and `next_actions` where applicable; `project.review_context` additionally exposes compact `top_risks[]` and `context[]` fields so agents can triage without pulling full details.
- Test precedent: extend `Private/Tests/MonolithIndexQueryTests.cpp` (`Monolith.IndexGuard.Project.*`, temp-DB fixture) — do not introduce a new directory or `WITH_DEV_AUTOMATION_TESTS` guard.

---
