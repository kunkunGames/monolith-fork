# unreal-animation — Action Reference

These per-action signatures are a snapshot of the live catalog — confirm the exact, full, current schema of any action via `monolith_discover` with `mode: "schema"` (or `animation_query({ action, mode: "schema" })`). The discover-first block in `../SKILL.md` is the authority.

Param notation: `name*` required, `name?` optional, `name=val` default, `a/b/c` allowed values, `[w]` mutates (transaction-wrapped). Signatures are a snapshot of the live catalog -- for the exact, full, current schema of any action call `monolith_discover` with `mode: "schema"` (or `animation_query({ action, mode: "schema" })`).

| Action | Params (req* opt? =default) | Purpose |
|--------|-----------|---------|
| **Montage** | | |
| `[w] add_montage_section` | `asset_path*`, `section_name*`, `start_time*` | Add named section |
| `[w] delete_montage_section` | `asset_path*`, `section_index*` | Remove section by index |
| `[w] set_section_next` | `asset_path*`, `section_name*`, `next_section_name*` | Set section playback order |
| `[w] set_section_time` | `asset_path*`, `section_name*`, `new_time*` | Move section to specific time |
| `get_montage_info` | `asset_path*` | Sections, slots, blend times/options, auto blend-out settings |
| `[w] create_montage` | `asset_path*`, `skeleton_path*` | New empty montage |
| `[w] set_montage_blend` | `asset_path*`, `blend_in_time?`, `blend_out_time?`, `blend_in_option?`, `blend_out_option?`, `blend_out_trigger_time?`, `enable_auto_blend_out?`; options: `Linear/Cubic/HermiteCubic/Sinusoidal/QuadraticInOut/CubicInOut/QuarticInOut/QuinticInOut/CircularIn/CircularOut/CircularInOut/ExpIn/ExpOut/ExpInOut/Custom` | Blend times, exact `EAlphaBlendOption` modes, trigger, auto blend-out |
| `[w] add_montage_slot` | `asset_path*`, `slot_name*` | Add slot track |
| `[w] set_montage_slot` | `asset_path*`, `slot_index*`, `slot_name*` | Rename a slot track by index |
| `[w] add_montage_anim_segment` | `asset_path*`, `anim_path*`, `slot_index?`, `start_pos?`, `anim_start_time?`, `anim_end_time?`, `play_rate?`, `looping_count?` | Add anim segment to slot |
| **Blend Space** | | |
| `[w] add_blendspace_sample` | `asset_path*`, `anim_path*`, `x*`, `y*` | Add animation at X/Y |
| `[w] edit_blendspace_sample` | `asset_path*`, `sample_index*`, `x*`, `y*`, `anim_path?` | Move/retarget existing sample |
| `[w] delete_blendspace_sample` | `asset_path*`, `sample_index*` | Remove sample point |
| `get_blend_space_info` | `asset_path*` | Samples and axis settings |
| `[w] set_blend_space_axis` | `asset_path*`, `axis*` (X/Y), `name?`, `min?`, `max?`, `grid_divisions?`, `snap_to_grid?`, `wrap_input?` | Configure an axis |
| `[w] create_blend_space` | `asset_path*`, `skeleton_path*`, `axis_x_name?`, `axis_x_min?`, `axis_x_max?`, `axis_y_name?`, `axis_y_min?`, `axis_y_max?` | New 2D blend space |
| `[w] create_aim_offset` | `asset_path*`, `skeleton_path*`, `axis_x_name=Yaw`, `axis_x_min=-180`, `axis_x_max=180`, `axis_y_name=Pitch`, `axis_y_min=-90`, `axis_y_max=90` | New 2D aim offset |
| **Sequence** | | |
| `get_sequence_info` | `asset_path*` | Duration, frames, root motion, compression |
| `[w] create_sequence` | `asset_path*`, `skeleton_path*` | New empty sequence |
| `[w] duplicate_sequence` | `source_path*`, `dest_path*` | Copy a sequence |
| `[w] set_sequence_properties` | `asset_path*`, `rate_scale?`, `loop?`, `interpolation?` (Linear/Step) | Playback props |
| `[w] set_root_motion_settings` | `asset_path*`, `enable_root_motion?`, `root_motion_lock?` (AnimFirstFrame/Zero/RefPose), `force_root_lock?` | Root motion config |
| `[w] set_additive_settings` | `asset_path*`, `additive_anim_type?` (NoAdditive/LocalSpace/MeshSpace), `ref_pose_type?` (RefPose/AnimScaled/AnimFrame/LocalAnimFrame), `ref_frame_index?`, `ref_pose_seq?` | Additive config (rebuilds DDC) |
| **ABP Reading** | | |
| `get_abp_info` | `asset_path*` | Skeleton, graphs, machines, variables, interfaces |
| `get_state_machines` | `asset_path*` | List all state machines |
| `get_state_info` | `asset_path*`, `machine_name*`, `state_name*` | State details |
| `get_transitions` | `asset_path*`, `machine_name?` | Transition rules (filter by machine) |
| `get_transition_rule` | `asset_path*`, `machine_name*`, `from_state*`, `to_state*` | Read one transition's rule |
| `get_blend_nodes` | `asset_path*`, `graph_name?` | Blend node trees |
| `get_linked_layers` | `asset_path*` | Linked anim layers |
| `get_graphs` | `asset_path*` | All graphs in ABP |
| `get_nodes` | `asset_path*`, `node_class_filter?`, `graph_name?`, `include_anim_graph=false` | Nodes in graph(s) |
| `get_abp_variables` | `asset_path*` | Variables with types and defaults |
| `get_abp_linked_assets` | `asset_path*` | Referenced anim assets via Asset Registry |
| `get_anim_graph_choosers` | `asset_path*`, `recursive=false` | EvaluateChooser nodes + resolved chooser asset |
| `get_anim_graph_output_connection` | `abp_path*`, `graph_name=AnimGraph` | Verify Output Pose is driven |
| **ABP Writing (EXPERIMENTAL)** | | |
| `[w] create_anim_blueprint` | `asset_path*`, `skeleton_path*`, `parent_class?` | New ABP asset |
| `[w] create_state_machine` | `asset_path*`, `state_machine_name?`, `graph_name?`, `position_x=200`, `position_y=200` | Spawn SM node |
| `[w] build_state_machine` | `asset_path*`, `states*`, `state_machine_name?`, `graph_name?`, `transitions?`, `entry_state?` | Declarative SM build |
| `[w] add_state_to_machine` | `asset_path*`, `machine_name*`, `state_name*`, `position_x=200`, `position_y=0` | Add state |
| `[w] add_transition` | `asset_path*`, `machine_name*`, `from_state*`, `to_state*` | Add transition |
| `[w] set_transition_rule` | `asset_path*`, `machine_name*`, `from_state*`, `to_state*`, `variable_name?`, `rule?` | Wire bool/auto/compare condition |
| `[w] set_state_animation` | `asset_path*`, `machine_name*`, `state_name*`, `anim_asset_path*`, `loop=false`, `clear_existing=true` | Set a state's animation (shortcut) |
| `[w] add_anim_graph_node` | `asset_path*`, `node_type?`, `node_class?`, `graph_name=AnimGraph`, `state_name?`, `position_x=200`, `position_y=0`, `anim_asset?`, `ik_bone?`, `effector_space?`, `joint_target_space?`, `bone_to_modify?`, `expose_pins?` | Place an anim graph node |
| `[w] connect_anim_graph_pins` | `asset_path*`, `source_node*`, `source_pin*`, `target_node*`, `target_pin*`, `graph_name?`, `state_name?`, `compile=true` | Wire two node pins |
| `[w] add_variable_get` | `asset_path*`, `variable_name*`, `graph_name=AnimGraph`, `state_name?`, `position_x=0`, `position_y=0` | Place a variable Get node |
| `[w] set_anim_graph_node_property` | `asset_path*`, `node_id*`, `property_path*`, `value*`, `graph_name?`, `state_name?` | Mutate a node's FAnimNode property |
| `[w] auto_layout` | `asset_path*`, `graph_name=AnimGraph`, `formatter=auto` | Auto-layout graph (Blueprint Assist) |
| **Notifies** | | |
| `get_sequence_notifies` | `asset_path*` | List all notifies, including `montage_tick_type` readback |
| `[w] add_notify` | `asset_path*`, `notify_class*`, `time*`, `track_name=1` | Add point notify |
| `[w] add_named_notify` | `asset_path*`, `notify_name*`, `time*`, exactly one of `track_name?` / `track_index?`, `montage_tick_type=Queued` (`Queued/BranchingPoint`) | Add a classless named montage point notify |
| `[w] add_notify_state` | `asset_path*`, `notify_class*`, `time*`, `duration*`, `track_name=1` | Add state notify (duration) |
| `[w] remove_notify` | `asset_path*`, `notify_index*` | Remove notify by index |
| `[w] set_notify_time` | `asset_path*`, `notify_index*`, `new_time*` | Move notify with range/collision validation and cache refresh |
| `[w] set_notify_tick_type` | `asset_path*`, `notify_index*`, `montage_tick_type*` (`Queued/BranchingPoint`) | Change a classless named montage notify's tick type |
| `[w] set_notify_duration` | `asset_path*`, `notify_index*`, `new_duration*` | Set notify state duration |
| `[w] set_notify_track` | `asset_path*`, `notify_index*`, `track_index*` | Move notify to a track |
| `[w] set_notify_properties` | `asset_path*`, `notify_index*`, `properties*` | Reflection-set notify UPROPERTYs |
| `[w] clone_notify_setup` | `source_path*`, `target_path*`, `time_scale?`, `auto_scale?`, `replace_existing?` | Copy notifies asset->asset |
| `[w] bulk_add_notify` | `asset_paths*`, `notify_class*`, `time*`, `time_mode?` (absolute/normalized), `duration?`, `track_name?` | Same notify across many assets |
| **Curves** | | |
| `list_curves` | `asset_path*`, `include_keys=false` | List float/transform curves |
| `get_curve_keys` | `asset_path*`, `curve_name*` | Read float curve keys |
| `[w] add_curve` | `asset_path*`, `curve_name*`, `curve_type=Float` (Float/Transform) | Add a curve |
| `[w] remove_curve` | `asset_path*`, `curve_name*`, `curve_type=Float` | Remove a curve |
| `[w] set_curve_keys` | `asset_path*`, `curve_name*`, `keys_json*` | Replace float curve keys |
| **Sync Markers** | | |
| `get_sync_markers` | `asset_path*` | Read authored sync markers |
| `[w] add_sync_marker` | `asset_path*`, `marker_name*`, `time*`, `track_index=0` | Add sync marker |
| `[w] remove_sync_marker` | `asset_path*`, `marker_name?`, `marker_index?` | Remove by name or index |
| `[w] rename_sync_marker` | `asset_path*`, `old_name*`, `new_name*` | Rename all markers of a name |
| **Bone Tracks** | | |
| `list_bone_tracks` | `asset_path*` | List animated bone names |
| `get_bone_track_keys` | `asset_path*`, `bone_name*`, `start_frame=0`, `end_frame=-1` | Read pos/rot/scale keys |
| `[w] set_bone_track_keys` | `asset_path*`, `bone_name*`, `positions_json*`, `rotations_json*`, `scales_json*` | Set keyframes |
| `[w] add_bone_track` | `asset_path*`, `bone_name*` | Add track |
| `[w] remove_bone_track` | `asset_path*`, `bone_name*`, `include_children=false` | Remove track |
| **Skeleton** | | |
| `get_skeleton_info` | `asset_path*` | Bone hierarchy, virtual bones |
| `get_skeletal_mesh_info` | `asset_path*` | Mesh details, LODs, materials, morphs, sockets |
| `get_skeleton_sockets` | `asset_path*` | Sockets from skeleton or mesh |
| `get_bone_ref_pose` | `asset_path*`, `bone_names=[]` | Bind-pose transforms (local + component) |
| `[w] add_virtual_bone` | `asset_path*`, `source_bone*`, `target_bone*` | Create virtual bone |
| `[w] remove_virtual_bones` | `asset_path*`, `bone_names*` | Remove virtual bones |
| `[w] add_socket` | `asset_path*`, `bone_name*`, `socket_name*`, `location?`, `rotation?`, `scale?` | Add socket |
| `[w] remove_socket` | `asset_path*`, `socket_name*` | Remove socket |
| `[w] set_socket_transform` | `asset_path*`, `socket_name*`, `location?`, `rotation?`, `scale?` | Set socket transform |
| `get_compatible_skeletons` | `asset_path*` | List declared-compatible skeletons |
| `[w] add_compatible_skeleton` | `asset_path*`, `compatible_with*`, `save=true` | Declare skeleton compatible |
| `compare_skeletons` | `skeleton_a*`, `skeleton_b*` | Bone compatibility report |
| **IKRig / Retargeter** | | |
| `get_ikrig_info` | `asset_path*` | Solvers, goals, chains, skeleton |
| `[w] create_ik_rig` | `asset_path*`, `skeletal_mesh_path*`, `pelvis_bone?` | New IK Rig asset |
| `[w] add_ik_solver` | `asset_path*`, `solver_type*`, `root_bone?`, `goals?` | Add solver + goals |
| `[w] add_retarget_chain` | `asset_path*`, `chain_name*`, `start_bone*`, `end_bone*`, `goal_name?` | Add retarget chain |
| `get_retargeter_info` | `asset_path*` | Source/target rigs, chain mappings |
| `[w] create_ik_retargeter` | `asset_path*`, `source_ik_rig_path?`, `target_ik_rig_path?`, `auto_map=fuzzy` (fuzzy/exact) | New IK Retargeter |
| `[w] set_retargeter_rigs` | `asset_path*`, `source_ik_rig_path*`, `target_ik_rig_path*`, `source_preview_mesh?`, `target_preview_mesh?`, `auto_map=fuzzy` | Set rigs on retargeter |
| `[w] set_retarget_chain_mapping` | `asset_path*`, `auto_map?` (exact/fuzzy/clear), `source_chain?`, `target_chain?` | Map chains (auto OR manual pair) |
| `[w] batch_retarget_animations` | `retargeter_path*`, `source_anims*`, `output_folder*`, `source_mesh?`, `target_mesh?`, `name_prefix?`, `name_suffix?`, `search?`, `replace?`, `include_referenced=false`, `overwrite=false`, `auto_map=fuzzy` | Cross-skeleton batch retarget |
| **Control Rig** | | |
| `get_control_rig_info` | `asset_path*`, `element_type?` (bone/control/null/curve/all) | Hierarchy -- bones, controls, nulls |
| `get_control_rig_variables` | `asset_path*` | Animatable controls and BP variables |
| `get_control_rig_graph` | `asset_path*`, `graph_name?` | RigVM nodes, pins, connections |
| `[w] add_control_rig_element` | `asset_path*`, `element_type*` (bone/control/null), `name*`, `parent?`, `parent_type?`, `control_type?` (Float/Integer/Transform/Rotator/Position/Scale/Vector2D), `animatable=true`, `transform?` | Add bone/control/null |
| `[w] add_control_rig_node` | `asset_path*`, `struct_path*`, `position_x?`, `position_y?`, `node_name?`, `method_name?`, `pin_defaults?` | Add a rig unit node |
| `[w] connect_control_rig_pins` | `asset_path*`, `source_pin*`, `target_pin*` | Connect pins (Node.Pin dot-notation) |
| **Physics Asset** | | |
| `get_physics_asset_info` | `asset_path*` | Bodies, constraints, profiles, solver |
| `[w] set_body_properties` | `asset_path*`, `bone_name*`, `mass?`, `physics_type?` (Default/Kinematic/Simulated), `collision_enabled?`, `collision_profile?`, `linear_damping?`, `angular_damping?`, `enable_gravity?` | Edit a physics body |
| `[w] set_constraint_properties` | `asset_path*`, `constraint_index?`, `bone_1?`, `bone_2?`, `swing1_motion?`, `swing1_limit?`, `swing2_motion?`, `swing2_limit?`, `twist_motion?`, `twist_limit?`, `disable_collision?` | Edit a constraint |
| **Motion Matching / Pose Search** | | |
| `get_pose_search_database` | `asset_path*` | Schema ref + anim list |
| `validate_pose_search_database` | `database_path*` | Schema/skeleton/index validation |
| `[w] create_pose_search_schema` | `asset_path*`, `skeleton_path*`, `sample_rate?`, `add_default_channels?` | New PoseSearch schema |
| `[w] create_pose_search_database` | `asset_path*`, `schema_path*` | New PoseSearch database |
| `[w] add_database_entry` | `database_path*`, `anim_path*`, `enabled?`, `mirror_option?` (UnmirroredOnly/MirroredOnly/UnmirroredAndMirrored) | Add anim to database |
| `[w] build_motion_matching_node` | `abp_path*`, `database_path*`, `chooser_path?` | Composite MM node + history, wired |
| `[w] configure_motion_matching_node` | `abp_path*`, `node_id*`, `blend_time?`, `pose_jump_threshold_min?`, `pose_jump_threshold_max?`, `search_throttle?`, `use_inertial_blend?`, `should_filter_notifies?`, `notify_recency_timeout?`, `max_active_blends?` | Tune MM node props |
| **Batch / PIE** | | |
| `[w] batch_execute` | `operations*`, `stop_on_error?` | Run many actions in one transaction |
| `[w] sample_pie_anim_instance` | `actor*`, `component_name?`, `variables?`, `bones?`, `sockets?`, `state_machines?` | Sample a live PIE actor's anim state |
