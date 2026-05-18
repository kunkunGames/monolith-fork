# Monolith — MonolithSource Module

**Parent:** [SPEC_CORE.md](../SPEC_CORE.md)
**Engine:** Unreal Engine 5.7+
**Version:** 0.14.9 (Beta)

---

## MonolithSource

**Dependencies:** Core, CoreUObject, Engine, MonolithCore, SQLiteCore, EditorSubsystem, UnrealEd, Json, JsonUtilities, Slate, SlateCore

**Note:** Module structure was flattened — the vestigial outer stub has been removed. MonolithSource registers 28 actions. The engine source indexer is a native C++ implementation (`UMonolithSourceSubsystem` builds `EngineSource.db` in-process). The legacy Python tree-sitter indexer (`Scripts/source_indexer/`) is no longer used and is not a schema authority — `MonolithSourceSchema.h` is the sole source-of-truth.

### Classes

| Class | Responsibility |
|-------|---------------|
| `FMonolithSourceModule` | Registers 28 actions total: 23 `source` actions and 5 `context` actions |
| `UMonolithSourceSubsystem` | UEditorSubsystem. Owns engine source DB. Runs native C++ source indexer. Exposes `TriggerReindex()` (full engine re-index) and `TriggerProjectReindex()` (project C++ only, incremental). **F17 (2026-04-26):** Auto-binds `FCoreUObjectDelegates::ReloadCompleteDelegate` at `Initialize` to kick incremental project reindex on Live Coding / hot-reload completion (5s cooldown + `bIsIndexing` re-entrancy guard + bootstrap-DB-missing skip). Unbinds at `Deinitialize`. |
| `FMonolithSourceDatabase` | Read/write SQLite wrapper (`Open`, `OpenForWriting`, schema reset, transactions, inserts). Thread-safe via FCriticalSection. Exact-name symbol lookup supports an optional SQL limit; FTS queries use prefix matching. Owns read/write `health`, `repair_fts`, `repair_crg_cache`, cached risk reads, `snapshot`, `diff_snapshots`, and `detect_changes` / `pre_merge_check` / `review_hotspots` / `find_unused` SQL or aggregation that needs the DB-owned surface |
| `FMonolithSourceActions` | 23 `source` handlers. Helpers: IsForwardDeclaration (regex), ExtractMembers (smart class outline) |
| `FMonolithSourceReview` | CRG-inspired navigation/review over the existing `"references"` + `inheritance` graph: bounded BFS impact radius, cached/query-time risk scoring, review-hotspot forwarding, and review-context packaging. Uses only the public DB query surface. `health`/`repair_fts`/`repair_crg_cache`/`detect_changes`/`pre_merge_check`/`snapshot`/`diff_snapshots`/`review_hotspots`/`find_unused` live on `FMonolithSourceDatabase` |
| `FMonolithSourceContextActions` | 5 `context` handlers for index readiness, indexing dispatch, context item search, attachment materialization, and read-only asset<->symbol bridge lookup |
| ~~`UMonolithQueryCommandlet`~~ | **Removed.** Replaced by standalone `monolith_query.exe` (see Section 5.1). The exe has no UE runtime dependency and starts instantly |

### Auto-Reindex on Hot-Reload (F17)

**Important:** `monolith_reindex` is the **asset/project** indexer (Blueprints, materials, textures via `MonolithIndex`). It does NOT update the C++ source DB. Source-symbol freshness is owned by this module via:

1. `source.trigger_reindex` — full clean rebuild (engine + shaders + project).
2. `source.trigger_project_reindex` — incremental, project C++ only.
3. **F17 auto-hook (2026-04-26):** `UMonolithSourceSubsystem` listens on `FCoreUObjectDelegates::ReloadCompleteDelegate`. After every Live Coding patch and after every UBT-driven editor restart that fires hot-reload, the subsystem auto-kicks `TriggerProjectReindex()` (async). Guarded by a 5-second cooldown and an in-flight `bIsIndexing` flag so multi-module reload bursts don't storm. Skips silently if `EngineSource.db` doesn't yet exist (first-install bootstrap requires a manual `source.trigger_reindex`).

