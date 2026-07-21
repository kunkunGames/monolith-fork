# Monolith MCP Proxy Configuration

## Current Configuration

The Monolith MCP is configured to use the C++ proxy executable:

```json
{
  "command": "<project-root>/Plugins/Monolith/Binaries/monolith_proxy-<source-hash>.exe",
  "args": []
}
```

Do not substitute the historical fixed filename manually. Run
`Scripts/onboard_monolith.ps1`; it validates `Binaries/monolith_proxy.current.json`
and registers the exact immutable image selected by that manifest. Existing MCP
sessions can continue using an older image while a new build is staged and new
sessions adopt it, avoiding Windows executable-lock collisions.

The recommended configuration is set in Codex and Claude user-level MCP config
through their CLIs. A project `.mcp.json` is optional for clients that require
project scope; this checkout keeps that file portable and does not commit an
absolute machine-local immutable image path.

## Rollback to Script Proxy (if needed)

If the C++ proxy encounters issues, you can revert to the script proxy by updating both config files.

Windows:
```json
{
  "command": "<project-root>/Plugins/Monolith/Scripts/monolith_proxy.bat",
  "args": []
}
```

macOS / Linux:
```json
{
  "command": "<project-root>/Plugins/Monolith/Scripts/monolith_proxy.sh",
  "args": []
}
```

Update the Monolith entry in the affected user-level client configuration. If a
client explicitly uses project scope, update its `<project-root>/.mcp.json` as a
separate opt-in operation.

Then restart Claude Code.

## Proxy Details

- **Script proxy:** `Scripts/monolith_proxy.bat` (Windows) / `Scripts/monolith_proxy.sh` (macOS/Linux) — Stdio-to-HTTP proxy launchers. The Windows launcher probes Python 3.8+, `python3`, Node.js, then `py -3`; the macOS/Linux launcher probes `python3`, `python` (3.8+), then Node.js. They wrap `monolith_proxy.py` or `monolith_proxy.js`, which survive editor restarts via background health polling
- **C++ proxy:** `Plugins/Monolith/Binaries/monolith_proxy-<source-hash>.exe` selected by `monolith_proxy.current.json` — Native executable and the preferred long-lived control plane. The source-addressed filename is immutable and the manifest verifies the full artifact SHA-256; the historical `monolith_proxy.exe` copy is compatibility-only. At startup the proxy also pins and validates the exact immutable Query executable and catalog selected by `monolith_query.current.json`. It forwards to the live editor when available and can execute a fixed read-only offline subset through that pair when the editor transport is down.
- **Generation identity:** `Scripts/source_generation_hash.py` hashes the Proxy build contract and ordered text inputs after CRLF/lone-CR to LF normalization. Raw executable and manifest SHA-256 verification is deliberately not normalized.
- **Backend:** Script proxies connect only to the Monolith HTTP server running in the Unreal Editor. The native proxy accepts a live endpoint only after validating the `/health` JSON contract and binding `monolith_status` to the expected host-project root. Plain HTTP is loopback-only and HTTPS requests use WinHTTP secure transport. A failed known-up request immediately opens the offline circuit, so later reads do not each pay the live timeout. The background poller honors transport backoff, while a successful validated MCP request closes the circuit; either validated state transition announces `tools/list_changed`.
- **Editor-down startup:** The native proxy advertises exactly four stable control-plane tools: `monolith_query`, `monolith_discover`, `monolith_status`, and `monolith_find`. It does not replay stale cached live/profile tools. Legacy `*_query` names remain accepted for existing clients but are not advertised while offline.

## Native Offline Fallback

The native proxy keeps useful Monolith work available while the editor is closed, restarting, or unavailable during a build:

```text
MCP client -> manifest-selected monolith_proxy-<source-hash>.exe
                  |-- live:    Unreal Editor /mcp
                  `-- offline: manifest-selected monolith_query-<source-hash>.exe
                               + monolith_catalog-<semantic-hash>.json
