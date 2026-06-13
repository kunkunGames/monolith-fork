---
name: unreal-level-instance
description: Use when creating, editing, committing, breaking, or packing Level Instances and packed level actors via Monolith MCP (level_instance namespace) — make a prefab from selected actors, spawn prefab instances, list instance/child actors, and report commit/load/unload availability. For placing, moving, or grouping ordinary actors in the live level use unreal-scene; for procedurally generating the building/room content later packed into an instance use unreal-worldgen; to edit a Blueprint level-instance's logic graph use unreal-blueprints. Triggers on level instance, packed level actor, PLA, instanced level, blueprint level instance, prefab, make prefab, spawn prefab, break level instance, commit level instance, pack actors into level.
---

# unreal-level-instance

Drives the **`level_instance`** namespace via `level_instance_query(action, params)` — create/edit/commit/break/pack Level Instances and packed level actors, plus prefab create/spawn helpers. The **16 actions** below are a snapshot; discover first so you never call a stale or guessed name.

## Discovery

```
monolith_discover({ namespace: "level_instance" })                      # all actions in this namespace
monolith_discover({ namespace: "level_instance", action: "<action>", mode: "schema" })  # exact params
```

## When to use / Use a different skill for

- **unreal-level-instance (this skill)** — the operation IS the instancing: create a Level Instance / packed level actor / prefab from actors, spawn prefab instances, commit/discard/break, or inspect instance contents.
- **unreal-scene** — placing, moving, duplicating, aligning, or grouping ordinary actors in the live level (the source actors before you pack them).
- **unreal-worldgen** — procedurally generating the building/room content (blockout, scatter, facades) that later gets packed into a Level Instance, versus the instancing operation itself.
- **unreal-blueprints** — editing a Blueprint level-instance's logic graph (variables/nodes/functions), versus committing or breaking the instanced level content.

## Action Reference

Param notation: `name*` required, `name?` optional, `name=val` default, `a/b/c` allowed values, `[w]` mutates (transaction-wrapped). The `actor` param accepts aliases `actor_name`/`actor_path` and takes an actor label, object name, or object path. Signatures are a snapshot of the live catalog — for the exact full schema call `monolith_discover` with `mode: "schema"` (the Discovery block above stays the authority).

### Core (13)

| Action | Signature | Purpose |
|--------|-----------|---------|
| `list_level_instances` | `limit?=100` | List Level Instance-like actors in the current editor world |
| `get_level_instance` | `actor?` | Inspect a Level Instance actor by label, name, or path |
| `list_child_instances` | `actor?` | List attached child Level Instance-like actors of a parent |
| `list_instance_actors` | `actor?` | List directly attached actors for a Level Instance actor |
| `[w] create_level_instance` | `actor_names*` (array) `save_path*` `confirm?=false` | Preview or create a Level Instance from selected actors |
| `[w] edit_level_instance` | `actor?` | Report edit-session availability without taking over editor-global state |
| `[w] commit_level_instance` | `actor?` | Report commit availability and dirty-package context |
| `[w] discard_level_instance` | `actor?` | Report discard availability and dirty-package context |
| `[w] load_level_instance` | `actor?` | Report load availability without forcing nested edit state |
| `[w] unload_level_instance` | `actor?` | Report unload availability without forcing nested edit state |
| `[w] move_actors_to_instance` | `actor?` `actor_names?` (array) `confirm?=false` | Preview actor movement into a Level Instance (nested mutation reserved) |
| `[w] create_packed_level_actor_blueprint` | `actor_names*` (array) `save_path*` `confirm?=false` | Preview or create a Packed Level Actor from actors |
| `[w] pack_level_actor` | `actor_names*` (array) `save_path*` `confirm?=false` | Preview or create a Packed Level Actor via the Level Instance path |

### Level Design Placement (3)

