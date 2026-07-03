# Source Call Graph Query Alias Verification

| Field | Value |
| --- | --- |
| Date | 2026-07-04 |
| Project | `D:\P4\speed\Speed.uproject` |
| Scope | Source and offline `source.find_callers` / `source.find_callees` input compatibility for `query` as an alias of `symbol` |

---

## 1. Results

| Check | Result |
| --- | --- |
| `cmd /c Plugins\Monolith\Tools\MonolithQuery\build.bat` | Passed. Built source hash `7252269ec596c3a2` and copied `monolith_query.exe` to `Plugins\Monolith\Binaries`. |
| `Plugins\Monolith\Binaries\monolith_query.exe --catalog-self-check --no-log` | Passed, `success=true` and `errors={}`. |
| `python Plugins\Monolith\Scripts\check_offline_exe_fresh.py` | Passed, executable `source_hash` matched current MonolithQuery source inputs. |
| `Plugins\Monolith\Binaries\monolith_query.exe source find_callers --query UObject::GetName --limit=1 --no-log` | Passed, exit 0, accepted `--query` and resolved qualified `Class::Method` through trailing-method lookup. |
| `Plugins\Monolith\Binaries\monolith_query.exe source find_callees --q UObject::GetName --limit=1 --no-log` | Passed, exit 0, accepted `--q` and resolved qualified `Class::Method` through trailing-method lookup. |
| `Plugins\Monolith\Binaries\monolith_query.exe source find_callers UObject::GetName --limit=1 --no-log` | Passed, exit 0, preserved canonical positional behavior. |
| `Plugins\Monolith\Binaries\monolith_query.exe source find_callees UObject::GetName --limit=1 --no-log` | Passed, exit 0, preserved canonical positional behavior. |
| `Plugins\Monolith\Binaries\monolith_query.exe source find_callers --limit=1 --no-log` | Passed as negative case, exit 1, error says `find_callers requires a symbol argument (positional or --query=<text>)`. |
| `Plugins\Monolith\Binaries\monolith_query.exe source find_callees --limit=1 --no-log` | Passed as negative case, exit 1, error says `find_callees requires a symbol argument (positional or --query=<text>)`. |
| `source find_callers --help` / `source find_callees --help` | Passed, both help pages document `--query TEXT`, `--q TEXT`, and qualified symbol support. |
| `Monolith.Registry.Source.CallGraphQueryAlias` | Added automation coverage: registered schemas for both actions rewrite `query` to canonical `symbol` through `FMonolithParamSchema::ApplyAliases`. |
| Primary `SpeedEditor Win64 Development` UBT build | Passed after the editor exited. Earlier attempt was blocked at link because running `UnrealEditor.exe` PID `45652` held `Plugins\Monolith\Binaries\Win64\UnrealEditor-MonolithSource.dll`; no C++ compile diagnostic preceded the lock. |
| `editor.get_build_status` via `http://localhost:9316/mcp` | Passed, MCP was up on PID `45652`, Live Coding was available, and no compile was active. |
| `editor.trigger_build(wait=true)` / `editor.get_compile_output` | Passed as a running-editor check: result `no_changes`, `error_count=0`, `warning_count=0`. The current editor process did not reload the modified registration schema. |
| Live `source_query` with `query` params after UBT + MCP recovery | Passed. `source.find_callers` with `query=UObject::GetName` and `source.find_callees` with `query=UObject::GetName` both returned results without `missing_required_param=symbol`. |
| `editor.run_automation_tests(prefix="Monolith.Registry.Source.CallGraphQueryAlias")` | Passed, 1/1 test succeeded with 0 errors and 0 warnings. |

## 2. Notes

The ROI trigger was `Saved\Monolith\LogAnalysis\roi-20260703`, where `action:source.find_callers` and `action:source.find_callees` repeatedly failed with `missing=[symbol] provided=[query]`. The fix is additive and keeps canonical `symbol` unchanged while accepting the natural `query` key agents were already using.

The first running editor predated the schema change. Live Coding reported no source changes because the earlier UBT attempt had already compiled the changed source files before the linker hit the locked DLL. After the editor exited, full UBT linked the new module binary and `recover_mcp.ps1` restarted MCP with the updated registry.
