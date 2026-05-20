# Tool Invocation Daily Logs Verification

Date: 2026-05-20
Module: MonolithCore, MonolithProxy, MonolithQuery
Result: P0 verified; format v2 proxy/query follow-up verified, editor action relink blocked by a running editor process

---

## 1. Scope

Verified the P0 daily invocation log contract for:

| Surface | Expected file |
|---|---|
| MCP stdio proxy | `Plugins/Monolith/Logs/yyyyMMdd/proxy.jsonl` |
| Offline query CLI | `Plugins/Monolith/Logs/yyyyMMdd/query.jsonl` |
| Editor action dispatch | `Plugins/Monolith/Logs/yyyyMMdd/action.jsonl` |

## 2. Commands And Results

| Gate | Command | Result |
|---|---|---|
| Node proxy syntax | `node --check Scripts\monolith_proxy.js` | Passed |
| Python proxy/static syntax | `uv run python -m py_compile Scripts\monolith_proxy.py Scripts\ci_static_checks.py` | Passed |
| Static CI | `uv run python Scripts\ci_static_checks.py check` | Passed, 0 blocking findings |
| Query build | `cmd /c Tools\MonolithQuery\build.bat` | Passed; copied `Binaries\monolith_query.exe`; C4819 warnings only |
| Proxy build | `cmd /c Tools\MonolithProxy\build.bat` | Passed; copied `Binaries\monolith_proxy.exe`; C4819 warning only |
| Query smoke | `Binaries\monolith_query.exe source health` with enabled/disabled env variants | Passed; default-on log appended, disabled wrote no log, stdout/stderr matched |
| Proxy smoke | C++/Python/Node proxy `tools/call` against offline URL | Passed; each runtime wrote valid proxy JSONL, redacted token, disabled mode wrote no log |
| MonolithCore module build | UBT `-Module=MonolithCore -NoUBTMakefiles -ForceRulesCompile` | Passed; compiled `MonolithToolInvocationLogger.cpp` / tests and linked `UnrealEditor-MonolithCore.dll` |
| Action automation | `UnrealEditor-Cmd.exe ... -ExecCmds="Automation RunTests Monolith.Core.ToolInvocationLogger; Quit"` | Passed; 3 tests found and succeeded |
| Config opt-in | `Config\DefaultMonolith.ini` plus plugin config set `bEnableDailyLog=True` | Passed; headless editor appended action logs without Monolith command-line overrides |
| Headless action matrix | `RunHeadlessEditor.bat`, then MCP `tools/call` for success, lookup failure, and schema failure cases | Passed; 8 action records appended and parsed |
| Proxy-to-action headless path | Python proxy calls to `monolith_status` and `source_query` against headless editor | Passed; 3 proxy records and 3 action records appended |
| Final integrated append | Headless editor with config-enabled daily logging, proxy `monolith_status`, query `source health`, action calls | Passed; records appended to all three daily log files |

## 2.1 Format V2 Follow-Up

After the initial P0 implementation, the log schema was compacted to format v2:

- `trace_id` and `span_id` are required on new proxy/query/action records.
- Proxy calls forward `_monolith_trace_id` into editor JSON-RPC requests.
- Editor actions export `MONOLITH_TRACE_ID` while handlers run so child `monolith_query.exe` records inherit the action trace.
- `return_summary` provides compact analyzer-friendly result metadata.
- Empty optional fields and duplicate `agent_signal.retry_signature` / byte fields are omitted.

