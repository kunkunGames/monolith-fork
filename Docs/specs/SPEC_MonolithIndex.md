# Monolith — MonolithIndex Module

**Parent:** [SPEC_CORE.md](../SPEC_CORE.md)
**Engine:** Unreal Engine 5.7+
**Version:** 0.18.1 (Beta)

---

## MonolithIndex

**Dependencies:** Core, CoreUObject, Engine, MonolithCore, UnrealEd, AssetRegistry, Json, JsonUtilities, SQLiteCore, Slate, SlateCore, BlueprintGraph, KismetCompiler, EditorSubsystem

**UE 5.7 unity-build rule:** action `.cpp` helpers must use action-specific names even inside anonymous namespaces. Adaptive unity can include sibling action files in a single generated translation unit, so generic helpers like `AppendPathString`, `AppendPathField`, and `CollectChangedPaths` must not be duplicated across Project action sources.

### Classes

| Class | Responsibility |
|-------|---------------|
| `FMonolithIndexModule` | Registers 11 project actions (7 baseline + 1 v0.17.0 cross-module `audit_orphan_assets` + 3 test/profiling harness Wave 1) |
| `FMonolithIndexDatabase` | RAII SQLite wrapper. 13 tables + 2 FTS5 + 6 triggers + 1 meta. DELETE journal mode, 64MB cache. Schema v2: `saved_hash` column (Blake3 `FIoHash` hex), `schema_version` meta key |
| `UMonolithIndexSubsystem` | UEditorSubsystem. 3-layer indexing (startup delta, live AR callbacks, full fallback). Hash-based startup catch-up. Live batched AR delegates on 2s timer. Deep asset indexing with game-thread batching. Batches every 100 assets. Progress notifications |
| `IMonolithIndexer` | Pure virtual interface: GetSupportedClasses(), IndexAsset(), GetName(), IsSentinel(), SupportsIncrementalIndex(), IndexScoped() |
| `FBlueprintIndexer` | Blueprint, WidgetBlueprint, AnimBlueprint — graphs, nodes, variables |
| `FMaterialIndexer` | Material, MaterialInstanceConstant, MaterialFunction — expressions, params, connections |
| `FAnimationIndexer` | AnimSequence, AnimMontage, BlendSpace, AnimBlueprint — tracks, notifies, slots, state machines |
| `FNiagaraIndexer` | NiagaraSystem, NiagaraEmitter — emitters, modules, parameters, renderers |
| `FDataTableIndexer` | DataTable — row names, struct type, column info |
| `FLevelIndexer` | World/MapBuildData — actors, components, sublevel references. **Editor-world skip invariant (v0.14.1, PR #28):** `IndexAsset` skips WorldPartition `Uninitialize` + `TryUnloadPackage` when the asset being indexed is the world currently open in the editor (`GEditor->GetEditorWorldContext().World()`). Prevents the indexer from tearing down the live editor WP world mid-session (fixes #20/#27). **Landscape-world safe-teardown invariant (issue #67):** worlds loaded purely to enumerate placed actors are torn down + unloaded after enumeration. For a world carrying a `ULandscapeSubsystem`, the indexer first unregisters every landscape proxy's components (`AActor::UnregisterAllComponents`, world-wide) — nulling the grass-builder state so the subsystem's `Deinitialize` no longer dereferences a null render scene — then drives `UWorld::CleanupWorld`, which clears the world subsystem collection's `bInitialized` (no GC ensure) and tears the world down normally, with no residency cost. Non-landscape worlds use the existing WorldPartition-uninit + unload path. A bare `CleanupWorld` without the unregister-first step is unsafe (it deinitializes the landscape subsystem while a grass-builder still references the null render scene, crashing) — hence the ordering. Runtime-verified: a full reindex with level indexing enabled completes with zero ensures, zero crashes, and all landscape worlds torn down (no residency) on landscape-heavy projects. |
| `FGameplayTagIndexer` | GameplayTag containers — tag hierarchies and references |
| `FConfigIndexer` | Config/INI files — sections, keys, values across config hierarchy |
| `FCppIndexer` | C++ source files — classes, functions, includes (project-level source) |
| `FGenericAssetIndexer` | StaticMesh, SkeletalMesh, Texture2D, SoundWave, etc. — metadata nodes |
| `FDependencyIndexer` | Hard + Soft package dependencies (runs after all other indexers) |
| `FMonolithIndexNotification` | Slate notification bar with throbber + percentage |

> **Shared read-side serializer (2026-06-07).** The DataAsset indexer's `PropertyToJsonValue` field serializer was deduplicated into the new `FMonolithReflectionReader` helper in `MonolithCore` (see [`SPEC_MonolithCore.md`](SPEC_MonolithCore.md)). The indexer now calls the shared reader instead of carrying its own copy — the same single implementation the Blueprint CDO actions (`get_cdo_properties`) and `seed_data_asset`'s `read_back_values` use, so indexed DataAsset field JSON and live verify-after-write JSON are produced by one code path.

### Actions (11 — namespace: "project")

| Action | Params | Description |
|--------|--------|-------------|
| `search` | `query` (required), `limit` (50), `include_content` (true) | FTS full-text search across indexed assets and graph/content signals. Inputs are automatically escaped and tokenized for safe prefix matching. Default content-inclusive mode covers assets, nodes, variables, parameters, DataTable rows, actors, and curated `asset_search_values`; `include_content=false` keeps legacy asset/node-only behavior |
| `find_references` | `asset_path` (required) | Bidirectional dependency lookup |
| `find_by_type` | `asset_type` (required), `limit` (100), `offset` (0) | Filter assets by class with pagination |
| `get_stats` | none | Row counts for all 13 tables + asset class breakdown (top 20) |
| `get_asset_details` | `asset_path` (required) | Deep inspection: nodes, variables, references for a single asset |
| `list_gameplay_tags` | `prefix` (optional) | List indexed gameplay tags, optionally filtered by prefix |
| `search_gameplay_tags` | `query` (required) | Search gameplay tags and return referencing assets |
| `audit_orphan_assets` | `asset_class_filter` (optional), `limit` (50, cap 200), `cursor` (optional) | **v0.17.0 (cross-module from `MonolithReflectionIntel`).** List `/Game/.../*.uasset` assets with ZERO `IAssetRegistry` referencers AND zero entries in `cpp_asset_edges`. Strictest orphan signal for pre-release cleanup. Excludes `/Engine/*` + `/Memory/*`. Read-only, cursor-paginated |

**Test/Profiling Harness — Wave 1 (3 — post-save freshness / disk state / sandboxed cleanup)**

| Action | Params | Description |
|--------|--------|-------------|
| `refresh_assets` | `asset_paths[]` (required), `wait_for_asset_registry` (default true), `wait_for_disk` (default false) | Force a synchronous asset-registry rescan of the requested `/Game/...` package or directory paths (post-save freshness). `wait_for_asset_registry` drains pending registry work so subsequent queries see fresh state; `wait_for_disk` bounded-polls until each package's backing file exists with size > 0 |
| `get_saved_asset_state` | `asset_path` (required) | Return disk-backed state for an asset — class, package, disk path, file size, mtime, dependencies, and referencers |
| `cleanup_generated_assets` | `paths[]` (required), `dry_run` (default true), `require_no_referencers` (default true), `remove_empty_folders` (default false) | Safely delete generated throwaway assets with reference checks. **HARD allowlist guard:** refuses any path outside `/Game/Tests/Monolith/`. Dry-run by default (reports what would be deleted without touching disk); `require_no_referencers` skips any asset still referenced from outside the request set; `remove_empty_folders` prunes now-empty folders under the allowlist |

### Database Schema

**14 Authoritative Tables:** assets, nodes, connections, variables, parameters, dependencies, actors, tags, tag_references, configs, cpp_symbols, datatable_rows, asset_search_values, meta

**7 FTS5 Virtual Tables:**
- `fts_assets` — content=assets, tokenize='porter unicode61', columns: asset_name, asset_class, description, package_path, module_name
- `fts_nodes` — content=nodes, tokenize='porter unicode61', columns: node_name, node_class, node_type
- `fts_variables` — content=variables, tokenize='porter unicode61', columns: var_name, var_type, category, default_value
- `fts_parameters` — content=parameters, tokenize='porter unicode61', columns: param_name, param_type, param_group, default_value, source
- `fts_datatable_rows` — content=datatable_rows, tokenize='porter unicode61', columns: row_name
- `fts_actors` — content=actors, tokenize='porter unicode61', columns: actor_name, actor_class, actor_label
- `fts_asset_search_values` — content=asset_search_values, tokenize='porter unicode61', columns: value_text

`asset_search_values` stores curated supplemental values that do not belong in a primary structured table column but are high-signal for search, including Blueprint comments, pin/default text, and DataTable cell values. This keeps the schema modular while letting `project.search include_content=true` find content that old asset/name search missed.

**DB Location:** `Plugins/Monolith/Saved/ProjectIndex.db`

**Derived CRG Projection Cache:** `crg_nodes`, `crg_edges`, `crg_node_metrics`, `crg_meta`, `crg_snapshots`.
These tables are rebuildable projections over `assets` and `dependencies`, not
source-of-truth tables. `project.repair_crg_cache execute=true` purges stale/orphan metrics, recreates the
projection, and `project.risk_score` reads `crg_node_metrics` first before
falling back to query-time scoring. Rebuilt projection metrics use
`crg_meta.scoring_version=3` for the UE-domain sensitivity factor. `crg_snapshots`
is a derived review aid created only by `project.snapshot execute=true`; it stores
compact node/edge manifests and may be dropped/recreated without losing source data.

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

The CRG-inspired review/navigation surface is **implemented** as 12 additive `project`
actions (`impact_radius`, `health`, `repair_fts`, `repair_crg_cache`, `risk_score`,
`detect_changes`, `find_unused`, `pre_merge_check`, `snapshot`, `diff_snapshots`, `review_hotspots`, `review_context`) over the **existing** `dependencies` graph. The CRG `nodes`/`edges`
idea is adopted only as a derived SQLite projection/cache (`crg_*` tables), while
`assets` and `dependencies` remain authoritative. There is no Python runtime or
generic parser replacement (monolith-native: asset-domain, lexical/local). Logic lives in
`FMonolithIndexReview` (`Private/MonolithIndexReview.{h,cpp}`) using only the public
`FMonolithIndexDatabase` surface; the DB impl file is unchanged (REQ-009). Source-of-truth
specification now lives in this file and `Docs/API_REFERENCE.md`. Tests:
`Monolith.IndexGuard.Project.*` in `Private/Tests/MonolithIndexQueryTests.cpp`,
including cycle/truncation guards, orphan-dependency health warnings, repair dry-run/execute,
CRG cache rebuild/cache-hit coverage, sensitivity scoring, review-hotspot ranking,
and minimal review-context output-contract coverage.

Invariants honored by the implementation:

- `ProjectIndex.db` is **Schema v2** (`schema_version` meta key + `assets.saved_hash` column/index, `PRAGMA table_info` migration). `project.health` must validate v2, not generic v1.
- 21 FTS triggers (seven project FTS tables × ai/ad/au) are external-content FTS5 → `'rebuild'` is valid for `repair_fts`.
- `FMonolithIndexDatabase` exposes a raw `FSQLiteDatabase*` (`GetRawDatabase()`) with **no DB-internal lock**; writes are caller-serialized. `repair_fts` and `repair_crg_cache` must gate on `UMonolithIndexSubsystem::IsIndexing()` and run inside transaction-scoped helpers.
- CRG projection rows are disposable: `crg_nodes` maps one row per asset, `crg_edges` maps one row per dependency, and `crg_node_metrics` stores `risk_score`, tier, reasons JSON, raw count JSON, and `scoring_version`. Missing projection rows are cache misses, not action failures; current scoring is v3 and includes a bounded UE-domain sensitivity signal.
- Direct lookup helpers to build bounded traversal on: `GetDependenciesForAsset` (out / `source_asset_id`), `GetReferencersOfAsset` (in / `target_asset_id`).
- Review/search action outputs expose a stable contract: `input`, `limits`, `truncated`, and `next_actions` where applicable; `project.search` returns `match_source`, `match_table`, `match_field`, `match_object_path`, and `match_value` so agents can separate asset/node hits from content hits; `project.detect_changes` exposes top-level `changed_entity_count` / `impacted_count` plus `review_priorities` in every detail level and standard-mode `changed_entities[]` / `impact` / empty `test_gaps[]`, treats `_` and `%` literally when using path stems for package-path suffix matching, `project.find_unused` exposes capped `items[]` with `asset_path`, `asset_name`, `asset_class`, `confidence`, and `reasons[]` fields, `project.pre_merge_check` exposes `decision`, `checks[]`, `findings[]`, `risk_score`, and standard-mode nested `health` / `change_analysis` / `unused` payloads, `project.snapshot` exposes dry-run or stored snapshot metadata and uses high-resolution auto labels when omitted, `project.diff_snapshots` exposes capped `new_nodes[]`, `removed_nodes[]`, `new_edges[]`, `removed_edges[]`, and `summary_counts` and fails with `status=error` when the current CRG projection cannot be queried, `project.review_hotspots` exposes capped `hotspots[]` plus optional `questions[]`, while `project.review_context` additionally exposes compact `top_risks[]` and `context[]` fields so agents can triage without pulling full details.
- Test precedent: extend `Private/Tests/MonolithIndexQueryTests.cpp` (`Monolith.IndexGuard.Project.*`, temp-DB fixture) — do not introduce a new directory or `WITH_DEV_AUTOMATION_TESTS` guard.

---
