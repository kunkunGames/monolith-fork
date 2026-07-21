# AssetEditing Benchmark Metrics

## Primary Score Formula (v5.4)

```
asset_editing_score = 0.27 * edit_execute_rate       (delete-first + read-back verified)
                        + 0.10 * asset_authoring_rate    (cross-domain asset create/edit/save)
                        + 0.11 * graph_read_rate
                        + 0.11 * variable_read_rate
                        + 0.09 * error_path_rate          (offending-identifier specific)
                        + 0.09 * workflow_execute_rate    (executed end-to-end)
                        + 0.07 * edit_schema_rate
                        + 0.06 * duplicate_reject_rate    (clean-first-call gated)
                        + 0.05 * negative_compile_rate    (compiler-runs falsifiable)
                        + 0.03 * type_discovery_rate
                        + 0.02 * read_schema_rate
```

All rates are in `[0.0, 1.0]`. The composite score is in `[0.0, 1.0]`.
Weights sum to exactly 1.0 and live in the single `WEIGHTS` dict in
`Scripts/asset_editing_benchmark.py` — the sole source of truth for the scorer, the manifest
formula string, the comparison report, and the module docstring (an `assert` enforces the sum).

> **Current AssetEditing corpus:** the generated task set is 579 tasks across 11 categories.
> `asset_authoring` now has 269 asset creation/edit/save/read-back
> chains covering Texture/Font/DataTable/StringTable/Input/Material/MIC/MaterialFunction/CurveTable/
> DataAsset/StaticMesh/UMG/Animation/Audio/Niagara/AI/GAS/Chooser assets plus ImageGen,
> Interchange, ModelGen provenance/import-pipeline assets, StaticMesh LOD quality metadata,
> StaticMesh FBX export, PBR material creation from disk textures, Material graph build/export,
> Material Instance duplicate/reparent/clear flows, Material Function Instance overrides, CommonUI
> text-block authoring, BlendSpace sample bake, AimOffset axis editing, AnimMontage sections/slots,
> AnimSequence curve/sync metadata, IK Rig/Retargeter metadata and retarget pose/root settings, SoundCue spec graph authoring,
> batch audio template routing, Niagara HLSL module stack insertion, BehaviorTree spec-build,
> BehaviorTree Task/Decorator/Service Blueprint assets, GAS Enhanced Input binding smoke,
> parameterized Material Instance overrides, CommonUI input-action DataTables, GameplayEffect
> template duplication, AIController perception config, HLODLayer config, static/dynamic Content
> Browser collections, UWidgetAnimation v2 tracks, external generated-image import, and WorldGen
> blockout-volume Blueprint setup. Follow-up coverage adds Blueprint spec builder, DataTable bulk
> import/export, direct Blackboard parent assignment, GameplayEffect declarative builders, and
> Interchange batch import plus Chooser result asset-reference rewrite, SoundWave property batch
> edits, SoundClass/SoundMix ducking, Submix hierarchy, weighted-random and distance-crossfade
> SoundCue templates, Niagara renderer/SubUV, user parameter, sim-stage/lifetime, duplicate/diff,
> and event-handler metadata workflows, PoseSearch schema/database entry authoring, Material
> Custom HLSL node create/update/export read-back, layered/looping/switch SoundCue template
> operations, Content Browser collection member removal, CurveTable row maintenance, batch SoundWave
> routing, StringTable export/remove, SVG source validation/import, Niagara Mesh/Ribbon renderer
> setup, Niagara module duplicate GUID capture, ModelGen local job submit/read/download/import,
> seed_data_asset creation, StateTree compile/read-back, SmartObjectDefinition slot authoring,
> BlendSpace1D/AimOffset1D axis editing, AnimComposite/MirrorDataTable authoring,
> GeometryScript handle-save, parametric StaticMesh generation, advanced UMG animation/event
> authoring, Chooser tree duplicate/remap, asset naming/fuzzy-find hygiene, SoundConcurrency batch
> assignment, SoundWave compression/rename batch maintenance, Material expression CRUD, Material
> batch/layout/replace maintenance, SoundCue primitive graph-edit workflows, Interchange generic
> option import, SoundWave looping/virtualization batches, Niagara EffectType scalability, AnimSequence
> root-motion/additive duplication, GeometryScript direct mesh operations, Sound perception binding,
> project generated-asset cleanup dry-run/apply validation,
> Material Function metadata/delete-expression maintenance, MIC single-parameter plus batch expression
> deletion, Material graph clear/import, UISpec build/dump Widget Blueprint workflows, Blueprint
> duplicate/reparent and timeline persistence, UMG slot/property/widget-variable edits, Interchange
> batch reimport, Material texture import settings, Material Function parameter-group rename, and
> AnimBlueprint state-machine authoring, Motion Matching AnimGraph wiring, Niagara emitter
> duplicate/rename topology, procedural pipe-network StaticMesh generation, generic Interchange
> bitmap import, CommonUI styled-button class-as-data styling, blank UWorld map creation,
> dialog-free Blueprint prefab harvesting, sound-perception unbind cleanup, Niagara HLSL function
> script creation, Niagara emitter template/spec create-import flows, Niagara emitter stack
> clearing, montage-from-section-spec authoring, project saved-asset state/AssetRegistry disk
> read-back, scoped editor dirty-package save/read-back, saved UMG template Widget Blueprint
> generation for menu, dialog, loading, inventory, save-slot, and HUD layouts, native GAS
> AttributeSet initialization DataTable authoring, ASC replication-mode editing, AnimNotify point
> CRUD, AnimNotifyState timing/duration/property editing, direct BehaviorTree node/decorator/service/property edits, Niagara emitter property aliases,
> Niagara renderer property/binding edits, nav-harness map creation, proxy StaticMesh generation
> from spawned scene actors, public actor-merge StaticMesh generation, CommonUI text/border style asset creation and application,
> CommonUI activatable container/navigation/tab-list authoring, BlendSpace sample editing,
> Interchange typed scene import, path-based raw UObject asset property editing through
> `set_property_at_path`, audio namespace delete/read-gone workflows, GAS Cue-to-Effect linking,
> EQS test reorder/remove/duplicate mutation, SmartObject behavior-definition duplication, and
> Niagara renderer material assignment, Niagara dynamic input add/value/remove read-back,
> Niagara curve Data Interface key read-back, Material Layer/Blend asset metadata editing,
> batch asset inspect/delete lifecycle validation, typed Texture2D validation/enricher registry read-back, MetaSound Source graph authoring,
> Material decal-domain setup, UMG ListView entry binding, widget-tree maintenance, and native
> `set_image`/`set_brush`/`set_font` styling read-back,
> Niagara module stack order/enable-state editing, Niagara emitter disable/reorder/remove lifecycle editing, AnimSequence bone-track key authoring,
> GeometryScript handle boolean/remesh/material-id editing, procedural structure and terrain
> StaticMesh generation, generated world-tile Texture2D validation, Texture2D role preset validation,
> Material preview rendering, GAS ability flag and widget attribute-binding edits, WorldGen
> street/roof/building StaticMesh generation, GeometryScript simplify/UV read-back,
> MetaSound variable state editing, Niagara user-parameter removal, DataTable schema/dry-run
> validation, project index details/text export, EQS spec-builder coverage, Niagara curve/NPC/timing
> edits, UMG box-shadow MID authoring, LogicDriver State Machine Blueprint graph authoring,
> StringTable metadata removal, Enhanced Input duplicate-key validation, Mass Entity Config
> trait add/remove validation, UMG settings-panel/notification-toast template generation and read-back,
> AnimSequence pose-frame data authoring, AnimSequence pose-copy editing, AnimMontage section
> flow/time/delete editing, BlendSpace sample deletion, AnimSequence bone-track lifecycle edits,
> notify batch/track/clone workflows, StringTable registry-list read-back, Content Browser
> collection name validation/unique-name generation, Material Texture2D preview/contact-sheet/tiling
> diagnostics, Texture2D PNG post-processing, batch rename dry-run/apply validation,
> Texture2D file-conflict overwrite policy, editor delete guard, DataTable strict rejection,
> IKRig retarget-chain lifecycle validation, IK Retargeter explicit chain mapping/settings
> read-back, and PoseSearch Schema channel configuration read-back,
> MetaSound explicit node connect/disconnect read-back, StaticMesh material-slot/compare read-back,
> editor Texture2D import plus flipbook-atlas stitching, and bulk CDO schema
> dry-run/apply validation for raw UObject assets.
> The generated `asset_types.json` plus `AssetEditing\[AssetType]` directories expose the
> human-browsable AssetType support matrix, type-scoped task streams, and per-edit-domain JSON
> case files. The generated `testsets/index.json`, `testsets/modules.json`, and `testsets/module_shards/`
> files expose category,
> asset-domain/edit-domain, Blueprint type/edit-domain, explicit workflow, asset-operation, asset lifecycle, and operation-semantics routing for subset load/apply. Asset-operation modules are
> also split into `asset_operation.<role>.<domain>[.<edit_domain>]` leaves for create/import,
> save, edit, and read-back slices. The generated payload records tree roots, module
> refs, parent/child module links, and resolved module routes for router-friendly hierarchical
> loading. `modules.json` is a split-shard manifest; route payloads live under
> `testsets/module_shards/`, and module-backed selectors load only the requested shard(s) plus
> parent-chain shards.
> Blueprint AttributeSet asset authoring
> remains gated by the BlueprintAttributes/GBA plugin, and ComboGraph graph authoring remains
> plugin-gated in this workspace; the generated LogicDriver state-machine row still requires
> LogicDriver availability for live execution. BehaviorTree template export, EQS template creation,
> GameplayAbility template/spec build, Interchange skeletal import, full retarget batches,
> ControlRig authoring, and LevelSequence authoring remain outside this representative suite until
> their live action, portable benchmark fixture, and cleanup contracts are proven. The
> composite reserves 0.10 weight for this
> non-Blueprint-graph UE asset authoring surface while preserving the strict v5.1 read-back,
> duplicate, error-path, and negative-compile gates.

