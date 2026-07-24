# Monolith - Sprite Asset Production Spec

**Parent:** [SPEC_MonolithImageGen.md](SPEC_MonolithImageGen.md), [SPEC_MonolithAsset.md](SPEC_MonolithAsset.md), [SPEC_MonolithPaper2D.md](SPEC_MonolithPaper2D.md)
**Engine:** Unreal Engine 5.7+
**Owner workflow:** `imagegen` generation -> `asset` Texture2D import -> runtime sprite/UI/Paper2D consumption
**Module:** `MonolithSprite`
**Namespace:** `sprite`
**Status:** Implemented M1/M2 validation/export plus M3 request preparation and delegated batch execution

---

## 1. Goal

Define one reusable sprite production contract for gameplay characters, NPCs, monsters, items, skills, UI icons, pickups, and VFX sprites. This contract replaces asset-specific prompt-only workflows with a repeatable package of references, guide images, `asset_spec.yaml`, candidate generation, selection, postprocess, and metadata export.

`hero_knight_v01` is an instance of the `pc_character` profile, not the owner of the workflow. The same production model must support NPCs, monsters, item icons, skill icons, world pickup sprites, and effect flipbooks without hard-coding HeroKnight assumptions.

---

## 2. Supported Asset Profiles

| Profile | Examples | Primary output | Texture role | Required guides |
|---------|----------|----------------|--------------|-----------------|
| `pc_character` | player hero, playable class, companion avatar | animation sprite sheet | `sprite` | pose guide plus silhouette guide per animation frame |
| `npc_character` | villager, merchant, quest NPC, neutral faction unit | idle/walk/talk sheet or portrait cells | `sprite` | pose plus silhouette for animated cells; silhouette-only for portraits |
| `monster_character` | humanoid monster, beast, boss, swarm unit | animation sheet with gameplay tells | `sprite` | silhouette always; pose/skeleton when anatomy is legible |
| `item_icon` | weapon, armor, consumable, currency, loot | single UI icon or icon atlas | `ui_icon` | silhouette or shape guide; pose guide not required |
| `skill_icon` | active skill, passive, buff, debuff, elemental spell | single UI icon or icon atlas | `ui_icon` | composition/silhouette guide; pose only when a figure is part of the icon |
| `world_pickup_sprite` | dropped item, pickup shard, interactable prop | in-world sprite or short sheet | `sprite` | silhouette guide; pose optional |
| `effect_sprite` | slash frame, impact burst, projectile, status mark | VFX sprite or flipbook | `sprite` or `decal` by runtime use | shape/flow guide; silhouette when outer shape matters |

Use `ui_icon` for UI-only item and skill icons. Use `sprite` for Paper2D/PaperZD sheets, in-world billboards, animated pickups, VFX flipbooks, or any output that needs sprite-sheet constraints.

---

## 3. Directory Layout

Each produced asset lives under a stable asset directory. Documentation and pre-generation assets use `Docs/assets/<asset_id>/`; imported runtime outputs may mirror final PNGs under `/Game/GeneratedImages` or another project content path.

```text
Docs/assets/<asset_id>/
  asset_spec.yaml
  style_ref/
    final_quality_target.png
  identity_ref/
    *.png
  pose_guides/
    <frame_id>_pose.png
  silhouette_guides/
    <frame_id>_silhouette.png
  composition_guides/
    <frame_id>_composition.png
  candidates/
    <frame_id>/
  selected/
  postprocessed/
  export/
    <asset_id>_sheet.png
    <asset_id>_metadata.json
```

Only the guide folders required by the selected profile need to exist. Do not create empty folders as false evidence of readiness.

---

## 4. Common Visual Contract

