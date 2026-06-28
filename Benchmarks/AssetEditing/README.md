# AssetEditing Benchmark

Measures Monolith MCP asset editing capability across Blueprint graph/class/component fixture workflows and high-ROI cross-domain Unreal asset creation, editing, save, and read-back chains.

## Files

| File | Purpose |
|------|---------|
| `tasks.jsonl` | 577 benchmark tasks across 11 categories |
| `manifest.json` | Benchmark metadata, score formula, weights, category counts |
| `asset_types.json` | Generated AssetType support matrix and links to per-type directories |
| `[AssetType]\README.md` | Generated summary for one asset type/domain, including supported operations and test cases |
| `[AssetType]\tasks.jsonl` | Full task payloads for one asset type/domain |
| `[AssetType]\testcases\*.json` | One edit-domain test case slice per AssetType |
| `testsets/index.json` | Generated hierarchy for category, asset-domain, edit-domain, Blueprint, workflow, asset-operation, lifecycle, and operation-semantics routing |
| `testsets/modules.json` | Split-shard manifest for generated module payloads |
| `testsets/module_shards/` | Generated route shards loaded by `select`/`run` only for requested modules |
| `METRICS.md` | Metric definitions, weights, scoring policy, anti-gaming notes |
| `RESULTS.md` | Benchmark results |
| `test_blueprints.md` | Fixture Blueprint contract used by graph, variable, function, and edit-read-back tasks |
| `Scripts/asset_editing_benchmark.py` | Runner/generator script |

## Generated Corpus and Release Packaging

The JSON/JSONL corpus is tracked for local benchmark reproducibility, but it is intentionally
excluded from Monolith runtime release ZIPs by `Scripts\make_release.ps1`. Excluded release payloads
include the root `tasks.jsonl`, `manifest.json`, `asset_types.json`, `testsets\`, and generated
per-AssetType `index.json`, `tasks.jsonl`, and `testcases\*.json` slices.

Local source-checkout use is unchanged. To rebuild the corpus after generator changes, run:

```powershell
python Scripts\asset_editing_benchmark.py generate `
  --tasks Benchmarks\AssetEditing\tasks.jsonl `
  --manifest Benchmarks\AssetEditing\manifest.json `
  --testsets Benchmarks\AssetEditing\testsets
```

For external benchmark distribution, package the generated corpus separately from runtime release
ZIPs and include the Monolith commit SHA plus the generated task, manifest, and testset hashes.
Restore that archive under `Benchmarks\AssetEditing\...` before running `select`, `list_testsets`,
or routed `run` commands.

## AssetType Support

`generate` keeps the root `tasks.jsonl` as the canonical full-suite stream, then mirrors
`asset_authoring` into `AssetEditing\[AssetType]` directories for human lookup. Each type
directory contains `README.md`, `index.json`, `tasks.jsonl`, and `testcases\*.json` slices.

