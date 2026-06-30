# SPEC_MonolithGameplayMessage

| Field | Value |
| --- | --- |
| Module | `MonolithGameplayMessage` |
| Namespace | `gameplay_message` |
| Type | Editor |
| Status | Current |

---

## 1. Purpose

`MonolithGameplayMessage` provides reusable read-only diagnostics for Unreal projects that use Epic's `GameplayMessageRouter` plugin. It validates the channel tag, match type, and payload `UScriptStruct` contract that `UGameplayMessageSubsystem` requires, without linking against or modifying the runtime plugin.

---

## 2. Ownership

| Area | Owner |
| --- | --- |
| Gameplay message plugin/module/class availability | `gameplay_message.get_status` |
| Listener/broadcast contract explanation | `gameplay_message.describe_listener_contract` |
| Payload struct validation | `gameplay_message.validate_message_struct` |
| Channel + payload pair validation | `gameplay_message.validate_channel_contract` |

---

## 3. Actions

| Action | Params | Behavior |
| --- | --- | --- |
| `gameplay_message.get_status` | none | Reports `GameplayMessageRouter`, `GameplayMessageRuntime`, `GameplayMessageNodes`, `UGameplayMessageSubsystem`, async listener action, listener handle struct, and match enum availability. |
| `gameplay_message.describe_listener_contract` | none | Reports the reflected subsystem/async-action functions and the shared listener contract rows for exact/partial match behavior and payload type agreement. |
| `gameplay_message.validate_message_struct` | `message_struct`, optional `require_blueprint_type=false`, optional `require_no_object_references=false` | Loads the requested object as `UObject`, verifies it is a `UScriptStruct`, checks metadata and object-reference properties, and returns structured checks/issues. |
| `gameplay_message.validate_channel_contract` | `channel_tag`, optional `message_struct`, optional `match_type=ExactMatch`, optional `require_registered_tag=true`, optional `require_blueprint_type=false` | Validates GameplayTags registration, match type (`ExactMatch`/`PartialMatch`), and optional payload struct compatibility. |

---

## 4. Constraints

| Constraint | Requirement |
| --- | --- |
| Runtime isolation | Do not add a compile-time dependency on `GameplayMessageRuntime` or `GameplayMessageNodes`; use reflection only. |
| Mutability | Actions are read-only and must not register listeners, broadcast messages, run PIE, or save assets. |
| Tag validation | Missing gameplay tags are errors by default, but callers can pass `require_registered_tag=false` for preflight planning. |
| Payload validation | `UScriptStruct` payload agreement is reported as data; the action does not infer per-channel runtime listener state. |

---

## 5. Verification

| Check | Required result |
| --- | --- |
| Build | `SpeedEditor Win64 Development` compiles `UnrealEditor-MonolithGameplayMessage.dll` via the engine resolver from `Speed.uproject`. |
| Automation | `Monolith.GameplayMessage.RegistryAndValidation` passes with zero warnings and zero errors. |
| Drift guard | `Scripts/check_skill_catalog_drift.ps1 -Skill unreal-gameplay-message` reports `RESULT=OK`. |
