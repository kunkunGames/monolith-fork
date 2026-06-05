---
name: unreal-level-instance
description: "Use for Level Instances and packed level actors via Monolith MCP: create, edit, commit, break, and pack instanced level content. Triggers on level instance, packed level actor, PLA, instanced level, blueprint level instance, break level instance, commit level instance."
---

# unreal-level-instance

**16 actions** via `level_instance_query(action, params)`. Action names below are the live registry surface; call `monolith_discover` for exact parameter schemas.

## Discovery

```
monolith_discover({ namespace: "level_instance" })                      # all actions in this namespace
monolith_discover({ namespace: "level_instance", action: "<action>", mode: "schema" })  # exact params
```

## Action Reference

### Core (13)

| Action | Purpose |
|--------|---------|
| `commit_level_instance` | Report Level Instance commit availability and dirty-package context. |
| `create_level_instance` | Preview or create a Level Instance from actor_names. Requires confirm=true for mutation. |
| `create_packed_level_actor_blueprint` | Preview or create a Packed Level Actor from actor_names. Requires confirm=true for mutation. |
| `discard_level_instance` | Report Level Instance discard availability and dirty-package context. |
| `edit_level_instance` | Report Level Instance edit-session availability without taking over editor-global state. |
| `get_level_instance` | Inspect a Level Instance actor by label, name, or object path. |
| `list_child_instances` | List attached child Level Instance-like actors for a parent Level Instance. |
| `list_instance_actors` | List directly attached actors for a Level Instance actor. |
| `list_level_instances` | List Level Instance-like actors in the current editor world. |
| `load_level_instance` | Report Level Instance load availability without forcing nested edit state. |
| `move_actors_to_instance` | Preview actor movement into a Level Instance; direct nested mutation is unavailable until conflict tests exist. |
| `pack_level_actor` | Preview or create a Packed Level Actor using the Level Instance creation path. Requires confirm=true. |
| `unload_level_instance` | Report Level Instance unload availability without forcing nested edit state. |

### Level Design Placement (3)

| Action | Purpose |
|--------|---------|
| `create_blueprint_prefab` | Create a Blueprint prefab from existing world actors. Harvests all components into a new Actor Blueprint's SCS. Dialog-free — safe for MCP/automation. Use place_blueprint_actor to spawn instances. |
| `create_prefab` | Create a Level Instance (prefab) from existing actors. WARNING: Source actors are MOVED into the new level. NOTE: This action triggers a Save As dialog which blocks MCP calls. For dialog-free prefab creation, use create_blueprint_prefab instead. |
| `spawn_prefab` | Spawn a Level Instance (prefab) at a location. |

## Notes

- This reference is generated from the live `RegisterAction` surface. If an action is missing or renamed, re-run `monolith_discover({ namespace: "level_instance" })` — the catalog is the source of truth.
- Pass `mode: "schema"` to `monolith_discover` for required/optional params and types before calling an action.
