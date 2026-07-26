# Monolith Source Scoped Maintenance Routing Verification

**Date:** 2026-07-26
**Scope:** `EngineSource.db` Graph FTS, override-edge cache, and full CRG repair planning
**Result:** Feature verification passed; OfflineParity acceptance blocked by an independently corrupted source database

---

## 1. Contract

Source health now keeps defect diagnostics independent while emitting the smallest non-overlapping execution plan.

| Defect range | Required action | Excluded work |
|---|---|---|
| Graph-node FTS only | `source.repair_fts target=graph_nodes` | No CRG or override-cache rebuild |
| Override-edge cache/version only | `source.repair_crg_cache scope=override_edges` | No `crg_nodes`, `crg_edges`, or `crg_node_metrics` rebuild |
| Core CRG parity/schema/metric defect | `source.repair_crg_cache scope=all` | No immediately redundant override-only pass |
| Core CRG plus override defect | `source.repair_crg_cache scope=all` | The full repair subsumes override-edge regeneration |

`Scripts\check_index_freshness.ps1` canonicalizes the legacy unscoped CRG spelling to `--scope=all`, compacts older health payloads that contain both full and override-only repairs, and rejects a repair plan whose scope is broader than the health flags permit.

## 2. Native Query Build and Offline Tests

Build command:

```powershell
Plugins\Monolith\Tools\MonolithQuery\build.bat
```

Published identity:

| Field | Value |
|---|---|
| Immutable executable | `Binaries\monolith_query-a2c470a0c741ff18.exe` |
| Source hash | `a2c470a0c741ff18` |
| SHA-256 | `5b9918ad3b74b6b6f7fd46620953a46bb4d104ad4e2b8469767f964ec1707406` |
| Catalog | `monolith_catalog-bc42a64e62d51f86625d7c42eec4343af706096a2ab7c992fcddae2ebd6928cd.json` |

Focused offline command:

```powershell
python Plugins\Monolith\Tools\MonolithQuery\test_engine_source_crg_search.py `
  --query-exe Plugins\Monolith\Binaries\monolith_query-a2c470a0c741ff18.exe
```

Result: **21 passed, 0 failed** in 3.552 seconds. The scoped fixture independently broke graph FTS, override-cache version, and core CRG parity, then verified each exact health action and repair boundary. Verification also exposed a native-reader/editor-writer collision: global `--readonly` still opened a database whose DELETE rollback journal belonged to the editor's custom `unreal-fs` VFS. The suite now holds a real rollback-journal writer open and proves Query refuses before opening or modifying it.

Pester command:

```powershell
Invoke-Pester -Path `
  "Plugins\Monolith\Scripts\tests\CheckIndexFreshness.Tests.ps1" -PassThru
```

Result: **7 passed, 0 failed** in 33.62 seconds. Coverage includes legacy full-plus-override compaction and fail-closed rejection of an override-only defect widened to a full CRG action.

## 3. Unreal Automation

The tests were dispatched through the live Monolith `editor.run_automation_tests` action after schema discovery.

| Test | Result | Run ID |
|---|---|---|
| `Monolith.IndexGuard.Source.ScopedMaintenanceRouting` | Passed, 0 errors, 0 warnings | `automation-20260726T074154Z-9ACC4EEE` |
| `Monolith.IndexGuard.Source.GraphSearchMigrationAndRepair` | Passed, 0 errors | `automation-20260726T074204Z-4F8486A1` |
| `Monolith.IndexGuard.Source.RepairCrgCache` | Passed, 0 errors, 0 warnings | `automation-20260726T074211Z-DAF09AAB` |
| `Monolith.IndexGuard.Source.RepairCrgBuildsOverrideEdgeCache` | Passed, 0 errors, 0 warnings | `automation-20260726T074221Z-D8873F84` |

The migration test emitted five expected SQLite warnings while proving repair from a fixture that intentionally lacked graph-node FTS backing tables. All four selected tests completed successfully against isolated temporary databases.

## 4. Protected Editor Build

The repository-protected wrapper was run with `P4_BUILD_CHANGELIST=1327` and `SKIP_EDITOR_LAUNCH=1`:

```powershell
$env:P4_BUILD_CHANGELIST = "1327"
$env:SKIP_EDITOR_LAUNCH = "1"
& Build\BatchFiles\BuildGameEditorAndRun.bat
```

Evidence: `Saved\Logs\MonolithScopedMaintenanceBuild-20260726.log`.

Result: `Target is up to date`, `Result: Succeeded`, total execution time 1.91 seconds. The linked `Binaries\Win64\UnrealEditor-MonolithSource.dll` contains the new `Monolith.IndexGuard.Source.ScopedMaintenanceRouting` registration and was loaded by the successful live automation runs.