| AssetType | Tasks | Test cases | Create/import | Edit | Save | Read-back | Directory |
|---|---:|---:|---:|---:|---:|---:|---|
| `ai` | 14 | 14 | 14 | 14 | 14 | 14 | `Benchmarks/AssetEditing/ai` |
| `animation` | 38 | 38 | 38 | 38 | 38 | 38 | `Benchmarks/AssetEditing/animation` |
| `asset` | 11 | 11 | 11 | 11 | 11 | 11 | `Benchmarks/AssetEditing/asset` |
| `audio` | 29 | 29 | 29 | 29 | 29 | 29 | `Benchmarks/AssetEditing/audio` |
| `blueprint` | 4 | 4 | 4 | 4 | 4 | 4 | `Benchmarks/AssetEditing/blueprint` |
| `collection` | 5 | 5 | 5 | 5 | 4 | 5 | `Benchmarks/AssetEditing/collection` |
| `data` | 12 | 12 | 12 | 12 | 12 | 12 | `Benchmarks/AssetEditing/data` |
| `editor` | 5 | 5 | 5 | 5 | 4 | 5 | `Benchmarks/AssetEditing/editor` |
| `gas` | 16 | 16 | 16 | 16 | 16 | 16 | `Benchmarks/AssetEditing/gas` |
| `hlod` | 2 | 2 | 2 | 2 | 2 | 2 | `Benchmarks/AssetEditing/hlod` |
| `imagegen` | 5 | 5 | 5 | 5 | 5 | 5 | `Benchmarks/AssetEditing/imagegen` |
| `input` | 5 | 5 | 5 | 5 | 5 | 5 | `Benchmarks/AssetEditing/input` |
| `interchange` | 8 | 8 | 8 | 8 | 8 | 8 | `Benchmarks/AssetEditing/interchange` |
| `level_instance` | 2 | 2 | 2 | 2 | 2 | 2 | `Benchmarks/AssetEditing/level_instance` |
| `localization` | 6 | 6 | 6 | 6 | 6 | 6 | `Benchmarks/AssetEditing/localization` |
| `logicdriver` | 1 | 1 | 1 | 1 | 1 | 1 | `Benchmarks/AssetEditing/logicdriver` |
| `material` | 21 | 21 | 21 | 21 | 21 | 21 | `Benchmarks/AssetEditing/material` |
| `mesh` | 21 | 21 | 21 | 21 | 21 | 21 | `Benchmarks/AssetEditing/mesh` |
| `modelgen` | 2 | 2 | 2 | 2 | 2 | 2 | `Benchmarks/AssetEditing/modelgen` |
| `niagara` | 29 | 29 | 29 | 29 | 29 | 29 | `Benchmarks/AssetEditing/niagara` |
| `project` | 3 | 3 | 3 | 3 | 3 | 3 | `Benchmarks/AssetEditing/project` |
| `ui` | 19 | 19 | 19 | 19 | 19 | 19 | `Benchmarks/AssetEditing/ui` |
| `worldgen` | 9 | 9 | 9 | 9 | 9 | 9 | `Benchmarks/AssetEditing/worldgen` |

## Task Categories

