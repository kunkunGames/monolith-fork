# Monolith MCP Resources First Slice

**Parent:** [SPEC_MonolithCore.md](SPEC_MonolithCore.md)
**Engine:** Unreal Engine 5.7+
**Status:** Implemented first slice
**Owner module:** MonolithCore
**Scope:** Add a bounded, read-only MCP `resources/list` and `resources/read` surface backed by explicit Monolith resource providers.
**Non-goals:** Arbitrary filesystem access, binary/blob resources, writable resources, subscriptions, resource templates, external network fetches, persistent caches.

---

## 1. ROI Queue Position

This is the next high-ROI Core slice after the ToolCall ledger:

| Candidate | Current state | ROI | Order |
|-----------|---------------|-----|-------|
| ToolCall ledger | Implemented as settings-gated redacted in-memory records. | High: makes MCP calls auditable. | Done first. |
| MCP resources | Implemented as a settings-gated read-only provider registry. | High: exposes durable docs and diagnostics without bloating `tools/list` or tool results. | This slice. |
| Structured tool results | Implemented as settings-gated `structuredContent` / `_meta` output. | High: improves client parsing after resources exist. | Done next. |
| MCP session/progress/cancel mode | Session observation is implemented; progress/cancel remain follow-up transport work. | Medium-high: valuable for long-running operations, but broader transport state. | Session observer done; progress/cancel later. |

The resources slice is intentionally read-only and provider-based so it can land before session mode.

---

## 2. Problem

Monolith sends most context through tools and text JSON responses. The resource slice adds a standard read-only MCP resource surface so stable reference material is discoverable without scanning the checkout:

| Question | Current state | Needed state |
|----------|---------------|--------------|
| What stable Monolith docs can the client read without scanning the checkout? | Implemented through bounded `resources/list` descriptors. | Keep descriptors explicit and sorted. |
| Can a client fetch one spec or API reference through standard MCP? | Implemented through `resources/read` over registered providers. | Keep reads bounded and text-only. |
| Can resources expose project files safely? | Not yet; arbitrary filesystem reads would be unsafe. | Only registered providers may serve content, and each content item has a size cap. |
| Can the server report whether resources are active? | Implemented through configured, active, handler-registered, and restart-required status fields. | Keep status tied to real registry state. |

---

## 3. First Slice Contract

Enable these JSON-RPC methods only when `UMonolithSettings::bEnableMcpResources=true` at startup:

| Method | Purpose | Required params | Optional params |
|--------|---------|-----------------|-----------------|
| `resources/list` | Return registered read-only resource descriptors in stable URI order. | none | `limit`, `cursor` |
| `resources/read` | Return the content for one registered resource URI. | `uri` | none |

If the setting is false, the methods are not handled and the existing method-not-found behavior remains.

Status reporting must update `monolith.get_mcp_server_status`:

```json
{
  "features": {
    "mcp_resources": {
      "compiled": true,
      "configured": true,
      "active": true,
      "handlers_registered": true,
      "state": "active_readonly_registry"
    }
  }
}
```

If config and handler state disagree after a runtime setting toggle, status must report `restart_required=true`.

---

## 4. Resource Registry Contract

Add a `FMonolithResourceRegistry` singleton owned by MonolithCore.

| Field | Contract |
|-------|----------|
| `uri` | Stable `monolith://...` URI. Empty URI is rejected. |
| `name` | Human-readable short name. |
| `description` | Short description for client selection. |
| `mimeType` | Defaults to `text/plain`; first slice should use `text/markdown` and `application/json` only when content is text. |
| `provider` | Bound delegate/lambda that returns text from an explicit source. |
| `max_chars` | Per-resource text cap, clamped to a safe upper bound. |

The first built-in provider set should expose stable documentation resources from tracked Monolith docs:

| URI | Source | Mime type |
|-----|--------|-----------|
| `monolith://docs/specs/core` | `Docs/specs/SPEC_MonolithCore.md` | `text/markdown` |
| `monolith://docs/specs/toolcall-ledger` | `Docs/specs/SPEC_MonolithToolCallLedger.md` | `text/markdown` |
| `monolith://docs/specs/mcp-resources` | `Docs/specs/SPEC_MonolithMcpResources.md` | `text/markdown` |
| `monolith://docs/api-reference` | `Docs/API_REFERENCE.md` | `text/markdown` |
| `monolith://docs/todo` | `Docs/TODO.md` | `text/markdown` |

