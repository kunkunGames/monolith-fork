---
name: unreal-ndisplay
description: Use to INSPECT nDisplay / DisplayCluster config assets via Monolith MCP (ndisplay namespace) — discover DisplayCluster config-like assets via AssetRegistry and report nDisplay authoring-support status. Read-only; creating, editing, or building nDisplay cluster/viewport/ICVFX configs is NOT exposed by this namespace today (confirm via monolith_discover). For placing the camera and actors on the live ICVFX/LED-wall stage use unreal-scene; for generic project/engine .ini and cvar edits use unreal-config; to drive the virtual-production shot through Sequencer use unreal-level-sequences. Triggers on ndisplay, nDisplay config, DisplayCluster, LED wall, LED volume, ICVFX, in-camera VFX, multi-display, multi-projector, render node, find display cluster config, list nDisplay configs, nDisplay status.
---

# unreal-ndisplay

Read-only inspection of nDisplay / DisplayCluster config assets via the Monolith **`ndisplay`** namespace. **2 actions** via `ndisplay_query(action, params)`, both inspection-only: report authoring-support status and list DisplayCluster config-like assets from AssetRegistry. Creating, editing, or building nDisplay cluster/viewport/ICVFX configs is **not exposed by this namespace today** — verify with `monolith_discover` and hand off stage setup to **unreal-scene**. Action names here are the live registry surface; call `monolith_discover` for exact parameter schemas.

## Discovery

```
monolith_discover({ namespace: "ndisplay" })                      # all actions in this namespace
monolith_discover({ namespace: "ndisplay", action: "<action>", mode: "schema" })  # exact params
```

## When to use

Use this skill to **inspect** the nDisplay / DisplayCluster configuration layer — finding DisplayCluster config-like assets and confirming what nDisplay support this checkout reports. Authoring the cluster/viewport/ICVFX config is not exposed here today.

Use a different skill for:

- **unreal-scene** — placing, moving, or aligning the camera and actors on the live ICVFX/LED-wall stage, or running spatial queries inside the level. This is where on-stage authoring happens once you have inventoried the configs here.
- **unreal-config** — a generic project/engine `.ini` setting or console variable, not the nDisplay DisplayCluster configuration asset.
- **unreal-level-sequences** — driving the virtual-production shot or camera animation through Sequencer, rather than the multi-display cluster setup.

## Action Reference

Param notation: `name*` required, `name?` optional, `name=val` default, `a/b/c` allowed, `[w]` mutates. Signatures are a snapshot of the live catalog — for the exact full schema call `monolith_discover` with `mode: "schema"`. The discover-first block above is the authority.

### Core (2)

| Action | Params | Purpose |
|--------|--------|---------|
| `get_status` | (none) | Report read-only nDisplay/DisplayCluster config authoring support without hard DisplayCluster dependencies. |
| `list_config_assets` | `package_path?=/Game` `limit?=100` | List nDisplay/DisplayCluster config-like assets under /Game using AssetRegistry metadata only (`limit` clamped to 1..500). Does not load, save, or mutate configs. |

## Common Workflows

### Inspect nDisplay support, inventory configs, then hand off stage setup
This namespace cannot author a DisplayCluster config; the recipe inspects, then routes on-stage placement to unreal-scene.

1. `ndisplay_query("get_status")` — confirm nDisplay/DisplayCluster authoring-support status before assuming richer cluster/viewport actions exist. Treat its result as the gate.
2. `ndisplay_query("list_config_assets", { "package_path": "/Game", "limit": 100 })` — inventory DisplayCluster config-like assets under a folder (AssetRegistry metadata only; `limit` clamps 1..500). Record the config object paths.
3. Hand off to **unreal-scene** to place, move, or align the camera and actors on the ICVFX/LED-wall stage for the config you found. Editing the config asset itself is not exposed by this namespace today; report that rather than substituting an action that does not exist.

## Gotchas

- **Read-only namespace.** Both actions are inspection-only — they do not load, create, save, or mutate DisplayCluster configs. To author the on-stage scene, hand off to **unreal-scene**.
- **AssetRegistry metadata only.** `list_config_assets` reads AssetRegistry tags and never loads the config asset, so it is safe without DisplayCluster runtime present.
- **No hard DisplayCluster dependency.** `get_status` reports authoring support even when the DisplayCluster plugin path is partial; treat its result as the gate before assuming richer cluster/viewport actions exist.

## Notes

- This reference is generated from the live `RegisterAction` surface. If an action is missing or renamed, re-run `monolith_discover({ namespace: "ndisplay" })` — the catalog is the source of truth.
- Pass `mode: "schema"` to `monolith_discover` for required/optional params and types before calling an action.
