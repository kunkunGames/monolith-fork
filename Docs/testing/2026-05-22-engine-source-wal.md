# EngineSource WAL Gate Verification

**Date:** 2026-05-22
**Scope:** `EngineSource.db` WAL gate for Monolith source DB writers and offline read-only source/bridge queries
**Spec:** [../specs/SPEC_MonolithSQLiteJournalMode.md](../specs/SPEC_MonolithSQLiteJournalMode.md)

---

## 1. Build

| Command | Expected | Result |
|---|---|---|
| `cmd /c build.bat` from `Plugins\Monolith\Tools\MonolithQuery` | Rebuilds `monolith_query.exe` | PASS. Compile/link succeeded with existing MSVC C4819 encoding warnings. The batch copy first hit a locked `monolith_query.exe`; after the stale process was stopped, the rebuilt exe was copied to `Plugins\Monolith\Binaries\monolith_query.exe`. |
| Primary Go UBT command for `GoGameEditor Win64 Development` | Compile full editor target | BLOCKED by unrelated `Source\GoGame\Private\Tests\GoGASFoundationTests.cpp:1137` `TestEqual` overload ambiguity between `FGameplayTag` and `FNativeGameplayTag`. `UnrealEditor-MonolithSource.dll` was rebuilt before the target stopped, but the full target did not complete. |

---

## 2. WAL Gate

| Command | Expected | Result |
|---|---|---|
| Temporary `monolith_query.exe source repair_crg_cache --execute` WAL build | Determine whether standalone CLI SQLite can use WAL | PASS for the CLI-only layer. It rebuilt the source CRG cache and converted `Saved\EngineSource.db` to WAL, with `EngineSource.db-wal` checkpointed back to zero bytes. |
| `UnrealEditor-Cmd.exe ... -ExecCmds="Automation RunTests Monolith.IndexGuard.Source.DatabaseUsesWalJournalMode; Quit"` | Verify editor/MCP SQLiteCore can create/open source DBs in WAL | FAIL. UE SQLiteCore opened the temp DB but `PRAGMA journal_mode=WAL` returned/stayed `delete`; the source DB wrapper refused to continue. |
| Editor startup against the WAL-converted live DB | Verify editor/MCP source DB access remains usable | FAIL. Source subsystem logged SQLiteCore `unable to open database file` around WAL pragmas. |
| `sqlite3 Plugins\Monolith\Saved\EngineSource.db "PRAGMA wal_checkpoint(TRUNCATE); PRAGMA journal_mode=DELETE;"` | Restore editor/MCP-compatible live DB mode | PASS. Required so `EngineSource.db` is usable by UE SQLiteCore. |
| `sqlite3 -readonly Plugins\Monolith\Saved\EngineSource.db "PRAGMA journal_mode;"` | `delete` | PASS after rollback. Returned `delete`. |
| `sqlite3 -readonly Plugins\Monolith\Saved\ProjectIndex.db "PRAGMA journal_mode;"` | `delete` | PASS. Returned `delete`. |
| `sqlite3 -readonly Plugins\Monolith\Saved\graph.db "PRAGMA journal_mode;"` | `delete` | PASS. Returned `delete`. |

---

## 3. Runtime Smoke

| Command | Expected | Result |
|---|---|---|
| `Plugins\Monolith\Binaries\monolith_query.exe source health --include-counts=false` | Source health reports DELETE and remains readable | PASS. Returned `schema.journal_mode=delete`. Existing orphan-reference warning remains unrelated. |
| `Plugins\Monolith\Binaries\monolith_query.exe source search_source UObject --limit=1` | Read-only source query returns source data after rollback | PASS. Returned a symbol match and source-line match for `UObject`. |
| `Plugins\Monolith\Binaries\monolith_query.exe bridge search_asset_symbols --symbol=UObject --limit=1` | Bridge opens ProjectIndex and EngineSource read-only | PASS. Returned `status=ok` and no warnings. |
| `Plugins\Monolith\Binaries\monolith_query.exe project health --include-counts=false` | Project DB remains healthy in `DELETE` | PASS. Returned `status=ok` and `schema.journal_mode=delete`. |
| `Plugins\Monolith\Binaries\monolith_query.exe source crg_graph_health` | `graph.db` remains healthy | PASS. Returned `status=ok`, schema version 9, and node/FTS parity. |
| `UnrealEditor-Cmd.exe ... -ExecCmds="Automation RunTests Monolith.IndexGuard.Source.DatabaseUsesDeleteJournalMode; Quit"` | Focused editor automation confirms supported source DB journal mode | PASS. Report `Saved\AutomationReports\engine-source-delete-20260522\index.json` shows `succeeded=1`, `failed=0`, `state=Success`. |

---

## 4. Notes

This verification intentionally does not leave WAL enabled. The standalone CLI SQLite layer can use WAL, but live editor/MCP source DB access uses UE 5.7 `SQLiteCore`, whose `SQLITE_OS_OTHER` VFS does not provide WAL shared-memory hooks. `EngineSource.db`, `ProjectIndex.db`, and `graph.db` therefore keep rollback-journal `DELETE`; future WAL conversion requires a WAL-capable SQLite/VFS layer before changing the live DB.
