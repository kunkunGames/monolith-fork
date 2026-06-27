# AssetEditing Benchmark Results

## v5.4 (2026-06-26) — AssetEditing rename + UE 5.8 high-ROI Monolith asset-action expansion

The current generated static suite has **552 tasks across 11 categories**. The new work expands
`asset_authoring` to **242** asset creation/edit/save/read-back chains for high-ROI UE asset types beyond
Blueprint graph edits: Texture file import metadata, ImageGen Texture2D/MSDF provenance assets,
Interchange texture/audio and StaticMesh import/reimport/export, Interchange batch import, ModelGen
StaticMesh provenance import, Font, StringTable CSV import, StaticMesh OBJ import/collision/LOD
screen-size metadata, StaticMesh FBX export, PBR material creation from disk textures, Material
Function, Material Function Instance overrides, Material Instance duplicate/reparent/clear flows,
animation core assets, BlendSpace sample bake, AimOffset axis editing, IK Rig/Retargeter metadata and retarget pose/root settings,
audio routing/template/spec assets, Niagara sidecars, EQS, Chooser Tables, GAS
Ability/Cue/TargetActor assets, Material graph build/export, CommonUI text-block authoring,
AnimMontage sections/slots, AnimSequence curve/sync metadata, Niagara HLSL module stack insertion,
BehaviorTree spec-build, BehaviorTree Task/Decorator/Service Blueprint assets, GAS Enhanced Input
binding smoke, parameterized Material Instance overrides, CommonUI input-action DataTables,
GameplayEffect template duplication, AIController perception config, HLODLayer config,
static/dynamic Content Browser collections, UWidgetAnimation v2 tracks, external generated-image
import, and WorldGen blockout-volume Blueprint setup. Follow-up coverage adds Blueprint spec
builder, DataTable bulk import/export, Blackboard inheritance/direct parent assignment,
GameplayEffect declarative builders, Chooser result asset-reference rewrite, SoundWave property
batch edits, SoundClass/SoundMix ducking, Submix hierarchy, weighted-random and distance-crossfade
SoundCue templates, and Niagara renderer/SubUV, user parameter, sim-stage/lifetime, duplicate/diff,
and event-handler metadata workflows, PoseSearch schema/database entry authoring, Material Custom
HLSL node create/update/export read-back, layered/looping/switch SoundCue template operations,
Content Browser collection member removal, CurveTable row maintenance, batch SoundWave routing,
StringTable export/remove, SVG source validation/import, Niagara Mesh/Ribbon renderer setup,
Niagara module duplicate GUID capture, ModelGen local job submit/read/download/import,
seed_data_asset creation, StateTree compile/read-back, SmartObjectDefinition slot authoring,
BlendSpace1D/AimOffset1D axis editing, AnimComposite/MirrorDataTable authoring,
GeometryScript handle-save, parametric StaticMesh generation, advanced UMG animation/event
authoring, Chooser tree duplicate/remap, asset naming/fuzzy-find hygiene, SoundConcurrency batch
assignment, SoundWave compression/rename batch maintenance, Material expression CRUD, Material
batch/layout/replace maintenance, SoundCue primitive graph-edit workflows, Interchange generic
option import, SoundWave looping/virtualization batches, Niagara EffectType scalability, AnimSequence
root-motion/additive duplication, GeometryScript direct mesh operations, Sound perception binding,
Material Function metadata/delete-expression maintenance, MIC single-parameter plus batch expression
delete, Material graph clear/import, UISpec build/dump Widget Blueprint workflows,
Blueprint FunctionLibrary/MacroLibrary/Interface asset-type authoring, PoseSearch NormalizationSet
assignment, Chooser context class retargeting, persisted StaticMesh auto-LOD generation, Blueprint
duplicate/reparent and timeline persistence, UMG slot/property/widget-variable edits, Interchange
batch reimport, Material texture import settings, Material Function parameter-group rename, and
AnimBlueprint state-machine authoring, Motion Matching AnimGraph wiring, Niagara emitter
duplicate/rename topology, procedural pipe-network StaticMesh generation, and generic Interchange
bitmap import, CommonUI styled-button class-as-data styling, blank UWorld map creation,
dialog-free Blueprint prefab harvesting, sound-perception unbind cleanup, Niagara HLSL function
script creation, Niagara emitter template/spec create-import flows, Niagara emitter stack
clearing, montage-from-section-spec authoring, project saved-asset state/AssetRegistry disk
read-back, scoped editor dirty-package save/read-back, saved UMG template Widget Blueprint
generation for menu, dialog, loading, inventory, save-slot, and HUD layouts, native GAS
AttributeSet initialization DataTable authoring, ASC replication-mode editing, AnimNotify point
CRUD, AnimNotifyState timing/duration/property editing, direct BehaviorTree node/decorator/service/property edits, Niagara emitter property aliases,
Niagara renderer property/binding edits, nav-harness map creation, proxy StaticMesh generation
from spawned scene actors, public actor-merge StaticMesh generation, CommonUI text/border style asset creation and application,
CommonUI activatable container/navigation/tab-list authoring, BlendSpace sample edit read-back,
Interchange typed scene import, and path-based raw UObject asset property editing through
`set_property_at_path`, audio namespace delete/read-gone workflows, GAS Cue-to-Effect linking,
EQS test reorder/remove/duplicate mutation, SmartObject behavior-definition duplication,
Niagara renderer material assignment, Niagara dynamic input add/value/remove read-back,
Niagara curve Data Interface key read-back, Material Layer/Blend asset metadata editing,
batch asset inspect/delete lifecycle validation, MetaSound Source graph authoring,
Material decal-domain setup, UMG ListView entry binding and widget-tree maintenance,
Niagara module stack order/enable-state editing, AnimSequence bone-track key authoring,
GeometryScript handle boolean/remesh/material-id editing, procedural structure and terrain
StaticMesh generation, generated world-tile Texture2D validation, Texture2D role preset validation,
Material preview rendering, GAS ability flag and widget attribute-binding edits, WorldGen street/roof/building
StaticMesh generation, GeometryScript simplify/UV read-back, MetaSound variable state editing,
Niagara user-parameter removal, Material parameter inventory with recursive MIC discovery,
MetaSound one-shot SFX creation, Niagara CustomHlsl text updates, CommonUI action widget binding,
GAS effect modifier CRUD, CommonUI pause-menu focus/navigation scaffolding, looping ambient
MetaSound preset references, Axis2D Enhanced Input movement mappings, schema-aware DataTable CSV
file export, GeometryScript plane-cut/mirror repair, GameplayAbility tag/cost/trigger editing,
WorldGen balcony/porch/fire-escape/ramp/railing StaticMesh generation, Enhanced Input binding
removal, GAS cue unlink lifecycle validation, Blueprint StringTable bulk edit/remove, UMG
animation removal, HLOD mesh setup-layer creation, procedural building-shell, maze, and
barricade StaticMesh generation, LevelInstance Blueprint prefab placement, collection
force-delete read-back, MetaSound synth-tone authoring, interactive MetaSound crossfade authoring,
BlendSpace interpolation editing, AnimMontage blend/slot editing, IKRig solver-stack editing,
Axis3D Enhanced Input mapping, editor map actor-default authoring/read-back, deterministic
GeometryScript mesh fragment StaticMesh save/read-back, MetaSound Patch I/O authoring/read-back,
PostProcess emissive Material graph authoring/read-back, MetaSound spec-driven node
add/layout/remove editing, AnimSequence curve/sync-marker rename-remove cleanup, Niagara
System EffectType assignment read-back, GeometryScript mesh quality repair, DataTable
schema-only read-back, project index search/details/text export verification, declarative
GameplayAbility and EQS spec builders, Niagara curve Data Interface key editing,
Niagara Parameter Collection parameter removal, Niagara system timing field editing, UMG
box-shadow MID authoring, DataTable dry-run versus strict apply validation, AnimSequence
pose-frame data authoring, StringTable metadata
removal, LogicDriver State Machine Blueprint graph authoring, Enhanced Input duplicate-key
conflict validation, Mass Entity Config trait add/remove validation, and UMG settings-panel plus
notification-toast template generation/read-back.
The generated `asset_types.json` plus `AssetEditing\[AssetType]` directories now expose
human-browsable AssetType support, type-scoped task streams, and per-edit-domain JSON case files.
The generated `testsets/index.json`, `testsets/modules.json`, and `testsets/module_shards/` files expose routable modules
for category, asset domain/edit domain, Blueprint type/edit domain, explicit workflow,
asset-operation, lifecycle, and operation-semantics phase. They also store tree roots, module refs,
parent/child links, and module-to-shard lookup so routed subsets can be loaded without re-deriving hierarchy. Asset
operations are now split into role/domain/edit-domain leaves, so subsets can be applied through
modules such as `asset_authoring.audio.sound_perception_binding`,
`workflow.audio_sound_perception_binding_roundtrip`, `asset_operation.edit`,
`asset_operation.edit.ui.widget_templates`, `asset_operation.save.ui`,
`asset_authoring.asset.batch_delete`,
`lifecycle.create_save`, `operation_semantics.create_save`,
or the matching selectors.
Blueprint AttributeSet asset authoring remains gated by the BlueprintAttributes/GBA plugin, and
ComboGraph graph authoring remains plugin-gated in this workspace; the generated LogicDriver
state-machine row still requires LogicDriver availability for live execution. Interchange skeletal import, full retarget
batches, ControlRig authoring, LevelSequence authoring, BehaviorTree template export, EQS template
creation, and GameplayAbility template build remain outside this representative generated suite
until their live action, portable benchmark fixture, and cleanup contracts are proven.

