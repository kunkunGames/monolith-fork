# Monolith CRG Scoped Refresh Verification

**Date:** 2026-06-16
**Scope:** MonolithIndex and MonolithSource CRG projection follow-up
**Result:** Passed

---

## 1. Build

Command:

```powershell
$projectRoot = (Get-Location).Path
$uproject = Get-ChildItem -LiteralPath $projectRoot -Filter *.uproject | Select-Object -First 1
$targetFile = Get-ChildItem -LiteralPath (Join-Path $projectRoot "Source") -Filter *Editor.Target.cs -Recurse | Select-Object -First 1
$editorTarget = if ($targetFile) {
  [System.IO.Path]::GetFileNameWithoutExtension([System.IO.Path]::GetFileNameWithoutExtension($targetFile.Name))
} else {
  "$([System.IO.Path]::GetFileNameWithoutExtension($uproject.Name))Editor"
}
$resolver = Join-Path $projectRoot "BatchFiles\Script\ResolveUnrealEngine.ps1"
$engineRoot = powershell -NoProfile -ExecutionPolicy Bypass -File $resolver -Project $uproject.FullName -Output Root
& "$engineRoot\Engine\Binaries\DotNET\UnrealBuildTool\UnrealBuildTool.exe" $editorTarget Win64 Development "-Project=$($uproject.FullName)" -WaitMutex -NoHotReloadFromIDE
```

Result: `Result: Succeeded`.

Note: earlier build attempts were blocked by stale headless editor/commandlet processes holding Monolith DLLs. The final build linked successfully after stopping the stale `UnrealEditor-Cmd.exe -run=MonolithReindex` process that was holding `UnrealEditor-MonolithSource.dll`.

## 2. Automation

Command:

```powershell
$report = Join-Path $projectRoot "Saved\Automation\MonolithCrgScopedRefresh_20260616_R11"
$log = Join-Path $projectRoot "Saved\Logs\MonolithCrgScopedRefresh_20260616_R11.log"
& "$engineRoot\Engine\Binaries\Win64\UnrealEditor-Cmd.exe" $uproject -NullRHI -Unattended -NoSplash -NoSound -nop4 -NoSourceControl "-ExecCmds=Automation RunTests Monolith.IndexGuard.Project.RefreshCrgCacheForAssetsScoped+Monolith.IndexGuard.Source.PruneIndexedFilesUnderRootsRemovesProjectSlice+Monolith.IndexGuard.Project.HealthWarnsOnStaleCrgCache+Monolith.IndexGuard.Source.KnownPathSymbolPreferred+Monolith.IndexGuard.Source.ResetDatabaseRecreatesMalformedFile; Quit" "-TestExit=Automation Test Queue Empty" "-ReportExportPath=$report" "-AbsLog=$log" -log
```

Report: `Saved\Automation\MonolithCrgScopedRefresh_20260616_R11\index.json`

| Test | Result |
|---|---|
| `Monolith.IndexGuard.Project.RefreshCrgCacheForAssetsScoped` | Success |
| `Monolith.IndexGuard.Source.PruneIndexedFilesUnderRootsRemovesProjectSlice` | Success |
| `Monolith.IndexGuard.Project.HealthWarnsOnStaleCrgCache` | Success |
| `Monolith.IndexGuard.Source.KnownPathSymbolPreferred` | Success |
| `Monolith.IndexGuard.Source.ResetDatabaseRecreatesMalformedFile` | Success |

Summary: 5 succeeded, 0 failures, total report duration 1.1556 s. The log contains unrelated PaperZD member-initialization `LogAutomationTest: Error` strings during startup, but the selected Monolith tests all report `Success` and the exported automation report has `failed=0`.

## 3. Project Reindex and Health

Project-only source reindex command:

```powershell
$log = Join-Path $projectRoot "Saved\Logs\MonolithReindexScopedRefresh_20260616_R10.log"
& "$engineRoot\Engine\Binaries\Win64\UnrealEditor-Cmd.exe" $uproject -run=MonolithReindex -mode=project -unattended -nopause -nosplash -nullrhi "-ABSLOG=$log"
```

Result:

