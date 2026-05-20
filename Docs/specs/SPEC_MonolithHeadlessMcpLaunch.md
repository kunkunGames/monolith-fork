# Monolith — Headless MCP Launch

**Parent:** [SPEC_CORE.md](../SPEC_CORE.md)
**Engine:** Unreal Engine 5.7+
**Version:** Planned
**Status:** Draft specification, not implemented
**Created:** 2026-05-20

---

## 1. Goal

Allow agents to start an Unreal Editor process without an interactive desktop workflow, wait until Monolith's embedded MCP HTTP server is ready, and then work through the normal Monolith MCP tool surface.

The feature is for automation and agent workstations where the editor is not already running. It must reuse the existing Monolith action registry, MCP HTTP server, and stdio proxy path instead of introducing a separate commandlet-only MCP server. The editor launch entrypoint should be a project-owned batch file so editor startup remains mostly independent from Monolith-specific command-line arguments while MCP client configuration stays on the existing proxy.

---

## 2. Non-Goals

| Non-goal | Reason |
|----------|--------|
| Running Monolith MCP inside commandlets | `FMonolithCoreModule` intentionally skips MCP server startup in commandlets to avoid bind conflicts during UBT/cook/compile paths. Headless MCP must run a full editor process with editor modules loaded. |
| Passing Monolith-specific editor launch arguments | Monolith MCP server enablement and port come from normal config. The launch wrapper should not need `-MonolithHeadlessMcp`, `-MonolithMcpPort`, or similar editor args. |
| Exposing MCP beyond loopback | The server remains localhost-only by default. Remote access, auth, and tunneling are outside this slice. |
| Guaranteeing viewport-dependent actions under `-NullRHI` | Visual viewport capture, Slate interaction, and in-viewport PIE may be unavailable in headless/null-RHI sessions and must report explicit capability states. |
| Replacing offline `monolith_query.exe` | Offline DB query remains the no-editor fallback for source/project index reads. Headless MCP is for editor-only capabilities and asset mutation workflows. |
| Using editor Python as the control plane | The launcher and readiness path must be native process/HTTP/MCP plumbing. `editor.run_python` remains a diagnostic escape hatch only. |

---

## 3. Terminology

| Term | Meaning |
|------|---------|
| Headless editor | `UnrealEditor.exe <Project>.uproject` launched without an interactive UI workflow, using `-Unattended`, `-NullRHI`, `-NoSplash`, `-NoSound`, and `-Log` by default. It is not a commandlet. |
| Batch wrapper | Project-owned batch file that resolves the engine and starts the editor. For Go: `BatchFiles\RunHeadlessEditor.bat`. |
| Proxy | Existing stdio-to-HTTP MCP bridge. MCP client configuration remains pointed at this proxy, which forwards to the editor-side HTTP server. The launcher prefers Python 3.8+ and falls back to Node.js when local Python/py launcher entries are unavailable or broken. |
| Instance record | Optional bounded JSON file under `Saved/HeadlessMcp/` describing a managed editor process (`pid`, project, log path, state). It must not contain secrets. |
| Capability profile | Runtime report of which action families are expected to work in the current editor mode (`interactive`, `headless`, `headless_nullrhi`, `commandlet`). |

---

## 4. User Workflows

### 4.1 Agent Starts With No Editor Running

1. Agent launches `BatchFiles\RunHeadlessEditor.bat` when no suitable editor is already running.
2. Batch resolves the editor from `GO.uproject`.
3. Batch starts the full editor with rendering disabled by `-NullRHI`, leaves P4 enabled, and exits.
4. MCP client keeps using the existing Monolith stdio proxy configuration.
5. Proxy retries until the editor-side Monolith MCP server is reachable.
6. Once ready, the MCP client sees the normal Monolith tools and can call `monolith_find`, `monolith_discover`, and `{namespace}_query`.

### 4.2 Agent Reuses Existing Headless Editor

1. Batch starts the editor unconditionally unless a future process-reuse check is added.
2. Proxy attaches to the configured endpoint when it becomes healthy.
3. If a separate reuse guard is implemented later, it must verify project identity before skipping launch.

