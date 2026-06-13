---
name: unreal-paper2d
description: Use to INSPECT Paper2D 2D content via Monolith MCP (paper2d namespace) - read PaperSprite, PaperFlipbook, PaperTileSet, and PaperTileMap asset metadata, check plugin/module availability, and list Paper2D assets via AssetRegistry under /Game. Read-only; building, editing, or authoring Paper2D / PaperZD assets is NOT exposed by this namespace today (confirm via monolith_discover). For the upstream sprite-sheet production contract (asset_spec.yaml, candidate selection, postprocess, export) feeding textures into Paper2D use unreal-sprite; for generic ingest/save/rename of the source Texture2D use unreal-asset; when that texture must be AI-generated use unreal-imagegen; to author the sprite material a flipbook references use unreal-materials. Triggers on paper2d, sprite, flipbook, tile map, tile set, PaperSprite, PaperFlipbook, PaperTileSet, PaperTileMap, inspect sprite, list paper2d assets, paper2d status, paper2d metadata.
---

# unreal-paper2d

Read-only inspection of Paper2D 2D content (PaperSprite, PaperFlipbook, PaperTileSet, PaperTileMap) through the Monolith MCP **paper2d** namespace via `paper2d_query(action, params)`. **3 actions**; all are read-only AssetRegistry/plugin-status queries that do not load Paper2D modules or mutate assets. Building, editing, or authoring Paper2D / PaperZD assets is **not exposed by this namespace today** — verify with `monolith_discover` and route production to **unreal-sprite** and source-texture ingest to **unreal-asset**. Action names below are a snapshot of the live registry surface — the discover-first block is the source of truth.

## Discovery

```
monolith_discover({ namespace: "paper2d" })                                      # all actions in this namespace
monolith_discover({ namespace: "paper2d", action: "<action>", mode: "schema" })  # exact params
```

When the right action is unclear, `monolith_find("<task>")` suggests candidates across namespaces. Module spec: `Docs/specs/SPEC_MonolithPaper2D.md`.

## When to use / Use a different skill for

- **This skill:** inspecting and listing existing Paper2D assets (sprites, flipbooks, tile sets, tile maps) and checking Paper2D plugin/module availability. Read-only — no Paper2D/PaperZD authoring is exposed here; route production and ingest to the skills below.
- **unreal-sprite** — the upstream sprite-sheet production contract (asset_spec.yaml, candidate selection, postprocess, export metadata) that hands finished textures into Paper2D, before the runtime Paper2D step.
- **unreal-asset** — generic ingest/save/rename/move/delete of the source Texture2D before a Paper2D sprite is built from it.
- **unreal-imagegen** — the source sprite texture must be AI-generated rather than imported, before assembling Paper2D sprites/flipbooks.
- **unreal-materials** — authoring the sprite/translucent material that a Paper2D sprite or flipbook references, versus the Paper2D asset itself.

## Action Reference

Param notation: `name*` required, `name?` optional, `name=val` default, `a/b/c` allowed, `[w]` mutates. Signatures are a snapshot of the live catalog — for the exact full schema call `monolith_discover` with `mode: "schema"`. The discover-first block above is the authority.

| Action | Params | Purpose |
|--------|--------|---------|
| `list_assets` | `package_path?=/Game` `limit?=100` | List Paper2D asset metadata under /Game using AssetRegistry only (PaperSprite, PaperFlipbook, PaperTileSet, PaperTileMap; `limit` clamped to 1..500). Does not load or mutate assets. |
| `get_asset` | `asset_path*` `include_tags?=true` `tag_limit?=50` | Inspect bounded AssetRegistry metadata for one Paper2D asset under /Game (`tag_limit` clamped to 0..200). Does not load Paper2D modules or mutate assets. |
| `get_status` | (none) | Report Paper2D plugin/module availability and the Monolith-native milestone for texture-atlas adjacent Paper2D discovery. Read-only; no Paper2D hard dependency. |

## Common Workflows

### Triage a Paper2D sprite/flipbook/tilemap: status, list, inspect
End-to-end read-only inspection of existing Paper2D content.

1. `paper2d_query("get_status", {})` — confirm the Paper2D plugin/module is available before inspecting; treat this as the gate.
2. `paper2d_query("list_assets", { "package_path": "/Game/Sprites", "limit": 50 })` — list Paper2D assets (PaperSprite/PaperFlipbook/PaperTileSet/PaperTileMap) under the folder via AssetRegistry only (`limit` clamps 1..500). Pick the object path to inspect.
3. `paper2d_query("get_asset", { "asset_path": "/Game/Sprites/Hero_Idle", "include_tags": true, "tag_limit": 50 })` — read bounded AssetRegistry metadata/tags for that one asset (`tag_limit` clamps 0..200). Asset paths are UE object paths, not `.uasset` files.

To **produce or edit** the content this only inspects: sprite-sheet production → **unreal-sprite**; source Texture2D ingest/save/rename → **unreal-asset**; AI-generated source texture → **unreal-imagegen**; referenced sprite material → **unreal-materials**.

## Gotchas / Rules

- Asset paths are UE object paths such as `/Game/Sprites/Hero_Idle`, not `.uasset` filesystem paths.
- These actions are AssetRegistry/plugin-status reads only — they do not load Paper2D modules or mutate assets. For producing the source sprite sheet hand off to `unreal-sprite`; for importing/saving the Texture2D use `unreal-asset`; for the referenced sprite material use `unreal-materials`.
- For generated Paper2D/PaperZD sprite sheets, keep the final Texture2D dimensions power-of-two when practical. Preserve proven cell sizes and adjust columns first; for 256px four-direction character sheets prefer `4x4=1024x1024` or `8x4=2048x1024` over non-power-of-two widths such as `6x4=1536x1024` unless the source art or runtime contract requires otherwise.
- This reference is generated from the live `RegisterAction` surface. If an action is missing or renamed, re-run `monolith_discover({ namespace: "paper2d" })` — the catalog is the source of truth.