After F17, agents do not need to invoke any source-reindex action manually in the common dev loop — just run UBT or Live Coding and `source_query` reflects the new symbols within ~1 second.

### Actions (28 — namespaces: "source", "context")

| Action | Params | Description |
|--------|--------|-------------|
| `read_source` | `symbol`, `include_header`, `max_lines`, `members_only` | Get source code for a class/function/struct. FTS fallback if exact match fails |
| `find_references` | `symbol`, `ref_kind`, `limit` | Find all usage sites |
| `find_callers` | `symbol`, `limit` | All functions that call the given function |
| `find_callees` | `symbol`, `limit` | All functions called by the given function |
| `search_source` | `query`, `scope`, `limit`, `mode`, `module`, `path_filter`, `symbol_kind` | Dual search: symbol FTS + source line FTS |
| `get_class_hierarchy` | `class_name`, `direction`, `depth` | Inheritance tree (both/ancestors/descendants, max 80 shown) |
| `get_module_info` | `module_name` | Module stats: file count, symbol counts, key classes |
| `get_symbol_context` | `symbol`, `context_lines` | Definition with surrounding context |
| `read_file` | `file_path`, `start_line`, `end_line` | Read source lines by path (absolute -> DB exact -> DB suffix match) |
| `trigger_reindex` | none | Trigger full C++ engine source re-index (replaces entire EngineSource.db) |
| `trigger_project_reindex` | none | Trigger incremental project-only C++ source re-index (updates project symbols in EngineSource.db without a full rebuild) |
| `get_index_status` | `include_stats` | Report local project/source index readiness for Monolith context mentions |
| `start_indexing` | `scope`, `full` | Start local project asset and/or source indexing for context search |
| `search_items` | `query`, `limit` | Search local indexed assets and source entries for mention-style prompt context |
| `build_attachment` | `item_id`, `context_lines` | Materialize a context.search_items result into a bounded prompt attachment |
| `bridge_asset_symbols` | `asset_path` or `symbol`, `limit` (20), `detail_level` (minimal) | RX-6 read-only bridge between ProjectIndex assets and EngineSource symbols. Returns bounded heuristic `links[]` with `confidence`, `reasons[]`, `asset`, `symbol`, `warnings[]`, `truncated`, and `next_actions` |
| `impact_radius` | `symbol` (required), `edge_kinds` (call\|type\|inheritance), `direction` (both), `max_depth` (2), `max_results` (200) | Bounded BFS over quoted `"references"` + `inheritance` (cycle-safe, `truncated`). `include` excluded |
| `health` | `include_counts` (true) | Read-only diagnostics: v1 schema, `symbols_ai/ad` triggers, `symbols_fts` parity, orphan refs, CRG projection table/index/parity checks. `source_fts` reported as info |
| `repair_fts` | `target` (all\|symbols\|source), `execute` (false) | Rebuilds `symbols_fts` (external-content). `target=source` always degrades to reindex guidance (plain fts5). Refused while `IsIndexing()` |
| `repair_crg_cache` | `scope` (all), `execute` (false) | Rebuild derived `crg_nodes`/`crg_edges`/`crg_node_metrics`/`crg_meta` from `symbols`, `"references"`, and `inheritance`. Dry-run unless `execute=true`; refused while `IsIndexing()` |
| `risk_score` | `symbol` (required), `limit` (10), `min_tier` (low) | Cached risk `{score,tier,reasons[],raw_counts,cache}` from CRG projection when present; safe query-time fallback on cache miss; scoring v3 adds UE-domain sensitivity |
| `detect_changes` | `changed_paths` or `paths`, `changed_ranges` (`[{path,ranges:[[s,e]...]}]`), `diff_text` (unified diff), `max_results` (200), `detail_level` (minimal) | VCS-agnostic changed source path mapping: `files.path` suffix -> symbols -> risk + depth-1 caller impact + heuristic test gaps + review priorities. RX-1.1 line precision: when a path has resolved line ranges (via `changed_ranges` or parsed `diff_text`), only symbols whose `[line_start,line_end]` overlaps a range are returned for that path (CRG `changes.py:204` rule, non-positive spans skipped); paths without ranges keep file-level behavior. `input.precision` is `line` or `file`. Diff parsing is pure text; never shells out to P4/git |
| `find_unused` | `kind` (all), `limit` (100), `min_confidence` (low) | Advisory dead-symbol candidates for function/class/struct symbols with `confidence` + `reasons[]`; never reports `high`, never mutates, and excludes UE reflection/automation/entry markers |
| `pre_merge_check` | `changed_paths` or `paths`, `max_results` (200), `unused_limit` (20), `detail_level` (minimal), `include_unused` (true) | Read-only pre-merge gate that composes `health`, `detect_changes`, and optional `find_unused` into `decision` (`pass`/`warn`/`fail`), `checks[]`, `findings[]`, and next actions. Never shells out to P4/git |
| `snapshot` | `label`, `execute` (false) | Dry-run by default; `execute=true` stores current `crg_nodes`/`crg_edges` manifest in derived `crg_snapshots` for later review diffs |
| `diff_snapshots` | `before` (label/id), `after` (label/id or current), `limit` (100) | Read-only CRG projection diff. Returns new/removed node and edge samples plus `summary_counts`; no cache rebuild or VCS shell-out |
| `review_hotspots` | `kind` (all), `limit` (50), `min_lines` (100), `include_questions` (true) | Global review queue over fan-in/fan-out/risk/large symbol signals with optional advisory questions |
| `review_context` | `symbol` (required), `direction` (both), `max_depth` (2), `max_results` (200), `detail_level` (minimal) | Token-efficient package: seed + impact + risk reasons + next actions. Distinct from single-item `context.build_attachment` |

