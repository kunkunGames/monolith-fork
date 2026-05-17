# Offline Snapshot / Diff Parity Verification

| Field | Value |
| --- | --- |
| Date | 2026-05-18 |
| Scope | `monolith_query.exe source snapshot`, `source diff_snapshots`, `project snapshot`, `project diff_snapshots` |
| Branch | `feat/offline-snapshot-diff-parity` |

---

## 1. Build

| Command | Result |
| --- | --- |
| `cmd /c build.bat` from `Tools/MonolithQuery` | PASS. Built and copied `Plugins/Monolith/Binaries/monolith_query.exe`; MSVC emitted existing CP949 source-encoding warnings only. |

---

## 2. Offline Smoke

Smoke used copies of `Saved/EngineSource.db` and `Saved/ProjectIndex.db` under `%TEMP%/monolith-snapshot-smoke`, so `snapshot --execute` did not mutate the live project DBs.

| Command | Expected Contract | Result |
| --- | --- | --- |
| `.\Binaries\monolith_query.exe source snapshot --db=<temp> --label=smoke-source` | Dry-run only, `executed=false`, current CRG projection counts returned | PASS. `status=ok`, `executed=false`, source projection count returned. |
| `.\Binaries\monolith_query.exe source snapshot --db=<temp> --label=smoke-source --execute` | Creates/updates `crg_snapshots`, returns `id`, `label`, counts, and next actions | PASS. `status=ok`, `executed=true`, `node_count=1065390`, `edge_count=2238254`. |
| `.\Binaries\monolith_query.exe source diff_snapshots smoke-source current --db=<temp> --limit=3` | Read-only diff against current manifest | PASS. `status=ok`, `summary_counts` all zero for unchanged copied DB. |
| `.\Binaries\monolith_query.exe project snapshot --db=<temp> --label=smoke-project` | Dry-run only, `executed=false`, current CRG projection counts returned | PASS. `status=ok`, `executed=false`. |
| `.\Binaries\monolith_query.exe project snapshot --db=<temp> --label=smoke-project --execute` | Creates/updates `crg_snapshots`, returns `id`, `label`, counts, and next actions | PASS. `status=ok`, `executed=true`, `node_count=1`, `edge_count=0`. |
| `.\Binaries\monolith_query.exe project diff_snapshots smoke-project current --db=<temp> --limit=3` | Read-only diff against current manifest | PASS. `status=ok`, `summary_counts` all zero for unchanged copied DB. |

---

## 3. Notes

- `snapshot --execute` is the only new write path. Dry-run `snapshot` and all `diff_snapshots` calls continue opening the DB read-only.
- The action does not rebuild `crg_nodes` / `crg_edges`; it only captures and compares the derived projection manifest that already exists.
