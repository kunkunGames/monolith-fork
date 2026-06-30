---
name: unreal-loading
description: Use when inspecting Unreal CommonLoadingScreen and Lyra loading-widget handoff contracts via Monolith MCP — manager reason getter, loading processor candidates, CVar/settings provenance, and no-PIE blocker diagnostics. Read-only; does not register processors, create tasks, set CVars, run PIE, or edit widgets/assets.
---

# Unreal Loading

Use the `loading` namespace for CommonLoadingScreen diagnostics. These actions are read-only and reflection-based; they do not require compile-time dependencies on `CommonLoadingScreen` or `LyraGame`.

## Discovery

```js
monolith_discover({ namespace: "loading", mode: "actions" })
monolith_discover({ namespace: "loading", action: "trace_loading_screen_blockers", mode: "schema" })
```

## Action Reference

| Action | Params | Use |
| --- | --- | --- |
| `get_status` | `include_settings?=true`, `include_cvars?=true`, `include_lyra_handoff?=false` | Check CommonLoadingScreen plugin/module/classes, reflected settings CDO values, known CVars, config, and optional Lyra handoff classes. |
| `describe_loading_processors` | `include_all_implementers?=false`, `class_filter?`, `max_objects?=100` | Describe `ULoadingProcessInterface` / `ULoadingProcessTask` and optionally list loaded implementer classes. |
| `validate_loading_reason_contract` | `include_known_lyra?=true`, `strict?=false` | Validate manager reason getter, settings class, known CVars, processor interface, and optional Lyra handoff classes. |
| `trace_loading_screen_blockers` | `world_context?=pie`, `include_settings?=true`, `include_cvars?=true`, `include_processor_candidates?=true`, `include_lyra_handoff?=true`, `max_candidates?=64` | Read the live manager debug reason when PIE/game world exists, or return `pie_not_running` / `world_not_running` without failing. |

## Typical Flow

1. Run `loading.get_status` to confirm plugin, settings, and CVar availability.
2. Run `loading.validate_loading_reason_contract` before relying on a project loading-screen setup.
3. Run `loading.trace_loading_screen_blockers` while PIE is running to read the authoritative `ULoadingScreenManager` debug reason.

Processor rows are candidate inventory. `ILoadingProcessInterface::ShouldShowLoadingScreen(FString&)` is native-only and not callable through reflection, so per-processor private reasons are not claimed by this first slice.
