# Offline CRG Cache Rebuild Verification

| Field | Value |
| --- | --- |
| Date | 2026-05-18 |
| Scope | `monolith_query.exe source repair_crg_cache`, `monolith_query.exe project repair_crg_cache` |
| Branch | `feat/offline-repair-crg-cache-writer` |

---

## 1. Build

| Command | Result |
| --- | --- |
| `cmd /c build.bat` from `Tools/MonolithQuery` | PASS. Built and copied `Plugins/Monolith/Binaries/monolith_query.exe`; MSVC emitted existing CP949 source-encoding warnings only. |

---

## 2. Offline Smoke

Smoke used copies of `Saved/EngineSource.db` and `Saved/ProjectIndex.db` under `%TEMP%/monolith-repair-crg-smoke-*`, so `repair_crg_cache --execute` did not mutate the live project DBs.

| Command | Expected Contract | Result |
| --- | --- | --- |
| `.\Binaries\monolith_query.exe project repair_crg_cache --db=<temp>` | Dry-run only, returns plan/counts and leaves `after={}` | PASS. `status=ok`, dry-run summary returned, `after={}`. |
| `.\Binaries\monolith_query.exe source repair_crg_cache --db=<temp>` | Dry-run only, returns plan/counts and leaves `after={}` | PASS. `status=ok`, dry-run summary returned, `after={}`. |
| `.\Binaries\monolith_query.exe project repair_crg_cache --db=<temp> --execute` | Rebuilds only derived project `crg_*` rows | PASS. `status=ok`, `crg_nodes=385`, `crg_edges=743`, `crg_node_metrics=385`, matching `assets=385` and `dependencies=743`. |
| `.\Binaries\monolith_query.exe source repair_crg_cache --db=<temp> --execute` | Rebuilds only derived source `crg_*` rows with dangling native refs excluded from CRG edges | PASS. `status=ok`, `crg_nodes=1065390`, `crg_edges=3080547`, `crg_node_metrics=1065390`; source native rows were `symbols=1065390`, `references=3125270`, `inheritance=36529`. |
| `.\Binaries\monolith_query.exe project health --db=<temp> --include-counts=false` | CRG table/parity/cache-version/scoring-version checks pass after rebuild | PASS with existing non-CRG warning count 1. |
| `.\Binaries\monolith_query.exe source health --db=<temp> --include-counts=false` | CRG table/parity/cache-version/scoring-version checks pass after rebuild | PASS with existing native orphan-reference warning count 1. |
| `.\Binaries\monolith_query.exe project risk_score --db=<temp> --limit=3` | Reads rebuilt cache and returns v3 cached risk | PASS. `status=ok`, summary reports `CRG cache hit`. |
| `.\Binaries\monolith_query.exe source risk_score Actor --db=<temp> --limit=1` | Reads rebuilt cache and returns v3 cached risk | PASS. `status=ok`, summary reports `CRG cache hit`. |

---

## 3. Notes

- `repair_crg_cache --execute` is the only new CRG cache write path. Dry-run `repair_crg_cache`, `risk_score`, `health`, `snapshot` dry-runs, and `diff_snapshots` remain read-only.
- Source rebuild intentionally joins reference/inheritance rows through existing symbol IDs, so pre-existing dangling `"references"` rows do not become orphan `crg_edges`.
- The remaining health warnings are pre-existing native DB issues, not CRG rebuild failures: ProjectIndex reports missing `meta.schema_version`; EngineSource reports 81,252 orphan native reference rows.
