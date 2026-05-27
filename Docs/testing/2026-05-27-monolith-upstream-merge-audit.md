# Monolith 0.16.0 Upstream Merge Audit

| | |
|---|---|
| Date | 2026-05-27 |
| Branch | `master` |
| Scope | Upstream 0.16.0 merge audit, PR #469 coordination diff check, Blueprint action registry deduplication |
| Specs | `Docs/SPEC_CORE.md`, `Docs/specs/SPEC_MonolithBlueprint.md`, `Docs/API_REFERENCE.md` |

---

## 1. Merge / Diff Audit

| Check | Result |
|---|---|
| `tumourlove/master` ancestry in `origin/master` | PASS: upstream merge is contained in `origin/master` |
| GitHub PR #469 diff | PASS: PR touched only `.jules/agent-coordination.md` and `.jules/marshal.md`; current branch contains an expanded equivalent coordination map |
| Upstream 0.16.0 overlap audit | PASS: overlap files were reviewed in Core HTTP initialize instructions, proxy initialize instructions, Editor preview/inspection actions, Build.cs dependencies, release script excludes, docs, and descriptor counts |

Overlap metrics:

| Metric | Value |
|---|---:|
| Upstream files changed from merge base to `tumourlove/master` | 22 |
| Fork-custom files changed from merge base to pre-upstream fork parent | 806 |
| Files touched by both upstream and fork-custom deltas | 12 |

## 2. Action Registry Deduplication

Command:

```powershell
rg -n 'RegisterAction\(TEXT\("[^"]+"\),\s*TEXT\("[^"]+"\)' Source
```

Result: PASS.

| Metric | Value |
|---|---:|
| Total static registrations | 1518 |
| Unique `namespace.action` registrations | 1518 |
| Duplicate registrations | 0 |
| Unique `blueprint` registrations | 121 |

Notes:

- Pre-fix audit found `blueprint.remove_data_table_row` registered in both `MonolithBlueprintStructActions.cpp` and `MonolithBlueprintDataTableActions.cpp`.
- Runtime `RegisterAction` overwrites duplicate keys, so the later DataTable registration masked the guarded maintenance implementation.
- The DataTable duplicate registration and unused handler were removed; `remove_data_table_row` now resolves to the guarded implementation requiring `dry_run=true` or `confirm=true`.
- `Scripts/ci_static_checks.py` now blocks duplicate static `RegisterAction(TEXT(namespace), TEXT(action))` declarations.

## 3. Static Checks

| Command | Result |
|---|---|
| `git diff --check` | PASS |
| `python Scripts\ci_static_checks.py --config .github\monolith-static-ci.json selftest` | PASS |
| `python Scripts\ci_static_checks.py --config .github\monolith-static-ci.json check` | PASS: blocking findings `0`; advisory findings `389` from existing CRLF line endings plus missing external `.claude/agents` directory |
| Broad source scan | PASS: `RegisterAction` duplicate pairs `0`; handler references with missing definitions `0`; duplicate `IMPLEMENT_MODULE` entries `0`; unresolved merge conflict markers `0`; git unmerged index entries `0` |

## 4. Build

Command:

```powershell
$engineRoot = powershell -NoProfile -ExecutionPolicy Bypass -File "D:\P4\game\BatchFiles\Script\ResolveUnrealEngine.ps1" -Project "D:\P4\game\GO.uproject" -Output Root
& "$engineRoot\Engine\Binaries\DotNET\UnrealBuildTool\UnrealBuildTool.exe" GoGameEditor Win64 Development -Project="D:\P4\game\GO.uproject" -WaitMutex -NoHotReloadFromIDE
```

Result: PASS for no-link compile validation; full link was blocked by an external running editor lock in the continuation audit.

Notes:

- `UnrealBuildTool ... -NoLink` reported `Result: Succeeded`.
- Continuation audit reran `UnrealBuildTool ... -NoLink` on 2026-05-27 and reported `Target is up to date` plus `Result: Succeeded`.
- A full link attempt after the audit was blocked because running `UnrealEditor.exe` PID `51732` held `D:\P4\game\Binaries\Win64\UnrealEditor-GoGame.dll`. This was a file-lock/link issue, not a C++ compile error.
- Non-blocking warning: Monolith/Go depend on deprecated `MassEntity`.

## 5. Automation

Command:

