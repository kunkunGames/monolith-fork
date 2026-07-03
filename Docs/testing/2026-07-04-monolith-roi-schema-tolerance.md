# Monolith ROI Schema Tolerance Verification

| Field | Value |
|---|---|
| Date | 2026-07-04 |
| Scope | `ui.apply_common_menu_transform_spec` singleton `screens` tolerance and live MCP search `q` alias parity |
| Source | `Saved\Monolith\LogAnalysis\roi-20260703\summary.md` and `Saved\Monolith\SessionAnalysis\roi-20260703\summary.md` |

---

## 1. Results

| Check | Result | Evidence |
|---|---|---|
| `SpeedEditor` UBT build | Passed | `UnrealBuildTool.exe SpeedEditor Win64 Development "-Project=D:\P4\speed\Speed.uproject" -WaitMutex -NoHotReloadFromIDE` completed with `Result: Succeeded`. |
| Automation | Passed | `Saved\Logs\Automation\MonolithRoiSchemaTolerance_20260704-010436\index.json` reports `succeeded=7`, `failed=0`, `notRun=0`, `EDITOR_EXIT=0`. |
| UI singleton `screens` regression | Passed | `Monolith.Registry.UI.ApplyCommonMenuTransformSpecSchema` and `Monolith.UI.ApplyCommonMenuTransformSpec.SingleScreenObject` passed. |
| Live MCP search alias schema parity | Passed | `Monolith.Registry.ProjectIndex.SearchQueryAlias`, `Monolith.Registry.Source.SearchSourceQueryAlias`, and existing `Monolith.Registry.Source.CallGraphQueryAlias` passed. |
| Screenshot / Discord upload | N/A | This was editor tooling schema, handler normalization, and registry alias behavior only; no runtime visual, gameplay, UI presentation, VFX, animation, material, or asset-presentation change required screenshot capture or Discord upload. |

---

## 2. Commands

```powershell
$projectRoot = (Get-Location).Path
$uproject = Join-Path $projectRoot "Speed.uproject"
$resolver = Join-Path $projectRoot "Build\BatchFiles\Script\ResolveUnrealEngine.ps1"
$engineRoot = powershell -NoProfile -ExecutionPolicy Bypass -File $resolver -Project $uproject -Output Root
& "$engineRoot\Engine\Binaries\DotNET\UnrealBuildTool\UnrealBuildTool.exe" SpeedEditor Win64 Development "-Project=$uproject" -WaitMutex -NoHotReloadFromIDE
```

```powershell
$tests = "Monolith.Registry.ProjectIndex.SearchQueryAlias+Monolith.Registry.Source.SearchSourceQueryAlias+Monolith.Registry.Source.CallGraphQueryAlias+Monolith.Registry.UI.ApplyCommonMenuTransformSpecSchema+Monolith.UI.ApplyCommonMenuTransformSpec.SingleScreenObject+Monolith.UI.ApplyCommonMenuTransformSpec.DryRunDeferredAggregation+Monolith.ParamGuard.UI.ApplyCommonMenuTransformSpecRequiresConfirm"
& "$engineRoot\Engine\Binaries\Win64\UnrealEditor-Cmd.exe" $uproject -NullRHI -Unattended -NoSplash -NoSound -nop4 -NoSourceControl "-ExecCmds=Automation RunTests $tests;Automation Quit" "-TestExit=Automation Test Queue Empty" "-ReportExportPath=D:\P4\speed\Saved\Logs\Automation\MonolithRoiSchemaTolerance_20260704-010436" "-AbsLog=D:\P4\speed\Saved\Logs\Automation\MonolithRoiSchemaTolerance_20260704-010436\Run.log" -log
```

---

## 3. Notes

- `ui.apply_common_menu_transform_spec.screens` now advertises `array|object`; singleton object input is normalized to a one-entry array before focus and navigation screen lookups.
- `project.search`, `project.search_gameplay_tags`, and `source.search_source` keep canonical `query` but accept `q` through registry alias rewriting, matching the existing offline `monolith_query.exe --q` surface.
