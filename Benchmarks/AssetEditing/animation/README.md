# AssetEditing: animation

Generated AssetType slice for the AssetEditing benchmark.

## Supported Operations

| Operation | Tasks |
|---|---:|
| `creation_or_import` | 28 |
| `edit` | 28 |
| `save` | 28 |
| `readback_verify` | 28 |

## Lifecycle

| Lifecycle phase | Tasks |
|---|---:|
| `create_save` | 28 |

## Test Cases

| Edit domain | Tasks | Module | Case file |
|---|---:|---|---|
| `aim_offset_axis` | 1 | `asset_authoring.animation.aim_offset_axis` | `testcases\aim_offset_axis.json` |
| `anim_blueprint_motion_matching` | 1 | `asset_authoring.animation.anim_blueprint_motion_matching` | `testcases\anim_blueprint_motion_matching.json` |
| `anim_blueprint_state_machine` | 1 | `asset_authoring.animation.anim_blueprint_state_machine` | `testcases\anim_blueprint_state_machine.json` |
| `animation_core` | 1 | `asset_authoring.animation.core` | `testcases\core.json` |
| `animation_sequence_events` | 1 | `asset_authoring.animation.sequence_events` | `testcases\sequence_events.json` |
| `blendspace_1d_aimoffset_1d` | 1 | `asset_authoring.animation.blendspace_1d_aimoffset_1d` | `testcases\blendspace_1d_aimoffset_1d.json` |
| `blendspace_interpolation` | 1 | `asset_authoring.animation.blendspace_interpolation` | `testcases\blendspace_interpolation.json` |
| `blendspace_sample_edit` | 1 | `asset_authoring.animation.blendspace_sample_edit` | `testcases\blendspace_sample_edit.json` |
| `blendspace_samples` | 1 | `asset_authoring.animation.blendspace_samples` | `testcases\blendspace_samples.json` |
| `bone_track_keys` | 1 | `asset_authoring.animation.bone_track_keys` | `testcases\bone_track_keys.json` |
| `chooser_context_class` | 1 | `asset_authoring.animation.chooser_context_class` | `testcases\chooser_context_class.json` |
| `chooser_result_reference_rewrite` | 1 | `asset_authoring.animation.chooser_result_reference_rewrite` | `testcases\chooser_result_reference_rewrite.json` |
| `chooser_table` | 1 | `asset_authoring.animation.chooser_table` | `testcases\chooser_table.json` |
| `chooser_tree_duplicate_remap` | 1 | `asset_authoring.animation.chooser_tree_duplicate_remap` | `testcases\chooser_tree_duplicate_remap.json` |
| `composite_mirror_table` | 1 | `asset_authoring.animation.composite_mirror_table` | `testcases\composite_mirror_table.json` |
| `ik_retargeter_pose_settings` | 1 | `asset_authoring.animation.ik_retargeter_pose_settings` | `testcases\ik_retargeter_pose_settings.json` |
| `ikrig_retargeter` | 1 | `asset_authoring.animation.ikrig_retargeter` | `testcases\ikrig_retargeter.json` |
| `ikrig_solver_stack` | 1 | `asset_authoring.animation.ikrig_solver_stack` | `testcases\ikrig_solver_stack.json` |
| `montage` | 1 | `asset_authoring.animation.montage` | `testcases\montage.json` |
| `montage_blend_slot` | 1 | `asset_authoring.animation.montage_blend_slot` | `testcases\montage_blend_slot.json` |
| `montage_from_sections` | 1 | `asset_authoring.animation.montage_from_sections` | `testcases\montage_from_sections.json` |
| `notify_point_crud` | 1 | `asset_authoring.animation.notify_point_crud` | `testcases\notify_point_crud.json` |
| `notify_state_timing` | 1 | `asset_authoring.animation.notify_state_timing` | `testcases\notify_state_timing.json` |
| `pose_search_database` | 1 | `asset_authoring.animation.pose_search_database` | `testcases\pose_search_database.json` |
| `pose_search_normalization_set` | 1 | `asset_authoring.animation.pose_search_normalization_set` | `testcases\pose_search_normalization_set.json` |
| `sequence_curve_marker_cleanup` | 1 | `asset_authoring.animation.sequence_curve_marker_cleanup` | `testcases\sequence_curve_marker_cleanup.json` |
| `sequence_from_poses` | 1 | `asset_authoring.animation.sequence_from_poses` | `testcases\sequence_from_poses.json` |
| `sequence_root_additive_duplicate` | 1 | `asset_authoring.animation.sequence_root_additive_duplicate` | `testcases\sequence_root_additive_duplicate.json` |

## Common Commands

```powershell
python Scripts\asset_editing_benchmark.py select --testset-module asset_authoring.animation
```

`tasks.jsonl` contains the full task payloads for this AssetType. `testcases\*.json` files contain one edit-domain slice each.
