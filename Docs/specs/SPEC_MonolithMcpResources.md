# Monolith MCP Resources First Slice

**Parent:** [SPEC_MonolithCore.md](SPEC_MonolithCore.md)
**Engine:** Unreal Engine 5.7+
**Status:** Proposed
**Owner module:** MonolithCore
**Scope:** Add a bounded, read-only MCP `resources/list` and `resources/read` surface backed by explicit Monolith resource providers.
**Non-goals:** Arbitrary filesystem access, binary/blob resources, writable resources, subscriptions, resource templates, external network fetches, persistent caches.

---

## 1. ROI Queue Position

This is the next high-ROI Core slice after the ToolCall ledger:

| Candidate | Current state | ROI | Order |
|-----------|---------------|-----|-------|
| ToolCall ledger | Spec and implementation PRs are open as the prior stack. | High: makes MCP calls auditable. | Done first. |
| MCP resources | `bEnableMcpResources` exists but reports `settings_only_provider_registry_pending`. | High: exposes durable docs and diagnostics without bloating `tools/list` or tool results. | This slice. |
| Structured tool results | `bEnableStructuredToolResults` exists but helper/result contract is pending. | High: improves client parsing after resources exist. | Next candidate. |
| MCP session/progress/cancel mode | `bEnableMcpSessionMode` exists but request/session rows are not active on master. | Medium-high: valuable for long-running operations, but broader transport state. | Later stack. |

The resources slice is intentionally read-only and provider-based so it can land before session mode.

---

## 2. Problem

Monolith currently sends most context through tools and text JSON responses. That keeps the protocol simple, but it makes stable reference material harder for MCP clients to discover:

| Question | Current state | Needed state |
|----------|---------------|--------------|
| What stable Monolith docs can the client read without scanning the checkout? | No MCP resource endpoint. | A bounded `resources/list` response with explicit Monolith resource descriptors. |
| Can a client fetch one spec or API reference through standard MCP? | No `resources/read` method. | A read-only provider registry that returns text content by URI. |
| Can resources expose project files safely? | Not yet; arbitrary filesystem reads would be unsafe. | Only registered providers may serve content, and each content item has a size cap. |
| Can the server report whether resources are active? | Server status says settings-only. | Status must distinguish configured, active, and handler-registered state. |

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

## 7. Implementation Plan

1. Add `MonolithResourceRegistry.h/.cpp` with descriptor registration, deterministic list pagination, bounded text read, and test reset hooks.
2. Register default docs resources during MonolithCore startup only when `bEnableMcpResources=true`.
3. Add `resources/list` and `resources/read` handling to `FMonolithHttpServer::HandleJsonRpc`.
4. Add `ErrResourceNotFound` to `FMonolithJsonUtils` in the server-defined range.
5. Update `monolith.get_mcp_server_status` feature reporting from settings-only to actual registered handler state.
6. Add automation tests for registry list/read, pagination, missing URI, missing resource, truncation, and default docs registration.
7. Update `Docs/API_REFERENCE.md` and `Docs/specs/SPEC_MonolithCore.md` in the implementation PR when public methods land.

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
