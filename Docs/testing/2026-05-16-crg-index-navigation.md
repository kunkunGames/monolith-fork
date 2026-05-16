# Verification — CRG-Inspired Navigation P0

**Date:** 2026-05-16
**Branch:** `feat/crg-index-navigation-p0` (off `origin/master`)
**Scope:** 10 additive actions (`project.*` + `source.*`: impact_radius, health, repair_fts, risk_index, review_context) + DB/helper code + automation tests.

---

## 1. Build (gate: zero errors)

Command (per CLAUDE.md):

```
& "D:\Engine\UE_5.7\Engine\Binaries\DotNET\UnrealBuildTool\UnrealBuildTool.exe" GoGameEditor Win64 Development -Project="D:\P4\game\GO.uproject" -WaitMutex -NoHotReloadFromIDE
```

**Result: `Succeeded`**. Final verification after the output-contract/test fixes reported `Target is up to date`; the preceding rebuild compiled and linked all changed TUs:
`MonolithIndexReview.cpp`, `ProjectImpactRadiusAction.cpp`, `ProjectHealthAction.cpp`,
`ProjectRepairFtsAction.cpp`, `ProjectRiskIndexAction.cpp`, `ProjectReviewContextAction.cpp`,
`MonolithIndexModule.cpp`, `MonolithIndexQueryTests.cpp`, `MonolithSourceReview.cpp`,
`MonolithSourceDatabase.cpp`, `MonolithSourceActions.cpp`, `MonolithSourceQueryTests.cpp`.
Linked `UnrealEditor-MonolithIndex.dll` + `UnrealEditor-MonolithSource.dll`. Zero warnings/errors in the changed modules.

## 2. Automation tests

Command:

```
& "D:\Engine\UE_5.7\Engine\Binaries\Win64\UnrealEditor-Cmd.exe" "D:\P4\game\GO.uproject" -unattended -nop4 -nosplash -NoSound -ExecCmds="Automation RunTests Monolith.IndexGuard; Quit" -TestExit="Automation Test Queue Empty"
```

**Result: `Succeeded`**. `Found 14 automation tests based on 'Monolith.IndexGuard'`; all completed with `Result={Success}` and `TEST COMPLETE. EXIT CODE: 0`.

Covered tests:

- `Monolith.IndexGuard.Project.FindByTypeClampsLimit`
- `Monolith.IndexGuard.Project.HealthHealthy`
- `Monolith.IndexGuard.Project.HealthWarnsOnOrphanDependency`
- `Monolith.IndexGuard.Project.ImpactRadiusCycleSafe`
- `Monolith.IndexGuard.Project.ImpactRadiusTruncates`
- `Monolith.IndexGuard.Project.RepairFtsDryRun`
- `Monolith.IndexGuard.Project.ReviewContextMinimal`
- `Monolith.IndexGuard.Source.HealthHealthy`
- `Monolith.IndexGuard.Source.HealthWarnsOnOrphanReference`
- `Monolith.IndexGuard.Source.ImpactRadiusCycleSafe`
- `Monolith.IndexGuard.Source.ImpactRadiusFiltersRefKind`
- `Monolith.IndexGuard.Source.RepairFtsSourceDegrades`
- `Monolith.IndexGuard.Source.ReviewContextMinimal`
- `Monolith.IndexGuard.Source.SearchSymbolsClampsLimit`

Note: the earlier `-nullrhi` command path hit an editor layout-save platform assertion unrelated to Monolith. The non-`nullrhi` command above is the verified runner for this test set.

## 3. Regression

Existing direct actions/handlers and the existing `Monolith.IndexGuard.*` clamp tests remain covered. New logic is additive (`FMonolithIndexReview` uses only the public DB surface; `MonolithIndexDatabase.cpp` unchanged). Source adds review helpers plus DB-scoped `health`/`repair_fts`; existing source lookup actions remain unchanged.
