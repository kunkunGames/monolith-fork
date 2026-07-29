# Monolith — MonolithSourceControl Module

**Parent:** [SPEC_CORE.md](../SPEC_CORE.md)
**Engine:** Unreal Engine 5.7+
**Version:** 0.21.3 (Beta)
**Status:** Editor module, default loading phase

---

## 1. Scope

`MonolithSourceControl` owns the `source_control` namespace and registers exactly 11 actions. It exposes:

- provider-neutral status and file operations through Unreal's active `ISourceControlProvider`;
- bounded Perforce opened-file inspection;
- depot/client/local/package path mapping;
- a shared `MonolithCore` helper for modules that must prepare files before saving.

The module does not edit file contents, submit or shelve changelists, create branches, or infer depot layouts. File and asset mutation remains with the owning domain action after source-control preparation succeeds.

---

## 2. Dependencies and Classes

### 2.1 Module dependencies

| Visibility | Modules | Reason |
|------------|---------|--------|
| Public | `Core`, `CoreUObject`, `Engine`, `MonolithCore` | Registry contract, JSON results, package/object path types |
| Private | `Json`, `JsonUtilities`, `SourceControl`, `UnrealEd` | Provider operations, state inspection, editor package resolution |
| `MonolithCore` private addition | `SourceControl` | Implements the reusable `FMonolithSourceControlUtils` adapter |

### 2.2 Classes

| Class | Responsibility |
|-------|----------------|
| `FMonolithSourceControlModule` | Registers `source_control` during startup and unregisters the namespace during shutdown |
| `FMonolithSourceControlActions` | Owns all 11 action handlers and their parameter schemas |
| `MonolithSourceControlP4` | Validates Perforce arguments, parses `-ztag` records, batches `p4 where`, and reports bounded mapping results |
| `FMonolithSourceControlUtils` | Shared `MonolithCore` path normalization and checkout-or-add preparation for files, package names, packages, and assets |

---

## 3. Ownership and Data Flow

| Step | Owner | Contract |
|------|-------|----------|
| 1. Dispatch | `FMonolithToolRegistry` | Applies aliases and required-field checks, then invokes the registered handler |
| 2. Parameter validation | `FMonolithSourceControlActions` / `MonolithSourceControlP4` | Rejects invalid types, bounds, changelists, and command arguments before provider or process work |
| 3. Path normalization | `FMonolithSourceControlUtils` | Resolves relative paths from `FPaths::ProjectDir()` and converts `/Game` package/object paths to filenames |
| 4. Provider operation | Active `ISourceControlProvider` | Executes status, checkout, add, delete, or revert synchronously |
| 5. Perforce inspection | `p4 -ztag` through `FPlatformProcess::ExecProcess` | Reads opened records or maps depot paths in bounded command batches |
| 6. Response | Action handler | Returns provider identity, per-path state/decision rows, command result, and bounded-count details |

The `files` alias is rewritten to the canonical `paths` field by the registry. Both fields accept either a single non-empty string or a non-empty string array.

---

## 4. Action Contract

| Action | Parameters | Behavior |
|--------|------------|----------|
| `get_capabilities` | none | Returns active provider identity, installed provider names, and the 11 supported action names |
| `get_status` | `paths` or `files` | Returns normalized paths and source-control state fields when the provider is available |
| `checkout` | `paths`/`files`, `dry_run=false` | Checks files out through the active provider; dry-run reports state without executing |
| `add` | `paths`/`files`, `dry_run=false` | Marks files for add; dry-run reports state without executing |
| `checkout_or_add` | `paths`/`files`, `dry_run=false` | Checks out source-controlled files and adds eligible local or not-yet-created files |
| `delete` | `paths`/`files`, `dry_run=false`, `confirm=false` | Marks files for delete; execution requires `confirm=true` |
| `mark_for_delete` | same as `delete` | Explicitly named alias using the same provider operation and confirmation contract |
| `revert` | `paths`/`files`, `dry_run=false`, `confirm=false` | Reverts files; execution requires `confirm=true` |
| `revert_unchanged` | `paths`/`files`, `dry_run=false`, `confirm=false` | Reverts unchanged files; execution requires `confirm=true` |
| `list_opened` | `changelist?`, `resolve_packages=true`, `limit=200` | Runs a bounded `p4 -ztag opened`, optionally maps depot records to local and package paths |
| `map_depot_paths` | `paths` or `files` | Maps depot/client/local/package inputs and returns one result row per input |

