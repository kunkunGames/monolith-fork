# Monolith MCP Session Mode Gate (P1c)

**Parent:** [SPEC_MonolithMcpSessionMode.md](SPEC_MonolithMcpSessionMode.md)
**Engine:** Unreal Engine 5.7+
**Status:** Implemented (spec-correctness slice; reuses existing `bEnableMcpSessionMode`, no new flag)
**Owner module:** MonolithCore
**Scope:** Promote the bounded in-memory MCP session observer toward the MCP Streamable HTTP lifecycle contract: track session lifecycle status and redacted client capabilities, enforce a server-side session gate around the per-request loop, and track a tool-list revision so `initialize` can advertise `tools.listChanged`. Every change early-returns to byte-identical legacy wire behavior when `bEnableMcpSessionMode` is off.
**Non-goals:** Persistent sessions, server-push `notifications/tools/list_changed` delivery (single-shot SSE has no long-lived push channel), interrupting running Unreal actions, storing raw session ids / params / capability objects, adding a new feature flag, changing any existing action's input/output JSON schema.

---

## 1. ROI Queue Position

This is the next session-mode slice after the bounded observer, reusing the same flag:

| Candidate | Current state | ROI | Order |
|-----------|---------------|-----|-------|
| MCP session observer | Implemented bounded in-memory observer (P1 base). | Medium-high: client/session visibility. | Done earlier. |
| Session lifecycle + gate + list revision | This slice: lifecycle status, redacted client caps, server session gate, tool-list revision. | High: spec-correct session handling without breaking stateless clients. | This slice. |
| `notifications/tools/list_changed` push | Deferred: needs a real SSE transport. | Medium: live tool-surface invalidation. | Follow-up. |

---

## 2. Problem

`bEnableMcpSessionMode` currently only records observation rows. The MCP Streamable HTTP lifecycle expects the server to (a) know a session's lifecycle phase, (b) reject malformed/stale session traffic, and (c) tell clients whether the tool list can change. None of that existed, but none of it may regress the default-off stateless path.

| Question | Before | This slice (enabled only) |
|----------|--------|---------------------------|
| Does the observer know a session's lifecycle phase? | No. | `Observed` / `Initializing` / `Initialized` per row. |
| Are client capabilities visible? | No. | Redacted booleans for `roots` / `sampling` / `elicitation`. |
| Are post-initialize requests without a session id rejected? | No. | InvalidRequest + HTTP 400. |
| Are unknown/expired sessions rejected? | No. | HTTP 404. |
| Is an unsupported `MCP-Protocol-Version` header rejected? | No. | InvalidRequest + HTTP 400. |
| Does `initialize` advertise `tools.listChanged`? | Always `false`. | `true`, with a tracked revision counter. |

---

## 3. Session Lifecycle Additions

`FMonolithMcpSessionTracker` gains an additive lifecycle. The enum and row fields default to the legacy observed shape, so a row created by the existing `ObserveRequest` path is unchanged in meaning.

| Member | Behavior |
|--------|----------|
| `enum class EMonolithMcpSessionStatus : uint8 { Observed, Initializing, Initialized }` | Lifecycle phase. `Observed` is the default for a plain request with no handshake. |
| `MarkInitialize(SessionId, ProtocolVersion, bRoots, bSampling, bElicitation)` | Seeds/updates the row, sets `Initializing`, stores only the boolean presence of each capability group. |
| `MarkInitialized(SessionId)` | Sets `Initialized`. No-op on an unknown session (never seeds a row). |
| `IsKnownSession(SessionId) const` | True when a hashed row exists. Raw id hashed before lookup, never stored. |

Redaction is unchanged: only `session_key` (hash), `session_id_redacted` (prefix/suffix), protocol version, method/tool name, counts, timestamps, lifecycle status, and capability booleans are retained. The raw capability object, client name, and version string are never stored.

### `list_mcp_sessions` row additions (additive keys)

```json
{
  "session_key": "md5:abcd1234...",
  "lifecycle_status": "initialized",
  "client_capabilities": { "roots": true, "sampling": false, "elicitation": true }
}
```

All pre-existing row keys are byte-identical; only `lifecycle_status` and `client_capabilities` are added.

---

## 4. Session Gate

`FMonolithHttpServer::EvaluateSessionGate(Methods, HeaderSessionId, HeaderProtocolVersion, bSessionKnown, bSessionModeEnabled)` is a pure, static, unit-testable function (mirrors `IsAllowedOrigin` / `NegotiateProtocolVersion`). It runs around the per-request loop in `HandlePostMcp`, after headers are parsed and the request list is built.

