# Monolith — Headless MCP Control Plane and Editor Workers

**Parent:** [SPEC_CORE.md](../SPEC_CORE.md)
**Engine:** Unreal Engine 5.8
**Version:** 2.0
**Status:** Phase A implementation — native offline control plane
**Created:** 2026-05-20
**Revised:** 2026-07-11

---

## 1. Decision

The long-lived MCP process must not be an Unreal Editor process.

Monolith uses the existing native binaries as the stable control plane:

```text
MCP client
    │ newline-delimited JSON-RPC over stdio
    ▼
monolith_proxy-<source-hash>.exe       long-lived immutable control plane, no UE DLLs
    ├─ live route ───────────────────► Unreal Editor HTTP /mcp
    │                                  editor/UObject/world/asset actions
    │
    └─ offline read route ───────────► monolith_query-<source-hash>.exe --readonly
                                       + monolith_catalog-<semantic-hash>.json
                                       source/project/bridge/RI/catalog DB reads
```

Phase A keeps the existing live-editor route and adds a read-only offline route.
When the editor is stopped, restarting, or temporarily unavailable during a
build, the MCP client remains connected to its immutable proxy image and supported
indexed queries continue to work.

Phase A does **not** claim that all editor actions work without an editor. It
also does not claim that the current installed UE build can produce a fully
isolated editor worker. Those capabilities have separate gates in this spec.

---

## 2. Why the Control Plane Is Native

The previous design made a full `UnrealEditor.exe` process both the action
worker and the MCP server owner. On this checkout that process loads all 46
Monolith module DLLs plus Speed, Lyra, CommonGame, CommonUser, and SpeedCore
module DLLs. As a result, ordinary UBT and Live Coding links compete with the
long-lived MCP process for the same files.

The native control plane avoids that coupling:

- `monolith_proxy-<source-hash>.exe` is an `/MT` standalone executable and loads no Unreal
  Engine, project, or Monolith plugin DLLs.
- `monolith_query-<source-hash>.exe` is an `/MT` standalone SQLite reader and loads no Unreal
  Engine, project, or Monolith plugin DLLs.
- MCP stdio session lifetime is independent from editor lifetime.
- Indexed read work remains available while editor-backed work is unavailable.
- A future editor worker can be replaced without changing MCP client config.

The proxy implementation remains single-source, but deployment is
source-addressed with full artifact SHA-256 verification.
`monolith_proxy.current.json` selects one immutable
`monolith_proxy-<source-hash>.exe`; onboarding records that exact path. A running
old session can retain its Windows image lock while a new build is staged for
new sessions. The historical fixed `monolith_proxy.exe` is compatibility-only
and is not the release/onboarding authority.

Query and Proxy generation ids use the shared `Scripts/source_generation_hash.py`
contract: build-contract bytes plus ordered text inputs, with CRLF and lone CR
canonicalized to LF. The normalization prevents clean Git and P4 workspaces from
minting different generation names for the same text. It never changes artifact
verification: executable, catalog, and manifest SHA-256 values are calculated over
their exact raw bytes, and publication fails on a same-generation byte collision.

At proxy startup, `monolith_query.current.json` is parsed once with an exact
schema. The proxy pins the selected immutable Query and catalog files, verifies
their SHA-256 and semantic/source identities, and keeps those handles for the
session lifetime. Publishing a newer pair therefore does not change the
semantics of an already-running proxy. The fixed `monolith_query.exe` remains a
compatibility alias for direct CLI users, not the proxy's production authority.

---

## 3. Rejected Unreal Target Shortcuts

These conclusions are based on UE 5.8 UBT source and an actual build attempt,
not target-name assumptions.

### 3.1 Another Modular Editor Target

An installed-engine `TargetType.Editor` defaults to a modular, shared build
environment. In that environment UBT uses `AppName=UnrealEditor`, so a custom
target or custom launcher still consumes binaries such as:

```text
UnrealEditor-MonolithCore.dll
UnrealEditor-MonolithSource.dll
UnrealEditor-Speed.dll
UnrealEditor-LyraGame.dll
```

