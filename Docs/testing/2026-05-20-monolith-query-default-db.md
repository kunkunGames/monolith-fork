# Monolith Query Default DB Resolution Verification

**Date:** 2026-05-20
**Scope:** `monolith_query.exe` default DB resolution for `source`, `project`, `bridge`, and source CRG graph actions
**Spec:** [../SPEC_CORE.md](../SPEC_CORE.md)

---

## 1. Build

| Command | Expected | Result |
|---|---|---|
| `cmd /c build.bat` from `Tools/MonolithQuery` | Rebuilds `monolith_query.exe` and copies it to `Binaries/` | PASS. Built and copied `Plugins/Monolith/Binaries/monolith_query.exe`; MSVC emitted existing C4819 source-encoding warnings only. |

---

## 2. Runtime Smoke

| Command | Expected | Result |
|---|---|---|
| `Plugins\Monolith\Binaries\monolith_query.exe source get_symbol_context UObject --context-lines=1` | Opens default `Saved/EngineSource.db` without `--db` | PASS. Returned `UObject` context from `Engine\Source\Runtime\CoreUObject\Public\UObject\Object.h`. |
| `Plugins\Monolith\Binaries\monolith_query.exe source get_symbol_context UObject --db=Plugins\Monolith\Saved --context-lines=1` | Directory override remains compatible | PASS. Returned the same `UObject` context. |
| `Plugins\Monolith\Binaries\monolith_query.exe source get_symbol_context UObject --db Plugins\Monolith\Saved\EngineSource.db --context-lines=1` | Space-separated DB file override remains compatible | PASS. Returned the same `UObject` context. |
| `Plugins\Monolith\Binaries\monolith_query.exe source health --include-counts=false` | Opens default source DB | PASS. Returned `status=warning` with 25 checks; existing DB health warnings are unrelated to path resolution. |
| `Plugins\Monolith\Binaries\monolith_query.exe source health --db Plugins\Monolith\Saved\EngineSource.db --include-counts=false` | Source DB file override remains compatible | PASS. Returned `status=warning` with 25 checks. |
| `Plugins\Monolith\Binaries\monolith_query.exe project health --include-counts=false` | Opens default project DB | PASS. Returned `status=warning` with 36 checks; existing DB health warnings are unrelated to path resolution. |
| `Plugins\Monolith\Binaries\monolith_query.exe project health --db Plugins\Monolith\Saved\ProjectIndex.db --include-counts=false` | Project DB file override remains compatible | PASS. Returned `status=warning` with 36 checks. |
| `Plugins\Monolith\Binaries\monolith_query.exe bridge search_asset_symbols --symbol=UObject --limit=1` | Opens both default DBs without `--db` | PASS. Returned `status=ok` with no warnings. |
| `Plugins\Monolith\Binaries\monolith_query.exe source build_crg_graph --limit=1` | Dry-run opens default `Saved/EngineSource.db` and default `Saved/graph.db` without DB overrides | PASS. Reported source counts from `EngineSource.db` and current graph counts from `Saved/graph.db`. |
| `Plugins\Monolith\Binaries\monolith_query.exe source build_crg_graph --execute` | Rebuilds default `Saved/graph.db` without `--graph-db` | PASS. Rebuilt 1,184,293 graph nodes, 82,850 file nodes, 1,101,443 symbol nodes, 4,155,553 edges, and 1,184,293 FTS rows. |
| `Plugins\Monolith\Binaries\monolith_query.exe source crg_graph_health` | Checks default `Saved/graph.db` without `--graph-db` | PASS. Returned `status=ok`, `schema_version=9`, and node/FTS parity of 1,184,293. |
| `Plugins\Monolith\Binaries\monolith_query.exe source search_crg_graph UObject --limit=3` | Searches default `Saved/graph.db` without `--graph-db` | PASS. Returned `status=ok`, `count=3`, `used_fts=true`, and `truncated=true`. |

---

## 3. Notes

Default-path agent calls use the built-in DB resolution. `--db`, `--source-db`, `--project-db`, and `--graph-db` remain available for copied DBs, temporary smoke tests, or non-standard layouts.