> **v5.4 (2026-06-26) — AssetEditing rename + UE 5.8 high-ROI Monolith asset-action expansion:** extends the
> `asset_authoring` dimension to 267 asset creation/edit/save/read-back chains. New coverage includes
> ImageGen deterministic Texture2D provenance, ImageGen MSDF texture/material baking, Interchange
> typed texture/audio import, Texture2D PNG post-processing, batch-rename dry-run/apply,
> file-conflict overwrite policy, editor delete guard, DataTable strict rejection, and IKRig
> retarget-chain lifecycle checks, Interchange StaticMesh import/reimport/export, Interchange batch
> import, and ModelGen
> provenance StaticMesh import/collision editing, plus StaticMesh LOD screen-size quality metadata,
> StaticMesh FBX export, PBR material creation from disk textures, Material graph build/export,
> Material Instance duplicate/reparent/clear flows, Material Function Instance overrides, CommonUI
> text-block authoring, BlendSpace sample bake, AimOffset axis editing, AnimMontage sections/slots,
> AnimSequence curve/sync marker metadata, IK Rig/Retargeter metadata and retarget pose/root settings, SoundCue spec graphs,
> batch audio template routing, Niagara HLSL module stack insertion, BehaviorTree spec-build,
> BehaviorTree Task/Decorator/Service Blueprint assets, GAS Enhanced Input binding smoke,
> parameterized Material Instance overrides, CommonUI input-action DataTables, GameplayEffect
> template duplication, AIController perception config, HLODLayer config, static/dynamic Content
> Browser collections, UWidgetAnimation v2 tracks, external generated-image import, and WorldGen
> blockout-volume Blueprint setup. Follow-up rows add Blueprint spec builder, DataTable bulk
> import/export, Blackboard inheritance + direct parent assignment, GameplayEffect declarative
> builders, Chooser result asset-reference rewrite, SoundWave property batch edits, SoundClass/
> SoundMix ducking, Submix hierarchy, weighted-random and distance-crossfade SoundCue templates,
> plus Niagara Sprite renderer/SubUV, user parameter, sim-stage/lifetime, duplicate/diff, and
> declarative GameplayAbility/EQS spec builders, Niagara curve-key editing, NPC parameter removal, system timing,
> event-handler metadata workflows, PoseSearch schema/database entry authoring, Material Custom
> HLSL node create/update/export read-back, layered/looping/switch SoundCue template operations,
> Content Browser collection member removal, CurveTable row maintenance, batch SoundWave routing,
> StringTable export/remove, SVG source validation/import, Niagara Mesh/Ribbon renderer setup,
> Niagara module duplicate GUID capture, ModelGen local job submit/read/download/import,
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
> deletion, Material graph clear/import, UISpec build/dump Widget Blueprint workflows,
> Blueprint FunctionLibrary/MacroLibrary/Interface asset-type authoring, PoseSearch
> NormalizationSet assignment, Chooser context class retargeting, and persisted StaticMesh
> auto-LOD workflows, Blueprint duplicate/reparent and timeline persistence, UMG slot/property/
> widget-variable edits, Interchange batch reimport, Material texture import settings, Material
> Function parameter-group rename, AnimBlueprint state-machine authoring, Motion Matching AnimGraph
> wiring, Niagara emitter duplicate/rename topology, procedural pipe-network StaticMesh generation,
> generic Interchange bitmap import, CommonUI styled-button class-as-data styling, blank UWorld map
> creation, editor Texture2D import plus flipbook-atlas stitching, dialog-free Blueprint prefab harvesting,
> sound-perception unbind cleanup, Niagara HLSL
> function script creation, Niagara emitter template/spec create-import flows, Niagara emitter stack
> clearing, montage-from-section-spec authoring, project saved-asset state/AssetRegistry disk
> read-back, scoped editor dirty-package save/read-back, saved UMG template Widget Blueprint
> generation for menu, dialog, loading, inventory, save-slot, and HUD layouts, native GAS
> AttributeSet initialization DataTable authoring, ASC replication-mode editing, AnimNotify point
> CRUD, AnimNotifyState timing/duration/property editing, direct BehaviorTree node/decorator/service/property edits, Niagara emitter property aliases,
> Niagara renderer property/binding edits, nav-harness map creation, proxy StaticMesh generation
> from spawned scene actors, and public actor-merge StaticMesh generation, CommonUI text/border style asset creation and application,
> CommonUI activatable container/navigation/tab-list authoring, BlendSpace sample edit read-back,
> Interchange typed scene import, path-based raw UObject asset property editing through
> `set_property_at_path`, audio namespace delete/read-gone workflows, GAS Cue-to-Effect linking,
> EQS test reorder/remove/duplicate mutation, SmartObject behavior-definition duplication, and
> Niagara renderer material assignment, Niagara dynamic input add/value/remove read-back, and
> Niagara curve Data Interface key read-back, Material Layer/Blend asset metadata editing,
> batch asset inspect/delete lifecycle validation, typed Texture2D validation/enricher registry read-back, MetaSound Source graph authoring,
> Material decal-domain setup, UMG ListView entry binding and widget-tree maintenance,
> Niagara module stack order/enable-state editing, Niagara emitter disable/reorder/remove lifecycle editing, AnimSequence bone-track key authoring,
> GeometryScript handle boolean/remesh/material-id editing, procedural structure and terrain
> StaticMesh generation, generated world-tile Texture2D validation, Texture2D role preset validation,
> Material preview rendering, GAS ability flag and widget attribute-binding edits, WorldGen
> street/roof/building StaticMesh generation, GeometryScript simplify/UV read-back,
> MetaSound variable state editing, Niagara user-parameter removal, Material parameter inventory
> with recursive MIC discovery, MetaSound one-shot SFX creation, Niagara CustomHlsl text updates,
> CommonUI action widget binding, GAS effect modifier CRUD, CommonUI pause-menu focus/navigation
> scaffolding, looping ambient MetaSound preset references, Axis2D Enhanced Input movement
> mappings, schema-aware DataTable CSV file export, GeometryScript plane-cut/mirror repair,
> GameplayAbility tag/cost/trigger editing, WorldGen balcony/porch/fire-escape/
> ramp/railing StaticMesh generation, Enhanced Input binding removal, GAS cue unlink
> lifecycle validation, Blueprint StringTable bulk edit/remove, UMG animation removal,
> HLOD mesh setup-layer creation, procedural building-shell, maze, and barricade StaticMesh
> generation, IK Retargeter explicit chain mapping/settings read-back, PoseSearch Schema channel
> configuration read-back, LevelInstance Blueprint prefab placement, collection force-delete read-back,
> MetaSound synth-tone authoring, interactive MetaSound crossfade authoring, BlendSpace
> interpolation editing, AnimMontage blend/slot editing, IKRig solver-stack editing,
> Axis3D Enhanced Input mapping, editor map actor-default authoring/read-back,
> deterministic GeometryScript mesh fragment StaticMesh save/read-back, MetaSound Patch I/O
> authoring/read-back, PostProcess emissive Material graph authoring/read-back, MetaSound
> spec-driven node add/layout/remove editing, AnimSequence curve/sync-marker rename-remove cleanup,
> Niagara System EffectType assignment read-back, GeometryScript mesh quality repair, DataTable
> schema-only read-back, project index search/details/text export verification, UMG box-shadow
> MID authoring, DataTable dry-run versus strict apply validation, LogicDriver State Machine
> Blueprint graph authoring, StringTable metadata removal, Enhanced Input duplicate-key conflict validation,
> Mass Entity Config trait add/remove validation, UMG settings-panel/notification-toast
> template generation and read-back, AnimSequence pose-frame data authoring, AnimSequence
> pose-copy editing, AnimMontage section flow/time/delete editing, BlendSpace sample deletion,
> AnimSequence bone-track lifecycle edits, notify batch/track/clone workflows, StringTable
> registry-list read-back, Content Browser collection name validation/unique-name generation,
> AnimSequence modifier stack persistence/read-back, Material Texture2D preview/contact-sheet/tiling
> diagnostics, MetaSound explicit node connect/disconnect read-back, StaticMesh material-slot/compare read-back, and bulk CDO schema dry-run/apply
> validation for raw UObject assets.
> The generated AssetType directory layer mirrors the `asset_authoring` rows into
> `AssetEditing\[AssetType]` folders, each with a type README, type-scoped tasks stream, and
> per-edit-domain JSON case files. The generated `testsets` tree routes the flat suite by category, asset domain/edit domain,
> Blueprint type/edit domain, explicit workflow, asset operation role, asset lifecycle phase, and
> operation semantics. Asset-operation routing is additionally materialized as
> `asset_operation.<role>.<domain>[.<edit_domain>]` modules, so creation/import, save,
> edit, and read-back slices can be loaded and applied by asset type and edit domain.
> `index.json` carries the loadable routing tree roots and module-to-shard lookup,
> `modules.json` records the split-shard manifest, and `testsets/module_shards/` stores the
> per-route module payloads. Module-backed `select` and `run` load only the requested shard(s)
> plus parent-chain shards, while `tasks.jsonl` stays the canonical task payload stream.
> Blueprint AttributeSet asset authoring remains
> gated by the BlueprintAttributes/GBA plugin, and ComboGraph graph authoring remains
> plugin-gated in this workspace; the generated LogicDriver state-machine row still requires
> LogicDriver availability for live execution. BehaviorTree template export, EQS template creation, GameplayAbility template build,
> and Interchange skeletal import are excluded until their live handlers or portable fixture
> contracts produce stable saved assets under the benchmark root.
>
> **v5.2 (2026-06-24) — high-ROI asset-authoring expansion:** expanded coverage to
> Texture file import metadata, Font, StringTable CSV import, Material Functions, StaticMesh OBJ
> import/collision, animation core assets, audio routing assets, Niagara Parameter Collections and
> EffectTypes, EQS, Chooser Tables, and GAS GameplayAbility/Cue/TargetActor assets, while keeping the benchmark
> project-content portable. MetaSound Source, StateTree creation, Smart Object Definition,
> GeometryScript parametric mesh candidates, Interchange skeletal import, full retarget
> batches, ControlRig authoring, and LevelSequence authoring are outside this representative suite
> until their live action, portable
> benchmark fixture, and cleanup contracts are proven.
>
> **v5.1 (2026-06-18) — adversarial hardening + practical-coverage expansion:** closed the
> scoring holes a reject-everything / read-only / no-op server could exploit and roughly doubled
> executed coverage. (1) **`error_path`** now scores on the **offending identifier** (the unique
> `NONEXISTENT_*`/`INVALID_*` token), not generic English words — a canned "could not find variable"
> no longer passes. (2) **`edit_execute` create tasks are delete-first op_chains** so the read-back
> proves THIS run made the edit (leftover state from a prior run can no longer mask a silent no-op);
> `add_node` reads back the exact returned node id; `set_component_property` reads back the VALUE;
> `add_replicated_variable` asserts the replication flag; CDO writes assert the property+value.
> (3) **`duplicate_reject`** requires a CLEAN first create (delete-reset each run). (4) New executed
> categories: **`negative_compile`** (deliberately break a graph and require the compiler to REPORT
> `error_count>0` — makes "compile is clean" falsifiable), plus data-pin wiring, pin literals, and
> delete round-trips. (5) **Re-weighted** away from trivially-passed schema-fetch (0.19→0.10) toward
> executed work. (6) Fixed three benchmark defects the live baseline surfaced: the `bIsActive`
> fixture var collided with `UActorComponent`'s native `bIsActive` (renamed `bComponentActive`),
> `validate_blueprint`'s lint-report shape was mis-scored as a failed compile, and interface tasks
> passed an unresolvable short name (now the full `/Game` path, backed by an interface-resolver fix).
>
> **v5 (2026-06-17):** `edit_execute` became read-back verified; `workflow_completeness` (existence
> only) was replaced by executed `workflow_execute`; suite expanded to 295 tasks with `preflight`.

