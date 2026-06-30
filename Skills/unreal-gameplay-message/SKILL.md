---
name: unreal-gameplay-message
description: Use when inspecting Unreal GameplayMessageRouter contracts via Monolith MCP — channel gameplay tags, ExactMatch/PartialMatch behavior, payload UScriptStruct validation, static broadcaster/listener source tracing, and listener/broadcast readiness. Read-only; does not register listeners, broadcast messages, run PIE, or edit assets.
---

# Unreal Gameplay Message

Use the `gameplay_message` namespace for GameplayMessageRouter diagnostics. These actions are read-only and reflection-based; they do not depend on project-specific Lyra or Speed classes.

## Discovery

```js
monolith_discover({ namespace: "gameplay_message", mode: "actions" })
monolith_discover({ namespace: "gameplay_message", action: "validate_channel_contract", mode: "schema" })
```

## Action Reference

| Action | Params | Use |
| --- | --- | --- |
| `get_status` | none | Check `GameplayMessageRouter`, runtime/nodes modules, subsystem class, async listener action, listener handle struct, and match enum availability. |
| `describe_listener_contract` | none | Summarize the `UGameplayMessageSubsystem` listener/broadcast contract and reflected Blueprint-facing functions. |
| `validate_message_struct` | `message_struct*`, `require_blueprint_type?=false`, `require_no_object_references?=false` | Validate a payload `UScriptStruct` path and optional metadata/property constraints. |
| `validate_channel_contract` | `channel_tag*`, `message_struct?`, `match_type?=ExactMatch`, `require_registered_tag?=true`, `require_blueprint_type?=false` | Validate a message channel gameplay tag, match type, and optional payload struct. |
| `trace_channel_usage` | `channel_tag?`, `source_root?`, `source_roots?`, `include_monolith_source?=false`, `include_engine_gameplay_message_sources?=false`, `max_files?=2000`, `max_results?=500`, `include_line_text?=false` | Lexically trace broadcaster/listener source call sites and report inferred channel/payload/match graph candidates. |

## Typical Flow

1. Run `gameplay_message.get_status` to confirm the plugin/runtime modules are available.
2. Run `gameplay_message.validate_message_struct` on the payload struct.
3. Run `gameplay_message.validate_channel_contract` with the channel tag and payload struct.
4. Run `gameplay_message.trace_channel_usage` when you need a static broadcaster/listener graph, payload mismatch candidates, orphan broadcaster/listener candidates, or ExactMatch/PartialMatch ambiguity candidates.

For preflight planning before a gameplay tag is added, pass `require_registered_tag=false`; otherwise missing tags are reported as errors.

`trace_channel_usage` is source-text analysis only. It does not run PIE, register listeners, broadcast messages, or prove runtime reachability; use its `channel_graph`, `broadcasters`, `listeners`, and `issues` as candidates to review.
