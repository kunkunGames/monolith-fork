# Monolith Action Execution Policy Metadata

**Parent:** [SPEC_MonolithCore.md](SPEC_MonolithCore.md)
**Engine:** Unreal Engine 5.7+
**Status:** Accepted second slice
**Owner module:** MonolithCore
**Scope:** Add registry-level execution policy metadata plus opt-in dirty-package tracking, transaction wrapping, and developer policy overrides without enabling automatic rollback or post-edit validation.

---

## 1. ROI Queue Position

| Candidate | Current state | ROI | Order |
|-----------|---------------|-----|-------|
| Payload-free action audit | Implemented by `FMonolithActionExecutionGuard`. | High: local diagnostics with changed package deltas. | Prior slice |
| Execution policy metadata | Implemented in the registry contract. | High: lets agents and guard code distinguish default fast-path behavior from explicit mutating policies. | Prior slice |
| Policy-driven dirty tracking and transaction wrapping | Metadata exists, but dispatch still treats every action the same. | High: removes package-scan overhead from read-only actions and gives mutating actions a central transaction boundary. | This slice |
| Developer policy override | `monolith.set_action_execution_policy` exists as an unavailable placeholder. | High: allows focused local testing and staged per-domain cleanup without touching every registration at once. | This slice |
| Post-edit validation and rollback | Not implemented. | High but requires per-domain validators. | Later |

---

## 2. Problem

Monolith already routes every action through a central registry and audit guard, and the registry can now describe each action's execution policy. The remaining high-ROI gap is that the guard still scans dirty packages for every action and the developer override action cannot update a policy for local testing. That leaves read-only calls paying mutation overhead while mutating policy experiments still require code edits.

---

## 3. Policy Contract

`FMonolithActionInfo` carries an `execution_policy` object with these fields:

| Field | Type | Meaning |
|-------|------|---------|
| `policy_id` | string | Policy name. The first default is `read_only`. |
| `defaulted` | bool | `true` when the action did not explicitly declare a policy. |
| `dirty_package_tracking` | bool | Whether the central guard should snapshot `/Game` dirty packages before and after handler execution. |
| `transaction_wrapping` | bool | Whether the central dispatch scope should wrap the handler in a UE editor transaction. |
| `post_edit_validation` | bool | Reserved for later validator hooks. Requests for enforced validation are rejected in this slice. |
| `enforced` | bool | `true` only for policies that the central guard actively applies. |

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

The default is a fast-path guard behavior, not a final authoritative claim that every legacy action is side-effect free. Explicit mutating policies are opt-in and can be declared at registration time or set for local testing through `monolith.set_action_execution_policy`.

Supported policy ids:

| Policy id | Dirty tracking | Transaction wrapping | Enforcement |
|-----------|----------------|----------------------|-------------|
| `read_only` | No | No | Fast path; no package scans or transaction. |
| `track_dirty_packages` | Yes | No | Audit rows report package deltas. |
| `transaction_optional` | Yes | Yes | Dispatch opens a UE transaction boundary and records transaction status. |
| `transaction_required` | Yes | Yes | Same central transaction behavior; domain handlers still own domain-specific validation. |

`post_edit_validate` remains reserved until validator hooks exist. A runtime override requesting post-edit validation must fail with a structured error instead of advertising a validator that is not wired.

---

## 4. Runtime Override Contract

`monolith.set_action_execution_policy` is a developer-only local override for a known action. It accepts:

| Param | Type | Required | Notes |
|-------|------|----------|-------|
| `action` | string | yes | Fully qualified action name such as `blueprint.add_node`. |
| `policy` | object | no | Optional policy object. If omitted, the policy resets to `read_only`. |
| `policy.policy_id` | string | no | One of the supported policy ids. |
| `policy.dirty_package_tracking` | bool | no | Optional explicit flag. Must be compatible with `policy_id`. |
| `policy.transaction_wrapping` | bool | no | Optional explicit flag. Must be compatible with `policy_id`. |
| `policy.post_edit_validation` | bool | no | Rejected when true in this slice. |

The action returns a structured success object with `changed=true`, the normalized policy, and the target action. Unknown actions, malformed names, unsupported policy ids, and unavailable validation requests return normal Monolith errors.

---

## 5. User-Visible Surfaces

`monolith.discover` and the deferred domain catalog `monolith.describe_domain` include `execution_policy` for every action row.

`monolith.list_recent_action_audit` and advanced ToolCall records include the policy metadata that was active for the dispatched action. Rows also expose:

| Field | Meaning |
|-------|---------|
| `dirty_package_tracking_status` | `skipped_by_policy` or `tracked_by_policy`. |
| `transaction_status` | `not_requested`, `wrapped_by_policy`, or an error-oriented status if transaction setup cannot run. |
| `rollback_status` | Still explicit unavailable metadata; no automatic rollback is claimed. |

Unknown or rejected actions use the same default metadata and keep their existing error behavior.

---

## 6. Non-Goals

- No automatic rollback in this slice.
- No Blueprint/widget post-edit validation in this slice.
- No bulk classification of all mutating actions in this slice.
- No automatic override persistence across editor restarts.
- No raw param or result payload logging.

---

## 7. Verification Gates

| Gate | Required evidence |
|------|-------------------|
| Backward compatibility | Existing `RegisterAction(...)` call sites compile without changes. |
| Discovery metadata | `monolith.discover({namespace})` returns `execution_policy` on action rows. |
| Domain metadata | `monolith.describe_domain({namespace})` returns the same policy object. |
| Audit metadata | Recent action audit and advanced records expose policy fields without raw payloads. |
| Read-only fast path | Default `read_only` audit rows skip dirty-package scans and transaction wrapping. |
| Runtime override | `monolith.set_action_execution_policy` updates a known action and rejects unsupported validator requests. |
| Transaction metadata | Transaction policies record `transaction_status` without claiming rollback. |