| Category | Count | Tool | Pass Criterion |
|----------|------:|------|----------------|
| `type_discovery` | 21 | `project_query` | Valid non-error response with ≥1 result; required-result queries target fixture names and exact fixture paths |
| `graph_read` | 35 | `blueprint_query` | No transport error AND no isError AND content shape / expected-token checks |
| `variable_read` | 28 | `blueprint_query` | No transport error AND no isError AND fixture-variable / setup-created function checks |
| `read_schema` | 23 | `monolith_discover` | Schema has `planning_signals` list and `skill` string (lenient) |
| `edit_schema` | 47 | `monolith_discover` | Schema has `planning_signals` + `skill` AND no isError (strict) |
| `workflow_execute` | 11 | `blueprint_query` | Executed multi-step chains run, compile clean, and read back their end state |
| `edit_execute` | 113 | `blueprint_query` | Edit call succeeds AND its mutation is observable via read-back; creates run delete-first so the read-back proves THIS run; includes UMG, AnimBP, GAS, ActorComponent, Interface, component/property value, exec- and data-pin wiring, pin literals, and delete round-trips |
| `asset_authoring` | 267 | mixed owner namespaces | Cross-domain UE assets are created or routed to asset edit workflows through owning namespaces, then saved where persistable and inspected/read back |
| `error_path` | 20 | `blueprint_query` | Server returns a structured `isError` whose message references the **offending identifier** (transport crash = fail; generic-only error = fail) |
| `duplicate_reject` | 11 | `blueprint_query` | First call CLEANLY creates the entity (delete-reset each run) AND a second identical `add_*` call returns a duplicate-specific `isError` |
| `negative_compile` | 1 | `blueprint_query` | A deliberately broken scratch Blueprint function signature must be REPORTED as a real compile failure (`error_count>0`); a transport/isError/clean envelope = fail |

