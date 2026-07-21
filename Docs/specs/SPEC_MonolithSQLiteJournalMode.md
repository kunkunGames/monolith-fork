# Monolith SQLite Journal Mode / WAL Conversion Spec

**Parent:** [SPEC_CORE.md](../SPEC_CORE.md)
**Scope:** `EngineSource.db`, `ProjectIndex.db`; retired `graph.db` evidence is preserved in sections 5.3 and 8.1
**Status:** `EngineSource.db` WAL gate failed for UE SQLiteCore; both live index DBs remain `DELETE`; the separate graph DB is removed
**Date:** 2026-05-22
**Owner Modules:** MonolithCore, MonolithSource, MonolithIndex, MonolithQuery

---

## 1. Purpose

This spec records Monolith's database-specific SQLite journal-mode policy.

The decision is intentionally DB-specific. `EngineSource.db` and `ProjectIndex.db` have different writer patterns, reader fan-out, and rebuild costs. A single global "turn WAL on everywhere" switch is not acceptable. The 2026-05-22 `EngineSource.db` WAL attempt passed with standalone `monolith_query.exe` SQLite but failed in the editor/MCP path because UE 5.7 `SQLiteCore` uses a custom `SQLITE_OS_OTHER` VFS without WAL shared-memory support. Leaving `EngineSource.db` in WAL would make editor/MCP source opens fail, so the live policy remains rollback-journal `DELETE`. The former `graph.db` export is no longer a live database: its only useful graph-node search meaning is served by the `source_graph_nodes` VIEW and `source_graph_nodes_fts` inside EngineSource.

---

## 2. Decision Summary

| Database | Current mode | Recommended default | Candidate future mode | ROI | Risk | Decision |
|----------|--------------|---------------------|-----------------------|-----|------|----------|
| `EngineSource.db` | `DELETE` + `synchronous=NORMAL` | Keep `DELETE` | `WAL` only after replacing/augmenting the UE SQLite VFS or moving editor source DB access to a WAL-capable SQLite layer | High | High | Do not enable WAL in the live DB. Standalone CLI can read WAL, but the editor/MCP `FSQLiteDatabase` path cannot safely open/write WAL DBs under UE 5.7 SQLiteCore. |
| `ProjectIndex.db` | `DELETE` + `synchronous=NORMAL` | Keep `DELETE` | `WAL` + `synchronous=NORMAL`, only after a separate asset-index smoke | Medium | High | Defer. The project index has useful concurrent readers, but the editor indexer and content FTS path are wider blast radius than source search. |
| ~~`graph.db`~~ | Retired | None | None | Removal eliminates duplicate storage and maintenance | Low | Removed. `source.search_crg_graph` now reads EngineSource's canonical graph-node VIEW/FTS. |

No live DB journal-mode behavior changes are authorized by this spec. `EngineSource.db` and `ProjectIndex.db` stay on rollback-journal `DELETE`; no journal policy applies to the removed graph export.

---

## 3. Current State

| Surface | DB | Writer | Readers | Current journal handling |
|---------|----|--------|---------|--------------------------|
| Source index | `Saved/EngineSource.db` | `UMonolithSourceSubsystem`, source reindex commandlet, repair/snapshot actions | MCP `source` actions, `monolith_query.exe source`, bridge actions | Source write-capable opens set `PRAGMA journal_mode=DELETE`, `synchronous=NORMAL`, `locking_mode=NORMAL`, and `busy_timeout=5000`. Read-only CLI opens use `query_only=ON` and never run `PRAGMA journal_mode=...`. |
| Project index | `Saved/ProjectIndex.db` | `UMonolithIndexSubsystem` and deep asset indexers | MCP `project` actions, `monolith_query.exe project`, bridge actions | Project DB opens force `PRAGMA journal_mode=DELETE`; comments note `WAL + ReadOnly` previously returned zero rows on Windows. |
| Source graph-node search | `Saved/EngineSource.db` | Source index/schema migration; `source repair_fts --target=graph_nodes --execute` when health requests repair | `source search_crg_graph` | Uses the EngineSource connection policy above. `source_graph_nodes` is a VIEW; `source_graph_nodes_fts` is external-content FTS5 maintained by file/symbol triggers. |

The repository contains two important historical signals:

| Signal | Meaning |
|--------|---------|
| Older source and project DB comments said `WAL` broke read-only opens on Windows. | This remains a hard gate. The 2026-05-22 source slice showed why: standalone CLI SQLite could read WAL, but UE SQLiteCore could not configure/open WAL in the editor path. |
| Subsystem commandlet guards mentioned a running editor holding a WAL lock. | The source commandlet comment was corrected to describe the current constraint: avoid a second long-lived DB handle and keep checkpoint ownership clear. |