```

Offline routing is intentionally allowlisted. `monolith_query` covers the fixed read-only `source`, `project`, `bridge`, `console`, `cppreflect`, `network`, `decision`, and `risk` query dispatchers; the other three advertised tools provide catalog discovery, status, and search. Snapshot-known actions that require Unreal return structured `live_only` guidance. Offline `mode=schema` is explicitly `degraded_guidance`: the snapshot describes routing metadata but never pretends to be the authoritative live JSON schema. When the editor health and project identity are valid, `monolith_discover(mode="schema")` therefore uses the normal live request budget rather than the short offline-read probe budget; only an unavailable or failed live transport may return that degraded guidance. Editor/UObject/asset/world actions remain live-only.

Every proxy-spawned query is read-only. Execute-gated repair, snapshot, graph-build, read-write/open-create, and writable hot-journal recovery paths are rejected until a single-writer lease exists; a failed HTTP endpoint does not prove the editor process has released its SQLite writer state. A hot rollback journal is left untouched with explicit recovery guidance.

Configuration:

| Variable | Default | Meaning |
|---|---|---|
| `MONOLITH_OFFLINE_FALLBACK` | `1` | Set to `0` to preserve live-only proxy behavior. |
| `MONOLITH_QUERY_EXE` | unset; `monolith_query.current.json` selects the immutable executable | Development/test override. Setting either Query or catalog override switches the proxy to the explicit override branch; production onboarding leaves both unset. |
| `MONOLITH_CATALOG_SNAPSHOT` | unset; `monolith_query.current.json` selects the immutable catalog | Development/test override for the catalog paired with Query. |
| `MONOLITH_URL` | `http://localhost:9316/mcp` | Live editor backend attempted before fallback. |
| `MONOLITH_EXPECTED_PROJECT_ROOT` | inferred from the deployed plugin path | Override the project identity that `monolith_status` must report. Tests and non-standard binary layouts should set this explicitly. |
| `MONOLITH_OFFLINE_SOURCE_DB` | deployed `Saved/EngineSource.db` | Launch-time trusted override for a copied/test source database. MCP call arguments cannot override database paths. |
| `MONOLITH_OFFLINE_PROJECT_DB` | deployed `Saved/ProjectIndex.db` | Launch-time trusted override for a copied/test project database. |

The child query process is launched directly with `CreateProcessW`, not through a shell. Before launch the proxy opens and pins the executable, rejects a reparse-point image, resolves its final path, and holds a non-write/non-delete-sharing handle across `CreateProcessW`; this closes the check/use replacement window. Calls have a 30-second timeout, bounded stdout/stderr capture, validated action/option names, Windows-safe argument quoting, restricted inherited handles, and inherited trace ancestry for invocation logs. Arrays remain compact JSON so comma-containing symbols and paths preserve element boundaries. An internal per-request type map lets Query enforce the original MCP JSON types, aliases, and reduced-offline capability boundary instead of accepting every value as a CLI string.

Offline `source.read_file` resolves only files already indexed in the proxy-selected `EngineSource.db`, rejects reparse points, and canonicalizes the requested/indexed path before reading. MCP parameters named `db`, `source_db`, `project_db`, or `snapshot` are rejected, so a caller cannot manufacture a SQLite file that lists an arbitrary host path. Direct `monolith_query.exe` usage retains its documented explicit copied-DB overrides.

The proxy accepts live-compatible top-level/nested params and JSON-encoded object params. Nested `params` wins on duplicates. Offline structured results support top-level `_fields`, `_omit`, and `_compact_json`; `_row_fields` and `_path_fields` are not part of the Phase A cold contract and return an explicit reduced-capability error when routed offline.

Malformed JSON-RPC input, wrong `tools/call` params/name/arguments types, and malformed live HTTP bodies cannot terminate or corrupt the long-lived stdio stream. The proxy returns the appropriate request error and continues serving later messages.

