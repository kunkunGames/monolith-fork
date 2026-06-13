---
name: unreal-hlod
description: "Use when configuring or building Hierarchical LOD (HLOD) and World Partition HLOD layers via Monolith MCP (hlod namespace) — create/configure HLODLayer assets, build/clear HLOD, proxy/merged-mesh setup, readiness hashing, and bounded HLOD inspection reports. To inspect or edit the proxy/merged StaticMesh an HLOD produces (tris/LODs/collision/UVs) use unreal-mesh; to place/group the source actors that feed a cluster use unreal-scene; for the draw-call/triangle budgeting that motivates an HLOD pass use unreal-performance; to generate the blockout/town content HLOD later merges use unreal-worldgen. Triggers on HLOD, hierarchical LOD, proxy mesh, merged mesh, HLOD layer, HLODLayer asset, world partition HLOD, build HLOD, clear HLOD, legacy HLOD, instancing HLOD, HLOD stats, HLOD readiness, reduce draw calls with HLOD, merge distant actors."
---

# unreal-hlod

Configure and build Hierarchical LOD layers, drives the Monolith `hlod` namespace. **12 actions** via `hlod_query(action, params)`. Action names below are the live registry surface; call `monolith_discover` for exact parameter schemas.

## Discovery

```
monolith_discover({ namespace: "hlod" })                      # all actions in this namespace
monolith_discover({ namespace: "hlod", action: "<action>", mode: "schema" })  # exact params
```

## When to use / Use a different skill for

- **HLOD layer setup, build/clear orchestration, readiness/stats** → this skill.
- Inspect or edit the **proxy/merged StaticMesh** an HLOD produces (tris, LOD chain, collision, UVs) → **unreal-mesh** (it also owns the underlying `mesh.setup_hlod` / `generate_proxy_mesh` this namespace delegates to).
- Place, spawn, or **group the source actors** that feed an HLOD cluster → **unreal-scene**.
- **Draw-call / triangle budgeting** that motivates an HLOD pass — profile there, build HLOD here → **unreal-performance**.
- Generate the **blockout/town content** that HLOD later merges, versus the merge configuration itself → **unreal-worldgen**.

## Action Reference

Param notation: `name*` required, `name?` optional, `name=val` default, `a/b/c` allowed values, `[w]` mutates state (transaction-wrapped). Signatures are a snapshot of the live catalog — for the exact, full, current schema of any action call `monolith_discover` with `mode: "schema"`.

### Core (12)

| Action | Params (req* opt? =default) | Purpose |
|--------|-----------------------------|---------|
| `list_hlod_layers` | `package_path=/Game` `limit=100` | List HLODLayer assets under a package path. |
| `get_hlod_layer` | `asset_path*` | Inspect a HLODLayer asset using AssetRegistry and reflection. |
| `create_hlod_layer` [w] | `save_path*` `layer_type=MeshSimplify` (MeshMerge/MeshSimplify/MeshApproximate/Custom) `cell_size=25600` `loading_range=2.0` | Create/configure a HLOD layer through `mesh.setup_hlod`. |
| `configure_hlod_layer` [w] | `save_path*` `layer_type=MeshSimplify` (MeshMerge/MeshSimplify/MeshApproximate/Custom) `cell_size=25600` `loading_range=2.0` | Configure a HLOD layer through `mesh.setup_hlod`. |
| `list_hlod_source_actors` | `limit=250` | List loaded static mesh actors eligible as HLOD report source candidates. |
| `list_hlod_actors` | `limit=250` | List loaded HLOD-like actors in the editor world. |
| `get_hlod_stats` | `package_path=/Game` | Return HLOD layer and loaded actor counts for the current editor world. |
| `check_hlod_hash` | `package_path=/Game` | Compute a lightweight HLOD readiness hash from layer and actor identity rows. |
| `build_hlod` [w] | `confirm=false` (reserved for future build execution) | Report HLOD build orchestration status. Does not launch long-running builds yet. |
| `clear_legacy_hlod` [w] | `confirm=false` (reserved for future clear execution) | Report legacy HLOD clear orchestration status. Does not clear generated actors yet. |
| `legacy_hlod_needs_build` [w] | _(no params)_ | Return a conservative legacy-HLOD needs-build signal based on loaded HLOD actor presence. |
| `export_hlod` [w] | `output_path?` (absolute JSON path; defaults under `Saved/Monolith/HLOD`) | Export a bounded HLOD inspection report JSON file. |

