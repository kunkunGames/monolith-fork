# Monolith - MonolithSlate Module

**Parent:** [../SPEC_CORE.md](../SPEC_CORE.md)
**Engine:** Unreal Engine 5.7+
**Version:** 0.14.10
**Owner module:** MonolithSlate
**Namespace:** `slate`
**Status:** Implemented first read-only slice
**Date:** 2026-05-19

---

## 1. Scope

`MonolithSlate` owns the native `slate` namespace for live editor Slate UI inspection.
This is the high-ROI UnrealMCP Toolsets parity item that Monolith did not
cover with its existing UMG/Widget Blueprint authoring actions. The first slice
is read-only: it can inspect visible Slate windows and widgets, return bounded
opaque refs, capture Slate widgets through Slate screenshot APIs, and poll for
UI state. It does not simulate input.

The implementation intentionally does not link against
`SlateInspectorToolset`, `ToolsetRegistry`, or UnrealMCP. It follows the useful
public behavior from UE 5.8's experimental `SlateInspectorToolset` while using
Monolith's settings, registry, and JSON result contracts.

---

## 2. Settings And Registration

| Field | Value |
|-------|-------|
| Owner module | `MonolithSlate` |
| Namespace | `slate` |
| Status action | `slate.get_inspector_status` always registers with the `MonolithSlate` module |
| Feature flag | `UMonolithSettings::bEnableSlateInspectorActions=false` |
| Registration rule | Read-only inspection actions register only after enabling the flag and restarting the editor |
| Input automation | Not implemented in this slice |

`get_inspector_status` is available even when the inspection flag is off so
clients can discover the disabled state without guessing at settings. All other
actions are gated by `bEnableSlateInspectorActions`.

---

## 3. Action Contract

| Action | Params | Contract |
|--------|--------|----------|
| `slate.get_inspector_status` | none | Reports flag state, Slate initialization, visible window count, generation, ref TTL, and which actions are registered or gated. |
| `slate.list_windows` | `include_titles=true` | Lists visible top-level Slate windows with indexes, visibility/enabled flags, size, and redacted titles. |
| `slate.snapshot_widgets` | `window_index=-1`, `max_depth=8`, `max_widgets=200`, `include_hidden=false` | Rebuilds the process-local ref cache and returns a bounded flat widget tree with opaque refs. |
| `slate.describe_widget` | `ref` | Resolves a current ref and returns type, visibility, enabled/focused state, geometry, text summary, parent summary, and capped child summaries. |
| `slate.capture_widget` | `ref?`, `max_bytes=1048576` | Uses `FSlateApplication::TakeScreenshot` for a widget or active top-level window, compresses PNG bytes, and returns base64 only when under the byte cap. It never falls back to level viewport capture. |
| `slate.wait_for_widget` | `text_contains?`, `type?`, `visible=true`, `timeout_ms=1000`, `poll_interval_ms=100` | Polls visible Slate widgets for a matching text/type/visibility condition without input simulation. |

All result arrays are capped. Text fields are trimmed and path-like window
titles are redacted. Returned refs do not expose memory addresses.

---

## 4. Opaque Ref Model

Refs are process-local and short-lived:

```text
slate:<generation>:<window_index>:<widget_index>:<short_hash>
```

| Rule | Behavior |
|------|----------|
| Generation | Incremented each time `snapshot_widgets` rebuilds the cache. |
| TTL | Current slice uses a fixed 5000 ms TTL. |
| Expiry | A ref is stale when the generation differs, the widget weak pointer has expired, or the TTL has elapsed. |
| Validation | `describe_widget` and `capture_widget` require a valid current ref unless capture is explicitly requested for the active window by omitting `ref`. |
| Error shape | Stale or malformed refs return `stale_ref`/`invalid_ref` style errors instead of best-effort rematching. |

---

## 5. Screenshot Contract

Slate capture must stay distinct from level viewport capture.

| Field | Meaning |
|-------|---------|
| `capture_source` | Always `slate_widget` or `slate_active_window` for success. |
| `viewport_fallback_used` | Always `false`. |
| `mime_type` | `image/png` on success. |
| `bytes_b64` | Present only when compressed bytes are within `max_bytes`. |
| `captured=false` | Returned for unavailable, empty, or over-cap captures with a concrete reason. |

The action does not write screenshots to arbitrary local paths in this slice.

---

## 6. Verification Gates

| Gate | Required evidence |
|------|-------------------|
| Static docs/code sync | This spec, `SPEC_CORE.md`, and `API_REFERENCE.md` describe the same action surface. |
| UE 5.7 compile | Full UBT plugin build against the `GO.uproject`-resolved engine root. |
| Disabled default | With default settings, only `get_inspector_status` is registered in the `slate` namespace. |
| Bounded output | Snapshot/wait/list actions clamp user-supplied limits and do not return raw memory addresses or local paths. |
| Capture source | `capture_widget` uses Slate screenshot APIs and reports `viewport_fallback_used=false`. |
| Input safety | No click/hover/type/key actions are registered in this slice. |

---

## 7. Deferred Work

| Item | Reason |
|------|--------|
| Observer/ref refresh loop | Higher runtime footprint; add after read-only snapshot behavior is stable. |
| Input simulation | Mutates live editor UI state and needs a separate explicit gate, confirmation contract, and before/after snapshot result. |
| Dynamic MCP image resources | Requires a resource-provider lifetime policy for transient screenshots. The first slice returns capped base64 PNG bytes instead. |
