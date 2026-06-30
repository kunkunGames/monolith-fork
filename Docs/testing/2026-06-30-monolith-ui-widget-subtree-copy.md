# Monolith UI Widget Subtree Copy Verification

**Date:** 2026-06-30
**Project:** Speed
**Scope:** MonolithUI post-copy Widget Blueprint subtree repair
**Changelist:** 1015

---

## 1. Summary

Added `ui.copy_widget_subtree_with_class_remap` as a guarded MonolithUI action. The action copies one or more source WBP widget subtrees into a destination WBP, remaps widget classes and hard/soft object references, defaults to dry-run, requires `confirm=true` for writes, and optionally compiles/saves the destination package.

## 2. Build

| Check | Result |
| --- | --- |
| `SpeedEditor Win64 Development` via `Build\BatchFiles\Script\ResolveUnrealEngine.ps1` | Passed. UBT compiled `MonolithUIModule.cpp`, `MonolithUIWidgetCopyActions.cpp`, and `MonolithUIWidgetSubtreeCopyTests.cpp`, then linked `UnrealEditor-MonolithUI.dll` with `Result: Succeeded`. |

## 3. Automation

Command:

```powershell
$tests = "Monolith.Registry.UI.CopyWidgetSubtreeWithClassRemapSchema+Monolith.ParamGuard.UI.CopyWidgetSubtreeRequiresRemap+Monolith.ParamGuard.UI.CopyWidgetSubtreeRequiresConfirm+Monolith.UI.WidgetSubtreeCopy.DryRunAndLiveSmoke"
& "$engineRoot\Engine\Binaries\Win64\UnrealEditor-Cmd.exe" $uproject -NullRHI -Unattended -NoSplash -NoSound -nop4 -NoSourceControl "-ExecCmds=Automation RunTests $tests;Automation Quit" "-TestExit=Automation Test Queue Empty" "-ReportExportPath=$report" "-AbsLog=$log" -log
```

Report: `Saved\Logs\Automation\MonolithUIWidgetSubtreeCopy\index.json`

| Test | Result |
| --- | --- |
| `Monolith.Registry.UI.CopyWidgetSubtreeWithClassRemapSchema` | Passed |
| `Monolith.ParamGuard.UI.CopyWidgetSubtreeRequiresRemap` | Passed |
| `Monolith.ParamGuard.UI.CopyWidgetSubtreeRequiresConfirm` | Passed |
| `Monolith.UI.WidgetSubtreeCopy.DryRunAndLiveSmoke` | Passed |

Summary: `succeeded=4`, `succeededWithWarnings=0`, `failed=0`, `notRun=0`, `totalDuration=0.37338429689407349`.

## 4. Screenshot

Not applicable. This is editor tooling and automation-test verification with no runtime or visual presentation change.
