# Monolith ToolCall Ledger First Slice

**Parent:** [SPEC_MonolithCore.md](SPEC_MonolithCore.md)
**Engine:** Unreal Engine 5.7+
**Status:** Proposed
**Owner module:** MonolithCore
**Scope:** Extend the existing central execution guard audit into a local, redacted ToolCall ledger.
**Non-goals:** External analytics, raw parameter logging, automatic rollback, persistent disk storage, session-progress/cancel support.

---

## 1. Problem

Monolith already records a small in-memory audit row through `FMonolithActionExecutionGuard`, but the row is not yet a ToolCall-shaped contract. The current row is useful for debugging duration and dirty package changes, but it cannot answer common operator questions:

| Question | Current state | Needed state |
|----------|---------------|--------------|
| Which MCP request or tool call produced this action? | No request or tool call id is recorded. | Store a stable local `tool_call_id`, optional redacted MCP request id, and action name. |
| Did the handler succeed, fail, or get profile-blocked? | Status is always `handler_returned`. | Store normalized `outcome` and `error_code` when available. |
| Did a mutating action dirty assets? | Dirty package count exists. | Keep package deltas, truncation flags, and a mutation hint in the ToolCall row. |
| Can clients fetch one row or analyze recent failures? | Only recent audit rows are exposed. | Add lookup and aggregate analysis actions. |

The first slice should improve local observability without changing any domain action contracts.

---

## 2. Existing Foundation

| Symbol/action | Current role | Reuse decision |
|---------------|--------------|----------------|
| `FMonolithActionExecutionGuard` | Starts and ends a central dispatch scope, snapshots dirty `/Game` packages, keeps a bounded in-memory audit array. | Reuse as the ledger owner. |
| `monolith.get_execution_guard_status` | Reports guard mode and audit capacity. | Extend status with ToolCall ledger capability fields. |
| `monolith.list_recent_action_audit` | Returns current audit rows. | Keep as compatibility surface; add ToolCall actions beside it. |
| `monolith.get_last_rollback` | Reports rollback unavailability honestly. | Keep unchanged. |
| `bEnableAdvancedToolCallRecords` | Existing default-off settings flag reserved for advanced ToolCall records. | Gate new ToolCall actions and detailed fields behind this flag. |

---

## 3. First Slice Actions

Add these actions under the existing `monolith` namespace only when `bEnableAdvancedToolCallRecords=true`.

| Action | Purpose | Required params | Optional params |
|--------|---------|-----------------|-----------------|
| `list_tool_call_records` | Return recent bounded ToolCall rows newest-first. | none | `limit`, `outcome`, `action_prefix`, `include_changed_packages` |
| `get_tool_call_record` | Return one record by id. | `id` | none |
| `analyze_tool_call_records` | Return local aggregate counts and slow/failing action summaries over the in-memory window. | none | `limit`, `slow_ms` |

Compatibility rule: `list_recent_action_audit` must keep its current row shape so existing clients do not break.

---

## 4. Record Contract

Every ToolCall row must be local, bounded, and redacted. Do not store raw params, full result JSON, auth headers, cookies, bearer tokens, API keys, or session secrets.

```json
{
  "id": "guid",
  "tool_call_id": "guid-or-client-id",
  "request_id_redacted": "optional-string",
  "action": "namespace.action",
  "namespace": "namespace",
  "action_name": "action",
  "started_utc": "2026-05-17T00:00:00Z",
  "duration_ms": 12.3,
  "outcome": "success|error|profile_blocked|cancelled|unknown",
  "error_code": "optional-stable-code",
  "changed_package_count": 1,
  "changed_packages": ["/Game/Example"],
  "changed_packages_truncated": false,
  "mutation_hint": "none|dirty_packages|unknown",
  "raw_payload_logged": false
}
```

Rules:

1. `id` is always generated locally by Monolith.
2. `tool_call_id` may reuse a future client-provided id only after validation; otherwise it is the same local GUID as `id`.
3. `request_id_redacted` must never contain secrets or complete session ids. It may be omitted.
4. `changed_packages` is capped to 25 entries unless the caller explicitly requests fewer.
5. `raw_payload_logged` is always `false` in this slice.

---

## 5. Implementation Plan

1. Extend `FMonolithActionExecutionGuard::FExecutionScope` with optional request metadata and a normalized outcome field.
2. Add a separate bounded `FToolCallRecord` array or evolve `FAuditRow` only if `list_recent_action_audit` can keep its old shape.
3. Add `GetToolCallRecordsJson`, `GetToolCallRecordJson`, and `AnalyzeToolCallRecordsJson` methods.
4. Register the three new actions in `MonolithExecutionGuardActions.cpp` only when `UMonolithSettings::bEnableAdvancedToolCallRecords` is true.
5. Keep status fields honest:
   - disabled: `configured=false`, `active=false`, `reason="advanced_tool_call_records_disabled"`;
   - enabled: `configured=true`, `active=true`, `storage="bounded_memory"`, `raw_payload_logging=false`.
6. Add automation tests that execute one success action and one invalid/profile-blocked action, then verify the ledger row shape and redaction flags.

---

## 6. Verification Gates

| Gate | Required evidence |
|------|-------------------|
| Disabled by default | With `bEnableAdvancedToolCallRecords=false`, the three ToolCall actions are not registered or return a disabled status, and existing audit actions still work. |
| Success row | A successful action records `outcome="success"`, duration, action name, and no raw payload. |
| Error row | A failing action records `outcome="error"` and a stable `error_code` when one exists. |
| Dirty package row | A mutating action records package deltas and truncates package lists above the cap. |
| Analysis row | Aggregate analysis returns counts by `outcome`, slowest actions, and most common failing actions without raw payload data. |
| Backward compatibility | `monolith.list_recent_action_audit` keeps its current fields. |

---

## 7. Follow-up Slices

| Follow-up | Reason to defer |
|-----------|-----------------|
| Persistent ledger storage | Requires retention policy and privacy review. |
| MCP session/request correlation | Belongs with the session/progress/cancel transport work. |
| Automatic rollback reports | Requires registry policy metadata and transaction integration. |
| Export/import of records | Security-sensitive; must stay opt-in and redacted. |
