# Product Requirements Document / Technical Specification

## Feature: SQLite Performance Optimization (Non-WAL Read/Write Strategy)

**Status:** Implementation pass applied  
**Date:** 2026-04-24  
**Scope:** `MonolithIndexDatabase`, `MonolithSourceDatabase`, and the call sites that open and maintain Monolith SQLite databases

This revision adds an explicit reuse-and-cohesion pass. The overriding rule is the **Priority Lock** below; every other section in this document flows from it.

### Implementation Checkpoint (2026-04-24)

The implementation pass covers the review gaps that remained after the spec audit:

- Normal editor project-index query wrappers route through a separate `OpenForQuery()` read-only connection when indexing is not active.
- `FullTextSearch` now uses one unioned query with one post-union limit and explicit source weighting instead of merging two independently limited result sets in C++.
- Hot SQLite statements share `FMonolithSQLiteStatementCache`, a connection-scoped persistent statement cache in `MonolithCore`.
- Pragma preset selection now caps `mmap_size`, `cache_size`, and memory temp-store use against `FPlatformMemory::GetStats().AvailablePhysical`.
- FTS payloads include `search_tokens` populated through the shared `BuildMonolithSQLiteSearchText` helper so CamelCase and PascalCase Unreal identifiers are searchable by middle words.
- Static and SQLite smoke verification lives in `verify_sqlite_prd.py` beside this document.

### Priority Lock

The goals of this PRD can conflict at PR time. When they do, resolve in this fixed order. This ordering overrides any phrasing elsewhere in the document that could be read as prescribing a different balance.

1. **Preserve existing DB files and existing observable behavior.** Any change must be a no-op on legacy data unless explicitly called out as a migration step (§11.4 FTS augmentation is the only such step in this PRD). A PR that silently changes query results, row ordering, or stored format on an existing DB is rejected regardless of other merits.
2. **Minimize blast radius.** Every new branch must degrade to a warning log on legacy input, never to a hard failure. The only exception is Application ID mismatch (§13.2 item 9), which is a security/identity check and is allowed to hard-fail by design.
3. **Maximize cohesion and reuse within the bounds of 1 and 2.** This is the primary design lever of this PRD. Two concrete gates enforce it:
   - **Reuse check** — if the same logic exists twice today, the refactor must collapse it into one owner. No new helper that only one of the two DBs uses without a documented reason.
   - **Cohesion check** — a helper must own exactly one concern (connection policy, maintenance, statement cache, schema versioning, path resolution). If it grows a second concern, split it in the same PR.

   Do not defend duplication as "safer". Do defend cohesion boundaries (§6.3) against ad-hoc cross-module leakage.
4. **Maximize SQLite feature utilization within the bounds of 1-3.** This PRD targets a *reasonable ceiling* under the above constraints, not a physical maximum. Phrases like "maximum", "extract every last", or "fully utilize" are not justifications for violating 1-3. Features that require turning off crash-safety, rewriting schemas, or patching the UE SQLite wrapper are out of scope for this PRD by construction.

**Note on code churn (optional tiebreaker, not a gate):** when two designs are equivalent under 1-4, prefer the one with less churn. This is a reviewer preference, never a reason to skip needed reuse/cohesion work. If priority 3 calls for splitting a file, splitting a type, or introducing a new helper, do it. Do not under-refactor to save LOC.

If a reviewer and author disagree on whether a change respects this ordering, the change is deferred to a follow-up PR. Priority Lock (1-4) is not negotiable per-PR.

---

## 1. Overview

Monolith uses SQLite in two critical places:

- `MonolithIndexDatabase` for project indexing and FTS-backed asset search
- `MonolithSourceDatabase` for C++ source indexing and symbol lookup

These databases sit on a hot path for:

- project search latency
- source search latency
- startup/open cost
- incremental indexing throughput

The original optimization proposal correctly rejected a blind switch to WAL mode, but it overstated the reason and did not fully align with the actual codebase. This revised PRD fixes those issues and turns the optimization effort into a complete database-open and database-maintenance strategy, not just a set of `PRAGMA` statements.

### 1.1. Current-state snapshot (verified against v0.14.2 code)

These are observed facts in the repo at the time of writing. They define the baseline the refactor must improve against.

- `MonolithIndexDatabase::Open` (`MonolithIndexDatabase.cpp:247`) — `ReadWriteCreate` + full RAM-tier pragma block (38 lines, `:262-299`).
- `MonolithSourceDatabase::Open` (`MonolithSourceDatabase.cpp:115`) — `ReadWrite` + **only** `journal_mode=DELETE`; no `mmap_size`, `cache_size`, `temp_store`, `synchronous`.
- `MonolithSourceDatabase::OpenForWriting` (`:752`) — `ReadWriteCreate` + full RAM-tier pragma block (38 lines, `:765-802`) **character-wise near-duplicate of the Index block**.
- `ESQLitePreparedStatementFlags::Persistent` is used in exactly 3 call sites across the whole plugin. Hot paths (`FullTextSearch`, every `Insert*`, every `Get*`) re-prepare on each call.
- `FSQLitePreparedStatement::Reset()` is used in exactly 1 place (`MonolithSourceDatabase.cpp:1116`, `InsertSourceChunks`).
- `ExecuteSQL` (Index, `:1437`) and `ExecuteMulti` (Source, `:18`) are two near-duplicate BEGIN/END-aware semicolon splitters with slightly different logic.
- `FMonolithIndexDatabase` has no mutex; `FMonolithSourceDatabase` holds `FScopeLock DbLock` across every method, including `while (Stmt.Step())` iteration.
- FTS5 schema differs between the two DBs: Index uses `porter unicode61` + plain `rank`, Source uses default `unicode61` + `bm25()`. Neither declares a `prefix=` option. `symbols_fts` has AI/AD triggers but no AU trigger.
- `PRAGMA optimize`, `ANALYZE`, FTS5 `('optimize')`, `PerformQuickIntegrityCheck()`, `GetUserVersion`/`SetUserVersion`, `GetApplicationId`/`SetApplicationId`, `VACUUM`, `auto_vacuum`, `PRAGMA threads`, and explicit `PRAGMA page_size` are all unused.
- `MonolithSourceDatabase.cpp:1244-1248` contains dead trailing code that will not compile (the last build is older than the source; the next rebuild breaks). See §13.0.

The refactor's job is to turn these into one consistent, deduplicated, observable story.

---

## 2. Problem Statement

### 2.1. What we want

We want the fastest possible read performance for Monolith's SQLite-backed search operations while preserving compatibility with real-world Unreal workflows:

- source-controlled plugin directories
- read-only database files
- packaged or copied plugin installs
- Windows file-lock edge cases
- large projects with significant FTS usage

### 2.2. What is wrong with the current PRD

The earlier draft had these gaps:

1. It treated WAL as fundamentally incompatible with read-only use, which is too absolute.
2. It described read-only compatibility in terms of journal mode only, while the actual code still opens databases using read-write modes.
3. It claimed the same memory tuning had already been applied to all three open paths, which is not true.
4. It optimized only page-cache behavior, but ignored statement lifecycle, query planning, and FTS maintenance.
5. It had no verification mechanism to prove that SQLite actually accepted the requested pragmas.
6. It framed reuse as a soft goal. The real baseline has two ~38-line RAM-tier blocks, two semicolon-splitters, and two open-mode strategies that must collapse into one owner. "Introduce a shared helper" is not a recommendation — it is a necessary precondition for any of the other items to stay consistent.
7. It treated the FTS5 layer as a single surface even though Index (`porter unicode61` + plain `rank`) and Source (`unicode61` + `bm25`) currently have inconsistent tokenizer/rank/prefix/trigger policy. A performance spec that does not force a decision here will drift further, not less.
8. It did not specify a concurrency model. The two DB classes today use opposite patterns (no lock vs. lock-across-iteration). This affects every later question about prepared-statement ownership and connection reuse.

This revision corrects those issues.

---

## 3. Corrected Technical Facts

### 3.1. WAL is not categorically impossible for read-only databases

SQLite officially allows read-only access to WAL-mode databases on modern versions if one of the documented conditions is met, such as pre-existing readable `-wal` and `-shm` files, directory write access, or immutable open semantics.

However, WAL is still the wrong default for Monolith because:

- Monolith cannot assume those sidecar files already exist
- Monolith cannot assume directory write access in all installations
- Unreal plugin deployment often copies only the `.db` file and not WAL sidecars
- Monolith currently uses Unreal's `FSQLiteDatabase` wrapper, which does not expose URI-based immutable opens directly

**Conclusion:** Monolith should still prefer `journal_mode=DELETE`, but the justification must be "operational compatibility with Monolith's deployment model", not "SQLite fundamentally cannot read WAL databases in read-only environments."

### 3.2. Read-only compatibility depends on open mode, not just journal mode

The code currently opens:

- `MonolithIndexDatabase::Open` with `ReadWriteCreate`
- `MonolithSourceDatabase::Open` with `ReadWrite`
- `MonolithSourceDatabase::OpenForWriting` with `ReadWriteCreate`

That means "works when `.db` is read-only" is not guaranteed today even though `journal_mode=DELETE` is forced. The open mode strategy must be part of the optimization design.

Additional nuance:

- Unreal's `FSQLiteDatabase` wrapper defines `ReadWrite` and `ReadWriteCreate` as modes that may fall back to read-only when write access is unavailable.
- Monolith must distinguish:
  - intentional `ReadOnly`
  - downgraded read-only after a write-capable open attempt
  - fully write-capable opens

The optimization design is incomplete unless it explicitly tests and documents all three cases.

### 3.3. Pragmas are advisory and may silently no-op

SQLite documents that:

- unknown pragmas can be silently ignored
- `cache_size` is a suggested limit, not a guaranteed allocation
- `mmap_size` is capped by compile-time and startup-time limits
- changing `temp_store` has connection-scoped behavior and should be done immediately after opening the connection

**Conclusion:** Any optimization plan must include readback validation and logging.

### 3.4. FTS performance is not solved by cache pragmas alone

Monolith relies on FTS5 for project search. SQLite recommends:

- `PRAGMA optimize` as the standard planner maintenance entry point
- FTS5 `optimize` / `merge` / `rebuild` when appropriate for index maintenance

Without maintenance strategy, cache pragmas alone will not keep search latency stable as the database grows and churns.

---

## 4. Goals

When any two goals below disagree, §0 Priority Lock breaks the tie. In particular: cohesion/reuse (goal 1) outranks raw performance (goal 5), and no-op-on-legacy outranks every performance goal.

1. **Unify connection-policy and pragma-validation behavior** between `MonolithIndexDatabase` and `MonolithSourceDatabase`, consolidating duplicated SQLite plumbing into shared owners. *(Priority Lock 3 — primary design lever.)*
2. Preserve compatibility with read-only database files where the operation is genuinely read-only. *(Priority Lock 1.)*
3. Keep `journal_mode=DELETE` as the default journaling model.
4. Make pragma behavior deterministic, observable, and verifiable.
5. Improve search and lookup performance for both project and source databases, within the Priority Lock ceiling.
6. Add maintenance hooks for SQLite planner and FTS health.

---

## 5. Non-Goals

1. Do not modify Unreal Engine's SQLite implementation.
2. Do not adopt WAL mode as the default.
3. Do not redesign the Monolith database schema in this workstream. **In scope:** FTS5 augmentation (`prefix=` option, missing `symbols_au` trigger, tokenizer normalization) as spelled out in §11.4 — these are additive migrations, not redesigns. **Out of scope:** restructuring of core tables (`assets`, `nodes`, `symbols`, etc.), changing primary keys, splitting columns, or moving to `WITHOUT ROWID`/`STRICT` tables.
4. Do not add unrelated search features while fixing the open/pragma strategy.

---

## 6. Design Principles

1. The database open mode must match the actual intent of the caller.
2. Pragmas must be applied immediately after open and validated immediately after application.
3. Read-path safety takes priority over speculative write-path performance.
4. The optimization logic must exist in one shared place, not be duplicated across both database implementations.
5. Every optimization that can silently no-op must be observable in logs or diagnostics.
6. Reuse is enforced by the reuse check in §0. A PR that leaves both the old and the new path in place (parallel writes for "safety") has not completed the refactor — it has doubled the surface.
7. Cohesion is enforced by one-concern-per-helper. Connection policy, maintenance, statement caching, schema versioning, and path resolution are five distinct concerns and must not share a type.
8. Code churn is a tiebreaker, not a constraint. When cohesion (§6.7) calls for splitting or reuse (§6.6) calls for a new helper, execute it. Do not defer real reuse/cohesion work to avoid LOC delta. Churn only factors in when two designs are genuinely equivalent on priorities 1-3 of the Priority Lock.
9. The performance bar is a *reasonable ceiling under the Priority Lock*, not a physical maximum. Every optimization in this document respects the no-op requirement on legacy DBs. Higher-yield levers (turbo-mode full indexing, covering indexes requiring schema redesign, in-DB JSON filtering) are deliberately out of scope and tracked separately.

### 6.1. Cohesion and Reuse Rule

This work should maximize reuse where the behavior is actually identical, and refuse reuse where workload shape differs.

- Shared code should cover connection-policy concerns that are the same for both databases.
- Database-specific code should remain local when workload shape, maintenance timing, or query structure differs.

The goal is not to centralize everything into one helper. The goal is:

- high cohesion per component
- low duplication for truly shared logic
- minimal cross-database coupling

If one helper starts owning open-mode policy, pragma policy, maintenance policy, and query-shape decisions, it is too broad and should be split.

### 6.2. Reuse targets with measurable exit criteria