### v5.4 static addendum

| Field | Value |
|-------|-------|
| Generated tasks | 552 |
| Generated `asset_authoring` tasks | 242 |
| Static generation | `python Plugins\Monolith\Scripts\asset_editing_benchmark.py generate` |
| Generated AssetType slices | 23 directories under `Benchmarks\AssetEditing\[AssetType]`, plus `asset_types.json`, each with type README, scoped `tasks.jsonl`, `index.json`, and `testcases\*.json` files |
| Generated test-set modules | 1659 modules split across 38 shard files in `Benchmarks\AssetEditing\testsets\module_shards`, including 92 `asset_operation_domain` modules and 966 `asset_operation_edit_domain` leaves |
| Latest static addendum | Added `BEB-512` UMG box-shadow MID authoring, `BEB-513` native UMG `set_image`/`set_brush`/`set_font` styling read-back, `BEB-514` DataTable dry-run/strict-apply validation, `BEB-515` LogicDriver State Machine graph authoring, `BEB-516` StringTable metadata removal, `BEB-517` Enhanced Input duplicate-key conflict validation, `BEB-518` Mass Entity Config trait add/remove validation, `BEB-519` UMG settings-panel/notification-toast template generation, and `BEB-520` AnimSequence pose-frame data authoring. The regenerated current asset-authoring range is `BEB-279..BEB-520`; use `tasks.jsonl`, `asset_types.json`, `testsets/index.json`, and `testsets/module_shards/` as the canonical ID map. Repeated domain prefixes are compacted in route module ids, for example `asset_authoring.asset.batch_delete`, `asset_authoring.asset.texture_role_presets`, and `asset_operation.edit.asset.batch_delete`; legacy duplicate selector inputs are normalized before lookup. |
| Live status | The latest full live scored run is still the 375-task pre-rename historical baseline below; previously appended focused rows passed the listed historical `Saved\Monolith\Benchmarks\AssetEditing\...` smoke runs. The current `BEB-279..BEB-520` AssetEditing authoring range plus the generated workflow/asset-operation/lifecycle/operation-semantics routing tree are statically generated and pending focused live smoke/full live rerun evidence. |