Confirm parameter names with `monolith_discover({ namespace: "hlod", action: "<action>", mode: "schema" })` before calling — the `Params` column is a current snapshot, not the authoritative schema.

## Common Workflows

### 1. Audit current HLOD state (read-only)

```
1. hlod_query({ action: "list_hlod_layers", params: { package_path: "/Game" } })
2. hlod_query({ action: "get_hlod_layer", params: { asset_path: "/Game/HLOD/HLODLayer_Far" } })
3. hlod_query({ action: "get_hlod_stats", params: { package_path: "/Game" } })
4. hlod_query({ action: "list_hlod_actors", params: { limit: 250 } })
```

### 2. Set up a layer, then check build readiness

Create and configure are both `[w]` (transaction-wrapped); `create_hlod_layer` / `configure_hlod_layer` delegate to `mesh.setup_hlod`. After configuring, list the eligible source actors, hash readiness, and read the conservative needs-build signal before treating a build as pending.

```
1. hlod_query({ action: "create_hlod_layer", params: { save_path: "/Game/HLOD/HLODLayer_Far", layer_type: "MeshSimplify", cell_size: 25600, loading_range: 2.0 } })
2. hlod_query({ action: "configure_hlod_layer", params: { save_path: "/Game/HLOD/HLODLayer_Far", layer_type: "MeshSimplify", cell_size: 51200, loading_range: 4.0 } })
3. hlod_query({ action: "list_hlod_source_actors", params: { limit: 250 } })
4. hlod_query({ action: "check_hlod_hash", params: { package_path: "/Game/HLOD" } })
5. hlod_query({ action: "legacy_hlod_needs_build", params: {} })
```

Step 4's hash and step 5's `legacy_hlod_needs_build` are advisory readiness signals, not the build itself. `build_hlod` / `clear_legacy_hlod` currently report orchestration status only (see Gotchas).

**Hand off the proxy mesh:** `create_hlod_layer` / `configure_hlod_layer` produce the layer through `mesh.setup_hlod`; to inspect or post-process the resulting proxy/merged StaticMesh (tris, LOD chain, collision, UVs) switch to **unreal-mesh**.

### 3. Inspect-only report

`export_hlod` writes a bounded JSON report under `Saved/Monolith/HLOD` for offline review.

```
1. hlod_query({ action: "export_hlod", params: {} })
```

## Gotchas / Rules

- `build_hlod` and `clear_legacy_hlod` currently **report orchestration status only** — they do not launch a long-running build or delete generated HLOD actors. Treat them as readiness/intent probes, not as the actual UE build step.
- `create_hlod_layer` / `configure_hlod_layer` delegate to `mesh.setup_hlod`. To inspect or post-process the resulting proxy/merged StaticMesh, switch to **unreal-mesh**.
- `check_hlod_hash` and `legacy_hlod_needs_build` are conservative signals derived from loaded actor/layer identity rows; a "needs build" result is advisory.
- World/actor-scoped actions read the **current editor world**, so the editor must be live for accurate `get_hlod_stats` / `list_hlod_actors` results.

## Notes

- This reference is generated from the live `RegisterAction` surface. If an action is missing or renamed, re-run `monolith_discover({ namespace: "hlod" })` — the catalog is the source of truth.
- Pass `mode: "schema"` to `monolith_discover` for required/optional params and types before calling an action.
