# Tool Invocation Daily Logs Verification

Date: 2026-05-20, updated 2026-05-21
Module: MonolithCore, MonolithProxy, MonolithQuery
Result: P0 verified; format v3 proxy/query/action data collection verified; analyzer implementation deferred

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

The format v2 link blocker is superseded by the format v3 verification below.

## 2.2 Format V3 Follow-Up

Format v3 adds the analysis fields needed to reconstruct agent tool timelines and bottlenecks:

- `record_id`, `process_instance_id`, `previous_record_id`, `time_since_previous_ms`, `session_key`, and `parent_span_id`.
- Proxy-to-action `_monolith_parent_span_id` transport and action-to-query `MONOLITH_PARENT_SPAN_ID`.
- `routing_context`, `workflow`, `phase_timing`, `environment`, and `return_summary.result_shape`.
- Action-side `child_process` summaries for synchronous `monolith_query.exe` calls.

| Gate | Command | Result |
|---|---|---|
| Syntax checks | `node --check Scripts\monolith_proxy.js`; `uv run --python 3.11 python -m py_compile Scripts\monolith_proxy.py Scripts\ci_static_checks.py` | Passed |
| Static CI | `uv run --python 3.11 python Scripts\ci_static_checks.py check` | Passed, 0 blocking findings |
| Native query/proxy builds | `Tools\MonolithQuery\build.bat`; `Tools\MonolithProxy\build.bat` | Passed; copied `Binaries\monolith_query.exe` and `Binaries\monolith_proxy.exe`; C4819 warnings only |
| GoGameEditor build | Primary UBT build command from `D:\P4\game\AGENTS.md` | Passed after P4 checkout of read-only plugin `UnrealEditor.modules` metadata files; MonolithCore and MonolithSource compiled/linked |
| Query v3 default-on/disable/output preservation | `Binaries\monolith_query.exe source search_source UObject --limit=1` with isolated log roots | Passed; unset `MONOLITH_TOOL_LOG_ENABLED` wrote v3 `query.jsonl`, disabled mode wrote no log, stdout/stderr matched |
| Query parent span and intent | `Binaries\monolith_query.exe source health` with `MONOLITH_TRACE_ID` and `MONOLITH_PARENT_SPAN_ID` | Passed; v3 query row inherited `parent_span_id`, emitted `namespace_source=child_process`, and classified health as `verification` |
| Query max field override | `Binaries\monolith_query.exe source read_file <large.txt> --source_db=<temp EngineSource.db>` with `MONOLITH_TOOL_LOG_MAX_FIELD_BYTES=2048` | Passed; v3 query row truncated stdout, kept preview within override bound, preserved `result_shape=text`, and kept stdout/stderr process output behavior |
| Proxy v3 default-on/parity | Python, Node, and C++ proxy offline `source_query health` calls under one isolated log root | Passed; all three appended v3 proxy rows with `routing_context`, `workflow`, `phase_timing`, `editor_unavailable`, and `inferred_intent=verification` |
| Proxy max field override | Python, Node, and C++ proxy offline `source_query search_source` calls with 6 KiB argument payload and `MONOLITH_TOOL_LOG_MAX_FIELD_BYTES=2048` | Passed; all three runtimes appended v3 proxy rows with bounded argument previews at or below 2048 characters and `redaction.truncated=true` |
| Cross-runtime lock protocol | Same proxy smoke wrote Python, Node, and C++ records sequentially to one `proxy.jsonl` | Passed after aligning all runtimes on exclusive `.lock` file create/delete with stale cleanup |
| Proxy disabled mode | Python, Node, and C++ proxy offline calls with `MONOLITH_TOOL_LOG_ENABLED=0` | Passed; no `proxy.jsonl` was created |
| Headless action trace continuity | Python proxy `source_query(crg_graph_health)` against `RunHeadlessEditor.bat` editor | Passed; proxy/action/query rows shared `trace_id`; action `parent_span_id` matched proxy `span_id`; query `parent_span_id` matched action `span_id`; action logged `child_process.exec_process_ms`; action environment reported `headless=true` |
| Action automation and max field override | `RunHeadlessEditor.bat`, then MCP `editor_query run_automation_tests` for `Monolith.Core.ToolInvocationLogger` with isolated `MONOLITH_TOOL_LOG_DIR` and `MONOLITH_TOOL_LOG_MAX_FIELD_BYTES=4096` | Passed; 3/3 tests succeeded, `action.jsonl` contained v3 `editor.run_automation_tests` row, and `DailyLogOptInRedaction` verified action preview bounds |

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

## 5. Project-Level Build

The primary Go build command was attempted:

```powershell
$engineRoot = powershell -NoProfile -ExecutionPolicy Bypass -File "D:\P4\game\BatchFiles\Script\ResolveUnrealEngine.ps1" -Project "D:\P4\game\GO.uproject" -Output Root
& "$engineRoot\Engine\Binaries\DotNET\UnrealBuildTool\UnrealBuildTool.exe" GoGameEditor Win64 Development -Project="D:\P4\game\GO.uproject" -WaitMutex -NoHotReloadFromIDE
```

Original 2026-05-20 result: failed before completion on an unrelated game-module link error:

```text
Module.GoGame.gen.3.cpp.obj : error LNK2001: unresolved external symbol AGoPlayerController::CreateVirtualJoystick()
D:\P4\game\Binaries\Win64\UnrealEditor-GoGame.dll : fatal error LNK1120
```

2026-05-21 result: passed. The previous `AGoPlayerController::CreateVirtualJoystick()` blocker is resolved, and the final UBT run completed after P4 checkout of plugin metadata files that UBT needed to rewrite.