| Field | Character / monster sprites | UI item / skill sprites |
|-------|-----------------------------|-------------------------|
| Work canvas | Usually `768x768`; larger only for a large creature, wide attack, or wide VFX frame. | Usually `512x512` or `768x768`; `1024x1024` only for high-detail source before downscale. |
| Final cell | Usually `256x256`; sheet dimensions should stay power-of-two when practical. | Usually `128x128` or `256x256`; atlas cells must be identical. |
| Background | Transparent target or flat chroma for edge-connected alpha extraction. | Transparent target or flat chroma; no baked UI panel unless specified. |
| Lighting | Stable light direction across a sheet. | Stable light direction across an icon family. |
| Outline | Explicit final-scale outline width when the style requires it. | Icon edge must remain readable under cooldown, disabled, hover, and quantity states. |
| Palette | Fixed palette budget by asset family. | Shared rarity/element palette rules must be explicit. |
| Pivot | Bottom-center for most characters; center for effects unless runtime says otherwise. | Center by default; optional overlay-safe region for cooldown, hotkey, or quantity UI. |

---

## 5. `asset_spec.yaml` Schema

The file is the machine-readable source of truth for a production run. It must be deterministic, reviewable, and free of secrets.

```yaml
asset_id: <stable_asset_id>
asset_family: sprite_asset
asset_profile: pc_character | npc_character | monster_character | item_icon | skill_icon | world_pickup_sprite | effect_sprite
target_surface: gameplay_sprite | paper2d_sheet | ui_icon | ui_atlas | vfx_flipbook
final_cell: 256x256
work_canvas: 768x768
camera: side view
background: transparent or flat chroma background
lighting: top-left, soft 2-step shading
outline: dark 2px outline
palette: 16-24 colors
model: your_base_model
style_ref: style_ref/final_quality_target.png
identity_ref:
  - identity_ref/source_01.png
pose_control: ControlNet pose + silhouette
seed_bank: [12031, 12032, 12033, 12034]
negative:
  - inconsistent identity
  - off-model silhouette
  - blurry
  - noisy outline
  - cropped subject
frames:
  - id: idle_00
    role: animation_frame
    pose_guide: pose_guides/idle_00_pose.png
    silhouette_guide: silhouette_guides/idle_00_silhouette.png
    composition_guide: null
    prompt_tags: [idle, side view]
    pivot: bottom-center
generation:
  candidates_per_frame: 4
  resolution: 768x768
  format: png
  texture_role: sprite
  compose_prompt: true
  reference_policy: use style_ref and all identity_ref images for every frame
selection:
  reject_if:
    - identity differs from identity_ref
    - subject is cropped
    - final cell preview loses gameplay or UI readability
postprocess:
  alpha: edge-connected background extraction only
  outline: normalize to declared final-scale width
  palette: quantize to declared palette after detail cleanup
  scale: downscale from work_canvas to final_cell per accepted frame
  pivot: asset-profile default unless overridden per frame
export:
  sprite_sheet: <asset_id>_sheet.png
  metadata: <asset_id>_metadata.json
  cell_size: [256, 256]
  frame_order: [idle_00]
```

### 5.1 Profile-Specific Required Fields

| Profile | Required additions |
|---------|--------------------|
| `pc_character`, `npc_character` | `frames[].pose_guide`, `frames[].silhouette_guide`, `frames[].pivot`, `baseline_px`, movement/action state tags. |
| `monster_character` | `scale_class`, `hitbox_hint`, `attack_tell_notes`, silhouette readability constraints, allowed limb/head count when non-humanoid. |
| `item_icon` | `icon_safe_area`, `rarity_frame_policy`, `inventory_size_class`, `material_readability_notes`. |
| `skill_icon` | `skill_school`, `element`, `cooldown_overlay_safe_area`, `no_text=true`, `readability_notes`. |
| `world_pickup_sprite` | `world_scale_hint`, `ground_contact_policy`, `billboard_pivot`, optional idle sparkle/effect frames. |
| `effect_sprite` | `flow_direction`, `additive_or_alpha_blend`, `frame_energy_curve`, `looping_policy`, `impact_center`. |

---

## 6. Profile Workflows And Effects

