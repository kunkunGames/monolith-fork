# Monolith — MonolithSource Module

**Parent:** [SPEC_CORE.md](../SPEC_CORE.md)
**Engine:** Unreal Engine 5.7+
**Version:** 0.14.9 (Beta)

---

## MonolithSource

**Dependencies:** Core, CoreUObject, Engine, MonolithCore, SQLiteCore, EditorSubsystem, UnrealEd, Json, JsonUtilities, Slate, SlateCore

**Note:** Module structure was flattened — the vestigial outer stub has been removed. MonolithSource registers 15 actions. The engine source indexer is a native C++ implementation (`UMonolithSourceSubsystem` builds `EngineSource.db` in-process). The legacy Python tree-sitter indexer (`Scripts/source_indexer/`) is no longer used.

### Classes

| Class | Responsibility |
|-------|---------------|
| `FMonolithSourceModule` | Registers 15 actions total: 11 `source` actions and 4 `context` actions |
| `UMonolithSourceSubsystem` | UEditorSubsystem. Owns engine source DB. Runs native C++ source indexer. Exposes `TriggerReindex()` (full engine re-index) and `TriggerProjectReindex()` (project C++ only, incremental). **F17 (2026-04-26):** Auto-binds `FCoreUObjectDelegates::ReloadCompleteDelegate` at `Initialize` to kick incremental project reindex on Live Coding / hot-reload completion (5s cooldown + `bIsIndexing` re-entrancy guard + bootstrap-DB-missing skip). Unbinds at `Deinitialize`. |
| `FMonolithSourceDatabase` | Read/write SQLite wrapper (`Open`, `OpenForWriting`, schema reset, transactions, inserts). Thread-safe via FCriticalSection. FTS queries with prefix matching |
| `FMonolithSourceActions` | 11 `source` handlers. Helpers: IsForwardDeclaration (regex), ExtractMembers (smart class outline) |
| `FMonolithSourceContextActions` | 4 `context` handlers for index readiness, indexing dispatch, context item search, and attachment materialization |
| ~~`UMonolithQueryCommandlet`~~ | **Removed.** Replaced by standalone `monolith_query.exe` (see Section 5.1). The exe has no UE runtime dependency and starts instantly |

### Auto-Reindex on Hot-Reload (F17)

**Important:** `monolith_reindex` is the **asset/project** indexer (Blueprints, materials, textures via `MonolithIndex`). It does NOT update the C++ source DB. Source-symbol freshness is owned by this module via:

1. `source.trigger_reindex` — full clean rebuild (engine + shaders + project).
2. `source.trigger_project_reindex` — incremental, project C++ only.
3. **F17 auto-hook (2026-04-26):** `UMonolithSourceSubsystem` listens on `FCoreUObjectDelegates::ReloadCompleteDelegate`. After every Live Coding patch and after every UBT-driven editor restart that fires hot-reload, the subsystem auto-kicks `TriggerProjectReindex()` (async). Guarded by a 5-second cooldown and an in-flight `bIsIndexing` flag so multi-module reload bursts don't storm. Skips silently if `EngineSource.db` doesn't yet exist (first-install bootstrap requires a manual `source.trigger_reindex`).

After F17, agents do not need to invoke any source-reindex action manually in the common dev loop — just run UBT or Live Coding and `source_query` reflects the new symbols within ~1 second.

### Actions (15 — namespaces: "source", "context")

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

**DB Location:** `Plugins/Monolith/Saved/EngineSource.db`

### Planned Extension — CRG-Inspired Navigation (spec accepted, NOT implemented as of v0.14.9)

A CRG-inspired review/navigation surface is specced but **not yet implemented** (no `impact_radius`/`health`/`repair_fts`/`risk_index`/`review_context` action exists in code). Spec source: `Plugins/Monolith/CRG/spec/monolith-crg-index-navigation-{prd,spec}.md`.

Accepted P0 scope (additive over **existing** `"references"` + `inheritance` — no new DB/schema): `source.impact_radius`, `source.health`, `source.repair_fts`, `source.risk_index`, `source.review_context`.

Verified current invariants any implementation must respect:

- `EngineSource.db` is **Schema v1**; native `MonolithSourceSchema.h` (`SchemaVersion=1`, `meta.schema_version`) is the sole authority. The `MonolithSourceSchema.h:5` comment claiming parity with `Scripts/source_indexer/db/schema.py` is **stale drift** (that Python indexer is legacy/uninvoked since 2026-03-15 — see `Docs/TODO.md`); correct that comment when implementing `source.health`.
- `symbols_fts` is external-content (`content=symbols`) → supports `'rebuild'`. `source_fts` is a plain `fts5(file_id UNINDEXED, line_number UNINDEXED, text)` with no backing table → `'rebuild'` is meaningless; `source.repair_fts(target=source)` always degrades to a reindex recommendation. Triggers are `symbols_ai`/`symbols_ad` only (no `symbols_au`, no `source_fts` trigger) — `source.health` must expect exactly that set.
- `"references"` is a quoted SQLite reserved word in schema and every query; new traversal/fixtures must quote it. Traversal: calls/type refs `from_symbol_id → to_symbol_id` (`GetReferencesTo`/`GetReferencesFrom`), inheritance `child_id → parent_id`. `includes` is file/path-level (no `included_file_id`) and stays opt-in only after path→`files.path` resolution.
- `FMonolithSourceDatabase` is a **read/write** wrapper with a private `DbLock`; `source.repair_fts` writes must run inside an existing DB/helper method that can take the private lock, gated on `UMonolithSourceSubsystem::IsIndexing()`.
- `context.build_attachment` is the existing monolith-native analog of `source.review_context`; REQ-007/008 must decide new-action vs `context` extension before coding.
- Test precedent: extend `Private/Tests/MonolithSourceQueryTests.cpp` (`Monolith.IndexGuard.Source.*`, temp-DB fixture) — do not introduce a new directory or `WITH_DEV_AUTOMATION_TESTS` guard.

---