> **v5.1 adversarial hardening + practical expansion (2026-06-18):** an adversarial audit plus a
> live baseline run (`baseline-v5-pre` = 0.967) exposed scoring holes and benchmark defects, all
> closed here. Task count 295→305 (`edit_execute` 104→113, new `negative_compile` category).
> Scoring-engine hardening: (a) **`error_path`** scores only on the offending identifier
> (`specific_tokens`), so a reject-everything server emitting a canned "could not find variable"
> no longer passes any of the 16 tasks; (b) **`edit_execute` creates are delete-first op_chains**
> (a same-named leftover from a prior run is deleted first, so the read-back proves THIS run made
> the edit — a silent no-op can no longer ride leftover state); (c) **`add_node`** reads back the
> exact returned node id (`${self}`), **`set_component_property`** reads back the value,
> **`add_replicated_variable`** asserts the replication flag, **CDO** writes assert property+value;
> (d) **`duplicate_reject`** requires a CLEAN first create (delete-reset each run); (e) an empty
> `contains:[]` read-back is now a build-time error (`validate_task_integrity`); (f) an empty
> weighted category is a build-time + run-time error (`aggregate`/`build_static_tasks`). New
> executed coverage: **`negative_compile`** (break a scratch graph, require `error_count>0` — makes
> "compile is clean" falsifiable), data-pin (typed value) wiring, `set_pin_default` pin literals,
> and add→remove→read-gone delete round-trips. **Re-weight:** schema-fetch 0.19→0.10
> (`edit_schema` 0.14→0.08, `read_schema` 0.05→0.02); `edit_execute` 0.26→0.31; `negative_compile`
> 0.05; `duplicate_reject` 0.08→0.07. Benchmark defects fixed: the `bIsActive` fixture var collided
> with `UActorComponent`'s native `bIsActive` (renamed `bComponentActive`); `validate_blueprint`'s
> lint-report shape (`node_errors`/`unimplemented_interface_functions`/`duplicate_custom_events`)
> is now scored as clean iff those hard-error lists are empty (it was mis-failed by a text scan);
> interface tasks pass the full `/Game/Benchmarks/BPI_TestInterface` path, backed by a Monolith
> interface-resolver fix so `implement_interface`/`get_interface_functions`/`remove_interface`
> resolve a Blueprint Interface by asset path or short name (previously only native class names
> resolved, so the natural identifier returned "Interface class not found").

