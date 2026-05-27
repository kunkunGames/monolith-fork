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

## 4. Build

Command:

```powershell
$engineRoot = powershell -NoProfile -ExecutionPolicy Bypass -File "D:\P4\game\BatchFiles\Script\ResolveUnrealEngine.ps1" -Project "D:\P4\game\GO.uproject" -Output Root
& "$engineRoot\Engine\Binaries\DotNET\UnrealBuildTool\UnrealBuildTool.exe" GoGameEditor Win64 Development -Project="D:\P4\game\GO.uproject" -WaitMutex -NoHotReloadFromIDE
```

Result: PASS.

Notes:

- UBT reported `Result: Succeeded`.
- Non-blocking warning: Monolith/Go depend on deprecated `MassEntity`.

## 5. Automation

Command:

```powershell
& "$engineRoot\Engine\Binaries\Win64\UnrealEditor-Cmd.exe" "D:\P4\game\GO.uproject" -NullRHI -NoSound -Unattended -NoSplash -NoP4 -ExecCmds="Automation RunTests Monolith.ParamGuard.Blueprint.DataTableMaintenance; Quit" -TestExit="Automation Test Queue Empty" -ReportExportPath="D:\P4\game\Saved\AutomationReports\monolith-merge-datatable-maintenance"
```

Result: PASS.

| Report | Value |
|---|---:|
| Succeeded | 0 |
| Succeeded with warnings | 5 |
| Failed | 0 |
| Report path | `D:\P4\game\Saved\AutomationReports\monolith-merge-datatable-maintenance\index.json` |

Notes:

- The warnings are expected test-local registry overwrite logs caused by repeated `FMonolithBlueprintStructActions::RegisterActions` calls inside the automation fixture.
- The fixture verified guarded DataTable writes, malformed boolean rejection, dry-run remove behavior, path guarding, and registration presence.
