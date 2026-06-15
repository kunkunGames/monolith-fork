# Proxy Transient-Retry + Schema Routing Tolerance

**Date:** 2026-06-14
**Spec:** [../specs/SPEC_MonolithToolCallReliabilityBacklog.md](../specs/SPEC_MonolithToolCallReliabilityBacklog.md) §5.3, §5.5, §6C
**Changelists:** 722 (proxy retry), 723 (schema/routing C++)

Two backlog items implemented and verified: the MCP-availability partial mitigation (proxy retry) and two agent input/routing tolerances (logicdriver discover, read_source path reroute).

---

## 1. Proxy transient-connection retry (CL 722, availability §6C)

**Change.** `Scripts/monolith_proxy.py` and `Scripts/monolith_proxy.js` retry the forward to the editor MCP endpoint on **send-side connection failures only** (`ECONNREFUSED`/`ECONNRESET`/etc. — the request never reached the server, so a retry cannot double-execute a mutation). Three safeguards keep it correct and prompt:

1. **Read timeouts are never retried** (the request may already be applied — mutation safety).
2. **Scoped to `tools/call`** (real action forwarding). `tools/list` and unknown-method forwards stay fast-fail because they have a cached/seed fallback; retrying there would only delay the offline path.
3. **Total wall-clock budget** (`MONOLITH_CONNECT_RETRY_BUDGET`, default 1.5 s) on top of the attempt cap (`MONOLITH_CONNECT_RETRIES`, default 3; 0.25 s × attempt backoff). Real editor flicker refuses in ~ms so several retries fit the budget and self-heal; a fully-down/blackholed endpoint refuses slowly so the budget bails after ~1 attempt, keeping the graceful-error path prompt.

Rationale: the §6C measurement showed the `9316` endpoint *flickers* (8/8 failing sessions interleave success and failure; 0 sustained-down), so a bounded retry self-heals most transient drops.

**Scope limit.** Helps proxy-routed clients (Claude Code via `.mcp.json`). Codex uses its own rmcp streamable-HTTP client directly to `9316` and bypasses the proxy, so the measured Codex failures need the editor-side root cause (spec §6C "Open work").

**Verification — Python (`python` unit harness, monkeypatched `urlopen`):**

| Test | Result |
|---|---|
| `py_compile` | OK |
| `tools/call` (retry on): transient `ECONNREFUSED` ×2 then success → retried, returns body | PASS |
| `tools/list` (retry off, default): `ECONNREFUSED` → fast-fail, 1 attempt | PASS |
| Read timeout → NOT retried (1 attempt, returns None) | PASS |
| Sustained refusal → bounded by attempt cap + 1.5 s budget → None | PASS |
| Classifier (`_is_retryable_connection_error` / `_is_timeout_error`) | PASS |

**Verification — JS (`node` mock `http.request`):** `node --check` OK; transient `ECONNREFUSED` ×2 then success → retried; read timeout → not retried.

**Regression gate.** `python Scripts/ci_static_checks.py --config .github/monolith-static-ci.json --github check` → **exit 0, 0 blocking findings** (the `proxy-smoke` offline `tools/list`+`tools/call` check passes; an interim count-based retry tripped its 5 s budget against the `127.0.0.1:9` blackhole offline URL, which the wall-clock budget fixed).

## 2. logicdriver discover entry (CL 723, §5.3)

**Change.** Added a `logicdriver` entry to `GetKnownOptionalModules()` (`MonolithCore/Private/MonolithCoreTools.cpp`): `{logicdriver, bEnableLogicDriver, logicdriver_query, "…Requires Logic Driver Pro plugin (Fab marketplace)."}`. Now `monolith.discover {namespace:"logicdriver"}` returns `Success{status:disabled|not_installed, hint}` via the existing graceful branch instead of the bare `-32602 "Unknown namespace: logicdriver"` (11 such errors on 06-13). Fixes the inconsistency where `bulk_fill`/`describe` already advertised `logicdriver` but `discover` did not.

## 3. read_source path reroute (CL 723, §5.5)

**Change.** In `HandleReadSource` (`MonolithSource/Private/MonolithSourceActions.cpp`), before the empty-`symbol` error: if `symbol` is absent/empty but `file_path` or `path` is present, build file params (carrying `start_line`/`end_line`/`line_count`/`max_lines`) and delegate to `HandleReadFile`. Mirrors the existing value-based reroute (`LooksLikeSourceFilePath`), which only fired when a path was passed *as* the symbol. Closes the routing gap where agents call `read_source` with a file path under the `file_path`/`path` key (167 lifetime errors).

## 4. Build verification (CL 723 C++)

```powershell
& "D:\Engine\UE_5.7\Engine\Binaries\DotNET\UnrealBuildTool\UnrealBuildTool.exe" `
  GoGameEditor Win64 Development "-Project=D:\P4\game\GO.uproject" -WaitMutex -NoHotReloadFromIDE
```

`Result: Succeeded` (exit 0). Artifacts rebuilt after the source edits (edits 23:19; objects 23:20:59–23:21:00; DLLs relinked 23:21:00):

- `Plugins/Monolith/Intermediate/Build/Win64/x64/UnrealEditor/Development/MonolithCore/MonolithCoreTools.cpp.obj`
- `…/MonolithSource/MonolithSourceActions.cpp.obj`
- `Plugins/Monolith/Binaries/Win64/UnrealEditor-MonolithCore.dll`, `UnrealEditor-MonolithSource.dll`

No live editor was running (port `9316` not listening), so no DLL-lock conflict.

## 5. Follow-ups

- Runtime PIE/editor confirmation of `discover {logicdriver}` and `read_source {file_path}` deferred (no live editor this session); the build proves compile/link, and both follow existing verified code paths.
- `source_control` string→bool/array coercion (§5.5) intentionally deferred (no recurrence in window; touches shared strict validation).
- Native C++ `monolith_proxy.exe` did not receive the retry (Python/JS only this slice).
