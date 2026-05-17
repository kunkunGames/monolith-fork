# Monolith MCP Compatibility Options First Slice

**Parent:** [SPEC_MonolithCore.md](SPEC_MonolithCore.md)
**Engine:** Unreal Engine 5.7+
**Status:** Implemented first slice
**Owner module:** MonolithCore
**Scope:** Make `monolith.set_mcp_compatibility_options` useful for safe browser-CORS compatibility without enabling legacy routes or broad origins.
**Non-goals:** Re-enabling wildcard CORS, adding legacy `/sse` or `/message` routes, exposing the server to non-loopback browser origins, changing MCP JSON-RPC dispatch, changing `.mcp.json` generation.

---

## 1. ROI Queue Position

This is a small Core follow-up after session observation:

| Candidate | Current state | ROI | Order |
|-----------|---------------|-----|-------|
| MCP session observer | Spec/implementation stack is open. | Medium-high: safe client/session diagnostics. | Prior slice. |
| Progress/cancel | Existing remote branches already cover active request rows, progress, and cancel contracts. | High but collision-prone. | Skip in this stack. |
| Resource links / typed media | Existing remote branches already cover resource-link and typed-media results. | High but collision-prone. | Skip in this stack. |
| Compatibility options | `set_mcp_compatibility_options` exists but always reports unavailable. | Medium: lets operators intentionally close browser CORS while keeping default loopback compatibility. | This slice. |

---

## 2. Problem

Monolith's transport defaults are intentionally conservative after the CORS hardening work:

| Question | Current state | Needed first slice |
|----------|---------------|--------------------|
| Can browser loopback CORS be disabled without shutting down the MCP server? | No. | Add a setting-backed `browser_access` option. |
| Can legacy routes be toggled? | No, and this is safer. | Continue rejecting legacy route enable requests explicitly. |
| Does the action tell callers what changed? | No. It always returns unavailable. | Return current/applied options and unsupported requests. |

---

## 3. First Slice Contract

`monolith.set_mcp_compatibility_options` accepts:

```json
{
  "options": {
    "browser_access": "loopback_only"
  }
}
```

or:

```json
{
  "options": {
    "browser_access": "disabled"
  }
}
```

Rules:

1. `browser_access="loopback_only"` keeps the current allowlist behavior: browsers may read responses only from `localhost`, `127.0.0.1`, or `[::1]` origins.
2. `browser_access="disabled"` omits `Access-Control-Allow-Origin` for all browser origins while keeping non-browser MCP clients functional.
3. The value is stored in `UMonolithSettings` and takes effect for subsequent HTTP responses without an editor restart.
4. The action must never accept wildcard or arbitrary origins in this slice.
5. The action must not enable legacy routes.

---

## 4. Response Contract

Successful update:

```json
{
  "status": "ok",
  "changed": true,
  "browser_access": "disabled",
  "legacy_sse_route_enabled": false,
  "legacy_message_route_enabled": false,
  "unsupported_options": []
}
```

Unsupported legacy route request:

```json
{
  "status": "ok",
  "changed": false,
  "browser_access": "loopback_only",
  "legacy_sse_route_enabled": false,
  "legacy_message_route_enabled": false,
  "unsupported_options": ["legacy_sse_route_enabled"],
  "reason": "Legacy SSE/message routes are not implemented in this slice."
}
```

Invalid `browser_access` values should return `ErrInvalidParams`.

---

## 5. Status Contract

`monolith.get_mcp_server_status.cors` should report:

```json
{
  "mode": "loopback_origin_allowlist",
  "browser_access": "loopback_only",
  "allow_origin_header_enabled": true
}
```

or:

```json
{
  "mode": "browser_cors_disabled",
  "browser_access": "disabled",
  "allow_origin_header_enabled": false
}
```

`routes.legacy_sse` and `routes.legacy_message` remain `false`.

---

## 6. Implementation Plan

1. Add `bEnableBrowserLoopbackCors` to `UMonolithSettings`, default `true`.
2. Gate `FMonolithHttpServer::AddCorsHeaders` origin echo on that setting.
3. Update `HandleGetMcpServerStatus` CORS output.
4. Implement `HandleSetMcpCompatibilityOptions` for `browser_access` only.
5. Reject unknown `browser_access` values with `ErrInvalidParams`.
6. Report legacy route requests as unsupported instead of mutating routes.
7. Add automation tests for status output and option mutation semantics.
8. Update `Docs/API_REFERENCE.md` and [SPEC_MonolithCore.md](SPEC_MonolithCore.md) when code lands.

---

## 7. Verification Gates

| Gate | Required evidence |
|------|-------------------|
| Default compatibility | Default setting preserves loopback allowlist CORS. |
| Disabled browser CORS | Setting false omits `Access-Control-Allow-Origin` for all origins. |
| Non-browser compatibility | MCP JSON responses still work; only browser read access changes. |
| Legacy routes remain off | `legacy_sse_route_enabled` and `legacy_message_route_enabled` stay false. |
| Invalid values rejected | Unknown `browser_access` returns invalid params. |
| No wildcard origin | No response path emits `Access-Control-Allow-Origin: *`. |

---

## 8. Follow-up Slices

| Follow-up | Reason to defer |
|-----------|-----------------|
| Per-client origin allowlist | Needs UI/config review and security documentation. |
| Legacy route compatibility | Requires route implementation, client matrix, and stronger regression testing. |
| `.mcp.json` compatibility generation | Belongs with setup/onboarding tooling, not HTTP response policy. |
