# Monolith API Reference

**Version:** v0.20.3 · **Last updated:** 2026-07-04

**In-tree action total is approximate: current source contains roughly 2,037 in-tree `RegisterAction` registrations** (public, in-tree only; all active by default, plus 45 experimental town-gen actions that register only when `bEnableProceduralTownGen=true`). The surface is too large and build-flag dependent to track to the unit — **query `monolith_discover()` (its `total_actions` field) for the exact live figure.** The `ui` namespace re-exports 4 GAS UI binding actions as aliases. v0.19.0 adds an LLM C++ authoring ergonomics pack (`source`, 8 actions + `editor.get_build_errors` fix hints), live-PIE introspection + driving and stat-group readout (`editor`), anim-node binding read/write and time-series PIE sampling (`animation`), a Blueprint variable census + contract reconciliation (`blueprint`), and T3D asset-text export (`project`); plus two first-launch fixes (issue #70) and a ~40% smaller `tools/list` manifest. The `console` namespace adds live `IConsoleManager` registry discovery plus EngineSource.db/FTS5 snapshot search. The `monolith_*` meta-tools (`discover`, `status`, `update`, `reindex`, `guide`) plus the `bulk_fill_query` and `describe_query` framework dispatchers round out the MCP tool count. This total EXCLUDES sibling-plugin actions — they ship in their own repos and are never in the public release zip.

The per-namespace numbers in the Table of Contents and body sections below are kept for structure, not precision — they drift with every action added and are no longer maintained to the unit. Treat them as ballpark; the live figure always comes from `monolith_discover()`.

> Auto-generated and hand-curated. Each action is dispatched via HTTP POST to `http://localhost:<port>` with JSON body `{ "namespace": "<ns>", "action": "<action>", "params": { ... } }`, or via the MCP `tools/list` surface that AI clients see at session start.
>
> For the most current param schemas, call `monolith_discover({ "namespace": "<namespace>", "action": "<action>", "mode": "schema" })` at runtime — it returns live schemas straight out of the plugin. This document is a curated reference, not a source-of-truth substitute.
>
> **0.15.0:** the namespace counts in the Table of Contents and the per-namespace body sections below are a curated in-tree reference. The 2026-05-26 live Go snapshot is 1584 actions / 45 namespaces with sibling/private plugins loaded. For the exhaustive live param schema of any action, call `monolith_discover({ "namespace": "<namespace>", "action": "<action>", "mode": "schema" })` or `describe_query("action_schema", ...)`.
>
> **Failure envelope (2026-07-04, `UMonolithSettings::bCompactErrorEnvelope=true` default):** error results carry exactly one machine-readable copy of `related_actions`/`hints`/`error_data`. With `bEnableStructuredToolResults=true` that copy lives in `structuredContent` and `content[0].text` is a one-line `"<message>; see structuredContent."` pointer; without structured results the copy stays in the top-level fields and `content[0].text` keeps the full error text. `error_data` fields are no longer flattened into the result's top level — read `error_data.<field>` instead of `<field>`. Set `bCompactErrorEnvelope=false` to reproduce the legacy duplicated/flattened shape.

---

## Table of Contents

| Namespace | Actions | Description |
|-----------|---------|-------------|
| [monolith](#monolith) | 5 | Core server tools (discover, status, update, reindex, guide) |
| [blueprint](#blueprint) | ~120 | Blueprint read/write, variable/component/graph CRUD, graph export/clone/remap, node ops, compile, auto-layout, spawn actors, dataset read/edit pack (DataTable/CurveTable/StringTable + `seed_data_asset`), cross-class property access, parent-function overrides |
| [material](#material) | 49 | Material graph editing, inspection, CRUD, material functions, PBR pipeline |
| [animation](#animation) | 145 | Curves, bone tracks, sync markers, root motion, compression, blend spaces (incl. baking + interpolation control), ABPs (incl. an AnimGraph-authoring pack — additive/slot/cached-pose/blend (by int + by enum)/sync/layered-blend/Control Rig/linked-layer/conduit nodes + output wiring — custom anim-graph nodes + state-machine teardown + compound expression transition rules), montages, skeletons, PoseSearch, IKRig, Control Rig |
| [cloth](#cloth) | 2 | Optional read-only Chaos Cloth/Outfit workflow discovery and asset listing |
| [niagara](#niagara) | 119 | Niagara VFX (emitters, modules, params, renderers, HLSL, dynamic inputs, event handlers, sim stages, effect types, event-aware summaries + validate_system event-chain reasoning, temporal-control composite writers + read aggregators, stateless-emitter factory) |
| [editor](#editor) | 33 | Live Coding builds, compile output capture, editor logs, scene capture, texture import, map/world-settings authoring, DataValidation, changelist validation planning, module status, automation test list/run, Python escape-hatch, persistent-level swap |
| [build](#build) | 2 | Unreal engine root resolution and guarded UAT BuildCookRun command construction |
| [artifact](#artifact) | 2 | Build-output manifests and screenshot evidence mirroring |
| [notify](#notify) | 1 | Redacted screenshot-evidence notification payloads |
| [config](#config) | 11 | INI config inspection and search |
| [console](#console) | 15 | Live console object registry, EngineSource.db snapshot refresh, FTS5 search, exact lookup, health, command resolution/execution, log expectations/waits, sequences with artifacts, capture, failure diagnosis, scoped CVar runs |
| [project](#project) | 7 | Project-wide asset index (SQLite + FTS5) |
| [collection](#collection) | 13 | Content Browser collection CRUD and manipulation |
| [source](#source) | 12 | Unreal Engine C++ source code navigation |
| [hlod](#hlod) | | World Partition Hierarchical Level of Detail (HLOD) configuration and legacy migration |
| [mesh](#mesh) | 194 | Mesh inspection, scene manipulation, spatial queries, blockout, GeometryScript, procedural geo, lighting, audio, performance, mesh import (incl. skeletal + animation). +45 town gen registers only with `bEnableProceduralTownGen=true` (experimental, not in the public count) |
| [ui](#ui) | live | UMG widget CRUD, explicit session/request work context, templates, styling, animation v1+v2 plus read inspection, EffectSurface, UIExtension points, CommonFramework diagnostics/authoring, Spec Builder, Type Registry, settings scaffolding, headline scaffolders, navigation/conversion gap-closure, accessibility, CommonUI, GAS UI bindings |
| [gas](#gas) | 142 | Gameplay Ability System: abilities, attributes, effects, ASC, tags, cues, targeting, input, DataAsset profile inspection/writes, runtime probes, scaffold |
| [input](#input) | 10 | Enhanced Input asset authoring for UInputAction and UInputMappingContext assets |
| [chaos_fracture](#chaos_fracture) | 3 | Optional Geometry Collection and Fracture module visibility and asset/component listing |
| [pcg](#pcg) | 14 | Typed PCG graph discovery, authoring, validation, and guarded reference migration |
| [dataflow](#dataflow) | 8 | Optional read-only Dataflow/Chaos graph discovery, bounded graph/node-schema reads, and validation (graph readers under `WITH_MONOLITH_DATAFLOW`) |
| [source_control](#source_control) | 11 | Unreal SourceControl-provider status, file prepare/delete/revert operations, and Perforce opened/path mapping |
| [water](#water) | 2 | Optional Water/Landscape discovery and actor/component listing |
| [world_conditions](#world_conditions) | 4 | Optional WorldConditions query and condition-type inspection |
| [gamefeatures](#gamefeatures) | 15 | Optional GameFeature plugin discovery, inspection, validation, and guarded instanced-action authoring |
| [lyra](#lyra) | 20 | Reflection-based Lyra Experience, UserFacingExperience, map reachability, GamePhase, PawnData, inventory, equipment, weapon, team, and cosmetic semantic diagnostics plus guarded writes |
| [online](#online) | 6 | EOS/OSSv2/CommonSession/CommonUser readiness diagnostics with credential redaction |
| [modular](#modular) | 4 | ModularGameplay receiver lifecycle, AddComponentRequest target, and static extension-event source diagnostics |
| [settings](#settings) | 6 | GameSettings registry/screen/setting, dynamic path, visual-data, and player-mappable input diagnostics |
| [loading](#loading) | 4 | CommonLoadingScreen manager reason, processor candidate, settings/CVar, and optional Lyra handoff diagnostics |
| [interchange](#interchange) | | Optional Interchange framework discovery, validation, and asset import/export operations |
| [ndisplay](#ndisplay) | | Optional nDisplay/DisplayCluster config discovery and validation |
| [combograph](#combograph) | 13 | ComboGraph melee combo authoring (conditional on `WITH_COMBOGRAPH`) |
| [ai](#ai) | 243 | Behavior Trees, State Trees, EQS, Blackboards, AI Controllers, Perception, Smart Objects, Navigation, Mass, Zone Graph, runtime PIE inspection, scaffolds |
| [logicdriver](#logicdriver) | 66 | Logic Driver Pro state machines: graph CRUD, runtime PIE control, scaffolds, dialogue (conditional on `WITH_LOGICDRIVER`) |
| [asset](#asset) | 18 | Asset lifecycle, hygiene, inspection, enrichment, and guarded package graph copy/remap |
| [audio](#audio) | 98 | Sound Cue + MetaSound graph CRUD + document introspection, attenuation/class/mix/submix/concurrency, batch ops, Sound Cue templates, perception bindings |
| [level_sequence](#level_sequence) | 8 | Level Sequence inspection: binding inventory (legacy + UE 5.7 custom bindings), Director Blueprint functions/variables, event-track bindings, cross-sequence reverse lookup |
| [bulk_fill](#bulk_fill) | 2 | Reflection-walker bulk property fill across 12 per-namespace adapters (`apply`, `list_namespaces`) |
| [describe](#describe) | 3 | Read-only schema introspection for the same 12 adapters (`schema`, `list_targets`, `action_schema`) |
| [decision](#decision) | 5 | **New v0.17.0.** Reflection Intelligence — architectural decision records mined from markdown corpora |
| [risk](#risk) | 5 | **New v0.17.0.** Reflection Intelligence — git-churn / co-change / hotspot signals + conditional-gate inventory |
| [cppreflect](#cppreflect) | 6 | **New v0.17.0.** Reflection Intelligence — UE 5.7 UHT reflection-edge queries (UCLASS / UPROPERTY / UFUNCTION / UINTERFACE + cpp↔asset edges + specifier discovery) |
| [network](#network) | 4 | **New v0.17.0.** Reflection Intelligence — UE 5.7 replication inspection (replicated classes, RPCs, OnRep handlers, unbalanced-OnRep audit) |
| [pipeline](#pipeline) | 2 | **New v0.17.0.** Reflection Intelligence — read-only composer actions (`pr_review`, `release_readiness`) |
| [slate](#slate) | 6 | Slate inspector status, window listing, widget snapshot/describe/capture, type- or text-based widget wait (gated by `bEnableSlateInspectorActions`) |
| [imagegen](#imagegen) | 10 | Generated Texture2D/PNG/SVG/MSDF image workflows and ima2 bridge |
| [bridge](#bridge) | 5 | Read-only RX-6 bridge integration |
| [reflect](#reflect) | 1 | **New [Unreleased].** Reflection Intelligence — index maintenance (`rebuild_reflection_index`, project-only force-rebuild of the RI reflection tables; WRITE/maintenance) |
| **Curated in-tree subtotal** | **v0.17.x public surface** | Static public/in-tree reference from the curated body below; fully loaded local projects may report additional sibling/private plugin actions. |
| [Sibling plugins](#sibling-plugins) | varies | Separate plugins, separate distribution |

---

## Recent API Changes (v0.14.0 → v0.14.7)

The Phase J retrofit cycle added five new actions and tightened param validation on several others. If you wrote integration code against v0.13.x or earlier, scan this list before upgrading.

| Action | Change | Reason |
|--------|--------|--------|
| `editor.create_empty_map` | **NEW** (Phase J F8) | Test scaffolding needed a blank UWorld factory that doesn't depend on engine templates. |
| `editor.get_module_status` | **NEW** (Phase J F8) | Lets clients query plugin/module load state without grepping logs. Wraps `IPluginManager` + `FModuleManager`. |
| `gas.grant_ability_to_pawn` | **NEW** (Phase J F8) | Convenience action for runtime ability grants. Earlier you had to grant via `apply_effect` or scaffold-side wiring. |
| `gas.describe_data_asset_gas_profile` / `gas.validate_data_asset_gas_profile` / `gas.set_data_asset_gas_fields` | **NEW** (2026-05-31 DataAsset GAS workflow P0/P3) | DataAsset-driven GAS profile inspection/validation plus strict, transacted, dry-run-first field writes for ability/effect/cue/input/policy roles. |
| `gas.start_event_cue_probe` / `gas.stop_event_cue_probe` / `gas.expect_event_cue` | **NEW** (2026-05-31 DataAsset GAS workflow P4) | Bounded PIE probe actions for GameplayEvent payload capture and active GameplayCue tag-count evidence; instant cue execute payload hooks are reported as unsupported instead of false success. |
| `gas.get_runtime_summary` | Return payload expanded (2026-05-31 DataAsset GAS workflow P0) | Adds namespace readiness, action count, `WITH_GBA`, ProjectIndex availability, and read-only fallback guidance while preserving safe no-PIE behavior. |
| `ai.add_perception_to_actor` | **NEW** (Phase J F8) | Direct perception attach without going through `add_perception_component` + manual wiring. |
| `ai.get_bt_graph` | **NEW** (Phase J F8) | Read-only graph dump distinct from `get_behavior_tree`'s structural inspection. |
| `audio.create_test_wave` | **NEW** (Phase J F18) | Procedurally synthesizes a 16-bit mono sine `USoundWave` for tests with zero asset deps. |
| `audio.bind_sound_to_perception` | Param validation tightened (Phase J F11) | `loudness <= 0`, `max_range < 0`, and unknown `sense_class` values now reject up-front instead of writing junk userdata. |
| `gas.bind_widget_to_attribute` (and 3 aliases) | Param validation tightened (Phase J F2/F3) | Empty `widget_path`, missing `attribute`, or unresolvable ASC now return structured errors before any reflection writes. |
| `ai` BT actions | Error message standardization (Phase J F15) | All BT-related actions now return `{ "error": "<code>", "detail": "<human>" }` instead of mixed prose. |
| `gas` UI binding response | Shape change (Phase J F5) | Returns `{ bindings: [...], count: N }` instead of a bare array. Wrap your client parsers. |

The aliased GAS UI binding actions live in **both** `ui::*` and `gas::*` namespaces — same handler, two callable paths. Pick whichever reads better from your client.

## Recent API Changes (v0.14.8 → v0.15.0)

These releases added the `level_sequence` namespace, the `bulk_fill` / `describe` ergonomics framework, a blueprint dataset read/edit pack, a UI/Blueprint gap-closure sweep, `monolith_guide`, and editor automation verbs. The per-namespace body sections below now document these; full param schemas for everything are also live via focused `monolith_discover` schema mode.

| Action | Change | Reason |
|--------|--------|--------|
| `bulk_fill_query("apply" / "list_namespaces")` | **NEW namespace** (0.15.0) | Reflection-walker bulk property fill across 11 in-tree adapters, with `dry_run` previews; sibling adapters may register when installed. |
| `describe_query("schema" / "list_targets" / "action_schema")` | **NEW namespace** (0.15.0) | Read-only schema introspection for the same adapter registry; `schema` supports namespace-level descriptors without `target`, `list_targets` is optional inventory, and `action_schema` returns a registered action's full param schema. |
| `monolith_discover` | Params added (0.15.0 compact merge) | Adds optional `action` and `mode` (`summary`, `actions`, `schema`) so callers can request one exact action schema without dumping a whole namespace. |
| `monolith.guide` | **NEW** (0.15.0) | Section-keyed onboarding guide for AI agents (onboarding / recipes / decisions / errors / skills_map / gotchas) with a live registry overlay. |
| Niagara Tranche 2 search/discovery hardening | Hardened ([Unreleased]) | `search_by_parameter`, `search_by_data_interface`, `query_niagara`, `find_similar_systems`, `search_by_material`, `find_niagara_references`, and `list_system_data_interfaces` now use typed schema validation, shared system enumeration/loading, range-checked cost governors, and strict integer parsing for the query DSL. |
| `FMonolithActionJsonBridge` | Core helper ([Unreleased]) | Shared registry-to-JSON bridge for Blueprint wrapper libraries. It preserves successful action payloads and emits the existing parseable `{success:false,error,code}` failure envelope. |
| `FMonolithToolInvocationLogger::FScopedTrace` | Export fixed ([Unreleased]) | The scoped trace/routing-context helper is now exported from `MonolithCore`, so sibling Monolith modules can verify or carry trace context across registry-dispatched action calls. |
| `blueprint.clone_graphs_with_reference_remap` | **NEW** ([Unreleased]) | Clone function/macro graphs from one Blueprint asset to another with class/object/root reference remaps, dry-run/confirm guard, collision policy, and optional destination compile/save reporting. |
| `blueprint` dataset pack (17 actions) | **NEW** (0.15.0) | DataTable (8), CurveTable (5), StringTable (3), `seed_data_asset` (1) — read with row-struct schema inline, bulk upsert with dry-run, row CRUD, JSON/CSV import/export. |
| `blueprint.add_property_access` / `override_parent_function` / `save_dirty_assets` | **NEW** (0.15.0) | Cross-class UPROPERTY get/set, value-returning parent-function override, batch save of dirty BP/Widget packages. |
| `ui` scaffolders + gap-closure (Tier 2/3/4 + Phase 3/4) | **NEW** (0.15.0) | `scaffold_main_menu`, `scaffold_settings_panel_with_tabs`, `scaffold_pause_menu`, `build_menu_from_spec`, `rename_widget`, `audit_focus_chain`, `set_widget_navigation_bulk`, `dump_widget_navigation`, `convert_border_to_common`, `reparent_widget_root`, `set_widget_is_variable`, and more. |
| `animation.add_anim_graph_node` | Param widened (0.15.0) | Optional `node_class` resolves arbitrary concrete custom `UAnimGraphNode_Base` classes by path/name; built-in `node_type` aliases preserved. |
| `niagara.get_system_summary` / `get_emitter_summary` | Param added (0.15.0, PR #60 @middle233) | Optional `detail_level: "compact" \| "full"` for event-aware payloads. `validate_system` now reasons about inter-emitter event chains. |
| `niagara.get_emitter_summary` `event_handlers[]` | **REMOVED — breaking** (0.15.0, PR #60) | Superseded by `consumed_events[]` (compact + full) and `incoming_events[]` (full only). Migrate `event_handlers[].source_emitter_id` → `incoming_events[].source_emitter_id`. |
| `mesh.import_mesh` | Params added (0.15.0, PR #58 @4698to) | Optional `import_as_skeletal` + `import_animations` widen the importer to `USkeletalMesh` + bundled `UAnimSequence` assets. |
| UserDefinedEnum-in-UserDefinedStruct schema | Fixed (0.15.0) | Now surfaces `enum_values` and accepts display-name writes instead of reporting a bare `int32`. Affects `read_data_table`, `describe_data_table_schema`, every bulk_fill/describe adapter, and `describe_query("schema")`. |
| `blueprint.create_user_defined_enum` | Fixed (0.15.0) | No longer drops the last enumerator of every enum it creates. |

---

## monolith

Core server management and introspection.

### `monolith.discover`

List available tool namespaces and their actions. Pass `namespace` to filter; pass `category` to narrow further (e.g. `"CommonUI"` inside `ui`). Pass both `namespace` and `action` with `mode="schema"` to fetch one exact action schema without dumping the whole namespace.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `namespace` | string | optional | Filter to a specific namespace |
| `action` | string | optional | Filter to a specific action inside `namespace`; most useful with `mode="schema"` |
| `category` | string | optional | Filter actions within the namespace by category |
| `mode` | enum | optional | `summary`, `actions`, or `schema`. Default is summary-style namespace discovery unless a legacy caller requests the namespace payload. |
| `planning_detail` | enum | optional | `compact` or `full`. Namespace action listings default to `compact`, which keeps planning status/count fields but omits heavy `precondition_details` and `planning_signals` arrays; pass `full` when auditing those arrays. |
| `schema_detail` | enum | optional | `compact` or `full`. Namespace action listings default to `compact`, which uses terse action descriptions and omits `search_metadata` plus per-param descriptions from inline `params`; focused `mode="schema"` defaults to `full`. |
| `if_version` | string | optional | Catalog version from a prior `monolith.status`/`monolith.discover` response. When it matches the live catalog, the response is a small `{status:"unchanged", catalog_version, total_actions, namespaces}` payload (<1KB) instead of the full listing. |
| `offset` | integer | optional | Pagination offset applied after category/filter. |
| `limit` | integer | optional | Pagination limit. Defaults to `50`; `0` is accepted for older callers but normalized to the default bounded page. Namespace listings with `detail=true` are capped to the default page size even when a larger limit is requested; continue with `next_cursor`/`offset`. |

**Returns:** Namespace summaries, action rows, or exact param schemas depending on `mode`; every non-short-circuit response also carries top-level `catalog_version`. Namespace action listings expose `planning_detail` / `schema_detail` plus hints when compact projections are active. AI clients also receive MCP tool schemas in `tools/list` at session start, so callers should request focused schema mode when they need exact params for one action. Recommended session routine: call `monolith.status` once, remember `catalog_version`, and pass it as `if_version` on repeat discovers.

---

### `monolith.find`

Fuzzy-rank profile-allowed actions by task text, namespace, category, action name, description, search metadata, and schema text. Use before `monolith.discover` when the exact action is unclear.

Match rows default to `planning_detail=compact` (planning status/count fields, no `precondition_details`/`planning_signals` arrays — the 2026-07-04 measurement showed the full arrays made the average find response ~37KB). The response carries the common list-projection contract next to the legacy `count`/`truncated` fields: `total`, `returned`, `next_cursor` (the next `offset` while more ranked matches remain), and a `projection` echo. Ranked order is deterministic for an unchanged catalog, so cursor pages do not overlap.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `query` | string | **required** | Task or action text, e.g. "find caller graph action" |
| `namespace` | string | optional | Namespace filter |
| `limit` | integer | optional | Max matches per page. Default: `8`, max `50` |
| `include_schema` | bool | optional | Include each match's param schema. Default: `false` |
| `planning_detail` | enum | optional | `compact` (default) or `full` |
| `offset` | integer | optional | Skip the first N ranked matches; chain pages through `next_cursor` |
| `cursor` | string | optional | Pagination cursor from a prior `next_cursor`; takes precedence over `offset` |
| `fields` | array\|string | optional | Project match rows to these fields (array or comma-separated), e.g. `action_id, description, score, skill`. Names absent from every row are reported in a warning listing the available fields |

---

### `monolith.execute_plan`

Execute a validated multi-step plan of registered actions in one call (v1, 2026-07-04). Steps run sequentially through the normal dispatch pipeline — profile gating, alias rewriting, schema validation, execution guards, and invocation logging apply to every child exactly as for a direct call, and child log records inherit the plan's trace with the plan action as parent span.

Whole-string params of the form `"$steps.<id>.result.<field.path>"` are replaced with a prior step's result value (object-field dot paths; all-digit segments index into arrays, e.g. `"$steps.s1.result.items.0.name"`; references to later steps are rejected at plan time). Plan-time validation checks every step's action existence, profile permission, aliases, required params (references count as provided), and typed/range/enum constraints before anything executes. Successful step results larger than `max_result_bytes_per_step` are summarized in the plan response (`result_truncated`, `result_bytes`, `result_top_keys`); the full result still reaches the invocation log.

**Transaction rollback (v2).** With `transaction=auto` (default) a mutating plan runs inside one outermost editor transaction. When `stop_on_error` halts the plan on a failure, the transaction is **cancelled** — every executed mutating step's undoable object edits roll back (each such step row gets `rolled_back:"editor_transaction"`), and no `partial_state_note` is emitted. The response `transaction` object reports `{mode, state}` (`state` ∈ `none`/`active`/`committed`/`cancelled`/`unavailable`/`off`) plus a `caveat`: **only undoable object edits roll back — saves, disk writes, source-control, and external-process effects stay applied.** `transaction=off` (or `stop_on_error=false`, which opts into partial state) commits surviving mutations and restores the v1 `partial_state_note` / `rollback_available:false` markers.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `steps` | array | **required** | Plan steps, each `{id?, namespace, action, params?}`. Max `25`; default ids `s1..sN` |
| `dry_run` | bool | optional | Validate every step and return the plan (per-step `policy_id`/`mutating`/`destructive`/`references`, `requires_confirm`, `requires_allow_destructive`) without executing. Default: `false` |
| `stop_on_error` | bool | optional | Stop at the first failing step and mark the rest `skipped`. `false` continues (opts into partial state); steps referencing a failed step still fail. Default: `true` |
| `confirm` | bool | optional | Required `true` when any step is mutating (execution policy other than `read_only`). Default: `false` |
| `allow_destructive` | bool | optional | Required `true` when any step is annotated destructive. Default: `false` |
| `transaction` | enum | optional | `auto` wraps a mutating plan in one outermost editor transaction, cancelled on a `stop_on_error` halt; `off` disables the wrapper. Default: `auto` |
| `max_result_bytes_per_step` | integer | optional | Per-step result size cap in the plan response. Default: `16384`, range `1024..262144` |

**Returns:** `status` (`ok`/`partial`/`error`), `succeeded`/`failed`/`skipped` counters, a `transaction` object, and per-step rows `{id, action_id, policy_id, mutating, destructive, references, status, duration_ms, result|error, rolled_back?}`. Nested `execute_plan` steps are rejected.

---

### `monolith.status`

Get Monolith server health: version, uptime, port, registered action count, namespace count, engine version, project name, module load status.

*No parameters.*

**Returns (catalog cache fields):** `catalog_version` (stable `sha256:<16>` fingerprint of the profile-visible catalog — names, schemas, planning/search metadata, annotations, active profile), `catalog_action_count`, `catalog_namespace_count`. Pass `catalog_version` to `monolith.discover(if_version=...)` to skip re-fetching an unchanged catalog.

---

### `monolith.update`

Check for or install Monolith updates from GitHub Releases. Auto-updater hits `https://api.github.com/repos/tumourlove/monolith/releases/latest`.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `action` | string | optional | `"check"` to compare versions, `"install"` to download and stage. Default: `"check"` |

---

### `monolith.reindex`

Re-index the Monolith project database. Incremental by default (delta only). Pass `force=true` for a full wipe + rebuild.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `force` | bool | optional | Full wipe + rebuild instead of incremental delta. Default: `false` |

**Returns:** with `UMonolithSettings::bEnableAsyncJobs=true` (default), `status:"started"`, `legacy_status:"reindex_started"`, `job_id`, `poll_action:"monolith.get_job"`, `cancel_action:"monolith.cancel_job"`, `supports_progress:true`, and `cancellable:true`. Polling that job reaches an honest terminal state from the index subsystem: `completed` on successful full/incremental/no-change indexing, `failed` when the indexer cannot start or reports failure, and `cancelled` when cooperative job cancellation is observed. If async jobs are disabled, the legacy response remains `status:"reindex_started"` plus a message. If the async start is rejected before indexing begins, the action response uses `status:"reindex_not_started"` and the returned `job_id` contains the failure details.

---

### `monolith.guide`

Section-keyed editorial onboarding guide for your AI agent — an onboarding script, worked cross-namespace recipes, X-vs-Y decision matrices, error-to-recovery maps, a skills map, and Monolith-specific gotchas. Hand-authored markdown plus a **live registry overlay**, so the action counts and version it reports always match your running build. New in 0.15.0. Built for users with no project `CLAUDE.md` or private skills — point your AI at it and it self-onboards. Offline parity via `monolith_query.exe monolith guide`.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `section` | string | optional | One of `onboarding`, `recipes`, `decisions`, `errors`, `skills_map`, `gotchas`. Omit for the full index + all sections. An unknown value returns a validation error listing the valid keys. |

**Returns:** The requested section (or full guide) as markdown, plus the live per-namespace action counts and plugin version.

---

### `monolith.get_job`

Return one async Monolith job's status, progress, and result by `job_id`. Read-only and idempotent. **Gated by `UMonolithSettings::bEnableAsyncJobs`** (default on). Long-running actions mint a `job_id` (e.g. `monolith.reindex` emits `job_id` + `poll_action="monolith.get_job"`).

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `job_id` | string | **required** | Async job id returned by a long-running action |

**Returns (flag on):** the registry row — `job_id`, `namespace`, `action`, `status` (`pending`/`running`/`completed`/`failed`/`cancelled`), `cancellable`, `supports_progress`, `cancel_requested`, optional `poll_action` / `cancel_action`, `progress {percent, stage, message}`, optional `result` / `error`, `created_utc`, `updated_utc`. An expired/evicted/never-minted id returns `{status:"not_found"}`. **Returns (flag off):** `{status:"disabled", requested_job_id, reason}`.

---

### `monolith.cancel_job`

Request cooperative cancellation of an async Monolith job by `job_id` and return its current row. A mutation (sets the cooperative cancel flag for cancellable jobs) but non-destructive and idempotent — the running action is expected to observe the flag and stop; the server never interrupts running work. **Gated by `UMonolithSettings::bEnableAsyncJobs`** (default on).

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `job_id` | string | **required** | Async job id to request cancellation for |

**Returns (flag on):** the post-request registry row with `cancel_requested:true` for a cancellable job. The row keeps its current non-terminal status (`pending`/`running`) until the producer observes the request and acknowledges cancellation, then `monolith.get_job` reports `status:"cancelled"`. A non-cancellable job keeps its current status and `cancel_requested:false`. Unknown ids return `{status:"not_found"}`. **Returns (flag off):** `{status:"disabled", requested_job_id, cancel_requested:false, reason}`.

---

## blueprint

Full read/write access to Blueprint graphs, variables, components, functions, nodes, pins, interfaces, timelines, comments, CDOs, spawn-time actor placement, and dataset read/edit (DataTable / CurveTable / StringTable round-trip + `seed_data_asset`). Count is approximate — query `monolith_discover("blueprint")` for the live figure.

**New in v0.18.1 (Motion Matching + thread-safe AnimBP authoring + inspection):**
- `set_anim_class`, `apply_movement_preset`, `add_engine_component_typed`, `scaffold_locomotion_input`, `validate_animbp_variable_contract`, `scaffold_motion_matching_character` — character/actor scaffolding for a motion-matching setup (adds an `EnhancedInput` dep).
- `add_property_access_node` (reflective `K2Node_PropertyAccess` for thread-safe property reads), `set_function_thread_safe` (mark a Blueprint function `BlueprintThreadSafe`). `scaffold_locomotion_anim_values` now emits a fully-wired thread-safe body via Property Access and can target a named function graph.
- `get_component_details` falls back to inherited native components (reports `is_inherited_native`, `skeletal_mesh`, `anim_class`, `animation_mode`); `get_blueprint_info` adds `native_component_count`; `get_inherited_component_override` reads the effective component template value + source; `seed_data_asset` gained `read_back_values`; `get_cdo_properties` routes through the shared reflection reader as the canonical verify-after-write path.

> For full param schemas, call `describe_query("action_schema", target_namespace="blueprint", target_action="<name>")` (or `monolith_discover("blueprint", detail=true)`). Plain `monolith_discover("blueprint")` is terse — action names + one-line descriptions only. The action surface is too broad to enumerate here without bloat — high-traffic actions are documented below; the rest are listed and discoverable.

**Action categories:**

| Category | Actions | Examples |
|----------|---------|----------|
| Graph inspection | 9 | `list_graphs`, `get_graph_data`, `get_graph_summary`, `get_execution_flow`, `search_nodes`, `get_node_details` |
| Variables | 9 | `get_variables`, `add_variable`, `remove_variable`, `rename_variable`, `set_variable_type`, `set_variable_defaults`, `add_local_variable`, `remove_local_variable`, `add_replicated_variable` |
| Components | 7 | `get_components`, `get_component_details`, `add_component`, `remove_component`, `rename_component`, `reparent_component`, `set_component_property`, `duplicate_component` |
| Functions / Macros / Events | 12 | `get_functions`, `add_function`, `remove_function`, `rename_function`, `add_macro`, `remove_macro`, `rename_macro`, `add_event_dispatcher`, `remove_event_dispatcher`, `set_function_params`, `set_event_dispatcher_params`, `get_function_signature` |
| Interfaces | 4 | `implement_interface`, `remove_interface`, `get_interfaces`, `scaffold_interface_implementation` |
| Node ops | 11 | `add_node`, `remove_node`, `connect_pins`, `disconnect_pins`, `set_pin_default`, `set_node_position`, `resolve_node`, `add_event_node`, `add_comment_node`, `promote_pin_to_variable`, `add_nodes_bulk` |
| Bulk / batch | 4 | `batch_execute`, `add_nodes_bulk`, `connect_pins_bulk`, `set_pin_defaults_bulk` |
| Timelines | 4 | `add_timeline`, `add_timeline_track`, `set_timeline_keys`, `get_timeline_data` |
| Compile / validate | 3 | `compile_blueprint`, `validate_blueprint`, `get_dependencies` |
| Asset CRUD | 8 | `create_blueprint`, `duplicate_blueprint`, `save_asset`, `create_user_defined_struct`, `create_user_defined_enum`, `create_data_table`, `add_data_table_row`, `get_data_table_rows`, `create_data_asset` |
| Dataset — DataTable (0.15.0) | 8 | `read_data_table`, `describe_data_table_schema`, `set_data_table_rows`, `remove_data_table_row`, `rename_data_table_row`, `duplicate_data_table_row`, `export_data_table`, `import_data_table` |
| Dataset — CurveTable (0.15.0) | 5 | `read_curve_table`, `set_curve_table_keys`, `add_curve_table_row`, `remove_curve_table_row`, `rename_curve_table_row` |
| Dataset — StringTable (0.15.0) | 3 | `read_string_table`, `set_string_table_entries`, `remove_string_table_entry` |
| Dataset — DataAsset (0.15.0) | 1 | `seed_data_asset` (create + bulk-fill in one atomic call) |
| Cross-class / overrides (0.15.0) | 3 | `add_property_access`, `override_parent_function`, `save_dirty_assets` |
| CDO | 2 | `get_cdo_properties`, `set_cdo_property` |
| Templates / spec | 4 | `build_blueprint_from_spec`, `apply_template`, `list_templates`, `compare_blueprints` |
| Graph export / clone | 4 | `export_graph`, `copy_nodes`, `duplicate_graph`, `clone_graphs_with_reference_remap` |
| Layout | 1 | `auto_layout` |
| Spawn | 2 | `spawn_blueprint_actor`, `batch_spawn_blueprint_actors` |

**Header set most callers reach for first:**

### `blueprint.list_graphs`

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `asset_path` | string | **required** | Package path of the Blueprint asset |

Returns array of graphs with name, type (`event_graph` / `function` / `macro` / `delegate_signature`), and node count.

### `blueprint.get_graph_summary`

Lightweight overview with node id/class/title and exec connections only. ~10 KB vs ~172 KB for `get_graph_data` on complex graphs.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `asset_path` | string | **required** | Package path |
| `graph_name` | string | optional | Defaults to first UbergraphPage |

### `blueprint.get_component_details`

Reads full reflected properties for a named Blueprint component. The lookup covers SCS components and inherited native actor components. If `component_name` is not found, the action returns `match_status=component_not_found` with `scs_components`, `inherited_native_components`, `candidate_components`, and `next_actions` instead of a blind retry error.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `asset_path` | string | **required** | Blueprint asset path |
| `component_name` | string | **required** | Component variable/native component name from `blueprint.get_components` |

### `blueprint.search_functions`

Search Blueprint-callable native functions. At least one of `query` or `class_filter` is required. The default `detail_level=minimal` returns function identity plus parameter counts; use `detail_level=standard` to include full `inputs[]` and `outputs[]` arrays.

The response carries the common list-projection contract next to the legacy `matched_count`/`returned_count` fields: `total`, `returned`, `next_cursor` (the next `offset`, present while more matches remain), `limits` (`default_limit=50`, `max_limit=1000`), and a `projection` echo (`detail`, `offset`, `fields`). Match order is stable within an editor session (session function cache), so pages chained through `next_cursor` do not overlap.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `query` | string | optional | Case-insensitive substring over function name, class name, and category |
| `class_filter` | string | optional | Case-insensitive class-name substring |
| `pure_only` | bool | optional | Only return pure functions. Default: `false` |
| `limit` | integer | optional | Max results per page. Default: `50`, hard max `1000` |
| `detail_level` | string | optional | `minimal` or `standard`. Default: `minimal` |
| `offset` | integer | optional | Skip the first N matches; chain pages through `next_cursor`. Default: `0` |
| `cursor` | string | optional | Pagination cursor from a prior `next_cursor`; takes precedence over `offset` |
| `fields` | array\|string | optional | Project result rows to these fields (array or comma-separated): `function_name`, `class_name`, `category`, `is_pure`, `is_static`, `k2_name`, `friendly_name`, `input_count`, `output_count`, `inputs`, `outputs`. Unknown names are ignored with a warning |

### `blueprint.build_blueprint_from_spec`

The crown jewel — author an entire Blueprint (parent class, variables, components, functions, event graph nodes, connections) from a single JSON spec. Validates and compiles in one call. See `describe_query("action_schema", target_namespace="blueprint", target_action="build_blueprint_from_spec")` for the full spec schema.

### `blueprint.clone_graphs_with_reference_remap`

Clone function or macro graphs from one Blueprint asset into another, then repair copied hard object/class references and reflected soft object paths through explicit class/object/root remaps. Event graphs, interface graphs, and delegate signature graphs are rejected with actionable errors.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `source_asset_path` | string | **required** | Source Blueprint asset path |
| `destination_asset_path` | string | **required** | Destination Blueprint asset path |
| `graph_name` | string | optional | Single source graph name |
| `new_name` | string | optional | Destination graph name for `graph_name`; defaults to the source graph name |
| `graphs` | array | optional | Batch graph specs as strings or `{source_graph|graph_name, destination_graph|new_name}` objects |
| `class_remaps` | object | optional | Map source class path/name to destination class path/name |
| `object_remaps` | object | optional | Map exact object paths used for hard/soft reference replacement |
| `root_remaps` | object | optional | Map source package roots to destination roots |
| `source_root` / `dest_root` | string | optional | Single-root shorthand; must be supplied together |
| `existing_policy` | string | optional | `fail`, `replace`, or `skip`. Default: `fail` |
| `allow_empty_remap` | bool | optional | Allow clone without remaps. Default: `false` |
| `dry_run` | bool | optional | Plan only. Default: `true` |
| `confirm` | bool | optional | Required when `dry_run=false`. Default: `false` |
| `compile` | bool | optional | Compile the destination Blueprint after applying. Default: `true` |
| `save` | bool | optional | Save the destination package after applying. Default: `false` |

Returns `plan`, `cloned_graphs`, `changed`, `replacement_object_count`, `hard_reference_fixup_count`, `soft_reference_fixup_count`, `pin_reference_fixup_count`, and optional `compile_result` / `saved_filename`.

### `blueprint.spawn_blueprint_actor`

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `blueprint` | string | **required** | Blueprint asset path (e.g. /Game/Blueprints/BP_Lamp) |
| `location` | array\|object | optional | Spawn location [x,y,z] or {x,y,z} |
| `rotation` | array\|object | optional | Spawn rotation [pitch,yaw,roll] |
| `scale` | array\|object | optional | Spawn scale [x,y,z] |
| `label` | string | optional | Actor label (display name) |
| `folder` | string | optional | Outliner folder path |
| `properties` | object | optional | Properties to set via reflection {name: value} |
| `tags` | array | optional | Array of string tags to add to actor |
| `sublevel` | string | optional | Target streaming sublevel name |
| `mobility` | string | optional | Root component mobility: static, stationary, movable |
| `select` | boolean | optional | Select actor after spawn |

### Dataset read/edit pack (0.15.0)

LLM-friendly read → edit → write loop for DataTables, CurveTables, and StringTables, plus `seed_data_asset` for DataAssets. Engine-generic (reflection + string class/struct resolution), zero sibling-plugin coupling. Reuses the MonolithCore reflection framework — reads inline an `FSchemaDescriptor`-shaped schema; writes return an `FDryRunReport`-shaped per-field result. All write actions take `save` (default `false`) to persist the package; reads never mutate.

#### `blueprint.read_data_table`

Read a DataTable's full contents plus its inline row schema. Supersedes `get_data_table_rows` by inlining the schema with the data.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `asset_path` | string | **required** | DataTable asset path (e.g. `/Game/Data/DT_Weapons`) |
| `include_schema` | boolean | optional | Include the inline row-field schema array. Default: `true` |
| `row_name` | string | optional | Return only this row; otherwise return all rows |

Returns `{ row_struct, row_struct_path, total_rows, schema:[{type_name, import_text_form, enum_values, range, children}], rows:[{row_name, values}] }`. Companion: `describe_data_table_schema` (schema only, no row data).

#### `blueprint.set_data_table_rows`

Bulk add/update DataTable rows in one call. Fires one editor-refresh broadcast at the end.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `asset_path` | string | **required** | DataTable asset path |
| `rows` | array | **required** | Array of `{row_name, values:{field:value}, mode?}` — mode is `upsert` (default), `add`, or `update` |
| `dry_run` | boolean | optional | Validate only — emit would-be writes but do not persist. Default: `false` |
| `strict` | boolean | optional | Promote silent drops / unknown fields / enum misses to hard errors. Default: `false` |
| `save` | boolean | optional | Save the package after applying. Default: `false` |

Returns an `FDryRunReport`-shaped per-field `{path, current, proposed, ok, reason}`.

#### Remaining dataset actions

| Action | Key params | Notes |
|--------|-----------|-------|
| `describe_data_table_schema` | `asset_path` | Row schema only, no data |
| `remove_data_table_row` | `asset_path`, `row_name`, `dry_run?`, `confirm?`, `save?` | Guarded row delete; requires `dry_run=true` or `confirm=true` |
| `rename_data_table_row` | `asset_path`, `old_name`, `new_name`, `save?` | Row rename |
| `duplicate_data_table_row` | `asset_path`, `source_row`, `new_name`, `save?` | Row copy |
| `export_data_table` | `asset_path`, `format?` (`json`/`csv`), `use_json_objects?`, `simple_text?` | Round-trippable text blob |
| `import_data_table` | `asset_path`, `text`, `format?`, `mode` (`replace` only), `save?` | **REPLACES** all rows; RowStruct must already be set |
| `read_curve_table` | `asset_path`, `row_name?` | Returns `mode` (`rich`/`simple`/`empty`) + per-key data |
| `set_curve_table_keys` | `asset_path`, `row_name`, `keys[{time,value}]`, `mode?`, `interp_mode?`, `save?` | First write locks rich-vs-simple; `cubic` → rich, others → simple |
| `add_curve_table_row` | `asset_path`, `row_name`, `interp_mode?`, `save?` | Empty curve row |
| `remove_curve_table_row` / `rename_curve_table_row` | `asset_path`, `row_name` / `old_name`+`new_name`, `save?` | Curve row CRUD |
| `read_string_table` | `asset_path`, `include_meta?` | Returns namespace + `entries:[{key, source_string, meta?}]` |
| `set_string_table_entries` | `asset_path`, `entries[{key, source_string}]`, `mode?` (`upsert`/`replace`), `namespace?`, `save?` | Upsert or full replace |
| `remove_string_table_entry` | `asset_path`, `key`, `save?` | Entry delete |

#### `blueprint.seed_data_asset`

Create AND populate a `UObject` DataAsset in one atomic call — `create_data_asset`'s body plus a reflection-walker fill of the supplied property `tree`.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `save_path` | string | **required** | Asset save path (e.g. `/Game/Data/DA_HealingPotion`) |
| `class_name` | string | **required** | `UObject` class name (same resolution as `create_data_asset`) |
| `tree` | object | **required** | Nested JSON of properties to walk against the new asset's reflection schema |
| `dry_run` | boolean | optional | Validate the tree against the class WITHOUT creating the asset. Default: `false` |
| `strict` | boolean | optional | Promote silent drops / unknown fields / enum misses to hard errors. Default: `false` |
| `skip_save` | boolean | optional | Skip synchronous package save. Default: `false` |

Existing DataAssets otherwise round-trip through `bulk_fill_query("apply")` + `describe_query("schema")`.

### Cross-class access + parent overrides (0.15.0)

#### `blueprint.add_property_access`

Author a `VariableGet` (or `VariableSet` if `is_setter`) node that reads/writes a UPROPERTY on an **arbitrary foreign class** — not just the Blueprint's own variables. `member_class` is resolved by string, then `VariableReference.SetExternalMember()` binds the member so the value pin resolves to the property's real type (unlike `node_type='VariableGet'`, which is self-context only and produces a wildcard 0-pin node for foreign properties).

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `asset_path` | string | **required** | Blueprint asset path |
| `member_class` | string | **required** | Class that owns the property (e.g. `Item`, `UItem`, `AActor`). Resolved native-first; accepts `U`/`A` prefix or bare name |
| `member_name` | string | **required** | Name of the UPROPERTY to read/write |
| `graph_name` | string | optional | Graph name (defaults to EventGraph) |
| `is_setter` | bool | optional | `true` creates a `VariableSet` (write) node; otherwise a `VariableGet` (read). Default: `false` |
| `position` | array | optional | `[x, y]` (default `[0, 0]`) |

Returns `node_id`, `value_pin_id`, and `target_pin_id` (the object/self input the caller wires via `connect_pins` to supply the instance).

#### `blueprint.override_parent_function`

Author a Blueprint override of an overridable parent function (`BlueprintImplementableEvent` / `BlueprintNativeEvent`), **including those that RETURN a value** (e.g. `UCommonActivatableWidget::BP_GetDesiredFocusTarget` → `UWidget*`). `add_function` cannot do this and the event-node form has no ReturnValue pin.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `asset_path` | string | **required** | Blueprint asset path |
| `parent_function_name` | string | **required** | Name of the overridable parent function |

Returns `graph_name`, `entry_node_id`, `return_pin_id`/`name`, `override_class`, `has_return_value`.

#### `blueprint.save_dirty_assets`

Save ALL currently-dirty Blueprint and Widget Blueprint packages in one sweep — closes the data-loss window after a batch of edits that dirty but do not persist packages.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `path_prefix` | string | optional | Only save assets whose package path starts with this prefix. Default: `/Game`. Empty string = all dirty BP/Widget packages |

Returns `saved[]`, `failed[]`, `count`.

See `Plugins/Monolith/Docs/specs/SPEC_MonolithBlueprint.md` for the deep dive.

---

## material

Material graph editing, inspection, CRUD, material functions, instances, custom HLSL nodes, PBR pipeline. **66 actions in the current default live catalog.** The category table below lists the documented primary surface; call live discovery for the full action list.

> For full param schemas, call `describe_query("action_schema", target_namespace="material", target_action="<name>")` (or `monolith_discover("material", detail=true)`). Plain `monolith_discover("material")` is terse — names + one-line descriptions only.

**Action categories:**

| Category | Actions | Examples |
|----------|---------|----------|
| Graph inspection | 7 | `get_all_expressions`, `get_expression_details`, `get_full_connection_graph`, `get_expression_pin_info`, `get_expression_connections`, `list_expression_classes`, `get_compilation_stats` |
| Graph CRUD | 10 | `build_material_graph`, `connect_expressions`, `disconnect_expression`, `delete_expression`, `move_expression`, `duplicate_expression`, `replace_expression`, `rename_expression`, `set_expression_property`, `auto_layout` |
| Material assets | 8 | `create_material`, `create_material_instance`, `duplicate_material`, `save_material`, `set_material_property`, `get_material_properties`, `recompile_material`, `refresh_copied_material_graphs` |
| Material instances | 8 | `get_material_parameters`, `get_instance_parameters`, `set_instance_parameter`, `set_instance_parameters`, `set_instance_parent`, `repair_copied_material_instance_parameters`, `clear_instance_parameter`, `list_material_instances` |
| Material functions | 7 | `create_material_function`, `build_function_graph`, `get_function_info`, `export_function_graph`, `set_function_metadata`, `update_material_function`, `delete_function_expression` |
| Custom HLSL | 2 | `create_custom_hlsl_node`, `update_custom_hlsl_node` |
| Spec / templates | 3 | `export_material_graph`, `import_material_graph`, `validate_material` |
| Preview / capture | 2 | `render_preview`, `get_thumbnail` |
| Layers | 1 | `get_layer_info` |
| Batch | 1 | `batch_set_material_property` |
| Transactions | 2 | `begin_transaction`, `end_transaction` |

### `build_material_graph` gotcha

This action **requires** the `{ "graph_spec": { ... } }` wrapper, not a bare spec. This trips people up:

```json
{ "graph_spec": { "expressions": [...], "connections": [...] } }
```

See `Plugins/Monolith/Docs/specs/SPEC_MonolithMaterial.md` for full graph_spec schema and the [§Pipelines](#pipelines) section for the canonical material build flow.

---

## animation

Animation curves, bone tracks, sync markers, root motion, compression, blend spaces, ABPs, montages, skeletons, PoseSearch, IKRig, retargeting (retarget pose + op-stack tuning + per-bone translation retargeting), locomotion authoring (root-motion speed, distance-curve baking), Control Rig, Motion Matching authoring, state machines, and live PIE anim telemetry. Count is approximate — query `monolith_discover("animation")` for the live figure.

**New in v0.18.1 (Motion Matching pack):**
- **Pose Search / database:** `create_normalization_set`, `add_database_to_normalization_set`, `set_database_normalization_set`, `add_database_entry`, `set_database_entry_tags`, `configure_schema_channel`, `derive_schema_channels_from_skeleton`, `add_pose_search_notify`, `validate_pose_search_database`.
- **Mirror tables:** `create_mirror_data_table`, `set_schema_mirror_data_table`.
- **AnimBP graph:** `configure_pose_history_node`, `configure_motion_matching_node`, `build_motion_matching_node` (composite, wires to the Output Pose), `add_evaluate_chooser_node`, `wire_chooser_to_motion_matching`, `build_foot_ik_pass`, `assign_post_process_anim_rig`, `bind_chooser_database_via_threadsafe`. `add_anim_graph_node` gained `pose_history` / `inertialization` aliases.
- **Retarget:** `create_ik_rig`, `create_ik_retargeter`, `set_retargeter_rigs`, `batch_retarget_animations`.
- **State machines + telemetry:** `create_state_machine`, `build_state_machine`, `sample_pie_anim_instance`, `get_anim_graph_choosers`, `get_transition_rule`, `get_anim_graph_output_connection`. `set_transition_rule` accepts a structured `kind=compare`; `get_nodes` gained `include_anim_graph`.

**New (Unreleased) — blend space baking + state-machine teardown + IK solver removal:**
- **Blend spaces:** `bake_blend_space` (rebuild a blend space's `FBlendSpaceData` triangulation via `ResampleData()` + mark dirty — repairs blend spaces authored externally or before the auto-bake fix; returns `has_blendspace_data`, `sample_count`, `baked`, and a `warning` when a 2D blend space has fewer than 3 samples), `set_blend_space_interpolation` (set `bInterpolateUsingGrid` via `use_grid` + a `preferred_triangulation_direction` of `None`/`Tangential`/`Radial`, then resample). In grid mode the triangulation is intentionally empty, so `has_blendspace_data` is `false` — correct, not a failure. The four blend space mutators (`add_blendspace_sample`, `edit_blendspace_sample`, `delete_blendspace_sample`, `set_blend_space_axis`) now auto-bake the triangulation after each edit, so MCP-authored blend spaces no longer ship empty and evaluate to the bind/reference pose at runtime.
- **State machines:** `remove_anim_state` (remove a state + tear down its inner anim graph; `remove_dependent_transitions` default `true`; refuses to remove the machine's current entry state — re-point first), `set_anim_entry_state` (re-point the Entry node at an existing state; returns the previous target, `unchanged:true` fast-path), `remove_anim_transition` (remove a from→to transition; reports `matched_transition_count`).
- **IKRig:** `remove_ik_solver` (remove a solver by 0-based `solver_index`, validated against the solver count; returns `removed_index`, `solver_count_after`). `add_ik_solver` now resolves `solver_type` against the live solver-struct table (friendly alias → exact struct name → unique substring; ambiguous input errors with the candidate list) instead of a hardcoded reflected path that did not resolve in UE 5.7, so Full Body IK now adds correctly and `root_bone` is meaningful for FBIK.
- **AnimGraph authoring (14):** `add_apply_additive` / `add_apply_mesh_space_additive` (Apply Additive / mesh-space additive nodes), `add_slot_node` (slot name validated against the skeleton's slot groups), `add_save_cached_pose` / `add_use_cached_pose` (paired by `cache_name`), `set_output_pose_source` (wire a node into the AnimGraph Output / Root result pin), `set_state_result_source` (wire a node into a state machine state's result pin), `add_blend_by_int` (grown to `num_poses` pose pins), `add_blend_by_enum` (Blend Poses by Enum bound to a `UEnum` via `enum_path`; one pose pin per exposed enumerator plus a Default/else pin, skipping the auto `_MAX` sentinel and `Hidden` enumerators; optional `enumerators` exposes a subset), `set_sync_group` (player node sync group name / role / method), `set_layered_blend_bones` (per-bone branch filters on a Layered Blend Per Bone node), `add_anim_control_rig_node` (`control_rig_class`; IO pins regenerate), `add_linked_anim_layer` (`layer_name` + optional `interface_class`), `add_conduit` (a state-machine conduit whose bound graph is a transition-logic graph, not an anim graph). Plus 3 extensions: `set_anim_node_pin_binding` now bootstraps the binding object on previously-unbound nodes; `auto_layout` gained a Blueprint-Assist-free `builtin` formatter (also the `auto` fallback) so layout works in release builds where Blueprint Assist is compiled out; `set_transition_rule` gained an `expression` kind (compound multi-term `terms[]` — each `{lhs, op, rhs, abs?, negate?}` — folded through Boolean AND/OR via `combine`), extending the existing `bool` / `auto` / `compare` kinds.

**New (Unreleased) — retargeting + locomotion authoring + inspection:**
- **Retarget pose + op-stack tuning (9):** `align_retarget_pose` (auto-align a source/target retarget pose via AutoAlign + SnapBoneToGround), `get_retarget_pose` / `set_retarget_pose` (read/edit a retarget pose — `set` `mode`: `from_reference` or `bone_deltas`; `from_animation` deferred), `get_retarget_chain_settings` / `set_retarget_chain_settings` (read/write a chain's FK & IK op-stack settings — rotation/translation modes, IK blend, etc.), `set_retarget_root_settings` (Pelvis Motion op: vertical scale, floor constraint, affect-IK), `enable_foot_ground_lock` (Speed Planting op foot ground-lock on named IK chains), `set_bone_translation_retargeting` / `get_bone_translation_retargeting` (per-bone `USkeleton` translation-retargeting modes — `Animation` / `Skeleton` / `AnimationScaled` / `AnimationRelative` / `OrientAndScale` — plus a `biped_locomotion` preset on the setter).
- **Locomotion authoring (3):** `get_root_motion_speed` (authored root-motion ground speed in cm/s, with an explicit "unknowable" signal for root-locked / no-root-motion clips), `bake_distance_curve` (bake a `Distance` curve onto a sequence via the engine `DistanceCurveModifier` — removes any existing same-named curve first, then persists into the asset's modifier stack), `bind_threadsafe_update_function` (wire a Blueprint-library static call into an AnimBP's `BlueprintThreadSafeUpdateAnimation` graph — v1a: known-signature).
- **IK Rig bone settings (2):** `set_ik_rig_bone_settings` / `get_ik_rig_bone_settings` (write/read a bone's per-solver IK Rig bone settings, reflective).
- **Inspection (1):** `get_animated_bone_transform` (FK-composed bone transform at a frame/time, component or world space).
- **Extensions (no new actions):** `get_retargeter_info` now emits an `ops[]` array (per-op type + settings); `apply_anim_modifier` now accepts a `properties` reflective field set + a `persist` flag (register into the `AnimationModifiers` stack); `get_blend_space_info` now reports per-sample authored root-motion speed + `triangulation_baked` / `interpolate_using_grid`; `get_anim_node_pin_bindings` now also emits wire-linked input pins (`type:"Link"` with source node/pin); `derive_foot_sync_markers` gains a `from_bones` mode (foot plants from per-frame foot-bone height + planar-speed minima); `get_curve_keys` now reports `monotonic` + `sign` flags; `set_anim_node_function_binding` now calls `RequestRefreshExtensions` so the recompile regenerates the anim-subsystem set (prevents a null `NodeRelevancy` subsystem at runtime).

> For full param schemas, call `describe_query("action_schema", target_namespace="animation", target_action="<name>")` (or `monolith_discover("animation", detail=true)`). Plain `monolith_discover("animation")` is terse — names + one-line descriptions only.

**Action categories:**

| Category | Actions | Examples |
|----------|---------|----------|
| Sequence ops | 12 | `get_sequence_info`, `get_sequence_notifies`, `set_sequence_properties`, `set_additive_settings`, `set_compression_settings`, `set_root_motion_settings`, `create_sequence`, `duplicate_sequence`, `build_sequence_from_poses` |
| Bone tracks | 6 | `add_bone_track`, `remove_bone_track`, `set_bone_track_keys`, `get_bone_track_keys`, `list_bone_tracks`, `copy_bone_pose_between_sequences` |
| Curves | 7 | `add_curve`, `remove_curve`, `set_curve_keys`, `get_curve_keys` (reports `monotonic` + `sign`), `list_curves`, `get_skeleton_curves`, `bake_distance_curve` |
| Notifies | 9 | `add_notify`, `add_notify_state`, `remove_notify`, `set_notify_time`, `set_notify_duration`, `set_notify_track`, `set_notify_properties`, `bulk_add_notify`, `clone_notify_setup` |
| Sync markers | 5 | `get_sync_markers`, `add_sync_marker`, `remove_sync_marker`, `rename_sync_marker`, `derive_foot_sync_markers` (gains a `from_bones` mode) |
| Skeleton | 9 | `get_skeleton_info`, `get_skeletal_mesh_info`, `add_socket`, `remove_socket`, `set_socket_transform`, `get_skeleton_sockets`, `add_virtual_bone`, `remove_virtual_bones`, `compare_skeletons` |
| Skeleton (read/compat, v0.14.10) | 5 | `get_skeleton_preview_attached_assets`, `get_bone_ref_pose`, `get_compatible_skeletons`, `add_compatible_skeleton`, `remove_compatible_skeleton` |
| Montages | 9 | `get_montage_info`, `create_montage`, `set_montage_blend`, `add_montage_section`, `delete_montage_section`, `set_section_next`, `set_section_time`, `add_montage_slot`, `set_montage_slot`, `add_montage_anim_segment`, `create_montage_from_sections` |
| Blend spaces | 11 | `get_blend_space_info`, `create_blend_space`, `create_blend_space_1d`, `create_aim_offset`, `create_aim_offset_1d`, `add_blendspace_sample`, `edit_blendspace_sample`, `delete_blendspace_sample`, `set_blend_space_axis`, `bake_blend_space`, `set_blend_space_interpolation` |
| ABPs | 9 | `get_abp_info`, `create_anim_blueprint`, `get_state_machines`, `get_state_info`, `get_transitions`, `get_blend_nodes`, `get_linked_layers`, `get_graphs`, `get_nodes`, `get_abp_variables`, `get_abp_linked_assets` |
| State machines (write) | 6 | `add_state_to_machine`, `add_transition`, `set_transition_rule` (`bool` / `auto` / `compare` / `expression` kinds — `expression` folds multi-term `terms[]` through Boolean AND/OR), `remove_anim_state`, `set_anim_entry_state`, `remove_anim_transition` |
| ABP graph (write) | 5 | `add_anim_graph_node` (aliases or generic `UAnimGraphNode_Base` class path/name via `node_type` / `node_class`), `connect_anim_graph_pins`, `set_state_animation`, `add_variable_get`, `set_anim_graph_node_property` |
| ABP graph authoring (Unreleased) | 14 | `add_apply_additive`, `add_apply_mesh_space_additive`, `add_slot_node`, `add_save_cached_pose`, `add_use_cached_pose`, `set_output_pose_source`, `set_state_result_source`, `add_blend_by_int`, `add_blend_by_enum` (Blend Poses by Enum bound to a `UEnum`; one pin per exposed enumerator + Default, skips `_MAX` + `Hidden`), `set_sync_group`, `set_layered_blend_bones`, `add_anim_control_rig_node`, `add_linked_anim_layer`, `add_conduit` |
| Composites | 3 | `get_composite_info`, `add_composite_segment`, `remove_composite_segment`, `create_composite` |
| IKRig / Retarget | 10 | `get_ikrig_info`, `add_ik_solver`, `remove_ik_solver`, `set_ik_rig_bone_settings`, `get_ik_rig_bone_settings`, `get_retargeter_info` (emits `ops[]`), `set_retarget_chain_mapping`, `add_retarget_chain`, `remove_retarget_chain`, `set_retarget_chain_bones` |
| Retarget pose + ops (Unreleased) | 9 | `align_retarget_pose`, `get_retarget_pose`, `set_retarget_pose`, `get_retarget_chain_settings`, `set_retarget_chain_settings`, `set_retarget_root_settings`, `enable_foot_ground_lock`, `set_bone_translation_retargeting`, `get_bone_translation_retargeting` |
| Locomotion / inspection (Unreleased) | 4 | `get_root_motion_speed`, `bake_distance_curve`, `bind_threadsafe_update_function`, `get_animated_bone_transform` |
| Control Rig | 7 | `get_control_rig_info`, `get_control_rig_variables`, `add_control_rig_element`, `get_control_rig_graph`, `add_control_rig_node`, `connect_control_rig_pins` |
| Anim modifiers | 2 | `apply_anim_modifier` (accepts `properties` + `persist`), `list_anim_modifiers` |
| Physics asset | 3 | `get_physics_asset_info`, `set_body_properties`, `set_constraint_properties` |
| PoseSearch | 13 | `get_pose_search_schema`, `get_pose_search_database`, `add_database_sequence`, `remove_database_sequence`, `get_database_stats`, `create_pose_search_schema`, `create_pose_search_database`, `set_database_sequence_properties`, `add_schema_channel`, `remove_schema_channel`, `set_channel_weight`, `rebuild_pose_search_index`, `set_database_search_mode` |
| Layout / batch | 2 | `auto_layout`, `batch_execute` |

See `Plugins/Monolith/Docs/specs/SPEC_MonolithAnimation.md` for the deep dive.

---

## metahuman

Read-only MetaHuman capability status and asset discovery.

| Action | Params |
|--------|--------|
| `get_status` | none |
| `list_character_assets` | `package_path` (optional string), `limit` (optional integer) |

See `Plugins/Monolith/Docs/specs/SPEC_MonolithAnimation.md` for the deep dive.

---

## cloth

Optional Chaos Cloth/Outfit workflow discovery. Read-only probes; does not load vertex data, weight maps, or mutate assets. **2 actions.**

| Action | Params |
|--------|--------|
| `get_status` | none |
| `list_clothing_assets` | `package_path` (optional string), `limit` (optional integer) |

See `Plugins/Monolith/Docs/specs/SPEC_MonolithAnimation.md` for the deep dive.

---

## niagara

Niagara VFX system editing — emitters, modules, params, renderers, HLSL, dynamic inputs, event handlers, sim stages, NPC, effect types, temporal control, stateless-emitter factory. **129 actions** (108 baseline + 1 layout + 9 temporal-control + 1 stateless-emitter factory + 7 issue #64 Tranche 2 search + 2 PR #65 CustomHlsl-text read/write).

> For full param schemas, prefer focused `monolith_discover({ "namespace": "niagara", "action": "<action>", "mode": "schema" })` at runtime.

The Tranche 2 search/discovery actions are backed by strict `FParamSchemaBuilder` validation and a shared Niagara system enumeration/load helper. Use `folder` and `limit` as cost governors where the focused schema exposes them (`limit` range: 1..1000). `find_similar_systems` additionally range-checks `threshold` from 0..1. `query_niagara` rejects malformed integer DSL clauses instead of silently converting them to zero. `find_similar_systems` and `list_system_data_interfaces` accept `system_path` as an alias for `asset_path`. Focused `monolith_discover(..., mode:"schema")` and `describe_query("action_schema", ...)` expose matching params for these actions. Blueprint JSON wrapper libraries route through the Core `FMonolithActionJsonBridge` so successful payloads stay unchanged while failures keep the existing parseable envelope.

**Action categories:**

| Category | Actions | Examples |
|----------|---------|----------|
| Systems | 11 | `create_system`, `create_system_from_spec`, `duplicate_system`, `validate_system`, `save_system`, `set_system_property`, `get_system_property`, `get_system_summary`, `get_system_diagnostics`, `set_fixed_bounds`, `set_effect_type`, `list_systems` |
| Emitters | 12 | `add_emitter`, `remove_emitter`, `duplicate_emitter`, `set_emitter_enabled`, `reorder_emitters`, `set_emitter_property`, `get_emitter_property`, `get_emitter_summary`, `list_emitters`, `list_emitter_properties`, `create_emitter`, `rename_emitter`, `save_emitter_as_template`, `clear_emitter_modules`, `get_emitter_parent` |
| Modules | 10 | `add_module`, `remove_module`, `move_module`, `set_module_enabled`, `get_ordered_modules`, `get_module_inputs`, `get_module_graph`, `get_module_input_value`, `get_module_output_parameters`, `get_module_script_inputs`, `set_module_input_value`, `set_module_input_binding`, `set_module_input_di`, `clone_module_overrides`, `duplicate_module`, `list_module_scripts` |

`get_ordered_modules` / `add_module` / `move_module` / `duplicate_module` support selector-based stages (PR #65): `usage: "particle_event"` with `usage_id` or `handler_index`, and `usage: "particle_simulation_stage"` with `usage_id`, `stage_name`, or `stage_index`.

| HLSL / scripts | 4 | `create_module_from_hlsl`, `create_function_from_hlsl`, `get_custom_hlsl_text`, `set_custom_hlsl_text` |
| Parameters | 9 | `get_all_parameters`, `get_user_parameters`, `get_parameter_value`, `get_parameter_type`, `trace_parameter_binding`, `add_user_parameter`, `remove_user_parameter`, `set_parameter_default`, `set_curve_value`, `get_available_parameters`, `rename_user_parameter`, `set_static_switch_value`, `get_static_switch_value` |
| Renderers | 9 | `add_renderer`, `remove_renderer`, `set_renderer_material`, `set_renderer_property`, `get_renderer_bindings`, `set_renderer_binding`, `list_renderers`, `list_renderer_properties`, `list_available_renderers`, `set_renderer_mesh`, `configure_ribbon`, `configure_subuv` |
| Dynamic inputs | 7 | `add_dynamic_input`, `set_dynamic_input_value`, `search_dynamic_inputs`, `list_dynamic_inputs`, `get_dynamic_input_tree`, `remove_dynamic_input`, `get_dynamic_input_value`, `get_dynamic_input_inputs` |
| Event handlers / sim stages | 8 | `add_event_handler`, `get_event_handlers`, `set_event_handler_property`, `remove_event_handler`, `add_simulation_stage`, `get_simulation_stages`, `set_simulation_stage_property`, `remove_simulation_stage`, `set_spawn_shape` |
| NPC | 5 | `create_npc`, `get_npc`, `add_npc_parameter`, `remove_npc_parameter`, `set_npc_default` |
| Effect types | 3 | `create_effect_type`, `get_effect_type`, `set_effect_type_property` |
| Data interfaces | 4 | `get_di_functions`, `get_compiled_gpu_hlsl`, `configure_data_interface`, `get_di_properties` |
| Compile / preview | 4 | `request_compile`, `preview_system`, `diff_systems`, `get_scalability_settings`, `set_scalability_settings` |
| Curves / spec | 5 | `configure_curve_keys`, `import_system_spec`, `export_system_spec`, `batch_execute`, `auto_layout` |
| Temporal control (system) | 4 | `get_system_timing` (bundled read of `WarmupTime` / `WarmupTickCount` / `WarmupTickDelta` / `bFixedTickDelta` / `FixedTickDeltaTime` / `bRequireCurrentFrameData`), `set_warmup_profile` (composite write returning the engine-resolved triple after `ResolveWarmupTickCount` snap), `set_fixed_tick_delta`, `set_require_current_frame_data` |
| Temporal control (emitter) | 2 | `set_emitter_loop_profile` (composite write of EmitterState loop topology — `loop_behavior` / `loop_duration` / `loop_delay` / `loop_count` / `loop_delay_enabled`; optional `loop_duration_mode` for stateless; native dispatch to `UNiagaraStatelessEmitter` standalone assets), `get_emitter_timing_summary` (read aggregator: loop topology + `sim_stages[]` + `InitializeParticle` lifetime fields; stateless branch returns `stateless: true` with `null` lifetime + empty `sim_stages`) |
| Temporal control (sim stage) | 2 | `set_sim_stage_iteration_count`, `set_sim_stage_execute_behavior` (both alias atop `set_simulation_stage_property` with PR #65's `stage_index` / `stage_name` selector convention) |
| Temporal control (particle) | 1 | `set_particle_lifetime` (`min` only → Direct mode constant `Lifetime`; `min` + `max` → Random mode `Lifetime Min` / `Lifetime Max`) |
| Stateless emitter factory | 1 | `create_stateless_emitter` (standalone `UNiagaraStatelessEmitter` / Lightweight Emitter asset; pairs with the stateless-aware branches of `set_emitter_loop_profile` + `get_emitter_timing_summary`) |

**CustomHlsl direct-editing (PR #65):**

| Action | Params | Notes |
|--------|--------|-------|
| `get_custom_hlsl_text` | `script_path` (required), `node_guid`? | Read the HLSL source from a `CustomHlsl` node via public UPROPERTY reflection. `node_guid` disambiguates multi-`CustomHlsl`-node scripts. Always available regardless of `WITH_NIAGARA_WIZARD_PRIVATE` |
| `set_custom_hlsl_text` | `script_path` (required), `hlsl` (required), `node_guid`? | Overwrite a `CustomHlsl` node's HLSL source under `Modify()` + transaction with a recompile. Always available regardless of `WITH_NIAGARA_WIZARD_PRIVATE` |

`add_event_handler` now returns `handler_index` + `usage_id` + `usage`; for inter-emitter handlers `source_emitter` must resolve or the handler is rejected. It does not auto-add `Receive<Event>` modules. `add_simulation_stage` materializes the matching `particle_simulation_stage` output node and returns `usage_id` / `stage_id` / `graph_outputs`. `create_module_from_hlsl` generates a ParameterMap bridge graph, preserves DI input types (NeighborGrid3D / Grid3D / ParticleRead), and strictly validates HLSL input/output types (unknown types hard-fail). **Before writing custom HLSL, read `Plugins/Monolith/Docs/NIAGARA_HLSL_GUIDE.md`.**

See `Plugins/Monolith/Docs/specs/SPEC_MonolithNiagara.md`.

---

## editor

Live Coding builds, compile output capture, editor log capture, scene capture, texture import, asset deletion, viewport info, GIF capture, **map creation** and **module status** (Phase J F8), plus the **PIE / console / Python automation** verbs (`run_console_command`, `start_pie`, `stop_pie`, `run_python`, `load_level`). Count is approximate — query `monolith_discover("editor")` for the live figure.

**New in v0.18.1 (PIE / profiling harness):**
- **PIE smoke + capture:** `run_pie_smoke` / `poll_pie_smoke` / `stop_pie_smoke` (async session model), `capture_pie_movement_clip` (with `discard_first_frames` warm-up, label-aware `view_target_actor`, staged hooks, runtime-identity report + `expected_anim_class` assert), `capture_anim_frames` (preview AnimSequence / BlendSpace / AnimBlueprint to PNG), `list_dirty_packages`, `save_packages`, `list_errored_blueprints`.
- **Profiling / actor setup:** a declarative `actor_setup` block (spawn N actors, copy a DataAsset's reflected fields, AIController MoveToLocation) plus `csv_profile` / `trace_channels` brackets scoped to the PIE window. `get_build_errors` gained `since_marker` / `since_iso` / `clear_baseline` + compile-vs-other buckets.
- **Map authoring:** `author_map_settings` (WorldSettings GameMode override + PlayerStarts + actor instances), `set_world_settings_property` (generic reflected `AWorldSettings` property writer, including Lyra-style `DefaultGameplayExperience`), `create_nav_harness_map` (now with `game_mode_override` + `player_starts`).
- **Validation:** `validate_assets` wraps `UEditorValidatorSubsystem::ValidateAssetsWithSettings` for explicit assets/packages or recursive package-path validation; `plan_content_validation_changeset` and `validate_changeset_assets` map Perforce opened/changelist or explicit depot/local/package paths into validation packages before running the same wrapper.

### `editor.trigger_build` / `editor.live_compile`

Trigger a Live Coding compile. Aliased — they're the same handler.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `wait` | bool | optional | Block until compile finishes. Default: `false` |

### `editor.get_build_errors`

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `since_marker` | string | optional | Report only errors AFTER the latest log line containing this token. Reports marker_found |
| `since_iso` | string | optional | Absolute ISO-8601 cutoff (e.g. 2026-06-06T12:00:00Z) |
| `since_seconds` | number | optional | Relative window: only errors from the last N seconds |
| `since` | number | optional | Legacy alias for since_seconds (last N seconds) |
| `clear_baseline` | bool | optional | Stamp a fresh baseline (now) and return nothing before it. Returns immediately. Default: `false` |
| `category` | string | optional | Narrow the QUERY to a specific log category |
| `compile_only` | bool | optional | Narrow the QUERY to compile categories only. Default: `false` |
| `exclude_categories` | array | optional | Categories bucketed under other_errors and kept OUT of the headline error_count. Default: `[LogPython, LogMonolith]` |

### `editor.get_build_status`

Check compile status: `compiling`, `last_result`, `last_compile_time`, `errors_since_compile`, `patch_applied`. *No parameters.*

### `editor.get_build_summary` · `editor.search_build_output` · `editor.get_compile_output`

Build summary, search-build-log-by-pattern, structured compile report. See `describe_query("action_schema", target_namespace="editor", target_action="<name>")` for params.

### `editor.get_recent_logs` · `editor.search_logs` · `editor.tail_log` · `editor.get_log_categories` · `editor.get_log_stats`

Editor log inspection. `search_logs` accepts `pattern`, `category`, `verbosity`, `limit`. `tail_log` and `get_recent_logs` take `count` (alias: `max` for `get_recent_logs`).

### `editor.get_crash_context`

Get last crash/ensure context. *No parameters.*

### `editor.capture_scene_preview`

Render an asset in a preview scene and screenshot it. Supported `asset_type` values are `niagara`, `material`, `static_mesh`, `skeletal_mesh`, and `widget`.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `asset_path` | string | **required** | Asset to preview |
| `asset_type` | string | **required** | `niagara`, `material`, `static_mesh`, `skeletal_mesh`, or `widget` |
| `preview_mesh` | string | optional | For materials: `plane`, `sphere`, `cube`. Default: `plane` |
| `animation_path` | string | optional | Skeletal mesh only: animation sequence used for posed capture |
| `seek_time` | number | optional | Niagara sim time or skeletal animation seek time in seconds. Default: `0.0` |
| `scale` | number | optional | Widget only: DPI multiplier. Default: `1.0` |
| `camera` | object | optional | `{location:[x,y,z], rotation:[p,y,r], fov:60}` |
| `resolution` | array | optional | `[width, height]`. Default: `[512, 512]` |
| `output_path` | string | optional | Output PNG path |

### `editor.capture_sequence_frames`

Capture multiple frames at specified timestamps. Same params as `capture_scene_preview` plus `timestamps[]`, `output_dir`, `filename_prefix`, `persistent`.

### `editor.import_texture`

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `source_path` | string | **required** | Absolute path to source image (PNG, TGA, EXR, HDR). Aliases: `source_file`, `file_path`, `path` |
| `destination` | string | **required** | UE asset path. Aliases: `dest_path`, `destination_path`; with `asset_name`, this may be a destination folder |
| `asset_name` | string | optional | Asset name to append when `destination`/`destination_path` is a folder |
| `settings` | object | optional | `{compression, srgb, tiling, max_size, lod_group}`; `compression` accepts TC_* names plus `UI`/`UserInterface2D` aliases |
| `replace_existing` | bool | optional | Overwrite existing destination asset. Default: `false` |
| `overwrite_policy` | string | optional | Compatibility alias: `overwrite`/`replace` -> `replace_existing=true`; `fail`/`unique` -> false |

### `editor.stitch_flipbook`

Stitch frame PNGs into a flipbook atlas. Used by the VFX training harness.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `frame_paths` | array | **required** | Ordered absolute paths to frame PNGs |
| `dest_path` | string | **required** | UE asset path for output texture |
| `grid` | array | **required** | `[columns, rows]` grid layout |
| `srgb` | bool | optional | sRGB color space. Default: `true` |
| `no_mipmaps` | bool | optional | Disable mipmaps to prevent atlas bleed. Default: `true` |
| `delete_sources` | bool | optional | Delete source PNGs after stitch. Default: `true` |
| `lod_group` | string | optional | Default: `TEXTUREGROUP_Effects` |

> **Experimental flag.** Designed for the VFX training harness. Treat as best-effort.

> `asset.delete_assets` is owned by `MonolithAsset`, not the `editor` namespace. See the canonical [`asset.delete_assets`](#assetdelete_assets) section for its package-segment prefix guard, force-mode disk-residual handling, and per-target postconditions.

### `editor.get_viewport_info`

Current editor viewport camera position, rotation, FOV, resolution. *No parameters.*

### `editor.capture_system_gif`

Capture a Niagara system as ordered PNG frames and, by default, encode a GIF by trying ffmpeg first and python imageio second. The action records the actual fps, any adaptive degradation, all frame paths, and GIF/encoder status. The encoded GIF is an opaque RGB visual-review artifact: UE/Slate frames with zero or undefined alpha are normalized at the encoder boundary without modifying the original PNG evidence. The ffmpeg and python paths apply the same opacity contract. Because GIF stores time in 10 ms units, the python path rounds cumulative frame boundaries and emits a per-frame millisecond schedule instead of truncating `1000 / fps`; examples are `20 fps -> [50,...]`, `30 fps -> [30,40,30,...]`, and `60 fps -> [20,10,20,20,10,20,...]`. The response keeps `gif_duration_seconds` as the backward-compatible nominal `frame_count / fps` value and labels it with `gif_duration_kind`; Python output additionally reports `encoded_gif_duration_seconds`, `gif_duration_quantization_error_seconds`, `gif_delay_unit_ms=10`, and `gif_timing_mode="cumulative_centisecond_rounding"`.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `asset_path` | string | **required** | Niagara system asset path |
| `duration_seconds` | number | optional | Default: `2.0` |
| `fps` | integer | optional | Target fps. Default: `60`; adaptively degrades to `30` and then `20` under resource limits; `20` is the quality floor unless the hard frame cap forces degradation. |
| `resolution` | integer | optional | Default: `256` |
| `output_path` | string | optional | Default: `Saved/Screenshots/Monolith/GIF_<timestamp>` |
| `encoder` | string | optional | `auto` (default), `frames_only`, `ffmpeg`, or `python` |

### `editor.encode_frame_sequence_gif`

Encode already-captured ordered PNG frames into a GIF without recapturing editor content. Use this for PIE screenshot/frame automation after the PNG frames already exist. The default target is 60 fps; the action preserves source duration while downsampling frames and degrades under resource pressure to 30 fps, 20 fps, or a lower reported rate only when the hard frame budget requires it. The output GIF is always an opaque RGB visual-review artifact: both ffmpeg and python normalize UE/Slate zero or undefined alpha during encoding, leave every source PNG unchanged, and use the selected fps as the timing contract. The python encoder converts that timing to a per-frame 10 ms schedule by rounding cumulative frame boundaries, which prevents the drift caused by independently truncating every `1000 / fps` delay. `gif_duration_seconds` and `nominal_gif_duration_seconds` report the nominal frame-count duration for compatibility; `gif_duration_kind` makes that semantic explicit, while Python output reports the exact scheduled `encoded_gif_duration_seconds` and its quantization error separately.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `frame_paths` | array | optional | Ordered absolute or project-relative PNG frame paths. Required when `frame_dir` is omitted. |
| `frame_dir` | string | optional | Directory containing PNG frames; matched files are sorted lexically and may be combined with `frame_paths`. |
| `pattern` | string | optional | Filename glob for `frame_dir`. Default: `*.png`. |
| `output_path` | string | optional | Output GIF path. Default: beside `frame_dir` or under `Saved/Screenshots/Monolith`. |
| `fps` | integer | optional | Target GIF fps. Default: `60`; adaptively degrades to `30` and then `20` under resource limits. |
| `source_fps` | integer | optional | FPS represented by the input frames. Defaults to `fps`; set this when converting a higher-rate PNG capture to a lower-rate GIF while preserving duration. |
| `max_frames` | integer | optional | Caller frame cap, clamped to the 1000-frame hard cap and the adaptive resource budget. |
| `scale_width` | integer | optional | Optional GIF output width. Requires ffmpeg; omitted preserves source frame size. |
| `encoder` | string | optional | `auto` (default), `frames_only`, `ffmpeg`, or `python` |

### `editor.create_empty_map` · NEW in Phase J F8

Create a fully blank `UWorld` asset at the given `/Game/...` path. Saves immediately.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `path` | string | **required** | Asset path under `/Game/...` (e.g. `/Game/Tests/Monolith/Audio/Map_Test`) |
| `map_template` | string | optional | `blank` (default). Reserved: `vr_basic`, `thirdperson_basic` — return error in v1; UE 5.7 templates are populated client-side, not via `UWorldFactory`. |

### `editor.get_module_status` · NEW in Phase J F8

Report plugin-enabled + module-loaded status for Monolith (or arbitrary) modules. Wraps `IPluginManager` + `FModuleManager`.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `module_names` | array | optional | Module name strings. Omit to query all Monolith modules. Unknown names return `enabled=false, loaded=false` (no error). |

### `editor.run_console_command` · NEW in v0.14.10

Execute a console command. Routes to the first PIE `PlayerController` found (so exec UFUNCTIONs on the possessed pawn fire); falls back to `GEngine->Exec` on the editor world when no PIE session is active. Returns which world type was used (`pie` / `editor`) and whether the PC path was taken.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `command` | string | **required** | Console command string (e.g. `BowLoop 1`, `WalkLoop`, `Cam3P 1`) |

### `editor.start_pie` · `editor.stop_pie` · NEW in v0.14.10

`start_pie` queues an in-viewport Play-In-Editor session (refuses to queue a duplicate when a PIE world is already alive); response includes `mode: 'in_viewport'`. `stop_pie` calls `RequestEndPlayMap` when a PIE world exists, no-op (`stopped: false`) otherwise. Both take *no parameters*. Pairs with `run_python` / `load_level` for fully automated in-game test flows.

### `editor.run_python` · NEW in v0.14.9

Execute a Python command, statement, or file via `IPythonScriptPlugin::ExecPythonCommandEx`. Returns success, captured Python stdout/stderr (typed info/warning/error), and (for `evaluate_statement` mode) the evaluated result. Requires `PythonScriptPlugin` (engine-shipped Experimental, enabled by `Monolith.uplugin`).

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `command` | string | **required** | Python source — inline code, single statement, or a file path with optional space-separated args (when `mode=execute_file`). Aliases: `code`, `script` |
| `mode` | string | optional | `execute_file` (default), `execute_statement`, or `evaluate_statement` |
| `unattended` | bool | optional | Set `GIsRunningUnattendedScript=true` to suppress UI dialogs. Default: `false` |
| `file_scope` | string | optional | Scope for `execute_file`: `private` (isolated, default) or `public` (shared with REPL console) |

### `editor.load_level` · NEW in v0.14.9

Close the current persistent level (without saving) and load the specified level by `/Game/...` asset path. Wraps `ULevelEditorSubsystem::LoadLevel`.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `path` | string | **required** | Asset path of the level to load (e.g. `/Game/Maps/L_Backyard`). Must exist. |

---

## build

Operational build helpers registered by `MonolithEditor`. These actions do not replace the project's primary build command; they provide structured, dry-run-first UAT planning and engine-path discovery.

### `build.resolve_unreal_engine`

Resolve the loaded project's Unreal Engine root from the current editor context and return `engine_root`, `engine_dir`, `uat_path`, `ubt_path`, `project_path`, and existence flags. This is read-only and does not use hard-coded alternate engine checkouts.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `project_path` | string | optional | `.uproject` path. Defaults to the loaded editor project. |

### `build.run_buildcookrun`

Build a UAT `BuildCookRun` command from structured parameters. Defaults to `dry_run=true`; launching UAT requires `dry_run=false` and `confirm=true`.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `project_path` | string | optional | `.uproject` path. Defaults to the loaded editor project. |
| `platform` | string | optional | UAT `-platform`. Default: `Win64` |
| `client_config` | string | optional | UAT `-clientconfig`. Default: `Development` |
| `server_config` | string | optional | Optional UAT `-serverconfig` |
| `target` | string | optional | Optional UAT `-target` |
| `map` | string | optional | Optional UAT `-map` |
| `custom_config` | string | optional | Optional `-CustomConfig`, e.g. `EOS` |
| `archive_directory` | string | optional | Optional UAT `-archivedirectory` |
| `build`, `cook`, `stage`, `pak`, `archive` | bool | optional | BuildCookRun switches; archive is used when `archive_directory` is set |
| `additional_args` | array | optional | Extra trusted UAT arguments appended verbatim |
| `wait` | bool | optional | If executing, wait and return exit code plus bounded stdout/stderr tails |
| `dry_run` | bool | optional | Preview command without launching. Default: `true` |
| `confirm` | bool | optional | Required with `dry_run=false` |

---

## artifact

Build and screenshot evidence artifact helpers. File writes/copies are dry-run-first and require explicit confirmation.

### `artifact.package_build_outputs`

Scan a build/archive output directory and return a JSON manifest object. Optional manifest file writing requires `write_manifest=true`, `dry_run=false`, and `confirm=true`.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `archive_dir` | string | **required** | Existing build/archive directory to scan |
| `manifest_path` | string | optional | Manifest output path. Defaults to `<archive_dir>/manifest.json` |
| `recursive` | bool | optional | Scan recursively. Default: `true` |
| `limit` | integer | optional | Maximum files to include. Values outside `1..50000`, fractional numbers, and wrong types are rejected. Default: `5000` |
| `write_manifest` | bool | optional | Write the manifest when confirmed. Default: `false` |
| `dry_run` | bool | optional | Preview without writing. Default: `true` |
| `confirm` | bool | optional | Required for manifest write |

### `artifact.mirror_screenshot_evidence`

Plan or copy screenshot/evidence files to a destination directory. Accepts explicit `files[]` or scans `source_dir` for `png/jpg/jpeg/gif`.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `files` | array | optional | Explicit file paths |
| `source_dir` | string | optional | Directory scanned when `files` is omitted |
| `dest_dir` | string | **required** | Destination directory |
| `limit` | integer | optional | Maximum files to mirror. Values outside `1..10000`, fractional numbers, and wrong types are rejected. Default: `200` |
| `dry_run` | bool | optional | Preview copy rows. Default: `true` |
| `confirm` | bool | optional | Required for copying |

---

## notify

Notification helpers for evidence workflows. Secret values are never accepted as action params.

### `notify.discord_screenshot_evidence`

Build a redacted text-only Discord screenshot-evidence payload from `test_name` and evidence files. Sending requires `send=true`, `dry_run=false`, `confirm=true`, and a webhook URL read from `webhook_env` (default `DISCORD_WEBHOOK_URL`).

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `test_name` | string | **required** | One-line evidence summary |
| `files` | array | optional | Evidence file paths |
| `source_dir` | string | optional | Directory scanned when `files` is omitted |
| `webhook_env` | string | optional | Environment variable containing the webhook URL. Default: `DISCORD_WEBHOOK_URL` |
| `send` | bool | optional | Actually send the notification. Default: `false` |
| `dry_run` | bool | optional | Preview payload without sending. Default: `true` |
| `confirm` | bool | optional | Required for sending |

---

## console

Live `IConsoleManager` registry discovery and EngineSource.db snapshot search. The namespace treats Unreal console variables and console commands as `IConsoleObject` rows, so it can find both cvars (`r.ShadowQuality`) and command-only entries (`stat`, custom `FAutoConsoleCommand`, exec-style commands whose token is registered). Existing `config.get_cvar` / `config.find_cvars` remain the compatibility path for CVar-only config workflows. Runtime-only actions return `status:"live_only"` in offline CLI instead of pretending SQLite can resolve live registry, log, world, capture, or CVar state.

### `console.list_live_objects`

List live registered console objects from `IConsoleManager`.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `query` | string | optional | Prefix or substring to match against console object names |
| `mode` | string | optional | `prefix` or `contains`. Default: `prefix` |
| `object_type` | string | optional | `all`, `variable`, or `command`. Default: `all` |
| `limit` | integer | optional | Maximum rows, clamped to 1..5000. Default: `100` |
| `include_values` | bool | optional | Include current variable values. Default: `true` |
| `include_defaults` | bool | optional | Include variable defaults. Default: `true` |

### `console.refresh_snapshot`

Refresh `EngineSource.db` tables `console_objects`, `console_objects_fts`, and `console_snapshot_meta` from the live registry. This is the bridge between live MCP and offline `monolith_query.exe console ...`.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `query` | string | optional | Optional prefix or substring to restrict captured rows. Empty captures every registered object |
| `mode` | string | optional | `prefix` or `contains`. Default: `prefix` |
| `object_type` | string | optional | `all`, `variable`, or `command`. Default: `all` |
| `limit` | integer | optional | Maximum objects to capture. `0` means no explicit cap. Default: `0` |
| `include_values` | bool | optional | Persist current variable values. Default: `true` |
| `include_defaults` | bool | optional | Persist variable defaults. Default: `true` |

### `console.search_objects`

Search the latest snapshot with FTS5.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `query` | string | optional | FTS query over name, type, help, values, variable type, and set-by source. Empty lists rows by name |
| `object_type` | string | optional | `all`, `variable`, or `command`. Default: `all` |
| `limit` | integer | optional | Maximum rows. Compact projection is capped at 500 rows; full/detail projection is capped at 200 rows. Default: `100` |

Returns rows in `results` only, plus `returned_count`, `requested_limit`, normalized `limit`, `offset`, `projection`, `detail`, `truncated`, and optional `next_cursor`. The legacy duplicate `objects` row array is intentionally omitted to avoid payload amplification.

### `console.get_object`

Get one console object by exact name from the snapshot or live registry.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `name` | string | **required** | Exact console object name |
| `source` | string | optional | `snapshot` or `live`. Default: `snapshot` |
| `include_values` | bool | optional | For `source=live`, include current variable value. Default: `true` |
| `include_defaults` | bool | optional | For `source=live`, include variable default value. Default: `true` |

### `console.health`

Report console snapshot schema, FTS parity, and last refresh metadata.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `include_counts` | bool | optional | Include row/FTS parity counts. Default: `false` |

### `console.execute`

Execute a live console command. Routes to the first PIE `PlayerController` when available and falls back to `GEngine->Exec` on the editor world. Offline `monolith_query.exe console execute ...` returns live-only guidance and never executes.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `command` | string | **required** | Console command string, including optional arguments |
| `dry_run` | bool | optional | Validate and report routing without executing. Default: `true` |
| `require_known_object` | bool | optional | Require the first token to resolve in `IConsoleManager`. Default: `false` |
| `target_world` | string | optional | `auto`, `pie`, or `editor`. Use `pie` to fail instead of falling back when no live game world exists. Default: `auto` |

### `console.resolve_command`

Resolve a console command line against the live registry and target-world route without executing the command. Offline `monolith_query.exe console resolve_command ...` returns live-only guidance and never invents live registry state from SQLite.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `command` | string | **required** | Console command string to inspect |
| `target_world` | string | optional | Route hint: `auto`, `pie`, or `editor`. Default: `auto` |
| `include_values` | bool | optional | Include current variable value when the first token is a CVar. Default: `true` |
| `include_defaults` | bool | optional | Include default variable value when available. Default: `true` |

### `console.get_log_cursor`

Return the current live log-capture cursor. The cursor is a monotonic editor-session sequence used to isolate logs emitted after a command.

No parameters.

### `console.search_logs_since`

Search only logs emitted after a cursor.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `cursor` | number | **required** | Cursor returned by `console.get_log_cursor` or a prior console verification action |
| `pattern` | string | optional | Case-insensitive substring match against log message |
| `category` | string | optional | Exact log category filter |
| `verbosity` | string | optional | Maximum verbosity: `fatal`, `error`, `warning`, `display`, `log`, `verbose`, `very_verbose`. Default: `very_verbose` |
| `limit` | integer | optional | Maximum entries, clamped to 1..2000. Default: `200` |

### `console.execute_and_expect`

Execute one console command and evaluate expected/rejected post-command log patterns. Expectation failures return `passed=false` in the result instead of becoming transport errors.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `command` | string | **required** | Console command string, including optional arguments |
| `dry_run` | bool | optional | Validate routing without executing. Default: `false` |
| `require_known_object` | bool | optional | Require first token registration. Default: `false` |
| `target_world` | string | optional | `auto`, `pie`, or `editor`. Default: `auto` |
| `expect_log` | string | optional | Single expected post-command log substring |
| `expect_logs` | array | optional | Expected strings or objects `{pattern, category?, min_count?, max_count?}` |
| `reject_log` | string | optional | Single rejected post-command log substring |
| `reject_logs` | array | optional | Rejected strings or objects `{pattern, category?}` |
| `settle_ms` | integer | optional | Wait after execution before log collection. Default: `100`, max `5000` |
| `log_limit` | integer | optional | Maximum post-command logs to inspect/return. Default: `200`, max `2000` |

### `console.wait_for_log`

Bounded wait over post-cursor logs. It polls live Monolith log capture until expected patterns pass, a rejected pattern appears, or a timeout expires, returning structured match evidence rather than forcing callers to hand-roll retry loops. Offline `monolith_query.exe console wait_for_log ...` returns live-only guidance.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `cursor` | number | **required** | Cursor returned by `console.get_log_cursor` or a prior console verification action |
| `pattern` | string | optional | Shortcut expected log substring |
| `expect_log` | string | optional | Single expected log substring |
| `expect_logs` | array | optional | Expected strings or objects `{pattern, category?, min_count?, max_count?}` |
| `reject_log` | string | optional | Single rejected log substring |
| `reject_logs` | array | optional | Rejected strings or objects `{pattern, category?}` |
| `mode` | string | optional | `expect` or `assert_absent`. Reject-only calls require `assert_absent`; when no rejected log appears for the full window the result is `status="absent"`, `passed=true`, `observed_full_window=true` |
| `category` | string | optional | Default category applied to expectations without their own category |
| `verbosity` | string | optional | Maximum verbosity: `fatal`, `error`, `warning`, `display`, `log`, `verbose`, `very_verbose`. Default: `very_verbose` |
| `timeout_ms` | integer | optional | Wait budget before returning a miss. Default: `3000`, max `30000` |
| `poll_interval_ms` | integer | optional | Polling cadence. Default: `100`, clamped to `25..1000` |
| `log_limit` | integer | optional | Maximum post-cursor logs to inspect/return. Default: `200`, max `2000` |

### `console.run_sequence`

Run up to 100 console command strings or step objects with per-step cursors and expectations. The response is the sequence evidence artifact: every step reports the command, cursor window, execution payload or error, expectation report, log count, and returned post-step logs.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `commands` | array | **required** | Command strings or step objects `{command, dry_run?, require_known_object?, target_world?, expect_log?, expect_logs?, reject_log?, reject_logs?, settle_ms?, log_limit?, capture?, capture_command?, capture_wait_ms?, capture_output_path?}` |
| `dry_run` | bool | optional | Default dry-run value for steps. Default: `false` |
| `require_known_object` | bool | optional | Default known-object guard for steps. Default: `false` |
| `target_world` | string | optional | Default target world: `auto`, `pie`, or `editor`. Default: `auto` |
| `abort_on_failure` | bool | optional | Stop after first failed execution or expectation. Default: `true` |
| `settle_ms` | integer | optional | Default wait after each command. Default: `100`, max `5000` |
| `log_limit` | integer | optional | Maximum per-step post-command logs. Default: `200`, max `2000` |
| `artifact_dir` | string | optional | Write `manifest.json` and `logs.jsonl`; relative paths resolve under the project root |

### `console.execute_and_capture`

Execute a command, run a screenshot console command such as `HighResShot 1920x1080`, and report the newly created or modified PNG path. It does not fall back to editor viewport capture. When the screenshot must complete on a later game tick, the result is `status="capture_pending"` with `capture_id`; poll `console.poll_capture` until completion.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `command` | string | **required** | Console command to execute before capture |
| `capture_command` | string | optional | Screenshot console command. Default: `HighResShot 1920x1080` |
| `output_path` | string | optional | Optional path to copy the captured PNG to; relative paths resolve under the project root |
| `require_known_object` | bool | optional | Require the first token of `command` to resolve in `IConsoleManager`. Default: `false` |
| `target_world` | string | optional | `auto`, `pie`, or `editor`. Default: `auto` |
| `settle_ms` | integer | optional | Wait between command and capture. Default: `250`, max `5000` |
| `capture_wait_ms` | integer | optional | Wait for the screenshot file after capture. Default: `120000`, max `240000` |

### `console.poll_capture`

Poll a pending `execute_and_capture` or `run_sequence` step capture. Completed results return `captured`, `capture_not_found`, or `copy_failed`.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `capture_id` | string | **required** | Capture id returned when a capture result has `status="capture_pending"` |
| `consume` | bool | optional | Remove the completed pending-capture record after reading. Default: `false` |

### `console.diagnose_failure`

Classify a failed console result and suggest focused next actions, including log timeouts, rejected logs, pending captures, capture failures, artifact write failures, missing worlds, and unknown commands.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `result` | object | **required** | Result object from `execute_and_expect`, `run_sequence`, `wait_for_log`, `execute_and_capture`, or `poll_capture` |

### `console.set_cvar_scoped`

Temporarily set one or more live CVars, run a console sequence, and restore the original values and original set-by priority before returning. The action preflights every requested CVar before mutating any value and returns structured `cvars[]` validation/restore reports.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `cvars` | object | **required** | Map of console variable name to temporary string/number/bool value |
| `commands` | array | **required** | Command strings or step objects to run while CVars are applied |
| `dry_run` | bool | optional | Default dry-run value for command steps. Default: `false` |
| `require_known_object` | bool | optional | Default known-object guard for command steps. Default: `false` |
| `target_world` | string | optional | Default target world: `auto`, `pie`, or `editor`. Default: `auto` |
| `abort_on_failure` | bool | optional | Stop command sequence after first failed step. Default: `true` |
| `settle_ms` | integer | optional | Default wait after each command. Default: `100`, max `5000` |
| `log_limit` | integer | optional | Maximum per-step logs. Default: `200`, max `2000` |
| `artifact_dir` | string | optional | Forwarded to `run_sequence` artifact writing |

Offline examples:

```powershell
Binaries\monolith_query.exe console health --include-counts=true
Binaries\monolith_query.exe console search_objects r.Shadow --limit=10
Binaries\monolith_query.exe console get_object r.ShadowQuality
Binaries\monolith_query.exe console resolve_command Project.Debug.DumpState
Binaries\monolith_query.exe console wait_for_log --cursor=42 --pattern=Project.Debug
Binaries\monolith_query.exe console poll_capture --capture-id=abc123
Binaries\monolith_query.exe console diagnose_failure
Binaries\monolith_query.exe console set_cvar_scoped
```

---

## config

INI config file inspection and search. **11 actions.** Read-only (except `set_developer_setting`).

### `config.resolve_setting`

Get effective value of a config key across the full INI hierarchy.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `file` | string | optional | Config category (e.g. `Engine`, `Game`, `Input`). When omitted, the section/key is searched across `Engine`, `Game`, `Input`, `Editor`, `Scalability`, `GameUserSettings`; the response reports the matched `category` and the `searched_categories` list |
| `section` | string | **required** | Config section (e.g. `/Script/Engine.RendererSettings`) |
| `key` | string | **required** | Config key name |

### `config.explain_setting`

Show where a config value comes from across Base → Default → User layers.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `file` | string | optional | Config category |
| `section` | string | optional | Config section |
| `key` | string | optional | Config key name |
| `setting` | string | optional | Convenience: search for this key across common categories |

### `config.diff_from_default`

Show project config overrides vs engine defaults for a category.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `file` | string | **required** | Config category to diff |
| `section` | string | optional | Filter to a specific section |

### `config.search_config`

Full-text search across all config files.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `query` | string | **required** | Search text |
| `category` | string | optional | Filter to a config category |

### `config.get_section`

Read an entire config section from a specific file.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `file` | string | **required** | Config file name or category |
| `section` | string | **required** | Section name |

### `config.get_config_files`

List all config files with their hierarchy level.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `category` | string | optional | Filter to a specific category |

### `config.list_plugins`

List discovered plugins with enabled state and descriptor metadata. Read-only.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `name_contains` | string | optional | Case-insensitive plugin name substring filter |
| `enabled_only` | boolean | optional | Only return enabled plugins. Default: `false` |

### `config.get_plugin`

Get descriptor metadata for one discovered plugin. Read-only.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `name` | string | **required** | Plugin name |

### `config.get_cvar`

Get one console variable value and flags. Read-only.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `name` | string | **required** | Console variable name |

### `config.find_cvars`

Find console variables by prefix or substring. Read-only.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `query` | string | optional | Prefix or substring to search for |
| `mode` | string | optional | `prefix` or `contains`. Default: `prefix` |

### `config.set_developer_setting`

DEV-ONLY (write): set a property on a UDeveloperSettings CDO at runtime. Resolves a settings class by short-name or full path, parses `value` via UProperty::ImportText_Direct, and optionally persists back to the INI via SaveConfig(). `#if WITH_EDITOR`-gated.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `class` | string | **required** | Settings class short-name or full path |
| `property` | string | **required** | Property name to mutate |
| `value` | string | **required** | New value (ImportText format) |
| `save_config` | boolean | optional | If true, calls SaveConfig() on the CDO to persist the change. Default: `false` |

---

## localization

Localization culture and StringTable asset management.

### `localization.list_cultures`

List available cultures known to Unreal internationalization.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `culture_names` | array | optional | Optional culture names to resolve; omitted returns configured/default culture context |
| `include_derived` | boolean | optional | Include derived cultures when resolving culture_names. Default: `true` |

### `localization.list_string_tables`

List StringTable assets under a project content path.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `path` | string | optional | Content path to scan. Default: `/Game` |
| `include_entries` | boolean | optional | Include capped entry rows. Default: `false` |
| `include_metadata` | boolean | optional | Include per-entry metadata when entries are included. Default: `false` |
| `limit` | integer | optional | Maximum tables or entries to return. Default: `100` |

### `localization.get_string_table`

Inspect a StringTable asset and return capped entries.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `asset_path` | string | **required** | StringTable asset path |
| `include_metadata` | boolean | optional | Include per-entry metadata. Default: `true` |
| `limit` | integer | optional | Maximum entries to return. Default: `200` |

### `localization.validate_string_table`

Validate a StringTable asset for empty keys, empty strings, duplicate-looking keys, and large output warnings.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `asset_path` | string | **required** | StringTable asset path |

### `localization.create_string_table`

Create a StringTable asset under /Game. Requires dry_run=true or confirm=true.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `asset_path` | string | **required** | New StringTable asset path |
| `namespace` | string | optional | StringTable namespace; defaults to asset name |
| `dry_run` | boolean | optional | Preview without writing. Default: `false` |
| `confirm` | boolean | optional | Required true for non-dry-run writes. Default: `false` |
| `save` | boolean | optional | Save the package after creation. Default: `false` |

### `localization.set_string_entry`

Add or replace one StringTable entry and optional metadata. Requires dry_run=true or confirm=true.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `asset_path` | string | **required** | StringTable asset path |
| `key` | string | **required** | Entry key |
| `source_string` | string | **required** | Source string |
| `metadata` | object | optional | String metadata fields |
| `dry_run` | boolean | optional | Preview without writing. Default: `false` |
| `confirm` | boolean | optional | Required true for non-dry-run writes. Default: `false` |
| `save` | boolean | optional | Save the package after mutation. Default: `false` |

### `localization.remove_string_entry`

Remove one StringTable entry by key. Requires dry_run=true or confirm=true.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `asset_path` | string | **required** | StringTable asset path |
| `key` | string | **required** | Entry key |
| `dry_run` | boolean | optional | Preview without writing. Default: `false` |
| `confirm` | boolean | optional | Required true for non-dry-run writes. Default: `false` |
| `save` | boolean | optional | Save the package after mutation. Default: `false` |

### `localization.set_string_metadata`

Add, replace, or remove metadata on one StringTable entry. Requires dry_run=true or confirm=true.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `asset_path` | string | **required** | StringTable asset path |
| `key` | string | **required** | Entry key |
| `metadata_key` | string | **required** | Metadata key |
| `metadata_value` | string | optional | Metadata value to set |
| `remove` | boolean | optional | Remove metadata_key instead of setting metadata_value. Default: `false` |
| `dry_run` | boolean | optional | Preview without writing. Default: `false` |
| `confirm` | boolean | optional | Required true for non-dry-run writes. Default: `false` |
| `save` | boolean | optional | Save the package after mutation. Default: `false` |

### `localization.import_string_table_csv`

Import key,source_string,metadata CSV rows into a StringTable. Requires dry_run=true or confirm=true.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `asset_path` | string | **required** | StringTable asset path |
| `file_path` | string | **required** | CSV path under the project directory |
| `replace_existing` | boolean | optional | Clear existing entries before import. Default: `false` |
| `dry_run` | boolean | optional | Preview without writing. Default: `false` |
| `confirm` | boolean | optional | Required true for non-dry-run writes. Default: `false` |
| `save` | boolean | optional | Save the package after mutation. Default: `false` |

### `localization.export_string_table_csv`

Export a StringTable to CSV under the project directory. Requires dry_run=true or confirm=true.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `asset_path` | string | **required** | StringTable asset path |
| `file_path` | string | **required** | Destination CSV path under the project directory |
| `include_metadata` | boolean | optional | Include metadata columns. Default: `true` |
| `dry_run` | boolean | optional | Preview without writing. Default: `false` |
| `confirm` | boolean | optional | Required true for non-dry-run writes. Default: `false` |

## collection

Content Browser collection CRUD, dynamic query management, and asset association. Backed by the CollectionManager module.
See `Plugins/Monolith/Docs/specs/SPEC_MonolithIndex.md` for the deep dive.

---

## project

Project-wide asset index backed by SQLite + FTS5. Base discovery/search actions plus CRG-inspired review actions over the existing `assets` and `dependencies` tables.

### `project.search`

Full-text search across all indexed project assets, nodes, variables, and parameters. (Returns an MCP error `-32000` if the index database is offline or indexing is currently in progress).

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `query` | string | **required** | FTS search query (automatically escaped and tokenized for prefix matching) |
| `limit` | integer | optional | Default: `50` |
| `include_content` | boolean | optional | Include variable/parameter/DataTable/actor/supplemental matches for discovery only. Default: `true` |
| `asset_class` | string | optional | Scope results to this exact asset class (e.g. `Blueprint`, `WidgetBlueprint`). Empty = any |
| `path_filter` | string | optional | Scope results to package paths containing this substring (e.g. `/Game/Combat`). Empty = any |

Results are ranked by bm25 column weighting (name >> body) fused across FTS tables via RRF, with a de-spaced CamelCase streak superset and an `identifier_split` supplemental value for CamelCase/snake token recall.

### `project.find_references`

Find all assets that reference or are referenced by the given asset.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `asset_path` | string | **required** | Package path |

### `project.find_by_type`

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `asset_type` | string | **required** | Asset class name (e.g. `Blueprint`, `Material`, `StaticMesh`, `Texture2D`) |
| `module` | string | optional | Filter by plugin/module name |
| `limit` | integer | optional | Default: `100` |
| `offset` | integer | optional | Pagination. Default: `0` |

### `project.get_stats`

Project index stats — total counts by table and asset class breakdown. *No parameters.*

### `project.get_asset_details`

Deep details for a specific asset — nodes, variables, parameters, dependencies.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `asset_path` | string | **required** | Package path |

### `project.list_gameplay_tags`

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `prefix` | string | optional | Tag prefix filter (e.g. `Weapon.Melee`) |

### `project.search_gameplay_tags`

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `query` | string | **required** | Substring to search for in tag names |

### `project.health`

Default health is shallow. `include_counts=true` also checks CRG projection count parity and warns when `assets`, `crg_nodes`, and `crg_node_metrics` diverge.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `include_counts` | bool | optional | Include row counts and CRG parity checks. Default: `false` |
| `include_deep_checks` | bool | optional | Run deeper FTS/orphan/projection checks without row-count output. Default: `false` |

### `project.repair_crg_cache`

Dry-run by default. `execute=true` rebuilds disposable `crg_nodes`, `crg_edges`, `crg_node_metrics`, and `crg_meta` from authoritative `assets` and `dependencies` tables, gated by indexing state.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `execute` | bool | optional | Apply the repair. Default: `false` |

### `project.risk_score`

Returns cached or query-time risk `{score,tier,reasons[],raw_counts,cache}`. Scoring v3 uses token-boundary/camel-case aware UE-domain sensitivity, so `Design` and `Assignment` do not match signing risk while `Signature`, `Crypto`, and `Hash` remain positive controls.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `asset_path` | string | **required** | Asset package path |
| `limit` | integer | optional | Default: `20` |
| `min_tier` | string | optional | `low`, `medium`, or `high`. Default: `low` |

### `project.review_context`

Token-efficient project review package: seed asset, bounded dependency impact, top risk rows, compact context, truncation state, and next actions.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `asset_path` | string | **required** | Asset package path |
| `direction` | string | optional | `in`, `out`, or `both`. Default: `both` |
| `max_depth` | integer | optional | Default: `2` |
| `max_results` | integer | optional | Default: `200` |
| `detail_level` | string | optional | `minimal` or `standard`. Default: `minimal` |

---

## source

Unreal Engine C++ source code navigation. 1M+ symbols indexed. **13 actions** (12 navigation + 1 Reflection Intelligence audit registered cross-namespace in v0.17.0).

### `source.read_source`

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `symbol` | string | **required** | Class, function, or struct name. Aliases: `query`, `name`, `path`; path-like values delegate to `source.read_file` |
| `include_header` | bool | optional | Include the header declaration. Default: `false` |
| `max_lines` | integer | optional | Default: `500` |
| `members_only` | bool | optional | Only show class members. Default: `false` |
| `start_line` | integer | optional | First line when `symbol`/`path` is a source file path. Default: `1` |
| `end_line` | integer | optional | Last line when `symbol`/`path` is a source file path |
| `line_count` | integer | optional | Number of file lines when `end_line` is omitted |

### `source.find_references` · `source.find_callers` · `source.find_callees`

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `symbol` | string | **required** | Symbol or function name. `find_callers` and `find_callees` also accept `query` as a validated alias for this value. |
| `ref_kind` | string | optional | (`find_references` only) Filter by reference kind |
| `limit` | integer | optional | Default: `50` |

`source.find_references` returns `match_status=no_symbol`, `count=0`, and `next_actions` when the requested symbol is not indexed. Use `source.search_source` to discover the indexed spelling before retrying.

### `source.search_source`

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `query` | string | **required** | Search query |
| `scope` | string | optional | `all`, `engine`, `shaders` |
| `mode` | string | optional | `fts`, `regex`, `exact` |
| `module` | string | optional | Filter to a specific module |
| `path_filter` | string | optional | File path pattern |
| `symbol_kind` | string | optional | `class`, `function`, `enum`, etc. |
| `limit` | integer | optional | Default: `50` |

### `source.get_class_hierarchy`

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `symbol` | string | **required** | Class name |
| `direction` | string | optional | `up` (parents), `down` (children), or `both`. Default: `both` |
| `depth` | integer | optional | Default: `5` |

### `source.get_module_info`

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `module_name` | string | **required** | Module name |

### `source.get_symbol_context`

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `symbol` | string | **required** | Symbol name |
| `context_lines` | integer | optional | Default: `10` |

### `source.read_file`

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `file_path` | string | **required** | Source file path. Alias: `path` |
| `start_line` | integer | optional | Default: `1` |
| `end_line` | integer | optional | Default: end of file |
| `line_count` | integer | optional | Number of lines when `end_line` is omitted |
| `max_lines` | integer | optional | Alias for `line_count` when `end_line` is omitted |

### `source.trigger_reindex` · `source.trigger_project_reindex`

`trigger_reindex` does a full clean build (engine + shaders + project). `trigger_project_reindex` is incremental (project Source/ + Plugins/ only). Both take *no parameters*. Offline `monolith_query.exe source trigger_project_reindex` returns live-only guidance instead of an unknown-action error; actual indexing still requires the editor-backed MCP action.

### `source.impact_radius`

Bounded BFS over call/type references, inheritance and function overrides: who is impacted within N hops.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `symbol` | string | **required** | Seed symbol name |
| `edge_kinds` | string | optional | `call\|type\|inheritance` by default; append `\|override` or use `find_overrides` for virtual/override function edits |
| `direction` | string | optional | `in`, `out`, or `both`. Default: `both` |
| `max_depth` | integer | optional | Max traversal hops. Default: `2` |
| `max_results` | integer | optional | Max impacted symbols. Default: `200` |

### `source.find_overrides`

Override-only traversal for virtual/function symbols. The default `detail_level=minimal` keeps `overrides[]`, reports `edge_count`, samples `edges[]`, and omits the duplicate `impacted_symbols` array. Use `detail_level=standard` for full edge arrays and the full impact payload.

The response carries the common list-projection contract next to the legacy fields: `total`, `returned`, `next_cursor` (the next `offset`, present while more rows remain), and a `projection` echo (`detail`, `offset`, `max_results`, `fields`). `total` counts the rows emitted by the current fetch window (`offset + max_results`), not the full population — it grows as later pages fetch deeper, so treat the absence of `next_cursor` as the completion signal. Cursor order is stable while the source index is unchanged; the traversal cannot page past the 2000-row emission cap (a warning is emitted when the cap is hit). The offline CLI mirrors the contract with `--offset`/`--fields` and a 1000-row cap; offline rows expose `line_start`/`line_end`/`signature` instead of the live `line`.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `symbol` | string | **required** | Function symbol, preferably qualified |
| `direction` | string | optional | `in`, `out`, or `both`. Default: `both` |
| `max_depth` | integer | optional | Default: `2` |
| `max_results` | integer | optional | Max override rows per page. Default: `200` |
| `detail_level` | string | optional | `minimal` or `standard`. Default: `minimal` |
| `offset` | integer | optional | Skip the first N override rows; chain pages through `next_cursor`. Default: `0` |
| `cursor` | string | optional | Pagination cursor from a prior `next_cursor`; takes precedence over `offset` |
| `fields` | array\|string | optional | Project override rows to these fields (array or comma-separated): `id`, `name`, `qualified_name`, `kind`, `file`, `path_status`, `line`, `depth`. Unknown names are ignored with a warning |

### `source.health`

Default `source.health` is a fast shallow check. It validates required tables, schema metadata, journal mode, triggers, and CRG structure without large row-count/parity scans.

The result includes `maintenance_recommendation` with explicit `maintenance_required`, `expensive_maintenance_required`, `reindex_required`, `repair_fts_required`, `repair_crg_cache_required`, `repair_override_edges_required`, `deep_health_ran`, `routine_deep_health_recommended`, and `reason_codes`. When a shallow result is healthy, deep health is reported only as `maintenance_recommendation.optional_diagnostic_actions`, not as a required `next_actions` entry.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `include_counts` | bool | optional | Include row counts and run deep parity checks. Default: `false` |
| `include_deep_checks` | bool | optional | Run expensive orphan-symbol, orphan-reference, FTS parity, and CRG row-parity checks without row-count output. Default: `false` |

### `source.repair_crg_cache`

Dry-run by default. `execute=true` is freshness-gated: if CRG projection counts, helper indexes, and cache metadata are already current, the action returns `skipped=true` and does not rebuild.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `scope` | string | optional | `all` or `override_edges`. Default: `all` |
| `execute` | bool | optional | Apply the repair only when freshness checks report `repair_needed=true`. Default: `false` |

### `source.search_crg_graph`

Searches the optional `Saved\graph.db` export. Results expose `path_status=known|missing`, prefer known source paths, and suppress duplicate missing-path rows for the same symbol identity. Use this action for explicit graph-export/search needs; routine review should prefer `source.search_source`, `source.risk_score`, and `source.review_context`.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `query` | string | **required** | Symbol/query text |
| `kind` | string | optional | Optional node kind filter |
| `limit` | integer | optional | Default: `20` |
| `graph_db` | string | optional | Non-default graph DB path |

### `source.risk_score`

Returns cached or query-time risk `{score,tier,reasons[],raw_counts,cache}`. Scoring v3 uses token-boundary/camel-case aware UE-domain sensitivity and adds query-time override fan-out/overridden-parent counts.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `symbol` | string | **required** | Symbol name, preferably qualified when ambiguous |
| `limit` | integer | optional | Default: `10` |
| `min_tier` | string | optional | `low`, `medium`, or `high`. Default: `low` |

### `source.review_context`

Token-efficient source review package: seed symbol, risk, impact summary, compact context, truncation state, and next actions. Seed/context rows include `path_status=known|missing`; known-path rows are preferred over missing-path duplicates.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `symbol` | string | **required** | Symbol name, preferably qualified when ambiguous |
| `direction` | string | optional | `in`, `out`, or `both`. Default: `both` |
| `max_depth` | integer | optional | Default: `2` |
| `max_results` | integer | optional | Default: `200` |
| `detail_level` | string | optional | `minimal` or `standard`. Default: `minimal` |

### `source.audit_module_dep_reality`

**New v0.17.0 (Reflection Intelligence, Phase 2).** Catches the LNK2019 bug class where a UPROPERTY (or any reflection-touching declaration) references a foreign-module type whose owning module is missing from the declaring module's `Build.cs` `Private/PublicDependencyModuleNames`. UHT generates `Z_Construct_*_NoRegister` calls that link against the foreign module's API macro at link time, so the failure surfaces as a confusing unresolved external. The audit regex-parses every `*.Build.cs` for declared deps, extracts type-bearing reflection declarations from every `*.h` / `*.cpp`, resolves each type against `EngineSource.db`'s symbol → owning-module mapping, and emits a violation when the owning module isn't declared and isn't on the implicit-deps whitelist (`Core`, `CoreUObject`, `Engine`, `Projects`, `RHI`, `RenderCore`). Read-only, idempotent, cursor-paginated. Owned by `MonolithReflectionIntel` but registered onto `source` for caller ergonomics.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `module_filter` | string | optional | Substring match against the **declaring** module's name. Empty scans all. Default: `""` |
| `include_whitelist` | bool | optional | When `true`, also reports references to whitelisted implicit-dep modules (debug aid). Default: `false` |
| `limit` | integer | optional | Page size. Hard cap `500`. Default: `100` |
| `cursor` | string | optional | Opaque base64+JSON cursor from a prior `next_cursor` |

**Returns:** `{ "violations": [ { "declaring_module", "source_path", "source_line", "used_type", "missing_dep" } ], "scanned_modules": N, "scanned_declarations": N, "next_cursor": "<opaque>" }`. Violations are sorted by `(declaring_module, source_path, source_line)`. Multi-argument templates extract only the first argument and typedef aliases aren't chased to the underlying type — both are documented heuristics.

---

## scene

Actor/scene manipulation, spatial queries, volumes, lighting, decals, and editor debug views. (Split from `mesh` on 2026-05-20).

See `Docs/specs/SPEC_MonolithScene.md` for the deep dive.

---

## worldgen

Procedural building/city/facade/roof/terrain generation, floor-plan generation, furnishing, blockout authoring, room/genre templates, asset presets, and context-prop placement. (Split out of `mesh` on 2026-05-20).

See `Docs/specs/SPEC_MonolithWorldGen.md` for the deep dive.

---

## hlod

World Partition Hierarchical Level of Detail (HLOD) layer configuration, builder orchestration, generation stats, and legacy HLOD migration/clearance actions. Registered via the MonolithMesh module.

---

## mesh

Mesh inspection, scene manipulation, spatial queries, level blockout, GeometryScript, procedural geometry, lighting, audio, performance, mesh import (incl. skeletal + animation, PR #58), and **experimental** procedural town generation. **194 actions** (always registered, in the public count) + 45 experimental town gen (gated on `bEnableProceduralTownGen=true`, default `false`) = 239 when town-gen is on.

> For full param schemas, call `describe_query("action_schema", target_namespace="mesh", target_action="<name>")` (or `monolith_discover("mesh", detail=true)`). Plain `monolith_discover("mesh")` is terse — names + one-line descriptions only. The action surface is too broad for full enumeration — see categories below.

**Action categories (core, always registered):**

| Category | Examples |
|----------|----------|
| Mesh inspection | `get_mesh_info`, `get_mesh_bounds`, `get_mesh_materials`, `get_mesh_lods`, `get_mesh_collision`, `get_mesh_uvs`, `analyze_skeletal_mesh`, `analyze_mesh_quality`, `compare_meshes`, `get_vertex_data`, `search_meshes_by_size`, `get_mesh_catalog_stats` |
| Scene actors | `get_actor_info`, `spawn_actor`, `move_actor`, `duplicate_actor`, `delete_actors`, `group_actors`, `set_actor_properties`, `align_actors`, `snap_to_floor`, `manage_folders`, `set_actor_tags` |
| Spatial queries | `query_raycast`, `query_multi_raycast`, `query_radial_sweep`, `query_overlap`, `query_nearest`, `query_line_of_sight`, `get_actors_in_volume`, `get_scene_bounds`, `get_scene_statistics`, `get_spatial_relationships`, `query_navmesh` |
| Blockout | `get_blockout_volumes`, `setup_blockout_volume`, `create_blockout_primitive`, `create_blockout_primitives_batch`, `create_blockout_grid`, `match_asset_to_blockout`, `match_all_in_volume`, `apply_replacement`, `clear_blockout`, `export_blockout_layout`, `import_blockout_layout`, `scan_volume`, `scatter_props`, `create_blockout_blueprint` |
| Level analysis | `analyze_sightlines`, `find_hiding_spots`, `find_ambush_points`, `analyze_choke_points`, `analyze_escape_routes`, `classify_zone_tension`, `analyze_pacing_curve`, `find_dead_ends`, `validate_path_width`, `validate_navigation_complexity`, `analyze_visual_contrast`, `find_rest_points`, `validate_interactive_reach`, `generate_accessibility_report` |
| Performance | `get_region_performance`, `estimate_placement_cost`, `find_overdraw_hotspots`, `analyze_shadow_cost`, `get_triangle_budget`, `analyze_texel_density`, `analyze_material_cost_in_region`, `analyze_lightmap_density`, `find_instancing_candidates`, `convert_to_hism`, `setup_hlod`, `analyze_texture_budget`, `generate_proxy_mesh` |
| Lighting | `place_light`, `set_light_properties`, `sample_light_levels`, `find_dark_corners`, `analyze_light_transitions`, `get_light_coverage`, `suggest_light_placement` |
| Audio | `get_audio_volumes`, `get_surface_materials`, `estimate_footstep_sound`, `analyze_room_acoustics`, `analyze_sound_propagation`, `find_loud_surfaces`, `find_sound_paths`, `can_ai_hear_from`, `get_stealth_map`, `find_quiet_path`, `suggest_audio_volumes`, `create_audio_volume`, `set_surface_type` |
| Decals / scatter | `place_decals`, `place_along_path`, `analyze_prop_density`, `place_storytelling_scene`, `scatter_on_surface`, `scatter_on_walls`, `scatter_on_ceiling`, `randomize_transforms` |
| Encounter design | `analyze_ai_territory`, `evaluate_safe_room`, `predict_player_paths`, `evaluate_spawn_point`, `suggest_scare_positions`, `evaluate_encounter_pacing`, `design_encounter`, `suggest_patrol_route`, `analyze_level_pacing_structure`, `generate_scare_sequence`, `validate_horror_intensity`, `evaluate_monster_reveal`, `analyze_co_op_balance` |
| Templates / presets | `list_room_templates`, `get_room_template`, `apply_room_template`, `create_room_template`, `list_storytelling_patterns`, `create_storytelling_pattern`, `list_acoustic_profiles`, `create_acoustic_profile`, `create_tension_profile`, `list_genre_presets`, `export_genre_preset`, `import_genre_preset` |
| Validation | `validate_game_ready`, `suggest_lod_strategy`, `batch_validate`, `compare_lod_chain`, `validate_naming_conventions`, `batch_rename_assets` |
| GeometryScript | `mesh_boolean`, `mesh_simplify`, `mesh_remesh`, `generate_collision`, `generate_lods`, `fill_holes`, `compute_uvs`, `mirror_mesh` |
| Mesh import / export | `import_mesh` (static or `import_as_skeletal` + `import_animations`, PR #58), `export_mesh` (FBX, PR #41) |
| Procedural meshes | `create_parametric_mesh`, `create_horror_prop`, `create_structure`, `create_building_shell`, `create_maze`, `create_pipe_network`, `create_fragments`, `create_terrain_patch` |
| Cache | `list_cached_meshes`, `clear_cache`, `validate_cache`, `get_cache_stats` |
| Handles | `create_handle`, `release_handle`, `list_handles`, `save_handle` |
| Prefabs | `create_prefab`, `create_blueprint_prefab`, `spawn_prefab`, `place_blueprint_actor`, `place_spline`, `create_prop_kit`, `place_prop_kit` |
| Hospice / accessibility | `generate_hospice_report`, `analyze_framing` |

**Action categories (experimental town gen, OFF by default):**

| Category | Examples |
|----------|----------|
| Floor plans | `generate_floor_plan`, `create_building_from_grid` |
| Facades / roofs | `generate_facade`, `generate_roof`, `generate_arch_features` |
| City blocks | `create_city_block`, `register_building`, `query_spatial_registry` |
| Auto volumes | `create_auto_volumes`, `adapt_terrain` |
| Furnishing | `furnish_room`, `validate_building` |
| Debug | Debug views and diagnostics |

> **Experimental — town gen has known geometry issues** (wall misalignment, room separation). Fix Plans v2-v5 applied 27+ fixes but fundamental issues remain. Core mesh actions (sweep walls, auto-collision, proc mesh caching, blueprint prefabs) work fine.

See `Plugins/Monolith/Docs/specs/SPEC_MonolithMesh.md` for the full action catalog.

---

## level_instance

Level instance creation, packing, blueprint packing, loading, unloading, and child instance querying. Registered via the MonolithMesh module.

---

## modelgen

Text-to-StaticMesh and caller-supplied generated-model jobs, provenance tracking, and import. (Split from `mesh` on 2026-05-20).

| Action | Params |
|--------|--------|
| `list_model_generation_providers` | none |
| `submit_generated_model_job` | `prompt` (required string), `provider` (optional string), `model` (optional string), `asset_name` (optional string) |
| `get_generated_model_job` | `job_id` (required string) |
| `cancel_generated_model_job` | `job_id` (required string) |
| `download_generated_model_result` | `job_id` (required string) |
| `import_generated_model` | `destination` (required string), `job_id` (optional string), `file_path` (optional string), `provider` (optional string), `model` (optional string), `prompt` (optional string), `source_image_hash` (optional string), `replace_existing` (optional boolean), `material_import` (optional string), `save` (optional boolean) |
| `get_generated_model_provenance` | `asset_path` (required string) |

See `Docs/specs/SPEC_MonolithModelGen.md` for the deep dive.

---

## paper2d

Optional Paper2D asset discovery. Read-only probes; does not load Paper2D modules or mutate assets.

| Action | Params |
|--------|--------|
| `get_status` | none |
| `list_assets` | `package_path` (optional string), `limit` (optional integer) |
| `get_asset` | `asset_path` (required string), `include_tags` (optional boolean), `tag_limit` (optional integer) |

See `Plugins/Monolith/Docs/specs/SPEC_MonolithPaper2D.md` for the deep dive.

## sprite

Optional sprite asset production orchestration and validation actions.

| Action | Params |
|--------|--------|
| `build_candidate_plan` | `spec_path` (string) |
| `build_preview_contact_sheet` | `spec_path` (string), `output_path` (optional string), `thumbnail_size` (optional integer) |
| `export_metadata` | `spec_path` (string), `output_path` (optional string), `sheet_path` (optional string) |
| `get_status` | none |
| `prepare_imagegen_requests` | `spec_path` (string) |
| `run_generation_batch` | `spec_path` (string), `execute` (optional bool), `provider_action` (optional string), `max_requests` (optional integer), `manifest_path` (optional string), `stop_on_error` (optional bool) |
| `validate_asset_spec` | `spec_path` (string) |
| `validate_guides` | `spec_path` (string) |
| `validate_sheet` | `spec_path` (string), `sheet_path` (string), `metadata_path` (optional string) |

See `Plugins/Monolith/Docs/specs/SPEC_MonolithSprite.md` for the deep dive.

## imagegen

Generated-image provider discovery, deterministic local PNG generation, ima2/imag2-gen bridge calls, generated PNG import, SVG source workflows, and MSDF Texture2D baking. `generate_image_via_ima2` keeps provider credentials outside Monolith and accepts only PNG provider output for generated Texture2D imports.

Provider rate limits return `error_data.error_class="provider_rate_limited"`, `retry_signature`, `retry_after_seconds`, `rate_limit_cooldown_active`, and `provider_call_skipped`. After a 429 / `RATE_LIMITED` / relayed rate-limit message, Monolith records a process-local cooldown for the same redacted retry signature; an identical request inside that window short-circuits before the provider call with `provider_call_skipped=true`.

| Action | Params |
|--------|--------|
| `list_image_models` | none |
| `get_image_generation_defaults` | none |
| `generate_image` | `prompt` (required), `aspect_ratio`, `resolution`, `texture_role`, `destination`, `save`, `save_source_png` |
| `generate_image_via_ima2` | `prompt` (required), `server_url`, `provider`, `model`, `quality`, `size`/`resolution`, `format` (`png` only), `background`, `compose_prompt`, reference image fields, `texture_role`, `destination`, `save`, `save_source_png` |
| `import_generated_image` | `bytes_b64` or `file_path`/`path`, `format_hint` (`png`), `texture_role`, `destination`, `save`, `save_source_png` |
| `generate_svg` | `svg_spec` or `prompt`, `profile`, `destination`, `return_svg`, `save`, `strict` |
| `import_generated_svg` | `svg_text` or `bytes_b64` or `file_path`/`path`, `profile`, `destination`, `save`, `strict` |
| `validate_svg` | `svg_text` or `bytes_b64` or `file_path`/`path`, `profile` |
| `generate_msdf_from_svg` | `svg_spec` or `svg_text` or `file_path` or `prompt`, `size`, `pixel_range`, `verify_samples`, `create_material`, `verify_material_render` |
| `get_generated_asset_provenance` | `asset_path` |

See `Plugins/Monolith/Docs/specs/SPEC_MonolithImageGen.md` for the deep dive.

## slate

Report Slate inspector flag state, Slate availability, visible window count, ref generation, and gated read-only actions. **Default-disabled gate:** by default `UMonolithSettings::bEnableSlateInspectorActions=false`, so only `get_inspector_status` is registered; the remaining actions require enabling that setting first. **Waits:** `wait_for_widget` accepts either `text_contains` (substring match on widget text/labels) or `type` (UMG/Slate class name); at least one must be non-empty.

| Action | Params |
|--------|--------|
| `get_inspector_status` | none |
| `list_windows` | `include_titles` |
| `snapshot_widgets` | `window_index` |
| `describe_widget` | `ref` (required) |
| `capture_widget` | `ref` |
| `wait_for_widget` | `text_contains` or `type` |

See `Plugins/Monolith/Docs/specs/SPEC_MonolithSlate.md` for the deep dive.

## ui

`diff_ui_spec` / `apply_ui_spec_patch` are the canonical design-data delta surface: diff reports stable patch candidates plus graph-binding preservation and explicit replace-decomposition evidence, and patch composes existing widget, image, Border brush, common-style, SizeBox/Border/ProgressBar style, CommonUI style, and EffectSurface owner actions instead of registering raw `apply_json_to_umg` / `apply_layout` compatibility actions. `replace_widget` is a confirm-gated patch op, not a duplicate public action: the normal path expands to `ui.remove_widget` + `ui.add_widget` plus existing setter actions, while `preserve_children=true` expands to temporary `ui.add_widget`, explicit child `ui.move_widget`, old `ui.remove_widget`, and `ui.rename_widget` to restore the stable name. Border `brushPath` expands to the existing `ui.set_brush` owner action. `audit_widget_layout` remains the single read-only structural lint surface and now emits guide-derived `rule_id` findings such as `OneChildCanvasWrapper`, `CanvasOveruse`, `EdgeUiMissingSafeZone`, `DecorativeHitTestBlocker`, `HiddenInteractiveSpace`, `UnstyledInteractiveState`, `MaterialDomainMismatch`, and `LargeStaticListWithoutListView` through the same action instead of adding duplicate validators. `audit_widget_material_lifecycle` is the companion read-only graph lint for dynamic-material lifetime: it scans Widget Blueprint K2 graphs for MID creation calls and reports repeated-lifecycle findings without cloning Blueprint graph edit APIs. `workflow.ui_shipping_widget_blueprint` forwards its `layout_rule_profile` and `suppress_layout_rule_ids` to the layout owner action, composes `ui.measure_widget_layout` for visual/runtime authored bounds, overlap, and safe-zone evidence, and reports `DpiSafeZoneProfileMissing` under `validation.visual_profile` when mobile/console visual proof profiles omit explicit DPI or safe-zone inputs. Its real visual fixture verifies that the workflow executes `editor.capture_scene_preview` and then `ui.verify_widget_visual_artifacts` against the generated PNG. `workflow.ui_bind_widget_event` is the companion interaction workflow: it does not add UI-local Blueprint graph primitives, and instead composes `blueprint.resolve_node`, `blueprint.add_node`, `blueprint.connect_pins`, `blueprint.compile_blueprint`, and `blueprint.get_graph_summary` behind a ViewModel-only boundary and explicit confirm gate. Its real WBP fixture verifies the resulting `StartButton.OnClicked` component-bound event, ViewModel getter, command call, exec link, and ViewModel target pin link in the EventGraph. `apply_animation_delta` likewise stays inside the canonical UI animation owner action: external Sequencer write names remain search aliases, while component design-data deltas use `property=transform/color` plus `component` to write/read canonical `RenderTransform.*` and `ColorAndOpacity.*` float tracks. `workflow.ui_material_hlsl_effect` follows the same owner-action rule for advanced UI visuals: it composes `material.create_material`/`set_material_property`/`create_custom_hlsl_node`/`update_custom_hlsl_node`/`connect_expressions`, optional `material.build_material_graph(clear_existing=false)` for float4 alpha ComponentMask-to-Opacity wiring, material compile/stat/connection readback actions, `ui.set_image` or `ui.set_brush`, `ui.audit_widget_material_lifecycle`, and optional `workflow.ui_shipping_widget_blueprint`, without adding a separate `hlsl` namespace or external material aliases. Retainer effects use the same split: `ui.set_retainer_effect_material` is the RetainerBox owner mutation, while `workflow.ui_retainer_effect_material` composes material parameter/domain readback plus that owner action, `ui.audit_widget_material_lifecycle`, and optional widget proof.

UMG widget Blueprint CRUD, explicit session/request work context, templates, styling, UI post-copy repair, animation (v1 + v2 plus read inspection and guarded delta edits), the schema-driven **Spec / EffectSurface** architecture, UIExtension-point setup, CommonFramework diagnostics/authoring, settings scaffolding, accessibility, **CommonUI**, and GAS UI bindings. The live count is registry-derived — includes `set_widget_context` / `get_widget_context` / `clear_widget_context` for diagnostic target focus without hidden write defaults, `get_animation_overview` / `get_animation_timeline` / `get_animation_time_slice` for read-only UMG animation evidence without registering duplicate external Sequencer names, `apply_animation_delta` for confirm-gated float-key merge/upsert/delete on existing animations including `RenderOpacity`, `RenderTransform.*`, and `ColorAndOpacity.*` component paths, `measure_widget_layout` for authored bounds / overlap / safe-zone evidence without cloning weak external layout APIs, `audit_widget_material_lifecycle` for read-only WBP graph DMI lifetime evidence, `add_extension_point_widget` for `UUIExtensionPointWidget` + GameplayTag + deterministic slot layout, `add_primary_game_layout_layer` for PrimaryGameLayout CommonActivatable layer container widgets, `apply_common_menu_transform_spec` for guarded menu-level transform application, `copy_widget_subtree_with_class_remap` for copied WBP subtree repair with class/object/root remaps, `clone_composite_font_with_remapped_faces` for copied composite `UFont` face-reference remapping, `repair_slate_font_references` for copied UI asset `FSlateFontInfo.FontObject` remapping, plus `get_common_framework_status` / `describe_common_widget_blueprint` / `describe_common_messaging_flow` / `validate_common_dialog_contract` / `validate_common_layer_push_contract` / `validate_frontend_menu_flow` for CommonUI, CommonGame, UIExtension, CommonUser, CommonLoadingScreen, GameSettings, GameplayMessageRouter, ModularGameplayActors, GameSubtitles, and CommonGame messaging/modal/frontend reflection diagnostics. The four CommonUI-surface gap-closure actions (`convert_border_to_common`, `convert_textblock_to_common`, `set_action_bar_button_class`, `apply_token_binding`) are `#if WITH_COMMONUI`-gated; `apply_token_binding` is validation/probe only until BP graph writes ship and returns non-success `status:"not_implemented"` for the deferred write.

> For full param schemas, use focused `monolith_discover` schema mode at runtime. The surface is large — categories below; the v0.15.0-new actions are flagged.

**Action categories (UMG + Spec baseline, always registered):**

| Category | Actions | Examples |
|----------|---------|----------|
| Widget CRUD | 9 | `create_widget_blueprint`, `get_widget_tree`, `add_widget`, `remove_widget`, `set_widget_property` (accepts `value` alias and routes `IsVariable`/`bIsVariable`), `compile_widget` (returns `errors[]`/`warnings[]`), `list_widget_types`, `rename_widget`, `dump_blueprint_compile_log` |
| Context | 3 | `set_widget_context`, `get_widget_context`, `clear_widget_context` remember/read/clear explicit UMG target focus by session/request scope; responses include `resolved_from`, freshness, existence checks, recent contexts, and `usable_as_default_for_mutation=false` |
| UIExtension points | 1 | `add_extension_point_widget` creates or updates a `UUIExtensionPointWidget`-compatible widget, assigns a registered GameplayTag, and keeps Canvas/box/overlay slot layout idempotent |
| CommonFramework diagnostics/authoring | 7 | `get_common_framework_status` reports CommonUI/CommonGame/UIExtension/CommonUser/CommonLoadingScreen/GameSettings/GameplayMessageRouter/ModularGameplayActors/GameSubtitles plugin, module, reflected class, and reflected struct availability; `add_primary_game_layout_layer` adds CommonActivatable layer container widgets to PrimaryGameLayout WBPs; `describe_common_widget_blueprint` reports PrimaryGameLayout parentage, extension point tags, and CommonActivatableWidgetContainerBase layer candidates; `describe_common_messaging_flow`, `validate_common_dialog_contract`, `validate_common_layer_push_contract`, and `validate_frontend_menu_flow` report CommonGame messaging/dialog/modal-layer/frontend contract readiness without runtime edits |
| Variable flags (v0.15.0) | 3 | `add_widget_variable`, `set_widget_is_variable`, `list_widget_property_enums` |
| Root / reparent (v0.15.0) | 1 | `reparent_widget_root` |
| Slot / layout | 4 | `set_slot_property`, `set_anchor_preset`, `move_widget`, `set_brush` |
| Styling | 6 | `set_font`, `set_color_scheme`, `batch_style`, `set_text`, `set_image`, `setup_list_view` |
| Post-copy repair | 3 | `copy_widget_subtree_with_class_remap` copies WBP widget subtrees into a destination WBP with class/object/root remaps and collision policy; `clone_composite_font_with_remapped_faces` clones a composite `UFont` into a new destination asset and remaps asset-backed `UFontFace` entries; `repair_slate_font_references` scans a copied UI asset package for serialized `FSlateFontInfo` values and remaps their `FontObject` `UFont` references |
| Templates / scaffolds | 13 | `create_hud_element`, `create_menu`, `create_settings_panel`, `create_dialog`, `create_notification_toast`, `create_loading_screen`, `create_inventory_grid`, `create_save_slot_list`, `scaffold_game_user_settings`, `scaffold_save_game`, `scaffold_save_subsystem`, `scaffold_audio_settings`, `scaffold_input_remapping` |
| Headline scaffolders (v0.15.0) | 3 | `scaffold_main_menu`, `scaffold_settings_panel_with_tabs`, `scaffold_pause_menu` |
| Animation v1 | 5 | `list_animations`, `get_animation_details`, `create_animation`, `add_animation_keyframe`, `remove_animation` |
| Animation v2 | 5 | `create_animation_v2`, `add_bezier_eased_segment`, `bake_spring_animation`, `add_animation_event_track`, `bind_animation_to_event` |
| Animation read inspection | 3 | `get_animation_overview`, `get_animation_timeline`, `get_animation_time_slice` |
| Animation delta | 1 | `apply_animation_delta` confirm-gated scalar/component float-key merge/upsert/delete on existing animations; external Sequencer names remain search aliases only |
| Inspection | 3 | `list_widget_events`, `list_widget_properties`, `get_widget_bindings` |
| Design import | 4 | `import_texture_from_bytes`, `import_font_family`, `set_rounded_corners`, `create_gradient_mid_from_spec` |
| EffectSurface (provider-gated, `-32010` when absent) | 13 | `apply_box_shadow`, `set_effect_surface_corners`, `set_effect_surface_fill`, `set_effect_surface_border`, `set_effect_surface_dropShadow`, `set_effect_surface_innerShadow`, `set_effect_surface_glow`, `set_effect_surface_filter`, `set_effect_surface_backdropBlur`, `set_effect_surface_insetHighlight`, `apply_effect_surface_preset` |
| Spec round-trip / menu transform | 11 | `build_ui_from_spec`, `dump_ui_spec`, `dump_ui_spec_schema`, `convert_markup_to_ui_spec`, `diff_ui_spec`, `apply_ui_spec_patch`, `audit_widget_layout`, `audit_widget_material_lifecycle`, `measure_widget_layout`, `build_menu_from_spec` (v0.15.0), `apply_common_menu_transform_spec` |
| Accessibility | 6 | `scaffold_accessibility_subsystem`, `audit_accessibility`, `set_colorblind_mode`, `set_text_scale`, `apply_high_contrast_variant`, `set_text_scale_binding` |
| Allowlist / diagnostics | 3 | `dump_property_allowlist`, `describe_widget_type_schema`, `dump_style_cache_stats` |

`describe_widget_type_schema` is the safe pre-write schema probe for JSON/markup-driven UMG authoring. For live widgets it reports the actual parent slot class, marks incompatible contextual `Slot.*` paths as `settable:false` with `blocked_reason`, and exposes `live_child_capacity` so full single-child parents are visible before add/move/patch writes. With `include_unsafe=true`, raw reflected properties remain non-settable and raw engine aliases already represented by curated paths are suppressed.

### `ui.apply_common_menu_transform_spec`

Apply the guarded counterpart to `ui.build_menu_from_spec` deferred aggregation and copied-menu repair. `dry_run=true` is the default. Writes require `dry_run=false` and `confirm=true`.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `spec` | object | optional | Nested transform object. When omitted, the payload itself is the spec. |
| `screens` | array | optional | Screen map entries `{id, asset_path}` used to resolve `focus_table` and `nav_overrides` screen ids. |
| `layout_asset_path` | string | optional | Default PrimaryGameLayout WBP path for `layout_layers` or `layers`. |
| `layout_layers` / `layers` | array | optional | `ui.add_primary_game_layout_layer` specs. `layers[].layer_tag` falls back to `tag` or `id`. |
| `extension_points` | array | optional | `ui.add_extension_point_widget` specs. |
| `widget_properties` | array | optional | `ui.set_widget_property` specs. Accepts `widget`/`name` and `property` aliases. |
| `remove_widgets` | array | optional | `ui.remove_widget` specs. Each entry accepts `widget_name` or `widget_names[]`. |
| `variable_defaults` | array | optional | `blueprint.set_variable_defaults` specs. Accepts `variable_name` and `value` aliases. |
| `focus_table` / `initial_focus` / `desired_focus` | array | optional | Entries routed to `ui.set_initial_focus_target`. |
| `nav_overrides` / `navigation_bulk` | array | optional | Navigation entries grouped into `ui.set_widget_navigation_bulk`. |
| `widget_subtrees` | array | optional | `ui.copy_widget_subtree_with_class_remap` child specs. |
| `blueprint_graphs` | array | optional | `blueprint.clone_graphs_with_reference_remap` child specs. |
| `font_repairs` | array | optional | `ui.repair_slate_font_references` child specs. |
| `frontend_validation` | object | optional | `ui.validate_frontend_menu_flow` spec run after transform planning/apply. |
| `class_remaps`, `object_remaps`, `root_remaps`, `source_root`, `dest_root` | mixed | optional | Shared remap defaults merged into child repair steps when omitted. |
| `dry_run` | boolean | optional | Plan mutating child steps. Default: `true`. |
| `confirm` | boolean | optional | Required for mutation. Default: `false`. |
| `compile`, `save`, `continue_on_error`, `request_id` | mixed | optional | Defaults forwarded to child writers when supported and echoed in the result. |

Returns `{ ok, status, dry_run, confirm, step_count, executed_step_count, planned_only_step_count, failed_step_count, changed, changed_known, changed_unknown_step_count, compiled, saved, planned_counts, applied_counts, steps[], errors[] }`. During dry-run, child writers without native dry-run are returned as planned rows and are not executed.

### `ui.validate_frontend_menu_flow`

Validate a frontend menu composition contract without editing assets. The action uses reflection and Widget Blueprint inspection to check optional `PrimaryGameLayout` layer candidates, dialog compatibility, and per-screen CommonUI/CommonActivatable expectations.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `layout_asset_path` | string | optional | Optional PrimaryGameLayout Widget Blueprint to inspect for required layer candidates. |
| `required_layers` | array | optional | Layer specs as strings or objects with `layer_tag` and optional `widget_name`. |
| `screens` | array | optional | Screen specs with `asset_path`, optional `role`, `expected_parent_class`, `desired_focus_widget`, `require_common_activatable`, `required_widgets`, `forbidden_widgets`, `expected_variables`, `expected_widget_classes`, `required_graph_needles`, and `forbidden_graph_needles`. |
| `modal_layer_tag` | string | optional | GameplayTag that should be registered for modal/dialog pushes. Default: `UI.Layer.Modal`. |
| `dialog_class` | string | optional | Dialog class expected to be compatible with `CommonGameDialog`. |
| `require_layout_asset` | boolean | optional | Fail parameter validation if `layout_asset_path` is omitted. Default: `false`. |
| `require_dialog` | boolean | optional | Report `ok=false` when `dialog_class` is omitted. Default: `false`. |
| `include_graph_scan` | boolean | optional | Scan Widget Blueprint graphs for required/forbidden text needles. Default: `true`. |

Returns `{ ok, overall_status, checks[], issues[], warnings[], limitations[], layout, dialog, screens[] }`. `issues[]` is populated for missing assets, wrong parent classes, missing/forbidden widgets, widget class mismatches, variable default mismatches, desired focus mismatches, and graph-needle failures.

### `ui.copy_widget_subtree_with_class_remap`

Copy one or more source Widget Blueprint widget subtrees into a destination Widget Blueprint while remapping widget classes and copied object/package references. The action preflights source widgets, destination parents, remapped classes, subtree child compatibility, and destination name collisions before mutation. `dry_run=true` returns the plan without calling `Modify`, compile, or save; writes require `dry_run=false` and `confirm=true`.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `source_asset_path` | string | **required** | Source Widget Blueprint asset path. |
| `destination_asset_path` | string | **required** | Destination Widget Blueprint asset path. |
| `source_widget_name` | string | optional | Single source widget name to copy. Supply this or `source_widget_names`. |
| `source_widget_names` | array | optional | Multiple source widget names to copy. |
| `destination_widget_name` | string | optional | Destination name override. Only valid with one source widget. |
| `destination_parent_name` | string | optional | Destination panel widget name. Defaults to the source-equivalent parent or destination root. |
| `class_remaps` | object | optional | Source widget class path/token to destination widget class path/token map, e.g. `{ "VerticalBox": "HorizontalBox" }`. |
| `object_remaps` | object | optional | Exact hard/soft object path remaps. |
| `root_remaps` | object | optional | Source-root to destination-root map for copied object paths, e.g. `{"/Game/Old":"/Game/New"}`. |
| `source_root` | string | optional | Single source-root shorthand; must be supplied with `dest_root`. |
| `dest_root` | string | optional | Single destination-root shorthand; must be supplied with `source_root`. |
| `existing_policy` | enum | optional | `fail`, `replace`, or `skip` when destination names already exist. Default: `fail`. |
| `insert_policy` | enum | optional | `source_index` or `append` for copied root placement. Default: `source_index`. |
| `require_remapped_classes` | boolean | optional | Require every copied widget class to match `class_remaps`. Default: `false`. |
| `compile` | boolean | optional | Compile destination WBP after applying. Default: `true`. |
| `dry_run` | boolean | optional | Plan without mutating the destination WBP. Default: `true`. |
| `confirm` | boolean | optional | Required when `dry_run=false`. Default: `false`. |
| `save` | boolean | optional | Save the destination package after applying. Default: `false`. |

### `ui.clone_composite_font_with_remapped_faces`

Clone a source composite `UFont` to a new destination asset and remap `UFontFace` references using exact face-path remaps and/or package-root remaps. The action preflights every default, fallback, and sub-typeface entry before any mutation; destination assets are never overwritten.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `source_font_path` | string | **required** | Source composite `UFont` asset path. |
| `destination_font_path` | string | **required** | New destination `UFont` asset path to create. Existing assets fail. |
| `root_remaps` | object | optional | Source-root to destination-root map, e.g. `{"/Game/Old":"/Game/New"}`. |
| `source_root` | string | optional | Single source-root shorthand; must be supplied with `dest_root`. |
| `dest_root` | string | optional | Single destination-root shorthand; must be supplied with `source_root`. |
| `font_face_remaps` | object | optional | Exact source `UFontFace` path to destination `UFontFace` path map. |
| `dry_run` | boolean | optional | Plan without creating the destination asset. Default: `true`. |
| `confirm` | boolean | optional | Required when `dry_run=false`. Default: `false`. |
| `save` | boolean | optional | Save the new font package after creation. Default: `false`. |

### `ui.repair_slate_font_references`

Scan a copied UI asset package for serialized `FSlateFontInfo` values and remap their `FontObject` `UFont` references using exact font-path remaps and/or package-root remaps. The action scans package-local objects only, skips generated classes/CDOs/world packages, preflights every remapped target as a loadable `UFont`, and only mutates when `dry_run=false` and `confirm=true`.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `asset_path` | string | **required** | Asset package to scan, such as a Widget Blueprint or UI style asset. |
| `root_remaps` | object | optional | Source-root to destination-root map, e.g. `{"/Game/Old":"/Game/New"}`. |
| `source_root` | string | optional | Single source-root shorthand; must be supplied with `dest_root`. |
| `dest_root` | string | optional | Single destination-root shorthand; must be supplied with `source_root`. |
| `font_asset_remaps` | object | optional | Exact source `UFont` path to destination `UFont` path map. |
| `include_unchanged` | boolean | optional | Include unchanged `FSlateFontInfo` entries in the changes array. Default: `false`. |
| `dry_run` | boolean | optional | Plan without mutating the asset package. Default: `true`. |
| `confirm` | boolean | optional | Required when `dry_run=false`. Default: `false`. |
| `save` | boolean | optional | Save the package after applying changes. Default: `false`. |

**Action categories (CommonUI, registered when `WITH_COMMONUI=1`):**

| Category | Actions | Examples |
|----------|---------|----------|
| Activatable widgets | 8 | `create_activatable_widget`, `create_activatable_stack`, `create_activatable_switcher`, `configure_activatable`, `push_to_activatable_stack`, `pop_activatable_stack`, `get_activatable_stack_state`, `set_activatable_transition` |
| Common buttons / styles | 7 | `convert_button_to_common`, `configure_common_button`, `create_common_button_style`, `create_common_text_style`, `create_common_border_style`, `apply_style_to_widget`, `batch_retheme` |
| Common config | 2 | `configure_common_text`, `configure_common_border` |
| Conversion gap-closure (v0.15.0) | 4 | `convert_textblock_to_common`, `convert_border_to_common`, `set_action_bar_button_class`, `apply_token_binding` |
| Input | 7 | `create_input_action_data_table`, `add_input_action_row`, `bind_common_action_widget`, `create_bound_action_bar`, `get_active_input_type`, `set_input_type_override`, `list_platform_input_tables` |
| Navigation / focus | 9 | `set_widget_navigation`, `set_widget_navigation_bulk` (v0.15.0), `dump_widget_navigation` (v0.15.0), `set_initial_focus_target`, `force_focus`, `get_focus_path`, `request_refresh_focus`, `audit_focus_chain`, `enforce_focus_ring` |
| Lists / tabs / groups | 4 | `setup_common_list_view`, `create_tab_list_widget`, `register_tab`, `create_button_group` |
| Carousels / switcher | 2 | `configure_animated_switcher`, `create_widget_carousel` |
| Hardware | 1 | `create_hardware_visibility_border` |
| Numeric / rotator | 2 | `configure_numeric_text`, `configure_rotator` |
| Lazy / load guard | 2 | `create_lazy_image`, `create_load_guard` |
| Modals / messages | 2 | `show_common_message`, `configure_modal_overlay` |
| Audit / report | 2 | `audit_commonui_widget`, `export_commonui_report` |
| Reload / diagnostics | 2 | `hot_reload_styles`, `dump_action_router_state` |
| Reduce motion | 1 | `wrap_with_reduce_motion_gate` |

**GAS UI binding aliases (4 — same handlers as `gas::*` versions):**

`bind_widget_to_attribute`, `unbind_widget_attribute`, `list_attribute_bindings`, `clear_widget_attribute_bindings`. These four are registered into `ui` from `MonolithGAS/Private/MonolithGASUIBindingActions.cpp`. Pick whichever namespace reads better in your call site — both dispatch to identical code.

> **Phase J F2/F3:** these four actions now reject empty `widget_path`, missing `attribute`, or unresolvable ASC up-front with structured errors instead of writing junk via reflection.
> **Phase J F5:** the response shape is `{ bindings: [...], count: N }`, not a bare array. Wrap your client parsers.

See `Plugins/Monolith/Docs/specs/SPEC_MonolithUI.md` for the deep dive including style-creator-as-data Blueprint pattern and conditional CommonUI gating.

---

## input

Enhanced Input asset authoring for `UInputAction` and `UInputMappingContext` assets. These actions are implemented by `MonolithGAS` but register under the standalone `input` namespace. **10 actions.**

| Action | Params |
|--------|--------|
| `list_input_actions` | `path` (optional string), `include_details` (optional boolean) |
| `get_input_action` | `asset_path` |
| `create_input_action` | `asset_path`, `value_type`, `description`, `consume_input`, `trigger_when_paused`, `accumulation`, `overwrite`, `save` |
| `set_input_action_properties` | `asset_path`, `value_type`, `description`, `consume_input`, `consume_legacy_mappings`, `trigger_when_paused`, `reserve_all_mappings`, `accumulation`, `save` |
| `list_input_mapping_contexts` | `path` (optional string), `include_details` (optional boolean) |
| `get_input_mapping_context` | `asset_path` |
| `create_input_mapping_context` | `asset_path`, `description`, `overwrite`, `save` |
| `add_input_mapping` | `context_path`, `action_path`, `key`, optional `source_context_path`/`source_action_path`/`source_key`, optional `modifier_classes`, optional `trigger_classes`, `allow_duplicate`, `dry_run`, `save` |
| `remove_input_mapping` | `context_path`, `action_path`, `key`, `save` |
| `validate_input_mappings` | `context_paths` |

`input.add_input_mapping` is idempotent by default: it reuses an existing action+key mapping, deep-compares modifier/trigger object values, can clone modifiers/triggers from another mapping, and only writes when the desired mapping differs. Set `allow_duplicate=true` to intentionally add a second identical action+key row.

---

## gas

Gameplay Ability System integration. **142 actions** across 12 categories — covers the full GAS authoring pipeline. **Conditional on `#if WITH_GBA`** — projects without the GameplayAbilities plugin register 0 GAS actions.

> For full param schemas, call `describe_query("action_schema", target_namespace="gas", target_action="<name>")` (or `monolith_discover("gas", detail=true)`). Plain `monolith_discover("gas")` is terse — names + one-line descriptions only.

**Action categories:**

| Category | Actions | Description |
|----------|---------|-------------|
| Scaffold | 7 | `bootstrap_gas_foundation`, `validate_gas_setup`, `scaffold_gas_project`, `scaffold_damage_pipeline`, `scaffold_status_effect`, `scaffold_weapon_ability`, `scaffold_tag_hierarchy` |
| Attributes | 18 | `create_attribute_set`, `add_attribute`, `get_attribute_set`, `set_attribute_defaults`, `list_attribute_sets`, `configure_attribute_clamping`, `configure_meta_attributes`, `create_attribute_set_from_template`, `create_attribute_init_datatable`, `duplicate_attribute_set`, `configure_attribute_replication`, `link_datatable_to_asc`, `bulk_edit_attributes`, `validate_attribute_set`, `find_attribute_modifiers`, `diff_attribute_sets`, `get_attribute_dependency_graph`, `remove_attribute`, `get_attribute_value`, `set_attribute_value` |
| Effects | 22 | `create_gameplay_effect`, `get_gameplay_effect`, `list_gameplay_effects`, `add_modifier`, `set_modifier`, `remove_modifier`, `list_modifiers`, `add_ge_component`, `set_ge_component`, `remove_ge_component`, `set_effect_stacking`, `set_duration`, `set_period`, `create_effect_from_template`, `build_effect_from_spec`, `batch_create_effects`, `add_execution`, `duplicate_gameplay_effect`, `delete_gameplay_effect`, `validate_effect`, `get_effect_interaction_matrix`, `get_active_effects`, `get_effect_modifiers_breakdown`, `apply_effect`, `remove_effect`, `simulate_effect_stack` |
| Abilities | 24 | `create_ability`, `get_ability_info`, `list_abilities`, `compile_ability`, `set_ability_tags`, `get_ability_tags`, `set_ability_policy`, `set_ability_cost`, `set_ability_cooldown`, `set_ability_triggers`, `set_ability_flags`, `add_ability_task_node`, `add_commit_and_end_flow`, `add_effect_application`, `add_gameplay_cue_node`, `create_ability_from_template`, `build_ability_from_spec`, `batch_create_abilities`, `duplicate_ability`, `list_ability_tasks`, `get_ability_task_pins`, `wire_ability_task_delegate`, `get_ability_graph_flow`, `validate_ability`, `find_abilities_by_tag`, `get_ability_tag_matrix`, `validate_ability_blueprint`, `scaffold_custom_ability_task` |
| ASC | 13 | `add_asc_to_actor`, `configure_asc`, `setup_asc_init`, `setup_ability_system_interface`, `apply_asc_template`, `set_default_abilities`, `set_default_effects`, `set_default_attribute_sets`, `set_asc_replication_mode`, `validate_asc_setup`, `grant_ability`, `revoke_ability`, `get_asc_snapshot`, `get_all_ascs`, `grant_ability_to_pawn` *(NEW Phase J F8)* |
| Tags | 10 | `add_gameplay_tags`, `get_tag_hierarchy`, `search_tag_usage`, `scaffold_tag_hierarchy`, `rename_tag`, `remove_gameplay_tags`, `validate_tag_consistency`, `audit_tag_naming`, `export_tag_hierarchy`, `import_tag_hierarchy` |
| Cues | 10 | `create_gameplay_cue_notify`, `link_cue_to_effect`, `unlink_cue_from_effect`, `get_cue_info`, `list_gameplay_cues`, `set_cue_parameters`, `find_cue_triggers`, `validate_cue_coverage`, `batch_create_cues`, `scaffold_cue_library` |
| Targeting | 5 | `create_target_actor`, `configure_target_actor`, `add_targeting_to_ability`, `scaffold_fps_targeting`, `validate_targeting` |
| Input | 5 | `setup_ability_input_binding`, `bind_ability_to_input`, `batch_bind_abilities`, `get_ability_input_bindings`, `scaffold_input_binding_component` |
| DataAsset Profile | 3 | `describe_data_asset_gas_profile`, `validate_data_asset_gas_profile`, `set_data_asset_gas_fields` |
| Inspect / Readiness | 10 | `export_gas_manifest`, `get_runtime_summary`, `snapshot_gas_state`, `get_tag_state`, `get_cooldown_state`, `trace_ability_activation`, `compare_gas_states`, `start_event_cue_probe`, `stop_event_cue_probe`, `expect_event_cue` |
| UI bindings | 4 | `bind_widget_to_attribute`, `unbind_widget_attribute`, `list_attribute_bindings`, `clear_widget_attribute_bindings` *(also aliased into `ui` namespace — same handlers)* |

### `gas.describe_data_asset_gas_profile` / `gas.validate_data_asset_gas_profile` / `gas.set_data_asset_gas_fields` · NEW in DataAsset GAS workflow P0/P3

Inspection, validation, and safe field writes for DataAsset-driven GAS skill/profile assets. The describe action accepts `asset_path` plus an optional profile map of semantic roles to UPROPERTY candidates. The validate action scans a `path_filter`, detects missing or invalid ability/effect/cue/input/policy fields, broken references, duplicate input action/tag pairs, deprecated `DynamicAbilityTags` source usage, and incomplete Started/Completed/Canceled release support. The set action accepts `asset_path`, `fields`, optional `profile`, `dry_run` (default true), `strict` (default true), and `save` (default false); non-dry-run writes are transacted and report the post-write profile shape.

### `gas.start_event_cue_probe` / `gas.stop_event_cue_probe` / `gas.expect_event_cue` · NEW in DataAsset GAS workflow P4

PIE-only runtime evidence for DataAsset/native GAS flows. `start_event_cue_probe` attaches bounded listeners to an actor ASC, `stop_event_cue_probe` removes them and returns captured rows, and `expect_event_cue` wraps an optional Monolith trigger action with pass/fail evidence. Gameplay events use `UAbilitySystemComponent::AddGameplayEventTagContainerDelegate`; cue coverage uses active tag-count changes through `RegisterGameplayTagEvent`, so instant `ExecuteGameplayCue` payload capture is explicitly reported as unsupported by UE 5.7 public hooks.

### `gas.grant_ability_to_pawn` · NEW in Phase J F8

Grant a `UGameplayAbility` to a pawn's `UAbilitySystemComponent` directly without scaffold-side wiring or `apply_effect` ceremony. See `describe_query("action_schema", target_namespace="gas", target_action="grant_ability_to_pawn")` for params.

See `Plugins/Monolith/Docs/specs/SPEC_MonolithGAS.md` for the deep dive.

---

## chaos_fracture

Optional Geometry Collection and Fracture module visibility. Read-only probes; does not load Fracture tools or mutate assets. **3 actions.**

| Action | Params |
|--------|--------|
| `get_status` | none |
| `list_geometry_collection_assets` | `package_path` (optional string), `limit` (optional integer) |
| `list_geometry_collection_components` | `limit` (optional integer) |

See `Plugins/Monolith/Docs/specs/SPEC_MonolithChaosFracture.md` for the deep dive.

---

## pcg

Typed PCG graph discovery, asset creation, transactional node/edge/settings editing, bounded structural read-back and validation, plus guarded copied-graph soft-reference migration. Typed authoring rejects cycles, half-attached edges, ambiguous authored titles, implicit single-input replacement, and implicit filter/conversion insertion; settings writes honor property-chain editability and editor callbacks. Graph mutations validate before persistence so an error is never introduced by a non-atomic validator after the package file changes. `get_pcg_graph_info` applies `pin_limit` per input/output direction and a shared `response_item_limit` across nodes, pins, edges, and nested settings; callers must inspect truncation flags and returned counts. Component assignment/generation execution remains unavailable. **14 actions.**

| Action | Params |
|--------|--------|
| `get_status` | none |
| `list_graph_assets` | `package_path` (optional string), `limit` (optional integer) |
| `get_graph_asset` | `asset_path`, `include_tags` (optional boolean), `tag_limit` (optional integer) |
| `remap_graph_references` | `asset_path`, `root_remaps`, `dry_run` (optional boolean), `confirm` (optional boolean), `require_targets` (optional boolean), `save` (optional boolean), `strict` (optional boolean), `max_objects` (optional integer), `max_references` (optional integer) |
| `list_components` | `limit` (optional integer) |
| `list_pcg_node_types` | `query` (optional string), `include_properties` (optional boolean), `property_limit` (optional integer), `limit` (optional integer) |
| `create_pcg_graph` | `asset_path`, `existing_policy` (optional string), `save` (optional boolean) |
| `get_pcg_graph_info` | `asset_path`, `include_settings` (optional boolean), `settings_fields` (optional array), `property_limit` (optional integer), `array_limit` (optional integer), `node_limit` (optional integer), `edge_limit` (optional integer), `pin_limit` (optional integer), `response_item_limit` (optional integer) |
| `add_pcg_node` | `asset_path`, `node_type`, `node_title` (optional string), `existing_policy` (optional string), `position` (optional array), `properties` (optional object), `save` (optional boolean) |
| `remove_pcg_node` | `asset_path`, `node_id`, `save` (optional boolean) |
| `connect_pcg_nodes` | `asset_path`, `source_node`, `source_pin`, `target_node`, `target_pin`, `save` (optional boolean) |
| `disconnect_pcg_nodes` | `asset_path`, `source_node`, `source_pin`, `target_node`, `target_pin`, `save` (optional boolean) |
| `set_pcg_node_params` | `asset_path`, `node_id`, `properties`, `dry_run` (optional boolean), `save` (optional boolean) |
| `validate_pcg_graph` | `asset_path`, `require_output_connection` (optional boolean), `require_no_isolated_nodes` (optional boolean), `issue_limit` (optional integer) |

See `Plugins/Monolith/Docs/specs/SPEC_MonolithPCG.md` for the deep dive.

---

## chooser

Optional ChooserTable authoring, modification, and evaluation discovery. Requires the engine Chooser plugin.

| Action | Params |
|--------|--------|
| `create_chooser_table` | `asset_path`, `output_type` (optional string) |
| `add_chooser_column` | `asset_path`, `column_kind`, `binding_property` (optional string) |
| `add_chooser_row` | `asset_path`, `cells`, `output_psd` (optional string) |
| `set_chooser_cell` | `asset_path`, `column_index`, `row_index`, `bool_value` (optional boolean), `enum_value` (optional integer), `comparison` (optional string), `float_min` (optional number), `float_max` (optional number), `tags` (optional any) |
| `set_context_object_class` | `asset_path`, `class_path`, `context_index` (optional integer) |
| `set_result_asset_reference` | `asset_path`, `asset_path_value`, `row_or_column` |
| `set_evaluate_chooser_result_reference` | `asset_path`, `child_chooser_path`, `row` |
| `duplicate_chooser_tree` | `source_paths`, `destination_folder`, `remap_rules` (optional array) |
| `inspect_chooser` | `asset_path`, `include_cells` (optional boolean), `recursive` (optional boolean) |
| `list_chooser_tables` | `path_filter` (optional string) |
| `get_chooser_table` | `asset_path`, `include_rows` (optional boolean) |
| `list_chooser_columns` | `asset_path` |
| `list_chooser_rows` | `asset_path`, `start_row` (optional number), `limit` (optional number) |
| `list_chooser_references` | `asset_path` |
| `validate_chooser` | `asset_path`, `expected_context_class` (optional string), `expected_result_type` (optional string) |
| `validate_chooser_table` | `asset_path` |

## dataflow

Read-only Dataflow/Chaos graph discovery and bounded graph inspection. `get_status` and `list_assets` always register; the graph and node-schema readers are conditional on `WITH_MONOLITH_DATAFLOW`. Does not load, evaluate, regenerate, or mutate Dataflow assets. **8 actions.**

| Action | Params |
|--------|--------|
| `get_status` | none |
| `list_assets` | `package_path` (optional string), `limit` (optional integer) |
| `get_dataflow_graph` | `asset_path`, `node_limit` (optional integer), `connection_limit` (optional integer), `include_properties` (optional boolean) |
| `list_dataflow_node_types` | `filter` (optional string), `common_only` (optional boolean), `limit` (optional integer), `include_pins` (optional boolean) |
| `get_dataflow_node_schema` | `type_name`, `include_properties` (optional boolean) |
| `validate_dataflow_graph` | `asset_path` |
| `list_dataflow_variables` | `asset_path` |
| `list_dataflow_comments` | `asset_path`, `node_limit` (optional integer) |

See `Plugins/Monolith/Docs/specs/SPEC_MonolithDataflow.md` for the deep dive.

---

## source_control

Unreal SourceControl-provider status, guarded file prepare/delete/revert operations, and Perforce opened/path mapping. **11 actions.**

Canonical file-list params are `paths` arrays. Source-control actions also accept a single path string and the alias key `files`; boolean options accept booleans plus string literals `true`, `false`, `1`, `0`, `yes`, `no`, `on`, and `off`. Destructive delete/revert actions still require `confirm=true` unless `dry_run=true`.

| Action | Params |
|--------|--------|
| `get_capabilities` | none |
| `get_status` | `paths` array/string, alias `files` |
| `checkout` | `paths` array/string, alias `files`; `dry_run` (optional boolean/string) |
| `add` | `paths` array/string, alias `files`; `dry_run` (optional boolean/string) |
| `checkout_or_add` | `paths` array/string, alias `files`; `dry_run` (optional boolean/string) |
| `delete` | `paths` array/string, alias `files`; `dry_run` (optional boolean/string), `confirm` (optional boolean/string) |
| `mark_for_delete` | `paths` array/string, alias `files`; `dry_run` (optional boolean/string), `confirm` (optional boolean/string) |
| `revert` | `paths` array/string, alias `files`; `dry_run` (optional boolean/string), `confirm` (optional boolean/string) |
| `revert_unchanged` | `paths` array/string, alias `files`; `dry_run` (optional boolean/string), `confirm` (optional boolean/string) |
| `list_opened` | `changelist` (optional string), `resolve_packages` (optional boolean/string), `limit` (optional integer in `1..5000`) |
| `map_depot_paths` | up to 5,000 `paths` as array/string, alias `files`; depot mappings use at most 40 bounded `p4 where` batches |

See `Plugins/Monolith/Docs/specs/SPEC_MonolithSourceControl.md` for the deep dive.

---

## water

Optional Water/Landscape discovery and actor/component listing. Read-only probes; does not mutate actors, splines, landscapes, or zones. **2 actions.**

| Action | Params |
|--------|--------|
| `get_status` | none |
| `list_bodies` | `limit` (optional integer), `actor_name_filter` (optional string) |

See `Plugins/Monolith/Docs/specs/SPEC_MonolithWater.md` for the deep dive.

---

## world_conditions

Optional WorldConditions inspection. Read-only probes; does not mutate condition queries or owners. The deeper inspection actions are gated by `bEnableWorldConditionsInspection`, which defaults off; call `get_status` first and expect `dependency_state=disabled` until the project enables the gate.

| Action | Params |
|--------|--------|
| `get_status` | none |
| `list_query_owners` | `path_filter` (optional string), `limit` (optional integer) |
| `describe_query` | `asset_path`, `query` (optional enum: `preconditions` default, `slot_selection_preconditions`), `slot_index` (required integer when `query=slot_selection_preconditions`) |
| `describe_condition_types` | `limit` (optional integer) |

See `Plugins/Monolith/Docs/specs/SPEC_MonolithWorldConditions.md` for the deep dive.

---

## gamefeatures

Optional GameFeature plugin discovery, inspection, validation, and guarded ActionSet/GameFeatureData instanced-action authoring. **15 actions.**

| Action | Params |
|--------|--------|
| `get_status` | none |
| `list_plugins` | `limit` (optional integer), `include_engine` (optional bool) |
| `find_game_feature_data` | `plugin_name` (optional string), `asset_path` (optional string) |
| `describe_game_feature_data` | `plugin_name` (optional string), `asset_path` (optional string), `include_action_properties` (optional bool), `editable_only` (optional bool), `include_values` (optional bool), `property_limit` (optional integer), `max_value_chars` (optional integer) |
| `list_action_classes` | `limit` (optional integer), `module` (optional string), `name_contains` (optional string), `include_abstract` (optional bool), `editable_only` (optional bool), `include_default_values` (optional bool), `property_limit` (optional integer), `max_value_chars` (optional integer) |
| `describe_action_set` | `action_set_path` (required string), `action_limit` (optional integer), `include_action_properties` (optional bool), `editable_only` (optional bool), `include_values` (optional bool), `property_limit` (optional integer), `max_value_chars` (optional integer) |
| `add_action_set_input_mapping` | `action_set_path` (required string), `mapping_context_path` (required string), `action_class_path` (optional string), `priority` (optional integer), `action_name` (optional string), `remove_null_actions` (optional bool), `save` (optional bool), `dry_run` (optional bool) |
| `set_primary_asset_scan` | `game_feature_data_path` (required string), `primary_asset_type` (required string), `asset_base_class` (optional string), `has_blueprint_classes` (optional bool), `is_editor_only` (optional bool), `directories` (optional array), `specific_assets` (optional array), `save` (optional bool), `dry_run` (optional bool) |
| `add_game_feature_data_input_mapping` | `game_feature_data_path` (required string), `mapping_context_path` (required string), `action_class_path` (optional string), `priority` (optional integer), `action_name` (optional string), `remove_null_actions` (optional bool), `save` (optional bool), `dry_run` (optional bool) |
| `add_game_feature_data_widgets` | `game_feature_data_path` (required string), `layouts` (optional array), `widgets` (optional array), `action_class_path` (optional string), `action_name` (optional string), property-name overrides (optional strings), `remove_null_actions` (optional bool), `save` (optional bool), `dry_run` (optional bool) |
| `add_game_feature_data_components` | `game_feature_data_path` (required string), `actor_class` (required string), `component_class` (required string), `action_class_path` (optional string), `action_name` (optional string), `client_component` (optional bool), `server_component` (optional bool), `addition_flags` (optional integer), `remove_null_actions` (optional bool), `save` (optional bool), `dry_run` (optional bool) |
| `add_game_feature_data_gameplay_cue_paths` | `game_feature_data_path` (required string), `directory_path` (optional string), `directory_paths` (optional array), `action_class_path` (optional string), `action_name` (optional string), `directory_array_property` (optional string), `remove_null_actions` (optional bool), `save` (optional bool), `dry_run` (optional bool) |
| `add_game_feature_data_abilities` | `game_feature_data_path` (required string), `actor_class` (required string), `ability_classes` (optional array), `attribute_sets` (optional array), `ability_sets` (optional array), `action_class_path` (optional string), `action_name` (optional string), `remove_null_actions` (optional bool), `save` (optional bool), `dry_run` (optional bool) |
| `remove_game_feature_data_action` | `game_feature_data_path` (required string), `action_index` (optional integer), `action_name` (optional string), `action_class_path` (optional string), `remove_all` (optional bool), `save` (optional bool), `dry_run` (optional bool) |
| `validate_plugin` | `plugin_name` (required string) |

---


## interchange

Optional Interchange framework discovery, validation, and asset import/export operations.

| Action | Params |
|--------|--------|
| `get_supported_formats` | none |
| `can_import` | `source_file` (required string), `destination_path` (optional string), `allow_external` (optional boolean) |
| `can_reimport` | `asset_path` (required string) |
| `get_import_data` | `asset_path` (required string) |
| `import_asset` | `source_file` (required string), `destination_path` (required string), `conflict_policy` (required string), `allow_external` (optional boolean), `confirm` (optional boolean), `dry_run` (optional boolean), `options` (optional object) |
| `import_assets` | `source_files` (required array), `destination_path` (required string), `conflict_policy` (required string), `allow_external` (optional boolean), `confirm` (optional boolean), `dry_run` (optional boolean), `options` (optional object) |
| `import_scene` | `source_file` (required string), `destination_path` (required string), `conflict_policy` (required string), `allow_external` (optional boolean), `confirm` (optional boolean), `dry_run` (optional boolean), `options` (optional object) |
| `import_mesh` | `source_file` (required string), `destination_path` (required string), `conflict_policy` (required string), `allow_external` (optional boolean), `confirm` (optional boolean), `dry_run` (optional boolean), `options` (optional object) |
| `import_skeletal_mesh` | `source_file` (required string), `destination_path` (required string), `conflict_policy` (required string), `allow_external` (optional boolean), `confirm` (optional boolean), `dry_run` (optional boolean), `options` (optional object) |
| `import_texture` | `source_file` (required string), `destination_path` (required string), `conflict_policy` (required string), `allow_external` (optional boolean), `confirm` (optional boolean), `dry_run` (optional boolean), `options` (optional object) |
| `import_audio` | `source_file` (required string), `destination_path` (required string), `conflict_policy` (required string), `allow_external` (optional boolean), `confirm` (optional boolean), `dry_run` (optional boolean), `options` (optional object) |
| `import_with_options` | `source_file` (required string), `destination_path` (required string), `conflict_policy` (required string), `allow_external` (optional boolean), `confirm` (optional boolean), `dry_run` (optional boolean), `options` (optional object) |
| `update_reimport_path` | `asset_path` (required string), `source_file` (required string), `source_file_index` (optional integer), `allow_external` (optional boolean), `confirm` (optional boolean), `dry_run` (optional boolean) |
| `reimport_asset` | `asset_path` (required string), `source_file` (optional string), `source_file_index` (optional integer), `allow_external` (optional boolean), `confirm` (optional boolean), `dry_run` (optional boolean) |
| `reimport_assets` | `asset_paths` (required array), `confirm` (optional boolean), `dry_run` (optional boolean) |
| `export_asset` | `asset_path` (required string), `file_path` (required string), `replace_existing` (optional boolean), `allow_external` (optional boolean), `confirm` (optional boolean), `dry_run` (optional boolean) |


## ndisplay

Optional nDisplay/DisplayCluster config discovery and validation. Read-only in this milestone.

| Action | Params |
|--------|--------|
| `get_status` | none |
| `list_config_assets` | `package_path` (optional string), `limit` (optional integer) |

## combograph

ComboGraph melee combo authoring. **13 actions.** **Conditional on `#if WITH_COMBOGRAPH`** — requires the ComboGraph marketplace plugin. Reflection-only (no direct C++ API linkage).

| Action | Params |
|--------|--------|
| `list_combo_graphs` | `path_filter?` |
| `get_combo_graph_info` | `asset_path` |
| `get_combo_node_effects` | `asset_path`, `node_index` |
| `validate_combo_graph` | `asset_path` |
| `create_combo_graph` | `save_path` |
| `add_combo_node` | `asset_path`, `animation_asset`, `node_type?`, `parent_node_index?`, `play_rate?` |
| `add_combo_edge` | `asset_path`, `from_node_index`, `to_node_index`, `input_action?`, `trigger_event?`, `transition_behavior?` |
| `set_combo_node_effects` | `asset_path`, `node_index`, `effects` (gameplay tag → container map) |
| `set_combo_node_cues` | `asset_path`, `node_index`, `cues` (gameplay tag → cue map) |
| `create_combo_ability` | `save_path`, `combo_graph?`, `initial_input?`, `parent_class?` |
| `link_ability_to_combo_graph` | `ability_path`, `combo_graph` |
| `scaffold_combo_from_montages` | `save_path`, `montages[]`, `input_action?`, `transition_behavior?` |
| `layout_combo_graph` | `asset_path`, `horizontal_spacing?`, `vertical_spacing?` |

See `Plugins/Monolith/Docs/specs/SPEC_MonolithComboGraph.md`.

---

## ai

Behavior Trees, State Trees, EQS, Blackboards, AI Controllers, Perception, Smart Objects, Navigation, Mass Entity, Zone Graph, runtime PIE inspection, and a deep library of scaffolds. The largest single conditional namespace — count is approximate, query `monolith_discover("ai")` for the live figure.

**New in v0.18.1:**
- `rebuild_navigation` (bounded async wait, optional save), `validate_nav_points` (per-point projection + per-pair path checks).
- **Runtime classes (no MCP-action delta):** `AMonolithBehaviorTreeAIController` (Blueprintable; runs a `UBehaviorTree` from `OnPossess`, plus a BlueprintCallable `StartBehaviorTree`), and three `UBTTaskNode` subclasses for locomotion control — `BTTask_SetMaxWalkSpeed`, `BTTask_SetCrouch`, `BTTask_RandomizeFloat` (placed via `add_bt_node`).
- **Fixes:** `reorder_bt_children` order now persists (`NodePosX`-based); `build_behavior_tree_from_spec` now links `UBehaviorTree::BlackboardAsset`.

**Conditional on `#if WITH_STATETREE` + `#if WITH_SMARTOBJECTS`** — projects missing either plugin register 0 AI actions.

> For full param schemas, call `describe_query("action_schema", target_namespace="ai", target_action="<name>")` (or `monolith_discover("ai", detail=true)`). Plain `monolith_discover("ai")` is terse — names + one-line descriptions only.

**Action categories:**

| Category | Actions | Examples |
|----------|---------|----------|
| Blackboards | 12 | `create_blackboard`, `get_blackboard`, `list_blackboards`, `delete_blackboard`, `duplicate_blackboard`, `add_bb_key`, `remove_bb_key`, `rename_bb_key`, `get_bb_key_details`, `batch_add_bb_keys`, `set_bb_parent`, `compare_blackboards` |
| Behavior Trees | 25 | `create_behavior_tree`, `get_behavior_tree`, `list_behavior_trees`, `delete_behavior_tree`, `duplicate_behavior_tree`, `set_bt_blackboard`, `list_bt_node_classes`, `add_bt_node`, `remove_bt_node`, `move_bt_node`, `add_bt_decorator`, `remove_bt_decorator`, `add_bt_service`, `remove_bt_service`, `set_bt_node_property`, `get_bt_node_properties`, `reorder_bt_children`, `add_bt_run_eqs_task`, `add_bt_smart_object_task`, `add_bt_use_ability_task`, `build_behavior_tree_from_spec`, `export_bt_spec`, `import_bt_spec`, `validate_behavior_tree`, `clone_bt_subtree`, `auto_arrange_bt`, `compare_behavior_trees`, `create_bt_task_blueprint`, `create_bt_decorator_blueprint`, `create_bt_service_blueprint`, `generate_bt_diagram`, `get_bt_graph` *(NEW Phase J F8)* |
| State Trees | 28 | `create_state_tree`, `get_state_tree`, `list_state_trees`, `delete_state_tree`, `duplicate_state_tree`, `compile_state_tree`, `set_st_schema`, `add_st_state`, `remove_st_state`, `rename_st_state`, `move_st_state`, `set_st_state_properties`, `add_st_task`, `remove_st_task`, `set_st_task_property`, `add_st_enter_condition`, `remove_st_enter_condition`, `add_st_transition`, `remove_st_transition`, `add_st_property_binding`, `remove_st_property_binding`, `get_st_bindings`, `get_st_bindable_properties`, `list_st_task_types`, `list_st_condition_types`, `add_st_transition_condition`, `add_st_consideration`, `configure_st_consideration`, `validate_state_tree`, `list_st_extension_types`, `add_st_extension`, `build_state_tree_from_spec`, `export_st_spec`, `generate_st_diagram`, `auto_arrange_st` |
| EQS | 21 | `create_eqs_query`, `get_eqs_query`, `list_eqs_queries`, `delete_eqs_query`, `duplicate_eqs_query`, `add_eqs_generator`, `remove_eqs_generator`, `configure_eqs_generator`, `add_eqs_test`, `remove_eqs_test`, `configure_eqs_test`, `configure_eqs_scoring`, `configure_eqs_filter`, `list_eqs_generator_types`, `list_eqs_test_types`, `list_eqs_contexts`, `validate_eqs_query`, `reorder_eqs_tests`, `build_eqs_query_from_spec`, `create_eqs_from_template` |
| AI Controllers | 8 | `create_ai_controller`, `get_ai_controller`, `list_ai_controllers`, `set_ai_controller_bt`, `set_pawn_ai_controller_class`, `set_ai_controller_flags`, `set_ai_team`, `get_ai_team`, `spawn_ai_actor`, `get_ai_actors` |
| Perception | 11 | `add_perception_component`, `get_perception_config`, `configure_sight_sense`, `configure_hearing_sense`, `configure_damage_sense`, `configure_touch_sense`, `remove_sense`, `add_stimuli_source_component`, `configure_stimuli_source`, `validate_perception_setup`, `get_ai_system_config`, `add_perception_to_actor` *(NEW Phase J F8)* |
| Smart Objects | 14 | `create_smart_object_definition`, `get_smart_object_definition`, `list_smart_object_definitions`, `delete_smart_object_definition`, `add_so_slot`, `remove_so_slot`, `configure_so_slot`, `add_so_behavior_definition`, `remove_so_behavior_definition`, `set_so_tags`, `add_smart_object_component`, `place_smart_object_actor`, `find_smart_objects_in_level`, `validate_smart_object_definition`, `create_so_from_template`, `duplicate_smart_object_definition` |
| Navigation | 19 | `get_nav_system_config`, `get_navmesh_config`, `set_navmesh_config`, `get_navmesh_stats`, `add_nav_bounds_volume`, `list_nav_bounds_volumes`, `build_navigation`, `get_nav_build_status`, `list_nav_areas`, `create_nav_area`, `add_nav_modifier_volume`, `add_nav_link_proxy`, `configure_nav_link`, `list_nav_links`, `find_path`, `test_path`, `project_point_to_navigation`, `get_random_navigable_point`, `navigation_raycast`, `configure_nav_agent`, `add_nav_invoker_component`, `get_crowd_manager_config`, `set_crowd_manager_config`, `analyze_navigation_coverage` |
| Runtime PIE | 13 | `runtime_get_bb_value`, `runtime_set_bb_value`, `runtime_clear_bb_value`, `runtime_get_bt_state`, `runtime_start_bt`, `runtime_stop_bt`, `runtime_get_bt_execution_path`, `runtime_get_perceived_actors`, `runtime_check_perception`, `runtime_report_noise`, `runtime_get_st_active_states`, `runtime_send_st_event`, `runtime_find_smart_objects`, `runtime_run_eqs_query` |
| Scaffolds | 21 | `hello_world_ai`, `scaffold_complete_ai_character`, `scaffold_perception_to_blackboard`, `scaffold_team_system`, `scaffold_patrol_investigate_ai`, `scaffold_enemy_ai`, `scaffold_eqs_move_sequence`, `create_bt_from_template`, `create_st_from_template`, `scaffold_ai_controller_blueprint`, `scaffold_companion_ai`, `scaffold_boss_ai`, `scaffold_ambient_npc`, `scaffold_horror_stalker`, `scaffold_horror_ambush`, `scaffold_horror_presence`, `scaffold_horror_mimic`, `scaffold_stealth_game_ai`, `scaffold_turret_ai`, `scaffold_group_coordinator`, `scaffold_flying_ai` |
| Validation / lint | 9 | `batch_validate_ai_assets`, `validate_ai_controller`, `get_ai_overview`, `list_ai_node_types`, `search_ai_assets`, `validate_ai_data_flow`, `find_eqs_references`, `find_so_references`, `lint_behavior_tree`, `lint_state_tree`, `detect_ai_circular_references`, `export_ai_manifest`, `get_ai_behavior_summary` |
| Mass Entity | 8 | `list_mass_entity_configs`, `get_mass_entity_config`, `create_mass_entity_config`, `add_mass_trait`, `remove_mass_trait`, `list_mass_traits`, `list_mass_processors`, `validate_mass_entity_config`, `get_mass_entity_stats` |
| Zone Graph | 3 | `list_zone_graphs`, `query_zone_lanes`, `get_zone_lane_info` |

> **Phase J F15:** all BT-related actions now return `{ "error": "<code>", "detail": "<human>" }` instead of mixed prose. Update your error parsers.

### `ai.add_perception_to_actor` · NEW in Phase J F8

Add `UAIPerceptionComponent` to ANY Actor BP (not just AIControllers) and configure senses in one call.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `actor_bp_path` | string | **required** | Actor Blueprint asset path (e.g. `/Game/Tests/BP_TestActor`) |
| `senses` | array | **required** | Array of sense names. Supported: `["Sight", "Hearing", "Damage"]`. |
| `sight_radius` | number | optional | Sight radius (cm). Only applied to Sight sense if present. Default: `1500` |
| `hearing_range` | number | optional | Hearing range (cm). Only applied to Hearing sense if present. Default: `3000` |

See `Plugins/Monolith/Docs/specs/SPEC_MonolithAI.md` for the deep dive — it's a long one.

---

## lyra

Reflection-based Lyra semantic inspection, validation, and guarded authoring. **21 actions.** The module resolves `/Script/LyraGame.*` classes reflectively and has no compile-time dependency on LyraGame, CommonGame, EOS, or Speed runtime modules.

| Action | Params |
|--------|--------|
| `get_status` | none |
| `describe_experience_graph` | `experience_path` |
| `validate_experience_bundle` | `experience_path`, optional expected members, `require_default_pawn_data`, `require_action_sets`, `validate_default_pawn_data`, `require_pawn_class`, `require_pawn_ability_sets`, `require_pawn_input_config`, `require_default_camera_mode`, `validate_action_sets`, `require_action_set_actions`, `disallow_null_actions`, `validate_action_classes`, `require_action_set_game_features`, `validate_game_feature_plugins` |
| `describe_user_facing_experience` | `user_facing_experience_path` |
| `validate_user_facing_experience` | `user_facing_experience_path`, optional `require_resolved_primary_assets` |
| `validate_map_default_experience` | `map_path`, optional `expected_experience_id`, `require_default_experience`, `require_lyra_world_settings`, `require_matching_experience` |
| `validate_user_facing_map_reachability` | `user_facing_experience_path`, optional `require_resolved_primary_assets`, `require_map_default_experience`, `require_lyra_world_settings`, `require_matching_map_default_experience` |
| `describe_gameplay_tag_domain` | optional `root_tag`, `include_children`, `max_tags` |
| `validate_game_phase_flow` | optional `root_tag`, `path_filter`, `phase_ability_paths`, `expected_phase_tags`, `disallow_duplicate_tags`, `max_assets` |
| `describe_team_setup` | optional `team_creation_component_class` |
| `describe_inventory_item` | `item_definition_path` |
| `describe_equipment_definition` | `equipment_definition_path` |
| `describe_weapon_definition` | `item_definition_path`, optional `require_equippable_fragment` |
| `describe_pawn_initialization_graph` | `pawn_data_path` |
| `validate_pawn_data_contract` | `pawn_data_path`, optional `require_pawn_class`, `require_ability_sets`, `require_input_config`, `require_default_camera_mode`, `expected_pawn_class` |
| `describe_character_part_graph` | optional `part_classes` |
| `validate_character_part_assets` | optional `part_classes`, `require_non_empty` |
| `set_experience_defaults` | `experience_path`, optional `default_pawn_data`, `action_sets`, `game_features_to_enable`, `dry_run`, `confirm`, `save`, `strict` |
| `add_experience_component_entry` | exactly one of `experience_path` or `action_set_path`; required `actor_class`, `component_class`; optional `action_name`, `client_component`, `server_component`, `addition_flags`, `dry_run`, `confirm`, `save` |
| `remove_experience_component_entry` | optional `experience_path`, `action_set_path`, `action_index`, `action_name`, `actor_class`, `component_class`, `component_index`, plus `dry_run`, `confirm`, `save` |
| `set_user_facing_experience` | `user_facing_experience_path`, optional `map_id`, `experience_id`, `extra_args`, tile fields, loading widget, session flags, `dry_run`, `confirm`, `save`, `strict` |

### `lyra.validate_experience_bundle`

Validate a Lyra Experience bundle as a semantic graph. In shallow mode it checks `DefaultPawnData`, composed `ActionSets`, and `GameFeaturesToEnable` against optional expected members. Deep flags inspect referenced PawnData fields, ActionSet action arrays, null entries, action class derivation from `UGameFeatureAction`, and optional GameFeature plugin descriptor presence/enabled state. The action is read-only and does not activate GameFeatures or mutate packages.

### `lyra.validate_map_default_experience`

Validate that a map loads as a `UWorld`, optionally uses `ALyraWorldSettings`, and exposes a reflected `DefaultGameplayExperience` that resolves to a `LyraExperienceDefinition` primary asset id. When `expected_experience_id` is supplied, `require_matching_experience=true` makes mismatches errors; otherwise they are reported as warnings.

### `lyra.validate_user_facing_map_reachability`

Validate a `ULyraUserFacingExperienceDefinition` hosting contract from `MapID` to a loadable map and optional map fallback experience. `ExperienceID` is compared to map `DefaultGameplayExperience` only as a warning by default because Lyra playlist hosting passes `ExperienceID` through the URL option before `ALyraWorldSettings` fallback is consulted; set `require_matching_map_default_experience=true` for strict audits.

### `lyra.describe_gameplay_tag_domain`

Describe a GameplayTag root such as `GamePhase`: root registration, source metadata, comments, child tags, child count, and truncation status. This is read-only and does not mutate tag dictionaries or assets.

### `lyra.validate_game_phase_flow`

Validate reflected `ULyraGamePhaseAbility` classes and their `GamePhaseTag` values against a root tag domain. The action inspects loaded native phase ability classes, Blueprint CDOs under a caller-provided `path_filter`, and explicit class/asset paths from `phase_ability_paths`; without `path_filter`, Blueprint asset discovery is skipped to avoid broad project loads. It reports invalid/missing tags, tags outside the root domain, duplicate exact tags, missing expected tags, and structured path errors. Duplicate exact phase tags are warnings by default because Lyra permits multiple phase abilities to share a phase tag; pass `disallow_duplicate_tags=true` to make them errors.

### Lyra static graph inspectors

`describe_team_setup`, `describe_inventory_item`, `describe_equipment_definition`, `describe_weapon_definition`, `describe_pawn_initialization_graph`, `validate_pawn_data_contract`, `describe_character_part_graph`, and `validate_character_part_assets` are read-only CDO/reflection diagnostics. They report exact missing fields and class incompatibilities without spawning teams, equipping items, initializing pawns, applying character parts, or mutating assets.

### Guarded write actions

`set_experience_defaults`, `add_experience_component_entry`, `remove_experience_component_entry`, and `set_user_facing_experience` require `dry_run=true` or `confirm=true`; `save=true` persists packages only after a confirmed clean mutation.

`add_experience_component_entry` targets exactly one Lyra Experience or explicit ExperienceActionSet. It validates the supplied actor/component subclasses, creates or reuses a reflected `GameFeatureAction_AddComponents`, adds a missing actor/component pair, and updates only `bClientComponent`, `bServerComponent`, and `AdditionFlags` when the pair already exists. A supplied `action_name` is created or reused as that exact instanced-object `FName` and never receives an automatic numeric suffix; a same-name direct child with an incompatible class, or a compatible orphan not referenced by the owner's `Actions` array, fails explicitly in both dry-run and commit. `MakeUniqueObjectName` is used only when `action_name` is omitted. With multiple AddComponents actions and no `action_name`, an action already owning the pair is preferred; duplicate matching pairs are reported as explicit errors. No-op calls do not dirty or save the package. Missing assets/properties are returned as explicit errors or check rows, never hidden behind fallback assets.

See `Plugins/Monolith/Docs/specs/SPEC_MonolithLyra.md`.

---

## logicdriver

Logic Driver Pro state machines: graph CRUD, node configuration, runtime PIE control, scaffolds, dialogue, text graph extraction. **66 actions.** **Conditional on `#if WITH_LOGICDRIVER`** — requires the Logic Driver Pro marketplace plugin. Reflection-only (precompiled marketplace plugin).

> For full param schemas, call `describe_query("action_schema", target_namespace="logicdriver", target_action="<name>")` (or `monolith_discover("logicdriver", detail=true)`). Plain `monolith_discover("logicdriver")` is terse — names + one-line descriptions only.

**Action categories:**

| Category | Actions | Examples |
|----------|---------|----------|
| State Machine assets | 5 | `create_state_machine`, `get_state_machine`, `list_state_machines`, `delete_state_machine`, `duplicate_state_machine` |
| Node Blueprints | 3 | `create_node_blueprint`, `get_node_blueprint`, `list_node_blueprints` |
| Inspection | 5 | `get_sm_structure`, `get_node_details`, `get_node_connections`, `find_nodes_by_type`, `find_nodes_by_class`, `get_sm_statistics` |
| Graph CRUD | 11 | `add_state`, `add_transition`, `add_conduit`, `add_state_machine_node`, `add_any_state_node`, `remove_node`, `set_node_properties`, `set_initial_state`, `set_end_state`, `set_node_class`, `rename_node`, `move_node`, `auto_arrange_graph` |
| Configuration | 6 | `configure_state`, `configure_transition`, `configure_conduit`, `configure_state_machine_node`, `set_transition_condition`, `set_state_tags`, `get_exposed_properties`, `set_exposed_property` |
| Compile | 1 | `compile_state_machine` |
| Runtime PIE | 7 | `runtime_get_sm_state`, `runtime_start_sm`, `runtime_stop_sm`, `runtime_restart_sm`, `runtime_switch_state`, `runtime_evaluate_transitions`, `runtime_get_state_history` |
| Spec / import / export | 5 | `build_sm_from_spec`, `export_sm_spec`, `export_sm_json`, `import_sm_json`, `compare_state_machines` |
| Scaffolds | 7 | `scaffold_hello_world_sm`, `scaffold_weapon_sm`, `scaffold_horror_encounter_sm`, `scaffold_game_flow_sm`, `scaffold_dialogue_sm`, `scaffold_quest_sm`, `scaffold_interactable_sm` |
| Project scan | 4 | `get_sm_overview`, `validate_state_machine`, `find_sm_references`, `find_node_class_usages` |
| Visualization | 2 | `visualize_sm_as_text` (ASCII / Mermaid / DOT), `explain_state_machine` |
| Components | 3 | `get_sm_component_config`, `add_sm_component`, `configure_sm_component` |
| Dialogue | 2 | `get_text_graph_content`, `get_dialogue_flow` |

The crown jewel is `build_sm_from_spec` — create a complete state machine (states, transitions, conduits, nested SMs, initial/end markers) from a single JSON spec, then compile, in one call.

See `Plugins/Monolith/Docs/specs/SPEC_MonolithLogicDriver.md`.

---

## online

EOS/OSSv2/CommonSession/CommonUser readiness diagnostics. **8 read-only actions.** Credential-bearing values are never returned; fields are reported as absent, empty, present, or `present_redacted`.

| Action | Params |
|--------|--------|
| `get_status` | none |
| `validate_eos_ossv2_config` | `platform_config_name` (optional string) |
| `describe_common_session_flow` | `user_facing_experience_path` (optional string) |
| `validate_common_session_schema` | `user_facing_experience_path` (optional string) |
| `validate_user_facing_session` | `user_facing_experience_path` (required string), `require_online_session`, `require_lobbies_for_online`, `require_lobby_schema`, `require_frontend_visible`, `require_resolved_primary_assets` (optional booleans) |
| `validate_common_user_initialization_contract` | none |
| `validate_common_user_privilege_matrix` | none |
| `diagnose_eos_accountportal_logs` | `log_root` (optional string), `max_results` (optional integer), `since_days` (optional integer) |

### `online.get_status`

Report OnlineSubsystem, OnlineServices, EOS, CommonUser, CommonGame, and Lyra online-facing plugin/module/reflection availability. Does not load assets, run PIE, or call EOS services.

### `online.validate_eos_ossv2_config`

Validate `OnlineServices`, `OnlineServices.EOS`, `EOSSDK`, `EOSSDK.Platform.<name>`, `OnlineSubsystemEOS.EOSSettings`, and SocketSubsystemEOS P2P settings across effective/project/custom config layers. Sensitive values such as ProductId, SandboxId, DeploymentId, ClientId, ClientSecret, and ClientEncryptionKey are redacted.

### `online.describe_common_session_flow`

Describe the CommonSession host and quick-play flow without creating a session. The action reports CommonSession classes/functions, host request defaults, OSSv1/OSSv2 branch rules, advertised `GameLobby` attributes, and optional Lyra UserFacingExperience host-request projection.

### `online.validate_common_session_schema`

Validate `OnlineServices.Lobbies` schema rows for expected Lyra/CommonSession attributes (`GAMEMODE`, `MAPNAME`, `MATCHTIMEOUT`, `SESSIONTEMPLATENAME`, `OSSv2`). When `user_facing_experience_path` is provided, also reports reflected UserFacingExperience session fields such as `MapID`, `ExperienceID`, `MaxPlayerCount`, `SessionMode`, lobby, voice, presence, and front-end flags.

### `online.validate_user_facing_session`

Validate a Lyra UserFacingExperience asset as a CommonSession hosting contract without creating a session. The action loads the asset read-only, reports the projected `UCommonSession_HostSessionRequest` fields (`OnlineMode`, effective lobby/voice/presence flags, `MapID`, `MaxPlayerCount`, advertised mode name, and injected `Experience` extra arg), checks CommonUser/CommonSession class availability, and optionally requires online mode, lobbies, lobby schema rows, front-end visibility, and AssetManager-resolved `MapID`/`ExperienceID`.

### `online.validate_common_user_initialization_contract`

Validate CommonUser/CommonGame plugin and module availability, reflected CommonUser/CommonSession/Lyra initialization classes, and Lyra local-player/game-instance config fields.

### `online.validate_common_user_privilege_matrix`

Validate the reflected CommonUser privilege, context, result, availability, and initialization-state enums plus Blueprint/CommonGame login entry points. The action reports the static CommonUser privilege-to-OSSv1/OSSv2 mapping for `CanPlay`, `CanPlayOnline`, text/voice communication, UGC, and cross-play without logging in or querying live privileges.

### `online.diagnose_eos_accountportal_logs`

Scan local `.log` files under `Saved/Logs` or a caller-supplied `log_root` for AccountPortal/auth patterns such as `client_has_no_application`, `invalid_client`, `EOS_Auth`, and `EOS_Invalid`. Returned message text is bounded and redacted.

See `Plugins/Monolith/Docs/specs/SPEC_MonolithOnline.md`.

---

## modular

Read-only ModularGameplay / ModularGameplayActors receiver lifecycle, AddComponentRequest target, and static extension-event source diagnostics. **4 actions.** The module uses reflection and lexical source scanning only and has no compile-time dependency on `ModularGameplay` or `ModularGameplayActors`.

| Action | Params |
|--------|--------|
| `get_status` | none |
| `describe_extension_receiver_lifecycle` | `actor_class` (optional string) |
| `validate_add_component_targets` | `actor_class`, `component_class`, optional `require_modular_receiver=true` |
| `trace_game_framework_extension_events` | optional `actor_class`, `source_root`, `source_roots`, `include_engine_modular_sources=false`, `include_monolith_source=false`, `max_results=200`, `max_files=2000`, `include_line_text=true` |

### `modular.get_status`

Report `ModularGameplay` and `ModularGameplayActors` plugin/module availability, reflected `UGameFrameworkComponentManager` and known ModularGameplayActors receiver classes, canonical extension events, and expected receiver lifecycle phases.

### `modular.describe_extension_receiver_lifecycle`

List known receiver classes and optionally classify an actor class as a known ModularGameplayActors receiver or `not_proven_by_reflection`. Arbitrary actor classes are not treated as ready unless they inherit from source-proven receiver types; use `modular.trace_game_framework_extension_events` to inspect project source call sites.

### `modular.validate_add_component_targets`

Validate an AddComponentRequest target pair: `actor_class` must be an `AActor`, `component_class` must be a `UActorComponent`, both must be concrete/non-deprecated, and by default the actor must be a known ModularGameplayActors receiver subclass. Pass `require_modular_receiver=false` to downgrade unproven receiver lifecycle to a warning.

### `modular.trace_game_framework_extension_events`

Perform a bounded lexical source trace for `UGameFrameworkComponentManager` receiver lifecycle calls, `AddExtensionHandler`, `AddComponentRequest`, extension-event sends, canonical event-name constants, and Lyra project event names such as `BindInputsNow` and `LyraAbilitiesReady`. Returns source roots, counts by code/role, candidate call-site rows, receiver classification, known lifecycle rows, checks, limitations, and an explicit static-trace contract. Does not run PIE, activate GameFeatures, register handlers, create component requests, or prove runtime reachability.

See `Plugins/Monolith/Docs/specs/SPEC_MonolithModular.md`.

---

## gameplay_message

Read-only GameplayMessageRouter channel, match-type, payload UScriptStruct, and static source-usage diagnostics. **5 actions.** The module uses reflection and lexical source tracing only and has no compile-time dependency on `GameplayMessageRuntime` or `GameplayMessageNodes`.

| Action | Params |
|--------|--------|
| `get_status` | none |
| `describe_listener_contract` | none |
| `validate_message_struct` | `message_struct`, optional `require_blueprint_type=false`, optional `require_no_object_references=false` |
| `validate_channel_contract` | `channel_tag`, optional `message_struct`, optional `match_type=ExactMatch`, optional `require_registered_tag=true`, optional `require_blueprint_type=false` |
| `trace_channel_usage` | optional `channel_tag`, `source_root`, `source_roots`, `include_monolith_source=false`, `include_engine_gameplay_message_sources=false`, `max_files=2000`, `max_results=500`, `include_line_text=false` |

### `gameplay_message.get_status`

Report `GameplayMessageRouter`, `GameplayMessageRuntime`, `GameplayMessageNodes`, `UGameplayMessageSubsystem`, async listener action, listener handle struct, and match enum availability.

### `gameplay_message.describe_listener_contract`

Describe the reflected listener/broadcast functions and the shared contract: broadcaster and listener must agree on the same payload `UScriptStruct`; `ExactMatch` receives the exact channel and `PartialMatch` includes child channels.

### `gameplay_message.validate_message_struct`

Validate a payload `UScriptStruct` path, optional `BlueprintType` metadata requirement, and optional no-object-reference requirement. Returns structured `ok`, `checks`, and `issues`.

### `gameplay_message.validate_channel_contract`

Validate a gameplay message channel tag, optional payload `UScriptStruct`, and match type. Missing tags are errors by default; pass `require_registered_tag=false` for preflight planning of tags that have not been added yet.

### `gameplay_message.trace_channel_usage`

Perform a bounded lexical source trace for GameplayMessageRouter broadcaster/listener call sites such as `BroadcastMessage`, `K2_BroadcastMessage`, `RegisterListener`, and `ListenForGameplayMessages`. Returns source roots, counts by role/code, raw matches, broadcaster/listener rows, a per-channel graph, and candidate issues for inferred payload mismatch, orphan broadcaster/listener, unresolved channel, and ExactMatch/PartialMatch ambiguity. This is read-only static analysis; it does not run PIE, register listeners, broadcast messages, or prove runtime reachability.

See `Plugins/Monolith/Docs/specs/SPEC_MonolithGameplayMessage.md`.

---

## settings

Read-only GameSettings registry/screen/setting, dynamic path, visual-data, and Enhanced Input player-mappable diagnostics. **6 actions.** The module has no compile-time dependency on `GameSettings`, `CommonUI`, `CommonInput`, or `LyraGame`; those contracts are inspected by reflection. The player-mappable validator uses public `EnhancedInput` asset APIs.

| Action | Params |
|--------|--------|
| `get_status` | none |
| `describe_registry_tree` | optional `screen_class`, optional `registry_class`, optional `setting_class` |
| `validate_setting_class_contract` | `setting_class`, optional `require_concrete=false`, optional `require_value_setting=false`, optional `require_collection=false` |
| `validate_data_source_bindings` | optional `getter_path`, optional `setter_path`, optional `dynamic_paths`, optional `require_getter=true`, optional `require_setter=false` |
| `validate_visual_data` | `asset_path`, optional `require_entry_widgets=false`, optional `require_detail_extensions=false` |
| `validate_player_mappable_input_settings` | optional `config_path`, optional `config_paths`, optional `context_path`, optional `context_paths`, optional semantic requirement toggles |

### `settings.get_status`

Report `GameSettings` plugin/module availability, known core classes, registry contract rows, and static data-source limitation notes.

### `settings.describe_registry_tree`

Describe reflected `UGameSettingScreen`, `UGameSettingRegistry`, `UGameSetting`, and `UGameSettingCollection` contracts. The action does not instantiate a runtime registry or local player.

### `settings.validate_setting_class_contract`

Validate a class path as a `UGameSetting` subclass, with optional concrete, value-setting, and collection-role requirements. Returns structured `ok`, `checks`, and `issues`.

### `settings.validate_data_source_bindings`

Validate supplied dotted dynamic getter/setter path shapes. `FGameSettingDataSourceDynamic` resolves against a `ULocalPlayer` at runtime, so this first slice reports static shape and runtime-resolution limits rather than reading private native data-source internals.

### `settings.validate_visual_data`

Load a `UGameSettingVisualData` object/asset read-only and report `EntryWidgetForClass`, `EntryWidgetForName`, `ExtensionsForClasses`, and `ExtensionsForName` map counts. Wrong object types return `ok=false` with structured issues.

### `settings.validate_player_mappable_input_settings`

Validate `UPlayerMappableInputConfig` and/or `UInputMappingContext` assets for GameSettings/Lyra-style key-binding screens. Reports context rows, default/profile mappings, mappable row counts, missing actions or keys, missing mapping/display names, and duplicate mapping names without registering configs, instantiating a local player, or reading live custom user bindings.

See `Plugins/Monolith/Docs/specs/SPEC_MonolithGameSettings.md`.

---

## loading

Read-only CommonLoadingScreen manager reason, processor candidate, settings/CVar, and optional Lyra loading handoff diagnostics. **4 actions.** The module uses reflection only and has no compile-time dependency on `CommonLoadingScreen` or `LyraGame`.

| Action | Params |
|--------|--------|
| `get_status` | optional `include_settings=true`, optional `include_cvars=true`, optional `include_lyra_handoff=false` |
| `describe_loading_processors` | optional `include_all_implementers=false`, optional `class_filter`, optional `max_objects=100` |
| `validate_loading_reason_contract` | optional `include_known_lyra=true`, optional `strict=false` |
| `trace_loading_screen_blockers` | optional `world_context=pie`, optional `include_settings=true`, optional `include_cvars=true`, optional `include_processor_candidates=true`, optional `include_lyra_handoff=true`, optional `max_candidates=64` |

### `loading.get_status`

Report CommonLoadingScreen plugin/module/classes, reflected settings CDO values, known CVars, config provenance, and optional Lyra handoff classes.

### `loading.describe_loading_processors`

Describe `ULoadingProcessInterface` and `ULoadingProcessTask`, and optionally list loaded implementer classes. Per-processor private reasons are not claimed because `ILoadingProcessInterface::ShouldShowLoadingScreen(FString&)` is native-only and not a UFUNCTION.

### `loading.validate_loading_reason_contract`

Validate manager reason getter reflection, processor interface availability, settings class, known CVars, and optional Lyra loading handoff classes. `strict=true` promotes optional Lyra absence to errors.

### `loading.trace_loading_screen_blockers`

Find a PIE/game world, ask the live `ULoadingScreenManager` for `GetDebugReasonForShowingOrHidingLoadingScreen()` when present, and return `pie_not_running` or `world_not_running` cleanly when no runtime world exists.

See `Plugins/Monolith/Docs/specs/SPEC_MonolithLoading.md`.

---

## asset

Asset lifecycle, hygiene, inspection, enrichment, and guarded package graph copy/remap. **18 actions.**

### `asset.import_texture_from_file`

Import an external image file as a UTexture2D asset with optional texture settings.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `source_path` | string | **required** | Absolute source image path. Aliases: source_file, file_path, path. |
| `destination` | string | **required** | Destination asset path, e.g. /Game/Textures/T_Example. Aliases: dest_path, destination_path. If asset_name is also supplied, destination/destination_path is treated as the output folder. |
| `asset_name` | string | optional | Optional asset name used with destination_path/destination as an output folder. |
| `settings` | object | optional | {compression, srgb, tiling, max_size, lod_group}. compression accepts TC_* names plus UI/UserInterface2D aliases. |
| `replace_existing` | boolean | optional | Overwrite existing destination asset. Default: `false` |
| `overwrite_policy` | string | optional | Compatibility alias for replace_existing: overwrite/replace -> true; fail/unique -> false. |

### `asset.import_texture_from_bytes`

Decode base64 image bytes and import them as a UTexture2D asset.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `destination` | string | **required** | Output texture asset path without `.uasset`. |
| `bytes_b64` | string | **required** | Base64-encoded image bytes. |
| `format_hint` | string | **required** | Image format: png, jpg, jpeg, bmp, exr, tga, hdr, tif, tiff, or dds. |
| `texture_role` | string | optional | Texture role preset: ui_icon, sprite, decal, basecolor, world_tile, normal, orm_mask, height, or emissive. |
| `settings` | object | optional | Texture settings such as compression_settings, srgb, mip_gen_settings, lod_group, address_x, address_y, alpha_bleed, alpha_from_edge_background, tile_seam_harmonize. |
| `conflict_policy` | string | optional | Existing destination handling: `fail`, `replace`, or `unique`. Default: `fail` |
| `save` | bool | optional | Save the imported texture asset. Default: `true` |
| `return_processed_png` | bool | optional | Return postprocessed imported pixels re-encoded as PNG. Default: `false` |

`fail` guarantees the exact requested package path and errors on any existing package instead of silently suffixing it. Only `unique` may call Unreal unique-name resolution and return a different `asset_path`. `replace` updates the exact existing top-level `UTexture2D` in the same UObject and package identity; another class, missing top-level asset, or redirected identity is rejected. Before mutation, replacement moves the complete original `FTextureSource` object, caller-facing settings, PostEdit/save side-effect values, CPU-copy helper identity, and package dirty state into an armed RAII snapshot. Complete running/cooked platform ownership is moved with it, preserving mip bulk and derived-data handles, VT/CPU copies, encoder metadata, hashes, and DDC/fetch keys rather than rebuilding from pixel bytes. A failure restores and verifies that state and re-notifies dependent materials; failed new creation removes the object/package header and sidecars. `LODGroup`, `CompressionSettings`, `SRGB`, `MipGenSettings`, `AddressX`, and `AddressY` changes emit property-specific editor callbacks. Whole-object source transfer supports long-lat, compressed, blocked/layered, and missing-running-platform-data textures; a null original platform pointer is restored to null. Replacement rejects active running/cooked platform builds, non-null `ResourceMem`, and aliased/duplicate cooked platform ownership before mutation. The response distinguishes intent and outcome through `requested_asset_path`, `asset_path`, `created`, `replaced`, and `conflict_policy`.

### `asset.import_font_family`

Import a font family from one or more TTF files as a composite UFont plus UFontFace assets.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `destination` | string | **required** | Output directory, e.g. /Game/UI/Fonts. |
| `family_name` | string | **required** | Composite UFont asset name. |
| `faces` | array | **required** | Typeface specs with `typeface` and absolute `source_path`; non-empty. |
| `loading_policy` | string | optional | LazyLoad, Stream, or Inline. Default: `LazyLoad` |
| `hinting` | string | optional | Default, Auto, AutoLight, Monochrome, or None. Default: `Default` |
| `save` | bool | optional | Save imported font assets. Default: `true` |

### `asset.save_asset`

Save a loaded asset package to disk and enforce persistence postconditions.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `asset_path` | string | **required** | Asset path to save |
| `verify_reload` | boolean | optional | Reload the clean package non-interactively and resolve the same asset/class again. Default: `false` |

Every successful save requires the package to be clean, its package file to exist, and the file size to be greater than zero. The response reports canonical path/class/package data plus `saved`, `was_dirty`, `dirty_after_save`, `exists_on_disk`, `filename`, and `file_size`. With `verify_reload=true`, `reloaded` and `reloaded_class` prove reload persistence; verification is rejected for `UWorld`, every package with `ContainsMap()`, and assets with an open editor.

### `asset.delete_assets`

Delete UE assets by path with package-segment guards and per-target postconditions.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `asset_paths` | string[] | **required** | Non-empty array of non-empty UE asset-path strings. Any malformed member rejects the request. |
| `allowed_prefixes` | string[] | optional | When supplied, this must be a non-empty array of non-empty strings. Each package must equal an allowed prefix or be a slash-delimited descendant. A prefix such as `/Game/Foo` does not permit `/Game/FooSibling`. |
| `dry_run` | boolean | optional | Validate and report without changing dirty/editor/string-table/package/source-control/file/registry state. Default: `false` |
| `force` | boolean | optional | Also evict package state and remove source-control-aware disk-only package residuals, including targets with no loaded object or AssetRegistry row. Default: `false` |
| `require_source_control` | boolean | optional | Before any mutation, require an enabled/available provider, valid per-file state, and verified delete or revert-add postconditions for every target. Any preflight failure aborts the whole request. Default: `false` |

The response includes one `targets[]` row per normalized package. Commit statuses are `deleted`, `residual_removed`, `already_absent`, or `failed`; dry-run statuses are `would_delete`, `would_remove_residual`, force-mode `already_absent`, or `not_found`. Committed deletion uses Unreal's editor garbage-collection keep flags, so evicting the requested package does not collect unrelated `RF_Standalone` assets or discard their unsaved edits. In force mode a target succeeds only when no loaded package, AssetRegistry entry, `.uasset`/`.umap`/`.utxt`/`.utxtmap` header, or `.uexp`/`.ubulk`/`.uptnl`/`.m.ubulk`/`.upayload` sidecar remains and no source-control failure occurred. When source control is enabled, provider/state unavailability or a failed/unsupported revert/delete blocks direct deletion and fails the target. `require_source_control=true` strengthens this into an all-target preflight: a missing/unknown provider state, unsupported delete/revert-add transition, or unverifiable postcondition prevents the first deletion instead of allowing a partial filesystem-only result. Each committed row reports `postcondition_met`, remaining package/registry state, `residual_files`, `source_control_failure`, and an optional `failure_reason`; top-level `success` is true only when every target succeeds.

### `asset.find_assets`

Fuzzy, scored, typo-tolerant search over the live AssetRegistry. Ranks assets by name/path/class with edit-distance typo tolerance; sees unsaved assets created this session. Results feed asset.inspect_asset. Distinct from offline project search.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `query` | string | **required** | Asset name or task text to search for. |
| `path` | string | optional | Content path scope, e.g. /Game or /Game/Characters. Default: `/Game` |
| `recursive` | boolean | optional | Recurse subfolders under path. Default: `true` |
| `class_names` | string[] | optional | Filter by asset class names (e.g. Texture2D, Blueprint) or /Script/Module.ClassName paths. |
| `limit` | integer | optional | Maximum ranked rows to return. Default: `20` |
| `threshold` | integer | optional | Minimum raw asset_fuzzy_v1 score to keep a match. |
| `include_tags` | boolean | optional | Also score selected AssetRegistry tag values. Default: `false` |
| `include_score_breakdown` | boolean | optional | Include reason, matched_tokens, distance, and per-field score breakdown. Default: `false` |
| `allow_transposition` | boolean | optional | Count adjacent character swaps as one typo edit for fuzzy token matching. Default: `true` |

### `asset.list_supported_asset_enrichers`

List read-only typed asset enrichers supported by Monolith. *No parameters.*

### `asset.inspect_asset`

Inspect an asset with typed read-only enrichment when supported.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `asset_path` | string | **required** | Asset path to inspect |
| `include_references` | boolean | optional | Include reflected object references. Default: `true` |
| `array_limit` | number | optional | Maximum items per reflected array. Default: `32` |

### `asset.inspect_assets_batch`

Inspect multiple assets with per-row success/error results.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `asset_paths` | string[] | **required** | Asset paths to inspect |
| `include_references` | boolean | optional | Include reflected object references. Default: `false` |
| `array_limit` | number | optional | Maximum items per reflected array. Default: `16` |

### `asset.validate_typed_asset`

Validate a typed asset and report warnings without mutation.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `asset_path` | string | **required** | Asset path to validate |
| `array_limit` | number | optional | Array cap used when evaluating large payload warnings. Default: `32` |

### `asset.validate_naming_conventions`

Scan assets by path and flag names not matching prefix conventions (SM_ for StaticMesh, SK_ for SkeletalMesh, M_ for Material, MI_ for MaterialInstance, T_ for Texture, BP_ for Blueprint). Supports custom rules.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `scan_path` | string | optional | Content path to scan (e.g. /Game/Environment). Default: `/Game` |
| `max_results` | integer | optional | Maximum violations to return. Default: `100` |
| `custom_rules` | object | optional | Custom prefix rules: {"ClassName": "Prefix_", ...} |

### `asset.batch_rename_assets`

Rename assets with find/replace, prefix add/remove, or suffix add/remove. Uses IAssetTools::RenameAssets for automatic reference fixup and redirector creation.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `asset_paths` | string[] | **required** | Array of asset paths to rename |
| `find` | string | optional | String to find in asset name |
| `replace` | string | optional | Replacement string |
| `add_prefix` | string | optional | Prefix to add |
| `remove_prefix` | string | optional | Prefix to remove |
| `add_suffix` | string | optional | Suffix to add |
| `remove_suffix` | string | optional | Suffix to remove |
| `dry_run` | boolean | optional | Preview renames without applying. Default: `false` |

### `asset.move_assets`

Move up to 512 exact source packages to exact destination packages through `IAssetTools::RenameAssets`. The action defaults to a no-load dry-run, never overwrites a registry, loaded, or on-disk destination, rejects duplicate sources/destinations and move chains/cycles, and verifies every committed destination/source/redirector postcondition.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `moves` | object[] | **required** | 1-512 objects shaped as `{ "source": "/Root/Old", "destination": "/OtherRoot/New" }`. Values are exact writable long package names, not object paths |
| `allowed_source_roots` | string[] | required | Non-empty source package-root allowlist. Matching is exact or a slash-delimited descendant, never a raw string-prefix sibling |
| `allowed_destination_roots` | string[] | required | Non-empty destination package-root allowlist. Matching is exact or a slash-delimited descendant |
| `dry_run` | boolean | optional | Validate with AssetRegistry/package-file state without loading or moving source assets. Default: `true` |
| `confirm` | boolean | optional | Required when `dry_run=false`. Default: `false` |
| `cleanup_redirectors` | boolean | optional | After a fully successful rename, delete at most 200 exact source redirector objects through the same guarded contract as `asset.cleanup_moved_redirectors`; never opens the AssetTools fixup report or rewrites references. Default: `false` |
| `accept_cdo_reference_warning` | boolean | optional | Explicitly accept only AssetRenameManager's exact CDO/config-reference warning during an interactive editor mutation. It does not accept any other modal and is rejected in commandlet or `-Unattended` contexts. Default: `false` |

Dry-run rows use `would_move`/`blocked`. Preflight rows include no-load AssetRegistry `hard_referencer_count` and `soft_referencer_count`. Committed rows use `moved`, `moved_with_redirector`, or `failed` and report destination registry/class/file checks, source-primary absence, redirector target validity, and `postconditions_met`. Redirector cleanup is attempted only when the entire `RenameAssets` batch reports success; a global or partial rename failure preserves Unreal-managed redirectors and reports `cleanup_status=skipped_due_to_rename_failure`. Cleanup captures and revalidates the complete redirector-object set in each source package, so Blueprint generated-class/CDO companion rows are permitted only when every source-package AssetRegistry row is a redirector and every companion targets the exact requested destination package. The exact requested source redirector must still target the exact requested destination object. All eligible redirector objects are submitted together to one `asset.delete_assets(require_source_control=true)` batch, capped at 200 objects, and source package/registry/file postconditions are checked afterward. `IAssetTools::RenameAssets` owns reference repair, package saving, and active-provider source-control branch/move behavior; the action does not emulate a filesystem move or overwrite collision.

### `asset.cleanup_moved_redirectors`

Idempotently finalize already-completed moves by deleting only exact, unreferenced source redirectors. This action performs no rename and no reference rewrite, and it never calls the modal `IAssetTools::FixupReferencers` report path.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `moves` | object[] | **required** | 1-200 completed move rows with exact `source` and `destination` package names. A row may also provide both `source_object_path` and `destination_object_path` for non-leaf object names. Destination packages/objects may be shared across rows; source packages/objects must remain unique. |
| `allowed_source_roots` | string[] | **required** | Non-empty, package-segment-bounded allowlist for source packages. |
| `allowed_destination_roots` | string[] | **required** | Non-empty, package-segment-bounded allowlist for destinations. Read-only roots are accepted because destinations are validated but never mutated. |
| `dry_run` | boolean | optional | Validate exact targets, complete source-package redirector sets, zero hard/soft referencers, destination integrity, and already-cleaned state without loading or deleting redirectors. Default: `true` |
| `confirm` | boolean | optional | Required when `dry_run=false`. Default: `false` |

For each source package, the exact requested source object must be a redirector to the exact destination object. Additional generated-class/CDO or other companion AssetRegistry rows are accepted only when every row is a redirector and every companion targets an object inside the same requested destination package; any non-redirector or foreign-package target blocks the whole preflight. The 200-item safety cap counts resolved redirector objects, not just input packages. Mutation additionally requires an editor game-thread call, a completed AssetRegistry scan, and an enabled/available source-control provider. One hardened `asset.delete_assets(require_source_control=true)` batch deletes the full captured object set, then the action rechecks destination class/file integrity and complete source registry/file removal. On a confirmed idempotent cleanup, already-cleaned sources succeed only when source control also proves the expected delete or revert-add state; dry-run remains observational. Results distinguish `success`, `already_cleaned`, `partial_failure`, and `failed`, and report package counts separately from `eligible_redirector_count`, `redirectors_submitted_for_delete`, and delete/postcondition counts.

### `asset.register_content_mount_points`

Safely register Unreal content mount points before package graph planning/copying. The action defaults to `dry_run=true`; process mount-table mutation requires `dry_run=false` and `confirm=true`. Each row uses `root`/`mount_point` plus exactly one resolver: direct `content_dir`, loaded `plugin_name`, or explicit `project_plugin_dir` under `ProjectPlugins`.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `mount_points` | object[] | **required** | Mount specs. Each entry accepts `root`/`mount_point` plus exactly one resolver: `content_dir`, `plugin_name`, or `project_plugin_dir`. `plugin_name` may derive `root` from the plugin mounted asset path |
| `dry_run` | boolean | optional | Preview without calling `FPackageName::RegisterMountPoint`. Default: `true` |
| `confirm` | boolean | optional | Required when `dry_run=false`. Default: `false` |
| `allow_override` | boolean | optional | Allow mounting a root that already resolves to a different local content directory. Default: `false` |
| `allow_core_mount_points` | boolean | optional | Allow `/Game/`, `/Engine/`, or `/Script/` specs. Default: `false` |
| `scan_asset_registry` | boolean | optional | After confirmed registration, scan registered roots in the live AssetRegistry. Default: `true` |
| `force_rescan` | boolean | optional | Force AssetRegistry rescan when `scan_asset_registry=true`. Default: `false` |
| `probe_packages` | string[] | optional | Check package existence after preflight/registration |

### `asset.plan_package_graph_copy`

Plan a root-remapped package graph copy from AssetRegistry dependencies without loading, copying, or saving assets.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `root_packages` | string[] | **required** | Source package paths that seed traversal |
| `root_remaps` | object | **required** | Source-root to destination-root map, e.g. `{"/ShooterMaps": "/SpeedMaps"}` |
| `dependency_kinds` | string[] | optional | Dependency kinds to follow: `hard`, `soft`. Default: both |
| `max_packages` | integer | optional | Traversal cap. Default: `512` |
| `strategy` | string | optional | Planning strategy. Only `registry_only_plan` is supported |
| `check_collisions` | boolean | optional | Report existing destination packages. Default: `true` |

### `asset.copy_package_graph_with_remap`

Guarded package graph duplication using the same root-remap plan contract. Requires `dry_run=true` or `confirm=true`; existing destination packages are never overwritten.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `root_packages` | string[] | **required** | Source package paths that seed traversal |
| `root_remaps` | object | **required** | Source-root to destination-root map |
| `dependency_kinds` | string[] | optional | Dependency kinds to follow: `hard`, `soft`. Default: both |
| `max_packages` | integer | optional | Traversal cap. Default: `512` |
| `check_collisions` | boolean | optional | Report existing destination packages. Default: `true` |
| `collision_policy` | string | optional | `fail_if_exists` or `skip_existing`; overwrite is unsupported. Default: `fail_if_exists` |
| `dry_run` | boolean | optional | Return copy report without duplicating assets. Default: `false` |
| `confirm` | boolean | optional | Required for mutation when `dry_run=false`. Default: `false` |
| `save` | boolean | optional | Save duplicated packages. Default: `true` |

### `asset.copy_package_graph_with_strategy`

Orchestrate a guarded package graph copy workflow with explicit copy strategy classification. The action always builds a read-only plan and `strategy_plan[]` first; confirmed runs execute `duplicate_asset`, `advanced_copy`, `header_patched_advanced_copy`, and opt-in `raw_package_file_copy` rows. Manual-copy selectors remain unsupported and hard-error instead of silently falling back. Workflows are `plan_only`, `copy_only`, `copy_fixup`, and `copy_fixup_validate`.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `root_packages` | string[] | **required** | Source package paths that seed traversal |
| `root_remaps` | object | **required** | Source-root to destination-root map |
| `workflow` | string | optional | `plan_only`, `copy_only`, `copy_fixup`, or `copy_fixup_validate`. Default: `copy_fixup_validate` |
| `strategy` | string | optional | Compatibility alias for either `workflow` or `copy_strategy` values |
| `copy_strategy` | string | optional | `auto`, `duplicate_asset`, `advanced_copy`, `raw_package_file_copy`, or `header_patched_advanced_copy`. Default: `auto` |
| `dependency_kinds` | string[] | optional | Dependency kinds to follow during planning: `hard`, `soft`. Default: both |
| `max_packages` | integer | optional | Traversal safety cap for plan/copy. Default: `512` |
| `fixup_max_packages` | integer | optional | Reference-fixup safety cap. Default: `1000` |
| `closure_max_packages` | integer | optional | Dependency-closure validation safety cap. Default: `1000` |
| `check_collisions` | boolean | optional | Report existing destination packages. Default: `true` |
| `collision_policy` | string | optional | `fail_if_exists` rejects existing destinations; `skip_existing` leaves existing destinations untouched and excludes them from fixup/cleanup/closure. Default: `fail_if_exists` |
| `header_patched_roots` / `header_patched_packages` | string[] | optional | Select packages that should use `header_patched_advanced_copy` when `copy_strategy=auto` |
| `raw_package_roots` / `raw_package_packages` | string[] | optional | Select packages that should use `raw_package_file_copy` when `copy_strategy=auto` |
| `manual_copy_roots` / `manual_copy_packages` | string[] | optional | Select packages known to require manual single-object duplication |
| `allow_raw_package_copy` | boolean | optional | Required opt-in before `raw_package_file_copy` rows can execute. Default: `false` |
| `cleanup_redirectors` | boolean | optional | After reference fixup, run one exact affected-package cleanup batch through `asset.cleanup_moved_redirectors` before closure validation. No modal fixup report is opened. Default: `false` |
| `allowed_external_roots` | string[] | optional | External roots allowed during dependency-closure validation |
| `legacy_source_roots` | string[] | optional | Source roots that should not remain referenced; defaults to `root_remaps` sources when omitted |
| `require_targets` | boolean | optional | Fail fixup when a remapped reference target package is missing. Default: `true` |
| `run_fixup_on_dry_run` / `run_closure_on_dry_run` | boolean | optional | Scan existing destination packages during dry-run; otherwise post-copy phases are only planned. Default: `false` |
| `dry_run` | boolean | optional | Return strategy/copy/fixup/closure plan without mutation. Default: `false` |
| `confirm` | boolean | optional | Required for mutation when `dry_run=false`. Default: `false` |
| `save` | boolean | optional | Save duplicated or changed packages. Default: `true` |
| `strict` | boolean | optional | Treat fixup blockers as errors. Default: `true` |

When `cleanup_redirectors=true`, the workflow builds one cleanup request for the exact affected copied packages, reuses the source/destination object mapping established by the copy plan, and delegates deletion to `asset.cleanup_moved_redirectors`. That action's full-package redirector validation, 200-object cap, zero hard/soft referencer requirement, source-control preflight, idempotent already-cleaned handling, and postconditions remain authoritative. The workflow never calls `IAssetTools::FixupReferencers`; a cleanup failure is surfaced as a workflow failure instead of opening a report window or silently leaving a partial result.

### `asset.fixup_copied_references`

Guarded reflected hard/soft reference rewrite inside copied destination packages, remapping references from source roots to destination roots. Requires `dry_run=true` or `confirm=true`.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `root_remaps` | object | **required** | Source-root to destination-root map |
| `destination_roots` | string[] | optional | Destination roots to scan; defaults to `root_remaps` destinations |
| `package_paths` | string[] | optional | Explicit destination packages to scan instead of scanning roots |
| `max_packages` | integer | optional | Scan cap. Default: `1000` |
| `require_targets` | boolean | optional | Treat missing remapped target packages as blockers. Default: `true` |
| `dry_run` | boolean | optional | Return reference rewrite report without mutating assets. Default: `false` |
| `confirm` | boolean | optional | Required for mutation when `dry_run=false`. Default: `false` |
| `save` | boolean | optional | Save changed packages. Default: `true` |
| `strict` | boolean | optional | Treat load/fixup blockers as errors. Default: `true` |

### `asset.validate_dependency_closure`

Validate that destination packages do not depend on disallowed external roots or legacy source roots after a package graph copy/remap.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `destination_roots` | string[] | **required** | Destination roots whose package closure should be validated |
| `package_paths` | string[] | optional | Specific destination packages to validate; omitted scans `destination_roots` |
| `allowed_external_roots` | string[] | optional | External roots allowed in dependencies, e.g. `/Script`, `/Engine` |
| `legacy_source_roots` | string[] | optional | Source roots that should not remain referenced |
| `dependency_kinds` | string[] | optional | Dependency kinds to validate: `hard`, `soft`. Default: both |
| `max_packages` | integer | optional | Validation cap. Default: `1000` |

---

## audio

Sound Cue + MetaSound graph CRUD + on-disk document introspection, attenuation/class/mix/submix/concurrency, batch ops, Sound Cue templates, perception bindings, and a small batch of test helpers. **98 actions.**

> For full param schemas, call `describe_query("action_schema", target_namespace="audio", target_action="<name>")` (or `monolith_discover("audio", detail=true)`). Plain `monolith_discover("audio")` is terse — names + one-line descriptions only. MetaSound graph + document actions are conditional on `#if WITH_METASOUND` — projects without MetaSound get Sound Cue + CRUD + batch actions but no MetaSound graph building or document walk. The 12 document-introspection actions (PR #18, v0.14.10) read **on-disk document state** for arbitrary assets without an active builder session — distinct from the Builder-side graph actions which read live builder state during mutation.

**Action categories:**

| Category | Actions | Examples |
|----------|---------|----------|
| Sound assets CRUD | 15 | `create_sound_attenuation`, `get_attenuation_settings`, `set_attenuation_settings`, `create_sound_class`, `get_sound_class_properties`, `set_sound_class_properties`, `create_sound_mix`, `get_sound_mix_settings`, `set_sound_mix_settings`, `create_sound_concurrency`, `get_concurrency_settings`, `set_concurrency_settings`, `create_sound_submix`, `get_submix_properties`, `set_submix_properties` |
| Test helpers | 1 | `create_test_wave` *(NEW Phase J F18)* — procedurally synthesizes a 16-bit mono sine `USoundWave` for tests with zero asset deps |
| Asset listing / search | 4 | `list_audio_assets`, `search_audio_assets`, `get_sound_wave_info`, `get_audio_stats` |
| Hierarchy | 2 | `get_sound_class_hierarchy`, `get_submix_hierarchy` |
| References / unused | 4 | `find_audio_references`, `find_unused_audio`, `find_sounds_without_class`, `find_unattenuated_sounds` |
| Batch ops | 9 | `batch_assign_sound_class`, `batch_assign_attenuation`, `batch_set_compression`, `batch_set_submix`, `batch_set_concurrency`, `batch_set_looping`, `batch_set_virtualization`, `batch_rename_audio`, `batch_set_sound_wave_properties` |
| Templates | 1 | `apply_audio_template` |
| Sound Cue graph | 10 | `create_sound_cue`, `get_sound_cue_graph`, `add_sound_cue_node`, `remove_sound_cue_node`, `connect_sound_cue_nodes`, `set_sound_cue_first_node`, `set_sound_cue_node_property`, `list_sound_cue_node_types`, `find_sound_waves_in_cue`, `validate_sound_cue` |
| Sound Cue spec / templates | 8 | `build_sound_cue_from_spec`, `create_random_sound_cue`, `create_layered_sound_cue`, `create_looping_ambient_cue`, `create_distance_crossfade_cue`, `create_switch_sound_cue`, `duplicate_sound_cue`, `delete_audio_asset` |
| Preview | 3 | `preview_sound`, `stop_preview`, `get_sound_cue_duration` |
| Perception bindings | 4 | `bind_sound_to_perception`, `unbind_sound_from_perception`, `get_sound_perception_binding`, `list_perception_bound_sounds` |
| MetaSound assets | 3 | `create_metasound_source`, `create_metasound_patch`, `create_metasound_preset` |
| MetaSound graph | 12 | `add_metasound_node`, `remove_metasound_node`, `connect_metasound_nodes`, `disconnect_metasound_nodes`, `add_metasound_input`, `add_metasound_output`, `set_metasound_input_default`, `add_metasound_interface`, `get_metasound_graph`, `list_metasound_connections`, `add_metasound_variable`, `set_metasound_node_location` |
| MetaSound discovery (Builder-side) | 6 | `list_available_metasound_nodes`, `get_metasound_node_info`, `find_metasound_node_inputs`, `find_metasound_node_outputs`, `get_metasound_input_names` |
| MetaSound document introspection (v0.14.10, PR #18) | 12 | `list_metasounds`, `list_metasound_documents`, `get_metasound_document`, `get_metasound_summary`, `inspect_metasound_node_instance`, `get_metasound_document_connections`, `get_metasound_document_variables`, `get_metasound_user_parameters`, `search_metasound_document_nodes`, `get_metasound_info`, `get_metasound_dependencies`, `validate_metasound` |
| MetaSound spec / templates | 6 | `build_metasound_from_spec`, `create_oneshot_sfx`, `create_looping_ambient_metasound`, `create_synthesized_tone`, `create_interactive_metasound`, `create_metasound_preset` |

`audio.list_available_metasound_nodes` defaults to `detail_level=minimal`: rows include node identity plus `input_count`/`output_count`, and the response includes `matched_count`, `returned_count`, `limit`, and `truncated`. Use `detail_level=standard` to include full per-node `inputs[]` and `outputs[]`.

### `audio.create_test_wave` · NEW in Phase J F18

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `asset_path` | string | **required** | Destination asset path under `/Game/` (Alias: `path`) |
| `frequency_hz` | number | optional | Sine frequency (20.0 to 20000.0). Default: `440.0` |
| `duration_seconds` | number | optional | Clip length (0.05 to 5.0). Default: `0.5` |
| `sample_rate` | integer | optional | Allowlist `{22050, 44100, 48000}`. Default: `44100` |
| `amplitude` | number | optional | Peak amplitude in `(0.0, 1.0]`. Default: `0.5` |

**Returns:** `{ asset_path, samples_written, duration_actual_seconds, frequency_hz, sample_rate, amplitude }`

### `audio.bind_sound_to_perception`

Stamp a `UMonolithSoundPerceptionUserData` onto a `USoundBase` (Cue / MetaSoundSource / Wave). Runtime `UWorldSubsystem` fires `AActor::MakeNoise` when this sound plays through a `UAudioComponent` owned by an actor.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `asset_path` | string | **required** | `USoundBase` asset path |
| `loudness` | number | optional | `FAINoiseEvent::Loudness` multiplier. Default: `1.0` |
| `max_range` | number | optional | Per-event max range in cm; `0` = use listener's `HearingRange`. Default: `0` |
| `tag` | string | optional | `FName` tag for downstream filtering |
| `sense_class` | string | optional | Sense class name (only `"Hearing"` supported in v1) |
| `enabled` | boolean | optional | Master switch. Default: `true` |
| `fire_on_fade_in` | boolean | optional | Also fire on `FadingIn`, not just `Playing`. Default: `true` |
| `require_owning_actor` | boolean | optional | Skip 2D / no-owner sounds. Default: `true` |

> **Phase J F11:** `loudness <= 0`, `max_range < 0`, and unknown `sense_class` values now reject up-front instead of writing junk userdata.

See `Plugins/Monolith/Docs/specs/SPEC_MonolithAudio.md`.

---

## leveldesign

Horror/encounter design, AI-tactical analysis, acoustic design analysis, accessibility reports, and co-op spatial balance review. Split out of `mesh` on 2026-05-20.

| Action | Params | Notes |
|--------|--------|-------|
| | | |

See `Plugins/Monolith/Docs/specs/SPEC_MonolithLevelDesign.md` for full schemas and details.

---

## level_sequence

Level Sequence inspection — binding inventory (legacy possessables/spawnables + UE 5.7 custom bindings), Director Blueprint functions/variables, event-track bindings, and cross-sequence reverse lookup. **8 actions.** Backed by a dedicated SQLite indexer (`MonolithLevelSequence` module, PR #45). Read-only.

| Action | Key params | Notes |
|--------|-----------|-------|
| `ping` | — | Smoke test; returns `{status:ok, module:MonolithLevelSequence}` |
| `list_directors` | `asset_path_filter?` (glob) | Level Sequences with a Director BP + function/variable counts |
| `get_director_info` | `asset_path` | Function counts by kind (`user`/`custom_event`/`sequencer_endpoint`), variable count, event-binding counts, sample of up to 10 functions |
| `list_director_functions` | `asset_path`, `kind?` | Own functions filtered by `user`/`custom_event`/`sequencer_endpoint`/`event`/`all` (own-only, matching the blueprint convention) |
| `list_director_variables` | `asset_path` | Director `NewVariables` (name + K2-schema type) in declaration order |
| `list_event_bindings` | `asset_path` | Event-track bindings grouped by binding GUID (possessable/spawnable/master) + the sections that fire Director functions |
| `list_bindings` | `asset_path`, `kind?` | **ALL** bindings regardless of event tracks — `possessable`/`spawnable`/`replaceable`/`custom`. Catches UE 5.7 `UMovieSceneCustomBinding` rows that `list_event_bindings` misses |
| `find_director_function_callers` | `function_name`, `asset_path_filter?` (glob) | Cross-sequence reverse lookup: every event-track section across the project that fires a given Director function |

See `Plugins/Monolith/Docs/specs/SPEC_MonolithLevelSequence.md`.

---

## movie_render

Movie Render Queue (MRQ) operations for Level Sequences. Backed by `MonolithLevelSequence` module.

| Action | Key params | Notes |
|--------|-----------|-------|
| `get_queue` | — | Get the current MRQ queue |
| `load_queue` | `asset_path` | Load a saved Queue asset |
| `save_queue` | `asset_path` | Save the current queue |
| `add_job` | `sequence_path`, `map_path` | Add a job to the queue |
| `duplicate_job` | `job_index` | Duplicate a job |
| `delete_job` | `job_index` | Delete a job |
| `delete_all_jobs` | — | Clear the queue |
| `set_job_index` | `job_index`, `new_index` | Move a job |
| `list_settings` | `job_index` | List job settings |
| `render_queue` | `executor?` | Start rendering |
| `is_rendering` | — | Check if MRQ is active |
| `render_progress` | — | Get ongoing render progress |
| `cancel_render` | — | Cancel the active render |

See `Plugins/Monolith/Docs/specs/SPEC_MonolithLevelSequence.md`.

---

## bulk_fill

Reflection-walker bulk property fill across 11 in-tree per-namespace adapters, with optional sibling adapters when installed. **2 actions.** Framework dispatcher in `MonolithCore` (0.15.0); each adapter self-registers from its owning module — zero compile-time linkage from core into adapter modules.

### `bulk_fill_query.apply`

Apply a JSON-tree fill to an asset via the target namespace's adapter. Walks the target's reflection schema, supports preview-without-persist and strict promotion of silent drops.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `target_namespace` | string | **required** | Adapter namespace: `blueprint`, `gas`, `ui`, `ai`, `niagara`, `material`, `audio`, `mesh`, `animation`, `logicdriver`, `combograph` (plus the sibling `inventory` adapter when present). Aliases: `namespace`, `domain` |
| `target` | string | **required** | Asset path or adapter-defined target (e.g. `/Game/Items/DA_HealingPotion`) |
| `tree` | object | **required** | Nested JSON of properties to walk against the target's reflection schema |
| `dry_run` | boolean | optional | Validate only — emit would-be writes but do not persist. Default: `false` |
| `strict` | boolean | optional | Promote silent drops / clamps / unknown-fields to hard errors. Default: `false` |

Returns an `FDryRunReport`-shaped result (`FieldWrites` / `SilentDrops` / `Clamps` / `Errors`).

### `bulk_fill_query.list_namespaces`

List `target_namespace` values the bulk_fill registry currently knows about (one row per registered adapter). *No parameters.*

---

## describe

Read-only schema introspection for the same adapter registry, plus action-param introspection. **3 actions.** Companion to `bulk_fill` (0.15.0).

### `describe_query.schema`

Return a rich `FSchemaDescriptor` tree (type names, ImportText forms, enum-value lists, clamp ranges, nested children) for a namespace, asset, or action via its namespace adapter.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `target_namespace` | string | optional | Adapter namespace whose schema should be introspected. Aliases: `namespace`, `domain`. If omitted or unknown, the action returns `match_status=namespace_index` or `match_status=no_adapter` guidance with `available_namespaces` and `next_actions` instead of a hard validation error. |
| `target` | string | optional | Asset path or action name to describe. Omit it to request the adapter's namespace-level writable-shape descriptor. |

### `describe_query.list_targets`

List the asset paths / action names the describe adapter can introspect for a given `target_namespace`, when that adapter supports inventory. Target listing is optional: adapters may return `inventory_supported=false` / `optional_inventory_not_implemented`; use `bulk_fill_query.list_namespaces` and `describe_query.schema` to determine adapter support.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `target_namespace` | string | optional | Adapter namespace whose introspection inventory should be listed. Aliases: `namespace`, `domain`. If omitted or unknown, the action returns registered namespace guidance with `available_namespaces`, per-namespace `inventory_supported`, and `next_actions`. |

### `describe_query.action_schema`

Return a registered ACTION's param schema (names, types, required, defaults, aliases, descriptions) by `(target_namespace, target_action)` — so callers stop trial-and-erroring param names.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `target_namespace` | string | **required** | Namespace that owns the action (e.g. `blueprint`, `ui`). Aliases: `namespace`, `domain` |
| `target_action` | string | **required** | Action name whose param schema to return (e.g. `add_nodes_bulk`). Alias: `action` |

See the per-system SPECs' "Bulk Fill & Describe Surface" sections for each adapter's `fill_kind` catalogue.

---

## decision

**New v0.17.0 (Reflection Intelligence, Phase 1).** Architectural decision records mined from the project's markdown corpora (specs, plans, `CHANGELOG.md`, `.claude/rules/`) into `decision_records` + `decision_supersedes` SQLite tables on `EngineSource.db`. Zero LLM calls, zero network. Three heuristic tiers with distinct confidence floors: YAML frontmatter `decision: true` / `status:` (0.90), `## ADR-N` / `## Architectural Decision` headers (0.85), and a markdown header followed within 8 lines by a paragraph containing `because` / `rationale` / `evidence` / `decision:` (0.65). All 5 actions are read-only + idempotent and participate in universal response shaping (`_fields` / `_omit` / `_compact_json`). **5 actions.**

> RI does NOT open its own handle to `EngineSource.db`. UE 5.7's SQLite is built with `SQLITE_OS_OTHER=1` and a custom `unreal-fs` VFS that allows only ONE open of a file per process; a second open returns `SQLITE_IOERR`. RI borrows `UMonolithSourceSubsystem`'s already-open handle (`FMonolithSourceDatabase::GetRawHandle()` / `GetLock()`): read-path adapters borrow it under a game-thread-only contract, write-path bootstrap indexers run under `FScopeLock`.

### `decision_query.list_decisions`

List architectural decisions filtered by source-path substring and minimum heuristic confidence. Cursor-paginated.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `path_filter` | string | optional | Substring match against `source_path` (project-relative). `\` → `/` rewritten by dispatcher with a surfaced warning. Default: `""` |
| `min_confidence` | number | optional | Floor in `[0, 1]`. Per-call value wins over the settings default (`0.6`). Default: `0.6` |
| `status` | string | optional | Exact match — `open`, `accepted`, `superseded`, `deprecated`, `draft`. Default: `""` |
| `limit` | integer | optional | Page size. Hard cap `200`. Default: `50` |
| `cursor` | string | optional | Opaque base64+JSON cursor |

**Returns:** `{ "decisions": [ { "decision_id", "title", "status", "source_path", "source_line", "confidence", "rationale", "source_mtime" } ], "total_estimate": N, "next_cursor": "<opaque>" }`. `total_estimate` is emitted on page 0 only.

### `decision_query.get_decision`

Fetch one record by stable id.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `decision_id` | string | **required** | Stable id of the form `<forward-slashed-path>#<header-anchor>` |

**Returns:** `{ "decision": <row-or-null> }` — `null` when the id is unknown.

### `decision_query.list_stale`

List decisions whose source markdown hasn't been modified within `max_age_days`. Useful for spec-drift detection. Cursor-paginated, ordered oldest-mtime-first.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `max_age_days` | integer | **required** | Positive only. Compared against source-file mtime in UTC |
| `path_filter` | string | optional | Substring match. Default: `""` |
| `limit` | integer | optional | Hard cap `200`. Default: `50` |
| `cursor` | string | optional | Opaque cursor |

**Returns:** `{ "stale_decisions": [ /* row objects */ ], "cutoff_unix": N, "next_cursor": "<opaque>" }`. Rows with `source_mtime = 0` (mtime unavailable) are excluded.

### `decision_query.find_supersession_chain`

Walk supersedes edges outward from a starting decision — the ordered chain of decisions the start id transitively supersedes. Cycle-protected.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `decision_id` | string | **required** | Start of the walk |
| `depth` | integer | optional | Max traversal depth. Hard cap `50`. Default: `10` |

**Returns:** `{ "start": "<id>", "chain": [ { "from", "to", "depth" } ], "truncated": false }`. `truncated: true` means the walk hit `depth` with frontier nodes remaining.

### `decision_query.find_referent_decisions`

Inverse of `find_supersession_chain` — list decisions that explicitly supersede the given id.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `decision_id` | string | **required** | The decision whose referents to list |

**Returns:** `{ "decision_id": "<id>", "referent_decisions": [ /* full row objects */ ] }`. Rows ordered by `source_path, source_line`.

---

## risk

**New v0.17.0 (Reflection Intelligence, Phase 2).** Repo-level risk signals mined from git history + LOC sweeps + conditional-gate regex scans across up to six nested git repos. Deterministic — no LLM, no embeddings, no network. Writes into `git_file_churn`, `git_cochange_pairs`, `risk_hotspot_scores`, and `reflect_conditional_gates` on `EngineSource.db`. Hotspot score is a traceable blend: `0.6 * normalised_churn + 0.4 * normalised_loc`, normalised per-repo. All 5 actions are read-only + idempotent. **5 actions.** (The Module-Dep Reality Audit also shipped in Phase 2 — it's registered under `source` as `source_query("audit_module_dep_reality")`, documented in the [source](#source) section.)

### `risk_query.get_hotspot_score`

Fetch the hotspot score for a single file path.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `file_path` | string | **required** | Project-relative or repo-relative path. `\` → `/` rewritten with a surfaced warning |
| `repo_path` | string | optional | When omitted, searches all indexed repos and returns the first match. Default: `""` |

**Returns:** `{ "score": <number-or-null>, "normalised_churn", "normalised_loc", "loc", "repo_path" }` — `score` is `null` when the file isn't in the index.

### `risk_query.get_cochange_pairs`

List files that frequently change in the same commits as the given file. Cursor-paginated.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `file_path` | string | **required** | Anchor file |
| `repo_path` | string | optional | Optional repo scope. Default: `""` |
| `min_commits` | integer | optional | Lower bound on `commit_count` per pair (filters one-off co-touches). Default: `2` |
| `limit` | integer | optional | Hard cap `200`. Default: `50` |
| `cursor` | string | optional | Opaque cursor |

**Returns:** `{ "anchor": "<path>", "pairs": [ { "partner", "commit_count" } ], "total_estimate": N, "next_cursor": "<opaque>" }`.

### `risk_query.get_file_churn`

Per-file churn record — commit count and line-delta totals.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `file_path` | string | **required** | Target file |
| `repo_path` | string | optional | Optional repo scope |

**Returns:** `{ "churn": <row-or-null> }` — row includes `commit_count`, `lines_added`, `lines_deleted`, `first_commit_ts`, `last_commit_ts`.

### `risk_query.get_release_window_hotspots`

List files whose hotspot score exceeds a threshold, descending. Designed for release-readiness queries. Cursor-paginated.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `threshold` | number | optional | Floor in `[0, 1]`. Default: `0.7` |
| `repo_path` | string | optional | Optional repo scope. Default: `""` |
| `limit` | integer | optional | Hard cap `200`. Default: `50` |
| `cursor` | string | optional | Opaque cursor |

**Returns:** `{ "hotspots": [ { "file_path", "score", "normalised_churn", "normalised_loc", "loc", "repo_path" } ], "total_estimate": N, "next_cursor": "<opaque>" }`.

### `risk_query.list_conditional_gates`

List `#if WITH_*` macros, `bHas*` 3-location probe variables, and `MONOLITH_RELEASE_BUILD` bypass branches across the project. Cursor-paginated.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `module_filter` | string | optional | Substring match against module name. Default: `""` |
| `gate_kind` | string | optional | Exact match — `with_macro`, `bhas_probe`, `release_bypass`. Default: `""` |
| `limit` | integer | optional | Hard cap `500`. Default: `100` |
| `cursor` | string | optional | Opaque cursor |

**Returns:** `{ "gates": [ { "module_name", "gate_name", "gate_kind", "source_path", "source_line", "probe_arity" } ], "total_estimate": N, "next_cursor": "<opaque>" }`.

---

## cppreflect

**New v0.17.0 (Reflection Intelligence, Phase 3a).** UE 5.7 reflection-edge queries driven by a regex sweep over UHT artefacts (`Intermediate/Build/Win64/.../Inc/<Module>/UHT/*.gen.cpp`) cross-joined with `IAssetRegistry::GetDependencies`. No tree-sitter dependency, no ThirdParty vendoring. Writes into `reflect_uclasses`, `reflect_uproperties`, `reflect_ufunctions`, `reflect_uinterfaces`, `reflect_uinterface_impls`, and `cpp_asset_edges` on `EngineSource.db`. All 6 actions are read-only + idempotent. **6 actions** (5 shipped in v0.17.0 Phase 3a; `list_class_specifiers` added v0.19.0).

> **Scan scope:** the indexers scan your project plugins by default, not just the game module. Scope follows a game-module → project-plugin → marketplace ladder driven by `IPluginManager::GetEnabledPlugins()`: `bIndexProjectPluginReflection` (default `true`) walks enabled `LoadedFrom == Project` plugins; `bIndexMarketplacePluginReflection` (default `false`) also walks enabled engine-installed marketplace plugins; Epic engine built-ins stay excluded (`bIndexEnginePluginReflection`, default off).

> **Phase 3a caller-contract notes:** `source_path` is UHT's `ModuleRelativePath` (not project-relative); `source_line` is `0` everywhere (UHT discards the original-header line — pair with `source_query("search_source")` for per-line precision); `reflect_uproperties.blueprint_visibility` / `.specifiers` are empty strings (Phase 3b populates them); `cpp_asset_edges.edge_kind` is the coarse `'package_dep'`.

### `cppreflect_query.get_uclass`

Fetch the UHT-derived UCLASS record — parent class, specifiers, source path/line.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `class_name` | string | **required** | Bare class name |
| `module_name` | string | optional | Disambiguates classes sharing a name across modules. Default: `""` |

**Returns:** `{ "uclass": <row-or-null> }` — row includes `class_name`, `module_name`, `parent_class`, `class_specifiers`, `source_path`, `source_line`.

### `cppreflect_query.list_uproperties`

Enumerate the UPROPERTY surface for a UCLASS. Cursor-paginated.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `class_name` | string | **required** | Bare class name |
| `module_name` | string | optional | Optional disambiguator. Default: `""` |
| `limit` | integer | optional | Hard cap `200`. Default: `50` |
| `cursor` | string | optional | Opaque cursor |

**Returns:** `{ "properties": [ { "property_name", "property_type", "blueprint_visibility", "specifiers", "source_path", "source_line" } ], "total_estimate": N, "next_cursor": "<opaque>" }`. `blueprint_visibility` and `specifiers` are empty in Phase 3a.

### `cppreflect_query.list_ufunctions`

Enumerate the UFUNCTION surface for a UCLASS. Cursor-paginated.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `class_name` | string | **required** | Bare class name |
| `module_name` | string | optional | Optional disambiguator. Default: `""` |
| `limit` | integer | optional | Hard cap `200`. Default: `50` |
| `cursor` | string | optional | Opaque cursor |

**Returns:** `{ "functions": [ { "function_name", "function_flags", "return_type", "params_json", "source_path", "source_line" } ], "total_estimate": N, "next_cursor": "<opaque>" }`. `function_flags` is the raw `EFunctionFlags` bitfield as emitted by UHT.

### `cppreflect_query.find_interface_impls`

List every C++ UCLASS that implements the given UINTERFACE. Blueprint implementations are NOT in this set (use `cpp_asset_edges` for the BP side). Cursor-paginated.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `interface_name` | string | **required** | UINTERFACE name |
| `limit` | integer | optional | Hard cap `500`. Default: `100` |
| `cursor` | string | optional | Opaque cursor |

**Returns:** `{ "interface_name": "<name>", "implementations": [ { "impl_class_name", "impl_module_name" } ], "total_estimate": N, "next_cursor": "<opaque>" }`.

### `cppreflect_query.find_class_specifier`

Find every UCLASS carrying a given specifier — substring match against the `flags` column of `reflect_uclasses`. Cursor-paginated. The `flags` column stores UHT metadata keys (`IsBlueprintBase`, `BlueprintType`, `Abstract`, etc.), NOT raw C++ UCLASS specifiers, so matching is forgiving (v0.19.0 enhancements): an alias map translates well-known C++ specifiers (`Blueprintable` → `IsBlueprintBase`); specifiers UHT drops entirely (`MinimalAPI`, `NotBlueprintable`) return an explicit not-captured note rather than a silent empty result; matching is case-insensitive. Call `list_class_specifiers` to discover the queryable token universe.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `specifier` | string | **required** | Substring match against the stored token, case-insensitive — `"BlueprintType"` matches both `BlueprintType` and `IsBlueprintBase:BlueprintType,...`. Well-known C++ specifiers (`Blueprintable`) are alias-mapped to the stored token |
| `module_filter` | string | optional | Optional module-name substring. Default: `""` |
| `limit` | integer | optional | Hard cap `500`. Default: `100` |
| `cursor` | string | optional | Opaque cursor |

**Returns:** `{ "classes": [ { "class_name", "module_name", "parent_class", "class_specifiers", "source_path", "source_line" } ], "effective_token": "<translated-token>", "total_estimate": N, "next_cursor": "<opaque>" }`. For specifiers UHT drops (`MinimalAPI` / `NotBlueprintable`), the response carries a not-captured note and refers you to `list_class_specifiers`.

### `cppreflect_query.list_class_specifiers`

**New v0.19.0.** Return the DISTINCT universe of tokens stored in the `flags` column of `reflect_uclasses`, each with a per-token class count. The `flags` column stores UHT metadata keys (e.g. `IsBlueprintBase`, `BlueprintType`, `Abstract`), NOT raw C++ UCLASS specifiers. Use this to discover what `find_class_specifier` can actually match. No params.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| _(none)_ | — | — | Takes no params |

**Returns:** `{ "specifiers": [ { "token": "IsBlueprintBase", "class_count": 142 }, ... ], "total_estimate": N }` — tokens ordered by `class_count` descending.

---

## network

**New v0.17.0 (Reflection Intelligence, Phase 4a).** UE 5.7 replication inspection driven by a second UHT-artefact regex sweep (independent of Phase 3a's reader) over per-property `MetaData` blocks plus the `CPF_Net` property-flag emission. Cross-joins against Phase 3a's `reflect_ufunctions`. Writes into `reflect_replicated_properties` on `EngineSource.db`. All 4 actions are read-only + idempotent. **4 actions.**

> **Status notes (v0.19.0 network-completeness workstream):**
> - The indexer now scans project plugins by default (the scan-scope ladder — see the `cppreflect` header note), so replicated classes and RPCs declared in project plugins are in scope, not just the game module.
> - `list_replicated_classes` now captures bare `UPROPERTY(Replicated)` + `DOREPLIFETIME` (via `CPF_Net`) in addition to `ReplicatedUsing` — verified end-to-end against a real project's replicated character/attribute classes.
> - `list_rpc_functions` switched to specifier-based detection (`reflect_ufunctions.specifiers` from `EFunctionFlags`) instead of name-prefix, and with project plugins in scope it now returns the project's actual RPCs — verified E2E against project-plugin Server RPCs. The prior "empty because game-module-only" status is resolved.
> - `COND_*` replication conditions still aren't surfaced.

### `network_query.list_replicated_classes`

Enumerate UCLASSes carrying at least one replicated property, sorted by replicated-property count. As of the v0.19.0 network-completeness workstream this captures bare `UPROPERTY(Replicated)` + `DOREPLIFETIME` (via `CPF_Net`) in addition to `ReplicatedUsing` — verified E2E. Cursor-paginated.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `module_filter` | string | optional | Substring match against `module_name`. Default: `""` |
| `limit` | integer | optional | Hard cap `200`. Default: `50` |
| `cursor` | string | optional | Opaque cursor |

**Returns:** `{ "classes": [ { "class_name", "module_name", "replicated_property_count" } ], "total_estimate": N, "next_cursor": "<opaque>" }`. Sorted by `replicated_property_count DESC, class_name ASC`.

### `network_query.list_rpc_functions`

Filter `reflect_ufunctions` by replication specifier (`reflect_ufunctions.specifiers` parsed from `EFunctionFlags` — `FUNC_NetServer` / `FUNC_NetClient` / `FUNC_NetMulticast`) to surface the project's RPC surface. As of the v0.19.0 network-completeness workstream this is specifier-based, not name-prefix-based, and the scan covers project plugins by default (see the scan-scope note in the `cppreflect` header). The project's actual RPCs — which often live in project plugins — are therefore in scope; an E2E run returned the project's project-plugin Server RPCs. Cursor-paginated.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `rpc_kind` | string | optional | Exact match — `server`, `client`, `multicast`, `netmulticast`. Empty returns all four. Default: `""` |
| `class_name` | string | optional | Optional UCLASS filter. Default: `""` |
| `module_filter` | string | optional | Substring match against module name. Default: `""` |
| `limit` | integer | optional | Hard cap `500`. Default: `100` |
| `cursor` | string | optional | Opaque cursor |

**Returns:** `{ "rpcs": [ { "class_name", "module_name", "function_name", "rpc_kind", "function_flags", "return_type", "source_path", "source_line" } ], "total_estimate": N, "next_cursor": "<opaque>" }`. `rpc_kind` is derived from the replication specifier (`EFunctionFlags`) at query time. With project plugins in scope by default the array populates from project-plugin RPCs.

### `network_query.list_onrep_handlers`

List every `OnRep_*` UFUNCTION paired with the property it covers (joined via `reflect_replicated_properties.rep_notify_func == function_name`). Cursor-paginated.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `class_name` | string | optional | Optional UCLASS filter. Default: `""` |
| `module_filter` | string | optional | Substring match. Default: `""` |
| `limit` | integer | optional | Hard cap `500`. Default: `100` |
| `cursor` | string | optional | Opaque cursor |

**Returns:** `{ "handlers": [ { "class_name", "module_name", "function_name", "covered_property", "source_path", "source_line" } ], "total_estimate": N, "next_cursor": "<opaque>" }`. `covered_property` is empty when the handler is orphaned (no matching `ReplicatedUsing`).

### `network_query.audit_unbalanced_onreps`

Find `ReplicatedUsing=OnRep_X` declarations whose `OnRep_X` function does NOT exist in the same class's reflected UFUNCTION surface — catches typos and rename drift. Cursor-paginated.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `module_filter` | string | optional | Substring match. Default: `""` |
| `limit` | integer | optional | Hard cap `500`. Default: `100` |
| `cursor` | string | optional | Opaque cursor |

**Returns:** `{ "violations": [ { "class_name", "module_name", "property_name", "missing_handler" } ], "total_estimate": N, "next_cursor": "<opaque>" }`. Sorted by `(module_name, class_name, property_name)`.

---

## pipeline

**New v0.17.0 (Reflection Intelligence, Phase 4a).** Two read-only composer actions that fan out other registered Monolith actions serially on the game thread and assemble the results into a single payload. They never mutate state — every action they invoke is itself read-only. No `ParallelFor`, no async dispatch. **2 actions.**

### `pipeline_query.pr_review`

Bundle the most common PR-review reads into a single call against a list of changed files (typically the output of `git diff --name-only`). For each path, fans out `risk_query("get_hotspot_score")`, `risk_query("get_cochange_pairs")`, `decision_query("list_decisions", path_filter=path)`, `source_query("audit_module_dep_reality")`, and (when `include_drift`) `blueprint_query("audit_cdo_drift")`, aggregated per-path.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `changed_files` | string[] | **required** | Array of project-relative paths. Hard cap 100 paths per call |
| `include_drift` | bool | optional | Include the CDO drift check. Default: `true` |

**Returns:** `{ "files": [ { "path", "hotspot_score", "cochange_partners", "decisions", "module_dep_violations", "cdo_drifts" } ], "summary": { "files_above_hotspot_threshold", "total_decisions_touched", "violation_count" } }`.

### `pipeline_query.release_readiness`

Release-gate composer. Bundles `monolith_status()`, `decision_query("list_stale")`, `risk_query("get_release_window_hotspots")`, plus the sentinel-list audit and CHANGELOG completeness audit specced in `.claude/rules/scoped/monolith-release.md`. Read-only end-to-end. No required params.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `stale_decision_days` | integer | optional | Forwarded to `decision_query("list_stale")`. Default: `90` |
| `hotspot_threshold` | number | optional | Forwarded to `risk_query("get_release_window_hotspots")`. Default: `0.7` |

**Returns:** `{ "status": { /* monolith_status payload */ }, "stale_decisions": [...], "release_window_hotspots": [...], "sentinel_audit": {...}, "changelog_completeness": {...} }`.

---

## Reflection Intelligence — Cross-Namespace Audit Actions

Four additional read-only audit actions ship with Reflection Intelligence Phase 4a. Each is owned by `MonolithReflectionIntel` but registered onto an **existing** host namespace's adapter for caller ergonomics (agents already discover the host namespace first). All four are cursor-paginated; `path_prefix` carries `AssetPath` semantics (`\` → `/` rewrite with a surfaced warning). Together with `source_query("audit_module_dep_reality")` these are the 5 cross-namespace RI audits.

### `material_query.audit_orphan_materials`

Identify materials with zero inbound references in the asset graph (orphans from deleted Blueprint owners or refactored material chains).

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `path_prefix` | string | optional | `/Game/...` path prefix to scope the scan. Default: `"/Game/"` |
| `limit` | integer | optional | Hard cap `500`. Default: `100` |
| `cursor` | string | optional | Opaque cursor |

**Returns:** `{ "orphans": [ { "asset_path", "asset_class" } ], "total_estimate": N, "next_cursor": "<opaque>" }`.

### `niagara_query.audit_cross_asset_refs`

Find broken or stale asset references inside Niagara systems / emitters — referenced assets that no longer exist or whose class has shifted. Joins against Phase 3a's `cpp_asset_edges`.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `path_prefix` | string | optional | `/Game/...` path prefix to scope the scan. Default: `"/Game/"` |
| `limit` | integer | optional | Hard cap `500`. Default: `100` |
| `cursor` | string | optional | Opaque cursor |

**Returns:** `{ "broken_refs": [ { "owning_asset", "missing_or_stale_ref", "expected_class", "actual_class_or_null" } ], "total_estimate": N, "next_cursor": "<opaque>" }`.

### `blueprint_query.audit_cdo_drift`

Detect Blueprint child classes whose CDO has overridden a native C++ parent's default value — useful when a native default changes upstream and BP children silently keep the stale override.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `path_prefix` | string | optional | `/Game/...` path prefix to scope the scan. Default: `"/Game/"` |
| `class_filter` | string | optional | Substring match against the parent C++ class name. Default: `""` |
| `limit` | integer | optional | Hard cap `500`. Default: `100` |
| `cursor` | string | optional | Opaque cursor |

**Returns:** `{ "drifts": [ { "bp_asset_path", "parent_class", "property_name", "parent_default", "bp_override" } ], "total_estimate": N, "next_cursor": "<opaque>" }`. Sorted by `(bp_asset_path, property_name)`.

### `project_query.audit_orphan_assets`

Project-wide zero-reference scan across all asset classes (the general form; `material_query("audit_orphan_materials")` is the type-scoped sibling). Cross-validates against Phase 3a's `cpp_asset_edges` to surface assets referenced only from C++ but not from the BP/asset graph.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `path_prefix` | string | optional | `/Game/...` path prefix to scope the scan. Default: `"/Game/"` |
| `asset_class_filter` | string | optional | Substring match against asset class name. Default: `""` |
| `limit` | integer | optional | Hard cap `500`. Default: `100` |
| `cursor` | string | optional | Opaque cursor |

**Returns:** `{ "orphans": [ { "asset_path", "asset_class" } ], "total_estimate": N, "next_cursor": "<opaque>" }`. An asset is orphaned only when both the asset-graph and C++-edge reference sets are empty.

---

## reflect

**New v0.19.0 (Reflection Intelligence, network-completeness workstream).** One WRITE/maintenance action for repopulating the RI reflection tables. **1 action.**

### `reflect_query.rebuild_reflection_index`

Force-rebuild the RI reflection tables from PROJECT UHT artefacts — `reflect_uclasses`, `reflect_uproperties`, `reflect_ufunctions`, `reflect_uinterfaces`, `reflect_uinterface_impls`, `cpp_asset_edges`, and `reflect_replicated_properties`. Re-runs the RI indexers (`FCppReflectIndexer` + `FNetworkIndexer`) over the project's on-disk UHT artefacts. Scope is PROJECT only (Epic engine built-ins excluded) — and, as of the v0.19.0 scan-scope ladder, "project" includes enabled `LoadedFrom == Project` plugins by default (and marketplace plugins when enabled), so a rebuild repopulates project-plugin reflection.

Exists because after an RI indexer code change there's no other clean repopulation trigger — the lazy bootstrap only fires on table-absence, `OnReloadComplete` only on Live Coding, and `source_query("trigger_reindex")` is the heavyweight full-engine reindex.

**WRITE / maintenance action — NOT read-only.** Idempotent (wipe-and-rewrite per indexer inside a single transaction) and non-destructive (regenerates deterministically from on-disk artefacts; never touches source or non-RI tables).

*No parameters.*

**Returns:** a per-table row-count summary — `{ "ok": true, "rebuilt": { "reflect_uclasses": N, "reflect_uproperties": N, "reflect_ufunctions": N, "reflect_uinterfaces": N, "reflect_uinterface_impls": N, "cpp_asset_edges": N, "reflect_replicated_properties": N } }`.

> Note: with the scan-scope ladder, a rebuild repopulates project-plugin reflection by default — so `network_query("list_rpc_functions")` returns the project's RPCs after a rebuild (see the `network` namespace status notes).

---

## workflow

Cross-domain practitioner workflow composers. The first slice is intentionally read-only: it returns the shared workflow proof envelope and delegates actual asset mutation to explicit next actions.

---

<a id="sibling-plugins"></a>

## Sibling Plugins

Sibling plugins live **beside** Monolith (not inside it) and register their own namespaces into Monolith's MCP action registry at startup. They ship as **separate plugins with separate releases** — they are not bundled in the Monolith zip.

If you're building a sibling plugin yourself, read `Plugins/Monolith/Docs/SIBLING_PLUGIN_GUIDE.md` for the architectural pattern, build setup, and reflection requirements.

| Sibling plugin | Namespace | Actions | Status | Repo |
|---|---|---|---|---|
| External sibling plugin | Custom | Varies | Registers its own namespace at startup and ships through its own repo/channel. | Outside `Plugins/Monolith/` |

**Why these aren't in the in-tree count:** the in-tree count counts only modules shipped inside the public `Monolith-vX.Y.Z.zip` release. Sibling plugins live in their own folders, ship via their own channels (or stay private), and may or may not be installed in any given consumer's project. Their absence is not a degraded state — Monolith is fully functional without them.

Private sibling bridges are intentionally omitted from the public API reference. Their action rosters, namespaces, and release notes belong in their own repos/channels; Monolith must not publish them as part of the public API surface.

---

<a id="pipelines"></a>

## Pipelines — Cross-Module Workflow Chains

Authoring complex assets typically requires calling a sequence of actions in order. The chains below are the canonical flows. Where a "build from spec" shortcut exists, prefer it — spec-based builders are transactional and handle validation, connection resolution, and rollback in a single call.

### Materials

```
create_material → build_material_graph → connect_expressions → recompile_material
```

**Shortcut:** `build_material_graph` accepts a `graph_spec` that can populate the entire graph (expressions + connections) in one call. Follow with `recompile_material`.

### State Machines (LogicDriver)

```
create_state_machine → add_state (×N) → add_transition (×N) → compile_state_machine
```

**Shortcut:** `build_sm_from_spec` builds the whole graph and compiles in a single call.

### Sound Cues

```
create_sound_cue → add_sound_cue_node (×N) → connect_sound_cue_nodes (×N) → set_sound_cue_first_node
```

**Shortcuts:** `build_sound_cue_from_spec` for arbitrary graphs; `create_random_sound_cue`, `create_layered_sound_cue`, `create_looping_ambient_cue`, `create_distance_crossfade_cue`, `create_switch_sound_cue` for canonical templates.

### MetaSounds

```
create_metasound_source → add_metasound_input/output → add_metasound_node (×N) → connect_metasound_nodes (×N)
```

**Shortcut:** `build_metasound_from_spec` does it all in one call (interfaces, inputs, outputs, nodes, connections).

### Behavior Trees

```
create_blackboard → add_bb_key (×N) → create_behavior_tree → set_bt_blackboard → add_bt_node (×N) → add_bt_decorator/service (×N)
```

**Shortcut:** `build_behavior_tree_from_spec` and the AI scaffold library (`scaffold_enemy_ai`, `scaffold_horror_stalker`, etc.) are usually the right entry points.

### State Trees

```
create_state_tree → set_st_schema → add_st_state (×N) → add_st_task (×N) → add_st_transition (×N) → compile_state_tree
```

**Shortcut:** `build_state_tree_from_spec`.

### Gameplay Abilities (GAS)

```
bootstrap_gas_foundation → create_attribute_set → add_attribute → create_gameplay_effect → add_modifier → create_ability → set_ability_tags → set_ability_triggers → compile_ability
```

**Shortcuts:** `scaffold_gas_project`, `scaffold_damage_pipeline`, `scaffold_status_effect`, `scaffold_weapon_ability`, `build_ability_from_spec`, `build_effect_from_spec`.

### UMG Widget Interaction

```
ui.add_widget_variable(ViewModel) -> workflow.ui_bind_widget_event -> workflow.ui_shipping_widget_blueprint -> asset.save_asset
```

`workflow.ui_bind_widget_event` is a cross-module workflow, not a duplicate `ui` action. It binds common widget events to ViewModel commands by composing existing Blueprint graph actions, rejects direct gameplay Actor/Pawn/Controller/component targets, and reports the child actions plus compile/read-back proof in the workflow envelope. Fixture coverage verifies the real WBP graph shape after apply: component-bound widget event, ViewModel getter, command call, exec link, and target data link.

### UMG Material Effects

```
material.create_material? -> workflow.ui_material_hlsl_effect -> workflow.ui_shipping_widget_blueprint -> asset.save_asset
material.get_material_parameters -> workflow.ui_retainer_effect_material -> workflow.ui_shipping_widget_blueprint -> asset.save_asset
```

`workflow.ui_material_hlsl_effect` is the UI material/HLSL composition entry point. It keeps graph writes in the `material` namespace, binds the resulting UI-domain material through existing `ui` styling actions, and reports HLSL lint, material compile/stat/domain/connection proof, opacity wiring proof, widget binding proof, `ui.audit_widget_material_lifecycle` MID lifetime proof, and next visual proof actions. When `connect_opacity=true` and no explicit matching Custom additional output is supplied, it uses `material.build_material_graph(clear_existing=false)` to add a `ComponentMask` node that routes the float4 alpha channel to `Opacity`.

`ui.set_retainer_effect_material` is the RetainerBox owner action for setting `EffectMaterial` and `TextureParameter` through `URetainerBox` APIs. `workflow.ui_retainer_effect_material` composes `material.get_material_parameters`, `material.get_material_properties`, `ui.set_retainer_effect_material`, Widget Blueprint compile/log proof, `ui.audit_widget_material_lifecycle`, and optional `workflow.ui_shipping_widget_blueprint`. The default Retainer texture parameter is `Texture`, but the workflow requires exact material parameter readback before treating the binding as proven; Retainer performance gains still require separate runtime profiling.

### Town Generation (experimental)

```
generate_floor_plan → create_building_from_grid → generate_facade → generate_roof → furnish_room → validate_building
```

> **Experimental.** `bEnableProceduralTownGen=true` required. Known geometry issues remain.

---

## Discovery Pattern (use this first)

Before writing any client code:

1. `monolith_find("task description")` — route the task to candidate namespaces/actions when the action is unclear.
2. `monolith_discover({ "namespace": "<namespace>", "action": "<action>", "mode": "schema" })` — get exact params for one action.
3. `describe_query("schema", { "target_namespace": "<namespace>", "target": "..." })` — inspect writable shape before reflective writes. Omit `target` only when you need the namespace-level descriptor.
4. `project_query("search", {query: "..."})` — find assets by name/type/content provenance.
5. `source_query("search_source", {query: "..."})` — verify UE 5.7 API signatures.

**Golden rule:** never fabricate action names. The cogitator will be displeased.

---

## Offline Fallback (editor not running)

When the editor is closed but you still need to query Monolith:

- **`Plugins/Monolith/Binaries/monolith_query.exe`** — standalone C++ tool and canonical offline path. Serves current `source`, `project`, `bridge`, `console` snapshot reads (`search_objects`, `get_object`, `health`), `monolith guide`, and the full **20-action Reflection Intelligence surface**; maintenance/runtime-only actions remain dry-run or return explicit `live_only` guidance unless their help explicitly requires `--execute`.
- **`python Plugins/Monolith/Scripts/monolith_offline.py`** — limited stdlib-only legacy fallback. It serves legacy `source` / `project` discovery plus the 20 RI actions, but not current `bridge`, `monolith guide`, source/project review, CRG graph, repair, snapshot, or expanded project content search workflows. Use the native exe for agent-facing offline work when available.

Both invoke the same SQLite indexes the live MCP uses.

**Reflection Intelligence offline parity.** All four RI namespaces are now fully servable offline — `cppreflect` (6 actions), `network` (4), `decision` (5), `risk` (5) — and emit JSON **byte-identical to the live MCP server** (same field names, types, ordering, row data, `%.17g` float formatting, and base64 cursor tokens). Earlier builds covered only 4 of the 20 with divergent shapes; the phantom `risk.list_hotspots` action has been removed. Two intentional, documented differences from the live payload remain (not bugs): the offline CLI adds a top-level `success` flag (its in-band status channel — the live MCP carries success/error out-of-band, so live has no `success` key; the nested DATA payload is byte-identical), and wall-clock fields (`cutoff_unix` / `since_unix` and the `risk.get_release_window_hotspots` cursor whose filter-hash includes them) differ by the run-time gap across process invocations on both live and offline.

`Scripts/verify_offline_parity.py` byte-diffs exe vs py across the 20 RI actions as a ship-blocking gate in `make_release.ps1`; `Scripts/check_offline_exe_fresh.py` flags a stale exe by comparing its `--version` `source_hash` against a fresh hash of `monolith_query.cpp`.

---

## Transport Notes

- Claude Code's MCP transport is `"http"`, not `"streamableHttp"`.
- Some clients serialize nested `params` objects to a JSON **string** instead of a nested object — detect and deserialize back.
- The HTTP server lives on `http://localhost:<port>`. Port is published in `monolith_status` output.
- For Claude Code specifically, the **MCP auto-reconnect proxy** at `Scripts/monolith_proxy.py` survives editor restarts. See [README.md](../README.md) for setup.

---

## Conditional Module Gating Reference

| Module | Gate | Actions when ungated |
|--------|------|----------------------|
| MonolithGAS | `WITH_GBA` (GameplayAbilities plugin) | 0 |
| MonolithComboGraph | `WITH_COMBOGRAPH` (ComboGraph marketplace plugin) | 0 |
| MonolithLogicDriver | `WITH_LOGICDRIVER` (Logic Driver Pro marketplace plugin) | 0 |
| MonolithAI | `WITH_STATETREE` + `WITH_SMARTOBJECTS` (engine plugins) | 0 |
| MonolithUI CommonUI | `WITH_COMMONUI` | UMG baseline only; use `monolith_discover({ "namespace": "ui" })` for the live count |
| MonolithAudio MetaSound | `WITH_METASOUND` | Sound Cue + CRUD + batch (no MetaSound graph) |
| MonolithMesh town gen | `bEnableProceduralTownGen` (Editor Preferences, default `false`) | 195 (core mesh only) |

---


## bridge

Read-only RX-6 bridge integration. **5 actions**.

### `bridge.get_index_status`

Report local project/source index readiness for Monolith bridge searches.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `include_stats` | bool | optional | Include project index stats when available. Default: `false` |

### `bridge.start_indexing`

Start local project asset and/or source indexing for bridge search.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `scope` | string | optional | Index scope: assets, source, or all. Default: `all` |
| `full` | bool | optional | Use full reindex instead of incremental/project-only indexing. Default: `false` |

### `bridge.search_items`

Search local indexed assets and source entries for mention-style prompt context.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `query` | string | **required** | Lexical query for assets, symbols, or source lines |
| `limit` | integer | optional | Maximum combined results. Default: `24` |

### `bridge.build_attachment`

Materialize a bridge.search_items result into a bounded prompt attachment.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `item_id` | string | **required** | Bridge item id returned by bridge.search_items |
| `context_lines` | integer | optional | Source lines before/after source hits. Default: `12` |

### `bridge.search_asset_symbols`

Read-only RX-6 bridge between ProjectIndex assets and EngineSource symbols.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `asset_path` | string | optional | Project asset package path seed |
| `symbol` | string | optional | Source symbol seed |

## `dataflow` namespace

Read-only Dataflow/Chaos graph discovery support without adding hard Dataflow link dependencies.

### `dataflow.get_status`

Report read-only Dataflow/Chaos graph discovery support without adding hard Dataflow link dependencies.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|

### `dataflow.list_assets`

List Dataflow asset metadata under /Game using AssetRegistry only. Does not load, evaluate, regenerate, or mutate assets.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `package_path` | string | optional | Content package path under /Game. Default: `/Game` |
| `limit` | integer | optional | Maximum assets to return, clamped to 1..500. Default: `100` |

### `dataflow.get_dataflow_graph`

Read a bounded Dataflow graph summary from a UDataflow asset. Does not mutate, evaluate, regenerate, or mark packages dirty.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `asset_path` | string | **required** | Dataflow asset path, e.g. /Game/Geometry/DF_Fracture |
| `node_limit` | integer | optional | Maximum node rows to return, clamped to 1..500. Default: `128` |
| `connection_limit` | integer | optional | Maximum connection rows to return after node_limit filtering, clamped to 1..5000. Default: `1000` |
| `include_properties` | boolean | optional | Include editable UPROPERTY snapshots for each returned node. Default: `false` |

### `dataflow.list_dataflow_node_types`

List registered Dataflow node factory types with optional filtering and pin summaries.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `filter` | string | optional | Substring filter across type, display name, category, and tags |
| `common_only` | boolean | optional | Exclude deprecated and experimental node types. Default: `true` |
| `limit` | integer | optional | Maximum type rows to return, clamped to 1..1000. Default: `200` |
| `include_pins` | boolean | optional | Include default input/output pins for each returned type. Default: `false` |

### `dataflow.get_dataflow_node_schema`

Return schema details for one registered Dataflow node type.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `type_name` | string | **required** | Registered Dataflow node type name, e.g. FAddFloatsDataflowNode |
| `include_properties` | boolean | optional | Include editable UPROPERTY defaults for this node type. Default: `true` |

### `dataflow.validate_dataflow_graph`

Validate a Dataflow graph for duplicate node identifiers and broken connection references without mutation.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `asset_path` | string | **required** | Dataflow asset path, e.g. /Game/Geometry/DF_Fracture |

### `dataflow.list_dataflow_variables`

List UDataflow property bag variables with descriptor metadata and serialized values without mutation.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `asset_path` | string | **required** | Dataflow asset path, e.g. /Game/Geometry/DF_Fracture |

### `dataflow.list_dataflow_comments`

List Dataflow editor comment boxes with bounded node membership hints without mutation.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `asset_path` | string | **required** | Dataflow asset path, e.g. /Game/Geometry/DF_Fracture |
| `node_limit` | integer | optional | Maximum contained-node rows per comment, clamped to 1..500. Default: `128` |


## Public C++ Surface (MonolithCore)

New public C++ symbols from the UnrealMCP-port slices. These are for sibling plugins / in-tree modules that produce async jobs, emit typed media, or contribute resource families. All are dark-by-default at the MCP wire level (flag-gated at the call site).

### `FMonolithAsyncJobRegistry` — `Public/MonolithAsyncJobRegistry.h`

Process-wide singleton bounded in-memory registry for long-running async jobs. Producers drive a job from `Pending` to a terminal state; the registry spawns no threads and persists nothing across restarts.

| Member | Signature | Behavior |
|--------|-----------|----------|
| `Get` | `static FMonolithAsyncJobRegistry& Get()` | Singleton accessor. |
| `SubmitJob` | `FString SubmitJob(const FString& Namespace, const FString& Action)` | Mints a lowercase-hyphen GUID job id, seeds a `Pending` row, returns the id. Evicts the oldest-`UpdatedUtc` row when the 128-row capacity is full. |
| `UpdateProgress` | `void UpdateProgress(const FString& JobId, double Percent, const FString& Stage, const FString& Message)` | Advances a `Pending`/`Running` row to `Running`; clamps `Percent` to 0–100; never overwrites a terminal state. |
| `CompleteJob` | `void CompleteJob(const FString& JobId, const TSharedPtr<FJsonObject>& Result)` | Marks `Completed`, percent 100, attaches optional result. |
| `FailJob` | `void FailJob(const FString& JobId, const FString& Error)` | Marks `Failed`, records error string. |
| `RequestCancel` | `void RequestCancel(const FString& JobId)` | Sets the **cooperative** cancel flag; flips a non-terminal row to `Cancelled`. Does NOT interrupt running work. |
| `IsCancelRequested` | `bool IsCancelRequested(const FString& JobId) const` | True if cancellation was requested — running actions poll this. |
| `GetJobJson` | `TSharedPtr<FJsonObject> GetJobJson(const FString& JobId) const` | Row JSON, or `{"status":"not_found"}`. |
| `ListJobsJson` | `TSharedPtr<FJsonObject> ListJobsJson(int32 Limit) const` | Up to a clamped `1..1000` rows ordered by `UpdatedUtc` desc. |

`EMonolithAsyncJobStatus`: `Pending`, `Running`, `Completed`, `Failed`, `Cancelled`. Thread-safe (single `FCriticalSection`).

### `FMonolithToolContentBlock` — `Public/MonolithToolRegistry.h`

Typed-media content block mirroring the MCP `{type, mimeType, data}` shape. `FMonolithActionResult` carries a `TArray<FMonolithToolContentBlock> MediaBlocks` slot (empty by default → existing responses byte-identical).

| Field | Type | Meaning |
|-------|------|---------|
| `Type` | `FString` | `"image"` or `"audio"` (other types are skipped during emission). |
| `MimeType` | `FString` | e.g. `"image/png"`, `"audio/wav"`. |
| `Base64Data` | `FString` | base64-encoded payload. |
| `Audience` | `FString` | optional MCP annotation (`"user"`/`"assistant"`); empty = omitted. |

Emitted by `FMonolithToolResultUtils::BuildMcpToolResult(const FMonolithActionResult&, bool bEnableStructuredContent, bool bEnableTypedMedia=false)` only when `bEnableTypedMedia` is true (wired to `bEnableTypedMediaResults`) and `MediaBlocks` is non-empty; blocks follow the always-present text block.

### `FMonolithResourceRegistry::RegisterBlobResource` — `Public/MonolithResourceRegistry.h`

```cpp
void RegisterBlobResource(
    const FMonolithResourceDescriptor& Descriptor,
    const TArray<uint8>& BlobBytes,
    int32 MaxBytes = 1024 * 1024);
```

Registers a bounded binary/blob resource. Bytes are stored eagerly and served verbatim as a base64 `"blob"` field (instead of `"text"`); `MaxBytes` is a separate cap from text `MaxChars` and is clamped to a safe upper bound. Reads populate `FMonolithResourceReadResult::bBinary=true` + `BlobBytes`; text resources keep `bBinary=false` so their JSON is byte-identical to the text-only slice.

### `IMonolithResourceProvider` — `Public/IMonolithResourceProvider.h`

Per-namespace resource provider seam (P3b). A provider advertises a bounded URI family (e.g. `monolith://source/file/{path}`) and resolves concrete reads on demand. Registered by code via `FMonolithResourceRegistry::RegisterProvider` / `UnregisterProvider`; consulted only AFTER the static-descriptor and eager-blob branches miss, and invoked OUTSIDE the registry lock.

| Method | Contract |
|--------|----------|
| `virtual void ListResources(TArray<FMonolithResourceDescriptor>& OutDescriptors) const` | Append ONE (or a few) stable TEMPLATE descriptors. MUST NOT perform an unbounded scan. |
| `virtual bool ReadResource(const FString& Uri, FMonolithResourceReadResult& Out) const` | Return `true` only when the provider owns the URI scheme AND resolves a payload (populate `bFound`/`Uri`/`MimeType` + either `Text` or `BlobBytes`+`bBinary`). Return `false` for a URI it does not own so the registry tries the next provider. |

Provider output is subject to the same safety rules as the registry (no absolute local paths, secrets, or env values; bounded content).

---

## Cross-References

- **SPEC_CORE.md** — Master Monolith spec, action count audit, pipelines, architecture
- **SPEC_Monolith*.md** — Per-module deep specs in `Plugins/Monolith/Docs/specs/`
- **SIBLING_PLUGIN_GUIDE.md** — How to build a sibling plugin against `FMonolithToolRegistry`
- **CHANGELOG.md** — Release-by-release change history (Keep a Changelog format)
- **Wiki** — User-facing tutorials at `https://github.com/tumourlove/monolith/wiki`

---

*The Omnissiah's blessing be upon every action call. May your discovery never return an empty array.*
