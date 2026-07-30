# Monolith Frontend Focus Override Validation

**Date:** 2026-07-30  
**Scope:** `ui.validate_frontend_menu_flow` desired-focus resolution for Widget Blueprint overrides  
**Changelist:** 1361  
**Result:** Submit-ready: exact current bytes compiled/linked, focused automation and real Session Browser read-back pass, and final ownership/line-ending gates are clean

---

## 1. Regression

`/SpeedCore/UI/Menu/Experiences/W_SessionBrowserScreen` implements
`BP_GetDesiredFocusTarget` and connects `Get LyraListView.LyraListView` to the
function result `ReturnValue` pin. The pre-fix
`ui.validate_frontend_menu_flow` response nevertheless reported
`desired_focus_widget=None` because it inspected only
`UUserWidget::GetDesiredFocusWidgetName()` on the generated-class default
object.

That class-default value does not represent an authored Blueprint function
override, so the validator produced a false error for a valid focus contract.

---

## 2. Fix Contract

The validator now resolves focus in authority order:

1. If `BP_GetDesiredFocusTarget` exists, inspect only the
   `UK2Node_FunctionResult` node's `ReturnValue` connection; similarly named
   pins on ordinary graph nodes are not authoritative return points.
2. Accept a direct `UK2Node_VariableGet` whose variable names an existing
   widget in the same Widget Blueprint. If the function has multiple result
   nodes, every result must resolve to that same widget.
3. Fail closed for an unconnected, conflicting, multiply-linked, unsupported,
   or missing-widget override result.
4. Use `GetDesiredFocusWidgetName()` only when no Blueprint override graph
   exists.

Each requested focus check also reports
`desired_focus_resolution_source` and
`desired_focus_override_graph_present`, so an operator can distinguish a
Blueprint override from a class-default result.

---

## 3. Verification Gates

| Gate | Required result | Current result |
|---|---|---|
| Focus resolver automation | Direct value-pin override resolves; decoy/non-value/missing/multiply-linked/conflicting/unconnected/missing-result overrides fail closed; identical multi-return paths agree; no-override path uses class default | **PASS:** current-source run `automation-20260730T150839Z-2EB20072`, `1/1`, zero errors and warnings in 0.000469 seconds |
| Protected editor build | Coordinator-owned protected `Build\BatchFiles\BuildGameEditorAndRun.bat` run; this review must not start another build | **PASS:** the exact current `MonolithUICommonFrameworkTests.cpp` and `MonolithUIFrontendFlowActions.cpp` bytes compiled at lines 153 and 164, `UnrealEditor-MonolithUI.dll` linked at line 169, and the 24-action build ended `Result: Succeeded` in 37.56 seconds at lines 230-231 of `C:\Users\12336\AppData\Local\UnrealBuildTool\Log.txt`. This review did not run another build. |
| Monolith automation | `Monolith.UI.CommonFramework.FrontendFocusResolution` passes in the fresh linked binary | **PASS:** `automation-20260730T150839Z-2EB20072`, `1/1`, zero errors and warnings |
| Live regression read-back | `W_SessionBrowserScreen` resolves `LyraListView` from `blueprint_override` with no focus mismatch | **PASS:** current live `ui.validate_frontend_menu_flow` returned `overall_status=ok`, `desired_focus_widget=LyraListView`, `desired_focus_resolution_source=blueprint_override`, `desired_focus_override_graph_present=true`, no issues, and no warnings |
| Line endings | Every CL1361 text file uses CRLF | **PASS:** `TestSourceLineEndings.ps1 -ProjectRoot D:\P4\speed -Changelist 1361` verified `4/4` files after normalization; no bare LF remains |
| Source-control audit | Current files, no unresolved integrations, no foreign opens/locks, no unchanged files | **PASS:** exactly four cohesive files are open in CL 1361; `p4 fstat -Ol` reports no `otherOpen`, `otherLock`, or `unresolved`; `p4 resolve -n -c 1361` reports nothing to resolve; `p4 revert -n -a -c 1361` reports no unchanged file |

---

## 4. Screenshot Scope

No screenshot capture or Discord upload is required for CL 1361. This
changelist changes an editor-side read-only validator and its automation only;
it does not change runtime UMG layout, text, styling, navigation behavior, or
player-facing presentation.