All boolean fields are strict JSON booleans. Quoted strings, numbers, and explicit `null` values are invalid. The `limit` field must be an integral JSON number in `1..5000`; `changelist` must be omitted, `default`, or an ASCII-decimal string.

---

## 5. Mutation and Provider Contract

| Situation | Result |
|-----------|--------|
| `dry_run=true` | Reports normalized paths and provider states/decisions without executing a provider mutation |
| Delete or revert family without `confirm=true` | Returns a successful transport result with `ok=false` and a confirmation message; no provider operation runs |
| Provider disabled or unavailable | Returns provider details with `available=false`; no alternate provider, file-attribute edit, or local substitute is attempted |
| Provider operation succeeds/fails/cancels | Returns the explicit command result plus provider messages and refreshed states |
| Mixed valid/invalid path array | Preserves a row for each input; execution proceeds only when at least one valid normalized file remains |

`FMonolithSourceControlUtils::CheckoutOrAddFiles` is the shared preparation path for other modules. Its options independently control dry-run, unavailable-provider handling, and whether not-yet-created files may be planned for add.

---

## 6. Bounded Perforce Contract

| Bound | Value | Enforcement |
|-------|-------|-------------|
| Raw input paths | 5,000 | Rejected before any process launch |
| Paths per `p4 where` command | 128 | Batch builder |
| Encoded argument characters per command | 24,000 | Batch builder |
| `p4 where` commands per call | 40 | Batch planner |
| `list_opened` result limit | 1..5,000 | Integral parameter validation |
| Opened-result probe | `limit + 1` | One sentinel row distinguishes exact counts from lower bounds |

`list_opened` reports `observed_count`, `returned_count`, `sentinel_record_count`, `count_is_lower_bound`, `has_more`, and `truncated`; callers must not treat the bounded observation as an exact depot-wide total when `count_is_lower_bound=true`.

`map_depot_paths` de-duplicates depot queries for process efficiency while preserving input order and one output row per raw input. The last matching `p4 where` record wins, including exclusion/overlay records. Control characters are rejected before command construction, and Windows arguments use explicit quoting.

---

## 7. Error Contract

| Error class | JSON-RPC code | Examples |
|-------------|---------------|----------|
| Invalid parameters | `-32602` | Wrong boolean type, invalid `paths`, non-integral/out-of-range `limit`, malformed changelist, excessive path count |
| Backend/process failure | `-32603` | `p4` executable cannot start or returns an unhandled failure |
| Provider unavailable | Successful action envelope with `available=false` | Unreal provider disabled or disconnected |

Input errors identify the rejected field. The module never silently converts string or numeric values into booleans and never substitutes a guessed changelist or depot path.

---

## 8. Extension Points

| Extension | Required location |
|-----------|-------------------|
| New provider-neutral verb | Add a handler and schema in `MonolithSourceControlActions`, register it in the same namespace, and add focused automation coverage |
| New Perforce read primitive | Add validation/parsing/batching to `MonolithSourceControlP4` first, then keep the action handler as orchestration |
| Another module needs pre-save source-control preparation | Reuse `FMonolithSourceControlUtils`; do not duplicate provider/path logic |
| New public action or parameter | Update this spec, `Docs/API_REFERENCE.md`, and `Skills/unreal-source-control/SKILL.md` in the same change |

---

## 9. Verification Gates

| Gate | Required evidence |
|------|-------------------|
| Catalog | Generated registry scan contains exactly 11 new `source_control.*` actions and no unrelated action delta |
| Parameter contract | `Monolith.SourceControl.ParamValidation.*` passes strict-type, alias, registration, and path-mapping cases |
| Perforce batching | `Monolith.SourceControl.P4WhereBatch.*` passes parsing, bounds, quoting, batching, sentinel, and mapping-order cases |
| Compile floor | Fresh linked Editor builds pass on Unreal Engine 5.7 and 5.8 |
| Repository checks | Static checks, `git diff --check`, and a final changed-file audit pass |
| Visual proof | Not applicable: this module has no rendered UI, gameplay, asset-presentation, or editor-panel surface |