Live (read-time) providers backed by Monolith services — these evaluate on each read so the payload reflects current state, and emit redacted, bounded JSON (no raw request/response payloads):

| URI | Source | Mime type |
|-----|--------|-----------|
| `monolith://tool-calls/recent` | `FMonolithActionExecutionGuard::GetToolCallRecordsJson(50)` | `application/json` |
| `monolith://audit/recent` | `FMonolithActionExecutionGuard::GetRecentAuditJson(50)` | `application/json` |
| `monolith://progress/active` | `FMonolithProgressRegistry::GetActiveJson()` (in-flight per-progressToken progress; poll-delivered) | `application/json` |

Missing optional docs must be skipped rather than returned as broken descriptors.

---

## 5. Response Shapes

`resources/list` success:

```json
{
  "resources": [
    {
      "uri": "monolith://docs/specs/core",
      "name": "MonolithCore spec",
      "description": "Top-level MonolithCore module behavior and contracts",
      "mimeType": "text/markdown"
    }
  ],
  "nextCursor": "25"
}
```

Rules:

1. Results are sorted by URI for deterministic pagination.
2. `limit` is clamped to `1..100`.
3. `cursor` is an opaque string in the public contract; the first implementation may use a numeric offset internally.
4. `nextCursor` is omitted on the final page.
5. Invalid cursor values are treated as the first page or rejected with `ErrInvalidParams`; the implementation must be consistent and tested.

`resources/read` success:

```json
{
  "contents": [
    {
      "uri": "monolith://docs/specs/core",
      "mimeType": "text/markdown",
      "text": "# Monolith -- MonolithCore Module",
      "truncated": false
    }
  ]
}
```

`resources/read` errors:

| Case | Error |
|------|-------|
| Missing or empty `uri` | JSON-RPC `ErrInvalidParams` |
| Unknown registered URI | Server-defined resource-not-found code in the JSON-RPC `-32000..-32099` range |
| Provider read failure | Server-defined resource-not-found or internal error, with no absolute filesystem path in client-facing text |

---

## 6. Security And Privacy

1. The registry must never read arbitrary caller-provided filesystem paths.
2. Providers must be explicit and registered by code.
3. Responses must not expose absolute local paths, environment variables, API keys, auth headers, cookies, session ids, or bearer tokens.
4. Content is bounded by `max_chars`; truncated reads set `truncated=true`.
5. First slice is text-only. Binary/blob support needs a later media contract.

---

## 7. Implementation Notes

1. `MonolithResourceRegistry.h/.cpp` owns descriptor registration, deterministic list pagination, bounded text read, and test reset hooks.
2. MonolithCore startup registers default docs resources only when `bEnableMcpResources=true`.
3. `FMonolithHttpServer::HandleJsonRpc` handles `resources/list` and `resources/read` only when the resource feature is active.
4. Server status reports configured, active, handler-registered, and restart-required state from real settings and registry data.
5. `MonolithResourceRegistryTests.cpp` covers list/read, pagination, missing URI, missing resource, truncation, and default docs registration.

---

## 8. Verification Gates

| Gate | Required evidence |
|------|-------------------|
| Disabled by default | With `bEnableMcpResources=false`, resource methods are not active and existing tools/ping behavior is unchanged. |
| Handler registration | With the flag true at startup, `resources/list` and `resources/read` are handled. |
| Deterministic list | Multiple resources list in sorted URI order with correct `nextCursor` behavior. |
| Bounded read | Long provider text is truncated and marks `truncated=true`. |
| Error shape | Missing `uri` returns invalid params; unknown URI returns the resource-not-found code. |
| Safety | Caller-provided filesystem paths are never read, and client errors do not expose absolute paths. |
| Status accuracy | Server status reports configured/active/registered/restart-required accurately. |

---

## 9. Follow-up Slices

| Follow-up | Reason to defer |
|-----------|-----------------|
| Resource templates | Needs URI template validation and parameter schema. |
| Typed media content | Better paired with structured/typed tool result helpers. |
| ToolCall ledger resource provider | Depends on the ToolCall ledger implementation PR landing first. |
| Subscriptions/change notifications | Requires MCP session/request state. |