| Profile | How production proceeds | Expected effect |
|---------|-------------------------|-----------------|
| `pc_character` | Lock identity, build frame pose/silhouette guides, generate 768x768 candidates per frame, select by identity/silhouette/readability, inpaint small defects, downscale to 256 cells, export sheet metadata. | Consistent playable character animation with stable pivot, baseline, costume, weapon, and final-cell readability. |
| `npc_character` | Reuse PC flow but reduce action complexity; prioritize faction/costume identity, idle/talk/emote/walk loops, and portrait/readability variants. | Faster NPC sheet production without PC-only combat assumptions; shared style while preserving role/faction distinction. |
| `monster_character` | Declare anatomy and gameplay tells first; use silhouette as primary control, optional landmark pose guides, then verify attack windup/hurt/death readability. | Monsters can have non-human anatomy without being rejected as "extra limbs"; gameplay tells become more readable and consistent. |
| `item_icon` | Use shape/silhouette guide and material refs, generate centered icon candidates, reject tiny-detail-dependent designs, reserve rarity/quantity overlay safe areas. | Inventory icons become readable at small sizes and compatible with UI overlays without baking UI chrome into the art. |
| `skill_icon` | Use composition guide for element, motion direction, focal shape, and overlay-safe region; forbid text/hotkeys/cooldown numbers in generation. | Skill families become visually consistent, distinguishable under cooldown/disabled states, and easier to scan in a hotbar. |
| `world_pickup_sprite` | Define world scale, ground contact, pivot, and optional idle sparkle; use sprite role and simple silhouette guides. | Dropped items read in-world and line up with interaction/pickup pivots instead of behaving like flat UI icons. |
| `effect_sprite` | Define flow direction, impact center, blend mode, and energy curve; generate flipbook frames with strict cell/anchor consistency. | VFX flipbooks animate smoothly and remain reusable by Niagara/Paper2D-style sprite consumers. |

---

## 7. Guide Contracts

### 7.1 Character And NPC Guides

- Pose guide controls joints, weapon anchors, feet contacts, head center, shoulder/hip line, and animation timing.
- Silhouette guide controls final occupancy, crop safety, readable negative space, equipment extents, and baseline stability.
- Preserve identity, costume, face, weapon/tool, body proportion, and gameplay-facing scale across frames.
- Reject changed identity, changed equipment, cropped feet, unstable baseline, or unreadable hands/tools at final cell size.

### 7.2 Monster Guides

- Non-human anatomy must be declared, including intended limb count, head count, tail, wings, horns, or extra appendages.
- Silhouette is the primary gameplay-readability control. Pose guides can be skeletal, blob-based, or landmark-based.
- Attack windup, vulnerable, hurt, and death states must read at gameplay camera distance.
- Reject unplanned appendages or off-model anatomy, not anatomy explicitly declared in the spec.

### 7.3 Item And Skill UI Guides

- Use silhouette, shape, or composition guides instead of pose guides.
- The icon must read at final UI size without tiny text, noisy engraving, or accidental watermark-like detail.
- Keep the subject inside the icon safe area and reserve optional corners for rarity, quantity, lock, hotkey, or cooldown overlays.
- Skill family consistency matters: element color, silhouette language, and intensity hierarchy must be controlled across a skill set.

### 7.4 Effect Guides

- Use shape/flow guides to control arc, impact center, projectile direction, or burst radius.
- Declare blend mode expectations before generation: alpha sprite, additive glow, or decal-like output.
- Flipbook frames need consistent canvas, anchor, effect center, and energy progression.

---

## 8. Production Workflow

1. Lock asset design.
   - Character/monster: identity, anatomy, costume, scale, silhouette, weapon/appendage rules.
   - Item/skill UI: shape language, icon safe area, element/rarity family, overlay constraints.

2. Separate identity references from style references.
   - Identity refs define what the asset is.
   - Style refs define finish quality only.
   - Style refs must not override anatomy, item shape, or skill motif.

3. Create required guides per frame or icon cell.
   - Character and monster animation: pose plus silhouette when anatomy supports it.
   - Item/skill UI: silhouette, shape, or composition guide.
   - Effect sprites: flow/composition guide and optional silhouette.

4. Generate candidates on the work canvas.
   - Use all required refs and guides.
   - Generate enough candidates per frame/cell to select by contract, not by first acceptable output.

5. Select candidates.
   - Selection priority: identity/profile correctness, silhouette/composition, final-size readability, polish.

6. Fix details with inpainting.
   - Use inpainting for local cleanup only.
   - Do not rescue wrong pose, wrong silhouette, changed identity, wrong item shape, or mismatched skill motif.

