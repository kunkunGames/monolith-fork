# Monolith Action Execution Policy Metadata

**Parent:** [SPEC_MonolithCore.md](SPEC_MonolithCore.md)
**Engine:** Unreal Engine 5.7+
**Status:** Accepted third slice
**Owner module:** MonolithCore
**Scope:** Add registry-level execution policy metadata plus opt-in dirty-package tracking, transaction wrapping, best-effort source-control prepare, developer policy overrides, and post-edit validator hooks without claiming automatic rollback.

---

## 1. ROI Queue Position

| Candidate | Current state | ROI | Order |
|-----------|---------------|-----|-------|
| Payload-free action audit | Implemented by `FMonolithActionExecutionGuard`. | High: local diagnostics with changed package deltas. | Prior slice |
| Execution policy metadata | Implemented in the registry contract. | High: lets agents and guard code distinguish default fast-path behavior from explicit mutating policies. | Prior slice |
| Policy-driven dirty tracking and transaction wrapping | Metadata exists, but dispatch still treats every action the same. | High: removes package-scan overhead from read-only actions and gives mutating actions a central transaction boundary. | This slice |
| Developer policy override | `monolith.set_action_execution_policy` exists as an unavailable placeholder. | High: allows focused local testing and staged per-domain cleanup without touching every registration at once. | This slice |
| Automatic source-control prepare | Implemented by `FMonolithActionExecutionGuard` using `FMonolithSourceControlUtils`. | High: read-only Perforce assets are checked out before mutation and newly saved assets are marked for add after mutation. | This slice |
| Post-edit validation hooks | Not implemented. | High: catches broken Blueprint/widget graph mutations at dispatch time instead of leaving silent dirty assets. | This slice |
| Automatic rollback reports | Not implemented. | High but requires validated per-domain undo semantics beyond UE transaction recording. | Later |

---

## 2. Problem

Monolith already routes every action through a central registry and audit guard, and the registry can now describe each action's execution policy. Dirty-package tracking, transaction wrapping, local policy overrides, and best-effort source-control prepare are wired. Source-control prepare uses the active Unreal `ISourceControlProvider`: existing project asset files are checked out before mutation, and newly saved project package files are marked for add after mutation. The automatic path is limited to asset-mutation namespaces/actions and skips index/source/collection/system namespaces. The remaining high-ROI gap is post-edit validation: mutating Blueprint and widget actions can finish with a dirty or compiler-error asset, but the central dispatch layer cannot yet run a validation hook or convert that failure into a structured action error.

---

## 3. Policy Contract

`FMonolithActionInfo` carries an `execution_policy` object with these fields:

| Field | Type | Meaning |
|-------|------|---------|
| `policy_id` | string | Policy name. The first default is `read_only`. |
| `defaulted` | bool | `true` when the action did not explicitly declare a policy. |
| `dirty_package_tracking` | bool | Whether the central guard should snapshot project-owned mounted packages before and after handler execution. This includes `/Game` and project/GameFeature plugin mounts such as `/SpeedCore` and `/SpeedBox`, while excluding `/Engine` and external mounts. |
| `transaction_wrapping` | bool | Whether the central dispatch scope should wrap the handler in a UE editor transaction. |
| `post_edit_validation` | bool | Whether the central dispatch scope should run a post-handler validator before returning success. |
| `enforced` | bool | `true` only for policies that the central guard actively applies. |

Existing `RegisterAction(...)` call sites remain source-compatible. If no explicit policy is provided, the registry assigns a default policy. Read-like action names stay on the read-only fast path:

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

The read-only default is a fast-path guard behavior, not a final authoritative claim that every legacy action is side-effect free. Explicit policies are preferred and can be declared at registration time or set for local testing through `monolith.set_action_execution_policy`.

For backward compatibility with existing production registrations, the registry applies conservative inference before storing metadata. Read-like names such as `get`, `list`, `find`, `search`, `read`, `validate`, `preview`, `describe`, `detect`, `analyze`, `compare`, `check`, `status`, `inspect`, `query`, and `resolve` keep the implicit `read_only` fast path. Every other implicit default legacy registration falls forward to `transaction_optional` unless the registration supplies an explicit policy. This keeps editor mutations such as `place_light`, `generate_floor_plan`, `edit_level_instance`, `connect_pins`, and future write verbs under dirty-package tracking and a central transaction boundary without requiring a bulk per-action registration rewrite in this slice.

Supported policy ids:

| Policy id | Dirty tracking | Transaction wrapping | Enforcement |
|-----------|----------------|----------------------|-------------|
| `read_only` | No | No | Fast path; no package scans or transaction. |
| `track_dirty_packages` | Yes | No | Audit rows report package deltas. |
| `transaction_optional` | Yes | Yes | Dispatch opens a UE transaction boundary and records transaction status. |
| Handler-owned transaction exception | Yes | No | `bulk_fill.apply` registers `track_dirty_packages` because each namespace adapter owns one target-specific transaction. This prevents nested transactions while preserving central dirty-package auditing. |
| `transaction_required` | Yes | Yes | Same central transaction behavior; domain handlers still own domain-specific validation. |
| `post_edit_validate` | Yes | Yes | Dispatch opens a transaction, runs a post-handler validator, and converts validator failure into a structured action error. |

