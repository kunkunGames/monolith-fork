---
name: unreal-animation
description: Use when inspecting or editing Unreal animation assets via the Monolith animation namespace — sequences, montages, blend spaces, animation blueprints (ABP) and their state machines, notifies, curves, sync markers, skeletons, IKRig, IK Retargeter, Control Rig. For an AnimBP STATE MACHINE this is the right skill; for a LogicDriver SM asset use unreal-logicdriver, for a ComboGraph attack chain use unreal-combograph, for a GAS ability or montage-task use unreal-gas, for data-driven anim SELECTION use unreal-chooser. Triggers on animation, montage, ABP, state machine, blend space, notify, anim notify state, anim sequence, play montage, additive animation, root motion, anim layer, morph target, pose asset, aim offset, slot, skeleton, IKRig, IK rig, retarget animation, retargeter, IK retargeter, batch retarget, control rig.
---

# Unreal Animation Workflows

Drives the **animation** namespace via `animation_query()` for inspecting and editing anim sequences, montages, blend spaces, animation blueprints (ABP), skeletons, IKRig, IK Retargeter, and Control Rig assets. The table below is a curated snapshot of the most-used actions, not the full catalog — always discover the live action set and confirm an action's exact parameter schema before calling it.

```
// 1. List the live action set for this namespace
monolith_discover({ namespace: "animation" })

// 2. Get the exact parameter schema for a specific action before calling it
describe_query({ action: "action_schema", params: { namespace: "animation", action: "add_montage_section" } })
// equivalent mode form: animation_query({ action: "add_montage_section", mode: "schema" })
```

## When to use / Use a different skill for

- **This skill (unreal-animation):** anim sequences, montages, blend spaces, ABP graphs and their **state machines**, notifies, curves, sync markers, skeletons, IKRig, IK Retargeter, Control Rig.
- **unreal-logicdriver:** when "state machine" means a LogicDriver SM asset, not an AnimBP state machine.
- **unreal-combograph:** when the montage is part of a ComboGraph combo/attack chain.
- **unreal-gas:** when the animation drives a gameplay ability or montage task.
- **unreal-chooser:** when you need data-driven animation SELECTION via a chooser table.

## Key Parameters

- `asset_path` -- animation asset path (e.g. `/Game/Animations/ABP_Player`)
- `machine_name` -- state machine name (from `get_state_machines`)
- `state_name` -- state within a machine
- `graph_name` -- optional graph filter for `get_nodes`

## Action Reference

Full per-action parameter signatures — grouped by category (Montage, Blend Space, Sequence, ABP Reading, ABP Writing, Notifies, Curves, Sync Markers, Bone Tracks, Skeleton, IKRig / Retargeter, Control Rig, Physics Asset, Motion Matching / Pose Search, Batch / PIE) — live in [`references/actions.md`](references/actions.md). Those signatures are a snapshot of the live catalog; the Discovery block above stays the authority — confirm an action's exact, full, current schema with `monolith_discover` (`mode: "schema"`, or `animation_query({ action, mode: "schema" })`) before calling.

## Common Workflows

Each recipe is a numbered sequence of real `animation_query` calls. Edits are live in the editor; read the list action first so index-based edits target the right element, then re-read to confirm. All actions below are from this skill's Action Reference table.

### Inspect ABP state machines
```
animation_query({ action: "get_state_machines", params: { asset_path: "/Game/Animations/ABP_Player" } })
animation_query({ action: "get_transitions", params: { asset_path: "/Game/Animations/ABP_Player", machine_name: "Locomotion" } })
```

### Recipe — Add a notify to a montage (inspect → add → verify)
```
// 1. Inspect montage layout to pick the section/time the notify should fire at
animation_query({ action: "get_montage_info", params: { asset_path: "/Game/Animations/AM_Attack" } })
// 2. Read existing notifies so you know current indices before mutating
animation_query({ action: "get_sequence_notifies", params: { asset_path: "/Game/Animations/AM_Attack" } })
// 3. Add the point notify at the chosen time on a named track
animation_query({ action: "add_notify", params: { asset_path: "/Game/Animations/AM_Attack", notify_class: "AnimNotify_PlaySound", time: 0.35, track_name: "1" } })
// 4. Re-read to confirm placement and capture the new notify_index (later edits index by notify_index, not name)
animation_query({ action: "get_sequence_notifies", params: { asset_path: "/Game/Animations/AM_Attack" } })
```

### Recipe — Classless named montage action point (add → verify → change tick type)
```
// 1. Inspect existing tracks/notifies. Index-based edits always use this fresh readback.
animation_query({ action: "get_sequence_notifies", params: { asset_path: "/Game/Animations/AM_Attack" } })
// 2. Add a named event without creating or substituting a UAnimNotify class.
animation_query({ action: "add_named_notify", params: { asset_path: "/Game/Animations/AM_Attack", notify_name: "ActionPoint", time: 0.35, track_name: "Gameplay", montage_tick_type: "BranchingPoint" } })
// 3. Re-read and use the returned index; every row includes montage_tick_type.
animation_query({ action: "get_sequence_notifies", params: { asset_path: "/Game/Animations/AM_Attack" } })
// 4. Change only the event-level tick mode. This action intentionally rejects class-backed notifies.
animation_query({ action: "set_notify_tick_type", params: { asset_path: "/Game/Animations/AM_Attack", notify_index: 0, montage_tick_type: "Queued" } })
```