| Action | Signature | Purpose |
|--------|-----------|---------|
| `[w] create_blueprint_prefab` | `actor_names*` (array) `save_path*` `center_pivot?=true` `keep_source_actors?=true` | Harvest world actors' components into a new Actor BP SCS (dialog-free; safe for MCP) |
| `[w] create_prefab` | `actor_names*` (array) `save_path*` `type?=LevelInstance` (`LevelInstance`/`PackedLevelActor`) | Create a Level Instance prefab; source actors are MOVED and a Save As dialog blocks MCP — prefer `create_blueprint_prefab` |
| `[w] spawn_prefab` | `prefab_path*` `location*` (array `[x,y,z]`) `rotation?=[0,0,0]` `scale?=[1,1,1]` `label?` | Spawn a Level Instance prefab at a location |

## Common Workflows

Numbered recipes use only the actions in the table above. Run `monolith_discover` with `mode: "schema"` for exact params before each call. Two-phase create discipline: the `create`/`pack`/`move` actions default to `confirm: false`, which returns a **preview** of what would be packed; re-call with `confirm: true` to actually create. `save_path` is the destination package the prefab/instance asset is written to.

### Recipe 1 — Pack selected actors into a prefab and place an instance

1. `level_instance_query("list_level_instances", { limit: 100 })` — see existing Level Instance-like actors so you do not duplicate one; `actor` params accept a label, object name, or object path (aliases `actor_name`/`actor_path`).
2. `level_instance_query("create_packed_level_actor_blueprint", { actor_names: [ ... ], save_path })` `[w]` — PREVIEW (`confirm` defaults to `false`): confirm which harvested actors would be packed and where. Re-call with `confirm: true` to create the Packed Level Actor at `save_path`. For a dialog-free SCS-component prefab that never blocks MCP, use `create_blueprint_prefab` (`center_pivot`/`keep_source_actors` instead of confirm); avoid `create_prefab` for unattended runs because it MOVES sources and a Save As dialog blocks MCP.
3. `level_instance_query("spawn_prefab", { prefab_path, location: [x,y,z], rotation: [0,0,0], scale: [1,1,1], label })` `[w]` — spawn an instance of the saved prefab at a world location.
4. `level_instance_query("list_instance_actors", { actor })` — list the directly attached actors of the spawned instance to verify its contents, and `level_instance_query("list_child_instances", { actor })` to enumerate nested child Level Instances.
5. (inspect) `level_instance_query("get_level_instance", { actor })` — read the spawned instance's summary; `commit_level_instance` / `discard_level_instance` / `load_level_instance` / `unload_level_instance` report availability and dirty-package context rather than forcing editor-global edit state.

### Recipe 2 — Edit-commit availability cycle on an existing instance

Walk a placed Level Instance through an edit/commit decision using the availability-reporting actions (these report state and dirty-package context; they do not force editor-global edit takeover).

1. `level_instance_query("list_level_instances", { limit: 100 })` — enumerate placed Level Instance-like actors and pick the target; `actor` params accept a label, object name, or object path (aliases `actor_name`/`actor_path`).
2. `level_instance_query("get_level_instance", { actor })` — read the target's summary before deciding to edit.
3. `level_instance_query("list_instance_actors", { actor })` then `level_instance_query("list_child_instances", { actor })` — inventory the directly attached actors and any nested child Level Instances so you know what an edit session would touch.
4. `level_instance_query("edit_level_instance", { actor })` `[w]` — report edit-session availability for the target without taking over editor-global state.
5. `level_instance_query("commit_level_instance", { actor })` `[w]` — report commit availability and dirty-package context to confirm whether pending edits can be written back. If you instead want to drop pending edits, call `level_instance_query("discard_level_instance", { actor })` `[w]` for discard availability and dirty-package context.
6. (optional load gating) `level_instance_query("load_level_instance", { actor })` / `level_instance_query("unload_level_instance", { actor })` `[w]` — report load/unload availability without forcing nested edit state.

## Notes

- This reference is generated from the live `RegisterAction` surface. If an action is missing or renamed, re-run `monolith_discover({ namespace: "level_instance" })` — the catalog is the source of truth.
- Pass `mode: "schema"` to `monolith_discover` for required/optional params and types before calling an action.