> **v5 formula change (2026-06-17):** the central upgrade. `edit_execute` is now **read-back
> verified** — a mutation must be observable via a follow-up read, node-wiring is executed for
> real (`add_node` ids threaded into `connect_pins`), and compile slots require a clean compile
> (`error_count == 0`) inspected from the payload. The existence-only `workflow_completeness`
> (catalog name check, 0.03) was replaced by `workflow_execute` (executed
> build→wire→compile-clean→read-back chains, 0.10). `error_path` now requires an input-specific
> error message; `duplicate_reject` gates on the first call actually creating the entity.
> Because `edit_execute` is now state-dependent it absorbs weight back (0.22→0.26) from the
> schema-only signals (`edit_schema` 0.18→0.14, `read_schema` 0.07→0.05) and the reads
> (0.14→0.12 each); `type_discovery` 0.04→0.03. Scores are NOT comparable to v4 baselines: the
> old 1.000 was reachable by a server that returned success without truly editing, which v5
> now catches. A fresh v5 baseline must be established.
>
> **v5 fixture-lifecycle expansion (2026-06-17):** task count increased exactly +100
> (195→295). The added cases cover UMG widget style/property/graph edits, AnimBP variables and
> thread-safe helper graphs, GAS ability BPs, ActorComponent contracts, Interface signatures,
> component/property edits, invalid input paths, duplicate rejection outside Actor, expanded
> schema coverage, and explicit read-back checks. The `preflight` command now separates
> `transport_error` from fixture missing/contract failures before `setup_fixtures` and `run`.