These are not aspirations. They are PR acceptance gates.

| Duplicated surface today | Target owner after refactor | Exit criterion |
|---|---|---|
| RAM-tier pragma block (`MonolithIndexDatabase.cpp:262-299` and `MonolithSourceDatabase.cpp:765-802`) | `MonolithSQLitePragmaPolicy` (new, in MonolithCore) | The 38-line block exists in exactly one `.cpp`. Both DBs call `SelectPragmaPreset(RamMB, bIs64Bit)`. |
| Multi-statement splitter (`MonolithIndexDatabase.cpp:1363+`, `MonolithSourceDatabase.cpp:18-81`) | `MonolithSQLiteExec::ExecuteMulti` (new) | One implementation. Both DBs remove their copy. Trigger-body BEGIN/END handling unified. Unit-tested against both schemas. |
| Open-mode + journal-mode decision | `OpenMonolithSQLiteDatabase` (new) | `Database->Open(...)` + `PRAGMA journal_mode=DELETE` + tuning preset applied through one call. Neither DB `.cpp` calls `PRAGMA journal_mode=` directly. |
| Schema version stamping (Index `meta.schema_version`, Source `meta.schema_version`) | SQLite-native `GetUserVersion`/`SetUserVersion` in a shared `MonolithSQLiteSchemaVersion` helper | `meta.schema_version` row becomes a compatibility mirror only; canonical source is the SQLite header. |
| Boilerplate "write update, then `SELECT changes();`" pattern (4 call sites in Index DB) | `MonolithSQLiteExec::ExecuteUpdate` returning changed row count through one prepared statement reused per connection | No more two-prepare UPDATE-then-`changes()` sequence. |

Any new helper that does not deduplicate an existing copy must document in the PR why.

### 6.3. Cohesion boundaries (explicit non-merges)

The following concerns must **not** land in the shared helper, even when tempting:

- FTS query text and `bm25()` weighting — lives in each DB's query file.
- Incremental index / delta planning — lives in the Index subsystem.
- Reference builder and C++ parsing — lives in MonolithSource.
- Any SQL string specific to one schema.

If a change accidentally moves any of these into the shared helper, revert it in the same PR.

---

## 7. Target Architecture Boundaries

The implementation should be separated into three layers.

### 7.0. Duplication audit (must shrink by the end of Phase 1)

Measured LOC of duplicated/near-duplicated SQLite logic today:

| Block | Location A | Location B | Approx. LOC | After refactor |
|---|---|---|---:|---:|
| RAM-tier pragmas | `MonolithIndexDatabase.cpp:262-299` | `MonolithSourceDatabase.cpp:765-802` | ~38 + ~38 | 1 copy, ~40 LOC + 2×1-line calls |
| Multi-statement splitter | `MonolithIndexDatabase.cpp:1363-1435` | `MonolithSourceDatabase.cpp:18-81` | ~70 + ~65 | 1 copy, ~70 LOC |
| `journal_mode=DELETE` hardcode | `MonolithIndexDatabase.cpp:257` | `MonolithSourceDatabase.cpp:124`, `:761` | 3 sites | 0 sites (owned by helper) |
| `SELECT changes();` after UPDATE | `MonolithIndexDatabase.cpp:960-967`, `:988-995`, `:1042-1049` | — | 3 blocks | 0 blocks (replaced by helper return value) |

**Exit criterion for Phase 1:** duplicated SQLite plumbing LOC drops by at least 60% compared to this table. Any PR that claims to land the shared helper but does not delete the old copies is not done.

### 7.1. Shared Layer — Connection Policy

Introduce a shared helper dedicated only to open-mode and pragma policy.

Suggested shape:

```cpp
enum class EMonolithSQLiteConnectionRole : uint8
{
	ReadMostly,
	WriteHeavy
};

enum class EMonolithSQLiteIntent : uint8
{
	QueryOnly,
	UpdateExisting,
	CreateOrRebuild
};

struct FMonolithSQLiteTuningResult
{
	FString JournalMode;
	int64 MmapSize = 0;
	int64 CacheSize = 0;
	int32 TempStore = 0;
	bool bForeignKeysEnabled = false;
};

struct FMonolithSQLiteOpenPolicy
{
	ESQLiteDatabaseOpenMode OpenMode;
	EMonolithSQLiteConnectionRole Role;
	bool bEnableForeignKeys = false;
};

bool OpenMonolithSQLiteDatabase(
	FSQLiteDatabase& Database,
	const FString& Path,
	const FMonolithSQLiteOpenPolicy& Policy,
	FMonolithSQLiteTuningResult* OutObserved);
```

This layer owns:

- intent -> `ESQLiteDatabaseOpenMode` mapping
- RAM-tier pragma selection
- pragma application
- pragma readback validation
- tuning diagnostics

This layer does not own:

- query text
- prepared statement caching
- FTS optimize/rebuild scheduling
- reindex orchestration

### 7.1.1. Required `Open` API Split

`MonolithIndexDatabase::Open()` currently behaves like both:

- connection open
- schema/bootstrap ensure

That is too broad for a clean read-only strategy.

The implementation should move toward an explicit split such as:

```cpp
bool OpenForQuery(const FString& Path);
bool OpenForWrite(const FString& Path, EMonolithSQLiteIntent Intent);
bool EnsureSchema();
```

Exact naming is flexible, but the separation is not. Query-only paths should not inherit bootstrap logic.

### 7.2. Per-Database Layer — Query and Schema Behavior

These should remain local:

- `MonolithIndexDatabase`
  - FTS queries
  - FTS result merge/sort behavior
  - schema creation and asset-index lifecycle
- `MonolithSourceDatabase`
  - symbol/reference query shapes
  - source-indexer write flow
  - source-db-specific maintenance timing

These are different problems and should not be forced into one abstraction.

### 7.2.1. Writer Policy Must Be More Granular Than One Generic Writer Mode

The current `ReadMostly` / `WriteHeavy` split is a good starting point, but not the final shape.

At minimum, the design should allow separate tuning/maintenance treatment for:

- full rebuild / bulk indexing
- incremental update / schema-touching writes
- lightweight metadata writes

Otherwise the shared helper will accumulate branching that reduces cohesion.

### 7.3. Shared-but-Separate Layer — Maintenance

If maintenance logic is shared, it should be a different helper from connection policy.

Suggested shape:

```cpp
struct FMonolithSQLiteMaintenanceOptions
{
	bool bRunPragmaOptimize = false;
	bool bRunFtsOptimize = false;
	bool bRunIncrementalVacuum = false;
	int32 IncrementalVacuumPageBudget = 1024; // bounded to avoid long pauses
};

bool RunMonolithSQLiteMaintenance(
	FSQLiteDatabase& Database,
	const FMonolithSQLiteMaintenanceOptions& Options);
```

This layer may own:

- `PRAGMA optimize`
- optional FTS optimize/rebuild hooks
- `PRAGMA incremental_vacuum(N)` calls (only effective when the DB was created with `auto_vacuum=INCREMENTAL`; no-op otherwise, so safe against legacy DBs)
- post-reindex maintenance

Keeping this separate prevents short-lived query opens from inheriting heavy maintenance work.

---

## 8. Required Open Mode Strategy

### 8.1. Reader / writer intent matrix

| Use case | Current behavior | Required behavior |
|---|---|---|
| Project index normal query open | `ReadWriteCreate` | Prefer `ReadOnly` when DB exists and caller is not mutating schema/data |
| Source DB normal query open | `ReadWrite` | Prefer `ReadOnly` |
| Full reindex / create fresh DB | mixed | `ReadWriteCreate` |
| Incremental index / schema migration / write path | mixed | `ReadWriteCreate` or `ReadWrite` depending on whether create is needed |

### 8.2. New rule

Monolith must split database opens into two explicit categories:

- **Read path**
  - Use `ESQLiteDatabaseOpenMode::ReadOnly` wherever the caller does not mutate DB content
- **Write path**
  - Use `ReadWriteCreate` for creation or rebuild flows
  - Use `ReadWrite` only when create is not desired and DB must already exist

### 8.3. Why this matters

This is the only way the success metric "read operations still work when the `.db` file is read-only" becomes meaningfully testable.

---

## 9. Shared PRAGMA Policy

Introduce a shared helper for connection policy, for example:

```cpp
bool OpenMonolithSQLiteDatabase(
	FSQLiteDatabase& Database,
	const FString& Path,
	const FMonolithSQLiteOpenPolicy& Policy,
	FMonolithSQLiteTuningResult* OutObserved);
```

### 9.1. Required base pragmas

The PRAGMA policy must be split by connection capability, not just by database type.

#### Read-only safe pragmas

These may be applied on true query-only connections, subject to engine-wrapper behavior verification:

- `PRAGMA mmap_size=...`
- `PRAGMA cache_size=...`
- `PRAGMA temp_store=2;` only when memory tier allows it

These are performance-oriented and do not intentionally mutate DB contents.

#### Write-capable pragmas

These should be applied only on connections opened with write capability:

- `PRAGMA journal_mode=DELETE;`
- `PRAGMA synchronous=NORMAL;`
- `PRAGMA threads=N;` — N derived from `FPlatformMisc::NumberOfCoresIncludingHyperthreads()`, clamped to `[0, 4]`. Hint only; SQLite silently ignores if compiled without threaded sorter. Readback in §10 confirms.

  **Build-flag verification (resolved 2026-04-23):** UE 5.7's `SQLiteCore.Build.cs` does not override `SQLITE_MAX_WORKER_THREADS`, so SQLite's compile-time default of `8` is in effect. `SQLITE_THREADSAFE` is also default (`1`, serialized). `PRAGMA threads` is therefore expected to take effect on desktop editor builds. Platforms compiled with `bCompileCustomSQLitePlatform=true` set `SQLITE_MUTEX_NOOP`, which disables the threaded sorter — this affects some mobile targets only and is out of scope for Monolith's editor plugin. Readback value `0` on any platform is logged at `Verbose` and treated as a no-op, not a failure.

For `MonolithIndexDatabase` on write-capable paths:

- `PRAGMA foreign_keys=ON;`

#### Create-only pragmas (fresh DB only)

These must be applied on the `ReadWriteCreate` path **before** any table is created, and only when the file did not pre-exist:

- `PRAGMA page_size=4096;` — matches SQLite default. Locked in explicitly so future default changes in vendored SQLite do not silently alter on-disk format. Revisit to `8192` only with benchmark evidence (tracked separately, not in this PRD).
- `PRAGMA auto_vacuum=INCREMENTAL;` — header-only flag; no immediate cost. Enables `PRAGMA incremental_vacuum(N)` later in the maintenance helper (§7.3). Legacy DBs created before this change keep their current auto_vacuum value until a `VACUUM` is manually run — acceptable.

Both pragmas are no-ops on pre-existing DBs (page_size is immutable after first write; auto_vacuum requires `VACUUM` to change). This bounds side effects to fresh creates.

#### Memory-tier pragmas (apply to both read-only and write-capable paths)

These are separate from the three categories above: the RAM-tier selection is the same whether the connection is read-only or write-capable; only the `synchronous`/`journal_mode`/`foreign_keys` set differs.

For sufficiently capable 64-bit read-mostly paths:

- `PRAGMA mmap_size=...`
- `PRAGMA cache_size=...`
- `PRAGMA temp_store=2;` only when memory tier allows it

For low-memory or 32-bit tiers:

- keep `mmap_size` and `cache_size` conservative
- omit `temp_store=2`

### 9.2. Critical behavioral requirements

1. `temp_store` must be set immediately after open, before any temp objects or prepared-query workload is established.
2. `mmap_size` must be treated as a request, not a guarantee.
3. Every applied pragma must be read back and logged.
4. `journal_mode=DELETE` must not be blindly executed on connections that are intentionally opened as `ReadOnly` until verified safe through the Unreal wrapper path.
5. The helper API should make it impossible for a caller to accidentally apply write-capable pragmas to a query-only connection.
6. The implementation must include an experiment matrix covering:
   - true `ReadOnly` opens
   - downgraded read-only after `ReadWrite` / `ReadWriteCreate`
   - fully write-capable opens
7. The final implementation must document which pragma subset is valid for each of those three states.

### 9.3. Proposed memory tiers

The current RAM-tier table is acceptable in spirit, but it must be defined in **one** place — the shared helper — and reused by both databases. Section 6.2 binds this as an exit criterion: the 38-line block exists in exactly one `.cpp` after Phase 1. Neither `MonolithIndexDatabase.cpp` nor `MonolithSourceDatabase.cpp` may retain a copy.

Keep the same rough ladder unless benchmark evidence disproves it:

- 64GB+ RAM: request 2GB mmap, 512MB cache, `temp_store=2`
- 32GB+ RAM: 1GB mmap, 256MB cache, `temp_store=2`
- 16GB+ RAM: 512MB mmap, 128MB cache, `temp_store=2`
- 8GB+ RAM: 256MB mmap, 64MB cache, `temp_store=2`
- `< 8GB`: 64MB mmap, 16MB cache, omit `temp_store=2`
- 32-bit: 32MB mmap, 8MB cache, omit `temp_store=2`

The actual observed `mmap_size` must be read back and logged because SQLite may cap the request due to build or runtime limits.

---

## 10. Readback Validation Requirements

After applying tuning, Monolith must query and persist the observed values:

- `PRAGMA journal_mode;`
- `PRAGMA mmap_size;`
- `PRAGMA cache_size;`
- `PRAGMA temp_store;`
- `PRAGMA foreign_keys;` where applicable
- `PRAGMA threads;`
- `PRAGMA page_size;` (once per open; mismatch vs requested on a fresh create is a warning)
- `PRAGMA auto_vacuum;` (once per open; value `0` on legacy DBs is expected and logged once at `Verbose`)
- `FSQLiteDatabase::GetApplicationId()` (see §13.2 item 9)

