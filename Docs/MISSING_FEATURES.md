# Monolith — Missing Features / MCP Action Gaps

**Purpose:** Running inbox of empirically-discovered Monolith MCP capability gaps — actions that are missing, broken, or that forced a manual in-editor workaround during real project work. Each entry proposes the action to add. Distinct from `ROADMAP.md` (milestones / ship-status); this is the raw gap log, fed from the field.

**Internal planning doc.** Lives in `Docs/` — if it should NOT ship in the public release zip, add it to the `make_release.ps1` strip list alongside `ROADMAP.md`.

---

## 2026-06-07 — RESOLVED: Motion Matching authoring pack + PIE/profiling harness (0.18.1)

Field gaps surfaced building an end-to-end Motion Matching setup + autonomous AI locomotion workflow, now addressed (see `SPEC_CORE.md` §12 2026-06-07 notes, `specs/SPEC_MonolithAnimation.md`, `specs/SPEC_MonolithBlueprint.md`, `specs/SPEC_MonolithAI.md`, `specs/SPEC_MonolithEditor.md`):

- **Motion Matching authoring.** Pose Search schema/database primitives, mirror data tables, chooser-table authoring (`create_chooser_table` / `add_chooser_column` w/ `enum_class` / `add_chooser_row` / `set_chooser_cell`), AnimBP graph (`build_motion_matching_node` etc.), and `build_foot_ik_pass`.
- **Thread-safe AnimBP authoring.** `add_property_access_node`, `set_function_thread_safe`, `bind_chooser_database_via_threadsafe`; `scaffold_locomotion_anim_values` now emits a fully-wired thread-safe body and can target a named function graph.
- **Character/actor scaffolding.** `scaffold_motion_matching_character` + the `blueprint` MM scaffolders; inherited native-component CDO-override persistence.
- **Retarget create/run.** `create_ik_rig` / `create_ik_retargeter` / `set_retargeter_rigs` / `batch_retarget_animations` (auto-seeds the retarget op stack so clips are no longer frozen).
- **AI autonomy.** `AMonolithBehaviorTreeAIController` (runs a BT on `OnPossess`) + movement-driving BT task classes; `reorder_bt_children` persistence + Blackboard linking fixes.
- **PIE / profiling harness.** Async PIE-smoke sessions, CSV/Insights profiling brackets, clip + anim-frame capture, `actor_setup`, map authoring, nav rebuild/validate.

**OPEN follow-up:**
- **`build_foot_ik_pass` ground-trace effector drive.** Foot IK is wired and pose-driving (non-Ignore `ModifyBone` mode + Two-Bone IK effector space/bone set), but the effector is NOT yet driven from a ground trace — full ground-contact adaptation is a follow-up. Document honestly; do not claim ground-adaptive foot IK until the trace drive lands.
- **State-machine float/expression transition rules.** `build_state_machine` supports bool-var + automatic rules; float and expression-graph rules are still deferred.

---

## 2026-06-06 — RESOLVED: field-surfaced shortcomings pass (Wave 16)

Field gaps surfaced building an AnimBP / state-machine authoring + test/profiling harness workflow, now addressed (5 new actions + behaviors; see `SPEC_CORE.md` §12 2026-06-06 note, `specs/SPEC_MonolithEditor.md`, `specs/SPEC_MonolithAnimation.md`):

