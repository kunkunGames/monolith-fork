---
name: unreal-chaos-fracture
description: Use to INSPECT Chaos destruction assets via Monolith MCP — report Geometry Collection / Fracture module status, list Geometry Collection assets, and list Geometry Collection components in the world. This namespace is read-only inspection today; the actual fracturing (Voronoi/cluster), anchor/field setup, and destruction authoring are NOT exposed here — confirm via monolith_discover, and if it lands route by name. For authoring the Chaos/geometry node GRAPH use unreal-dataflow; if the Chaos system is cloth simulation use unreal-cloth. This skill owns Geometry Collection inspection. Triggers on chaos, geometry collection, GeometryCollection asset, destructible, list geometry collections, geometry collection status, inspect geometry collection, fracture asset, shatter asset, voronoi, cluster, rigid body, RBD.
---

# unreal-chaos-fracture

Drives the Monolith **chaos_fracture** namespace for **inspecting** Chaos destruction assets. **3 actions** via `chaos_fracture_query(action, params)`, all read-only (status / list). The actual fracturing (Voronoi/cluster), anchor/field setup, cluster connection, and destruction authoring are **not exposed by this namespace today** — verify the live catalog with `monolith_discover` before assuming a write action exists, and if one lands route to it by name. Action names below are the live registry surface; call `monolith_discover` for exact parameter schemas.

## Discovery

```
monolith_discover({ namespace: "chaos_fracture" })                      # all actions in this namespace
monolith_discover({ namespace: "chaos_fracture", action: "<action>", mode: "schema" })  # exact params
```

## When to use / Use a different skill for

- **Use this skill** to inspect Chaos destruction assets — report Geometry Collection / Fracture module status, list Geometry Collection-like assets, and list Geometry Collection-like components in the editor world. Inspection / read-only only.
- **Authoring is not exposed here.** Performing the fracture (Voronoi/cluster), wiring anchor/field systems, setting cluster connections, or building rigid-body (RBD) destruction is not in this namespace today — confirm with `monolith_discover({ namespace: "chaos_fracture" })` before assuming a write action exists.
- **Use unreal-dataflow** when authoring the Chaos/geometry node GRAPH (Dataflow) rather than the Geometry Collection itself.
- **Use unreal-cloth** when the Chaos system is cloth simulation, not destruction.

## Action Reference

Param notation: `name*` required, `name?` optional, `name=val` default, `a/b/c` allowed values, `[w]` mutates. Signatures are a snapshot of the live catalog — for the exact, full, current schema of any action call `monolith_discover` with `mode: "schema"`.

### Core (3)

| Action | Params (req* opt? =default) | Purpose |
|--------|------------------------------|---------|
| `get_status` | (none) | Report optional Geometry Collection / Fracture module and reflected type availability without mutating assets |
| `list_geometry_collection_assets` | `package_path=/Game` `limit=100` (1-500) | List Geometry Collection-like assets using AssetRegistry class paths without loading Fracture modules |
| `list_geometry_collection_components` | `limit=100` (1-500) | List Geometry Collection-like components in the current editor world using reflected class names |

All three actions are `read_only` (no `[w]` writes-marker); none mutate assets or wrap a transaction.

## Common Workflows

### 1. Inspect Geometry Collection destruction assets and live instances (read-only)

```
# 1. Confirm the optional Geometry Collection / Fracture module + reflected types are present
chaos_fracture_query("get_status", {})

# 2. List Geometry Collection-like assets under a content path (limit 1-500)
chaos_fracture_query("list_geometry_collection_assets", { "package_path": "/Game", "limit": 100 })

# 3. List Geometry Collection-like components placed in the current editor world (limit 1-500)
chaos_fracture_query("list_geometry_collection_components", { "limit": 100 })

# To actually fracture / set anchors / build destruction, confirm the live catalog first
# (authoring is not in this snapshot):
monolith_discover({ namespace: "chaos_fracture" })
```

## Notes

- This reference is generated from the live `RegisterAction` surface. If an action is missing or renamed, re-run `monolith_discover({ namespace: "chaos_fracture" })` — the catalog is the source of truth.
- Pass `mode: "schema"` to `monolith_discover` for required/optional params and types before calling an action.
