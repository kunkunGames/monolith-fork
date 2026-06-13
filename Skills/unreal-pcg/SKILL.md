---
name: unreal-pcg
description: Use to INSPECT Procedural Content Generation (PCG) graphs via Monolith MCP (pcg namespace) — report PCG module/type availability, list PCG graph assets, inspect one PCG graph asset's metadata, and list PCG components in the world. This namespace is read-only inspection today; authoring (create/edit graph, add and wire nodes, set settings, run generation) is NOT exposed here — confirm via monolith_discover, and if it lands route by name. For fixed blockout/town/building/facade generation use unreal-worldgen; for a geometry/Chaos node graph use unreal-dataflow (PCG and Dataflow are different node-graph systems); to edit the source meshes a PCG graph scatters use unreal-mesh; to place the PCG component/volume actor and inspect spawned actors use unreal-scene. Triggers on PCG, procedural content generation, PCG graph, PCG node, inspect PCG graph, list PCG graphs, PCG component, PCG volume, PCG graph asset, PCG module status, PCG component list.
---

# unreal-pcg

Drives the Monolith **pcg** namespace for **inspecting** Procedural Content Generation (PCG) graphs. **4 actions** via `pcg_query(action, params)`, all read-only (status / list / read). Authoring a PCG graph (create/edit graph, add/wire nodes, set settings, run generation) is **not exposed by this namespace today** — verify the live catalog with `monolith_discover` before assuming a write action exists, and if one lands route to it by name. Action names below are the live registry surface; call `monolith_discover` for exact parameter schemas.

## Discovery

```
monolith_discover({ namespace: "pcg" })                      # all actions in this namespace
monolith_discover({ namespace: "pcg", action: "<action>", mode: "schema" })  # exact params
```

## When to use / Use a different skill for

- **Use this skill** to inspect a reusable PCG graph — report PCG module/type availability, list PCG graph assets, read one graph asset's AssetRegistry metadata, and list the PCG components in the world. Inspection / read-only only.
- **Authoring is not exposed here.** Creating/editing a graph, adding and wiring nodes, setting node settings, or running a generation pass is not in this namespace today — confirm with `monolith_discover({ namespace: "pcg" })` before assuming a write action exists.
- **unreal-worldgen** for fixed blockout, town/building, facade, street, and furnishing generation — discrete placement passes rather than a reusable PCG node graph.
- **unreal-dataflow** when the procedural node graph is a geometry/Chaos Dataflow graph. PCG and Dataflow are different node-graph systems — pick by which graph type the asset is.
- **unreal-mesh** to edit the source StaticMesh/SkeletalMesh assets a PCG graph scatters, versus wiring the PCG graph nodes.
- **unreal-scene** to place the PCG component/volume actor in the level and inspect the actors a generation pass spawned, versus building the graph.

## Action Reference

Param notation: `name*` required, `name?` optional, `name=val` default, `a/b/c` allowed, `[w]` mutates. Signatures are a snapshot of the live catalog — for the exact full schema call `monolith_discover` with `mode: "schema"`. The discover-first block above is the authority. Authoring write actions are not in this snapshot — confirm them (and their `[w]` mutation policy) via discover before calling.

### Core (4)

| Action | Params | Purpose |
|--------|--------|---------|
| `get_status` | (none) | Report optional PCG module/type availability without loading PCG or mutating the level |
| `list_graph_assets` | `package_path?=/Game` `limit?=100` | List PCG graph-like assets using AssetRegistry class paths without hard PCG dependencies (`limit` 1-500) |
| `get_graph_asset` | `asset_path*` `include_tags?=true` `tag_limit?=50` | Inspect bounded AssetRegistry metadata for one PCG graph-like asset without loading PCG or mutating packages (`tag_limit` 0-200) |
| `list_components` | `limit?=100` | List PCG-like components in the current editor world using reflected class names (`limit` 1-500) |

## Common Workflows

### 1. Survey PCG in the project and inspect one graph (read-only)

```
# 1. Confirm PCG support is available before touching graphs (PCG is an optional module)
pcg_query("get_status", {})

# 2. List PCG graph-like assets via AssetRegistry (limit 1-500)
pcg_query("list_graph_assets", { "package_path": "/Game", "limit": 100 })

# 3. List PCG-like components in the current editor world to see what runs a graph live
pcg_query("list_components", { "limit": 100 })

# 4. Read bounded AssetRegistry metadata for one PCG graph asset (tags included by default)
pcg_query("get_graph_asset", { "asset_path": "/Game/PCG/PCG_Scatter", "include_tags": true, "tag_limit": 50 })

# To author (create/edit graph, add/wire nodes, set settings, generate), confirm the action
# and its params exist in the live catalog first — authoring is not in this snapshot:
monolith_discover({ namespace: "pcg", action: "<action>", mode: "schema" })
```

## Gotchas

- The four actions in the table are **read-only** — they inspect assets/components and report module availability without loading PCG or dirtying packages. Authoring intents (create/edit graph, add and wire nodes, set settings, run generation) must be resolved against the live catalog via `monolith_discover` before you call them; do not assume a write action exists from this snapshot.
- `get_status` first — PCG is an optional module. If it reports the type/module unavailable, graph reads and authoring will not work until PCG is enabled for the project.
- `list_graph_assets` / `get_graph_asset` use AssetRegistry class paths and stay decoupled from a hard PCG link dependency, so they work even when PCG is not loaded but report metadata only, not live graph node contents.

## Notes

- This reference is generated from the live `RegisterAction` surface. If an action is missing or renamed, re-run `monolith_discover({ namespace: "pcg" })` — the catalog is the source of truth.
- Pass `mode: "schema"` to `monolith_discover` for required/optional params and types before calling an action.
