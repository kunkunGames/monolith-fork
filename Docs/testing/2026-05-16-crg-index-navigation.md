# Verification — CRG-Inspired Navigation P0

**Date:** 2026-05-16; projection-cache update 2026-05-17
**Branch:** `feat/crg-index-navigation-p0` (off `origin/master`)
**Scope:** 12 additive actions (`project.*` + `source.*`: impact_radius, health, repair_fts, repair_crg_cache, risk_score, review_context) + DB/helper code + rebuildable SQLite CRG projection/cache + offline CLI compatibility + automation tests.

---

## 1. Build (gate: zero errors)

Command (per CLAUDE.md):

```
& "D:\Engine\UE_5.7\Engine\Binaries\DotNET\UnrealBuildTool\UnrealBuildTool.exe" GoGameEditor Win64 Development -Project="D:\P4\game\GO.uproject" -WaitMutex -NoHotReloadFromIDE
```

**Result: `Succeeded`**. The 2026-05-17 projection-cache rebuild compiled and linked all changed TUs:
`MonolithIndexReview.cpp`, `ProjectImpactRadiusAction.cpp`, `ProjectHealthAction.cpp`,
`ProjectRepairFtsAction.cpp`, `ProjectRepairCrgCacheAction.cpp`, `ProjectRiskScoreAction.cpp`, `ProjectReviewContextAction.cpp`,
`MonolithIndexModule.cpp`, `MonolithIndexQueryTests.cpp`, `MonolithSourceReview.cpp`,
`MonolithSourceDatabase.cpp`, `MonolithSourceActions.cpp`, `MonolithSourceQueryTests.cpp`.
Linked `UnrealEditor-MonolithIndex.dll` + `UnrealEditor-MonolithSource.dll`. Zero warnings/errors in the changed modules.

## 2. Automation tests

Command:

```
& "D:\Engine\UE_5.7\Engine\Binaries\Win64\UnrealEditor-Cmd.exe" "D:\P4\game\GO.uproject" -Unattended -NoSplash -NullRHI -ExecCmds="Automation RunTests Monolith.IndexGuard; Quit"
```

**Result: `Succeeded`** with exit code 0.

Covered tests:

- `Monolith.IndexGuard.Project.FindByTypeClampsLimit`
- `Monolith.IndexGuard.Project.HealthHealthy`
- `Monolith.IndexGuard.Project.HealthWarnsOnOrphanDependency`
- `Monolith.IndexGuard.Project.ImpactRadiusCycleSafe`
- `Monolith.IndexGuard.Project.ImpactRadiusTruncates`
- `Monolith.IndexGuard.Project.RepairCrgCache`
- `Monolith.IndexGuard.Project.RepairFtsDryRun`
- `Monolith.IndexGuard.Project.RiskScoreUsesCrgCache`
- `Monolith.IndexGuard.Project.ReviewContextMinimal`
- `Monolith.IndexGuard.Source.HealthHealthy`
- `Monolith.IndexGuard.Source.HealthWarnsOnOrphanReference`
- `Monolith.IndexGuard.Source.ImpactRadiusCycleSafe`
- `Monolith.IndexGuard.Source.ImpactRadiusFiltersRefKind`
- `Monolith.IndexGuard.Source.RepairCrgCache`
- `Monolith.IndexGuard.Source.RepairFtsSourceDegrades`
- `Monolith.IndexGuard.Source.RiskScoreUsesCrgCache`
- `Monolith.IndexGuard.Source.ReviewContextMinimal`
- `Monolith.IndexGuard.Source.SearchSymbolsClampsLimit`

The projection-cache tests validate dry-run plans, execute rebuild row parity
(`crg_nodes`, `crg_edges`, `crg_node_metrics`), and `risk_score` cache hits.

## 3. Regression

Existing direct actions/handlers and the existing `Monolith.IndexGuard.*` clamp tests remain covered. New logic is additive (`FMonolithIndexReview` uses only the public DB surface; `MonolithIndexDatabase.cpp` unchanged). Source adds review helpers plus DB-scoped `health`/`repair_fts`/`repair_crg_cache`; existing source lookup actions remain unchanged.

## 4. Offline CLI

`Tools/MonolithQuery/monolith_query.cpp` mirrors the same CRG action names for `project` and `source`:
`impact_radius`, `health`, `repair_fts`, `risk_score`, `review_context`. The tool opens SQLite read-only with `PRAGMA query_only=ON` by default and switches to read-write only for explicit `repair_fts --execute`.

The 2026-05-17 CRG projection-cache update keeps offline behavior compatible:
offline `risk_score` remains query-time when `crg_*` cache rows are absent, and
offline cache rebuild is deferred by spec (`repair_crg_cache` is live/editor only
in this change).

Command:

```
cd D:\P4\game\Plugins\Monolith\Tools\MonolithQuery
.\build.bat
```

**Result: `Succeeded`** and copied `monolith_query.exe` to `D:\P4\game\Plugins\Monolith\Binaries\monolith_query.exe`.
MSVC emitted warning `C4819` for the existing source file encoding, but produced the executable.

Smoke commands:

```
.\Binaries\monolith_query.exe source health --include-counts=false
.\Binaries\monolith_query.exe source risk_score UGameplayAbility --limit=1
.\Binaries\monolith_query.exe source impact_radius UGameplayAbility --edge-kinds=inheritance --direction=in --max-depth=1 --max-results=3
.\Binaries\monolith_query.exe source repair_fts --target=source
.\Binaries\monolith_query.exe project health --include-counts=false
.\Binaries\monolith_query.exe project risk_score --limit=1
.\Binaries\monolith_query.exe project impact_radius /Game/Developers/TH/King/Textures/King --max-depth=1 --max-results=3
.\Binaries\monolith_query.exe project review_context /Game/Developers/TH/King/Textures/King --max-depth=1 --max-results=5
```

**Result: `Succeeded`**. `source health` returned `status=ok`; project CRG actions returned JSON results. Local `ProjectIndex.db` health returned `status=warning` because that database is missing `meta.schema_version`, while FTS/triggers/parity checks still completed.
