# Monolith — MonolithSource Module

**Parent:** [SPEC_CORE.md](../SPEC_CORE.md)
**Engine:** Unreal Engine 5.7+
**Version:** 0.14.9 (Beta)

---

## MonolithSource

**Dependencies:** Core, CoreUObject, Engine, MonolithCore, SQLiteCore, EditorSubsystem, UnrealEd, Json, JsonUtilities, Slate, SlateCore

**Note:** Module structure was flattened — the vestigial outer stub has been removed. MonolithSource registers 20 actions. The engine source indexer is a native C++ implementation (`UMonolithSourceSubsystem` builds `EngineSource.db` in-process). The legacy Python tree-sitter indexer (`Scripts/source_indexer/`) is no longer used and is not a schema authority — `MonolithSourceSchema.h` is the sole source-of-truth.

### Classes

| Class | Responsibility |
|-------|---------------|
| `FMonolithSourceModule` | Registers 20 actions total: 16 `source` actions and 4 `context` actions |
| `UMonolithSourceSubsystem` | UEditorSubsystem. Owns engine source DB. Runs native C++ source indexer. Exposes `TriggerReindex()` (full engine re-index) and `TriggerProjectReindex()` (project C++ only, incremental). **F17 (2026-04-26):** Auto-binds `FCoreUObjectDelegates::ReloadCompleteDelegate` at `Initialize` to kick incremental project reindex on Live Coding / hot-reload completion (5s cooldown + `bIsIndexing` re-entrancy guard + bootstrap-DB-missing skip). Unbinds at `Deinitialize`. |
| `FMonolithSourceDatabase` | Read/write SQLite wrapper (`Open`, `OpenForWriting`, schema reset, transactions, inserts). Thread-safe via FCriticalSection. FTS queries with prefix matching |
| `FMonolithSourceActions` | 16 `source` handlers. Helpers: IsForwardDeclaration (regex), ExtractMembers (smart class outline) |
| `FMonolithSourceReview` | CRG-inspired navigation/review over the existing `"references"` + `inheritance` graph: bounded BFS impact radius, query-time risk, review-context packaging. Uses only the public DB query surface. `health`/`repair_fts` live on `FMonolithSourceDatabase` (private `DbLock`) |
| `FMonolithSourceContextActions` | 4 `context` handlers for index readiness, indexing dispatch, context item search, and attachment materialization |
| ~~`UMonolithQueryCommandlet`~~ | **Removed.** Replaced by standalone `monolith_query.exe` (see Section 5.1). The exe has no UE runtime dependency and starts instantly |

### Auto-Reindex on Hot-Reload (F17)

**Important:** `monolith_reindex` is the **asset/project** indexer (Blueprints, materials, textures via `MonolithIndex`). It does NOT update the C++ source DB. Source-symbol freshness is owned by this module via:

1. `source.trigger_reindex` — full clean rebuild (engine + shaders + project).
2. `source.trigger_project_reindex` — incremental, project C++ only.
3. **F17 auto-hook (2026-04-26):** `UMonolithSourceSubsystem` listens on `FCoreUObjectDelegates::ReloadCompleteDelegate`. After every Live Coding patch and after every UBT-driven editor restart that fires hot-reload, the subsystem auto-kicks `TriggerProjectReindex()` (async). Guarded by a 5-second cooldown and an in-flight `bIsIndexing` flag so multi-module reload bursts don't storm. Skips silently if `EngineSource.db` doesn't yet exist (first-install bootstrap requires a manual `source.trigger_reindex`).

After F17, agents do not need to invoke any source-reindex action manually in the common dev loop — just run UBT or Live Coding and `source_query` reflects the new symbols within ~1 second.