**DB Location:** `Plugins/Monolith/Saved/EngineSource.db`

**Derived CRG Projection Cache:** `crg_nodes`, `crg_edges`, `crg_node_metrics`, `crg_meta`, `crg_snapshots`.
These tables are rebuildable projections over `symbols`, `"references"`, and
`inheritance`, not source-of-truth tables. `source.repair_crg_cache execute=true`
purges stale/orphan metrics, recreates the projection, and `source.risk_score` reads `crg_node_metrics` first
before falling back to query-time scoring. Rebuilt projection metrics use
`crg_meta.scoring_version=3` for the UE-domain sensitivity factor. `crg_snapshots`
is a derived review aid created only by `source.snapshot execute=true`; it stores
compact node/edge manifests and may be dropped/recreated without losing source data.

### CRG-Inspired Navigation + Projection Cache — IMPLEMENTED (P0, 2026-05-16; cache 2026-05-17)

The CRG-inspired review/navigation surface is **implemented** as 12 additive `source`
actions (`impact_radius`, `health`, `repair_fts`, `repair_crg_cache`, `risk_score`,
`detect_changes`, `find_unused`, `pre_merge_check`, `snapshot`, `diff_snapshots`, `review_hotspots`, `review_context`) over the **existing** `"references"` + `inheritance` graph. The CRG
`nodes`/`edges` idea is adopted only as a derived SQLite projection/cache (`crg_*`
tables), while `symbols`, `"references"`, and `inheritance` remain authoritative.
There is no Python runtime or generic parser replacement (monolith-native:
source-symbol, lexical/local).
`impact_radius`/`risk_score`/`review_hotspots`/`review_context` live in
`FMonolithSourceReview` (`Private/MonolithSourceReview.{h,cpp}`) using only
public DB queries or DB-owned helpers; `ComputeHealth`/`RepairFts`/
`RepairCrgCache`/`DetectChanges`/`PreMergeCheck`/`Snapshot`/`DiffSnapshots`/`ReviewHotspots`/`FindUnused` are methods on `FMonolithSourceDatabase`
(private `DbLock`).
Spec source: `Plugins/Monolith/CRG/spec/monolith-crg-index-navigation-{prd,spec}.md`.
Tests: `Monolith.IndexGuard.Source.*` in `Private/Tests/MonolithSourceQueryTests.cpp`,
including cycle guards, exact `call`/`type` edge filtering, orphan-reference health
warnings, repair dry-run/degraded source-FTS handling, CRG cache rebuild/cache-hit
coverage, sensitivity scoring, review-hotspot ranking, and minimal review-context
output-contract coverage.

