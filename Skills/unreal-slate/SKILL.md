---
name: unreal-slate
description: Use when introspecting or capturing live editor Slate / Editor Utility Widget (EUW) UI via Monolith MCP (slate namespace) - snapshot the live Slate widget tree, describe a widget's type/geometry/text/focus, list visible top-level windows, capture a widget or window to PNG, and poll widget state without simulating input. Editor-side Slate tooling only; for runtime UMG Widget Blueprints, HUDs, and menus use unreal-ui; to edit an Editor Utility Widget's Blueprint graph logic use unreal-blueprints. Triggers on slate, slate widget, editor widget, editor utility widget, EUW, editor UI, inspect editor widget, capture widget, screenshot editor window, list windows, widget snapshot, describe widget, wait for widget, slate inspector.
---

# unreal-slate

Drives the **`slate`** namespace via `slate_query(action, params)` for live editor Slate introspection and capture: snapshot the widget tree, describe a widget ref, list visible windows, capture to PNG, and poll widget state. The **6 actions** below are a snapshot of the live registry surface; discover first so you never call a stale or guessed name.

## Discovery

```
monolith_discover({ namespace: "slate" })                                      # all actions in this namespace
monolith_discover({ namespace: "slate", action: "<action>", mode: "schema" })  # exact params for one action
```

## When to use / Use a different skill for

- **unreal-ui** — the work is runtime UMG Widget Blueprints, HUDs, menus, or game-facing UI rather than editor-side Slate introspection. This skill owns only editor Slate / Editor Utility Widget tooling.
- **unreal-blueprints** — you need to edit the Editor Utility Widget's Blueprint graph logic (nodes, variables, events) rather than introspect or capture the live Slate widget tree.

## Action Reference

Param notation: `name*` required, `name?` optional, `name=val` default, `a/b/c` allowed, `[w]` mutates. Signatures are a snapshot of the live catalog — for the exact full schema call `monolith_discover` with `mode: "schema"`. Keep the discover-first block above as the authority.

### Inspector (6)

| Action | Params | Purpose |
|--------|--------|---------|
| `get_inspector_status` | _(none)_ | Report Slate inspector flag state, Slate availability, visible window count, ref generation, and gated read-only actions. |
| `list_windows` | `include_titles?=true` | List visible top-level Slate windows with bounded metadata and redacted titles. |
| `snapshot_widgets` | `window_index?=-1` `max_depth?=8` `max_widgets?=200` `include_hidden?=false` | Return a bounded live Slate widget snapshot and short-lived opaque refs. Rebuilds the ref cache. |
| `describe_widget` | `ref*` | Describe a current Slate ref: type, visibility, focus, geometry, text, parent, and capped children. |
| `capture_widget` | `ref?` `max_bytes?=1048576` | Capture a Slate widget (`ref`) or, when `ref` is omitted, the active top-level window with `FSlateApplication::TakeScreenshot`. No level viewport fallback. |
| `wait_for_widget` | `text_contains?` `type?` `visible?=true` `timeout_ms?=1000` `poll_interval_ms?=100` `max_depth?=12` | Single non-blocking scan of visible Slate widgets for text/type/visibility state without input simulation. |

`get_inspector_status` is the always-registered action and takes no params. The other five signatures above are transcribed from the `slate` `RegisterAction` / `FParamSchemaBuilder` calls in `MonolithSlate/Private/MonolithSlateInspectorActions.cpp`; they require `bEnableSlateInspectorActions` to be registered (see caveat below). All six are read-only inspector actions (no `[w]` mutation). Two runtime notes the schema does not encode: `describe_widget`/`capture_widget` `ref` must come from a current `snapshot_widgets` generation (refs expire after ~5000 ms), and `wait_for_widget` requires at least one of `text_contains` or `type` and no longer blocks server-side — `timeout_ms`/`poll_interval_ms` are accepted for call-site compatibility but you must poll client-side.

## Common Workflows

**Flag-gated surface:** every action except `get_inspector_status` requires `bEnableSlateInspectorActions` to be registered. Always start with `get_inspector_status` to learn whether Slate is available, whether the inspector flag is on, and which read-only actions are currently gated; if `enabled` is false the inspector actions are not registered and `gated_actions` lists them. All steps below use only Action Reference actions with their real params.

### Inspect the live editor UI end-to-end (gate-check → list → snapshot → describe → capture)
```
1. slate_query("get_inspector_status", {})                                          # Slate availability, inspector flag, visible window count, ref generation, which actions are gated
2. slate_query("list_windows", { "include_titles": true })                          # visible top-level windows (titles redacted by design) — pick a target window_index
3. slate_query("snapshot_widgets", { "window_index": 0, "max_depth": 8, "max_widgets": 200 })  # bounded widget tree + short-lived opaque refs; rebuilds the ref cache
4. slate_query("describe_widget", { "ref": "slate:<gen>:<win>:<idx>:<hash>" })       # type, visibility, focus, geometry, text, parent, capped children (ref from step 3)
5. slate_query("capture_widget", { "ref": "slate:<gen>:<win>:<idx>:<hash>", "max_bytes": 1048576 })  # PNG via FSlateApplication::TakeScreenshot, no level-viewport fallback
```

### Poll for a widget to appear, then inspect it (no input simulation)
```
1. slate_query("get_inspector_status", {})                                          # confirm Slate up + wait_for_widget registered (enabled=true)
2. slate_query("wait_for_widget", { "text_contains": "Save", "visible": true, "max_depth": 12 })  # single non-blocking scan; need text_contains OR type. Re-call client-side to wait.
3. slate_query("snapshot_widgets", {})                                              # refresh refs after the widget appears (refs expire after ~5000 ms)
4. slate_query("describe_widget", { "ref": "slate:<gen>:<win>:<idx>:<hash>" })       # inspect the now-visible widget (ref from step 3)
```

## Gotchas / Rules

- Refs are short-lived opaque handles tied to the current ref generation — re-run `snapshot_widgets` if a ref goes stale, and check `get_inspector_status` ref generation when a `describe_widget`/`capture_widget` call rejects a ref.
- `capture_widget` uses `FSlateApplication::TakeScreenshot` and has **no level viewport fallback**; for in-level/PIE captures use scene/editor capture paths instead of this namespace.
- Window titles are redacted in `list_windows` metadata by design — do not treat redacted titles as a bug.
- `get_inspector_status` is the only always-registered action; `list_windows`, `snapshot_widgets`, `describe_widget`, `capture_widget`, and `wait_for_widget` require `bEnableSlateInspectorActions` to be registered. Call `get_inspector_status` first — its `enabled`, `gated_actions`, and `slate_initialized` fields tell you which read-only actions are currently callable.
- This reference is generated from the live `RegisterAction` surface. If an action is missing or renamed, re-run `monolith_discover({ namespace: "slate" })` — the catalog is the source of truth.