### Historical pre-rename v5.4 live scored run

| Field | Value |
|-------|-------|
| Label | `v5.4-full-jobs4b` |
| Run (UTC) | 2026-06-25T00:33:07Z |
| MCP version | `0.20.3` |
| Engine | `++UE5+Release-5.8-CL-55116800` |
| Project | `GO` |
| Tasks run | 375 |
| `asset_authoring` tasks | 66 |
| Transport errors | 0 |
| `error_count` | 0 |
| `asset_editing_score` | **1.000** |
| Execution | `--jobs 4` (`mode=parallel`) |
| Output | `Saved\Monolith\Benchmarks\AssetEditing\v5.4-full-jobs4b\summary.json` |

All eleven dimensions scored **1.000**: `edit_execute`, `asset_authoring`, `edit_schema`,
`graph_read`, `variable_read`, `error_path`, `workflow_execute`, `duplicate_reject`,
`negative_compile`, `read_schema`, and `type_discovery`. The run verified the then-current UE 5.8
asset-authoring set included in the 375-task full suite, including PBR disk material read-back, MIC
duplicate/reparent/clear, MaterialFunctionInstance override metadata, StaticMesh FBX export,
BlendSpace sample bake, AimOffset axis editing, Interchange batch import, Blueprint spec builder,
DataTable bulk import/export, Blackboard parent assignment, GameplayEffect declarative builders,
and Chooser result asset-reference rewrite.