Invariants honored by the implementation:

- `EngineSource.db` is **Schema v1**; native `MonolithSourceSchema.h` (`SchemaVersion=1`, `meta.schema_version`) is the sole authority. The `MonolithSourceSchema.h:5` comment claiming parity with `Scripts/source_indexer/db/schema.py` is **stale drift** (that Python indexer is legacy/uninvoked since 2026-03-15 — see `Docs/TODO.md`); correct that comment when implementing `source.health`.
- `symbols_fts` is external-content (`content=symbols`) → supports `'rebuild'`. `source_fts` is a plain `fts5(file_id UNINDEXED, line_number UNINDEXED, text)` with no backing table → `'rebuild'` is meaningless; `source.repair_fts(target=source)` always degrades to a reindex recommendation. Triggers are `symbols_ai`/`symbols_ad` only (no `symbols_au`, no `source_fts` trigger) — `source.health` must expect exactly that set.
- `"references"` is a quoted SQLite reserved word in schema and every query; new traversal/fixtures must quote it. Traversal: calls/type refs `from_symbol_id → to_symbol_id` (`GetReferencesTo`/`GetReferencesFrom`), inheritance `child_id → parent_id`. `call` and `type` edge filters are exact; `includes` is file/path-level (no `included_file_id`) and returns a P0 warning until path→`files.path` resolution exists.
- `FMonolithSourceDatabase` is a **read/write** wrapper with a private `DbLock`; `source.repair_fts` and `source.repair_crg_cache` writes must run inside DB/helper methods that can take the private lock, gated on `UMonolithSourceSubsystem::IsIndexing()`.
- CRG projection rows are disposable: `crg_nodes` maps one row per symbol, `crg_edges` maps one row per valid reference/inheritance edge, and `crg_node_metrics` stores `risk_score`, tier, reasons JSON, raw count JSON, and `scoring_version`. Missing projection rows are cache misses, not action failures; current scoring is v3 and includes a bounded UE-domain sensitivity signal.
- `source.detect_changes` is read-only changed-path review triage over `files`, `symbols`, quoted `"references"`, and the cached/query-time risk path. It escapes SQL `LIKE` wildcards in changed path suffixes so `_` and `%` match literal source filenames rather than broad patterns. It exposes `input` (including `precision` ∈ `{file,line}` and `range_paths`), `limits`, `risk_score`, `scoring_version`, top-level `changed_entity_count` / `impacted_count` / `test_gap_count` in every detail level, standard-mode `changed_entities[]` (with `matched_ranges` under line precision), `impact`, heuristic `test_gaps[]`, `review_priorities`, `truncated`, and `next_actions`; it is VCS-agnostic and must not shell out to P4/git. RX-1.1 line precision (port of CRG `changes.py:_parse_unified_diff` + the `line_start<=end AND line_end>=start` overlap rule) is additive and opt-in: callers pass `changed_ranges` and/or a unified-diff `diff_text` (the plugin parses the text only — the caller produces the diff). `FMonolithSourceDatabase::ParseUnifiedDiffRanges` is the shared public parser; a path that appears only in `changed_ranges`/`diff_text` is still treated as a changed path; malformed ranges and non-positive line spans degrade safely to file-level. `pre_merge_check` stays file-level (passes no ranges). Spec source: `CRG/spec/monolith-crg-detect-changes-line-precision-spec.md`.
- `source.pre_merge_check` is a read-only pre-merge gate over `health`, `detect_changes`, and optional `find_unused`. It exposes `decision` (`pass`/`warn`/`fail`), `checks[]`, `findings[]`, `risk_score`, `truncated`, and standard-mode nested `health` / `change_analysis` / `unused` payloads. It is advisory, does not mutate, and does not shell out to P4/git.
- `source.snapshot` is dry-run by default and writes only when `execute=true`. It creates/updates derived `crg_snapshots` rows from the current CRG projection manifest and never rebuilds the projection itself. Auto labels use high-resolution UTC ticks to avoid same-second overwrite collisions.
- `source.diff_snapshots` is read-only and compares a stored snapshot against another stored snapshot or the current projection. It accepts positional refs and `--before`/`--after` refs, including mixed `--before=<ref> <after-ref>` calls. It exposes `summary`, capped `new_nodes[]`, `removed_nodes[]`, `new_edges[]`, `removed_edges[]`, `truncated`, and `next_actions`; if the current CRG projection cannot be queried, it returns `status=error` rather than diffing against an empty manifest.
- `source.review_hotspots` is read-only global triage over cached/native fan, risk, and LOC signals. It exposes `input`, `limits`, `hotspots[]`, optional `questions[]`, `truncated`, and `next_actions`, and intentionally avoids community/betweenness semantics.
- `source.find_unused` is read-only advisory dead-symbol discovery over `symbols`, quoted `"references"`, and `inheritance`. It exposes `input`, `limits`, capped `items[]`, `confidence`, `reasons[]`, `truncated`, and `next_actions`; it must be recall-first (`min_confidence=low`) by default because UE reflection, delegates, Blueprint references, and soft-path references are outside the graph.
- `source.review_context` is a dedicated CRG-style review package distinct from single-item `context.build_attachment`; it exposes `input`, `limits`, `risk`, `top_risks[]`, `impact`, compact `context[]`, `truncated`, and `next_actions`.
- `context.bridge_asset_symbols` is the RX-6 cross-DB bridge. It remains read-only in editor and offline CLI paths: it never writes ProjectIndex, EngineSource, CRG cache, or snapshots; it never shells out to P4/git; and it must degrade with `warnings[]` when either index is unavailable or currently indexing. The bridge requires exactly one non-empty string seed (`asset_path` or `symbol`) and rejects wrong-typed seeds with `-32602` instead of silently selecting the other mode. The bridge is heuristic rather than authoritative: asset-seeded lookups derive candidate native class names from the package path, asset name, asset class, and common UE prefixes/suffixes, then search EngineSource symbols with bounded exact-name/FTS queries before score-sorting and trimming; symbol-seeded lookups search source symbols first and then ProjectIndex assets by exact/normalized names, evaluating the bounded symbol set before global score sorting rather than stopping on a pre-ranking candidate cap. Duplicate asset/symbol pairs preserve the highest-scoring candidate so an early lexical hit cannot block a later exact/normalized hit. Every returned item must include `confidence` (`high|medium|low`) plus concrete `reasons[]` so reviewers can tell whether a match came from exact name parity, normalized prefix stripping, asset class hints, or lexical fallback, and `truncated=true` when more than `limit` scored links are available. Offline parity uses `monolith_query context bridge_asset_symbols` over `ProjectIndex.db` and `EngineSource.db`, with the same `links[]`, `warnings[]`, `count`, `truncated`, and `lexical_only=true` output shape.
- Test precedent: extend `Private/Tests/MonolithSourceQueryTests.cpp` (`Monolith.IndexGuard.Source.*`, temp-DB fixture) — do not introduce a new directory or `WITH_DEV_AUTOMATION_TESTS` guard.

---
