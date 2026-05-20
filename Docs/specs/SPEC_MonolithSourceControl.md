# Monolith - MonolithSourceControl Module

**Parent:** [SPEC_CORE.md](../SPEC_CORE.md)
**Engine:** Unreal Engine 5.7+
**Version:** 0.14.10
**Status:** Implemented

---

## 1. Purpose

`MonolithSourceControl` owns the `source_control` namespace for Unreal SourceControl-provider status and guarded file prepare/revert operations. `MonolithCore` owns the shared `FMonolithSourceControlUtils` helper so explicit source-control actions and automatic asset mutation prepare use the same path normalization and checkout/add decisioning.

---

## 2. Ownership

| Class | Responsibility |
|-------|----------------|
| `FMonolithSourceControlUtils` (`MonolithCore`) | Shared provider availability, package/filesystem path normalization, and checkout-or-add decision/execution helper. |
| `FMonolithSourceControlModule` | Registers and unregisters the `source_control` namespace. |
| `FMonolithSourceControlActions` | Exposes explicit source-control actions and delegates `checkout_or_add` to the shared core helper. |

| Dependency | Purpose |
|------------|---------|
| `MonolithCore` | Tool registry, action result, parameter schema contracts, and shared source-control prepare helper. |
| `SourceControl` | Active Unreal source-control provider, state, and operations. |
| `Engine`, `UnrealEd` | Project path/package path resolution in editor builds. |
| `Json`, `JsonUtilities` | Action response payloads. |

---

## 3. Action Surface

| Action | Params | Description |
|--------|--------|-------------|
| `source_control.get_capabilities` | none | Reports provider identity, enabled/available state, available provider names, and supported action names. |
| `source_control.get_status` | `paths` | Returns normalized path rows and provider state rows when the provider is available. |
| `source_control.checkout` | `paths`, `dry_run`? | Checks out files through the active provider, or returns current states for dry-runs. |
| `source_control.add` | `paths`, `dry_run`? | Marks files for add through the active provider, or returns current states for dry-runs. |
| `source_control.checkout_or_add` | `paths`, `dry_run`? | Chooses checkout/add/skip per file based on provider state, then executes the needed operations unless dry-run. |
| `source_control.revert` | `paths`, `dry_run`?, `confirm`? | Reverts files. Requires `confirm=true` unless `dry_run=true`. |
| `source_control.revert_unchanged` | `paths`, `dry_run`?, `confirm`? | Reverts unchanged files. Requires `confirm=true` unless `dry_run=true`. |

`source_control.checkout_or_add` and the central action execution guard both use `FMonolithSourceControlUtils::CheckoutOrAddFiles`. Existing source-controlled files are checked out; local files are marked for add; already checked-out or added files are skipped. Explicit `source_control.checkout_or_add` allows add planning for missing package filenames to preserve its manual prepare behavior. Automatic asset mutation prepare skips missing files before the handler runs, then retries after the handler succeeds so newly saved `.uasset` and `.umap` files can be marked for add. Automatic prepare is scoped to asset-mutation namespaces/actions and project-owned package files; read-only project/source/context/catalog calls are excluded even if legacy policy inference marks them as mutating.

---

## 4. Routing Validation Contract

All seven `source_control` actions opt into registry-level top-level parameter validation via `FParamSchemaBuilder::EnableValidation()`. The registry rejects malformed `paths`, `dry_run`, and `confirm` types before provider state queries or source-control operations run.

| Action | Registry-owned validation | Handler-owned validation |
|--------|---------------------------|--------------------------|
| `get_capabilities` | Empty typed schema. | Provider inventory and capability projection. |
| `get_status` | `paths` must be an array. | Non-empty path array, string element validation, package/filesystem path normalization, provider availability. |
| `checkout` | `paths` array; `dry_run` bool. | Path normalization, provider availability, checkout execution/state rows. |
| `add` | `paths` array; `dry_run` bool. | Path normalization, provider availability, mark-for-add execution/state rows. |
| `checkout_or_add` | `paths` array; `dry_run` bool. | State-based checkout/add decisioning and operation result aggregation. |
| `revert` | `paths` array; `dry_run` bool; `confirm` bool. | Confirm gate, path normalization, provider availability, revert execution/state rows. |
| `revert_unchanged` | `paths` array; `dry_run` bool; `confirm` bool. | Confirm gate, path normalization, provider availability, revert-unchanged execution/state rows. |

Focused coverage: `FMonolithSourceControlTypedParamsTest`.

---

## 5. Safety Contract

| Gate | Requirement |
|------|-------------|
| Provider boundary | Actions use Unreal's active `ISourceControlProvider`; they do not shell out to P4, git, or external CLIs. |
| Path boundary | Handlers normalize filesystem and `/Game` package/object paths before provider calls and report invalid entries per row. |
| Mutation preview | `checkout`, `add`, `checkout_or_add`, `revert`, and `revert_unchanged` support `dry_run=true`; revert operations require either `confirm=true` or dry-run. |
| Result shape | Actions return provider metadata, normalized path rows, operation booleans/results, messages/errors, and state rows only; no file contents or credential material are returned. |
| Automatic mutation prepare | Asset-mutation actions route through `FMonolithActionExecutionGuard`, which calls the same helper before and after handlers when dirty-package tracking is active. Provider unavailable/disabled is a non-fatal skip for this automatic path. Project/source/context/collection/system namespaces and non-project paths are ignored. |

---

## 6. Verification Gates

| Gate | Evidence |
|------|----------|
| Registration | `FMonolithSourceControlModule::StartupModule` registers the `source_control` namespace. |
| Parameter guard | `FMonolithSourceControlTypedParamsTest` verifies malformed path/bool requests are rejected by the registry. |
| UE 5.7 build | Full plugin UBT build must succeed with the engine root resolved from `GO.uproject`. |
| Optional-off build | Full plugin UBT build must also pass with `MONOLITH_RELEASE_BUILD=1`, even though SourceControl itself has no optional compile guard. |