### v5.3 live scored run

| Field | Value |
|-------|-------|
| Label | `v5.3-full-jobs4f` |
| Run (UTC) | 2026-06-24T20:43:14Z |
| MCP version | `0.20.3` |
| Engine | `++UE5+Release-5.8-CL-55116800` |
| Project | `GO` |
| Tasks run | 362 |
| Transport errors | 0 |
| `error_count` | 0 |
| `asset_editing_score` | **1.000** |
| Output | `Saved\Monolith\Benchmarks\AssetEditing\v5.3-full-jobs4f\summary.json` |

All eleven dimensions scored **1.000**: `edit_execute`, `asset_authoring`, `edit_schema`,
`graph_read`, `variable_read`, `error_path`, `workflow_execute`, `duplicate_reject`,
`negative_compile`, `read_schema`, and `type_discovery`. The run used `--jobs 4` with editor
resource locks, so asset creation/save chains do not race Blueprint/project graph-index work.

Focused live smoke runs for the latest asset-authoring rows also passed on UE 5.8 /
Monolith MCP 0.20.3:

- Seven deep asset-authoring rows passed with `task_count=7`, `error_count=0`, and
`asset_authoring_rate=1.0` at
`Saved\Monolith\Benchmarks\AssetEditing\asset_authoring_deep_focus2d_20260625_0424\summary.json`.
- Six UE5.8 high-ROI rows passed with `task_count=6`, `error_count=0`, and
`asset_authoring_rate=1.0` at
`Saved\Monolith\Benchmarks\AssetEditing\asset_authoring_expansion_focus3_20260625_0447\summary.json`.
- Four additional UE5.8 high-ROI rows passed with `task_count=4`, `error_count=0`, and
`asset_authoring_rate=1.0` at
`Saved\Monolith\Benchmarks\AssetEditing\asset_authoring_expansion_focus4b_20260625_0502\summary.json`.

The v5.1 live 1.000 remains useful historical evidence for the Blueprint graph hardening. The
v5.3 run above is the previous full scored 362-task UE 5.8 baseline; v5.4's latest full scored
baseline is 375 tasks, while the generated suite is now 552 tasks pending a full live rerun for the
post-baseline asset-authoring addendum.

## v5.1 (2026-06-18) — adversarial hardening + practical expansion

