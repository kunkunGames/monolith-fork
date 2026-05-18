# Niagara Layout Emitter Selector Verification

| | |
|---|---|
| Date | 2026-05-18 |
| Branch | `feat/niagara-layout-emitter-index` |
| Spec | `Docs/specs/SPEC_MonolithNiagara.md` |
| Scope | `niagara.auto_layout` emitter selector parity with core Niagara emitter actions |

---

## 1. Build

Command:

```powershell
& "D:\Engine\UE_5.7\Engine\Binaries\DotNET\UnrealBuildTool\UnrealBuildTool.exe" GoGameEditor Win64 Development -Project="D:\P4\game\GO.uproject" -WaitMutex -NoHotReloadFromIDE
```

Result: PASS.

Notes:

- Final UBT result was `Succeeded`.
- Pre-existing environment warnings remained non-blocking: invalid/unset `P4PASSWD`, deprecated `MassEntity`, and existing Build.cs deprecation warnings.

## 2. Automation

Command:

```powershell
& "D:\Engine\UE_5.7\Engine\Binaries\Win64\UnrealEditor-Cmd.exe" "D:\P4\game\GO.uproject" -NullRHI -NoSound -Unattended -NoSplash -NoP4 -ExecCmds="Automation RunTests Monolith.ParamGuard.Niagara.LayoutEmitterSelector; Quit" -TestExit="Automation Test Queue Empty" -ReportExportPath="D:\P4\game\Saved\AutomationReports\niagara-layout-emitter-selector"
```

Result: PASS. Automation report `index.json` shows 1 succeeded, 0 warnings, 0 failed.

## 3. Verified Contracts

| Contract | Result |
|---|---|
| `auto_layout` emitter selector accepts `list_emitters` numeric index strings | PASS |
| Out-of-range numeric selectors return `INDEX_NONE` instead of selecting a fallback emitter | PASS |
| Empty selectors and null systems remain rejected | PASS |

## 4. Static CI

Command:

```powershell
& "C:\Users\12336\.cache\codex-runtimes\codex-primary-runtime\dependencies\python\python.exe" Scripts\ci_static_checks.py --config .github\monolith-static-ci.json --github check
```

Result: PASS. Clean detached worktree run reported 0 blocking findings and 1 advisory `.claude/agents` external-prerequisite warning.
