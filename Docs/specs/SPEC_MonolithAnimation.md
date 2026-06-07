# Monolith — MonolithAnimation Module

**Parent:** [SPEC_CORE.md](../SPEC_CORE.md)
**Engine:** Unreal Engine 5.7+
**Version:** 0.18.1 (Beta)

---

## MonolithAnimation

**Dependencies:** Core, CoreUObject, Engine, MonolithCore, UnrealEd, AnimGraph, AnimGraphRuntime, BlueprintGraph, AnimationBlueprintLibrary, PoseSearch, BlendStackEditor, AnimationModifiers, EditorScriptingUtilities, Json, JsonUtilities

> **`BlendStackEditor` dep (2026-06-07)** added for the Motion Matching action pack — `build_motion_matching_node` spawns the bound-graph `UAnimGraphNode_MotionMatching` / BlendStack nodes.

### Classes

| Class | Responsibility |
|-------|---------------|
| `FMonolithAnimationModule` | Registers ~150 animation actions across `MonolithAnimationActions.cpp` (incl. the state-machine authoring actions `create_state_machine` / `build_state_machine`, plus 2026-06-07 readback actions `get_anim_graph_choosers` / `get_transition_rule`), `MonolithPoseSearchActions.cpp` (PoseSearch + the 2026-06-07 Motion Matching action pack), `MonolithAbpWriteActions.cpp` (5), `MonolithControlRigWriteActions.cpp` (3), `MonolithAnimLayoutActions.cpp` (1), `MonolithAnimationRuntimeActions.cpp` (1 — `sample_pie_anim_instance`), and the 5 graph-surgery actions (`rebuild_evaluate_chooser_node`, `replace_evaluate_chooser_nodes`, `duplicate_reparent_and_sanitize`, `find_node_slice`, `remove_node_slice`). The `WITH_CHOOSER`-gated `chooser` namespace (6 actions) is registered from this module but counted under its own namespace |
| `FMonolithAnimationActions` | Static handlers organized in 15 groups (the original action handlers) |
| `FMonolithAbpWriteActions` | ABP graph write actions (Phase v0.14.3 PR #34): `add_anim_graph_node` (built-in aliases plus generic `UAnimGraphNode_Base` class path/name resolution, with TwoBoneIK / ModifyBone helpers and auto-pin exposure), `connect_anim_graph_pins`, `set_state_animation`, `add_variable_get`, `set_anim_graph_node_property` |
| `FMonolithControlRigWriteActions` | ControlRig write actions: 3 actions (graph node creation, pin configuration, variable management) |
| `FMonolithAnimLayoutActions` | `auto_layout` for AnimBP graphs |

### Actions (~150 — namespace: "animation")

> **Counts are approximate.** Exact integers are no longer tracked to the unit — query `monolith_discover("animation")` for the live figure.

**Note (2026-04-26 audit):** The detailed per-category tables below cover the 103 baseline actions. The remaining **27 actions** (5 ABP write + 13 PoseSearch + 3 ControlRig + 1 layout + 5 graph-surgery) are documented in their own sections at the bottom of this spec. The ABP write actions landed in v0.14.3 (PR #34 by @MaxenceEpitech). No Phase J changes touched this module. v0.14.9 added `copy_bone_pose_between_sequences` (PR #51 by @MaxenceEpitech). v0.14.10 added `list_bone_tracks` (PR #54 by @MaxenceEpitech) and rewrote `get_bone_track_keys` to use the non-deprecated `IsValidBoneTrackName` + `GetBoneTrackTransforms` API path. v0.14.10 also added `get_skeleton_preview_attached_assets` + `get_bone_ref_pose` (PR #55 by @MaxenceEpitech) and the three `CompatibleSkeletons` actions (`get_compatible_skeletons` / `add_compatible_skeleton` / `remove_compatible_skeleton` — PR #56 by @MaxenceEpitech), bringing the module total to 125. The test/profiling harness Wave 2 added the 5 graph-surgery actions, bringing the module total to ~130. Wave 16 added 3 actions — `create_state_machine` + `build_state_machine` (state-machine authoring, in `MonolithAnimationActions.cpp`) and `sample_pie_anim_instance` (PIE runtime telemetry, in the new `MonolithAnimationRuntimeActions.cpp`). The 2026-06-07 gap-closure pass added 2 readback actions — `get_anim_graph_choosers` + `get_transition_rule` — plus a structured `set_transition_rule.rule` object and `get_nodes.include_anim_graph`, bringing the module total to ~135.

**Sequence Info (5) — read-only**
| Action | Description |
|--------|-------------|
| `get_sequence_info` | Get sequence metadata (duration, frames, root motion, compression, etc.) |
| `get_sequence_notifies` | Get all notifies on an animation asset (sequence, montage, composite) |
| `get_bone_track_keys` | Get position/rotation/scale keys for a bone track (with optional frame range) (rewritten v0.14.10 to use non-deprecated `IsValidBoneTrackName` + `GetBoneTrackTransforms`; emits scales unconditionally — see CHANGELOG behaviour note) |
| `get_sequence_curves` | Get float and transform curves on an animation sequence |
| `list_bone_tracks` | List all bone tracks present on an animation sequence (returns `count` + `bone_names: [..]`). Discovery action for `get_bone_track_keys`. (PR #54, v0.14.10) |

**Bone Track Editing (4)**
| Action | Description |
|--------|-------------|
| `set_bone_track_keys` | Set position/rotation/scale keys (JSON arrays) |
| `add_bone_track` | Add a bone track to an animation sequence |
| `remove_bone_track` | Remove a bone track (with optional `include_children`) |
| `copy_bone_pose_between_sequences` | Read evaluated pose from source `UAnimSequence` at a time and write as keys to a destination sequence for a list of bones. Per-bone skip with structured `reason`. (PR #51 v0.14.9 by @MaxenceEpitech) |

**Notify Operations (6)**
| Action | Description |
|--------|-------------|
| `add_notify` | Add a point notify to an animation asset |
| `add_notify_state` | Add a state notify (with duration) to an animation asset |
| `remove_notify` | Remove a notify by index |
| `set_notify_time` | Set trigger time of an animation notify |
| `set_notify_duration` | Set duration of a state animation notify |
| `set_notify_track` | Move a notify to a different track |

**Curve Operations (5)**
| Action | Description |
|--------|-------------|
| `list_curves` | List all animation curves on a sequence (optional `include_keys`) |
| `add_curve` | Add a float or transform curve to an animation sequence |
| `remove_curve` | Remove a curve from an animation sequence |
| `set_curve_keys` | Set keys on a float curve (replaces existing keys) |
| `get_curve_keys` | Get all keys from a float curve |

**BlendSpace Operations (5)**
| Action | Description |
|--------|-------------|
| `get_blend_space_info` | Get blend space samples and axis settings |
| `add_blendspace_sample` | Add a sample to a blend space |
| `edit_blendspace_sample` | Edit sample position and optionally its animation |
| `delete_blendspace_sample` | Delete a sample by index |
| `set_blend_space_axis` | Configure axis (name, range, grid divisions, snap, wrap) |

**ABP Graph Reading (8) — read-only**
| Action | Description |
|--------|-------------|
| `get_abp_info` | Get ABP overview (skeleton, graphs, state machines, variables, interfaces) |
| `get_state_machines` | Get all state machines with full topology |
| `get_state_info` | Detailed info about a state in a state machine |
| `get_transitions` | All transitions (supports empty machine_name for ALL state machines) |
| `get_blend_nodes` | Blend nodes in an ABP graph |
| `get_linked_layers` | Linked animation layers |
| `get_graphs` | All graphs in an ABP |
| `get_nodes` | Animation nodes with optional class and graph_name filters. **`include_anim_graph` (bool, 2026-06-07):** when set, also traverses the main AnimGraph (default behavior covers function graphs only) and emits `LinkedTo` endpoints (default output reports connection counts only). Opt-in to preserve the existing output shape by default. |
| `get_anim_graph_choosers` | **(2026-06-07)** Walk an AnimBP's graphs (main AnimGraph + function graphs) for chooser-evaluating nodes (Evaluate-Chooser K2 nodes, resolved reflectively by class-name prefix). Reports `{ node_guid, node_title, chooser_asset, output_pin_links: [...] }` per node. Optional `recursive` expands each referenced chooser tree via the shared chooser-tree collector (the same walk `chooser::inspect_chooser recursive:true` uses). `WITH_CHOOSER` + editor-only. |

**Montage Operations (8)**
| Action | Description |
|--------|-------------|
| `get_montage_info` | Get montage sections, slots, blend settings |
| `add_montage_section` | Add a section to an animation montage |
| `delete_montage_section` | Delete a section by index |
| `set_section_next` | Set the next section for a montage section |
| `set_section_time` | Set start time of a montage section |
| `set_montage_blend` | Set blend in/out times and auto blend out |
| `add_montage_slot` | Add a slot track to a montage |
| `set_montage_slot` | Rename a slot track by index |

**Skeleton Operations (11)**
| Action | Description |
|--------|-------------|
| `get_skeleton_info` | Skeleton bone hierarchy, virtual bones, and sockets |
| `get_skeletal_mesh_info` | Mesh info: morph targets, sockets, LODs, materials |
| `get_skeleton_sockets` | Get sockets from a skeleton or skeletal mesh |
| `get_skeleton_curves` | Get all registered animation curve names from a skeleton |
| `get_skeleton_preview_attached_assets` | Read `USkeleton::PreviewAttachedAssetContainer` (the `[Preview Only]` list shown in Persona's bone tree). Returns `{ asset_path, attached_objects: [{ attach_point, attached_object, attached_object_class }, ...], count, transforms_stored: false }`. The `transforms_stored: false` flag documents that the container does NOT carry per-asset relative transforms (Persona attaches at the socket origin with the asset's natural pivot). (PR #55, v0.14.10) |
| `get_bone_ref_pose` | Reference (bind) pose transforms for skeleton bones in BOTH parent-relative AND component-space. Walks `FReferenceSkeleton` once for component-space accumulation. Accepts a `bone_names: array` filter (default: all bones). Works on either a `USkeleton` or `USkeletalMesh` asset path — `source_type` field in response indicates which. (PR #55, v0.14.10) |
| `add_virtual_bone` | Add a virtual bone to a skeleton |
| `remove_virtual_bones` | Remove virtual bones (specific names) |
| `add_socket` | Add a socket to a skeleton |
| `remove_socket` | Remove a socket from a skeleton |
| `set_socket_transform` | Set the transform of a skeleton socket |

**Skeleton Compatibility (3)**
Wraps `USkeleton::CompatibleSkeletons` — the canonical UE5 mechanism that lets anims authored on one skeleton play on another (typical case: UE4 mannequin animation packs on UE5 `SK_Mannequin` meshes). Closes the prior `editor_query.run_python` workaround for cross-skeleton retarget setup. All three actions accept `save: bool = true` controlling whether `UEditorAssetLibrary::SaveAsset` runs after the mutation. (PR #56 by @MaxenceEpitech, v0.14.10.)

| Action | Description |
|--------|-------------|
| `get_compatible_skeletons` | Returns `{ asset_path, compatible_skeletons: [..], count }`. Lists the soft-pointer paths currently registered in `USkeleton::CompatibleSkeletons`. |
| `add_compatible_skeleton` | Marks a target skeleton compatible with the source. Idempotent: returns disjoint `added` / `already_compatible` booleans + resulting `count`. Self-compat rejected with `"Cannot mark a skeleton compatible with itself"`. |
| `remove_compatible_skeleton` | Removes a previously-registered compatible skeleton. Idempotent: returns disjoint `removed` / `was_compatible` booleans + resulting `count`. |

**Root Motion (1)**
| Action | Description |
|--------|-------------|
| `set_root_motion_settings` | Configure root motion settings (enable, lock mode, force root lock) |

**Asset Creation (3)**
| Action | Description |
|--------|-------------|
| `create_sequence` | Create a new empty animation sequence |
| `duplicate_sequence` | Duplicate an animation sequence to a new path |
| `create_montage` | Create a new animation montage with skeleton |

**Anim Modifiers (2)**
| Action | Description |
|--------|-------------|
| `apply_anim_modifier` | Apply an animation modifier class to a sequence |
| `list_anim_modifiers` | List animation modifiers applied to a sequence |

**Composites (3)**
| Action | Description |
|--------|-------------|
| `get_composite_info` | Get segments and metadata from an animation composite |
| `add_composite_segment` | Add a segment to an animation composite |
| `remove_composite_segment` | Remove a segment from an animation composite by index |

**PoseSearch (~12 core; +14 Motion Matching pack below)**
| Action | Description |
|--------|-------------|
| `get_pose_search_schema` | Get PoseSearch schema config and channels |
| `get_pose_search_database` | Get PoseSearch database sequences and schema reference |
| `add_database_sequence` | Add an animation sequence to a PoseSearch database |
| `remove_database_sequence` | Remove a sequence from a PoseSearch database by index |
| `get_database_stats` | Get PoseSearch database statistics (pose count, search mode, costs) |
| `create_pose_search_schema` | Create a new PoseSearch schema asset |
| `create_pose_search_database` | Create a new PoseSearch database asset |
| `set_database_sequence_properties` | Set per-sequence properties (looping, mirror option, sample range) |
| `add_schema_channel` | Add a channel to a PoseSearch schema |
| `remove_schema_channel` | Remove a channel from a PoseSearch schema |
| `set_channel_weight` | Set the weight on a PoseSearch schema channel |
| `rebuild_pose_search_index` | Rebuild a PoseSearch database's search index |
| `set_database_search_mode` | Set a PoseSearch database's search mode |

**Note:** `get_database_stats` is hardened against unbuilt databases (it previously asserted on a PoseSearch database with no built search index — see Fixes below). `get_database_stats` and `get_pose_search_schema` also gained read-back fields surfacing additional database/schema state (enhancement, no count delta).

**Motion Matching action pack (14 — 2026-06-07)** — namespace `animation`. End-to-end authoring surface for UE 5.7 Motion Matching: PoseSearch normalization sets, asset-type-agnostic database entries, schema mirroring/channels, notifies, validation, and the Pose-History / Motion-Matching anim-graph nodes.

| Action | Description |
|--------|-------------|
| `create_normalization_set` | Create a `UPoseSearchNormalizationSet` asset (shared cost normalization across databases). |
| `add_database_to_normalization_set` | Add a PoseSearch database to a normalization set. |
| `set_database_normalization_set` | Assign a normalization set to a PoseSearch database. |
| `add_database_entry` | Add a database entry, asset-type-agnostic (`UAnimSequence` / `UBlendSpace` / `UAnimComposite` / `UAnimMontage`) via the unified 5.7 `FPoseSearchDatabaseAnimationAsset` discriminated shape. |
| `set_database_entry_tags` | Set the tags on a database entry. |
| `create_mirror_data_table` | Create a `UMirrorDataTable` asset for mirrored-pose matching. |
| `set_schema_mirror_data_table` | Assign a mirror data table to a PoseSearch schema. |
| `configure_schema_channel` | Configure an existing schema channel's properties via reflection. |
| `add_pose_search_notify` | Add a PoseSearch notify-state to a sequence — supports 8 notify-state kinds. |
| `derive_schema_channels_from_skeleton` | Derive schema channels (bone/trajectory sampling) from the target skeleton automatically. |
| `validate_pose_search_database` | Validate a PoseSearch database (schema/entry/normalization consistency + build state). |
| `configure_pose_history_node` | Configure a Pose-History anim-graph node (`UAnimGraphNode_PoseHistory`) in an ABP. |
| `configure_motion_matching_node` | Configure a Motion-Matching anim-graph node (database, schema, settings) in an ABP. |
| `build_motion_matching_node` | Composite: spawn + wire + configure a Motion-Matching node (with its Pose-History) in one call. As of 2026-06-07 also wires the Pose-History pose-out to the AnimGraph Output Pose (`UAnimGraphNode_Root` 'Result' input) and reports `output_pose_wired`. |
| `get_anim_graph_output_connection` | READ-ONLY: report whether the AnimGraph's Output Pose (`UAnimGraphNode_Root` 'Result' input) is driven, and by which `source_node`/`source_pin`. Optional `graph_name` (default the main AnimGraph). |

**Retarget create/run pack (4 — 2026-06-07)** — namespace `animation`. Create the IK Rig / IK Retargeter assets and run a batch retarget, so a source skeleton's animation library can be re-authored onto a target skeleton.

| Action | Description |
|--------|-------------|
| `create_ik_rig` | Create a `UIKRigDefinition` for a target skeletal mesh / skeleton. |
| `create_ik_retargeter` | Create a `UIKRetargeter` referencing a source and target IK Rig. |
| `set_retargeter_rigs` | Assign the source and target IK Rig on an existing `UIKRetargeter`. Auto-seeds the default retarget op stack (Pelvis / FKChains / RunIK / IKChains / RootMotion / CurveRemap) plus `AutoMapChains` so a freshly-created retargeter actually produces motion rather than passing through a frozen pose. Optional `auto_map` (default true) controls the chain auto-mapping pass. |
| `batch_retarget_animations` | Run a batch retarget of source animations onto the target skeleton through a configured `UIKRetargeter`, writing the retargeted sequences. When the retargeter was created without an op stack it is seeded as in `set_retargeter_rigs` before the run. |

> **Op-stack seeding (2026-06-07).** A bare UE 5.7 `UIKRetargeter` carries no retarget operations, so an un-seeded retarget passes the source pose straight through and produces frozen output clips. `set_retargeter_rigs` and `batch_retarget_animations` therefore seed the canonical op stack (Pelvis / FKChains / RunIK / IKChains / RootMotion / CurveRemap) + `AutoMapChains` on creation. See Fixes below.

**ABP Write (5) — v0.14.3 PR #34 by @MaxenceEpitech**
| Action | Description |
|--------|-------------|
| `add_anim_graph_node` | Place an animation graph node. `node_type` still accepts the existing aliases (`SequencePlayer`, `BlendSpacePlayer`, `TwoWayBlend`, `BlendListByBool`, `LayeredBoneBlend`, `MotionMatching`, `TwoBoneIK`, `ModifyBone`, `LocalToComponentSpace`, `ComponentToLocalSpace`, plus 2026-06-07 `pose_history` + `inertialization` aliases) and may also be a class path/name for legacy clients. New `node_class` accepts any loaded non-abstract `UAnimGraphNode_Base` subclass by class path or name. Rejects missing, ambiguous, non-`UAnimGraphNode_Base`, abstract, and unresolved classes with actionable errors. TwoBoneIK auto-exposes `EffectorLocation`, `JointTargetLocation`, `Alpha` as input pins; `expose_pins` manually controls optional pins on any node type |
| `connect_anim_graph_pins` | Wire two pins inside an ABP graph |
| `set_state_animation` | Assign an animation asset to a state machine state |
| `add_variable_get` | Place a `K2Node_VariableGet` in an ABP anim graph for reading AnimInstance member variables. Validates the variable exists on the skeleton class before spawning |
| `set_anim_graph_node_property` | Set a property on a previously-placed anim graph node via reflection |

**Fixes (2026-06-07)**
- `add_anim_graph_node` — fixed a pre-existing crash when spawning bound-graph nodes (BlendStack / MotionMatching); the spawn path now uses `FGraphNodeCreator` so the node's bound graph is constructed correctly.
- `get_database_stats` — fixed a pre-existing crash that asserted on a PoseSearch database with no built search index (unbuilt database). Now returns stats / a clear state instead of asserting.
- `build_motion_matching_node` — now wires the Pose-History chain through to the AnimGraph Output Pose (`UAnimGraphNode_Root` 'Result' input); previously the composite spawned and configured the node but left it disconnected from the output, so it never drove the final pose.
- `batch_retarget_animations` — no longer produces frozen (pass-through) output clips. The default retarget op stack is now seeded on the retargeter (see Retarget pack above), so retargeted sequences carry actual remapped motion.

**ControlRig Write (3)**
| Action | Description |
|--------|-------------|
| (3 actions in `MonolithControlRigWriteActions.cpp` — graph node creation, pin configuration, variable management) |

**Layout (1)**
| Action | Description |
|--------|-------------|
| `auto_layout` | Animation Blueprint graph auto-layout has no built-in formatter. `formatter="auto"` falls back to Blueprint Assist when the bridge is available, `"blueprint_assist"` forces that fallback, and `"monolith"` is unsupported. Optional `graph_name` targets a specific graph. |

**Graph Surgery (5) — Test/Profiling Harness Wave 2**

Node-level write operations over Animation Blueprint graphs, built for AnimBP reparenting and Chooser-node rewiring. The two batch / reparent / slice-removal actions default to `dry_run=true`.

| Action | Description |
|--------|-------------|
| `rebuild_evaluate_chooser_node` | Delete and reflectively respawn a `UK2Node_EvaluateChooser2` (class resolved as `/Script/ChooserUncooked.K2Node_EvaluateChooser2`), regenerating pins from a target `UChooserTable`. Reconnects compatible pins through the graph schema (`CanCreateConnection` / `TryCreateConnection`), coercing reroute / `Knot` wildcard pins. Compile-checks the result and never auto-saves on a failed compile. |
| `replace_evaluate_chooser_nodes` | Batch `rebuild_evaluate_chooser_node` across every Evaluate-Chooser node in an ABP. `dry_run` defaults to `true`. |
| `duplicate_reparent_and_sanitize` | Duplicate an ABP and reparent it to `new_parent_class`, then classify every node against the new parent's reflected surface into `safe` / `requires_guard` / `requires_rebuild` / `remove_for_smoke` (node kinds: cast, variable_get, function-call, Evaluate-Chooser). A self-context `variable_get` is classified `safe` when its variable exists on the new parent OR is defined locally on the duplicate (locals survive reparenting); `requires_guard` only when neither holds. `dry_run` defaults to `true`. |
| `find_node_slice` | Compute a directional (`upstream` / `downstream`) node slice from a seed node, honoring `stop_rules`. Reports the slice set, before/after node counts, and orphaned pins. Read-only. |
| `remove_node_slice` | Remove a directional node slice. Reports counts, orphaned pins, and any broken required-exec continuity — surfaced in the response, never auto-rewired. `dry_run` defaults to `true`. |

**State Machine Authoring (2) — Test/Profiling Harness Wave 16**

| Action | Description |
|--------|-------------|
| `create_state_machine` | Spawn a `UAnimGraphNode_StateMachine` into an ABP's anim graph via `FEdGraphSchemaAction_NewStateNode::SpawnNodeFromTemplate` (auto-creates the SM graph + entry node through `PostPlacedNewNode`). Renames the auto-created `EditorStateMachineGraph` (the SM node title derives from it). Picks the first graph with a `UAnimationGraphSchema` by default; optional `graph_name` targets a specific anim graph for layered ABPs. Compiles + marks dirty. Returns the SM node title + SM graph name + a state/transition readback. |
| `build_state_machine` | Declarative builder composing `create_state_machine` + state/transition/rule mutators in one transaction from `{states:[{name, animation?}], transitions:[{from,to,rule?}], entry_state}`. States optionally wire a `UAnimGraphNode_SequencePlayer` to the state result pose pin. Rules support bool variables and `auto`/`automatic` (`bAutomaticRuleBasedOnSequencePlayerInState`); any other rule expression is reported as `unsupported rule expression (deferred)` per-element without failing the build. Returns a per-element states/transitions report. Compiles + saves once at the end. |

**Transition Rules (2026-06-07)**

`set_transition_rule` accepts a structured **`rule`** object so callers don't author rule-graph nodes by hand:

| `rule.kind` | Shape | Notes |
|---|---|---|
| `bool` | `{ kind: "bool", variable: <name> }` | Existing behavior. The variable may be an inherited `BlueprintReadOnly`/`BlueprintVisible` bool on the parent AnimInstance class — validation now walks the skeleton/generated/parent class chain instead of `NewVariables` only, so inherited native bools are accepted. |
| `auto` | `{ kind: "auto" }` | Existing — sequence-completion automatic rule. |
| `compare` | `{ kind: "compare", lhs: <variable\|expr>, op: ">"\|"<"\|">="\|"<="\|"=="\|"!=", rhs: <number> }` | Float/numeric comparison against an AnimInstance property (inherited float props validate via the same class-chain walk). |

Full free-form **expression-graph** authoring (e.g. `Abs(X) > 45.0`) is **deferred** — it is the multi-node graph-authoring + compile surface, not yet shipped.

| Action | Description |
|--------|-------------|
| `get_transition_rule` | **(2026-06-07)** Read back an authored transition rule: its kind + operands + compile status. Read-only. |

**Runtime Telemetry (1) — Test/Profiling Harness Wave 16, `MonolithAnimationRuntimeActions.cpp`**

| Action | Description |
|--------|-------------|
| `sample_pie_anim_instance` | Sample a live PIE actor's animation state. Resolves actor → `USkeletalMeshComponent` → `GetAnimInstance()`. Reports anim-instance class path, mesh `AnimClass` path, skeletal mesh path, animation mode, active state-machine state(s) (`GetStateMachineIndex` + `GetCurrentStateName` + `GetInstanceMachineWeight`; enumerates all baked machines via `IAnimClassInterface::GetBakedStateMachines` when `state_machines` omitted), active montage (`GetCurrentActiveMontage` + current section), requested anim-instance variables (live reflection), and requested bone/socket world transforms. Bone names are resolved to indices via `GetBoneIndex(FName)` before `GetBoneTransform(index)`; sockets via `GetSocketTransform(FName, RTS_World)`. Per-asset-player weights have no direct public getter (deferred); state/machine weights ARE reported. |

### Bulk Fill & Describe Surface (2026-05-11)

`MonolithAnimationBulkFillAdapter` registers under `FMonolithBulkFillRegistry` for the `animation` namespace, exposed via the framework-level `bulk_fill_query("apply", ...)` and `describe_query("schema", ...)` dispatchers. Phase 5 of the MCP ergonomics rollout (design spec `Docs/plans/2026-05-11-monolith-mcp-ergonomics-design.md`).

**Surface summary.** `bulk_fill_query("apply", target_namespace="animation", target="<asset_path>", tree={...})` covers PoseSearch database bulk-populate (the 60-300+ entry pain) and a v1 audit-only notify-apply-template scan. `describe_query("schema", target_namespace="animation", target="<asset_path>")` returns the PoseSearch entry schema and the notify/curve track schema for the target sequence.

**fill_kind catalogue (2 — enumerated against `MonolithAnimationBulkFillAdapter.cpp`):**

| `fill_kind` | Target shape | Walks |
|---|---|---|
| `PoseSearchDatabase` | `UPoseSearchDatabase` | `entries:[]` walked as `FPoseSearchDatabaseAnimationAsset` rows. Discriminated 5.7-unified shape: `sequence` / `blendspace` / `composite` / `montage` per entry, plus `looping` / `mirror_option` / `sample_range` |
| `NotifyApplyTemplate` | Folder + name_glob | **v1 audit-only.** Scans the folder via name glob (e.g. `name_glob: "A_Walk_*"`) and surfaces which sequences a template would apply to. Commit still through existing per-asset notify CRUD actions |

**Sample tree (PoseSearchDatabase, design spec Appendix B.3):**

```json
{
  "target": "/Game/AnimGraph/PSD_Locomotion",
  "tree": {
    "fill_kind": "PoseSearchDatabase",
    "entries": [
      {"animation": "/Game/Anim/A_Idle",     "looping": true,  "mirror_option": "UnmirroredOnly"},
      {"animation": "/Game/Anim/A_Walk_F",   "looping": true,  "sample_range": {"min": 0.0, "max": 1.5}},
      {"animation": "/Game/Anim/A_Run_F",    "looping": true},
      {"blendspace": "/Game/Anim/BS_Strafe", "mirror_option": "UnmirroredAndMirrored"}
    ]
  },
  "dry_run": true
}
```

**Adapter-specific quirks.**

- **`FPoseSearchDatabaseAnimationAsset` is a unified 5.7 shape.** UE 5.7 collapsed prior per-asset-type containers into a single discriminated struct. The adapter routes per-row writes via the discriminator (the first of `sequence` / `blendspace` / `composite` / `montage` present in each row). Schema surfaces the discriminator under `entries[].asset_type` with the four valid values.
- **`IAnimationDataController` requires bracket transactions.** Sequence-level writes (notify CRUD, curve CRUD, bone-track CRUD) must open / close an `IAnimationDataController` transaction. The PoseSearchDatabase fill_kind does NOT touch sequence-level transactions (it writes to the database asset directly); notify/curve fill_kinds would, hence they remain `(WISHLIST v1.1)`.
- **CHT_ chooser-table read/edit surface lives in the `chooser` namespace, not `animation`.** Chooser tables (`UChooserTable`) are inspected and edited via the dedicated `chooser` namespace (`inspect_chooser`, `duplicate_chooser_tree`, `set_context_object_class`, `set_result_asset_reference`, `validate_chooser` — all `#if WITH_CHOOSER` gated; see `SPEC_MonolithAnimation.md` § Chooser Namespace). The bulk_fill `animation` adapter still does NOT carry a chooser-table `fill_kind` — that remains `(WISHLIST)`.
- **`v1 NotifyApplyTemplate fill_kind is audit-only.** Cited from the design spec. The handler scans the folder + glob and returns matched sequences with their existing notify / curve state, plus the template that would be applied. No writes commit. Real per-asset notify writes still flow through the existing `add_notify` / `add_curve` / `set_notify_time` actions.
- **Skeleton compatibility surface in v0.14.10.** Schema surfaces `CompatibleSkeletons` via the existing `get_compatible_skeletons` action — bulk_fill of compatible-skeleton lists is a separate `(WISHLIST v1.1)` fill_kind.

**Limitations / v1.1 follow-ups.**

- Notify/curve glob-template real-write fill_kind — `(v1.1)` — blocked on per-asset `IAnimationDataController` transaction surface enumeration.
- `set_blend_space_axis` / `set_section_next` / `add_montage_section` dry-run integration — `(WISHLIST v1.1)` — dry_run integration on existing actions.
- CHT_ chooser-table population fill_kind — `(WISHLIST)` — entire chooser-table action surface absent from animation_query.
- CSV ingest of PoseSearch entries via folder + naming convention — `(WISHLIST v1.1)` per Q2.

---

## Chooser Namespace (10 — namespace: "chooser")

A dedicated namespace for inspecting and editing `UChooserTable` assets, registered from `MonolithAnimation`. **All actions are `#if WITH_CHOOSER` gated** — they register only when the Chooser plugin (`Engine/Plugins/Chooser`) is present. The namespace registers no actions in builds without it.

| Action | Description |
|--------|-------------|
| `inspect_chooser` | Read-only inspection of a `UChooserTable`: result type and result class, ContextData parameters (class/struct requirements), row count, column count + types, referenced assets, and compile/validation status. **`referenced_assets` walks only direct `FAssetChooser` / `FSoftAssetChooser` result rows** — `NestedChooser` and `FObjectChooser` result rows return empty by default (the reference lives behind an indirection the default walk does not follow). **`recursive` (bool, default false, 2026-06-07):** when set, resolves the FULL nested chooser tree via the shared chooser-tree collector — emits a `child_tables[]` tree with each row's resolved asset path AND row kind (`asset` / `soft_asset` / `evaluate_chooser` / `nested_chooser`), plus `nested_objects`, `parent_table`, `root_chooser`, fallback, and output-object cells. A mandatory visited-set guards against cyclic ParentTable/Nested references. Non-recursive output shape is unchanged. |
| `duplicate_chooser_tree` | Duplicate one or more chooser tables into a destination folder; sources are never mutated. Params: `source_assets[]` (required), `destination_folder` (required), optional `remap_rules` (map of old-asset-path → new-asset-path). When `remap_rules` is supplied the action runs a **two-pass duplicate-then-remap** (all duplicates are created first, then references are rewritten — making the result order-independent of how nested tables reference each other) and rewrites `RootChooser` / `ParentTable` / `NestedChoosers` plus result `FInstancedStruct` asset references in each duplicate. The remap now also recurses through **nested `FEvaluateChooser` + `FNestedChooser` references** — `ResultsStructs` / `FallbackResult` / `FOutputObjectColumn`, recursing into `NestedObjects` — using normalized path matching so trailing-slash / case / `.uasset` variants resolve. Each duplicate reports a per-row `row_remap_report` of what was rewritten, and all duplicates are saved to disk. |
| `set_context_object_class` | Rewrite the `Class` on a ContextData parameter entry (`FContextObjectTypeClass`), e.g. to retarget a chooser at a new ABP class. Marks the package dirty and recompiles (`Compile(true)`). |
| `set_result_asset_reference` | Rewrite the `Asset` reference on a result row (`FAssetChooser` / `FSoftAssetChooser`), e.g. a PoseSearch database. Rejects non-asset result rows (e.g. `NestedChooser` / EvaluateChooser) with a precise error — use `set_evaluate_chooser_result_reference` for those. Marks the package dirty and recompiles (`Compile(true)`). |
| `set_evaluate_chooser_result_reference` | Rewrite the child `UChooserTable` that an EvaluateChooser result row points at (`FEvaluateChooser`). Root / nested chooser rows are EvaluateChooser rows and are NOT settable via `set_result_asset_reference`; this action handles them. Params: `asset_path` (required, the table to edit), `row` (required, 0-based result row index of the EvaluateChooser row), `child_chooser_path` (required, the `UChooserTable` to point it at). Marks the package dirty and recompiles (`Compile(true)`). |
| `validate_chooser` | `Compile(true)` plus validation: optional `expected_context_class` and `expected_result_type` (`ObjectResult` / `ClassResult` / `NoPrimaryResult`), plus a sweep for null / stale result-row asset references. Read-only apart from the compile pass. |

**Chooser authoring (4 — 2026-06-07)** — create a chooser table from scratch and populate it row-by-row (plus single-cell edits), the companion write surface to the inspect/edit/duplicate actions above.

| Action | Description |
|--------|-------------|
| `create_chooser_table` | Create a new `UChooserTable`. Sets `output_type` (`ObjectResult` (default) / `ClassResult` / `NoPrimaryResult`; `Object` aliased to `ObjectResult`), an optional `output_class` (the Result Class — resolved from a class path/name, e.g. an ABP `_C` or `PoseSearchDatabase`), and an optional `context_class` added as a `FContextObjectTypeClass` parameter. Marks the package dirty. |
| `add_chooser_column` | Append a column. `column_kind` is `Bool` / `Enum` / `GameplayTag` / `FloatRange` / `OutputObject`. Input (filter) columns take an optional `binding_property` dotted path setting the `InputValue` binding chain. For an `Enum` column, an optional `enum_class` (resolved from an enum path/name) sets the column's enum type so cell values validate against the right `UEnum`. The new column's per-row value array is grown to the table's current row count so all parallel arrays stay aligned. Marks the package dirty. |
| `add_chooser_row` | Append a row. `cells` is one entry per INPUT column in column order (`Bool`: bool/`any`; `Enum`: int; `FloatRange`: `{min,max}`; `GameplayTag`: tag string); `output_psd` is the asset the row selects (written as an `FAssetChooser` result). Every parallel array (per-column value arrays, `OutputObject` `RowValues`, `ResultsStructs`, `DisabledRows`) grows by exactly 1 atomically. Marks the package dirty. |
| `set_chooser_cell` | Set a single cell value at `(row, column)` in an existing chooser table. Dispatches per column kind to the matching predicate-cell write (`Bool` → bool/`any`; `Enum` → int validated against the column's `enum_class`; `FloatRange` → `{min,max}`; `GameplayTag` → tag string), keeping the typed predicate arrays aligned. Marks the package dirty. |

---