```powershell
& "$engineRoot\Engine\Binaries\Win64\UnrealEditor-Cmd.exe" "D:\P4\game\GO.uproject" -NullRHI -NoSound -Unattended -NoSplash -NoP4 -ExecCmds="Automation RunTests Monolith.ParamGuard.Blueprint.DataTableMaintenance; Quit" -TestExit="Automation Test Queue Empty" -ReportExportPath="D:\P4\game\Saved\AutomationReports\monolith-merge-datatable-maintenance-current"
```

Result: PASS.

| Report | Value |
|---|---:|
| Succeeded | 0 |
| Succeeded with warnings | 5 |
| Failed | 0 |
| Report path | `D:\P4\game\Saved\AutomationReports\monolith-merge-datatable-maintenance-current\index.json` |

Notes:

- The warnings are expected test-local registry overwrite logs caused by repeated `FMonolithBlueprintStructActions::RegisterActions` calls inside the automation fixture.
- The fixture verified guarded DataTable writes, malformed boolean rejection, dry-run remove behavior, path guarding, and registration presence.

## 6. Editor 0.16 Preview / Inspect Automation

Command:

```powershell
& "$engineRoot\Engine\Binaries\Win64\UnrealEditor-Cmd.exe" "D:\P4\game\GO.uproject" -NullRHI -NoSound -Unattended -NoSplash -NoP4 -ExecCmds="Automation RunTests Monolith.Editor; Quit" -TestExit="Automation Test Queue Empty" -ReportExportPath="D:\P4\game\Saved\AutomationReports\monolith-merge-editor-current"
```

Result: PASS.

| Report | Value |
|---|---:|
| Succeeded | 16 |
| Succeeded with warnings | 0 |
| Failed | 0 |
| Report path | `D:\P4\game\Saved\AutomationReports\monolith-merge-editor-current\index.json` |

Covered high-risk 0.16 paths:

- `Monolith.Editor.Inspect.MaterialPBR`
- `Monolith.Editor.Inspect.TextureChannels`
- `Monolith.Editor.Preview.CaptureMaterialGrid`
- `Monolith.Editor.Preview.CaptureWithOverlay.*`
- `Monolith.Editor.Preview.CaptureStaticMesh`
- `Monolith.Editor.Preview.CaptureSkeletalMesh`
- `Monolith.Editor.Preview.CaptureWidget`

## 7. Persistence Regression Automation

Commands:

```powershell
& "$engineRoot\Engine\Binaries\Win64\UnrealEditor-Cmd.exe" "D:\P4\game\GO.uproject" -NullRHI -NoSound -Unattended -NoSplash -NoP4 -ExecCmds="Automation RunTests Monolith.Mesh.Persistence; Quit" -TestExit="Automation Test Queue Empty" -ReportExportPath="D:\P4\game\Saved\AutomationReports\monolith-merge-persistence-current"
& "$engineRoot\Engine\Binaries\Win64\UnrealEditor-Cmd.exe" "D:\P4\game\GO.uproject" -NullRHI -NoSound -Unattended -NoSplash -NoP4 -ExecCmds="Automation RunTests Monolith.AI.Persistence; Quit" -TestExit="Automation Test Queue Empty" -ReportExportPath="D:\P4\game\Saved\AutomationReports\monolith-merge-ai-persistence-current"
```

Result: PASS.

| Report | Succeeded | Succeeded with warnings | Failed | Path |
|---|---:|---:|---:|---|
| Mesh persistence | 2 | 0 | 0 | `D:\P4\game\Saved\AutomationReports\monolith-merge-persistence-current\index.json` |
| AI persistence | 1 | 0 | 0 | `D:\P4\game\Saved\AutomationReports\monolith-merge-ai-persistence-current\index.json` |

Covered high-risk 0.16 persistence paths:

- `Monolith.Mesh.Persistence.ConvertToHism`
- `Monolith.Mesh.Persistence.PlaceSpline`
- `Monolith.AI.Persistence.PlaceSmartObjectActor.RootComponent`

Notes:

- Current code verifies the Issue #63 component-persistence pattern in the fork's moved implementations: `mesh.convert_to_hism` lives in `MonolithLevelDesignEditingActions.cpp`, `scene.place_spline` lives in `MonolithLevelDesignPlacementActions.cpp`, and `ai.place_smart_object_actor` keeps both root and SmartObject components in `InstanceComponents`.
