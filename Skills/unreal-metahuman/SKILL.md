---
name: unreal-metahuman
description: Use to INSPECT MetaHuman / digital-human assets via Monolith MCP (metahuman namespace) — report MetaHuman capability status and list MetaHuman-like character assets via AssetRegistry. Read-only; MetaHuman component (MHC) setup, layout, build, rig, and conform are NOT exposed by this namespace today (confirm via monolith_discover). For setup/authoring, hand off by name. To inspect or edit the body or face skeletal mesh of a MetaHuman use unreal-mesh; for the MetaHuman face/body Control Rig, retargeting, and anim assets use unreal-animation; to edit the skin, eye, hair, or groom material graphs use unreal-materials. Triggers on metahuman, MetaHuman, MHC, MetaHuman Component, digital human, virtual human, character creator, list metahuman assets, metahuman status, find metahuman, inspect metahuman, metahuman capability.
---

# unreal-metahuman

Read-only inspection of MetaHuman / digital-human assets via the Monolith MCP `metahuman` namespace. **2 actions** via `metahuman_query(action, params)`, both inspection-only: report capability status and list MetaHuman-like character assets from AssetRegistry. MetaHuman component (MHC) setup, layout, build, rig, and conform are **not exposed by this namespace today** — verify with `monolith_discover` and hand off authoring to the skills named below. Action names here are the live registry surface; call `monolith_discover` for exact parameter schemas.

## Discovery

```
monolith_discover({ namespace: "metahuman" })                      # all actions in this namespace
monolith_discover({ namespace: "metahuman", action: "<action>", mode: "schema" })  # exact params
```

## When to use / Use a different skill for

- **Use this skill** to inspect MetaHuman tooling availability and to discover MetaHuman-like character assets in the project (status + AssetRegistry listing only).
- **Use `unreal-mesh`** to inspect or edit the body or face **skeletal mesh** of a MetaHuman (LODs, collision, UVs, tris/verts).
- **Use `unreal-animation`** for the MetaHuman's face/body **Control Rig**, retargeting, and anim assets.
- **Use `unreal-materials`** to edit the **skin/eye/hair/groom material graphs** of a MetaHuman.
- **No authoring here:** MHC creation/setup/layout, MetaHuman build/rig/conform are not exposed by the `metahuman` namespace today. This skill discovers the assets; the skills above own the edits.

## Action Reference

Param notation: `name*` required, `name?` optional, `name=val` default, `a/b/c` allowed, `[w]` mutates. Signatures are a snapshot of the live catalog — for the exact full schema call `monolith_discover` with `mode: "schema"`. The discover-first block above is the authority.

### Layout (2)

| Action | Params | Purpose |
|--------|--------|---------|
| `get_status` | (none) | Report read-only MetaHuman capability status without hard MetaHuman plugin dependencies or service calls. |
| `list_character_assets` | `package_path?=/Game` `limit?=100` | List MetaHuman-like assets under /Game using AssetRegistry metadata only (`limit` clamped to 1..500). Does not load characters, build, rig, conform, or call services. |

## Common Workflows

### Inspect MetaHuman support, find a character, then hand off authoring
This namespace cannot author a MetaHuman; the recipe inspects, then routes the actual edit to the right skill by name.

1. `metahuman_query("get_status")` — confirm MetaHuman capability/availability before assuming any MetaHuman work is possible.
2. `metahuman_query("list_character_assets", { "package_path": "/Game/Characters", "limit": 100 })` — list MetaHuman-like assets under a folder (AssetRegistry metadata only; `limit` clamps 1..500). Note the asset object paths you want to work on.
3. Hand off the authoring step to the owning skill, passing the path from step 2:
   - body/face **skeletal mesh** (LODs, collision, UVs) → **unreal-mesh**
   - face/body **Control Rig**, retarget, anim assets → **unreal-animation**
   - **skin/eye/hair/groom material** graphs → **unreal-materials**

If `get_status` reports tooling unavailable, stop and report it rather than guessing at a setup action that does not exist in this namespace.

## Gotchas

- Both actions are read-only — `get_status` makes no service calls and `list_character_assets` reads AssetRegistry metadata only. Neither loads characters or builds, rigs, or conforms a MetaHuman.
- `list_character_assets` depends on AssetRegistry being populated; if results look incomplete, ensure the project index is current (`monolith_reindex()`).

## Notes

- This reference is generated from the live `RegisterAction` surface. If an action is missing or renamed, re-run `monolith_discover({ namespace: "metahuman" })` — the catalog is the source of truth.
- Pass `mode: "schema"` to `monolith_discover` for required/optional params and types before calling an action.