### 10.1. Required logging

On open, log a single structured summary like:

```text
SQLite tuning applied: role=ReadMostly, journal=delete, mmap_size=268435456, cache_size=-64000, temp_store=2, foreign_keys=1
```

### 10.2. Error handling

If a requested pragma is not reflected in readback:

- do not fail the open
- emit a warning
- continue with the observed state

This is required because SQLite pragmas can be ignored or capped.

---

## 11. FTS and Query Planner Maintenance

### 11.1. Add planner maintenance

Monolith should use SQLite's recommended maintenance entry point:

- `PRAGMA optimize;`
- `PRAGMA optimize=0x10002;` for the first maintenance pass on a freshly opened long-lived connection, or immediately after schema/index creation, so SQLite considers all tables rather than only tables touched by that connection

Run it:

1. after schema creation or index creation, using `0x10002` when the connection has no useful query history yet
2. after full reindex completion, using `0x10002` for the post-rebuild pass
3. after incremental indexing and on clean close of long-lived write connections when practical, using plain `PRAGMA optimize;`

**Version note:** UE 5.7.3 currently bundles SQLite 3.47.1. Since SQLite 3.46, `PRAGMA optimize` automatically applies a temporary analysis limit for any ANALYZE work it decides to run, so callers must not set a fixed `PRAGMA analysis_limit=1000` before `PRAGMA optimize` as default policy.

### 11.1.1. Planner statistics strategy (new)

`PRAGMA optimize` is the default planner-statistics maintenance mechanism for this workstream.

Required behavior:

- Do not run explicit `ANALYZE;` as a mandatory prerequisite to `PRAGMA optimize` on SQLite 3.46+.
- Use `PRAGMA optimize=0x10002;` after full rebuild/schema/index creation so SQLite may evaluate all tables even on a new connection.
- Use plain `PRAGMA optimize;` for incremental/clean-close maintenance after the connection has touched normal workload tables.
- Treat explicit `ANALYZE;` as an escape hatch only: it may be added when benchmark output or `EXPLAIN QUERY PLAN` diffs prove `PRAGMA optimize` did not produce useful stats. That commit must record the measured reason and the extra cost.
- On `ResetDatabase()`, clear stats implicitly (DROP+CREATE handles this) and let the next maintenance pass rebuild planner stats through `PRAGMA optimize=0x10002;`.

This lives in the maintenance helper (§7.3), not in the connection helper.

### 11.2. FTS5 maintenance policy

For the FTS-backed project index:

- evaluate whether `INSERT INTO fts_assets(fts_assets) VALUES('optimize');`
- and same for `fts_nodes`, `symbols_fts`, `source_fts`

should be run after full reindex only.

This must be benchmark-driven because FTS optimize can be expensive on larger datasets.

### 11.3. Why this is in scope

This PRD is about search performance, and FTS maintenance is part of search performance. Excluding it would leave the optimization plan incomplete.

### 11.4. FTS5 schema consistency policy (new)

Today the two DBs diverge on FTS5 configuration. This is a cohesion problem as much as a performance problem: two FTS surfaces answering to the same `project_search` / `source_search` UX need the same search behavior.

Required decisions, **resolved**:

| Aspect | Index DB (`fts_assets`, `fts_nodes`) | Source DB (`symbols_fts`, `source_fts`) | Final decision |
|---|---|---|---|
| tokenizer | was `porter unicode61` | was `unicode61` (default) | **`unicode61 remove_diacritics 2`** for all four tables. Porter removed: identifier-heavy corpora (`TextureRenderTarget2D`, `UE_LOG`) are mangled by English stemming, and the natural-language gain on `description`/`docstring` is smaller than the identifier loss. SQLite FTS5's default `unicode61` diacritic behavior is `remove_diacritics 1`; this PRD pins `2` explicitly to get the bug-fixed Latin diacritic behavior and avoid future SQLite-vendor drift. |
| `prefix=` option | missing | missing | **`prefix='2 3 4'`** on all four FTS tables. Covers 2/3/4-character prefix roots — the common Monolith query length. Current `"token"*` queries traverse the full index without this. |
| ranking | plain `rank` | `bm25(source_fts)` | **Per-corpus `bm25()` weights, locked as Phase 4 initial values** (tuning in Phase 5 is allowed but must land with a commit noting measured improvement): `bm25(fts_assets, 10.0, 3.0, 1.0, 2.0, 1.0)` — columns `asset_name, asset_class, description, package_path, module_name`. `bm25(fts_nodes, 5.0, 2.0, 1.0)` — `node_name, node_class, node_type`. `bm25(symbols_fts, 5.0, 3.0, 1.0)` — `name, qualified_name, docstring`. `bm25(source_fts)` (single indexed column `text`; others `UNINDEXED`). |
| AFTER UPDATE trigger | present (`fts_assets_au`, `fts_nodes_au`) | **missing on `symbols_fts`** | Add `symbols_au` mirror of `symbols_ai` + `symbols_ad` so future incremental UPDATEs don't silently desync the FTS index. |

Rationale for bm25 weights:

- `fts_assets`: `asset_name` is the primary query target (10.0). `asset_class` narrows by type (3.0). `package_path` often contains partial matches (2.0). `description` and `module_name` are filler (1.0).
- `fts_nodes`: `node_name` is the most specific identifier (5.0); `node_class` follows (2.0); `node_type` is coarse (1.0).
- `symbols_fts`: `name` is terse and exact-match-heavy (5.0); `qualified_name` carries namespace/module context (3.0); `docstring` is natural-language filler (1.0).
- `source_fts`: single indexed column, default weight 1.0 — no per-column tuning possible.

Schema change note: all four FTS5 changes above — tokenizer normalization, `prefix=` addition, `bm25()` weighting query rewrites, and the missing `symbols_au` trigger — together require a one-shot FTS rebuild on existing DBs. They must be folded into the **same** Source DB v2 migration and Index DB v3 migration, not split into separate scripts. This migration is the single "explicitly called out migration step" permitted by Priority Lock 1.

### 11.5. Incremental vacuum (new, low-cost addition)

- `MonolithSQLitePragmaPolicy` applies `PRAGMA auto_vacuum=INCREMENTAL` on fresh create only (§9.1 create-only pragmas). No effect on existing DBs.
- The maintenance helper's `bRunIncrementalVacuum` option calls `PRAGMA incremental_vacuum(IncrementalVacuumPageBudget);` after full reindex on write-capable connections.
- Page budget is bounded (default 1024 pages ≈ 4MB at `page_size=4096`) so the call is short and non-blocking for foreground editor work.
- Existing DBs created before this change see zero effect from the pragma call (SQLite reports 0 freed pages). No forced conversion — users who want it can `ResetDatabase()`.