## Blueprint Types Tested

| Type | Domain | Benchmark Path |
|------|--------|----------------|
| Actor | gameplay | `/Game/Benchmarks/BPB_TestActor` |
| Character | gameplay | `/Game/Benchmarks/BPB_TestCharacter` |
| Widget | ui | `/Game/Benchmarks/WBP_TestWidget` |
| AnimInstance | animation | `/Game/Benchmarks/ABP_TestAnim` |
| GameplayAbility | ability | `/Game/Benchmarks/GA_TestAbility` |
| ActorComponent | component | `/Game/Benchmarks/BC_TestComponent` |
| Interface | interface | `/Game/Benchmarks/BPI_TestInterface` |

## Primary Score (v5.4)

The weights live in the single `WEIGHTS` dict in `Scripts/asset_editing_benchmark.py` and
are the only source of truth (consumed by the scorer, the manifest formula, the comparison
report, and the module docstring — they cannot drift apart). See `METRICS.md` for the full
weights rationale and changelog.

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

## Score Dimensions

| Metric | Weight | Direction | Scoring Policy |
|--------|--------|-----------|----------------|
| `edit_execute_rate` | 0.27 | higher is better | Strict + delete-first read-back. Creates run a leading remove so the read-back proves THIS run made the edit; `add_node` reads back the returned node id; `set_component_property` reads back the value; node-wiring chains execute `connect_pins` (exec AND data pins); compile slots require `error_count == 0`. A non-editing stub scores 0. |
| `asset_authoring_rate` | 0.10 | higher is better | Strict. Cross-domain UE asset chains must create/edit/save through owner namespaces and prove results through read-back/inspection. |
| `graph_read_rate` | 0.11 | higher is better | Strict. isError from server fails (asset-not-found is a real signal). |
| `variable_read_rate` | 0.11 | higher is better | Strict. Same policy as graph_read; Interface fixture asserts its function stubs. |
| `error_path_rate` | 0.09 | higher is better | Inverted + input-specific. The isError message must reference the **offending identifier**; a generic always-error server fails. Covers nonexistent inputs AND real precondition/value failures (create-on-existing, bad type/parent/node_type tokens). |
| `workflow_execute_rate` | 0.09 | higher is better | Strict. An executed multi-step chain must run, compile clean, and read back its end state. Replaces existence-only `workflow_completeness`. |
| `edit_schema_rate` | 0.07 | higher is better | Strict. isError on schema fetch fails the task. |
| `duplicate_reject_rate` | 0.06 | higher is better | Inverted. First call must CLEANLY create (delete-reset each run); the second identical call must return a duplicate-specific isError. Catches silent-suffix/no-op handling that `edit_execute` cannot see. |
| `negative_compile_rate` | 0.05 | higher is better | A deliberately broken scratch Blueprint function signature must be REPORTED as a real compile failure (`error_count>0`). Makes "compile is clean" falsifiable. |
| `type_discovery_rate` | 0.03 | higher is better | Valid non-error project search with ≥1 result; required-result queries target the benchmark's own fixtures (portable across projects). |
| `read_schema_rate` | 0.02 | higher is better | Lenient. Only checks `planning_signals` and `skill` metadata fields. |