- **Chooser nested-remap + EvaluateChooser setter.** `duplicate_chooser_tree` now does a two-pass duplicate-then-remap (order-independent) recursing nested `FEvaluateChooser` + `FNestedChooser` refs (`ResultsStructs`/`FallbackResult`/`FOutputObjectColumn` → `NestedObjects`) with normalized path matching + per-row `row_remap_report`; new `chooser::set_evaluate_chooser_result_reference` rewrites root/nested EvaluateChooser result rows that `set_result_asset_reference` rejects.
- **State-machine authoring + PIE telemetry.** New `animation::create_state_machine` + `build_state_machine` (declarative; bool-var + automatic rules — float/expression rules deferred-per-element) and `animation::sample_pie_anim_instance` (live PIE AnimInstance class/AnimClass/mode/montage/SM-state/bone+socket).
- **PIE-smoke lifecycle / world-leak hardening.** `run_pie_smoke` + `load_level` world-leak handshake; grouped `log_patterns` + teardown bucketing; session `lifecycle` field; `probe_scripts` + `stages` staged hooks.
- **Clip capture validity / staging / identity + anim preview capture.** `capture_pie_movement_clip` gained a capture-validity heuristic + `view_target_actor`/`view_target` + `stages` + `runtime_identity` + `expected_anim_class` assert; new `editor::capture_anim_frames` renders AnimSequence/BlendSpace/AnimBlueprint preview→PNG with no PIE.
- **Build-error scope.** `get_build_errors` gained `since_marker`/`since_iso`/`since_seconds`/`clear_baseline` + `compile_errors` vs `other_errors` buckets + `exclude_categories`.
- **describe/bulk_fill aliases.** `target_namespace`/`target_action` now accept namespace/action aliases (no new action).
- **GAS-free data-asset authoring** confirmed already covered via existing `blueprint::seed_data_asset` (no new action needed).

---

## 2026-05-23 — AnimBP / Blueprint (first-person framework work)

Surfaced finishing a first-person weapon framework AnimBP. Each gap blocked an action or forced a manual Details-panel workaround. Source refs are UE 5.7 engine paths.

1. **Set/read an AnimBP function `Thread Safe` flag.** No MCP path to SET `bThreadSafe` (`BlueprintThreadSafe` meta) on a user function, and `get_functions` doesn't surface it for READ either — so a thread-safe AnimBP getter can't be authored end-to-end via MCP.
   - **Add:** `blueprint::set_function_thread_safe(asset, function, bool)` (mirror `OnIsThreadSafeFunctionModified`) + expose `thread_safe` in `get_functions` output.
   - **Refs:** `BlueprintDetailsCustomization.cpp:6421-6461`; `UK2Node_FunctionEntry::MetaData.bThreadSafe` → `MD_ThreadSafe`.

2. **Override / implement an interface `BlueprintNativeEvent`.** `override_parent_function` on an already-implemented interface event (e.g. `GetProceduralSourceActors`) throws a duplicate-name compile error.
   - **Add:** `blueprint::implement_interface_event(asset, interface, event)` that binds the inherited UFunction instead of redeclaring.

3. **Read/write `FBoneReference` node-internal properties.** AnimGraph node bone-ref fields (e.g. ProceduralHandIK `HandL/R`, `TargetHandL/R`, `LowerarmL/R`; ProceduralAimOffset `SpineBoneParams`) are not pin-exposed, so `get_node_details` returns nothing for them and there's no setter — forcing manual Details-panel entry for every bone reference.
   - **Add:** read + write support for `FBoneReference` (and arrays of structs containing them, e.g. `TArray<FBoneParams>`) in `get_node_details` / `set_anim_graph_node_property`.

4. **Anim-namespace `add_function` / `add_variable` aliases.** No dedicated anim-namespace creators; must fall back to the `blueprint::` namespace.
   - **Add:** thin anim-namespace aliases for discoverability.

5. **`add_function` rejects an `outputs` param.** Function outputs can't be declared at creation; requires a follow-up `set_function_params`.
   - **Add:** accept `inputs` / `outputs` directly in `add_function`.

6. **Author Instanced polymorphic `TArray` elements (DataAsset presets).** MCP can SIZE a `TArray` of `Instanced` polymorphic UObjects (e.g. `UProceduralPresetData.Presets : TArray<UProceduralPreset*>`) but cannot set each element's CONCRETE CLASS or populate its nested struct fields (`FSwayData`). `seed_data_asset` / `set_cdo_properties` / `bulk_fill_query apply` / `set_cdo_property` treat each element as opaque (garbage fields placed INSIDE an element pass even under `strict:true`); `set_cdo_property` ImportText on the array path forces JSON and rejects ImportText grammar; `describe_query` can't introspect the nested `FStruct` layout (resolves bare types to `UScriptStruct` internals). This forces full MANUAL editor authoring of preset DataAssets (blocked the `DA_Viper_PFPPreset` native-preset rebuild 2026-05-23).
   - **Add:** instanced-element authoring (set element concrete class + nested struct values) in `set_cdo_property`/`seed_data_asset`, + `FStruct` layout introspection in `describe_query`.