---

## 4. SQLite Mode Inventory

SQLite `PRAGMA journal_mode` supports the modes below. Monolith should expose only the safe subset needed by the two live index DBs, not every SQLite mode.

| Mode | Behavior | Monolith policy |
|------|----------|-----------------|
| `DELETE` | Rollback journal is deleted after commit. | Current baseline and safest default. Best for simple DB-file lifecycle and least surprise around sidecar files. |
| `TRUNCATE` | Rollback journal file remains but is truncated to zero bytes. | Possible optimization for high-churn rebuild tools, but it still leaves a `*-journal` sidecar and does not solve read/write concurrency. |
| `PERSIST` | Rollback journal file remains and its header is zeroed. | Do not use by default; a persistent `*-journal` sidecar creates the same operator confusion this work is trying to avoid. |
| `MEMORY` | Rollback journal is kept in memory. | Disallowed for Monolith index DBs because a crash can corrupt the database. |
| `WAL` | Writes append to `*-wal`; readers use DB + WAL with `*-shm` shared-memory index. | Not enabled for live Monolith DBs under UE 5.7 SQLiteCore. Requires a WAL-capable SQLite/VFS layer first. |
| `OFF` | Rollback protection is disabled. | Disallowed. |

WAL is persistent at the database level. Once a writer sets `PRAGMA journal_mode=WAL`, later connections reopen in WAL until a writer changes the mode back to `DELETE`.

### 4.1 WAL Checkpoint Modes

When WAL is enabled, checkpointing becomes part of the operational contract.

| Checkpoint mode | Behavior | Monolith use |
|-----------------|----------|--------------|
| `PASSIVE` | Checkpoints frames without waiting for active readers or writers. | Safe routine/background checkpoint at batch boundaries. |
| `FULL` | Waits until there is no writer and readers have a current snapshot, then checkpoints. Readers can proceed; writers are blocked. | Controlled maintenance only. |
| `RESTART` | Like `FULL`, then waits until readers are no longer using the WAL so the next writer can restart it. | Rare maintenance; can block under many readers. |
| `TRUNCATE` | Like `RESTART`, then truncates the WAL file to zero bytes. | Required after full reindex/repair/rebuild when Monolith wants sidecar cleanup and predictable disk usage. |
| `NOOP` | Reports WAL checkpoint counters without checkpointing. | Health/diagnostics only. |

### 4.2 WAL-Related Connection Modes

| Setting | Recommended value | Reason |
|---------|-------------------|--------|
| `PRAGMA synchronous` | `NORMAL` for Monolith WAL candidates | Keeps write latency low. Durability after OS/power failure is weaker than `FULL`, but this matches the current `DELETE` tuning tradeoff and the DBs are rebuildable indexes. |
| `PRAGMA locking_mode` | `NORMAL` | Required for multi-process readers. `EXCLUSIVE` would defeat the core goal of allowing MCP, offline CLI, and editor readers to coexist. |
| `PRAGMA busy_timeout` | Writer-only, initially 5000 ms | Gives short overlapping writes a chance to serialize. A second writer can still fail with `database is locked` after the timeout. |
| Read-only opens | Do not set `journal_mode`; use `query_only=ON` after open | Changing journal mode is a write-capable operation. Read-only query paths must observe the DB mode, not mutate it. |

---

## 5. Database-Specific Recommendations

### 5.1 `EngineSource.db`

`EngineSource.db` is the best WAL candidate because it has many read-only consumers and occasional heavy write phases:

| Workload | Impact |
|----------|--------|
| Offline CLI `source` search/read/review calls while an editor or commandlet reindexes | WAL can let readers continue against a stable snapshot while the writer appends committed changes. |
| Bridge actions reading both source and project indexes | WAL can reduce false failures from a source writer blocking read-only bridge lookup. |
| Full source reindex or CRG repair | WAL can reduce reader stalls, but a long transaction can still grow `EngineSource.db-wal` until checkpoint. |

Rejected live policy:

| Field | Value |
|-------|-------|
| Journal mode | Keep `DELETE` |
| Synchronous | `NORMAL` |
| Checkpoint | Not applicable while live DBs stay in rollback-journal mode |
| Read path | Read-only open, `query_only=ON`, no journal-mode PRAGMA |
| Failure policy | If any tool leaves `EngineSource.db` in WAL, checkpoint/convert it back to `DELETE` before starting editor/MCP source DB access |

Expected ROI: high for developer ergonomics when `monolith_query.exe`, MCP source actions, and background source indexing overlap. This does not permit concurrent writers; SQLite still allows one writer at a time.