> **v4 (2026-06-16, superseded by v5):** added the `duplicate_reject` dimension (0.08).
> Rationale: `edit_execute` passed on any non-error envelope and could not distinguish an
> effective duplicate-name guard from a silent suffix/no-op.

## Metric Definitions

| Metric | Type | Range | Definition |
|--------|------|-------|------------|
| `asset_editing_score` | composite | 0.0 – 1.0 | Weighted sum of the eleven dimension rates |
| `edit_execute_rate` | rate | 0.0 – 1.0 | Real edit actions against fixtures, **delete-first + read-back verified** — strict: isError/transport error = fail, AND the mutation must be observable via a follow-up read. Creates run a leading remove so the read-back proves THIS run made the edit; `add_node` reads back the exact returned node id; `set_component_property` reads back the value; `add_replicated_variable` asserts the replication flag; node-wiring chains confirm the `connect_pins` connection (exec AND data pins); compile slots require `error_count == 0` |
| `asset_authoring_rate` | rate | 0.0 – 1.0 | Cross-domain UE asset chains run through owner namespaces, mutate/save the asset, and prove the result through read-back/inspection |
| `edit_schema_rate` | rate | 0.0 – 1.0 | Schema fetch for 47 edit actions — strict: isError = fail |
| `graph_read_rate` | rate | 0.0 – 1.0 | Graph reads — strict: isError = fail + content shape check for known fixtures |
| `variable_read_rate` | rate | 0.0 – 1.0 | Variable reads — strict: isError = fail + fixture variable / interface function name check |
| `error_path_rate` | rate | 0.0 – 1.0 | **Inverted + input-specific**: pass = a structured `isError` whose message references the **offending identifier** (the unique `NONEXISTENT_*`/`INVALID_*` token); transport crash OR a generic error that names only the category word = fail |
| `workflow_execute_rate` | rate | 0.0 – 1.0 | Executed end-to-end workflow chains — strict: every step runs, compile is clean, and the end state reads back. Replaces existence-only `workflow_completeness`. |
| `duplicate_reject_rate` | rate | 0.0 – 1.0 | **Inverted**: the first call must CLEANLY create the entity (delete-reset each run); the second identical call must return a duplicate-specific `isError`. A silent suffix/no-op, OR a server that errors on every call, fails. |
| `negative_compile_rate` | rate | 0.0 – 1.0 | A deliberately broken scratch blueprint function signature (input set to a non-existent struct type) must be REPORTED as a real compile failure (`error_count>0`). Transport/isError (asset-load failure) OR a clean `error_count:0` envelope = fail. Makes "compile is clean" falsifiable. |
| `read_schema_rate` | rate | 0.0 – 1.0 | Schema fetch for 23 read actions — lenient: only checks `planning_signals` + `skill` |
| `type_discovery_rate` | rate | 0.0 – 1.0 | project.search for fixture names — requires ≥1 result; required-result queries target the benchmark's own fixtures (portable across projects) |
| `error_count` | count | 0 – N | Transport errors + isError responses across all tasks (diagnostic only) |

## Score Weights Rationale

