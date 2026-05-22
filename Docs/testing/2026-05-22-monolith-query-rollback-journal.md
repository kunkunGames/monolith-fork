# Monolith Query Rollback Journal Verification

**Date:** 2026-05-22
**Scope:** `monolith_query.exe` rollback journal handling for offline read-only source and bridge queries
**Spec:** [../API_REFERENCE.md](../API_REFERENCE.md)

---

## 1. Build

| Command | Expected | Result |
|---|---|---|
| `cmd /c build.bat` from `Tools/MonolithQuery` | Rebuilds `monolith_query.exe` and copies it to `Binaries/` | PASS. Built and copied `Plugins/Monolith/Binaries/monolith_query.exe`; MSVC emitted the existing C4819 source-encoding warnings only. |

---

## 2. Runtime Smoke

| Command | Expected | Result |
|---|---|---|
| `Binaries\monolith_query.exe source health --include-counts=false` while `Saved\EngineSource.db-journal` existed | Read-only health returns structured JSON without repeated `attempt to write a readonly database` query errors | PASS. The first run recovered the hot rollback journal through SQLite and removed the journal file. |
| `Binaries\monolith_query.exe source health --include-counts=false` after the source reindex and CRG cache repair completed | Source DB is readable after recovery/reindex | PASS. Returned `status=warning` with one existing orphan-reference warning; core schema, FTS tables, triggers, schema version, symbol/FTS parity, and CRG cache parity were OK. |
| `Binaries\monolith_query.exe bridge search_asset_symbols --symbol=UObject --limit=1` after the source reindex completed | Bridge opens ProjectIndex and EngineSource through the same journal-safe read-only path | PASS. Returned `status=ok`, `success=true`, and no warnings. |

---

## 3. Notes

The first recovery rolled back a hot journal from an interrupted source-index transaction, leaving `EngineSource.db` in a consistent but bootstrap-sized state. The post-build `UnrealEditor-Cmd.exe -run=MonolithReindex` flow then rebuilt `EngineSource.db`; after it exited, `EngineSource.db-journal` was absent and offline source/bridge queries were usable again. The CLI fix intentionally never deletes journals by hand; it asks SQLite to recover hot journals and leaves active writer journals to the writer.
