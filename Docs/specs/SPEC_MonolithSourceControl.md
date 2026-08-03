# Monolith - MonolithSourceControl Module

**Parent:** [SPEC_CORE.md](../SPEC_CORE.md)
**Engine:** Unreal Engine 5.7+
**Version:** 0.14.11
**Status:** Implemented

---

## 1. Purpose

`MonolithSourceControl` owns the `source_control` namespace for Unreal SourceControl-provider status, guarded file prepare/delete/revert operations, and read-only Perforce opened/path mapping. `MonolithCore` owns the shared `FMonolithSourceControlUtils` helper so explicit source-control mutation actions and automatic asset mutation prepare use the same path normalization and checkout/add decisioning.

---

## 2. Ownership

| Class | Responsibility |
|-------|----------------|
| `FMonolithSourceControlUtils` (`MonolithCore`) | Shared provider availability, package/filesystem path normalization, and checkout-or-add decision/execution helper. |
| `FMonolithSourceControlModule` | Registers and unregisters the `source_control` namespace. |
| `FMonolithSourceControlActions` | Exposes explicit source-control actions, delegates `checkout_or_add` to the shared core helper, and provides read-only `p4 -ztag opened/where` mapping helpers for changelist-to-package workflows. |
| `MonolithSourceControlP4` private helpers | Encode Windows process arguments with Microsoft CRT backslash/quote rules, reject control characters, validate decimal/default changelists, build bounded `p4 opened -m (limit + 1)` commands, project honest observed/sentinel count semantics, and resolve at most 5,000 raw/unique depot paths through at most 40 `p4 where` commands. Tagged records are associated only by explicit depot/client/path identity; later client-view records, row-local errors, and aggregate counters remain deterministic. |

| Dependency | Purpose |
|------------|---------|
| `MonolithCore` | Tool registry, action result, parameter schema contracts, and shared source-control prepare helper. |
| `SourceControl` | Active Unreal source-control provider, state, and operations. |
| `Engine`, `UnrealEd` | Project path/package path resolution in editor builds. |
| `Json`, `JsonUtilities` | Action response payloads. |
| External `p4` command | Read-only opened-file and depot/client/local path mapping for `list_opened` and `map_depot_paths`; unavailable CLI returns structured errors. |

---

## 3. Action Surface

| Action | Params | Description |
|--------|--------|-------------|
| `source_control.get_capabilities` | none | Reports provider identity, enabled/available state, available provider names, and supported action names. |
| `source_control.get_status` | `paths` | Returns normalized path rows and provider state rows when the provider is available. |
| `source_control.checkout` | `paths`, `dry_run`? | Checks out files through the active provider, or returns current states for dry-runs. |
| `source_control.add` | `paths`, `dry_run`? | Marks files for add through the active provider, or returns current states for dry-runs. |
| `source_control.checkout_or_add` | `paths`, `dry_run`? | Chooses checkout/add/skip per file based on provider state, then executes the needed operations unless dry-run. |
| `source_control.delete` | `paths`, `dry_run`?, `confirm`? | Marks files for delete through the active provider. Requires `confirm=true` unless `dry_run=true`. |
| `source_control.mark_for_delete` | `paths`, `dry_run`?, `confirm`? | Explicit alias for provider mark-for-delete. Requires `confirm=true` unless `dry_run=true`. |
| `source_control.revert` | `paths`, `dry_run`?, `confirm`?, `delete_new_files`? | Reverts files. Requires `confirm=true` unless `dry_run=true`. `delete_new_files=false` is the action-owned default, so files opened for add are preserved unless their deletion is requested explicitly. |
| `source_control.revert_unchanged` | `paths`, `dry_run`?, `confirm`? | Reverts unchanged files. Requires `confirm=true` unless `dry_run=true`. |
| `source_control.list_opened` | `changelist`?, `resolve_packages`?, `limit`? | Runs `p4 -ztag opened -m (limit + 1)` with `limit` in `[1, 5000]`. It returns at most `limit` rows plus honest bounded observation fields: `observed_count`, `sentinel_record_count`, `has_more`, `truncated`, `count_semantics`, and `count_is_lower_bound`. Backward `count` equals the bounded observed count and is exact only when `count_is_lower_bound=false`; no unbounded exact-count query runs. Selected depot paths are then mapped through the shared bounded `p4 where` resolver. |
| `source_control.map_depot_paths` | `paths` | Maps at most 5,000 raw inputs across Perforce depot/client, local filesystem, and `/Game` paths. Depot/client paths are deduplicated, individually capped by the actual encoded command length, and resolved through at most 40 commands of at most 128 paths / 24,000 encoded characters each. Checkout-relative inputs resolve from `FPaths::ProjectDir`; mapping diagnostics expose raw, requested, unique, resolved, failed, and command counts. |

