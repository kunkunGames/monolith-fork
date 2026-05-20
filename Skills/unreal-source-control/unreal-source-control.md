---
name: unreal-source-control
description: "Use for source control (Perforce/Git) operations from the editor via Monolith MCP: provider status, file/asset status, checkout, mark for add, revert, submit/changelists, and history. Triggers on source control, perforce, p4, git, changelist, checkout, mark for add, revert, submit, diff, file history, revision, depot."
---

# unreal-source-control

**7 actions** via `source_control_query(action, params)`. Action names below are the live registry surface; call `monolith_discover` for exact parameter schemas.

## Discovery

```
monolith_discover({ namespace: "source_control" })                      # all actions in this namespace
monolith_discover({ namespace: "source_control", action: "<action>", mode: "schema" })  # exact params
```

## Action Reference

### Core (7)

| Action | Purpose |
|--------|---------|
| `add` | Mark files for add through the active Unreal source-control provider. |
| `checkout` | Check out files through the active Unreal source-control provider. |
| `checkout_or_add` | Prepare files for mutation by checking out existing source-controlled files or adding local files. |
| `get_capabilities` | Return the active Unreal source-control provider and Phase 1 Monolith action capabilities. |
| `get_status` | Return source-control status for filesystem or /Game package paths. |
| `revert` | Revert files through the active Unreal source-control provider. Requires confirm=true unless dry_run=true. |
| `revert_unchanged` | Revert unchanged files through the active Unreal source-control provider. Requires confirm=true unless dry_run=true. |

## Notes

- This reference is generated from the live `RegisterAction` surface. If an action is missing or renamed, re-run `monolith_discover({ namespace: "source_control" })` — the catalog is the source of truth.
- Pass `mode: "schema"` to `monolith_discover` for required/optional params and types before calling an action.