The v5.1 changeset hardened the scoring engine against gaming, roughly doubled executed
coverage, re-weighted away from schema-fetch, and fixed three benchmark defects that a live
baseline run surfaced. See `METRICS.md` → v5.1 note for the full list. Task count 295→305,
10 categories (new `negative_compile`).

### Live baseline (pre-hardening), v5 task set

| Field | Value |
|-------|-------|
| Label | `baseline-v5-pre` |
| Run (UTC) | 2026-06-17T23:5x (local 2026-06-18 08:5x KST) |
| MCP version | `0.20.2` |
| Engine | `++UE5+Release-5.7-CL-51494982` |
| Project | `GO` |
| Tasks run | 295 |
| `asset_editing_score` | **0.967** |

Per-dimension: `edit_schema`/`graph_read`/`error_path`/`read_schema`/`type_discovery`/
`duplicate_reject` = 1.000; `edit_execute` 0.942; `variable_read` 0.929; `workflow_execute`
0.909. **All 9 failures were benchmark/contract defects, not server-capability gaps:**

| Failure | Root cause | Fixed in v5.1 by |
|---------|-----------|------------------|
| `get_variables`/`set_variable_defaults` on `bComponentActive` (×2) | `bIsActive` fixture var collided with `UActorComponent`'s native `bIsActive` (silently not created) | renamed fixture var `bComponentActive` |
| `get_interface_functions` (×1) | required param `interface_class` not supplied | pass the interface `/Game` path + resolver fix |
| `implement_interface` workflow (×1) | short name `BPI_TestInterface` unresolvable | full `/Game` path + interface-resolver fix |
| `validate_blueprint` (×4) | lint-report shape (`node_errors`…) text-scanned as a failed compile | `_compile_is_clean` reads the hard-error lists |
| `add_node` FormatText (×1) | read-back token `"Widget {Label}"` not in `get_graph_data` | read back the returned node id (`${self}`) |

A near-1.0 v5 score with these defects shows the **server** is healthy; the score was depressed
by benchmark measurement bugs. v5.1 removes those, then raises the bar so the score reflects
genuine mutation rather than envelope health (NOT comparable to v5 — establish a fresh baseline).

### v5.1 live scored run

| Field | Value |
|-------|-------|
| Label | `v5.1` |
| Run (UTC) | 2026-06-18 |
| MCP version | `0.20.2` (MonolithBlueprint DLL rebuilt with the interface-resolver fix) |
| Engine | `++UE5+Release-5.7-CL-51494982` |
| Project | `GO` |
| Tasks run | 305 |
| Transport errors | 0 |
| `asset_editing_score` | **1.000** |

All ten dimensions scored **1.000**: `edit_execute`, `edit_schema`, `graph_read`, `variable_read`,
`error_path`, `workflow_execute`, `duplicate_reject`, `negative_compile`, `read_schema`,
`type_discovery`. A clean run of the hardened suite on the benchmark's own fixtures: the live editor
performs and read-back-confirms real edits (delete-first), wires exec and data pins, compiles clean,
rejects invalid input by the offending identifier, guards duplicate names, and reports a real compile
error on the deliberately-broken `negative_compile` scratch blueprint. Because the v5.1 hardening
drops the no-edit stub ceiling to ≈0.22, a 1.000 here *means* genuine Blueprint-authoring capability,
not envelope health.

**Comparison vs `baseline-v5-pre` (`compare/comparison.md`):** `asset_editing_score`
0.967 → 1.000 (**+0.033**); `edit_execute` +0.058, `variable_read` +0.071, `workflow_execute`
+0.091; `negative_compile` is new in v5.1. The score *rose* despite the suite getting strictly
harder, because the benchmark/contract defects that depressed the baseline (the `bComponentActive`
fixture collision, the `validate_blueprint` shape mis-score, and unresolvable interface identifiers)
are fixed — the last backed by the Monolith `ResolveInterfaceClass` handler fix.