| Check | Evidence |
|---|---|
| Commandlet result | Exit code 0; wall time 39.3 s |
| Project source indexing | `MonolithReindex: done in 31.9s - files=1937 symbols=12175 errors=0` |
| Scoped source CRG refresh | `Indexer: Scoped source CRG projection/cache refreshed for 1934 file(s), 12494 affected symbol(s)` |
| Scoped CRG step timing | `insert scoped nodes=0.380s`, `insert scoped reference edges=0.910s`, `insert scoped inheritance edges=0.151s`, `insert scoped metrics=4.034s` |
| Manual orphan check | `orphan_symbols=0`, `orphan_inheritance=0` in `Plugins\Monolith\Saved\EngineSource.db` |

Standalone CLI was rebuilt with `Plugins\Monolith\Tools\MonolithQuery\build.bat`; `Plugins\Monolith\Binaries\monolith_query.exe --version` reports `source_hash=ecfaa97ca26b8192`. Deep source health:

```powershell
Plugins\Monolith\Binaries\monolith_query.exe source health --include-counts=true --include-deep-checks=true
```

Result: `status=ok`, `warnings=[]`, `integrity:orphan_symbols=ok`, `integrity:orphan_references=ok`, `crg:nodes_row_parity=ok`, `crg:edges_row_parity=ok`, `crg:metrics_row_parity=ok`, `source_override_edges_version=2`, with counts `symbols=964295`, `valid native edges=84286`, `crg_edges=84286`, and `crg_node_metrics=964295`.

## 4. Coverage

| Requirement | Evidence |
|---|---|
| Project asset indexing can refresh derived CRG rows without a full rebuild when projection tables exist | `RefreshCrgCacheForAssetsScoped` inserts a new asset/dependency, runs scoped refresh, and verifies project `crg_nodes`, `crg_edges`, and `crg_node_metrics` parity. |
| Scoped project refresh does not duplicate dependency edges when both endpoints are affected | `RefreshCrgCacheForAssetsScoped` asserts `crg_edges == dependencies` after a dependency whose source and target are both in the affected set. |
| Deleted asset paths recompute neighbor risk metrics | `RefreshCrgCacheForAssetsScoped` deletes the new asset, refreshes by the deleted path, and verifies the neighbor inbound risk count is recomputed. |
| Project source reindex does not append duplicate symbols | `PruneIndexedFilesUnderRootsRemovesProjectSlice` prunes project-root files, verifies dependent rows are removed, reinserts the same project symbol, and asserts one remaining symbol. The fixture also verifies `InsertModule`/`InsertFile` return canonical ids after duplicate unique-key inserts. |
| Project source prune removes invalid dependent rows | `PruneIndexedFilesUnderRootsRemovesProjectSlice` seeds orphan symbols/inheritance and verifies the prune removes orphan symbol and invalid inheritance rows instead of leaving stale native inputs for CRG projection. |
| Project source indexing can refresh derived source CRG rows without a full rebuild when projection tables exist | `PruneIndexedFilesUnderRootsRemovesProjectSlice` calls `RefreshCrgCacheForFiles` for the reindexed file id, verifies source `crg_nodes`, `crg_edges`, and `crg_node_metrics` parity, and checks `source.health` is clean. The full project reindex log confirms scoped refresh on the live DB without touching `Saved\graph.db`. |
| Deleted source references recompute surviving-neighbor risk metrics | `PruneIndexedFilesUnderRootsRemovesProjectSlice` keeps an engine symbol neighbor after pruning the project slice and verifies `source.risk_score` reports the recomputed caller count. |
| Malformed source DB reset does not leave a broken file behind | `ResetDatabaseRecreatesMalformedFile` creates a malformed DB file and verifies `ResetDatabase` deletes/recreates the source DB instead of returning success over invalid SQLite bytes. |
| `Saved\graph.db` remains outside routine review/indexing refresh | No `source.build_crg_graph` invocation was required or performed. |

## 5. Screenshot Upload

Not applicable. This was editor automation and C++/SQLite projection behavior; no runtime visual, gameplay, UI, VFX, material, or asset-presentation change required screenshot capture or Discord upload.