This is additive, reversible, and does nothing bad on legacy files.

---

## 12. Query Path Improvements Beyond PRAGMA

This effort must explicitly include non-pragma performance work.

### 12.1. Prepared statement reuse (required, not investigative)

The v0.14.2 code re-prepares statements on every call across all hot paths. `ESQLitePreparedStatementFlags::Persistent` is used in 3 places; `FSQLitePreparedStatement::Reset()` is used in 1. The UE wrapper exposes both primitives directly. This is wasted work, not an open question.

Required behavior after Phase 3:

- All write-path inserts that run in tight loops during indexing (asset, node, connection, variable, parameter, dependency, actor, tag, config, cpp_symbol, datatable_row, source symbol, reference, include) must hold a persistent, connection-scoped statement cache. Each row binds + `Step()` + `Reset()` + `ClearBindings()`, no re-prepare.
- All read-path hot queries (project FTS asset/node search, source FTS symbol/source search, `GetAssetId`, `GetSavedHash`, `FindReferences` inner lookups) must be served from the same cache.

Cache ownership rules:

- Cache is connection-scoped. Owned by `FMonolithIndexDatabase` / `FMonolithSourceDatabase`, not by a free helper.
- Cache is invalidated on `Close()` and on any `ResetDatabase()` / schema migration. `Open()` constructs a fresh cache.
- Cache is invalidated before any DDL statement executes.
- Keyed by SQL text (const `TCHAR*` pointer identity OR hashed string, documented choice).
- No cross-DB helper owns statements — that would break cohesion (§6.3).

**Reuse check applies here.** The cache API must be the same shape for both DBs (same method names, same invalidation contract), even though each DB owns its own instance.

### 12.2. Query shape review

For hot queries:

- capture `EXPLAIN QUERY PLAN`
- record warm/cold timings
- validate whether join order or ranking computation can be improved

This pairs with §11.1.1: `EXPLAIN QUERY PLAN` output should be captured after planner maintenance has run, so query plans are not evaluated against an empty-statistics database.

### 12.3. FTS result merge: current implementation is mathematically incorrect

`MonolithIndexDatabase::FullTextSearch` (`MonolithIndexDatabase.cpp:1056-1118`) runs two separate FTS queries and then sorts the merged results by the raw `rank` column. Three concrete problems:

1. **Limit double-apply.** Each query does `LIMIT %d`, so a caller asking for 50 can trigger up to 100 `snippet()` computations before the final truncate to 50. `snippet()` is not free at scale.
2. **Rank scales are not comparable across FTS tables.** `rank` is a function of each table's column count, column weights, and per-document term distribution. Sorting a merged list by raw `rank` across `fts_assets` and `fts_nodes` is not meaningful ordering. Today's output looks sensible only because both tables happen to have similar schemas.
3. **Fixed snippet column.** `snippet(fts_assets, 2, ...)` always returns a `description` snippet even when the match was on `asset_name` or `package_path`.

Required resolution:

- Replace the two-query + memory-merge path with a single `UNION ALL` query ordered by a normalized `bm25(<table>, ...)` expression, or
- Run two queries but normalize each to a comparable score (e.g. `bm25 / max_bm25_in_result`) before merging.
- Use `highlight()` keyed to the actual matched column, or pre-compute per-column snippets and pick the non-empty one.

This is not a "future optimization" — it is a correctness issue dressed as a performance issue.

---

## 13. Implementation Requirements

### 13.0. P0 pre-work (blocks everything else)

Before any shared-helper refactor lands, the following must be resolved. Each is a small, mechanical change.

1. Remove the dead trailing block in `MonolithSourceDatabase.cpp:1244-1248`. The function closes at `:1243`; the following lines are orphaned fragments that will fail compilation on the next rebuild. The `.obj` timestamp is older than the source timestamp, so this break has not yet been surfaced.
2. Add `mmap_size` / `cache_size` / `temp_store` / `synchronous` tuning to `MonolithSourceDatabase::Open` (`:115`) so the read path matches the write path. Do this inline (copy of the existing block) as a temporary measure — the real fix is the shared helper in Phase 1, but leaving the read path untuned until then blocks realistic benchmarking.
3. Confirm that `PRAGMA journal_mode=DELETE` is not issued against any caller that intends `ReadOnly`. For now the code always opens with write capability, so this is a no-op check — document it and revisit when §8 rolls out.

These are not design work. They are the minimum state the refactor starts from.

### 13.1. Files in scope

- `Plugins/Monolith/Source/MonolithIndex/Private/MonolithIndexDatabase.cpp`
- `Plugins/Monolith/Source/MonolithSource/Private/MonolithSourceDatabase.cpp`
- any new shared helper under a common Monolith core/database utility location
- the first rollout read-path call sites that must switch to `ReadOnly`

#### First rollout targets

The initial implementation should explicitly target:

1. the normal project-index read/query path
2. `UMonolithSourceSubsystem::Initialize` read open for source lookup
3. full reindex and DB-create flows as write-capable paths
4. incremental index and schema-touching paths as write-capable paths

The rollout must sequence `MonolithIndexDatabase` open/bootstrap separation before switching project query paths to explicit `ReadOnly`.

### 13.2. Code changes required

1. Introduce a shared connection-policy helper
2. Split read-path and write-path open intent
3. Introduce a separate maintenance helper
4. Keep query-shape optimization local to each database implementation
5. Add pragma readback logging
6. Add planner maintenance hooks
7. Add benchmarks or at minimum timing instrumentation
8. Add an integrity-check policy around `FSQLiteDatabase::PerformQuickIntegrityCheck()`, not an unconditional every-open call. `quick_check` is O(N) in database size, so the default open path must not run it unless the caller explicitly requests verification. Run it after fresh create, `ResetDatabase()`, schema migration, full reindex completion, or a suspicious open/recovery condition. On query-only connections where verification is enabled, failure logs a warning and returns `false` from the open wrapper. On write-capable connections, failure may trigger `ResetDatabase()` only when the caller explicitly owns rebuild/recovery. Any benchmark that enables quick-check-on-open must report that condition separately from default open latency.
9. Stamp a Monolith-owned application ID via `SetApplicationId` on fresh create (`ReadWriteCreate` path where file did not pre-exist). On every subsequent open, read it via `GetApplicationId`:
   - value `0` on a write-capable connection → legacy Monolith DB, stamp it now and continue.
   - value `0` on an intentional `ReadOnly` / query-only connection → legacy Monolith DB with pending stamp; continue and log once at `Verbose`/`Display`. Do not escalate to a write-capable open solely to stamp the header, because read-only file compatibility outranks identity stamping.
   - value matches → continue.
   - value differs → refuse to use the file, log error, return `false`. Protects against accidental re-opens of foreign SQLite files in the same path slot.

   The ID constant lives in exactly one place — declared as `constexpr int32 MonolithSQLiteApplicationId = 0x4D4F4E4C;` in the shared identity header (colocated with the schema-version helper per §6.2). No DB-specific `.cpp` may hardcode the value. Reuse check (§0 Priority Lock 3) applies.

