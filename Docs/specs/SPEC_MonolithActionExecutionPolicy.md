# Monolith Action Execution Policy Metadata

**Parent:** [SPEC_MonolithCore.md](SPEC_MonolithCore.md)
**Engine:** Unreal Engine 5.7+
**Status:** Accepted first slice
**Owner module:** MonolithCore
**Scope:** Add registry-level execution policy metadata to action discovery and audit rows without enabling automatic transaction wrapping or rollback.

---

## 1. ROI Queue Position

| Candidate | Current state | ROI | Order |
|-----------|---------------|-----|-------|
| Payload-free action audit | Implemented by `FMonolithActionExecutionGuard`. | High: local diagnostics with changed package deltas. | Prior slice |
| Execution policy metadata | Missing from the registry contract. | High: lets agents and future guard code distinguish default fast-path behavior from explicit mutating policies. | This slice |
| Policy-driven transaction wrapping | Not implemented. | High but risky: changes mutating action behavior. | Later |
| Post-edit validation and rollback | Not implemented. | High but requires per-domain validators. | Later |

---

## 2. Problem

Monolith already routes every action through a central registry and audit guard, but the registry cannot describe what execution policy an action expects. Without a policy field, future rollback, transaction, and validator work must either guess from action names or patch every namespace at once.

---

## 3. First Slice Contract

`FMonolithActionInfo` carries an `execution_policy` object with these fields:

| Field | Type | Meaning |
|-------|------|---------|
| `policy_id` | string | Policy name. The first default is `read_only`. |
| `defaulted` | bool | `true` when the action did not explicitly declare a policy. |
| `dirty_package_tracking` | bool | Whether future policy wiring should track package deltas for this action. |
| `transaction_wrapping` | bool | Whether future policy wiring should wrap the handler in a UE transaction. |
| `post_edit_validation` | bool | Whether future policy wiring should run validators after a mutation. |
| `enforced` | bool | `false` in this slice. Metadata is discoverable but does not alter dispatch behavior. |

Existing `RegisterAction(...)` call sites remain source-compatible. If no explicit policy is provided, the registry assigns the default metadata:

```json
{
  "policy_id": "read_only",
  "defaulted": true,
  "dirty_package_tracking": false,
  "transaction_wrapping": false,
  "post_edit_validation": false,
  "enforced": false
}
```

The default is a fast-path guard behavior, not a final authoritative claim that every legacy action is side-effect free. Explicit mutating policies are a later per-domain cleanup.

---

## 4. User-Visible Surfaces

`monolith.discover` and the deferred domain catalog `monolith.describe_domain` include `execution_policy` for every action row.

`monolith.list_recent_action_audit` and advanced ToolCall records include the policy metadata that was active for the dispatched action. Unknown or rejected actions use the same default metadata and keep their existing error behavior.

---

## 5. Non-Goals

- No transaction wrapping in this slice.
- No automatic rollback in this slice.
- No Blueprint/widget post-edit validation in this slice.
- No bulk classification of all mutating actions in this slice.
- No raw param or result payload logging.

---

## 6. Verification Gates

| Gate | Required evidence |
|------|-------------------|
| Backward compatibility | Existing `RegisterAction(...)` call sites compile without changes. |
| Discovery metadata | `monolith.discover({namespace})` returns `execution_policy` on action rows. |
| Domain metadata | `monolith.describe_domain({namespace})` returns the same policy object. |
| Audit metadata | Recent action audit and advanced records expose policy fields without raw payloads. |
| No behavior change | Guard status still reports transaction wrapping, rollback, and post-edit validation as disabled. |