## Workflows Executed (`workflow_execute`)

Each workflow is actually **run** step-by-step against a fixture (not just checked for catalog
presence), with `add_node` ids threaded into `connect_pins` and a final read-back.

| Workflow | Executed Steps | Read-back |
|----------|----------------|-----------|
| `build_function` | `add_function` → two `add_node` (PrintString) in the function graph → `connect_pins` (then→execute) → `compile_blueprint` | function present in `get_functions` AND the connection exists |
| `implement_interface` | `implement_interface` (BPI_TestInterface) → `compile_blueprint` | interface present in `get_interfaces` |
| `component_assembly` | `add_component` (SphereComponent) → `set_component_property` (SphereRadius) → `compile_blueprint` | component present in `get_components` |
| `widget_style_refresh` | `add_function` → `set_function_params` → `add_node` (FormatText) → `compile_blueprint` | signature contains style input |
| `widget_comment_layout` | `add_comment_node` → `auto_layout` → `compile_blueprint` | graph contains comment text |
| `anim_threadsafe_graph` | `add_function` → `set_function_thread_safe` → `add_node` (Speed get) → `compile_blueprint` | function present |
| `gas_activate_override` | `override_parent_function` (K2_ActivateAbility) → `add_node` → `compile_blueprint` | override present |
| `actorcomponent_toggle_contract` | `add_function` → `set_function_params` → `add_node` (bIsActive set) → `compile_blueprint` | signature contains input |
| `interface_signature_contract` | `add_function` → `set_function_params` → `compile_blueprint` | interface signature contains output |
| `actor_component_property_assembly` | `add_component` (PointLightComponent) → `set_component_property` (Intensity) → `compile_blueprint` | component present |
| `duplicate_function_graph` | `add_function` → `duplicate_graph` → `compile_blueprint` | duplicated graph present |

## Fixture Lifecycle

```powershell
python Scripts\asset_editing_benchmark.py preflight `
  --mcp-url http://localhost:9316/mcp