## 5. OfflineParity Blocker

The exact static CI command was run:

```powershell
python Scripts\ci_static_checks.py --config .github/monolith-static-ci.json --github check
```

The source and focused tests passed. The final aggregate gate reports one
blocking acceptance finding:

```text
benchmark-contract-tests: accepted OfflineParity input size drifted: Binaries/monolith_query.exe
```

An earlier run against the missing/incomplete canonical source DB additionally
reported OfflineParity score `0.4418` and error rate `0.7974`. Those two
environment-derived findings were absent from the final run; the accepted
bundle identity drift is the only remaining blocker.

A replacement accepted bundle cannot be generated from the current canonical source DB. On headless startup, the existing 3,825,901,568-byte database failed an incremental write with `database disk image is malformed` and was automatically quarantined to:

```text
Plugins\Monolith\Saved\Corrupt\EngineSource-20260726-quickcheck-failed\EngineSource.db
```

During the first automatic clean rebuild, static CI's global-`--readonly` OfflineParity smoke overlapped the editor writer. UE SQLiteCore uses `unreal-fs`, while native Query uses the Win32 SQLite VFS; the mixed-VFS rollback-journal probe preceded editor `disk I/O error` writes and a failed 26,455-file partial index. Query now treats any rollback journal as an active-writer boundary under global `--readonly`, without opening either a read-only or recovery handle. The partial DB was quarantined separately under `EngineSource-20260726-reindex-disk-io`.

The subsequent clean retry was interrupted by an external `ConsoleCtrl RequestExit` at 16:57:19 KST before completion and was preserved under `EngineSource-20260726-reindex-consolectrl-interrupted`. A later launcher-bound retry reached 637/1372 modules and was preserved with its rollback journal under `EngineSource-20260726-reindex-launcher-timeout` after the launcher terminated at its 800-second limit.

A detached retry removed that launcher limit but stopped at 122/1372 modules
when the `Online` module transaction failed to commit:

```text
[2026.07.26-08.25.29:774] Indexer: failed to commit transaction for module 'Online'
```

The proxy invocation log records a separate offline
`source.read_source` request starting at 17:25:29.692 KST, 82 ms before the
writer reported the commit failure. The request itself returned an error, and
the available log does not prove that it opened the database; the overlap is
therefore recorded as a concurrent-environment correlation rather than a
confirmed SQLite root cause.

After both processes exited, the canonical DB had no rollback journal and no
active Monolith writer. Read-only deep health reported internally consistent
Graph FTS parity (`296107/296107`) but only the partial rebuild payload
(`281927` symbols), no CRG projection cache, and the exact minimal full-CRG
action:

```text
source.repair_crg_cache --scope=all
```

Repairing CRG on that partial source payload would make the derived cache
consistent with incomplete input, so it was intentionally not executed. A
healthy full source rebuild remains required before benchmark acceptance.

The incomplete `scoped-maintenance-routing-20260726-01` benchmark attempt was not accepted and did not update shared benchmark pointers. A healthy, fully rebuilt canonical `Plugins\Monolith\Saved\EngineSource.db` is required before a new accepted OfflineParity bundle can close this independent gate.

## 6. Coverage

| Requirement | Evidence |
|---|---|
| Graph FTS can be repaired without rebuilding CRG | Native and offline scoped fixtures report only `repair_fts graph_nodes` for the missing graph FTS trigger/table case. |
| Isolated override drift avoids full CRG cost | Native and offline fixtures report only `repair_crg_cache scope=override_edges`; dry-run follow-up routes to `find_overrides`. |
| Core CRG corruption receives a full repair | Native and offline fixtures report `repair_crg_cache scope=all`; repair restores node, edge, metric, and override parity. |
| Full CRG and override actions never execute back-to-back | Live health uses full/else-if override routing; the PowerShell executor compacts legacy payloads that list both. |
| Automation does not widen a health-indicated scope | Pester rejects a full CRG action when only `repair_override_edges_required` is true. |
| Live and offline behavior remain aligned | The same scoped scenarios pass in Unreal automation and the standalone Query integration suite. |
| Offline verification cannot break an active editor writer | Global `--readonly` refuses a real rollback-journal fixture before opening the database and leaves the writer's journal intact until rollback. |

## 7. Screenshot and Discord Upload

Not applicable. This change affects C++/SQLite health planning and command-line repair routing only; it has no runtime visual, UI, gameplay, VFX, material, or asset-presentation change, so no `1920x1080` screenshot or Discord upload was required.
