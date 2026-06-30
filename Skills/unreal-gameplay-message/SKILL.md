---
name: unreal-gameplay-message
description: Use when inspecting Unreal GameplayMessageRouter contracts via Monolith MCP — channel gameplay tags, ExactMatch/PartialMatch behavior, payload UScriptStruct validation, and listener/broadcast readiness. Read-only; does not register listeners, broadcast messages, run PIE, or edit assets.
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

## Typical Flow

1. Run `gameplay_message.get_status` to confirm the plugin/runtime modules are available.
2. Run `gameplay_message.validate_message_struct` on the payload struct.
3. Run `gameplay_message.validate_channel_contract` with the channel tag and payload struct.

For preflight planning before a gameplay tag is added, pass `require_registered_tag=false`; otherwise missing tags are reported as errors.
