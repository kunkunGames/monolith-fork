---
name: unreal-source-control
description: Use when running source control (Perforce/Git) operations from the editor via Monolith MCP (source_control) - provider/capabilities status, file or /Game asset status, checkout, mark for add, mark for delete, revert/revert unchanged, plus Perforce opened/changelist and depot/local/package path mapping. This skill owns the P4/Git checkout/add/delete/revert and read-only opened/path-mapping actions, not the thing being versioned. To edit the .ini content after a checkout use unreal-config; to save/move/delete the asset package itself use unreal-asset; when group/collection means an editor asset Collection not a changelist use unreal-collection. Triggers on source control, perforce, p4, git, changelist, CL, checkout, check out file, mark for add, mark for delete, revert, revert unchanged, file status, depot, revision, who has this checked out, prepare file for edit, add to depot.
---

# unreal-source-control

**11 actions** via `source_control_query(action, params)`. Action names below are the live registry surface; call `monolith_discover` for exact parameter schemas.

## Discovery

```
monolith_discover({ namespace: "source_control" })                      # all actions in this namespace
monolith_discover({ namespace: "source_control", action: "<action>", mode: "schema" })  # exact params
```

When the right action is unclear, `monolith_find("<task>")` suggests candidates across namespaces.

## When to use / Use a different skill for

- Use this skill for the source-control ACTION itself — provider/capabilities status, file/asset status, checkout, mark for add, mark for delete, revert, revert-unchanged through the active Unreal provider (Perforce or Git), plus read-only Perforce opened/changelist and path mapping.
- **unreal-build / editor validation actions** — to run validation or build review after mapping a changelist. `source_control` can list/map opened files; `editor.plan_content_validation_changeset` and `editor.validate_changeset_assets` turn those mapped rows into DataValidation targets.
- **unreal-config** — to read or edit the `.ini` setting/section/cvar content after this skill checks the file out; this skill owns the checkout/add, not the edit.
- **unreal-asset** — to actually save/move/delete the asset package itself; this skill only marks files for add/delete/checkout in source control.
- **unreal-collection** — when "group" or "collection" means a Content Browser asset Collection, not a source-control changelist or depot grouping.

## Action Reference

Param notation: `name*` required, `name?` optional, `name=val` default, `a/b/c` allowed, `[w]` mutates. Signatures are a snapshot of the live catalog — for the exact full schema call `monolith_discover` with `mode: "schema"`. Keep the discover-first block above as the authority. The canonical mutation param is `paths` (array of filesystem or `/Game` package/object paths); the live schema also accepts a single path string and the alias key `files` for compatibility with natural agent input.

### Core (11)

| Action | Params | Purpose |
|--------|--------|---------|
| `get_capabilities` | _(none)_ | Return the active Unreal source-control provider and Phase 1 Monolith action capabilities. |
| `get_status` | `paths*` or `files*` | Return source-control status for filesystem or /Game package paths. |
| `[w] checkout` | `paths*` or `files*`, `dry_run?=false` | Check out files through the active Unreal source-control provider. |
| `[w] add` | `paths*` or `files*`, `dry_run?=false` | Mark files for add through the active Unreal source-control provider. |
| `[w] checkout_or_add` | `paths*` or `files*`, `dry_run?=false` | Prepare files for mutation by checking out existing source-controlled files or adding local files. |
| `[w] delete` | `paths*` or `files*`, `confirm?=false`, `dry_run?=false` | Mark files for delete. Requires `confirm=true` unless `dry_run=true`. |
| `[w] mark_for_delete` | `paths*` or `files*`, `confirm?=false`, `dry_run?=false` | Explicit mark-for-delete alias of `delete`. Requires `confirm=true` unless `dry_run=true`. |
| `[w] revert` | `paths*` or `files*`, `confirm?=false`, `dry_run?=false`, `delete_new_files?=false` | Revert files. Requires `confirm=true` unless `dry_run=true`. Files opened for add are preserved unless `delete_new_files=true` is explicit. |
| `[w] revert_unchanged` | `paths*` or `files*`, `confirm?=false`, `dry_run?=false` | Revert unchanged files. Requires `confirm=true` unless `dry_run=true`. |
| `list_opened` | `changelist?` `resolve_packages?=true` `limit?=200` (`1..5000`) | Read-only bounded `p4 -ztag opened -m (limit + 1)`, optionally scoped to empty/`default`/ASCII-decimal changelist, with depot-to-local/package mapping and explicit exact-vs-lower-bound count fields. |
| `map_depot_paths` | `paths*` or `files*` (max 5,000 raw entries) | Read-only mapping for depot, client, local filesystem, `/Game` package, or object paths. Uses at most 40 commands, 128 paths and 24,000 encoded characters per command; control characters are rejected before process launch. |

## Common workflows

