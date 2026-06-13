---
name: unreal-cloth
description: Use to INSPECT Chaos Cloth assets via Monolith MCP — report cloth/outfit workflow support status and list cloth/clothing/outfit-like assets via AssetRegistry. This namespace is read-only inspection today; cloth config, weight/paint maps, collision, LOD, wind, and backstop authoring are NOT exposed here — confirm via monolith_discover, and if it lands route by name. For Chaos destruction/Geometry Collections use unreal-chaos-fracture; cloth attaches to a skeletal mesh, so to edit that underlying mesh asset use unreal-mesh. This skill owns cloth-asset inspection. Triggers on cloth, chaos cloth, clothing, ClothingAsset, list clothing assets, cloth status, inspect cloth, outfit asset, skin cloth, cloth LOD, cloth collision.
---

# unreal-cloth

Drives the Monolith **cloth** namespace for **inspecting** Chaos Cloth assets. **2 actions** via `cloth_query(action, params)`, both read-only (status / list). Cloth config, weight/paint maps, collision, LOD, wind, and backstop authoring are **not exposed by this namespace today** — verify the live catalog with `monolith_discover` before assuming a write action exists, and if one lands route to it by name. For cloth that attaches to a skeletal mesh, edits to the underlying mesh asset belong to **unreal-mesh**. Action names below are the live registry surface; call `monolith_discover` for exact parameter schemas.

## Discovery

```
monolith_discover({ namespace: "cloth" })                      # all actions in this namespace
monolith_discover({ namespace: "cloth", action: "<action>", mode: "schema" })  # exact params
```

## When to use / Use a different skill for

- **Use this skill** to inspect Chaos Cloth assets — report cloth/outfit workflow support status and list cloth/clothing/outfit-like assets via AssetRegistry. Inspection / read-only only.
- **Authoring is not exposed here.** Cloth config, weight/paint maps, cloth LODs, collision, backstops, and wind are not in this namespace today — confirm with `monolith_discover({ namespace: "cloth" })` before assuming a write action exists. Cloth-mesh edits (skin weights, collision geometry on the skeletal mesh) belong to **unreal-mesh**.
- **Use `unreal-chaos-fracture`** when the Chaos task is destruction or Geometry Collections (Voronoi/cluster fracturing) rather than cloth.
- **Use `unreal-mesh`** when you need the underlying skeletal mesh asset edited (LODs, collision, UVs, geometry) rather than inspecting cloth assets.

## Action Reference

Param notation: `name*` required, `name?` optional, `name=val` default, `a/b/c` allowed values, `[w]` mutates state. Signatures are a snapshot of the live catalog — for the exact, full, current schema of any action call `monolith_discover` with `mode: "schema"`.

### Cloth (2)

| Action | Purpose | Params (req* opt? =default) |
|--------|---------|------------------------------|
| `get_status` | Report read-only Chaos Cloth/Outfit workflow support without hard Chaos Outfit dependencies. | _(none)_ |
| `list_clothing_assets` | List cloth/clothing/outfit-like assets under /Game using AssetRegistry metadata only. Does not load vertex data, weight maps, or mutate assets. | `package_path="/Game"` (content package path under /Game); `limit=100` (int, clamped 1..500) |

## Common Workflows

### 1. Inspect cloth assets in the project (read-only)

```
# 1. Confirm Chaos Cloth / Outfit workflow support is present (no hard Outfit dep)
cloth_query("get_status", {})

# 2. List cloth/clothing/outfit-like assets under a content path (limit clamped 1..500)
cloth_query("list_clothing_assets", { "package_path": "/Game", "limit": 100 })

# To edit cloth config / paint weight maps / set collision or LOD, that authoring is not
# in this namespace — hand the mesh-side edits to unreal-mesh, and confirm cloth authoring
# against the live catalog first:
monolith_discover({ namespace: "cloth" })
```

## Notes

- This reference is generated from the live `RegisterAction` surface. If an action is missing or renamed, re-run `monolith_discover({ namespace: "cloth" })` — the catalog is the source of truth.
- Pass `mode: "schema"` to `monolith_discover` for required/optional params and types before calling an action.