### 4.3 Agent Starts With Interactive Editor Already Running

1. Batch may start another editor unless the operator avoids doing so.
2. Proxy attaches to whichever editor-side Monolith server owns the configured URL.
3. Future process-reuse work should detect a healthy matching project before spawning.

### 4.4 Agent Finishes Work

1. Default behavior is to leave the editor running for reuse.
2. Managed shutdown is not part of the first batch wrapper. Agents should stop the editor explicitly only when the user asks.
3. Crash or timeout leaves log and instance metadata for inspection.

---

## 5. Launch Contract

### 5.1 External Entrypoint

Primary editor launch helper:

```powershell
D:\P4\game\BatchFiles\RunHeadlessEditor.bat
```

MCP config remains on the existing Monolith stdio proxy command. Do not point MCP config at `RunHeadlessEditor.bat`, and do not require MCP config files to pass separate `--auto-launch-editor`, `--headless`, `--project`, `-MonolithHeadlessMcp`, or `-MonolithMcpPort` controls.

Optional flags stay separate only when they describe independent behavior:

| Control | Description |
|---------|-------------|
| `UE_PROJECT` | Optional environment override for the `.uproject` path. |
| `UE_EDITOR_ARGS` | Optional environment replacement for base editor args. Use this to intentionally run with rendering/viewport support. |
| `UE_EDITOR_EXTRA_ARGS` or batch command args | Optional generic editor args appended after the render-disabled defaults. |
| `MONOLITH_URL` | Optional proxy URL override for the existing MCP proxy, not for the editor launch batch. |

Batch diagnostics go to stderr only. The batch starts the editor and exits; the foreground MCP process remains the existing stdio proxy.

### 5.2 Editor Command Line

Baseline Windows command shape:

```powershell
UnrealEditor.exe D:\P4\game\GO.uproject -Unattended -NullRHI -NoSplash -NoSound -Log -AbsLog=D:\P4\game\Saved\HeadlessMcp\Logs\HeadlessEditor-<timestamp>.log
```

Rules:

- Do not use `-run=...`, commandlets, cook, compile, `-server`, or game-only modes for MCP serving.
- Do not add `-NoP4`; Go project headless MCP workflows need P4/source control.
- Do not add Monolith-specific editor args. `UMonolithSettings` / config decides whether the MCP server starts and which port it uses.
- `-AbsLog=<path>` is required for managed launch so failures can be diagnosed without an editor UI.
- The launcher must resolve the engine executable from the `.uproject` `EngineAssociation` when possible and must not hard-code a local engine path.

### 5.3 Port Selection

| Input | Behavior |
|-------|----------|
| Explicit `MONOLITH_URL` | Proxy uses it unchanged. |
| No `MONOLITH_URL` | Existing proxy default/config behavior applies. The editor launch batch does not derive or override proxy URLs. |
| No readable config | Proxy default applies, currently `http://localhost:9316/mcp`. |
| Port busy | Editor-side Monolith server reports bind failure in log; batch does not override config. |

---

## 6. Runtime Contract

### 6.1 Readiness

The launcher reports ready only after all conditions pass:

| Check | Required result |
|-------|-----------------|
| Process state | Editor process is alive. |
| HTTP endpoint | `POST /mcp` responds to `initialize` or `tools/list`. |
| Monolith status | `monolith.status` returns `server_running=true`, expected project name/path, and non-zero action catalog counts. |
| Registry floor | At least `monolith`, `editor`, `project`, and `source` namespaces are discoverable unless disabled by build/settings. |
| Capability report | Runtime mode reports unattended/headless-relevant state when implemented. |

Timeouts:

- Default startup timeout: 180 seconds.
- Default readiness poll interval: 1000 ms with bounded backoff.
- Timeout result includes the editor command line with sensitive values redacted, log path, process id if available, and the last readiness error.

### 6.2 Capability Reporting

Add or extend a `monolith` namespace diagnostic action:

| Action | Status | Description |
|--------|--------|-------------|
| `monolith.get_runtime_environment` | planned | Report editor mode, commandlet flag, unattended/headless-relevant flags, RHI/null-RHI state, display availability, process id, project path, server port, and capability groups. |

Example response:

```json
{
  "status": "ok",
  "mode": "headless_nullrhi",
  "is_commandlet": false,
  "unattended": true,
  "project_path": "D:/P4/game/GO.uproject",
  "server_port": 9316,
  "capabilities": {
    "source_index": "available",
    "project_index": "available",
    "asset_mutation": "available",
    "viewport_capture": "unavailable_nullrhi",
    "slate_inspection": "limited",
    "pie_in_viewport": "unavailable_headless"
  },
  "next_actions": [
    "monolith.find",
    "monolith.discover",
    "editor.get_recent_logs"
  ]
}
```

Viewport, Slate, PIE, and screenshot actions must either work normally or return explicit `unavailable_*` errors. They must not silently return empty screenshots or pretend a viewport exists.

### 6.3 Lifecycle Actions

Optional lifecycle diagnostics may be added later, but the first batch wrapper does not require editor-side shutdown control:

| Action | Mutability | Description |
|--------|------------|-------------|
| `get_headless_instances` | read-only | List bounded local instance records for this project/user. |
| `get_headless_logs` | read-only | Return tail excerpts from the managed headless editor log. Bounded text only. |
| `request_headless_shutdown` | mutating | Deferred. Requires a reliable project-owned process marker before it can safely terminate anything. |

`request_headless_shutdown` must reject interactive editors and unmanaged editor processes if implemented.

---

## 7. Instance Record

Location:

```text
Saved/HeadlessMcp/instances/<project-hash>.json
```

Required fields:

| Field | Type | Notes |
|-------|------|-------|
| `schema_version` | integer | Start at `1`. |
| `project_path` | string | Normalized absolute path. |
| `engine_exe` | string | Normalized absolute path. |
| `pid` | integer | OS process id. |
| `started_at` | string | ISO-8601 UTC timestamp. |
| `log_path` | string | Managed log path. |
| `launch_args` | array | Redacted args; no env dump. |
| `state` | string | `starting`, `ready`, `stale`, `failed`, `exited`. |

Forbidden:

- API keys, bearer tokens, cookies, auth headers, private keys, passwords.
- Full environment-variable dumps.
- Raw MCP request or result payloads.

---

## 8. Security And Safety

- Bind only to localhost / loopback by default.
- Browser CORS remains controlled by `monolith.set_mcp_compatibility_options`; headless mode must not widen it.
- The launcher must not auto-open arbitrary `.uproject` paths from untrusted MCP request payloads. Project path comes from local batch/env configuration.
- Reuse detection must verify project identity before skipping editor launch if implemented.
- Managed shutdown must remain out of scope until there is a reliable process marker; it must not close an interactive editor by accident.
- Logs and instance records must redact known secret-like CLI args and environment references.
- Asset mutation permissions and audit behavior remain governed by existing Monolith action execution policy metadata.

---

## 9. Failure Contract

| Failure | Required behavior |
|---------|-------------------|
| Engine executable cannot be resolved | Fail before spawn with the project path and resolution strategy. |
| Port is occupied by another process | Fail unless it is a healthy Monolith server for the same project. |
| Editor exits before readiness | Return process exit code, log path, and last log excerpt if available. |
| MCP server disabled in settings | Proxy stays alive but Monolith actions fail until settings are corrected and the editor is restarted. |
| Commandlet accidentally launched | Report unsupported mode; never claim MCP is available. |
| `-NullRHI` breaks a visual action | Return explicit `unavailable_nullrhi` from that action or from capability report. |
| Readiness timeout | Leave process running by default for inspection unless `--kill-on-timeout` is explicit. |

---

## 10. Implementation Plan

### P0 — Spec And Routing Docs

- Add this spec.
- Add TODO plan entry.
- Link the spec from `SPEC_CORE.md` and `SPEC_MonolithCore.md`.

