---
name: unreal-animation
description: Use when inspecting or editing Unreal animation assets via Monolith MCP — sequences, montages, blend spaces, animation blueprints, notifies, curves, sync markers, skeletons, IKRig, IK Retargeter, Control Rig. Triggers on animation, montage, ABP, blend space, notify, anim sequence, skeleton, IKRig, retargeter, control rig.
---

# Unreal Animation Workflows

**135 animation actions** via `animation_query()`. Discover with `monolith_discover({ namespace: "animation" })`.

## Key Parameters

- `asset_path` -- animation asset path (e.g. `/Game/Animations/ABP_Player`)
- `machine_name` -- state machine name (from `get_state_machines`)
- `state_name` -- state within a machine
- `graph_name` -- optional graph filter for `get_nodes`

## Action Reference

| Action | Key Params | Purpose |
|--------|-----------|---------|
| **Montage** | | |
| `add_montage_section` | `asset_path`, `name`, `time` | Add named section |
| `delete_montage_section` | `asset_path`, `name` | Remove section |
| `set_section_next` | `asset_path`, `section`, `next` | Set section playback order |
| `set_section_time` | `asset_path`, `section`, `time` | Move section to specific time |
| **Blend Space** | | |
| `add_blendspace_sample` | `asset_path`, `animation`, `x`, `y` | Add animation at X/Y |
| `edit_blendspace_sample` | `asset_path`, `index`, `x`, `y` | Move existing sample |
| `delete_blendspace_sample` | `asset_path`, `index` | Remove sample point |
| **ABP Reading** | | |
| `get_state_machines` | `asset_path` | List all state machines |
| `get_state_info` | `asset_path`, `machine_name`, `state_name` | State details |
| `get_transitions` | `asset_path`, `machine_name` | Transition rules |
| `get_blend_nodes` | `asset_path` | Blend node trees |
| `get_linked_layers` | `asset_path` | Linked anim layers |
| `get_graphs` | `asset_path` | All graphs in ABP |
| `get_nodes` | `asset_path`, `graph_name`? | Nodes in graph(s) |
| `get_abp_variables` | `asset_path` | Variables with types and defaults |
| `get_abp_linked_assets` | `asset_path` | Referenced anim assets via Asset Registry |
| **ABP Writing (EXPERIMENTAL)** | | |
| `add_state_to_machine` | `asset_path`, `machine_name`, `state_name`, `position_x/y`? | Add state |
| `add_transition` | `asset_path`, `machine_name`, `from_state`, `to_state` | Add transition |
| `set_transition_rule` | `asset_path`, `machine_name`, `from_state`, `to_state`, `variable_name` | Wire boolean condition |
| **Notifies** | | |
| `set_notify_time` | `asset_path`, `notify`, `time` | Move notify |
| `set_notify_duration` | `asset_path`, `notify`, `duration` | Set notify state duration |
| **Bone Tracks** | | |
| `set_bone_track_keys` | `asset_path`, `bone`, `keys` | Set keyframes |
| `add_bone_track` | `asset_path`, `bone` | Add track |
| `remove_bone_track` | `asset_path`, `bone` | Remove track |
| **Skeleton** | | |
| `add_virtual_bone` | `asset_path`, `source`, `target` | Create virtual bone |
| `remove_virtual_bones` | `asset_path`, `bones` | Remove virtual bones |
| `get_skeleton_info` | `asset_path` | Bone hierarchy, sockets, virtual bones |
| `get_skeletal_mesh_info` | `asset_path` | Mesh details, LODs, materials |
| **IKRig** | | |
| `get_ikrig_info` | `asset_path` | Solvers, goals, chains, skeleton |
| `add_ik_solver` | `asset_path`, `solver_type`, `root_bone`?, `goals`? | Add solver + goals |
| `get_retargeter_info` | `asset_path` | Source/target rigs, chain mappings |
| `set_retarget_chain_mapping` | `asset_path`, `auto_map`? OR `source_chain`+`target_chain` | Map chains |
| **Control Rig** | | |
| `get_control_rig_info` | `asset_path`, `element_type`? | Hierarchy -- bones, controls, nulls |
| `get_control_rig_variables` | `asset_path` | Animatable controls and BP variables |
| `add_control_rig_element` | `asset_path`, `element_type`, `name`, `parent`?, `control_type`?, `animatable`?, `transform`? | Add bone/control/null |

## Common Workflows

### Inspect ABP state machines
```
animation_query({ action: "get_state_machines", params: { asset_path: "/Game/Animations/ABP_Player" } })
animation_query({ action: "get_transitions", params: { asset_path: "/Game/Animations/ABP_Player", machine_name: "Locomotion" } })
```

### Set up montage section flow (intro -> loop -> outro)
```
animation_query({ action: "add_montage_section", params: { asset_path: "/Game/Animations/AM_Attack", name: "Intro", time: 0.0 } })
animation_query({ action: "add_montage_section", params: { asset_path: "/Game/Animations/AM_Attack", name: "Loop", time: 0.5 } })
animation_query({ action: "set_section_next", params: { asset_path: "/Game/Animations/AM_Attack", section: "Intro", next: "Loop" } })
```

## Rules

- Editing tools modify assets **live in the editor** -- changes are immediate
- Primary param is `asset_path` (not `asset`)
- `get_nodes` accepts optional `graph_name` filter
- Use `project_query("search", { query: "AM_*" })` to find animation assets first
- ABP write actions are **EXPERIMENTAL** -- always compile after and check for errors
- `set_retarget_chain_mapping`: `auto_map: true` for automatic OR explicit `source_chain`+`target_chain`
- `add_control_rig_element`: `animatable` uses `IsAnimatable()` internally -- not a raw bool field