Changing only `LaunchModuleName`, `OutputFile`, target name, or executable
subfolder does not isolate ordinary module DLLs. LiveLinkHub is a concrete UE
example: it has a separate executable but still consumes `UnrealEditor-*.dll`.

### 3.2 Unique Modular Editor Target on the Installed Engine

`BuildEnvironment = TargetBuildEnvironment.Unique` would give a custom editor
target its own target-prefixed DLL and intermediate set. UBT explicitly rejects
a unique build environment when `Engine/Build/InstalledBuild.txt` exists, which
is the configuration of `D:\Engine\UE_5.8`.

`bOverrideBuildEnvironment=true` does not remove the installed-engine
read-only/precompiled restriction.

### 3.3 Monolithic Editor Target on the Installed Engine

UBT JSON export accepted a monolithic `SpeedEditor` graph with one executable,
1,219 linked modules, and all 46 Monolith modules embedded. That graph would
have isolated the link outputs.

The actual build was then executed:

```powershell
D:\Engine\UE_5.8\Engine\Build\BatchFiles\Build.bat `
  SpeedMonolithMcpEditor Win64 Development `
  -Project=D:\P4\speed\Speed.uproject `
  -WaitMutex -NoHotReloadFromIDE
```

It failed before linking because the installed build does not contain the
required UnrealEditor monolithic `.precompiled` manifests for `InputCore`,
`Core`, `Engine`, `UnrealEd`, and many dependent modules. The experimental
target and its target-specific intermediates were removed after the test.

### 3.4 Program Target

`TargetType.Program` is not an editor substitute. Forty-five of Monolith's 46
plugin modules are `Editor` modules, and the action surface depends on
`GEditor`, `UnrealEd`, editor subsystems, AssetTools, Slate, live worlds, and
editor asset lifecycle. Reclassifying those modules as Runtime/Program would
pollute game targets and still would not reproduce editor initialization.

---

## 4. Phase A Routing Contract

### 4.1 Live Route Has Priority

For each `tools/call`, the native proxy sends the request unchanged to a backend
already observed healthy. On a cold connection, fixed read-only fallback calls
do not wait for an unknown/booting backend. Live-only calls run a bounded health
preflight and retain the normal live action timeout.

- A 2xx HTTP response is live only when its body is a valid JSON-RPC 2.0 object,
  its id matches the request, and it contains exactly one of `result` or
  object-valued `error`. A valid tool-level or JSON-RPC error is returned
  unchanged.
- Offline fallback must not hide a live action error, validation error, or
  policy rejection.
- Offline fallback is considered when the HTTP transport returns no response,
  a non-2xx response, malformed/non-JSON content, or a mismatched JSON-RPC
  envelope. Invalid HTML/proxy bodies must never be written to MCP stdio.
- The first cold `tools/list` exposes exactly the four stable control-plane
  tools; stale cached live catalogs are never replayed. A first successful
  health observation emits `tools/list_changed`; later live catalog refreshes
  have a 750-millisecond total wall-clock deadline. Missing that metadata budget
  falls back to the same four tools without opening the live-action transport
  circuit.
- Health transitions are owned by the background poller. Neither
  `notifications/initialized` nor cold `tools/list` performs a synchronous
  health call on the stdio dispatch thread.
- Once the backend is known healthy, an offline-capable read gets a bounded
  three-second per-phase live attempt. Slow/unhealthy transport may fall back
  only because this surface is fixed read-only; editor-only calls do not use
  this reduced timeout.

This preserves public action behavior whenever a live registry is reachable.

### 4.2 Fixed Offline Allowlist

The proxy may route only these namespace dispatchers to `monolith_query.exe`:

| MCP tool | Offline namespace |
|----------|-------------------|
| `source_query` | `source` |
| `project_query` | `project` |
| `bridge_query` | `bridge` |
| `console_query` | `console` |
| `cppreflect_query` | `cppreflect` |
| `network_query` | `network` |
| `decision_query` | `decision` |
| `risk_query` | `risk` |

The following Monolith catalog tools also have fixed offline mappings:

| MCP tool | Query CLI action |
|----------|------------------|
| `monolith_status` | `monolith status` |
| `monolith_discover` | `monolith discover` |
| `monolith_find` | `monolith find` |
| `monolith_guide` | `monolith guide` |
| `monolith_get_action_metadata_coverage` | `monolith get_action_metadata_coverage` |

All other tools retain the existing typed editor-unavailable result. The proxy
must never guess that an arbitrary tool is safe offline.

### 4.3 Read-Only Boundary

Every proxy-spawned query includes the global `--readonly` option.

This rule is mandatory even when the editor transport is down: an editor
process can still be alive and retain SQLite writer or asset state after its
HTTP endpoint stops responding. Execute-gated repair, snapshot, and graph-build
operations therefore fail explicitly through the query CLI's read-only gate.

Offline database mutation requires a future single-writer lease. It is not part
of Phase A.

The query's global read-only gate is stronger than SQLite `query_only`: it
forbids every read-write/open-create path and refuses writable hot rollback-
journal recovery. A detected hot journal remains untouched and produces an
actionable error. Recovery must be an explicit non-readonly operation after the
writer has been closed.

### 4.4 Argument Translation

For `{namespace}_query` tools:

- `arguments.action` becomes the query CLI action.
- `arguments.params` accepts an object or a JSON-encoded object string. As in
  the live HTTP adapter, non-reserved top-level query arguments are merged and
  nested `params` wins on duplicate keys.
- merged parameters become validated `--snake_case=value` arguments.
- scalar JSON values are serialized without a shell.
- all arrays and objects use compact JSON, preserving commas and array element
  boundaries exactly.
- the proxy passes an internal original-JSON-type map to the child. Query
  accessors validate action-specific string/boolean/integer/number/array
  contracts before dispatch; integral JSON numbers such as `1.0` satisfy an
  integer schema, while fractional/trailing-garbage values do not.
- alias/canonical collisions, unsupported reduced-offline parameters, enum or
  range violations, null, invalid parameter names, NUL bytes, command-line
  overflow, or unsupported shapes return a typed tool error.
- action names must match ASCII `[A-Za-z0-9_]+`; action-position `--help`,
  `--version`, path-like, and global-option routing bypasses are rejected in
  both proxy and query layers.

The proxy invokes `CreateProcessW` directly with Windows command-line quoting.
It must not invoke `cmd.exe`, PowerShell, `system`, or another shell.

### 4.5 Result Shape

Successful offline responses use the normal MCP tool-result envelope:

```json
{
  "content": [{"type": "text", "text": "{...}"}],
  "structuredContent": {
    "_monolith": {
      "offline_fallback": true,
      "routing_context": {
        "decision_source": "offline_fallback",
        "backend": "monolith_query"
      }
    }
  },
  "isError": false
}
```

The result records the namespace, action, process exit state, bounded timing,
and truncation state. Query actions intentionally have two output contracts:
JSON objects remain `structuredContent`, while a non-empty plain-text result
(source excerpts, reference lists, or guide Markdown) is wrapped as
`{"output":"..."}` and remains successful. Empty output, truncated stdout,
an object-looking (`{`) malformed JSON result, failed launch, timeout, non-zero
exit, top-level `error`, or `success=false` sets `isError=true` without
pretending the editor handled the request.

The universal `_fields`, `_omit`, and `_compact_json` controls are applied to
offline structured results at the proxy boundary with the same top-level
projection rules used by live calls. They are not forwarded as action params
or silently discarded. Phase A does not advertise or implement live nested-row
`_row_fields`/`_path_fields`; cached calls using those controls receive an
explicit reduced-offline capability error rather than silent partial shaping.

---

## 5. Process and Security Contract

### 5.1 Executable Resolution

Resolution order:

1. `MONOLITH_QUERY_EXE`, when explicitly set.
2. `monolith_query.exe` beside the running immutable proxy image.

The resolved path must exist as a regular non-reparse file before process
creation. The proxy holds it open without write/delete sharing, launches it
suspended, compares the child image's volume/file identity with the pinned
handle, and resumes only on a match. Release packaging keeps the current
immutable proxy, its manifest, and Query together under
`Plugins/Monolith/Binaries`.

MCP callers cannot pass `db`, `source_db`, `project_db`, `graph_db`, or
`snapshot` overrides. Database selection is proxy-controlled so a caller cannot
manufacture an index that authorizes an arbitrary filesystem path. Direct Query
CLI use keeps its copied-DB flags. Tests/nonstandard deployments may use the
launch-time trusted `MONOLITH_OFFLINE_SOURCE_DB`,
`MONOLITH_OFFLINE_PROJECT_DB`, and `MONOLITH_OFFLINE_GRAPH_DB` variables; each
must resolve to an existing regular non-reparse file.

### 5.2 Limits

| Limit | Phase A value |
|-------|---------------|
| Query timeout | 30 seconds |
| Captured stdout | 16 MiB maximum |
| Captured stderr | 16 MiB maximum |
| Windows command line | less than 32,767 UTF-16 code units |
| Concurrent query per proxy process | one, serialized by stdio request loop |

Timeout terminates only the child query process launched for that request. The
proxy does not terminate Unreal Editor, game, user, or unrelated query
processes.

### 5.3 Environment

The child inherits the current local environment and receives bounded
trace-parent additions required by the daily invocation log contract plus the
internal `MONOLITH_MCP_PARAM_TYPES_JSON` type map. Any stale inherited value of
that internal variable is removed before the request-specific value is added.
The proxy does not print or persist a full environment dump.

No API key, token, cookie, password, private key, or authentication header may
be copied into routing metadata or a tool result.

---

## 6. Configuration

| Variable | Default | Meaning |
|----------|---------|---------|
| `MONOLITH_URL` | `http://localhost:9316/mcp` | Live editor backend URL. |
| `MONOLITH_OFFLINE_FALLBACK` | `1` | Set to `0` to disable native offline routing. |
| `MONOLITH_QUERY_EXE` | sibling binary | Explicit offline query executable path. |
| `MONOLITH_EXPECTED_PROJECT_ROOT` | inferred project root | Project identity that live `monolith_status` must report. |
| `MONOLITH_OFFLINE_SOURCE_DB` | deployed source DB | Trusted launch-time copied/test DB override; never an MCP argument. |
| `MONOLITH_OFFLINE_PROJECT_DB` | deployed project DB | Trusted launch-time copied/test DB override. |
| `MONOLITH_OFFLINE_GRAPH_DB` | source DB sibling graph | Trusted launch-time copied/test graph override. |
| `MONOLITH_TOOL_LOG_ENABLED` | `1` | Existing daily proxy/query log switch. |
| `MONOLITH_TOOL_LOG_DIR` | plugin `Logs/` | Existing isolated log-root override. |

MCP clients must use onboarding, which validates
`Plugins/Monolith/Binaries/monolith_proxy.current.json` (schema, tool/runtime
identity, leaf filename, version/source hash, binary SHA-256, executable
identity, containment, and reparse state) and records the exact immutable proxy
path in generated local/global config. A
direct `http://localhost:9316/mcp` entry
bypasses the control plane and receives no offline fallback. Onboarding defaults
to proxy mode; replacing an existing entry is explicit and reversible through
`-McpMode Http -ReplaceMcpConfig` or the timestamped project-config backup. A
mutable fixed proxy path is not required.

Script proxies remain compatibility fallbacks. Phase A native offline routing
is a Windows native-proxy capability; script-proxy parity is a separate change.

---

## 7. Cold Start and Catalog Behavior

The first offline `tools/list` always returns exactly four locally defined
control-plane tools (`monolith_query`, `monolith_discover`, `monolith_status`,
and `monolith_find`). Stale live/profile caches are never replayed. After the background
poller has observed the editor healthy, a later list request may return a live
response within the bounded catalog timeout. The response must contain a
`result.tools` array of unique minimally valid descriptors; a JSON-RPC envelope
or tool-style error alone is insufficient. Invalid live list responses open the
transport circuit and fall back to the same four tools. Legacy `*_query` names
remain accepted for compatibility but are not advertised offline.

`notifications/tools/list_changed` is sent on validated online/offline route
changes. Cold-start descriptions distinguish offline execution from live-only
guidance; no stale catalog is represented as an execution guarantee.

Offline action rows distinguish `offline_mode=execute` from
`offline_mode=guidance`. Guidance rows remain discoverable but set
`executes_offline=false` and `requires_live_editor=true`. Discovery defaults to
compact projection. `mode=schema` is explicitly `degraded_guidance` offline:
the snapshot cannot claim an authoritative current live JSON schema.

---

## 8. Capability Boundary

The query executable currently describes 96 offline CLI actions across source,
project, bridge, console, reflection intelligence, risk, and catalog domains.
Within the Phase A proxy boundary:

- 73 read/text actions can execute against on-disk databases or bundled data.
- 14 console/source actions return useful live-only guidance rather than
  touching editor state.
- 9 execute-gated maintenance actions are deliberately blocked by `--readonly`.

Exact action availability comes from `monolith_query.exe --help`; documentation
must not hard-code this count as a public MCP action count.

Editor/UObject/asset/world actions remain live-only, including Blueprint graph
mutation, material and Niagara authoring, scene placement, Slate inspection,
PIE control, viewport capture, and asset-editor operations.

---

## 9. Future Isolated Editor Worker

A persistent full-action worker is gated on a matching source-built UE 5.8
root. Its target should use:

```csharp
Type = TargetType.Editor;
LinkType = TargetLinkType.Modular;
BuildEnvironment = TargetBuildEnvironment.Unique;
bExplicitTargetForType = true;
LaunchModuleName = "MonolithHeadlessLauncher";
```

The launcher should follow the narrow LiveLinkHub-style engine loop rather than
starting the normal MainFrame/layout restoration path:

- set the exact project path and worker role;
- run `FEngineLoop::PreInit`, `Init`, an explicit tick loop, and `Exit`;
- use a private backend port or authenticated local IPC;
- isolate config, user settings, logs, intermediates, and module manifests;
- publish immutable process identity and build identity;
- expose a headless capability profile;
- shut down gracefully when the owning supervisor requests it.

The proxy or a dedicated HTTP gateway must own the public/client endpoint. A
worker must not compete with interactive editors for public port 9316.

### Worker Acceptance Gates

- Receipt contains no `UnrealEditor-Monolith*.dll`, `UnrealEditor-Speed*.dll`,
  or `UnrealEditor-Lyra*.dll` references.
- Worker modules use a worker-target prefix and separate intermediates.
- Worker running: normal `SpeedEditor` and game links succeed without LNK1104.
- Interactive editor running: worker target links without overwriting normal
  editor outputs.
- Only an ownership-proven worker can be stopped or restarted.
- Mutating asset and SQLite operations have a single-writer lease.
- Public and worker ports cannot be confused after bind failure.
- NullRHI/Slate/PIE/viewport limitations return explicit capability errors.
- A 30-minute soak has no self-kill loop, orphan, false readiness, or blocking
  modal.

Until all gates pass, the existing full editor remains the live backend and
must not be described as an isolated worker.

---

## 10. Failure Contract

| Failure | Required behavior |
|---------|-------------------|
| Live endpoint returns a valid error | Return it unchanged; do not fallback. |
| Live endpoint has no transport response, non-2xx status, or invalid JSON-RPC envelope | Try only the fixed offline allowlist. |
| `/health` is 200 but its JSON contract/project identity is missing or foreign | Reject the endpoint; do not forward, build, launch, or kill on its behalf. |
| A known-up MCP request fails | Open the circuit immediately; exponential 10/20/40/60-second cooldown prevents health-only polling from reintroducing repeated tail latency. |
| Concrete generic action is live-only, mutating, or long-running | Freshly validate project identity and use the normal live timeout; never apply the read fast-path timeout. |
| Live `tools/list` has a valid envelope but no valid `result.tools` contract | Discard it, open the circuit, and return the stable four-tool control plane. |
| Unsupported offline tool | Typed editor-unavailable result. |
| Query executable missing | Typed offline-backend error with resolved path. |
| Invalid or ambiguous arguments | Reject before process creation. |
| Malformed JSON-RPC request or invalid `tools/call` params/name/arguments | Return `-32700`, `-32600`, or `-32602` as appropriate and continue serving later requests. |
| Query exceeds 30 seconds | Terminate that child and return timeout. |
| Query output exceeds a bound | Drain the pipe, mark truncation, and bound response memory. |
| `execute=true` in fallback | Reject through the read-only gate. |
| Query returns non-zero, empty, truncated, object-looking malformed JSON, top-level `error`, or `success=false` | `isError=true` with bounded diagnostics. Intentional non-empty plain text remains a successful `{output}` result. |
| Editor bind fails | Never publish listening/readiness solely from a startup log or sentinel. |

---

## 11. Verification

Phase A verification must run against an unreachable live URL and must not rely
on a running editor:

1. Build `monolith_query.exe` and the current immutable proxy with their native
   scripts; require both self-reported source hashes to match the current inputs.
2. Set `MONOLITH_URL=http://127.0.0.1:9/mcp`.
3. Initialize the native proxy and call `tools/list`.
4. Verify the cold surface is exactly `monolith_query`, `monolith_discover`,
   `monolith_status`, and `monolith_find`, even when a stale cache exists.
5. Call `source_query` with `action=health`; require a successful offline
   result and `_monolith.offline_fallback=true`.
6. Call at least one project/catalog action and one canonical named-parameter
   action that previously required a positional CLI argument.
7. Call an editor-only action; require the existing typed unavailable result.
8. Set `MONOLITH_OFFLINE_FALLBACK=0`; require source query to return unavailable.
9. Pass `execute=true` to an execute-gated query; require read-only rejection.
10. Verify a source text action and `monolith_guide` preserve successful plain
    text/Markdown output.
11. Verify native proxy/query daily logs share trace ancestry and classify the
    offline route.
12. Verify invalid request shapes cannot terminate the proxy and that malformed
    live HTTP bodies fall through only to the fixed read-only allowlist.
13. Verify action-scoped JSON types, integral-number semantics, alias collision,
    string cursors, response shaping, compact discovery, caller DB/snapshot
    override rejection, indexed-only file reads, and summary-only logs.
14. Verify a cold live-only generic action reaches a healthy correct-project
    endpoint, while an accepted endpoint that changes project root is rejected
    before the action POST.
15. Verify a transport failure remains fast after a health-poll interval, and a
    method-invalid live `tools/list` returns the stable four tools.
16. Rebuild both native binaries in release packaging; validate the manifest
    filename/source hash/version/tool/runtime/binary SHA-256, snapshot its exact
    bytes and selected image, and atomically publish an explicit top-level
    Binaries allowlist containing Query, optional Watchdog, the selected proxy,
    and its manifest after staged pair revalidation.
17. Run `git diff --check` and the exact hosted static-check command.

Live verification remains separate: while the editor endpoint is healthy, the
same read call must be forwarded live and must not contain offline routing
metadata.

---

## 12. Implementation Phases

| Phase | Scope | State |
|-------|-------|-------|
| A | Native proxy control plane + read-only query fallback | Implemented; focused native verification complete. Repository and UE submission-gate evidence is recorded separately in `Docs/testing/2026-07-11-native-proxy-offline-fallback.md`. |
| B | Isolated editor-worker ownership and single-writer lease | Pending future architecture work. |
| C | Source-built UE Unique modular headless worker and custom launcher | Blocked on matching source-built UE root. |
| D | Public HTTP gateway ownership, private worker IPC/ports, single-writer lease | Pending architecture work. |
| E | Per-action headless capability metadata and enforcement | Pending after worker exists. |