### P1 — Project Batch Wrapper

- Add `BatchFiles\RunHeadlessEditor.bat`.
- Resolve engine from `.uproject` `EngineAssociation`.
- Spawn full editor process with managed log path and render-disabled headless flags.
- Keep P4 enabled by omitting `-NoP4`.
- Do not pass Monolith-specific editor args.
- Leave MCP stdio proxy ownership to the existing MCP client configuration.

### P2 — Runtime Environment Diagnostics

- Add `monolith.get_runtime_environment`.
- Detect commandlet state, unattended/headless-relevant flags, RHI/null-RHI state, process id, and project path.
- Include capability profile in `monolith.status` or readiness output without breaking existing response fields.

### P3 — Lifecycle Helpers

- Add read-only instance/log diagnostics.
- Keep shutdown out of the first batch wrapper.
- Add guarded shutdown only after a reliable process marker exists.

### P4 — Capability-Safe Action Behavior

- Audit viewport, Slate, PIE, capture, and screenshot actions.
- Replace headless/null-RHI silent failures with explicit `unavailable_*` errors and next actions.
- Keep source/project/asset actions unchanged unless they assume a visible viewport.

### P5 — Tests And Verification

- Unit-test launch arg construction, config port parsing, optional instance record stale detection, and secret redaction.
- Automation-test `monolith.get_runtime_environment` shape.
- Headless smoke-test on Windows with `-Unattended -NullRHI -NoSplash -NoSound -Log`.
- Validate MCP round-trip: `initialize`, `tools/list`, `monolith.status`, `monolith.find`, `monolith.discover`, `editor.get_recent_logs`.

---

## 11. Validation Matrix

| Scenario | Command / check | Expected |
|----------|-----------------|----------|
| No editor running | `BatchFiles\RunHeadlessEditor.bat`, then existing MCP proxy | Editor starts, existing proxy attaches, MCP tools become available. |
| Existing matching editor | Start batch while editor already runs | First implementation may start another editor; future reuse guard should avoid duplicates. |
| Existing wrong project | Start batch against occupied URL | Proxy follows configured URL; future reuse guard should report mismatch. |
| NullRHI mode | Run the batch with defaults | Source/project/editor log actions work; viewport actions report unavailable. |
| MCP disabled | Disable `bMcpServerEnabled` | Readiness fails with direct settings diagnostic. |
| Crash before ready | Force invalid project/plugin load | Returns exit/log diagnostics. |
| P4 enabled | Run batch and inspect editor source-control state | P4 remains available because `-NoP4` is not passed. |

Required local verification before implementation is marked complete:

```powershell
$engineRoot = powershell -NoProfile -ExecutionPolicy Bypass -File "D:\P4\game\BatchFiles\Script\ResolveUnrealEngine.ps1" -Project "D:\P4\game\GO.uproject" -Output Root
& "$engineRoot\Engine\Binaries\DotNET\UnrealBuildTool\UnrealBuildTool.exe" GoGameEditor Win64 Development -Project="D:\P4\game\GO.uproject" -WaitMutex -NoHotReloadFromIDE
```

Plus a real headless MCP smoke run through `D:\P4\game\BatchFiles\RunHeadlessEditor.bat`.

---

## 12. Open Questions

| Question | Default for implementation |
|----------|----------------------------|
| Should `-NullRHI` be default? | Yes for the agent launch batch. Agents should get a non-rendering editor by default; visual workflows can override `UE_EDITOR_ARGS` explicitly. |
| Should port auto-selection be default? | No. Use configured/default port and fail on conflicts; allow `--port=auto` only when the client can consume the selected URL. |
| Should the editor close when MCP client disconnects? | No. Leave running by default; add a guarded process-marker design before implementing shutdown. |
| Should the feature support remote agents? | No. Loopback-only until an explicit authentication and transport spec exists. |
| Should commandlet mode be supported later? | Only for a separate read-only/offline action subset. It must not be called the normal Monolith editor MCP surface. |
