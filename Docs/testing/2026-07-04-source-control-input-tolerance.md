# Source Control Input Tolerance Verification

| Field | Value |
| --- | --- |
| Date | 2026-07-04 |
| Project | `D:\P4\speed\Speed.uproject` |
| Scope | `source_control` additive input tolerance for `files`, scalar path strings, and boolean string literals |

---

## 1. Results

| Check | Result |
| --- | --- |
| ROI trigger | `SPEC_MonolithToolCallReliabilityBacklog.md` tracked latent `source_control` failures for `files` instead of `paths`, scalar `paths`, and string booleans such as `dry_run="true"`. |
| Source schema update | `source_control` actions that take paths now declare `paths` as `array|string` with alias `files`; boolean options that need tolerance declare `bool|string`. |
| Handler update | Source-control handlers wrap a single string path into a one-item list and parse only known boolean literals: `true`, `false`, `1`, `0`, `yes`, `no`, `on`, `off`. |
| Safety negative cases | Numeric booleans and arbitrary boolean strings such as `later` and `sure` remain errors. |
| Automation coverage | Added `Monolith.ParamValidation.MonolithSourceControl.InputTolerance` and updated `Monolith.ParamValidation.MonolithSourceControl.TypedParams`. |
| Primary `SpeedEditor Win64 Development` UBT build | Passed after fixing one local variable shadowing diagnostic in `MonolithSourceControlActions.cpp`. |
| `recover_mcp.ps1` | Passed, restored MCP at `http://localhost:9316/mcp` after the editor process exited during the earlier Live Coding attempt. |
| Live `source_control.get_status(files="Project.uproject")` | Passed, accepted `files` as alias for `paths` and a scalar path string. |
| Live `source_control.delete(paths="Project.uproject", dry_run="yes")` | Passed without mutation, accepted scalar `paths` and string `dry_run`. |
| Live `source_control.checkout_or_add(paths="Project.uproject", dry_run="later")` | Passed as negative case, returned handler error `dry_run must be a bool or one of true/false/1/0/yes/no/on/off.` |
| `editor.run_automation_tests(prefix="Monolith.ParamValidation.MonolithSourceControl")` | Passed, 2/2 tests succeeded with 0 errors and 0 warnings. |

## 2. Notes

The canonical source-control call shape remains `paths: [...]` with real booleans. The compatibility path is intentionally narrow and source-control-specific; it does not change global parameter validation and does not remove destructive confirm gates.