**Process notes (reproducibility):** the headless `-nullrhi` editor crashes intermittently under
heavy mutation; the scored run is gated on **zero transport errors** (a crash mid-run is rejected and
retried on a fresh boot). `setup_fixtures` now `save_asset`s each fixture so new entities survive a
reboot. Running the suite twice on the SAME editor instance accumulates in-memory test entities; run
once per boot (or reset fixtures) for a clean number — see `Docs/TODO.md` (fixture-growth item).

---

## v5 status (2026-06-17): superseded by v5.1

The v5 upgrade (295 tasks: read-back-verified `edit_execute`, executed `workflow_execute`,
input-specific `error_path`, first-call-gated `duplicate_reject`, expanded schema/read coverage,
fixture-name/path `type_discovery`, and fixture lifecycle preflight) changed what the score
*means*: the previous v4 `asset_editing_score = 1.000` was reachable by a server that
returned well-formed success envelopes without truly performing/confirming edits — exactly the
gap v5 closes. **The v4 result below is therefore not comparable to v5/v5.1.**

The v5 scoring logic was **validated offline** against mock servers (no live editor required):

| Mock server | Result |
|-------------|--------|
| Faithful editor (mutations take effect, reads reflect them, invalid input rejected, duplicate guard present) | Passes every sampled category (`edit_execute`, `workflow_execute`, `error_path`, `duplicate_reject`, …) |
| No-op stub (returns success envelopes but never mutates; reads come back empty) | **Blocked on every read-back-dependent task** — fails `edit_execute`, the wiring `op_chain`, `workflow_execute`, `error_path`, and `duplicate_reject` |

The live scored run is still pending a healthy editor/MCP endpoint. Use `preflight` first:
`transport_error` means there is no usable live MCP response; `fixture_missing_or_invalid` means
the endpoint is alive but a fixture asset is absent/invalid; `fixture_contract_missing` means a
fixture exists but setup-created variables/functions are missing. All response-shape dependencies
the read-back logic relies on are recorded in `METRICS.md` → Catalog Version. **Re-run once
preflight passes** (see Run Command below).

## v4 result (2026-06-16, superseded — not comparable to v5)

| Field | Value |
|-------|-------|
| Label | `current` (v4) |
| Run timestamp (UTC) | 2026-06-16T19:05:13Z |
| MCP version | `0.20.2` |
| Engine | `++UE5+Release-5.7-CL-51494982` |
| Project | `GO` |
| Tasks run | 191 |
| `asset_editing_score` | 1.000 (v4 weights; reachable without true read-back — the v5 defect class) |

## Run Command (v5.4)

```powershell
# 1. Bring the editor / MCP endpoint up first if it is down:
#    D:\P4\game\BatchFiles\RunHeadlessEditor.bat   (wait for localhost:9316 to listen)
# 2. Check endpoint + fixture readiness. This fails fast with transport_error,
#    fixture_missing_or_invalid, or fixture_contract_missing:
python Scripts\asset_editing_benchmark.py preflight --mcp-url http://localhost:9316/mcp
# 3. Create / verify fixtures:
python Scripts\asset_editing_benchmark.py setup_fixtures --mcp-url http://localhost:9316/mcp
# 4. Run the v5.4 benchmark in parallel. run performs readiness preflight by default:
python Scripts\asset_editing_benchmark.py run `
  --mcp-url http://localhost:9316/mcp `
  --tasks Benchmarks\AssetEditing\tasks.jsonl `
  --label v5.4 `
  --output-dir Saved\Monolith\Benchmarks\AssetEditing\v5.4 `
  --jobs 4
```

Expected on a healthy live editor: 552 tasks loaded and all eleven rates at or near 1.0. A near-1.0
here now *means* the server actually performs, wires, compiles-clean, creates/edits/saves common UE
asset types, rejects bad input, and guards duplicates — not merely that it replied without error.