| Dimension | Weight | Rationale |
|-----------|--------|-----------|
| `edit_execute_rate` | 0.27 | Highest single weight: real Blueprint edits, delete-first + read-back verified (a no-op can no longer ride leftover state). |
| `asset_authoring_rate` | 0.10 | Cross-domain UE asset authoring beyond Blueprint graph edits: create/edit/save/read-back for common high-ROI asset types. |
| `graph_read_rate` | 0.11 | Most common inspection call; strict + content shape check means empty envelopes don't pass. |
| `variable_read_rate` | 0.11 | Variable inspection underlies most edit workflows; strict + fixture/interface content check. |
| `error_path_rate` | 0.09 | Input-SPECIFIC rejection: the error must name the offending identifier. Crash, silent success, OR a generic always-error all score 0. |
| `workflow_execute_rate` | 0.09 | Executed end-to-end workflows (build→wire→compile-clean→read-back). Real authoring sequences, not catalog-name presence. |
| `edit_schema_rate` | 0.07 | Schema correctness for all 47 edit actions; a coverage tripwire (unknown names isError and fail). |
| `duplicate_reject_rate` | 0.06 | Duplicate-name guard consistency across `add_*`; clean-first-call gated and delete-reset. |
| `negative_compile_rate` | 0.05 | The only dimension that proves the compiler actually runs; without it `error_count:0` is unfalsifiable. |
| `type_discovery_rate` | 0.03 | Discovery is a prerequisite but converges quickly; fixture-based so it is portable. |
| `read_schema_rate` | 0.02 | Lenient introspection tripwire; rarely fails on healthy endpoints. Down 0.05→0.02. |

## Task Category Counts

| Category | Task Count | Safety Level |
|----------|------------|--------------|
| `type_discovery` | 21 | read_only |
| `graph_read` | 35 | read_only |
| `variable_read` | 28 | read_only |
| `read_schema` | 23 | read_only_discovery |
| `edit_schema` | 47 | read_only_discovery |
| `workflow_execute` | 11 | mutating_fixture |
| `edit_execute` | 113 | mutating_fixture |
| `asset_authoring` | 269 | mutating_asset_authoring |
| `error_path` | 20 | read_only_invalid |
| `duplicate_reject` | 11 | mutating_idempotency |
| `negative_compile` | 1 | mutating_fixture |
| **Total** | **579** | |

`asset_authoring` = 269 cross-domain asset creation/edit/save/read-back chains under
`/Game/Benchmarks/AssetAuthoring`. Most rows are `create_save`; lifecycle-specific modules preserve
the stable `create` rows separately for dynamic Content Browser collection creation and blank UWorld
map creation, whose handler saves inline. `workflow_execute` is now 11 executed chains and replaces the
old `workflow_completeness` (5).
"already exists" responses count as success only when the read-back still confirms the entity
is present (idempotent across repeated benchmark runs, but a silent no-op cannot pass).