| Gate | Command | Result |
|---|---|---|
| Node proxy syntax | `node --check Scripts\monolith_proxy.js` | Passed |
| Python proxy/static syntax | `uv run python -m py_compile Scripts\monolith_proxy.py Scripts\ci_static_checks.py` | Passed |
| Static CI | `uv run python Scripts\ci_static_checks.py check` | Passed, 0 blocking findings |
| Query build | `cmd /c Tools\MonolithQuery\build.bat` | Passed; copied `Binaries\monolith_query.exe`; C4819 warnings only |
| Proxy build | `cmd /c Tools\MonolithProxy\build.bat` | Passed; copied `Binaries\monolith_proxy.exe`; C4819 warning only |
| Proxy/query v2 smoke | C++ proxy offline call, Python proxy offline call, Node proxy offline call, and `Binaries\monolith_query.exe source health` with `MONOLITH_TRACE_ID=trace-smoke-shared` under isolated `MONOLITH_TOOL_LOG_DIR` paths | Passed; proxy/query records had `format_version=2`, `trace_id`, `span_id`, `return_summary`, and no duplicate `agent_signal.retry_signature`; query inherited `trace-smoke-shared` |
| MonolithCore module compile | UBT `-Module=MonolithCore -NoUBTMakefiles -ForceRulesCompile` | Compile actions passed for `MonolithToolInvocationLogger.cpp`, `MonolithToolRegistry.cpp`, `MonolithHttpServer.cpp`, and tests; DLL link was blocked because a running `UnrealEditor.exe` held `Binaries\Win64\UnrealEditor-MonolithCore.dll` |

Action automation for format v2 should be rerun after the editor process using `UnrealEditor-MonolithCore.dll` is closed.

## 3. Automation Details

`Saved\Logs\GO.log` recorded:

| Test | Result |
|---|---|
| `Monolith.Core.ToolInvocationLogger.DailyLogOptInRedaction` | Success |
| `Monolith.Core.ToolInvocationLogger.PreDispatchFailureLogged` | Success |
| `Monolith.Core.ToolInvocationLogger.SourceChildQueryDualSurface` | Success |

## 4. Integrated Log Evidence

Final integrated verification appended records under `D:\P4\game\Plugins\Monolith\Logs`:

| File | Appended | Last call | Outcome |
|---|---:|---|---|
| `20260520/proxy.jsonl` | 3 | `source_query search_source` | `tool_error` for missing param |
| `20260520/query.jsonl` | 1 | `source.health` | `success` |
| `20260520/action.jsonl` | 12 | `monolith.discover` | `success` |

All appended records parsed as compact JSONL. The action matrix included:

| Call | Expected log outcome |
|---|---|
| `monolith_status` | `status=success`, `tool_name=monolith_status` |
| `monolith_discover` | `status=success`, `tool_name=monolith_discover` |
| `source_query health` | `status=success`, `namespace=source`, `action=health` |
| `source_query search_source` | `status=success`, `namespace=source`, `action=search_source` |
| `project_query search` | `status=success`, `namespace=project`, `action=search` |
| `editor_query get_build_errors` | `status=success`, `namespace=editor`, `action=get_build_errors` |
| Missing `source` action | `status=error`, `validation_phase=lookup`, `error_class=unknown_action` |
| Missing `source.search_source` query param | `status=error`, `validation_phase=schema`, `outcome=validation_rejected`, `error_class=missing_param` |

## 5. Project-Level Build Blocker

The primary Go build command was attempted:

```powershell
$engineRoot = powershell -NoProfile -ExecutionPolicy Bypass -File "D:\P4\game\BatchFiles\Script\ResolveUnrealEngine.ps1" -Project "D:\P4\game\GO.uproject" -Output Root
& "$engineRoot\Engine\Binaries\DotNET\UnrealBuildTool\UnrealBuildTool.exe" GoGameEditor Win64 Development -Project="D:\P4\game\GO.uproject" -WaitMutex -NoHotReloadFromIDE
```

Result: failed before completion on an unrelated game-module link error:

```text
Module.GoGame.gen.3.cpp.obj : error LNK2001: unresolved external symbol AGoPlayerController::CreateVirtualJoystick()
D:\P4\game\Binaries\Win64\UnrealEditor-GoGame.dll : fatal error LNK1120
```

This blocker is outside the Monolith daily-log implementation. Monolith-specific automation and native proxy/query checks passed.