### Recipe — Add an AnimBP state-machine transition (EXPERIMENTAL writes; compile after)
```
// 1. List machines, then read the source/target states you will connect
animation_query({ action: "get_state_machines", params: { asset_path: "/Game/Animations/ABP_Player" } })
animation_query({ action: "get_state_info", params: { asset_path: "/Game/Animations/ABP_Player", machine_name: "Locomotion", state_name: "Idle" } })
// 2. Create the transition edge between two existing states
animation_query({ action: "add_transition", params: { asset_path: "/Game/Animations/ABP_Player", machine_name: "Locomotion", from_state: "Idle", to_state: "Run" } })
// 3. Wire the transition's condition (bool variable here; auto/compare also supported)
animation_query({ action: "set_transition_rule", params: { asset_path: "/Game/Animations/ABP_Player", machine_name: "Locomotion", from_state: "Idle", to_state: "Run", variable_name: "bIsMoving" } })
// 4. Confirm the rule resolved as intended (ABP writes are EXPERIMENTAL — verify and recompile in the editor)
animation_query({ action: "get_transition_rule", params: { asset_path: "/Game/Animations/ABP_Player", machine_name: "Locomotion", from_state: "Idle", to_state: "Run" } })
```

### Recipe — Set up montage section flow (intro → loop → outro)
```
animation_query({ action: "add_montage_section", params: { asset_path: "/Game/Animations/AM_Attack", section_name: "Intro", start_time: 0.0 } })
animation_query({ action: "add_montage_section", params: { asset_path: "/Game/Animations/AM_Attack", section_name: "Loop", start_time: 0.5 } })
animation_query({ action: "set_section_next", params: { asset_path: "/Game/Animations/AM_Attack", section_name: "Intro", next_section_name: "Loop" } })
```

### Recipe — Cross-skeleton retarget (build rigs → retargeter → map chains → batch)
```
// 1. Create (or inspect) the source and target IK Rigs
animation_query({ action: "create_ik_rig", params: { asset_path: "/Game/Animations/Retarget/IKR_Source", skeletal_mesh_path: "/Game/Characters/SK_Source" } })
animation_query({ action: "create_ik_rig", params: { asset_path: "/Game/Animations/Retarget/IKR_Target", skeletal_mesh_path: "/Game/Characters/SK_Target" } })
// 2. Create the retargeter and bind both rigs (auto_map is a string: fuzzy/exact)
animation_query({ action: "create_ik_retargeter", params: { asset_path: "/Game/Animations/Retarget/RTG_SourceToTarget", source_ik_rig_path: "/Game/Animations/Retarget/IKR_Source", target_ik_rig_path: "/Game/Animations/Retarget/IKR_Target", auto_map: "fuzzy" } })
// 3. Verify rig/chain mapping; re-map fuzzy or pair chains manually if needed
animation_query({ action: "get_retargeter_info", params: { asset_path: "/Game/Animations/Retarget/RTG_SourceToTarget" } })
animation_query({ action: "set_retarget_chain_mapping", params: { asset_path: "/Game/Animations/Retarget/RTG_SourceToTarget", auto_map: "fuzzy" } })
// 4. Batch-retarget a set of source animations into an output folder
animation_query({ action: "batch_retarget_animations", params: { retargeter_path: "/Game/Animations/Retarget/RTG_SourceToTarget", source_anims: ["/Game/Animations/Source/A_Run", "/Game/Animations/Source/A_Idle"], output_folder: "/Game/Animations/Target", auto_map: "fuzzy" } })
```

## Posed Skeletal-Mesh Capture (cross-namespace: editor ns)

This is an **editor**-namespace action, not an animation-table action — call it through `editor_query`, not `animation_query`.

`editor_query("capture_scene_preview", { asset_path: "/Game/.../SK_Char", asset_type: "skeletal_mesh", animation_path: "/Game/.../A_Idle", seek_time: 0.5 })` renders the skeletal mesh at the requested pose (omit `animation_path` for rest pose). Useful for visually verifying retarget output, montage section poses, or sequence keyframes. See `monolith_guide(section="recipes")` entry "Visual introspection — going beyond thumbnails".

## Rules

- Editing tools modify assets **live in the editor** -- changes are immediate
- Primary param is `asset_path` (not `asset`)
- `get_nodes` accepts optional `graph_name` filter
- Use `project_query("search", { query: "AM_*" })` to find animation assets first
- ABP write actions are **EXPERIMENTAL** -- always compile after and check for errors
- `set_retarget_chain_mapping`: `auto_map` is a string (`exact`/`fuzzy`/`clear`), not a bool -- OR pass explicit `source_chain`+`target_chain`
- `add_control_rig_element`: `animatable=true` default; control kinds via `control_type` (Float/Integer/Transform/Rotator/Position/Scale/Vector2D)
- Notify, section, blend-space, and bone-track edits index by `*_index` (`notify_index`, `section_index`, `sample_index`), not by name -- read the list action first (`get_sequence_notifies`, `get_montage_info`, `get_blend_space_info`, `list_bone_tracks`)
- Montage blend options use exact `EAlphaBlendOption` tokens such as `HermiteCubic`; `get_montage_info` returns `blend_in_option` / `blend_out_option` for readback.
- `add_named_notify` creates a classless named event and requires exactly one existing `track_name` or `track_index`; `set_notify_tick_type` is limited to those classless named events.
- Bone params are `bone_name` / `source_bone` / `target_bone` / `bone_names`, not `bone`/`source`/`target`/`bones`