### 5.2 `ProjectIndex.db`

`ProjectIndex.db` should not be the first WAL conversion because it has wider asset-indexing surface area and content FTS tables.

Recommended future policy:

| Field | Value |
|-------|-------|
| Journal mode | Keep `DELETE` by default; consider `WAL` only after `EngineSource.db` pilot succeeds |
| Synchronous | `NORMAL` |
| Checkpoint if WAL is enabled | `PASSIVE` after incremental asset batches; `TRUNCATE` after full project reindex and `repair_fts --target=all` |
| Read path | Read-only open, `query_only=ON`, no journal-mode PRAGMA |
| Failure policy | Any Windows read-only zero-row reproduction keeps this DB on `DELETE` |

Expected ROI: medium. It helps when project search, impact/risk/review context, and bridge reads overlap with editor asset indexing. The blast radius is higher than source because project indexing touches asset registry state, deep indexers, FTS repair, and collection/project actions.

### 5.3 Retired `graph.db` policy record

`graph.db` and its build/rebuild/health actions are removed. `source.search_crg_graph` now reads the canonical `source_graph_nodes` VIEW and `source_graph_nodes_fts` in `EngineSource.db`, so it inherits the EngineSource journal policy and no longer needs a second writer, sidecar lifecycle, lock, cooldown, or copied-DB override.

The table below is retained as the historical policy that applied before removal; it is not a current runtime recommendation:

| Field | Value |
|-------|-------|
| Journal mode | `DELETE` |
| Synchronous | `NORMAL` |
| Alternative if rebuild interruption becomes painful | Build into a temp DB in the same directory, validate schema/FTS parity, then atomically replace `graph.db` when no reader owns it |
| WAL | Not recommended by default |

Historical ROI was low: `graph.db` duplicated data derived from `EngineSource.db`, while its flow/community/risk tables were unused. Moving the only effective consumer—graph-node search—onto EngineSource removes that artifact instead of optimizing its journal mode.

---

## 6. Design Constraints

The WAL attempt established these constraints:

| Area | Requirement |
|------|-------------|
| Source writer policy | `FMonolithSourceDatabase` and `monolith_query.exe source` write-capable opens must keep `DELETE` until the editor SQLite layer supports WAL. |
| Read-only policy | Readers must never run `PRAGMA journal_mode=...`. They may run `PRAGMA query_only=ON` after opening and then query `PRAGMA journal_mode;` for diagnostics. |
| Writer policy | Writers set the configured journal mode, `synchronous=NORMAL`, `locking_mode=NORMAL`, and a bounded `busy_timeout`. |
| Checkpoint policy | Query-only tools never delete or truncate sidecars manually. If a future WAL-capable writer exists, that writer must own checkpoints. |
| Health output | `source health` reports `journal_mode`; when an accidental WAL DB is opened by a WAL-capable path, it also reports WAL/SHM sizes and checkpoint counters. It also owns `source_graph_nodes` VIEW/FTS availability and parity diagnostics. `project health` continues to report `journal_mode=delete`. |
| Tooling | `monolith_query.exe` diagnostics must treat `db`, `db-wal`, and `db-shm` as one logical DB state. It must not delete WAL sidecars manually. |
| Backup/copy guidance | Copying or archiving a WAL DB must use SQLite backup API or happen after a controlled checkpoint/close window. Copying only `*.db` while `*.db-wal` has committed frames is invalid. |

---

## 7. Migration Status

| Phase | Scope | Exit gate |
|-------|-------|-----------|
| 0 | Verify `EngineSource.db` WAL read-only behavior on Windows. | Failed for live editor/MCP. Standalone CLI smoke passed, but UE SQLiteCore automation failed to set WAL (`journal_mode` stayed `delete`) and a WAL-converted live DB produced SQLiteCore open/configure errors. |
| 1 | Keep source health journal-mode reporting and focused source DB automation coverage. | Done. `source health` reports `journal_mode`; `Monolith.IndexGuard.Source.DatabaseUsesDeleteJournalMode` covers the supported live source DB mode. |
| 2 | Enable `EngineSource.db` WAL. | Not authorized. The live DB was converted back to `DELETE` after the failed gate. |
| 3 | Evaluate `ProjectIndex.db` with the same harness only after source pilot is stable. | Project search, FTS repair, asset dependency review, and bridge reads all pass under overlap with index writes. |
| 4 | Remove the low-ROI `graph.db` export and move graph-node search into EngineSource. | Complete when `source.search_crg_graph` preserves search semantics through `source_graph_nodes`/`source_graph_nodes_fts` and graph build/rebuild/health surfaces are absent. |

---

## 8. Verification Gates