Canonical calls should use `paths` as an array of filesystem or `/Game` paths. For additive agent-input tolerance, every `source_control` action that takes `paths` also accepts a single non-empty string and the alias key `files`; handlers normalize both forms to the same path row contract. `map_depot_paths` rejects raw arrays above 5,000 before any process launch, including 5,001 duplicate entries, and rejects NUL/CR/LF/tab/other control characters before trimming or quoting. Optional boolean fields (`dry_run`, `confirm`, `resolve_packages`, and `delete_new_files`) accept booleans plus string literals `true`, `false`, `1`, `0`, `yes`, `no`, `on`, and `off`; malformed strings and numeric booleans remain validation errors. Destructive delete/revert actions still require `confirm=true` unless `dry_run=true`.

`list_opened` and `map_depot_paths` register an explicit `read_only` execution policy. This prevents action-name inference from classifying `map_depot_paths` as a transaction-wrapped asset mutation even though it only invokes read-only `p4 where` queries and package-name conversion.

`source_control.checkout_or_add` and the central action execution guard both use `FMonolithSourceControlUtils::CheckoutOrAddFiles`. Existing source-controlled files are checked out; local files are marked for add; already checked-out or added files are skipped. Explicit `source_control.checkout_or_add` allows add planning for missing package filenames to preserve its manual prepare behavior. Automatic asset mutation prepare skips missing files before the handler runs, then retries after the handler succeeds so newly saved `.uasset` and `.umap` files can be marked for add. Automatic prepare is scoped to asset-mutation namespaces/actions and project-owned package files; read-only project/source/bridge/context/catalog calls are excluded even if legacy policy inference marks them as mutating.

---

## 4. Routing Validation Contract

All eleven `source_control` actions opt into registry-level top-level parameter validation via `FParamSchemaBuilder::EnableValidation()`. The registry rejects malformed `paths`, `dry_run`, `confirm`, `changelist`, `resolve_packages`, and `limit` types before provider state queries, source-control operations, or P4 mapping commands run. The only supported tolerance is the explicit source-control contract above: `files` aliases to `paths`, scalar path strings wrap to a one-item list, and known boolean strings are parsed by the source-control handlers.

| Action | Registry-owned validation | Handler-owned validation |
|--------|---------------------------|--------------------------|
| `get_capabilities` | Empty typed schema. | Provider inventory and capability projection. |
| `get_status` | `paths` array/string with `files` alias. | Non-empty path value validation, package/filesystem path normalization, provider availability. |
| `checkout` | `paths` array/string with `files` alias; `dry_run` bool/string. | Path normalization, tolerant bool parsing, provider availability, checkout execution/state rows. |
| `add` | `paths` array/string with `files` alias; `dry_run` bool/string. | Path normalization, tolerant bool parsing, provider availability, mark-for-add execution/state rows. |
| `checkout_or_add` | `paths` array/string with `files` alias; `dry_run` bool/string. | Tolerant bool parsing, state-based checkout/add decisioning, and operation result aggregation. |
| `delete` | `paths` array/string with `files` alias; `dry_run` bool/string; `confirm` bool/string. | Tolerant bool parsing, confirm gate, path normalization, provider availability, provider delete execution/state rows. |
| `mark_for_delete` | `paths` array/string with `files` alias; `dry_run` bool/string; `confirm` bool/string. | Tolerant bool parsing, confirm gate, path normalization, provider availability, provider delete execution/state rows. |
| `revert` | `paths` array/string with `files` alias; `dry_run` bool/string; `confirm` bool/string; `delete_new_files` bool/string default false. | Tolerant bool parsing, confirm gate, path normalization, provider availability, revert execution/state rows, explicit added-file deletion policy, and scoped restoration of the editor-global source-control preference. |
| `revert_unchanged` | `paths` array/string with `files` alias; `dry_run` bool/string; `confirm` bool/string. | Tolerant bool parsing, confirm gate, path normalization, provider availability, revert-unchanged execution/state rows. |
| `list_opened` | `changelist` string; `resolve_packages` bool/string; `limit` integer in `[1, 5000]`. | `changelist` is empty, `default`, or ASCII decimal only; control characters fail before execution. The backend receives `-m (limit + 1)` with a hard maximum of 5,001 records. Returned rows exclude the one sentinel, and count fields explicitly distinguish exact from lower-bound observations. |
| `map_depot_paths` | `paths` array/string with `files` alias. | Rejects more than 5,000 raw inputs and all control characters before execution; deduplicates depot/client inputs; enforces 5,000 unique inputs, 128 paths / 24,000 actual encoded characters per command, and 40 total commands; preserves failures per row; resolves checkout-relative paths from the project root; emits absolute normalized local paths for package inputs. |

