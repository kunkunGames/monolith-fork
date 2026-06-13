---
name: unreal-water
description: Use when inspecting the Unreal Water system via Monolith MCP (water namespace) - report Water/Landscape module availability and enumerate water-body actors (ocean/lake/river) and water zones in the live editor world using reflected class names. To place/move the water body actor or run spatial queries around it use unreal-scene; to edit the water surface material or Gerstner-wave material graph use unreal-materials; to generate the surrounding terrain/landscape the water sits in use unreal-worldgen. Triggers on water, ocean, lake, river, water body, WaterBody, water zone, WaterZone, water mesh, buoyancy, waves, wave, gerstner, gerstner waves, water module status, list water bodies, find water in level.
---

# unreal-water

Drives the **water** namespace to inspect the Unreal Water plugin surface — reports Water/Landscape module availability and enumerates water-like actors (ocean/lake/river bodies, water zones) in the live editor world via reflected class names. Read-only; it does not mutate actors, splines, landscapes, or zones.

**2 actions** via `water_query(action, params)`. Action names below are the live registry surface; call `monolith_discover` for exact parameter schemas.

## Discovery

```
monolith_discover({ namespace: "water" })                      # all actions in this namespace
monolith_discover({ namespace: "water", action: "<action>", mode: "schema" })  # exact params
```

## When to use / Use a different skill for

- **This skill:** check whether the Water/Landscape modules are present and list the water bodies / water zones already in the level (reflected, read-only).
- **unreal-scene** — place, move, duplicate, or delete the water body actor, or run spatial queries (raycast/overlap/nearest/navmesh) around it; this skill only enumerates, it does not place or transform.
- **unreal-materials** — edit the water surface material or the Gerstner-wave material graph; the wave/shader visuals live in the material asset, not the water body actor.
- **unreal-worldgen** — generate the surrounding terrain, landscape, or town the water sits in; this skill reports water actors but does not author the world around them.

## Action Reference

Param notation: `name*` required, `name?` optional, `name=val` default, `a/b/c` allowed, `[w]` mutates. Signatures are a snapshot of the live catalog — for the exact full schema call `monolith_discover` with `mode: "schema"`. Keep the discover-first block above as the authority.

### Core (2)

| Action | Params | Purpose |
|--------|--------|---------|
| `get_status` | _(none)_ | Report Water/Landscape module availability and reflected Water-like actor counts. Read-only; no Water or Landscape hard dependency. |
| `list_bodies` | `limit?=100` `actor_name_filter?` | List Water-like actors/components in the current editor world using reflected class names only. `limit` is clamped to 1..500; `actor_name_filter` is an optional case-insensitive substring filter on actor label/name. Does not mutate actors, splines, landscapes, or zones. |

## Typical workflows

- **Confirm the plugin is usable:** `water_query({ action: "get_status" })` first — if the Water/Landscape modules are not reported, no water bodies can exist yet.
- **Inventory existing water:** `water_query({ action: "get_status" })` → `water_query({ action: "list_bodies" })` to enumerate ocean/lake/river bodies and zones already placed.
- **Act on a found body:** use `list_bodies` here to get the actor names, then switch to **unreal-scene** to move/duplicate/delete or run spatial queries against them.

## Gotchas

- Both actions are **read-only and reflection-based** — they detect Water-like actors by reflected class name, so they work even without a hard Water plugin dependency, but they will not place, transform, or configure anything.
- `get_status` reporting the Water module as unavailable means the plugin is not enabled; enable it in project plugins/config (use unreal-config for the `.ini` or plugin-descriptor edit) before expecting `list_bodies` to return results.
- This namespace does not author wave parameters or the water material — those live in the surface material asset (use unreal-materials).

## Notes

- This reference is generated from the live `RegisterAction` surface. If an action is missing or renamed, re-run `monolith_discover({ namespace: "water" })` — the catalog is the source of truth.
- Pass `mode: "schema"` to `monolith_discover` for required/optional params and types before calling an action.