```text
# Confirm a provider is connected before any mutation.
source_control_query("get_capabilities", {})

# Inspect status for a /Game asset and a filesystem .ini before editing.
source_control_query("get_status", { paths: ["/Game/Maps/Interactable/BP_Wave", "Config/DefaultEngine.ini"] })

# Prepare a file for edit (checkout if tracked, add if new local file).
source_control_query("checkout_or_add", { paths: ["Config/DefaultEngine.ini"] })

# Mark a file for delete — destructive, dry-run first then confirm.
source_control_query("delete", { paths: ["/Game/Old/BP_Legacy"], dry_run: true })
source_control_query("delete", { paths: ["/Game/Old/BP_Legacy"], confirm: true })

# Roll back local changes through the active provider.
source_control_query("revert", { paths: ["/Game/Maps/Interactable/BP_Wave"], confirm: true })

# Inspect a pending changelist and map opened packages without mutating source control.
source_control_query("list_opened", { changelist: "1006", resolve_packages: true })
source_control_query("map_depot_paths", { paths: ["//depot/Game/UI/WBP_Menu.uasset", "/Game/UI/WBP_Menu"] })
```

`list_opened.count` is the bounded observed count, not an implicit exact total. When
`count_is_lower_bound=true`, the extra sentinel proves more rows exist; inspect
`count_semantics`, `observed_count`, `returned_count`, `sentinel_record_count`,
`backend_record_limit`, `has_more`, and `truncated`. Both mapping actions also expose
`mapping_raw_count`, `mapping_requested_count`, `mapping_unique_count`,
`mapping_resolved_count`, `mapping_failed_count`, and `mapping_command_count`.

### Recipe: prepare a file for edit, then drop no-op checkouts

End-to-end prepare-for-edit pass. Confirms the provider, inspects status, checks the file out, then (after the actual content edit happens elsewhere) reverts any file left unchanged so the changelist stays clean. The content edit itself is a separate step — for a `.ini` use **unreal-config**, for an asset package use **unreal-asset**.

```text
# 1. Confirm a provider is connected.
source_control_query("get_capabilities", {})

# 2. Inspect status for the target .ini and asset before touching them.
source_control_query("get_status", { paths: ["Config/DefaultEngine.ini", "/Game/Maps/Interactable/BP_Wave"] })

# 3. Check the files out through the active provider.
source_control_query("checkout", { paths: ["Config/DefaultEngine.ini", "/Game/Maps/Interactable/BP_Wave"] })

# 4. (Edit the .ini in unreal-config / save the asset in unreal-asset here.)

# 5. Drop any checkout that ended up unchanged — dry-run first, then confirm.
source_control_query("revert_unchanged", { paths: ["Config/DefaultEngine.ini", "/Game/Maps/Interactable/BP_Wave"], dry_run: true })
source_control_query("revert_unchanged", { paths: ["Config/DefaultEngine.ini", "/Game/Maps/Interactable/BP_Wave"], confirm: true })
```

### Recipe: mark a new file for add and an old file for delete

End-to-end add/delete flow. `checkout_or_add` adds the new local file (or checks it out if already tracked); `mark_for_delete` is the explicit delete verb, dry-run first then confirmed. (Save/create the new asset package itself in **unreal-asset** before adding it.)

```text
# 1. Add the new local file (checks out instead if already source-controlled).
source_control_query("checkout_or_add", { paths: ["/Game/UI/Icons/T_icon_skill"] })

# 2. Mark the obsolete file for delete — destructive, preview then confirm.
source_control_query("mark_for_delete", { paths: ["/Game/UI/Icons/T_icon_old"], dry_run: true })
source_control_query("mark_for_delete", { paths: ["/Game/UI/Icons/T_icon_old"], confirm: true })

# 3. Confirm the resulting marked-for-add / marked-for-delete state.
source_control_query("get_status", { paths: ["/Game/UI/Icons/T_icon_skill", "/Game/UI/Icons/T_icon_old"] })
```

## Gotchas / Rules

- `delete`, `mark_for_delete`, `revert`, and `revert_unchanged` are destructive — they require `confirm=true` unless you pass `dry_run=true`. Run a `dry_run` first to preview the affected files.
- `revert` does not inherit the editor-global "delete new files on revert" preference. Its action contract defaults `delete_new_files=false`; pass `delete_new_files=true` only when files opened for add should also be removed from disk.
- This skill performs the source-control verb only. Editing the file content (`.ini`, asset package) is a separate step in **unreal-config** / **unreal-asset** after checkout.
- `list_opened` and `map_depot_paths` are explicitly read-only, bounded mapping primitives. `list_opened` never performs an unbounded exact-count query; treat `count` as exact only when `count_is_lower_bound=false`. For validation workflows, prefer `editor.plan_content_validation_changeset` / `editor.validate_changeset_assets` after this layer proves the changelist or path mapping.
- Prefer the canonical `paths` array for file lists. The schema also accepts `files` and a single path string for compatibility, and boolean options accept `true`/`false` booleans plus string literals `true`, `false`, `1`, `0`, `yes`, `no`, `on`, and `off`. `get_status` accepts both filesystem and `/Game` package paths; resolve `/Game` asset paths through the active provider rather than assuming a depot/disk layout.

## Notes

- This reference is generated from the live `RegisterAction` surface. If an action is missing or renamed, re-run `monolith_discover({ namespace: "source_control" })` — the catalog is the source of truth.
- Pass `mode: "schema"` to `monolith_discover` for required/optional params and types before calling an action.