### Actions (20 — namespaces: "source", "context")

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
| `impact_radius` | `symbol` (required), `edge_kinds` (call\|type\|inheritance), `direction` (both), `max_depth` (2), `max_results` (200) | Bounded BFS over quoted `"references"` + `inheritance` (cycle-safe, `truncated`). `include` excluded |
| `health` | `include_counts` (true) | Read-only diagnostics: v1 schema, `symbols_ai/ad` triggers, `symbols_fts` parity, orphan refs. `source_fts` reported as info |
| `repair_fts` | `target` (all\|symbols\|source), `execute` (false) | Rebuilds `symbols_fts` (external-content). `target=source` always degrades to reindex guidance (plain fts5). Refused while `IsIndexing()` |
| `risk_score` | `symbol` (required), `limit` (10), `min_tier` (low) | Query-time risk `{score,tier,reasons[],raw_counts}` (caller fan-in, descendants, UE macro, file-boundary crossing) |
| `review_context` | `symbol` (required), `direction` (both), `max_depth` (2), `max_results` (200), `detail_level` (minimal) | Token-efficient package: seed + impact + risk reasons + next actions. Distinct from single-item `context.build_attachment` |

**DB Location:** `Plugins/Monolith/Saved/EngineSource.db`

### CRG-Inspired Navigation — IMPLEMENTED (P0, 2026-05-16)

The CRG-inspired review/navigation surface is **implemented** as 5 additive `source`
actions (`impact_radius`, `health`, `repair_fts`, `risk_score`, `review_context`) over
the **existing** `"references"` + `inheritance` graph — no new DB/schema, no Python
runtime, no generic nodes/edges (monolith-native: source-symbol, lexical/local).
`impact_radius`/`risk_score`/`review_context` live in `FMonolithSourceReview`
(`Private/MonolithSourceReview.{h,cpp}`) using only public DB queries;
`ComputeHealth`/`RepairFts` are methods on `FMonolithSourceDatabase` (private `DbLock`).
Spec source: `Plugins/Monolith/CRG/spec/monolith-crg-index-navigation-{prd,spec}.md`.
Tests: `Monolith.IndexGuard.Source.*` in `Private/Tests/MonolithSourceQueryTests.cpp`,
including cycle guards, exact `call`/`type` edge filtering, orphan-reference health
warnings, repair dry-run/degraded source-FTS handling, and minimal review-context
output-contract coverage.

Invariants honored by the implementation:

- `EngineSource.db` is **Schema v1**; native `MonolithSourceSchema.h` (`SchemaVersion=1`, `meta.schema_version`) is the sole authority. The `MonolithSourceSchema.h:5` comment claiming parity with `Scripts/source_indexer/db/schema.py` is **stale drift** (that Python indexer is legacy/uninvoked since 2026-03-15 — see `Docs/TODO.md`); correct that comment when implementing `source.health`.
- `symbols_fts` is external-content (`content=symbols`) → supports `'rebuild'`. `source_fts` is a plain `fts5(file_id UNINDEXED, line_number UNINDEXED, text)` with no backing table → `'rebuild'` is meaningless; `source.repair_fts(target=source)` always degrades to a reindex recommendation. Triggers are `symbols_ai`/`symbols_ad` only (no `symbols_au`, no `source_fts` trigger) — `source.health` must expect exactly that set.
- `"references"` is a quoted SQLite reserved word in schema and every query; new traversal/fixtures must quote it. Traversal: calls/type refs `from_symbol_id → to_symbol_id` (`GetReferencesTo`/`GetReferencesFrom`), inheritance `child_id → parent_id`. `call` and `type` edge filters are exact; `includes` is file/path-level (no `included_file_id`) and returns a P0 warning until path→`files.path` resolution exists.
- `FMonolithSourceDatabase` is a **read/write** wrapper with a private `DbLock`; `source.repair_fts` writes must run inside an existing DB/helper method that can take the private lock, gated on `UMonolithSourceSubsystem::IsIndexing()`.
- `source.review_context` is a dedicated CRG-style review package distinct from single-item `context.build_attachment`; it exposes `input`, `limits`, `risk`, `top_risks[]`, `impact`, compact `context[]`, `truncated`, and `next_actions`.
- Test precedent: extend `Private/Tests/MonolithSourceQueryTests.cpp` (`Monolith.IndexGuard.Source.*`, temp-DB fixture) — do not introduce a new directory or `WITH_DEV_AUTOMATION_TESTS` guard.

---
