---
name: unreal-sprite
description: "Use for Monolith sprite production workflows: sprite-sheet contracts, generated image to asset import, asset_spec.yaml, candidate selection, postprocess, export metadata, and Paper2D/UI/VFX handoff. Triggers on sprite, sprite sheet, icon atlas, Paper2D sheet, asset_spec.yaml, skill icon, item icon, effect sprite."
---

# Unreal Sprite Skill

Use this skill for sprite asset production before routing finished textures into Paper2D, UI, Niagara, or gameplay systems.

## Namespace

- Primary namespace: `sprite` when present in the live registry.
- Production spec: `Docs/specs/SPEC_MonolithSpriteAsset.md`
- Common handoff namespaces: `imagegen`, `asset`, `paper2d`, `ui`, `niagara`

## Workflow

1. Start from an `asset_spec.yaml` contract for the sprite asset.
2. Confirm the asset profile: `pc_character`, `npc_character`, `monster_character`, `item_icon`, `skill_icon`, `world_pickup_sprite`, or `effect_sprite`.
3. Validate guide requirements for the profile: pose, silhouette, composition, identity, style, safe area, pivot, and frame order.
4. Use `imagegen` only after the spec and guide assets are ready.
5. Import accepted outputs through `asset.import_texture_from_bytes` or `asset.import_texture_from_file` with `texture_role` set to `sprite`, `ui_icon`, or `decal` as appropriate.
6. Use `sprite` namespace actions when live discovery shows them; otherwise use the spec as the production contract and hand off to the consuming domain.
7. Verify final cell size, sheet dimensions, alpha, pivot, metadata, and target consumer expectations.

## Rules

- Do not bake UI frames, cooldown numbers, hotkeys, or text into icons unless the asset spec explicitly requires it.
- For animation sheets, keep canvas, lighting, baseline, pivot, and identity stable across frames.
- For effects, declare blend mode, flow direction, impact center, and frame energy before generation.
- For Paper2D runtime work, switch to `unreal-paper2d` after the texture/sheet production step is complete.
