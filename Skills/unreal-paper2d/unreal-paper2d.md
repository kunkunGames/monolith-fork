---
name: unreal-paper2d
description: "Use for Paper2D 2D content via Monolith MCP: sprites, flipbooks, and tile maps. Triggers on paper2d, sprite, flipbook, tile map, tile set, 2D, paper sprite, paper character."
---

# unreal-paper2d

**3 actions** via `paper2d_query(action, params)`. Action names below are the live registry surface; call `monolith_discover` for exact parameter schemas.

## Discovery

```
monolith_discover({ namespace: "paper2d" })                      # all actions in this namespace
monolith_discover({ namespace: "paper2d", action: "<action>", mode: "schema" })  # exact params
```

## Action Reference

### Core (3)

| Action | Purpose |
|--------|---------|
| `get_asset` | Inspect bounded AssetRegistry metadata for one Paper2D asset under /Game. Does not load Paper2D modules or mutate assets. |
| `get_status` | Report Paper2D plugin/module availability and the Monolith-native first milestone for texture-atlas adjacent Paper2D discovery. Read-only; no Paper2D hard dependency. |
| `list_assets` | List Paper2D asset metadata under /Game using AssetRegistry only: PaperSprite, PaperFlipbook, PaperTileSet, and PaperTileMap. Does not load or mutate assets. |

## Notes

- This reference is generated from the live `RegisterAction` surface. If an action is missing or renamed, re-run `monolith_discover({ namespace: "paper2d" })` — the catalog is the source of truth.
- Pass `mode: "schema"` to `monolith_discover` for required/optional params and types before calling an action.