python Scripts\asset_editing_benchmark.py setup_fixtures `
  --mcp-url http://localhost:9316/mcp
```

`preflight` first checks `monolith_status`. If the live MCP endpoint is absent, the result is
`failure_kind=transport_error`. If the endpoint is alive but fixture assets or setup-created
variables/functions are missing, the result is `fixture_missing_or_invalid` or
`fixture_contract_missing`. `run` performs this readiness preflight by default; use
`--skip-preflight` only for compatibility testing of old behavior.

## Generate

```powershell
python Scripts\asset_editing_benchmark.py generate `
  --tasks Benchmarks\AssetEditing\tasks.jsonl `
  --manifest Benchmarks\AssetEditing\manifest.json `
  --testsets Benchmarks\AssetEditing\testsets
```

`generate` keeps `tasks.jsonl` as the canonical full-suite stream and emits
`asset_types.json`, `AssetEditing\[AssetType]\...` lookup folders, `testsets/index.json`,
`testsets/modules.json`, and `testsets/module_shards/` for routable subsets. The module manifest
advertises 1809 generated modules across 38 shard files, so a routed subset can load a small module
payload without parsing the full route tree.

## Select Test Sets

```powershell
python Scripts\asset_editing_benchmark.py select `
  --testset-module asset_authoring.audio

python Scripts\asset_editing_benchmark.py select `
  --testset-module asset_authoring.asset.hygiene

python Scripts\asset_editing_benchmark.py select `
  --testset-module blueprint.Actor.node

python Scripts\asset_editing_benchmark.py select `
  --testset-module asset_authoring.material.batch_layout_replace

python Scripts\asset_editing_benchmark.py select `
  --testset-module asset_authoring.material.layer_assets

python Scripts\asset_editing_benchmark.py select `
  --testset-module workflow.audio_sound_perception_binding_roundtrip

python Scripts\asset_editing_benchmark.py select `
  --testset-module lifecycle.create_save

python Scripts\asset_editing_benchmark.py select `
  --testset-module lifecycle.create

python Scripts\asset_editing_benchmark.py select `
  --testset-module asset_operation.edit

python Scripts\asset_editing_benchmark.py select `
  --testset-module asset_operation.edit.ui.widget_templates

python Scripts\asset_editing_benchmark.py select `
  --testset-module asset_operation.creation_or_import.ui `
  --testset-module asset_operation.save.ui `
  --module-mode intersection

python Scripts\asset_editing_benchmark.py select `
  --testset-module asset_authoring.blueprint.timeline_persistence

python Scripts\asset_editing_benchmark.py select `
  --testset-module asset_authoring.material.graph_clear_import

python Scripts\asset_editing_benchmark.py select `
  --testset-module asset_authoring.ui.widget_spec_builder

python Scripts\asset_editing_benchmark.py select `
  --testset-module asset_authoring.ui.commonui_style_assets

python Scripts\asset_editing_benchmark.py select `
  --testset-module asset_authoring.ui.commonui_container_navigation

python Scripts\asset_editing_benchmark.py select `
  --testset-module asset_authoring.animation.blendspace_sample_edit

python Scripts\asset_editing_benchmark.py select `
  --testset-module asset_authoring.animation.blendspace_interpolation

python Scripts\asset_editing_benchmark.py select `
  --testset-module asset_authoring.animation.montage_blend_slot

python Scripts\asset_editing_benchmark.py select `
  --testset-module asset_authoring.animation.ikrig_solver_stack

python Scripts\asset_editing_benchmark.py select `
  --testset-module asset_authoring.input.axis3d_mapping

python Scripts\asset_editing_benchmark.py select `
  --testset-module asset_authoring.interchange.scene_import

python Scripts\asset_editing_benchmark.py select `
  --testset-module asset_authoring.data.asset_property_path

python Scripts\asset_editing_benchmark.py select `
  --testset-module asset_authoring.animation.anim_blueprint_motion_matching

python Scripts\asset_editing_benchmark.py select `
  --testset-module asset_authoring.niagara.emitter_topology

python Scripts\asset_editing_benchmark.py select `
  --testset-module asset_authoring.mesh.merge_actors