Daily `proxy.jsonl`/`query.jsonl` diagnostics retain call arguments subject to redaction, outcome, byte counts, shape, routing, and timing. Tool response bodies, previews, and response-content hashes are deliberately omitted so source/asset data is not copied into diagnostic logs.

This fallback is a native Windows proxy feature. The Python and Node script proxies remain live-editor transport fallbacks and do not claim offline query execution parity.

## Native legacy call log

In addition to the shared daily v3 diagnostics described above, the native C++
proxy appends one legacy JSONL line per upstream MCP roundtrip to:

```
<project-root>/Saved/Logs/MonolithCalls.jsonl
```

The Python and Node script proxies do not emit this legacy file; they emit only
`Logs/yyyyMMdd/proxy.jsonl`, controlled by `MONOLITH_TOOL_LOG_ENABLED`,
`MONOLITH_TOOL_LOG_DIR`, and `MONOLITH_TOOL_LOG_MAX_FIELD_BYTES`. The native
proxy emits that daily v3 log as well as this compatibility log. See
[`SPEC_MonolithToolInvocationLogs.md`](../../Docs/specs/SPEC_MonolithToolInvocationLogs.md)
for the daily contract.

Legacy path resolution: `MONOLITH_PROJECT_ROOT` env var if set, otherwise the native proxy's
current working directory (Claude Code launches the proxy with the project root
as CWD). The `Saved/Logs/` parent directories are created on first run.

### Schema (one object per line, terminated by `\n`)

```json
{"ts":"2026-05-27T18:14:56Z","namespace":"editor","action":"get_build_errors","params_hash":"da39a3ee5e6b4b0d3255bfef95601890afd80709","duration_ms":42.5,"ok":true,"error_code":null,"result_bytes":1834}
```

| Field | Type | Notes |
|---|---|---|
| `ts` | string | ISO-8601 UTC, second precision. |
| `namespace` | string | For `*_query` tools: the prefix (e.g. `editor`). For `monolith_*` tools: `monolith`. For non-`tools/call` methods: the method name (`initialize`, `tools/list`, `ping`). |
| `action` | string | For `*_query`: the `action` argument. For `monolith_*`: the suffix (`discover`, `status`, etc.). Empty otherwise. |
| `params_hash` | string | 40-char hex SHA-1 over canonicalised JSON of the params dict (`sort_keys=True`, tightest separators). Used to recognise repeat calls without storing arguments. NOT 32-bit FCrc — collision-safe. |
| `duration_ms` | number | Wall time between sending the upstream HTTP request and receiving the response. |
| `ok` | bool | `true` iff the response has no JSON-RPC `error` field. |
| `error_code` | int or null | The JSON-RPC `error.code` when `ok` is false; null otherwise. |
| `result_bytes` | int | Byte length of the serialised `result` payload (or the full response body if no `result`). |

### Opt-out

Set the environment variable `MONOLITH_CALL_LOG=0` before launching the native proxy.
This disables emission entirely — no file handle is opened, no writes are made.
Any other value (including unset) leaves logging enabled. The env var is read
once at startup; toggling it mid-session requires a proxy restart.

### Use cases

- Post-hoc grep / analysis of which actions an agent session called.
- Spot the silent retries hidden by the dedup window.
- Pipe through your own log-tailing tool for live tailing.
- Cheap input to future Markov-style breadcrumb analytics (deferred — substrate
  ships first, consumers later).

### Privacy

Local-only. Nothing is uploaded. The `params_hash` is one-way — the original
parameter values cannot be recovered from the log. Filenames and asset paths
inside the hashed JSON are not extractable from the line.

### Rotation / reset

User-managed. Delete the file to start fresh; the native proxy recreates it on the
next call. No automatic rotation in v1 — the file is small (≈200 bytes/line) and
single-user.

### Crash-reporter exclusion

UE's crash reporter sweeps editor logs from `Saved/Logs/`, not arbitrary JSONL.
If a downstream crash collector pattern is added that sweeps `Saved/Logs/*`,
add `MonolithCalls.jsonl` to its exclusion list. The file is intentionally
local-only.