### 13.3. Code changes explicitly not required

- no Unreal Engine source patch
- no WAL adoption
- no schema redesign

---

## 14. Benchmark Protocol

Performance claims in this PRD must be backed by measurement.

### 14.1. Workloads

Measure separately:

1. project FTS search
2. source symbol exact lookup
3. source reference traversal
4. database open cost
5. full reindex completion time

### 14.2. Conditions

For each workload:

- cold open / cold cache
- warm open / repeated query
- small project DB
- large project DB
- in-editor in-process benchmark
- read-only file attribute benchmark
- downgraded open-mode benchmark
- immediate post-reindex benchmark
- aged/churned DB benchmark

### 14.3. Metrics

Capture:

- p50 latency
- p95 latency
- peak memory during indexing
- DB open failure rate on read-only files
- observed pragma values after open
- open mode requested vs mode effectively obtained
- `EXPLAIN QUERY PLAN` output for selected hot queries

---

## 15. Success Metrics

The work is complete only if all of the following are true:

### 15.1. Correctness

1. Project and source query paths use the correct open mode for their intent.
2. Reader paths continue to function when `.db` files are marked read-only.
3. All tuned pragmas are read back and logged (including `page_size`, `auto_vacuum`, `threads`).
4. Full reindex and schema/index creation run `PRAGMA optimize=0x10002`; incremental and clean-close maintenance run `PRAGMA optimize` (§11.1.1).
5. `FullTextSearch` result ordering is defined by a single, comparable scoring rule (§12.3).
6. `symbols_fts` has AI + AD + AU triggers; all four FTS tables declare `prefix='2 3 4'`.
7. Integrity verification runs in the explicit cases from §13.2 item 8 and never hides its open-latency cost inside the default benchmark path.
8. Fresh DBs and writable legacy DBs carry the Monolith application ID; read-only legacy DBs may open with a pending-stamp diagnostic; opens of foreign SQLite files in the same path slot are refused with a clear log.
9. No regression in DB integrity or incremental indexing correctness.

### 15.2. Performance (two baselines, captured explicitly)

Measured on a warm editor with the Lyra-class project DB. Numbers below are acceptance targets, not estimates.

Because Phase 0 already tunes `MonolithSourceDatabase::Open` (§13.0 item 2) before Phase 1 lands, the PRD defines **two** baselines:

- **Baseline A (historical):** captured before Phase 0. Represents the untuned Source DB read path. Kept for reference only.
- **Baseline B (working):** captured after Phase 0 and before Phase 1 merges. This is the baseline all Phase 1+ targets compare against.

| Metric | Baseline | Target vs Baseline B |
|---|---|---|
| Project FTS search p50 | B, warm | ≤ 0.8× |
| Project FTS search p95 | B, warm | ≤ 0.8× |
| Source FTS symbol search p50 | B, warm | ≤ 0.8× |
| Source reference traversal p50 | B, warm | ≤ 0.9× (less head-room here; cache-bound, not compile-bound) |
| DB open latency (Index) | B | ≤ 1.0× (no regression) |
| DB open latency (Source, read path) | B | ≤ 1.0× (Phase 0 already captured the big gain into Baseline B; Phase 1 should match it) |
| Full reindex wall time | B, captured before Phase 3 merges | ≤ 0.75× (from prepared-statement reuse alone, before maintenance scheduling changes) |

If any target is missed, the PR must either attach the `EXPLAIN QUERY PLAN` diff that explains the miss or reduce scope before merging.

### 15.3. Reuse and cohesion (measured at PR time)

These are the reuse/cohesion gates from §0 turned into numeric acceptance.

1. Duplicated SQLite plumbing LOC drops by at least 60% against the §7.0 table.
2. Zero direct `PRAGMA journal_mode=` call sites remain in `MonolithIndexDatabase.cpp` and `MonolithSourceDatabase.cpp` — the shared helper owns it.
3. The RAM-tier pragma block exists in exactly one `.cpp` file.
4. The multi-statement splitter exists in exactly one `.cpp` file.
5. `ESQLitePreparedStatementFlags::Persistent` usage count rises from 3 to **≥ 14 insert sites** (asset, node, connection, variable, parameter, dependency, actor, tag, config, cpp_symbol, datatable_row, source symbol, reference, include) **plus every read hot-path query listed in §12.1**. Verified by grep of `Persistent` across `Plugins/Monolith/Source`.
6. No helper in `MonolithCore` takes a dependency on `MonolithIndex` or `MonolithSource` types (preserves §6.3 cohesion boundary).

---

## 16. Risks and Mitigations

### Risk: Increased virtual memory usage

Mitigation:

- keep RAM-tier scaling
- keep 32-bit conservative
- omit `temp_store=2` on weak tiers

### Risk: Read-only open regressions

Mitigation:

- explicitly test with `ReadOnly`
- keep write paths separate
- test downgraded read-only behavior separately from intentional `ReadOnly`

### Risk: FTS optimize cost spikes

Mitigation:

- run only after full reindex initially
- benchmark before expanding usage

### Risk: Silent pragma no-ops

Mitigation:

- mandatory readback validation
- warnings on mismatches

### Risk: Integrity checks inflate open latency

Mitigation:

- do not run `PerformQuickIntegrityCheck()` on every default query open
- run it after create/reset/migration/full-reindex or explicit diagnostic requests
- report quick-check-enabled benchmarks separately from default open-latency benchmarks

### Risk: Over-centralized helper becomes a new god object

Mitigation:

- keep connection policy and maintenance policy in separate helpers
- keep query-shape optimization inside each DB class
- reject cross-database helpers that start encoding domain-specific query semantics

---

## 17. Recommended Rollout

Each phase has an explicit reuse/cohesion gate. A phase is not done until its gate passes.

### Phase 0 — P0 hotfixes

- Delete dead code `MonolithSourceDatabase.cpp:1244-1248`.
- Copy the RAM-tier tuning block into `MonolithSourceDatabase::Open` as a temporary measure so benchmarks in Phase 1 are not measuring an untuned read path. The copy is deleted again in Phase 1.
- **Gate:** project builds cleanly; editor launches; Index and Source DBs open with identical pragma readback on the same machine.

### Phase 1 — Shared connection policy (reuse-heavy)