| Gate | Required checks |
|------|-----------------|
| Rollback baseline | `DELETE` mode still leaves no stale hot rollback journal after clean close, and query-only CLI does not require writable opens except bounded hot-journal recovery. |
| WAL read-only | Read-only CLI and MCP query paths return expected rows with `*.db-wal` and `*.db-shm` present, absent, and after clean close where possible. |
| Read while write | A long writer transaction does not block unrelated readers in WAL mode. Readers see a stable snapshot. |
| Write while write | A second writer waits only up to the configured `busy_timeout`, then fails clearly with `database is locked` if the first writer still owns the write lock. |
| Checkpoint | `PASSIVE` checkpoints do not interrupt readers; `TRUNCATE` checkpoints after full rebuild reduce the WAL file to zero bytes or report a clear busy state. |
| Bridge | `bridge search_asset_symbols` works when source and project DBs use different journal modes during staged migration. |
| Downgrade | A copied WAL DB can be converted back to `DELETE`, checkpointed, and read by current release tools. |
| Crash recovery | Simulated interrupted writers preserve DB integrity and do not require manual deletion of `*-wal`, `*-shm`, or `*-journal`. |

### 8.1 EngineSource WAL Gate Record

This verification table is preserved as historical evidence. In particular, the `Graph DB remains DELETE` row records the retired artifact at the time of the 2026-05-22 gate; it is not a current file-existence requirement.

| Check | Command | Result |
|-------|---------|--------|
| Standalone CLI WAL conversion attempt | `Plugins\Monolith\Binaries\monolith_query.exe source repair_crg_cache --execute` with the temporary WAL implementation | PASS for CLI SQLite only; converted `EngineSource.db` to WAL and checkpointed the WAL to zero bytes. |
| Editor SQLiteCore WAL automation | `Automation RunTests Monolith.IndexGuard.Source.DatabaseUsesWalJournalMode` | FAIL. UE SQLiteCore opened the temp DB but `PRAGMA journal_mode=WAL` returned/stayed `delete`, so the source DB wrapper refused to continue. |
| Editor startup against WAL-converted live DB | `UnrealEditor-Cmd.exe ... Automation RunTests ...` | FAIL. Source subsystem logged `unable to open database file` around `PRAGMA journal_mode=WAL` / `PRAGMA journal_mode`, confirming the live editor path cannot safely own a WAL `EngineSource.db`. |
| Source DB restored to DELETE | `sqlite3 Plugins\Monolith\Saved\EngineSource.db "PRAGMA wal_checkpoint(TRUNCATE); PRAGMA journal_mode=DELETE;"` | Required after the failed WAL gate so editor/MCP source DB access remains usable. |
| Project DB remains DELETE | `sqlite3 -readonly Plugins\Monolith\Saved\ProjectIndex.db "PRAGMA journal_mode;"` | PASS: `delete`. |
| Graph DB remains DELETE | `sqlite3 -readonly Plugins\Monolith\Saved\graph.db "PRAGMA journal_mode;"` | PASS: `delete`. |
| Source read-only query | `Plugins\Monolith\Binaries\monolith_query.exe source search_source UObject --limit=1` | PASS after restoring DELETE: returned symbol/source matches from `EngineSource.db`. |
| Source health | `Plugins\Monolith\Binaries\monolith_query.exe source health --include-counts=false` | PASS after restoring DELETE with an unrelated existing orphan-reference warning; `schema.journal_mode=delete`. |
| Bridge read-only query | `Plugins\Monolith\Binaries\monolith_query.exe bridge search_asset_symbols --symbol=UObject --limit=1` | PASS: `status=ok`, no warnings. |

---

## 9. Non-Goals

- Do not change DB schemas as part of journal-mode work.
- Do not copy the DB per offline CLI invocation as a concurrency solution; the source DB can be large and copying increases I/O, disk, and memory pressure without solving writer serialization.
- Do not permit concurrent SQLite writers. WAL improves reader/writer overlap, but SQLite still has a single writer.
- Do not use `MEMORY`, `OFF`, or persistent rollback journals for production Monolith index DBs.
- Do not manually delete `*-wal`, `*-shm`, or `*-journal` files as cleanup policy.

---

## 10. References

| Reference | Use |
|-----------|-----|
| <https://www.sqlite.org/pragma.html#pragma_journal_mode> | SQLite journal mode names and persistence behavior. |
| <https://www.sqlite.org/wal.html> | WAL concurrency, read-only constraints, sidecar file lifecycle, and checkpoint tradeoffs. |
| <https://www.sqlite.org/pragma.html#pragma_wal_checkpoint> | WAL checkpoint mode definitions. |