| Rule | Condition (enabled only) | Result |
|------|--------------------------|--------|
| Unsupported protocol version | `MCP-Protocol-Version` present and not in `GetSupportedProtocolVersions()` | JSON-RPC InvalidRequest + HTTP 400 |
| Unknown / expired session | `MCP-Session-Id` present but `bSessionKnown == false` | HTTP 404 |
| Missing session id post-initialize | No `MCP-Session-Id` and any method is not `initialize` / `notifications/initialized` / `ping` | JSON-RPC InvalidRequest + HTTP 400 |
| Pass | none of the above | `bReject == false` |

`FSessionGateResult { bReject, HttpCode, RpcCode, Message }`. On reject the caller serializes a JSON-RPC error body (id `null`) and returns it as the whole HTTP response with `HttpCode`. CORS headers are applied identically to every other response path.

**Off path:** `EvaluateSessionGate` returns `bReject == false` unconditionally when `bSessionModeEnabled == false`, and `HandlePostMcp` does not even build the `Methods` array in that case — so the request flows into the existing per-request loop with zero behavioral or byte difference.

---

## 5. Tool-List Revision and `tools.listChanged`

`FMonolithToolProfileManager` owns a monotonic `int64 ToolListRevision`, bumped (after a successful save) by every mutator that changes the advertised tool surface: `SetActiveProfile`, `SetActionEnabled`, `SetNamespaceEnabled`, `SetDescriptionOverride`, `UpsertProfile`, `DeleteProfile`. `GetToolListRevision()` reads it under the manager lock. The manager stays MCP-agnostic — it owns only the counter; the MCP server is the sole consumer.

`HandleInitialize` consumes the counter only when `bEnableMcpSessionMode` is on:

```json
"capabilities": {
  "tools": { "listChanged": true, "_monolith_tool_list_revision": 7 }
}
```

When the flag is off, `tools.listChanged` stays `false` and no revision field is emitted — byte-identical legacy capabilities.

### No-fake delivery discipline

This slice advertises the `tools.listChanged` capability, tracks the revision, and defines the wire shape. It does **not** fabricate a `notifications/tools/list_changed` push, because `GET /mcp` returns a single-shot SSE event and closes — there is no long-lived channel to deliver an unsolicited notification on. A client detects a changed surface by re-running `tools/list` (and may compare `_monolith_tool_list_revision`). Real push delivery awaits an SSE transport and is deferred.

---

## 6. Contract Preservation

| Surface | Guarantee |
|---------|-----------|
| `tools/call`, `tools/list`, `resources/*`, existing action schemas | Unchanged. |
| `list_mcp_sessions` rows | Existing keys byte-identical; lifecycle/capability keys added only. |
| `initialize` capabilities (flag off) | `tools.listChanged=false`, no revision field — byte-identical. |
| Per-request loop (flag off) | Gate is skipped entirely; legacy path unchanged. |

---

## 7. Verification Gates

| Gate | Required evidence |
|------|-------------------|
| Disabled pass-through | `EvaluateSessionGate(..., bSessionModeEnabled=false)` never rejects for inputs that reject when enabled. |
| Missing id post-initialize | Reject InvalidRequest + 400; `initialize` / `notifications/initialized` / `ping` exempt. |
| Unknown session | Reject 404; known session passes. |
| Protocol version | Unsupported header rejects 400; supported and empty header pass. |
| Lifecycle | `MarkInitialize` -> `initializing` + capability booleans; `MarkInitialized` -> `initialized`; `MarkInitialized` no-ops on unknown id; raw id never serialized. |
| Tool-list revision | Each profile mutator bumps `GetToolListRevision()` monotonically. |
| Test hygiene | Tracker tests call `ResetForTests()` at start and end; profile test deletes its temp profile. |

Automation tests live at `Source/MonolithCore/Private/Tests/MonolithMcpSessionModeGateTests.cpp`
(`Monolith.Core.McpSessionGate.*`, `Monolith.Core.McpSessionTracker.Lifecycle`, `Monolith.Core.ToolProfile.ToolListRevision`).

---

## 8. Follow-up Slices

| Follow-up | Reason to defer | Gate |
|-----------|-----------------|------|
| `notifications/tools/list_changed` push | Needs a real SSE transport; single-shot SSE cannot push. | `bEnableMcpSessionMode` |
| Per-session protocol-version pinning | Requires storing the negotiated version per row and rejecting drift. | `bEnableMcpSessionMode` |
| In-flight cancellation tied to session terminate | Requires cooperative cancellation inside long-running actions (see async job registry). | `bEnableAsyncJobs` |