- Add `MonolithSQLitePragmaPolicy` + `OpenMonolithSQLiteDatabase` in `MonolithCore`.
- Migrate `MonolithIndexDatabase::Open` and both `MonolithSourceDatabase` open methods to use it.
- **Delete the temporary inline pragma block added to `MonolithSourceDatabase::Open` in Phase 0 item 2.** Phase 1 is not complete if that copy still exists.
- Introduce shared multi-statement splitter; delete both per-DB copies.
- Add pragma readback + structured logging (§10).
- Introduce explicit open/bootstrap split (§7.1.1) where APIs conflate them.
- Apply create-only pragmas (`page_size`, `auto_vacuum=INCREMENTAL`) and the threaded-sorter hint (`PRAGMA threads`) inside the helper (§9.1).
- Wire the integrity-check policy and application-ID stamp/verify into the helper (§13.2 items 8, 9). Declare `MonolithSQLiteApplicationId` in the shared identity header.
- Land the concurrency contract from §19: add a one-sentence header comment to both `FMonolithIndexDatabase` and `FMonolithSourceDatabase` stating the access model, and enforce Index DB caller serialization at the subsystem/lifecycle boundary (§19.2).
- Adopt the initial 2-tier writer policy (`ReadMostly`/`WriteHeavy`) from §7.1. The 3-tier expansion from §7.2.1 is deferred to Phase 3.
- **Gate (from §15.3):** the §7.0 duplication table shows at least 60% LOC reduction. No `PRAGMA journal_mode=` direct call sites remain outside the helper. RAM-tier block exists once. The Phase 0 temporary copy is deleted. Fresh DBs and writable legacy DBs have the Monolith application ID in their header; read-only legacy DBs log a pending-stamp diagnostic; every open logs readback of `page_size`, `auto_vacuum`, `threads`. Both DB headers document their concurrency policy.

### Phase 2 — Maintenance helper

- Add `MonolithSQLiteMaintenance` (separate type from the connection helper, §7.3).
- Wire `PRAGMA optimize=0x10002` after schema/index creation and full reindex, plus plain `PRAGMA optimize` for incremental / clean close maintenance.
- Keep explicit `ANALYZE` as an optional measured fallback only when §11.1.1's escape-hatch condition is met.
- Wire optional FTS5 `('optimize')` post full reindex, benchmark-gated.
- Wire `PRAGMA incremental_vacuum(N)` option (§11.5), bounded by `IncrementalVacuumPageBudget`.
- Add timing instrumentation.
- **Gate:** `PRAGMA optimize`, any optional measured `ANALYZE` fallback, and `incremental_vacuum` are called from the maintenance helper only. No query path takes a dependency on the maintenance helper.

### Phase 3 — Prepared statement cache (per-DB, shared API shape)

- Add connection-scoped prepared statement cache to each DB. Same method surface, each DB owns its instance.
- Convert insert + read hot paths (§12.1 list) to use the cache.
- Replace the UPDATE + `SELECT changes();` pattern with a helper method that returns changed row count from one statement reuse.
- Expand the writer policy from the Phase 1 2-tier `ReadMostly`/`WriteHeavy` to the 3-tier shape described in §7.2.1 (full rebuild / incremental / lightweight). This is the phase where the cache's invalidation semantics already force a granular write model.
- **Gate:** `ESQLitePreparedStatementFlags::Persistent` usage count matches the §15.3 item 5 target (≥ 14 inserts + read hot paths). Full reindex wall time meets §15.2 target.

### Phase 4 — FTS schema consolidation

- Unify tokenizer, add `prefix='2 3 4'`, add `symbols_au` trigger, migrate rank path to `bm25()` with documented per-column weights (§11.4).
- Fix `FullTextSearch` merge correctness (§12.3).
- Bump schema version (Index v3, Source v2) and write migration.
- **Gate:** §15.1 items 5 and 6 are verifiable by reading the DDL.

### Phase 5 — Re-benchmark and lock

- Re-run the §14 benchmark suite.
- Add regression tests that assert pragma readback, tuning preset selection by RAM tier, schema version, and `FullTextSearch` ordering stability.
- **Gate:** all §15.2 performance targets hit; §15.3 reuse targets re-verified by grep.

---

## 18. Final Recommendation

Proceed, but do not implement this as "just add more pragmas".

The correct scope is:

- open-mode correctness
- shared connection-policy logic (the reuse lever)
- separate maintenance helper
- explicit open/bootstrap separation where current APIs mix both concerns
- readback validation (including `page_size`, `auto_vacuum`, `threads`)
- planner/FTS maintenance (`PRAGMA optimize` first, explicit `ANALYZE` only as a measured fallback) and incremental vacuum
- prepared statement reuse on hot paths (committed, not investigative)
- FTS5 consistency: tokenizer, `prefix=`, `bm25()` weights, missing AU trigger
- correctness fix to `FullTextSearch` merge
- file-identity (application ID) and gated integrity-check policy
- concurrency contract documented in headers, caller-synchronized enforcement on the Index DB
- benchmark-backed verification with two baselines (pre- and post-Phase 0), reuse/cohesion targets measured at PR time

If those are not part of the change, this optimization effort will look complete in code review while still leaving the main correctness, performance, and maintainability risks unresolved.

---

## 19. Concurrency Policy (new)

The two databases today use opposite concurrency patterns. Picking one policy and applying it consistently is part of the cohesion goal.

### 19.1. Current state

- `FMonolithIndexDatabase`: no internal mutex. The database connection is currently touched by both editor-thread query/live-callback flows and the `MonolithIndexing` background thread used by full indexing. The safe invariant is caller-side serialization of ownership, not game-thread-only access.
- `FMonolithSourceDatabase`: `FScopeLock DbLock` on every public method, including across `while (Stmt.Step())` loops. A long FTS scan blocks every other read and write on the same DB.

Neither pattern is documented.

### 19.2. Required policy

- Both DBs declare their concurrency contract in their public header, in one sentence. Same phrasing pattern (differ only in the "caller-synchronized" vs "internally serialized" clause).
- Policy choice: **single-connection serialized access, caller-synchronized for the Index DB; internal serialization for the Source DB**. For the Index DB, the caller may be the editor game thread or the `MonolithIndexing` background thread. Only one owner may touch the connection at a time.
- The shared helper does not hold a lock. Locking is a per-DB concern.
- For Source DB iteration: document that holding the DB lock across `Step()` is intentional under the current workload. If future multi-reader workloads appear, revisit before adopting WAL — not during this PRD.
- **Enforcement for the Index DB:** do **not** add `checkSlow(IsInGameThread())` to `FMonolithIndexDatabase`; that would break the existing full-index background thread. Instead, enforce caller serialization at `UMonolithIndexSubsystem` lifecycle boundaries: no query/live-callback write path may touch the connection while full/background indexing owns it, live callbacks remain disabled during incremental/full indexing, and entry points that would start competing DB work must `ensure`/`check` against `bIsIndexing` or an equivalent owner token. If future work needs concurrent query + indexing access, add a real per-DB lock or separate read/write connections before statement-cache reuse.

### 19.3. Why this matters for the reuse goal

Prepared statement caches (§12.1) have different invalidation semantics under different concurrency models. Pinning the policy before the cache lands prevents two near-identical caches with subtly different locking assumptions.