7. Postprocess alpha, outline, palette, scale, and pivot.
   - Use edge-connected background extraction for chroma/flat backgrounds.
   - Do not globally delete a color when that color may exist inside the subject.

8. Crop/downscale to final cells.
   - Verify final-size readability directly.
   - UI icons must be tested under normal, hover, disabled, and cooldown/quantity overlay states when applicable.

9. Merge sheet/atlas and export metadata.
   - Sprite sheets and icon atlases must use strict identical cells.
   - Metadata must include cell rects, frame/cell ids, pivot, source guide paths, seed/candidate id, postprocess notes, and runtime role.

---

## 9. Monolith Integration And Module Ownership

`MonolithImageGen` remains the provider and generated Texture2D boundary. The full sprite workflow is broader than image generation: it validates specs, checks guide readiness, plans candidate batches, validates selected cells, exports metadata, and coordinates profile-specific rules. That orchestration belongs in the `MonolithSprite` module.

| Module | Ownership |
|--------|-----------|
| `MonolithImageGen` | Provider discovery, ima2 bridge calls, generated PNG import, generated-image provenance. |
| `MonolithAsset` | Texture2D ingest, role-aware postprocess, alpha extraction, alpha bleed, mip/LOD/addressing settings, role validation. |
| `MonolithPaper2D` | Current read-only Paper2D AssetRegistry inspection. Future slicing/flipbook mutation should remain Paper2D-owned if implemented. |
| `MonolithSprite` | Sprite production orchestration: asset spec validation, profile rules, guide manifest validation, candidate plan export, sheet/atlas metadata validation, profile-aware prompt bundle creation, final sprite package QA. |

### 9.1 `MonolithSprite` Milestones

| Milestone | Actions | Purpose |
|-----------|---------|---------|
| M1 read/validate | `sprite.validate_asset_spec`, `sprite.validate_guides`, `sprite.build_candidate_plan` | Implemented. Adds value without duplicating ImageGen or mutating assets. |
| M2 package/export | `sprite.validate_sheet`, `sprite.export_metadata`, `sprite.build_preview_contact_sheet` | Implemented. Verifies final cells, pivots, overlays, and metadata before runtime import. |
| M3 orchestration | `sprite.prepare_imagegen_requests`, `sprite.run_generation_batch` | Implemented. Request preparation is dry-run by default; batch execution requires `execute=true` and delegates to `imagegen` or another explicit provider action. |
| M4 runtime authoring | Optional Paper2D/PaperZD slicing/flipbook integration | Implement only if it cannot be better owned by `MonolithPaper2D` or a project commandlet. |

`MonolithSprite` must not duplicate provider code, Texture2D import code, or Paper2D asset mutation logic. It should orchestrate and validate, then call or hand off to the owning modules.

---

## 10. Current Concrete Instance

| Asset | Asset spec | Profile | Current guide state |
|-------|------------|---------|---------------------|
| `hero_knight_v01` | [../assets/hero_knight_v01/asset_spec.yaml](../assets/hero_knight_v01/asset_spec.yaml) | `pc_character` | Pose and silhouette guides exist for `idle_00`, `walk_00`, `walk_01`, `walk_02`, and `walk_03`. |

---

## 11. Acceptance Criteria

| Gate | Evidence |
|------|----------|
| Profile declared | `asset_spec.yaml` includes `asset_profile`, `target_surface`, `texture_role`, dimensions, refs, negative prompts, postprocess, and export contract. |
| Guides match profile | Required guide files exist for every frame/cell and match the chosen profile's guide contract. |
| Candidate coverage | Candidates are generated per frame/cell on the declared work canvas using required refs/guides/seeds. |
| Selection quality | Selected outputs preserve identity, anatomy/shape, composition, crop safety, and final-size readability. |
| Postprocess quality | Alpha, outline, palette, scale, and pivot match the profile contract. |
| Sheet or atlas integrity | Final sheet/atlas uses identical cells, no overlap, no boundary bleed, and complete metadata. |
| Runtime import | Imported Texture2D uses the declared role settings and passes role-specific validation warnings review. |
