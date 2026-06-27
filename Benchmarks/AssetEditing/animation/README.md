# AssetEditing: animation

Generated AssetType slice for the AssetEditing benchmark.

## Supported Operations

| Operation | Tasks |
|---|---:|
| `creation_or_import` | 38 |
| `edit` | 38 |
| `save` | 38 |
| `readback_verify` | 38 |

## Lifecycle

| Lifecycle phase | Tasks |
|---|---:|
| `create_save` | 38 |

## Test Cases

| Case | Tasks | Module | Case file |
|---|---:|---|---|
| `aim_offset_axis` | 1 | `asset_authoring.animation.aim_offset_axis` | `testcases\aim_offset_axis.json` |
| `anim_blueprint_motion_matching` | 1 | `asset_authoring.animation.anim_blueprint_motion_matching` | `testcases\anim_blueprint_motion_matching.json` |
| `anim_blueprint_state_machine` | 1 | `asset_authoring.animation.anim_blueprint_state_machine` | `testcases\anim_blueprint_state_machine.json` |
| `core` | 1 | `asset_authoring.animation.core` | `testcases\core.json` |
| `sequence_events` | 1 | `asset_authoring.animation.sequence_events` | `testcases\sequence_events.json` |
| `blendspace_1d_aimoffset_1d` | 1 | `asset_authoring.animation.blendspace_1d_aimoffset_1d` | `testcases\blendspace_1d_aimoffset_1d.json` |
| `blendspace_interpolation` | 1 | `asset_authoring.animation.blendspace_interpolation` | `testcases\blendspace_interpolation.json` |
| `blendspace_sample_delete` | 1 | `asset_authoring.animation.blendspace_sample_delete` | `testcases\blendspace_sample_delete.json` |
| `blendspace_sample_edit` | 1 | `asset_authoring.animation.blendspace_sample_edit` | `testcases\blendspace_sample_edit.json` |
| `blendspace_samples` | 1 | `asset_authoring.animation.blendspace_samples` | `testcases\blendspace_samples.json` |
| `bone_track_keys` | 1 | `asset_authoring.animation.bone_track_keys` | `testcases\bone_track_keys.json` |
| `bone_track_lifecycle` | 1 | `asset_authoring.animation.bone_track_lifecycle` | `testcases\bone_track_lifecycle.json` |
| `chooser_context_class` | 1 | `asset_authoring.animation.chooser_context_class` | `testcases\chooser_context_class.json` |
| `chooser_result_reference_rewrite` | 1 | `asset_authoring.animation.chooser_result_reference_rewrite` | `testcases\chooser_result_reference_rewrite.json` |
| `chooser_table` | 1 | `asset_authoring.animation.chooser_table` | `testcases\chooser_table.json` |
| `chooser_tree_duplicate_remap` | 1 | `asset_authoring.animation.chooser_tree_duplicate_remap` | `testcases\chooser_tree_duplicate_remap.json` |
| `composite_mirror_table` | 1 | `asset_authoring.animation.composite_mirror_table` | `testcases\composite_mirror_table.json` |
| `ik_retargeter_pose_settings` | 1 | `asset_authoring.animation.ik_retargeter_pose_settings` | `testcases\ik_retargeter_pose_settings.json` |
| `ikrig_retargeter` | 1 | `asset_authoring.animation.ikrig_retargeter` | `testcases\ikrig_retargeter.json` |
| `ikrig_solver_stack` | 1 | `asset_authoring.animation.ikrig_solver_stack` | `testcases\ikrig_solver_stack.json` |
| `modifier_stack` | 1 | `asset_authoring.animation.modifier_stack` | `testcases\modifier_stack.json` |
| `montage` | 1 | `asset_authoring.animation.montage` | `testcases\montage.json` |
| `montage_blend_slot` | 1 | `asset_authoring.animation.montage_blend_slot` | `testcases\montage_blend_slot.json` |
| `montage_from_sections` | 1 | `asset_authoring.animation.montage_from_sections` | `testcases\montage_from_sections.json` |
| `montage_section_crud` | 1 | `asset_authoring.animation.montage_section_crud` | `testcases\montage_section_crud.json` |
| `notify_batch_track_clone` | 1 | `asset_authoring.animation.notify_batch_track_clone` | `testcases\notify_batch_track_clone.json` |
| `notify_point_crud` | 1 | `asset_authoring.animation.notify_point_crud` | `testcases\notify_point_crud.json` |
| `notify_state_timing` | 1 | `asset_authoring.animation.notify_state_timing` | `testcases\notify_state_timing.json` |
| `pose_search_database` | 1 | `asset_authoring.animation.pose_search_database` | `testcases\pose_search_database.json` |
| `pose_search_normalization_set` | 1 | `asset_authoring.animation.pose_search_normalization_set` | `testcases\pose_search_normalization_set.json` |
| `pose_search_schema_channels` | 1 | `asset_authoring.animation.pose_search_schema_channels` | `testcases\pose_search_schema_channels.json` |
| `retarget_chain_lifecycle` | 1 | `asset_authoring.animation.retarget_chain_lifecycle` | `testcases\retarget_chain_lifecycle.json` |
| `retarget_chain_mapping` | 1 | `asset_authoring.animation.retarget_chain_mapping` | `testcases\retarget_chain_mapping.json` |
| `retarget_chain_settings` | 1 | `asset_authoring.animation.retarget_chain_settings` | `testcases\retarget_chain_settings.json` |
| `sequence_curve_marker_cleanup` | 1 | `asset_authoring.animation.sequence_curve_marker_cleanup` | `testcases\sequence_curve_marker_cleanup.json` |
| `sequence_from_poses` | 1 | `asset_authoring.animation.sequence_from_poses` | `testcases\sequence_from_poses.json` |
| `sequence_pose_copy` | 1 | `asset_authoring.animation.sequence_pose_copy` | `testcases\sequence_pose_copy.json` |
| `sequence_root_additive_duplicate` | 1 | `asset_authoring.animation.sequence_root_additive_duplicate` | `testcases\sequence_root_additive_duplicate.json` |

## Common Commands

```powershell
python Scripts\asset_editing_benchmark.py select --testset-module asset_authoring.animation
```

`tasks.jsonl` contains the full task payloads for this AssetType. `testcases\*.json` files contain one edit-domain slice each.
