# Monolith Structured Tool Results First Slice

**Parent:** [SPEC_MonolithCore.md](SPEC_MonolithCore.md)
**Engine:** Unreal Engine 5.7+
**Status:** Proposed
**Owner module:** MonolithCore
**Scope:** Add settings-gated MCP `structuredContent` helper output while preserving the legacy text JSON result envelope.
**Non-goals:** Removing legacy text results, changing domain action result payloads, typed media content, resource links, session/progress metadata, per-action custom serializers.

---

## 1. ROI Queue Position

This is the next high-ROI Core slice after MCP resources:

| Candidate | Current state | ROI | Order |
|-----------|---------------|-----|-------|
| ToolCall ledger | Prior spec/implementation stack is open. | High: local observability. | Done first. |
| MCP resources | Prior spec/implementation stack is open. | High: durable docs and diagnostics through standard MCP resources. | Done second. |
| Structured tool results | `bEnableStructuredToolResults` exists but still reports settings-only. | High: lets clients parse native JSON without scraping text. | This slice. |
| MCP session/progress/cancel mode | `bEnableMcpSessionMode` exists but requires broader transport state. | Medium-high but larger blast radius. | Later stack. |

Structured results should land after resources because later slices can add resource links and typed media entries on top of the same helper surface.

---

## 2. Problem

Monolith's MCP `tools/call` response currently serializes the action result JSON into `content[0].text`. That is compatible with simple clients, but it forces newer MCP clients to parse JSON from text even when the server already has a structured `FJsonObject`.

| Question | Current state | Needed state |
|----------|---------------|--------------|
| Can clients read successful results as JSON without text parsing? | No; JSON is embedded as a string. | Add `structuredContent` when opted in. |
| Do old clients keep working? | Yes today. | Preserve `content[]` text JSON exactly as the compatibility surface. |
| Can errors expose hints and related actions structurally? | Mostly text plus top-level legacy fields. | Add structured error content while preserving text and legacy fields. |
| Is the behavior safely gated? | Setting exists but implementation is pending. | Gate helper output behind `bEnableStructuredToolResults`. |

---

## 3. First Slice Contract

When `UMonolithSettings::bEnableStructuredToolResults=false`, `tools/call` response shape remains legacy:

```json
{
  "content": [
    {
      "type": "text",
      "text": "{\"value\":42}"
    }
  ],
  "isError": false
}
```

When `UMonolithSettings::bEnableStructuredToolResults=true`, successful calls additionally include `structuredContent` and `_meta`:

```json
{
  "content": [
    {
      "type": "text",
      "text": "{\"value\":42}"
    }
  ],
  "isError": false,
  "structuredContent": {
    "value": 42
  },
  "_meta": {
    "result_kind": "structured",
    "legacy_text_json": true,
    "truncated": false
  }
}
```

Compatibility rules:

1. `content` remains an array with a text entry for both success and error responses.
2. Successful `structuredContent` is the same JSON object currently serialized into text.
3. If an action succeeds with no result object, text remains `{}` and `structuredContent` is `{}`.
4. Existing top-level legacy error fields such as `related_actions`, `hints`, and copied `ErrorData` stay available.

---

## 4. Error Contract

When structured output is enabled, errors should include a structured error object:

```json
{
  "content": [
    {
      "type": "text",
      "text": "Missing required param(s): [asset_path].\n\nHint: asset_path is required"
    }
  ],
  "isError": true,
  "structuredContent": {
    "ok": false,
    "error": "Missing required param(s): [asset_path].",
    "error_code": -32602,
    "hints": ["asset_path is required"],
    "related_actions": ["compile_blueprint"],
    "error_data": {
      "retry": "adjust_params"
    }
  },
  "_meta": {
    "result_kind": "error",
    "legacy_text_json": true,
    "truncated": false
  }
}
```

Rules:

1. The text error remains human-readable and includes hints/related actions as before.
2. `structuredContent.ok` is `false` for errors.
3. `error_code` uses the same JSON-RPC/Monolith error code as the action result.
4. `error_data` is copied as an object only when available.

---

## 5. Status Contract

`monolith.get_mcp_server_status` should report structured results as active when the setting is enabled:

```json
{
  "features": {
    "structured_tool_results": {
      "compiled": true,
      "configured": true,
      "active": true,
      "state": "active_structured_content",
      "legacy_text_json": true
    }
  }
}
```

This slice does not require restart-sensitive registration because helper output is evaluated per `tools/call`.

---

## 6. Implementation Plan

1. Add `FMonolithToolResultUtils` to build MCP tool result envelopes from `FMonolithActionResult`.
2. Move duplicated success/error envelope construction out of `FMonolithHttpServer::HandleToolsCall`.
3. Pass `UMonolithSettings::bEnableStructuredToolResults` into the helper at response build time.
4. Preserve legacy text JSON and top-level legacy error fields.
5. Update server status feature reporting from settings-only to active/inactive helper state.
6. Add automation tests for legacy success, structured success, structured no-result success, and structured error.
7. Update `Docs/API_REFERENCE.md` and [SPEC_MonolithCore.md](SPEC_MonolithCore.md) when code lands.

---

## 7. Verification Gates

| Gate | Required evidence |
|------|-------------------|
| Legacy shape | With the flag false, `structuredContent` and `_meta` are absent and text JSON remains present. |
| Structured success | With the flag true, `structuredContent` equals the success result object and text JSON is still present. |
| Empty success | A success with no result object returns `{}` in text and `{}` in `structuredContent`. |
| Structured error | Errors include `structuredContent.ok=false`, `error`, `error_code`, hints, related actions, and optional `error_data`. |
| Status accuracy | `structured_tool_results` reports active only when configured. |
| No domain contract drift | Domain action handlers do not need per-action changes. |

---

## 8. Follow-up Slices

| Follow-up | Reason to defer |
|-----------|-----------------|
| Typed media content | Needs a media-specific result contract and client compatibility review. |
| Resource link result entries | Should depend on both MCP resources and this helper. |
| Per-action result schemas | Larger domain-by-domain documentation effort. |
| Response truncation policy | Requires consistent max-size policy across tool results, resources, and ToolCall ledger. |