Focused coverage: `FMonolithSourceControlTypedParamsTest`.

---

## 5. Safety Contract

| Gate | Requirement |
|------|-------------|
| Provider boundary | Mutation actions use Unreal's active `ISourceControlProvider`; they do not shell out to P4, git, or external CLIs. `revert` scopes `USourceControlPreferences::bShouldDeleteNewFilesOnRevert` to the explicit `delete_new_files` request for the synchronous provider call and restores the prior editor value before returning. |
| Perforce mapping boundary | `list_opened` and `map_depot_paths` are read-only Perforce CLI wrappers. `list_opened` is backend-bounded by `-m limit+1`; `map_depot_paths` is bounded by raw/unique path, actual encoded command-length, per-command path, and aggregate command caps. Windows argv uses CRT-compatible backslash/quote encoding, and command/control validation occurs before process launch. The wrappers do not place credentials on the command line and return structured command errors when `p4` is unavailable or the workspace is unmapped. |
| Path boundary | Handlers normalize filesystem and `/Game` package/object paths before provider calls and report invalid entries per row. |
| Mutation preview | `checkout`, `add`, `checkout_or_add`, `delete`, `mark_for_delete`, `revert`, and `revert_unchanged` support `dry_run=true`; delete/revert operations require either `confirm=true` or dry-run. |
| Result shape | Actions return provider metadata, normalized path rows, operation booleans/results, messages/errors, and state rows only; no file contents or credential material are returned. |
| Automatic mutation prepare | Asset-mutation actions route through `FMonolithActionExecutionGuard`, which calls the same helper before and after handlers when dirty-package tracking is active. Provider unavailable/disabled is a non-fatal skip for this automatic path. Project/source/bridge/context/collection/system namespaces and non-project paths are ignored. |

---

## 6. Verification Gates

| Gate | Evidence |
|------|----------|
| Registration | `FMonolithSourceControlModule::StartupModule` registers the `source_control` namespace. |
| Parameter guard and policy | `FMonolithSourceControlTypedParamsTest` verifies malformed path/bool requests, non-integral limits, and limits outside `[1, 5000]` are rejected while both Perforce query actions remain explicitly `read_only`. A SourceControl-specific RAII fixture restores the production namespace after isolation, so running automation cannot remove live actions from the editor. |
| Input tolerance and path roots | `FMonolithSourceControlInputToleranceTest` verifies `files` aliases, scalar path strings, supported boolean literals, project-root-relative resolution, and absolute `/Game` filename projection without executing mutations. |
| Batched P4 mapping | `FMonolithSourceControlP4WindowsCommandLineTest` independently parses encoded arguments and covers spaces, literal quotes, quote-adjacent backslash runs, trailing backslashes, control rejection, and actual encoded-length bounds. `FMonolithSourceControlP4OpenedBoundsTest` verifies `-m limit+1`, decimal/default changelist validation, maximum backend 5,001, and exact/lower-bound sentinel windows. `FMonolithSourceControlP4BatchScaleTest` verifies 100/1,000/2,000/5,000 paths use 1/8/16/40 commands, rejects 5,001 raw duplicates or unique paths and a forty-first command before dispatch, preserves character bounds, and rejects one oversized argument. Partial-failure, view-order, and row-local-stderr tests retain association and attribution coverage. |
| P4 mapping | Manual or automation smoke should verify `source_control.list_opened(changelist)` maps opened `.uasset`/`.umap` files to `/Game` package paths and that `source_control.map_depot_paths(paths)` handles depot, local, and package inputs without mutation. |
| UE 5.7 build | Full plugin UBT build must succeed with the engine root resolved from the host `.uproject`. |
| Optional-off build | Full plugin UBT build must also pass with `MONOLITH_RELEASE_BUILD=1`, even though SourceControl itself has no optional compile guard. |