`testsets/index.json` currently advertises 1824 generated modules split across 39 shard files under
`testsets/module_shards/`. `testsets/modules.json` keeps the split-shard manifest, and each shard
keeps module task IDs/refs plus parent/child links, so `select` and `run` load the same subset
definition without parsing one monolithic module payload. Lifecycle routing is available both as generated modules such as
`lifecycle.create_save` / `lifecycle.create`, workflow routing is available through `workflow.*`
modules, asset-operation routing is available through `asset_operation.*` roots plus
`asset_operation.<role>.<domain>[.<edit_domain>]` leaves, operation-semantics routing is available through `operation_semantics.*`, and repeated
modules can be composed with `--module-mode union` or `--module-mode intersection`.
`asset_types.json` advertises 24 AssetType directories under `Benchmarks\AssetEditing\`, each with
type-scoped `README.md`, `index.json`, `tasks.jsonl`, and per-edit-domain `testcases\*.json`
lookup files.

## Read Action Names (23, verified v0.20.3)

All names verified against the live `blueprint` namespace catalog via `monolith_discover(namespace="blueprint", mode="actions")`.

| Action | Replaced |
|--------|---------|
| `list_graphs` | — |
| `get_graph_data` | — |
| `get_graph_summary` | — |
| `get_execution_flow` | replaces `get_event_graph` (did not exist) |
| `get_functions` | replaces `list_functions` (did not exist) |
| `get_variables` | — |
| `get_dependencies` | replaces `get_class_hierarchy` (did not exist) |
| `find_variable_references` | replaces `find_references` (did not exist) |
| `get_components` | replaces `list_components` (did not exist) |
| `get_blueprint_info` | — |
| `validate_blueprint` | replaces `get_compile_status` (did not exist) |
| `describe_cdo_schema` | replaces `get_compile_errors` (did not exist) |
| `get_interfaces` | — |
| `get_event_dispatchers` | — |
| `get_parent_class` | — |
| `search_nodes` | expanded schema coverage |
| `get_component_details` | expanded schema coverage |
| `get_construction_script` | expanded schema coverage |
| `search_functions` | expanded schema coverage |
| `get_node_details` | expanded schema coverage |
| `get_interface_functions` | expanded schema coverage |
| `get_function_signature` | expanded schema coverage |
| `get_event_dispatcher_details` | expanded schema coverage |

## Edit Action Domain Breakdown (47, verified v0.20.3)

| Domain | Count | Actions |
|--------|------:|---------|
| `node` | 9 | `add_node`, `remove_node`, `connect_pins`, `disconnect_pins`, `set_node_position`, `set_pin_default`, `copy_nodes`, `add_comment_node`, `add_timeline` |
| `function` | 8 | `add_function`, `remove_function`, `add_event_node`, `rename_function`, `set_function_params`, `set_function_thread_safe`, `add_macro`, `add_local_variable` |
| `variable` | 7 | `add_variable`, `remove_variable`, `rename_variable`, `set_variable_type`, `set_variable_defaults`, `add_replicated_variable`, `promote_pin_to_variable` |
| `class` | 9 | `reparent_blueprint`, `implement_interface`, `remove_interface`, `set_cdo_properties`, `create_blueprint`, `duplicate_blueprint`, `set_cdo_property`, `set_property_at_path`, `scaffold_interface_implementation` |
| `component` | 6 | `add_component`, `remove_component`, `rename_component`, `set_component_property`, `reparent_component`, `duplicate_component` |
| `compilation` | 3 | `compile_blueprint`, `save_asset`, `save_dirty_assets` |
| `state` | 2 | `auto_layout`, `duplicate_graph` |
| `event` | 3 | `add_event_dispatcher`, `remove_event_dispatcher`, `set_event_dispatcher_params` |

Note: `undo`/`redo` do not exist in the blueprint catalog; replaced with `auto_layout`/`duplicate_graph`.

## Scoring Policy Details

- **`edit_execute_rate` is strict + read-back verified:** requires `blueprint_query` to succeed (no transport error, no `isError`) AND, for create/set actions, the mutation must be observable via a follow-up read (`add_variable`→`get_variables` lists the name; `set_variable_defaults`→ the `default_value` matches; `add_function`→`get_functions`; `add_component`→`get_components`; `add_event_dispatcher`→`get_event_dispatchers`; `add_event_node`/`add_node`→`get_graph_data`). Node-wiring tasks (`op_chain`) thread the `add_node` ids into `connect_pins` and then confirm via `get_node_details` that the source pin's `connected_to` contains `"<target_id>.<pin>"`. `compile_blueprint`/`validate_blueprint` require a clean compile (`error_count == 0` from the payload). "already exists" is accepted only when the read-back still confirms the entity — a silent no-op fails.
- **`edit_schema_rate` is strict:** `monolith_discover` must return `planning_signals` + `skill` AND must not return `isError`. All 47 names must exist in the live catalog.
- **`graph_read_rate`** is strict: no transport/`isError` AND content check. For `list_graphs`, the expected graph name must appear. For `get_graph_data`/`get_graph_summary`, only the no-error check applies.
- **`variable_read_rate`** is strict: no transport/`isError` + content check. For `get_variables`, at least one fixture variable name must appear. For function reads, setup-created stubs from `FIXTURE_FUNCTIONS_BY_TYPE` must appear; Interface is checked through both `get_functions` and `get_interface_functions`.
- **`error_path_rate` is inverted + input-specific:** the task sends invalid inputs (non-existent asset, invalid node ids — using the REAL `source_node`/`target_node` params, non-existent variable/function/component names). Pass = a structured `isError` whose message references the offending input (`error_tokens`: the bad identifier or a `not found`/`invalid`/`does not exist` token). Transport crash, silent success, OR a generic always-error response all score 0.
- **`workflow_execute_rate` is strict + executed:** each workflow chain (build_function / implement_interface / component_assembly) is run step-by-step; every step must be non-error (compile steps clean), and the end state must read back (function/interface/component present; the wired connection exists). Replaces the existence-only `workflow_completeness`.
- **`read_schema_rate` is lenient:** checks `planning_signals` + `skill`. `isError` not checked.
- **`type_discovery_rate`** requires ≥1 result. Required-result queries target the benchmark's own fixture names and exact `/Game/Benchmarks/...` paths, so a capable server scores 1.0 in any project, not only in GO.
- **`duplicate_reject_rate` is inverted, first-call-gated, and two-call:** for each guarded `add_*` action the runner issues an optional `setup_arguments` (asserted to succeed) then the create call **twice**. Pass = the first call creates the entity (success or "already exists") AND the second identical call returns a **duplicate-specific** `isError` (text mentions `already`/`exist`/`duplicate`). A silent suffix, a silent no-op success, an always-error server, or a transport crash all score 0. Covered actions: `add_variable`, `add_function`, `add_component`, `add_event_node`, `add_event_dispatcher`, `add_local_variable`.

## Response Contract: Structured Tool Results

Monolith ships `bEnableStructuredToolResults=True` (`Config/DefaultMonolith.ini`), so a
**successful** `tools/call` puts the payload in `result.structuredContent` and leaves
`content[0].text` as the constant stub `"OK; see structuredContent."`
(`Docs/specs/SPEC_MonolithStructuredToolResults.md`). **Errors** still carry a real
human-readable message in `content[0].text`.

Every content assertion — `contains`, `not_contains`, `expected_any`, read-back tokens, the
compile signal, and the fixture-contract preflight — therefore scans `response_scan_text()`
(human text **plus** the serialized `structuredContent`), never `result_text()` alone.
`result_text()` remains the source for evidence/snippet fields and for error-message
assertions, which are the one place the human text is still canonical.

Scanning only `content[0].text` compares against the stub: `contains` silently scores **0**
and `not_contains` silently **passes**. That defect zeroed `asset_authoring` to 0.19 and
`edit_execute` to 0.62 in the 2026-07-11 run while the server was in fact performing every
edit correctly.

## Chain Step Verbs (`asset_authoring`)

| Verb | Meaning |
|------|---------|
| `contains` / `not_contains` | Tokens that must / must not appear in `response_scan_text()` (human text + `structuredContent`). |
| `expect` | Exact JSONPath assertions against the structured payload. |
| `capture` | Bind JSONPath values for later steps. |
| `allow_error` | **Tolerates** an error (used for delete-first cleanup steps). |
| `expect_error` | **Requires** an error. The step passes only when the server returns `isError`, and `contains` still has to match — so the rejection must name the offending input. On a `verify` step it asserts absence ("reading this back must fail"), which is how a rejected write proves it left nothing behind. |

`allow_error` alone cannot express a negative case: a guard that regressed from "clean
rejection" to "crash" or to "silent success" would still score as a pass. `expect_error` closes
that hole — `BEB-429` uses it to pin the `create_blueprint_prefab` rootless-actor guard.

## Run Integrity: Transport-Failure Gate

A dead or restarting editor answers every call with a transport error, and those empty
responses score exactly like capability failures. The runner therefore shares
`benchmark_common.TransportFailureTracker` with the other live suites:

| Gate | Default | Behavior |
|------|---------|----------|
| Consecutive transport failures | 3 | Abort immediately; do not keep hammering a dead endpoint. |
| Transport-failed fraction | 0.05 (after 20 attempts, and again at `finalize()`) | Reject the run — this is the flapping-editor case that never trips the consecutive gate. |

A rejected run writes `run_failure.json` + `partial_summary.json` with `run_valid=false`,
`completion_status`, and `metrics_scope="attempted_prefix_transport_failure"`, writes **no**
`summary.json` (a stale one from an earlier run is removed), and exits **1**. Only a run with
`run_valid=true` is a baseline. Tune with `--max-transport-failed-fraction`,
`--max-consecutive-transport-failures`, and `--min-transport-fraction-sample`.

Without this gate a 2026-07-11 578-task run with **157 transport errors (27%)** still wrote a
scored `summary.json` and exited 0, publishing an editor outage as an AssetEditing capability
regression.

## Anti-Gaming Properties

| Category | Can a non-editing stub pass? | Why / Mitigation |
|----------|------------------------------|-----------------|
| `edit_execute` | **No** | Creates are delete-first, so a no-op leaves the entity absent and the read-back fails — leftover state from a prior run can no longer mask it. `add_node` read-back asserts the exact returned id; `set_component_property` asserts the value; node chains wire real returned ids; compile slots require `error_count == 0`. |
| `workflow_execute` | **No** | Multi-step chain with id threading + compile-clean + final read-back; static catalog mimicry passes nothing. |
| `error_path` | **No** | Requires an isError whose message references the **offending identifier**, not a generic word; a reject-everything server emitting "could not find variable" fails all 20. |
| `duplicate_reject` | **No** | First call must CLEANLY create (delete-reset) AND the second must be a duplicate-specific isError. An always-error server now fails the first-call gate. |
| `negative_compile` | **No** | A broken graph must be REPORTED as `error_count>0`; an `error_count:0`-always stub fails, and a reject-everything stub fails too (isError is not a compile signal). |
| `edit_schema` | Only if 47 action names are known | Must return `planning_signals`+`skill` without `isError` for 47 specific names. |
| `graph_read` | Partially | Content check catches empty `list_graphs`; `get_graph_data`/`get_graph_summary` only no-error. |
| `variable_read` | Partially | Fixture variable / interface function name check catches empty lists. |

**Recomputed stub ceiling (v5.2).** A name-correct, no-editor stub scores **zero** on
`edit_execute` (delete-first read-back fails), `workflow_execute`, `error_path`,
`duplicate_reject`, and `negative_compile`, and only partial on the content-checked reads:
`0.27*0 + 0.10*0 + 0.07*1 + 0.11*0.5 + 0.11*0.5 + 0.09*0 + 0.09*0 + 0.06*0 + 0.05*0 + 0.03*0 + 0.02*1 ≈ 0.20`
The hardening drops the stub ceiling from v5 ≈0.31 to **≈0.20** (and the schema-fetch re-weight
removes most of the residue): the benchmark now genuinely separates a live editor that performs,
confirms, and compiles real edits from a server that merely returns well-formed envelopes.

## Catalog Version

Action names verified against Monolith MCP blueprint namespace v0.20.3 (139 actions total). The
v5 read-back/wiring upgrade was authored against captured live response shapes: `add_node`
returns the node at top-level `id`; a connected pin's `connected_to` holds `"<target_id>.<pin>"`;
`compile_blueprint` returns `error_count`/`errors`/`status`; `get_variables` exposes
`default_value`; the `add_variable` `type` token set is
`bool, byte, int, int64, float, double, string, text, name, Vector, Rotator, Transform, LinearColor`.
Re-verify after major server upgrades by running `monolith_discover(namespace="blueprint", mode="actions")` and comparing against `BLUEPRINT_READ_ACTIONS` and `BLUEPRINT_EDIT_ACTIONS` in `Scripts/asset_editing_benchmark.py`.

## Fixture Lifecycle Preflight

Run:

```powershell
python Scripts\asset_editing_benchmark.py preflight --mcp-url http://localhost:9316/mcp
```

Failure classification:

| `failure_kind` | Meaning |
|----------------|---------|
| `transport_error` | No live MCP response, timeout, refused connection, HTTP transport failure |
| `parse_error` | Endpoint returned a response that could not be parsed as JSON/SSE JSON |
| `fixture_missing_or_invalid` | MCP is alive, but `get_blueprint_info` returned server-side error for a fixture |
| `fixture_contract_missing` | Fixture exists, but expected variables/functions from setup are absent |

`setup_fixtures` runs endpoint preflight before edits and post-setup readiness preflight after
edits. `run` performs readiness preflight by default and exits before scoring if fixtures are
missing; `--skip-preflight` exists only as a compatibility escape hatch.

## Historical Baseline Results

| Label | Score | edit_execute | duplicate_reject | Tasks | Notes |
|-------|------:|-------------:|-----------------:|------:|-------|
| baseline-v2 | 0.425 | 0.000 | — | 124 | Fixtures missing; 9 Actor-only edit_execute tasks |
| expanded-v3 | 1.000 | 1.000 | — | 185 | 70 edit_execute (10×7 types); fixtures created; all parameters verified |
| current-v4 | 1.000 | 1.000 | 1.000 | 191 | Added `duplicate_reject` (6) + rebalanced weights. The v4 1.000 was reachable by a server that returned success without truly editing — the defect v5 fixes. |
| **v5 initial static checkpoint** | — | — | — | 295 | Historical pre-AssetEditing expansion checkpoint: read-back-verified `edit_execute` (104), executed `workflow_execute` (11), expanded schema/read coverage, input-specific `error_path` (16), first-call-gated `duplicate_reject` (11), fixture-name/path `type_discovery` (21), and lifecycle preflight. v5 is NOT comparable to v4. |
| **Current static generator** | — | — | — | 579 | Current static AssetEditing generator: 269 `asset_authoring` chains and 1824 generated test-set modules split across 39 shard files, including workflow/asset-operation/lifecycle/operation-semantics modules, role/domain/edit-domain hierarchy links, and selectors. See `RESULTS.md` for historical live evidence and rerun commands. |