`post_edit_validate` is opt-in. It does not imply automatic rollback. If validation fails while a central transaction is open, Monolith cancels the transaction record so the failed edit is not advertised as a valid undo step, returns an action error, and records explicit rollback metadata that no automatic asset revert was performed.

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
| `policy.post_edit_validation` | bool | no | Optional explicit flag. Must be compatible with `policy_id`; true is supported only by `post_edit_validate`. |

The action returns a structured success object with `changed=true`, the normalized policy, and the target action. Unknown actions, malformed names, unsupported policy ids, and unavailable validation requests return normal Monolith errors.

Legacy boolean aliases such as `policy.post_edit_validate` are rejected instead of being ignored. Callers must request validation through `policy.policy_id="post_edit_validate"` plus the normalized `policy.post_edit_validation=true` field when they send explicit flags.

---

## 5. User-Visible Surfaces

`monolith.discover` and the deferred domain catalog `monolith.describe_domain` include `execution_policy` for every action row.

`monolith.list_recent_action_audit` and advanced ToolCall records include the policy metadata that was active for the dispatched action. Rows also expose:

| Field | Meaning |
|-------|---------|
| `dirty_package_tracking_status` | `skipped_by_policy` or `tracked_by_policy`. |
| `transaction_status` | `not_requested`, `wrapped_by_policy`, or an error-oriented status if transaction setup cannot run. |
| `source_control_prepare_status` | Central statuses (`skipped_by_policy`, `no_targets`, `no_change`, `skipped_provider_unavailable`, `prepared_checkout_N_add_N`, `prepare_failed_checkout_N_add_N`) or an explicit `handler_owned_*` status. |
| `post_edit_validation_status` | `not_requested`, `requested_by_policy`, `passed_by_validator`, `failed_by_validator`, `skipped_handler_error`, or a target/validator failure status. |
| `rollback_status` | Still explicit unavailable metadata; no automatic rollback is claimed. |

Unknown or rejected actions use the same default metadata and keep their existing error behavior.

When validation succeeds, successful handler result objects receive a `post_edit_validation` object. When validation fails, the action returns a normal Monolith error with structured `error.data.post_edit_validation`; raw params and raw result payloads are not copied into audit rows.

When an asset-mutation action succeeds, successful handler result objects may receive a `source_control_prepare` object. `before_action` reports pre-handler checkout/add decisions for target-like params such as `asset_path`, `blueprint_path`, `widget_blueprint`, `wbp_path`, `save_path`, `new_path`, `dest_path`, and `new_asset_path`. `after_action` reports decisions for result asset paths and newly dirtied project-owned packages, including project/GameFeature content mounts. Automatic target collection only accepts `.uasset`/`.umap` files that resolve under the project directory, so `/Script`, engine package paths, and external files are ignored. Provider disabled/unavailable states are non-fatal for automatic prepare and are reported as skipped. `source_control`, `project`, `source`, `context`, `collection`, `asset`, and `monolith` namespaces are excluded from this automatic prepare path to avoid recursive or read-only index/source calls.

Actions whose exact package cannot be known from a coarse request path may register through `FMonolithActionExecutionGuard::RegisterHandlerOwnedSourceControlActions`. This keeps policy-driven dirty-package tracking and audit enabled while suppressing the central param/result scan for those action IDs. The owning handler must resolve the live object, derive its exact outermost project package (including external-actor packages), run checkout before its first mutation or requested save, and attach a structured `source_control_prepare` result. Explicit `changed=false, saved=false` calls must not prepare source control. Modules unregister the action set during shutdown so the ownership contract is removable with the module.

Built-in validation covers Blueprint-derived assets returned or addressed by common fields (`asset_path`, `blueprint_path`, `widget_blueprint`, `wbp_path`, `save_path`, `new_path`). Blueprint and widget targets are compiled once with UE's editor compiler and fail validation when the post-compile status is not `BS_UpToDate` or `BS_UpToDateWithWarnings`. Domain modules can also register explicit validators for specific `namespace.action` pairs.

---

## 6. Non-Goals

- No automatic rollback in this slice.
- No best-effort object graph revert beyond canceling a failed central transaction record.
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
| Runtime override | `monolith.set_action_execution_policy` updates a known action and accepts compatible `post_edit_validate` requests. |
| Transaction metadata | Transaction policies record `transaction_status` without claiming rollback. |
| Source-control prepare | Mutating asset actions report checkout/add decisions in `source_control_prepare` and audit `source_control_prepare_status`; unavailable providers are non-fatal skips. |
| Handler-owned exact target | Registered handler-owned actions prepare the resolved outermost package before mutation, do not run the central coarse scan, and leave explicit unsaved no-ops unopened. |
| Validator hook | `post_edit_validate` actions run a post-handler validator and report `post_edit_validation_status`. |
| Validation failure | Validator failure converts the action result to a structured error without raw payload logging or rollback claims. |