python Scripts\asset_editing_benchmark.py select `
  --testset-module asset_authoring.worldgen.balcony_static_mesh

python Scripts\asset_editing_benchmark.py select `
  --testset-module asset_operation.edit.worldgen.railing_path_static_mesh

python Scripts\asset_editing_benchmark.py select `
  --testset-module asset_authoring.input.enhanced_input_mapping_removal

python Scripts\asset_editing_benchmark.py select `
  --testset-module asset_operation.edit.gas.gameplay_cue_unlink_lifecycle

python Scripts\asset_editing_benchmark.py select `
  --testset-module asset_authoring.localization.stringtable_blueprint_bulk

python Scripts\asset_editing_benchmark.py select `
  --testset-module asset_operation.edit.ui.widget_animation_remove

python Scripts\asset_editing_benchmark.py select `
  --testset-module asset_authoring.hlod.mesh_setup_layer

python Scripts\asset_editing_benchmark.py select `
  --testset-module asset_operation.edit.mesh.building_shell_static_mesh

python Scripts\asset_editing_benchmark.py select `
  --testset-module asset_authoring.level_instance.prefab_placement

python Scripts\asset_editing_benchmark.py select `
  --testset-module asset_authoring.collection.delete_readback

python Scripts\asset_editing_benchmark.py select `
  --testset-module asset_operation.edit.audio.metasound_synth_tone

python Scripts\asset_editing_benchmark.py select `
  --testset-module asset_operation.edit.audio.metasound_interactive_crossfade

python Scripts\asset_editing_benchmark.py select `
  --testset-module lifecycle.not_applicable

python Scripts\asset_editing_benchmark.py select `
  --testset-module asset_authoring.audio `
  --testset-module lifecycle.create_save `
  --module-mode intersection

python Scripts\asset_editing_benchmark.py select `
  --testset-module operation_semantics.create_save

python Scripts\asset_editing_benchmark.py list_testsets `
  --track asset_operation_edit_domain `
  --module-prefix asset_operation.edit.ui
```

Use `--domain`, `--edit-domain`, `--blueprint-type`, `--workflow`, `--lifecycle-phase`, or
`--asset-operation`, `--operation-semantics` / `--task-id` to apply additional intersections. Repeated
`--testset-module` values default to `--module-mode union`; pass `--module-mode intersection`
when the intended route is a tree intersection such as `asset_authoring.audio` and
`lifecycle.create_save`. Pass `--allow-empty` only for selection audits that intentionally prove a
route intersection is empty; `run` still rejects empty task selections. Subset summaries include `task_selection`
metadata, the resolved module routes, parent chains, loaded shard ids, source task SHA, and
`score_comparable_to_full_run=false`.

Legacy duplicate-prefix route selector inputs are normalized before lookup, and generated docs expose
only the compact canonical routes such as `asset_authoring.asset.batch_delete` and
`asset_operation.edit.asset.batch_delete`.
Canonical route module IDs and generated test case files must use the compact leaf form; repeated
AssetType tokens are accepted only as legacy selector input and normalize to the compact route.

Most `asset_authoring` rows are `create_save`, but lifecycle routing is explicit rather than inferred:
the dynamic Content Browser collection query row and blank UWorld map row are `create` only and are
selected by `lifecycle.create` or `--lifecycle-phase create`, not by `lifecycle.create_save`.

## Run

```powershell
python Scripts\asset_editing_benchmark.py run `
  --mcp-url http://localhost:9316/mcp `
  --tasks Benchmarks\AssetEditing\tasks.jsonl `
  --label current `
  --output-dir Saved\Monolith\Benchmarks\AssetEditing\current `
  --jobs 4
```

To run one routed subset against MCP, pass the same selector flags, for example
`--testset-module asset_authoring.audio.sound_concurrency`.

## Compare

```powershell
python Scripts\asset_editing_benchmark.py compare `
  --baseline Saved\Monolith\Benchmarks\AssetEditing\baseline\summary.json `
  --current  Saved\Monolith\Benchmarks\AssetEditing\current\summary.json `
  --output-dir Saved\Monolith\Benchmarks\AssetEditing\compare
```

## See Also

- [Benchmarks README](../README.md)
