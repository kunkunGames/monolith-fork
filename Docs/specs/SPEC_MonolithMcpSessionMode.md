# Monolith MCP Session Mode First Slice

**Parent:** [SPEC_MonolithCore.md](SPEC_MonolithCore.md)
**Engine:** Unreal Engine 5.7+
**Status:** Proposed
**Owner module:** MonolithCore
**Scope:** Add bounded, in-memory MCP session observation for Streamable HTTP requests while preserving stateless request execution.
**Non-goals:** Persistent session storage, long-lived server push streams, progress notifications, interrupting running Unreal actions, storing raw session ids, storing raw request params or results.

---

## 1. ROI Queue Position

This is the next MonolithCore slice after structured tool results:

| Candidate | Current state | ROI | Order |
|-----------|---------------|-----|-------|
| ToolCall ledger | Spec and implementation stack is open. | High: local observability. | Done first. |
| MCP resources | Spec and implementation stack is open. | High: durable docs and diagnostics through standard MCP resources. | Done second. |
| Structured tool results | Spec and implementation stack is open. | High: native JSON parsing for modern clients. | Done third. |
| MCP session mode | `bEnableMcpSessionMode` exists but reports settings-only. Session actions return unavailable. | Medium-high: lets clients/operators see active MCP clients before progress/cancel work. | This slice. |

Session observation should land before progress/cancel because cancellation needs a request/session correlation surface that is safe to expose.

---

## 2. Problem

Monolith's current Streamable HTTP endpoint accepts MCP protocol/session headers, but the server remains completely stateless:

| Question | Current state | Needed first slice |
|----------|---------------|--------------------|
| Can an operator see whether clients are sending `MCP-Session-Id`? | No. `list_mcp_sessions` reports unavailable. | Return bounded observed session rows when enabled. |
| Does enabling `bEnableMcpSessionMode` change behavior? | Only status says settings-only pending. | Status reports an active in-memory observer. |
| Are raw session ids stored? | No. | Continue not storing raw session ids; expose redacted/hash identifiers only. |
| Can clients cancel active work? | No. | Keep cancellation out of scope and report that no in-flight request was interrupted. |

---

## 3. First Slice Contract

When `UMonolithSettings::bEnableMcpSessionMode=false`, behavior stays legacy:

1. `monolith.list_mcp_sessions` returns `status: "unavailable"` and `session_count: 0`.
2. `monolith.terminate_mcp_session` returns `terminated: false`.
3. `monolith.get_mcp_server_status.features.mcp_session_mode.active=false`.

When `UMonolithSettings::bEnableMcpSessionMode=true`, Monolith records bounded, process-local observations for `POST /mcp` requests:

| Field | Source | Storage rule |
|-------|--------|--------------|
| `session_key` | Hash of `MCP-Session-Id` header when present, otherwise `"stateless"` bucket | No raw session id stored. |
| `session_id_redacted` | `MCP-Session-Id` prefix/suffix only | Safe for diagnostics. |
| `protocol_version` | `MCP-Protocol-Version` header or initialize params | Bounded string. |
| `request_count` | Incremented per observed POST request | In memory only. |
| `first_seen_utc` / `last_seen_utc` | Server clock | In memory only. |
| `last_method` | JSON-RPC method name | No params or results stored. |
| `last_tool_name` | `tools/call.params.name` when present | No action params stored. |

The session table is bounded and process-local. Editor restart clears it.

---

## 4. Action Contracts

### `monolith.list_mcp_sessions`

When enabled:

```json
{
  "status": "active",
  "mode": "in_memory_observer",
  "raw_session_ids_stored": false,
  "session_count": 1,
  "sessions": [
    {
      "session_key": "md5:abcd1234...",
      "session_id_redacted": "mcp_...7890",
      "protocol_version": "2025-03-26",
      "request_count": 12,
      "first_seen_utc": "2026-05-18T00:00:00Z",
      "last_seen_utc": "2026-05-18T00:01:00Z",
      "last_method": "tools/call",
      "last_tool_name": "blueprint_query"
    }
  ]
}
```

Rules:

1. `limit` is clamped to `1..1000`.
2. Rows are sorted by most recent observation first.
3. The result never includes raw headers, raw params, raw request bodies, auth headers, cookies, bearer tokens, or API keys.

### `monolith.terminate_mcp_session`

When enabled, this action removes the observed row that matches the supplied raw `session_id` hash:

```json
{
  "status": "observed_row_removed",
  "terminated": true,
  "cancelled_in_flight_requests": false,
  "reason": "First slice removes only the local observed session row; it does not interrupt running Unreal actions."
}
```

Rules:

1. The request may accept the raw `session_id` because a caller must prove knowledge of the id to remove the row.
2. The raw `session_id` is never echoed in the response and is never stored.
3. If there is no matching observed row, return `terminated: false` and `status: "not_found"`.

---

## 5. Status Contract

`monolith.get_mcp_server_status` should report session mode as active only when the setting is enabled:

```json
{
  "features": {
    "mcp_session_mode": {
      "compiled": true,
      "configured": true,
      "active": true,
      "state": "active_in_memory_observer",
      "raw_session_ids_stored": false,
      "progress_notifications": false,
      "request_cancellation": false
    }
  }
}
```

The top-level `session_tracking` field should change from `"not_persistent"` to `"in_memory_observer"` when enabled.

---

## 6. Implementation Plan

1. Add `FMonolithMcpSessionTracker` to own a bounded process-local observation table.
2. Extract `MCP-Session-Id` and `MCP-Protocol-Version` headers in `FMonolithHttpServer::HandlePostMcp`.
3. Observe JSON-RPC method names and `tools/call.params.name` without storing params or result payloads.
4. Keep tracking disabled unless `bEnableMcpSessionMode=true`.
5. Update `monolith.list_mcp_sessions` and `monolith.terminate_mcp_session` to use the tracker when enabled.
6. Update `monolith.get_mcp_server_status` to report active observer state.
7. Add focused automation tests for redaction, bounded rows, disabled behavior, list ordering, and terminate semantics.
8. Update `Docs/API_REFERENCE.md` and [SPEC_MonolithCore.md](SPEC_MonolithCore.md) when code lands.

---

## 7. Verification Gates

| Gate | Required evidence |
|------|-------------------|
| Disabled compatibility | Existing unavailable session action responses remain when the setting is false. |
| Redaction | Raw `MCP-Session-Id` never appears in session rows or status output. |
| Observation | Enabled mode records method, tool name, protocol version, counts, and timestamps. |
| Bound | Tracker enforces a fixed row capacity and evicts oldest rows. |
| Terminate | Removing a known row works by raw id hash match without echoing the raw id. |
| No cancellation claim | Responses clearly state that no in-flight request interruption exists in this slice. |

---

## 8. Follow-up Slices

| Follow-up | Reason to defer |
|-----------|-----------------|
| Request-level progress records | Requires action-level progress hooks or scoped progress callbacks. |
| `notifications/progress` emission | Needs a durable notification transport policy and client compatibility review. |
| In-flight cancellation | Requires cooperative cancellation checks inside long-running domain actions. |
| Per-request correlation with ToolCall ledger | Should build on the session observer and advanced ToolCall records stack. |
