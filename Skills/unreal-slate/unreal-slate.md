---
name: unreal-slate
description: Use for editor Slate / Editor Utility Widget introspection and tooling via Monolith MCP. Triggers on slate, editor widget, editor utility widget, EUW, editor UI, slate widget.
---

# unreal-slate

**6 actions** via `slate_query(action, params)`. Action names below are the live registry surface; call `monolith_discover` for exact parameter schemas.

## Discovery

```
monolith_discover({ namespace: "slate" })                      # all actions in this namespace
monolith_discover({ namespace: "slate", action: "<action>", mode: "schema" })  # exact params
```

## Action Reference

### Inspector (6)

| Action | Purpose |
|--------|---------|
| `capture_widget` | Capture a Slate widget or active top-level window with FSlateApplication::TakeScreenshot. No level viewport fallback. |
| `describe_widget` | Describe a current Slate ref: type, visibility, focus, geometry, text, parent, and capped children. |
| `get_inspector_status` | Report Slate inspector flag state, Slate availability, visible window count, ref generation, and gated read-only actions. |
| `list_windows` | List visible top-level Slate windows with bounded metadata and redacted titles. |
| `snapshot_widgets` | Return a bounded live Slate widget snapshot and short-lived opaque refs. Rebuilds the ref cache. |
| `wait_for_widget` | Poll visible Slate widgets for text/type/visibility state without input simulation. |

## Notes

- This reference is generated from the live `RegisterAction` surface. If an action is missing or renamed, re-run `monolith_discover({ namespace: "slate" })` — the catalog is the source of truth.
- Pass `mode: "schema"` to `monolith_discover` for required/optional params and types before calling an action.
