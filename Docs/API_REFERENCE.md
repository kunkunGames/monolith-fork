# Monolith API Reference

**Version:** v0.22.0 · **Last updated:** 2026-08-01

**In-tree action total is approximate: ~1,400+ actions across 26+ in-tree namespaces** (public, in-tree only; most are active by default, the GameFeatures surface is optional/config-gated, and 45 experimental town-gen actions register only when `bEnableProceduralTownGen=true`). The surface is too large to track to the unit — **query `monolith_discover()` (its `total_actions` field) for the exact live figure.** The `ui` namespace re-exports 4 GAS UI binding actions as aliases. v0.19.0 adds an LLM C++ authoring ergonomics pack (`source`, 8 actions + `editor.get_build_errors` fix hints), live-PIE introspection + driving and stat-group readout (`editor`), anim-node binding read/write and time-series PIE sampling (`animation`), a Blueprint variable census + contract reconciliation (`blueprint`), and T3D asset-text export (`project`); plus two first-launch fixes (issue #70) and a ~40% smaller `tools/list` manifest. The `monolith_*` meta-tools (`discover`, `status`, `update`, `reindex`, `guide`) plus the `bulk_fill_query` and `describe_query` framework dispatchers round out the MCP tool count. This total EXCLUDES sibling-plugin actions — they ship in their own repos and are never in the public release zip.

The per-namespace numbers in the Table of Contents and body sections below are kept for structure, not precision — they drift with every action added and are no longer maintained to the unit. Treat them as ballpark; the live figure always comes from `monolith_discover()`.

> Auto-generated and hand-curated. Each action is dispatched via HTTP POST to `http://localhost:<port>` with JSON body `{ "namespace": "<ns>", "action": "<action>", "params": { ... } }`, or via the MCP `tools/list` surface that AI clients see at session start.
>
> `monolith_discover("<namespace>")` is terse by default — it lists each action's name plus a one-line description, NOT the full param schemas. For a single action's exhaustive live param schema, call `describe_query("action_schema", target_namespace="<ns>", target_action="<name>")`, or pass `detail=true` (alias `verbose=true`) to `monolith_discover` to inline all schemas. Discover also accepts `filter` (case-insensitive substring on name or description), `offset`, and `limit` (default 0 = full list). This document is a curated reference, not a source-of-truth substitute.
>
> **0.15.0:** the namespace counts in the Table of Contents and the per-namespace body sections below were regenerated against live `monolith_discover()` on 2026-05-23 — the 0.14.8 → 0.15.0 additions are reflected (the `bulk_fill` / `describe` framework, the blueprint dataset read/edit pack, the UI/Blueprint gap-closure actions, `monolith_guide`, `editor` Python/PIE/console verbs, the `level_sequence` namespace, and the audio MetaSound document-introspection actions). Body sections list every action by category; deep-dive param tables cover the high-traffic ones. For the exhaustive live param schema of any action, call `describe_query("action_schema", ...)` (or pass `detail=true` to `monolith_discover("<namespace>")`).

---

## Table of Contents

| Namespace | Actions | Description |
|-----------|---------|-------------|
| [monolith](#monolith) | 5 | Core server tools (discover, status, update, reindex, guide) |
| [blueprint](#blueprint) | 111 | Blueprint read/write, variable/component/graph CRUD, node ops, compile, auto-layout, spawn actors, dataset read/edit pack (DataTable/CurveTable/StringTable + `seed_data_asset`), cross-class property access, parent-function overrides |
| [material](#material) | 63 | Material graph editing, inspection, CRUD, material functions, PBR pipeline |
| [animation](#animation) | 145 | Curves, bone tracks, sync markers, root motion, compression, blend spaces (incl. baking + interpolation control), ABPs (incl. an AnimGraph-authoring pack — additive/slot/cached-pose/blend (by int + by enum)/sync/layered-blend/Control Rig/linked-layer/conduit nodes + output wiring — ABP-native animation layer graphs, custom anim-graph nodes + state-machine teardown + compound expression transition rules), montages, skeletons, PoseSearch, IKRig, Control Rig |
| [chooser](#chooser) | 16 | ChooserTable discovery, bounded readback, non-mutating structural validation, deep inspection, authoring, and reference editing |
| [niagara](#niagara) | 119 | Niagara VFX (emitters, modules, params, renderers, HLSL, dynamic inputs, event handlers, sim stages, effect types, event-aware summaries + validate_system event-chain reasoning, temporal-control composite writers + read aggregators, stateless-emitter factory) |
| [editor](#editor) | 29 | Live Coding builds, compile output capture, editor logs, scene capture, texture import, map creation, module status, automation test list/run, Python escape-hatch, persistent-level swap |
| [asset](#asset) | 20 | Exact-path texture/font ingest, lifecycle, inspection/search, guarded move/rename, and package-graph copy/fixup/closure workflows |
| [config](#config) | 6 | INI config inspection and search |
| [localization](#localization) | 10 | Culture discovery plus guarded StringTable inspect, validate, mutation, and CSV round-trip actions |
| [project](#project) | 7 | Project-wide asset index (SQLite + FTS5) |
| [collection](#collection) | 13 | Content Browser collection discovery, membership, dynamic queries, colors, and validation |
| [source](#source) | 11 | Unreal Engine C++ source code navigation |
| [source_control](#source_control) | 11 | Provider-backed status/checkout/add/delete/revert plus bounded Perforce opened/path mapping |
| [pcg](#pcg) | 28 | Exact-path PCG graph discovery/authoring/replacement and asynchronous editor-world/Blueprint-template component lifecycle control |
| [mesh](#mesh) | 194 | Mesh inspection, scene manipulation, spatial queries, blockout, GeometryScript, procedural geo, lighting, audio, performance, mesh import (incl. skeletal + animation). +45 town gen registers only with `bEnableProceduralTownGen=true` (experimental, not in the public count) |
| [ui](#ui) | 138 | UMG widget CRUD, templates, styling, animation v1+v2, EffectSurface, Spec Builder, Type Registry, settings scaffolding, headline scaffolders, navigation/conversion gap-closure, accessibility, CommonUI, GAS UI bindings |
| [gas](#gas) | 135 | Gameplay Ability System: abilities, attributes, effects, ASC, tags, cues, targeting, input, inspect, scaffold |
| [input](#input) | 10 | Enhanced Input asset inspection, guarded `UInputAction` / `UInputMappingContext` authoring, mapping clone/edit, validation |
| [gamefeatures](#gamefeatures) | 15 | Optional Game Feature plugin/data inspection and guarded ActionSet/`UGameFeatureData` instanced-action authoring |
| [combograph](#combograph) | 13 | ComboGraph melee combo authoring (conditional on `WITH_COMBOGRAPH`) |
| [ai](#ai) | 221 | Behavior Trees, State Trees, EQS, Blackboards, AI Controllers, Perception, Smart Objects, Navigation, Mass, Zone Graph, runtime PIE inspection, scaffolds |
| [logicdriver](#logicdriver) | 66 | Logic Driver Pro state machines: graph CRUD, runtime PIE control, scaffolds, dialogue (conditional on `WITH_LOGICDRIVER`) |
| [audio](#audio) | 98 | Sound Cue + MetaSound graph CRUD + document introspection, attenuation/class/mix/submix/concurrency, batch ops, Sound Cue templates, perception bindings |
| [level_sequence](#level_sequence) | 8 | Level Sequence inspection: binding inventory (legacy + UE 5.7 custom bindings), Director Blueprint functions/variables, event-track bindings, cross-sequence reverse lookup |
| [interchange](#interchange) | 15 | Guarded import, batch import, reimport-source management, reimport, export, and source/format inspection |
| [bulk_fill](#bulk_fill) | 2 | Reflection-walker bulk property fill across 12 per-namespace adapters (`apply`, `list_namespaces`) |
| [describe](#describe) | 3 | Read-only schema introspection for the same 12 adapters (`schema`, `list_targets`, `action_schema`) |
| [decision](#decision) | 5 | **New v0.17.0.** Reflection Intelligence — architectural decision records mined from markdown corpora |
| [risk](#risk) | 5 | **New v0.17.0.** Reflection Intelligence — git-churn / co-change / hotspot signals + conditional-gate inventory |
| [cppreflect](#cppreflect) | 6 | **New v0.17.0.** Reflection Intelligence — UE 5.7 UHT reflection-edge queries (UCLASS / UPROPERTY / UFUNCTION / UINTERFACE + cpp↔asset edges + specifier discovery) |
| [network](#network) | 4 | **New v0.17.0.** Reflection Intelligence — UE 5.7 replication inspection (replicated classes, RPCs, OnRep handlers, unbalanced-OnRep audit) |
| [pipeline](#pipeline) | 2 | **New v0.17.0.** Reflection Intelligence — read-only composer actions (`pr_review`, `release_readiness`) |
| [reflect](#reflect) | 1 | **New v0.19.0.** Reflection Intelligence — index maintenance (`rebuild_reflection_index`, project-only force-rebuild of the RI reflection tables; WRITE/maintenance) |
| **In-tree subtotal** | **~1,400+** | Counts are intentionally approximate; query `monolith_discover()` for the current registry total. |
| **In-tree subtotal** | **1417** | (all default-active; +45 experimental town gen → 1462 when registered) |
| **In-tree subtotal** | **1405** | (all default-active; +45 experimental town gen → 1450 when registered) |
| **In-tree subtotal** | **1419** | (all default-active; +45 experimental town gen → 1464 when registered) |
| **In-tree subtotal** | **1416** | (all default-active; +45 experimental town gen → 1461 when registered) |
| **In-tree subtotal** | **1421** | (full compiled/config-enabled GameFeatures surface; +45 experimental town gen → 1466 when registered) |
| **In-tree subtotal** | **1426** | (all default-active; +45 experimental town gen → 1471 when registered) |
| [Sibling plugins](#sibling-plugins) | varies | Separate plugins, separate distribution |

---

## Recent API Changes (v0.14.0 → v0.14.7)

The Phase J retrofit cycle added five new actions and tightened param validation on several others. If you wrote integration code against v0.13.x or earlier, scan this list before upgrading.

| Action | Change | Reason |
|--------|--------|--------|
| `editor.create_empty_map` | **NEW** (Phase J F8) | Test scaffolding needed a blank UWorld factory that doesn't depend on engine templates. |
| `editor.get_module_status` | **NEW** (Phase J F8) | Lets clients query plugin/module load state without grepping logs. Wraps `IPluginManager` + `FModuleManager`. |
| `gas.grant_ability_to_pawn` | **NEW** (Phase J F8) | Convenience action for runtime ability grants. Earlier you had to grant via `apply_effect` or scaffold-side wiring. |
| `ai.add_perception_to_actor` | **NEW** (Phase J F8) | Direct perception attach without going through `add_perception_component` + manual wiring. |
| `ai.get_bt_graph` | **NEW** (Phase J F8) | Read-only graph dump distinct from `get_behavior_tree`'s structural inspection. |
| `audio.create_test_wave` | **NEW** (Phase J F18) | Procedurally synthesizes a 16-bit mono sine `USoundWave` for tests with zero asset deps. |
| `audio.bind_sound_to_perception` | Param validation tightened (Phase J F11) | `loudness <= 0`, `max_range < 0`, and unknown `sense_class` values now reject up-front instead of writing junk userdata. |
| `gas.bind_widget_to_attribute` (and 3 aliases) | Param validation tightened (Phase J F2/F3) | Empty `widget_path`, missing `attribute`, or unresolvable ASC now return structured errors before any reflection writes. |
| `ai` BT actions | Error message standardization (Phase J F15) | All BT-related actions now return `{ "error": "<code>", "detail": "<human>" }` instead of mixed prose. |
| `gas` UI binding response | Shape change (Phase J F5) | Returns `{ bindings: [...], count: N }` instead of a bare array. Wrap your client parsers. |

The aliased GAS UI binding actions live in **both** `ui::*` and `gas::*` namespaces — same handler, two callable paths. Pick whichever reads better from your client.

## Recent API Changes (v0.14.8 → v0.15.0)

These releases added the `level_sequence` namespace, the `bulk_fill` / `describe` ergonomics framework, a blueprint dataset read/edit pack, a UI/Blueprint gap-closure sweep, `monolith_guide`, and editor automation verbs. The per-namespace body sections below now document these; full param schemas for everything are live via `describe_query("action_schema", ...)` (or `monolith_discover("<namespace>", detail=true)`).

| Action | Change | Reason |
|--------|--------|--------|
| `bulk_fill_query("apply" / "list_namespaces")` | **NEW namespace** (0.15.0) | Reflection-walker bulk property fill across 12 per-namespace adapters, with `dry_run` previews. |
| `describe_query("schema" / "list_targets" / "action_schema")` | **NEW namespace** (0.15.0) | Read-only schema introspection for the same adapters; `action_schema` returns any registered action's full param schema. |
| `monolith.guide` | **NEW** (0.15.0) | Section-keyed onboarding guide for AI agents (onboarding / recipes / decisions / errors / skills_map / gotchas) with a live registry overlay. |
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

List available tool namespaces and their actions. Pass `namespace` to filter; pass `category` to narrow further (e.g. `"CommonUI"` inside `ui`).

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `namespace` | string | optional | Filter to a specific namespace |
| `category` | string | optional | Filter actions within the namespace by category |

**Returns:** Per-action param schemas for every registered action. AI clients also receive these in `tools/list` at session start, so most callers never need to call `discover` explicitly.

---

### `monolith.status`

Get Monolith server health: version, uptime, port, registered action count, namespace count, engine version, project name, module load status.

*No parameters.*

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

**Returns:** `{ "status", "message" }`. `status` is `reindex_started` when a run actually began, or `reindex_not_started` when the subsystem declined it — indexing was already in progress, or the index database is not open. Before 0.22.0 this always reported `reindex_started`, including for requests that never ran. `module_not_loaded` / `subsystem_unavailable` / `function_not_found` are returned when the index subsystem is not reachable.

---

### `monolith.guide`

Section-keyed editorial onboarding guide for your AI agent — an onboarding script, worked cross-namespace recipes, X-vs-Y decision matrices, error-to-recovery maps, a skills map, and Monolith-specific gotchas. Hand-authored markdown plus a **live registry overlay**, so the action counts and version it reports always match your running build. New in 0.15.0. Built for users with no project `CLAUDE.md` or private skills — point your AI at it and it self-onboards. Offline parity via `monolith_query.exe monolith guide`.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `section` | string | optional | One of `onboarding`, `recipes`, `decisions`, `errors`, `skills_map`, `gotchas`. Omit for the full index + all sections. An unknown value returns a validation error listing the valid keys. |

**Returns:** The requested section (or full guide) as markdown, plus the live per-namespace action counts and plugin version.

---

## blueprint

Full read/write access to Blueprint graphs, variables, components, functions, nodes, pins, interfaces, timelines, comments, CDOs, spawn-time actor placement, and dataset read/edit (DataTable / CurveTable / StringTable round-trip + `seed_data_asset`). Count is approximate — query `monolith_discover("blueprint")` for the live figure.

**New in v0.18.1 (Motion Matching + thread-safe AnimBP authoring + inspection):**
- `set_anim_class`, `apply_movement_preset`, `add_engine_component_typed`, `scaffold_locomotion_input`, `validate_animbp_variable_contract`, `scaffold_motion_matching_character` — character/actor scaffolding for a motion-matching setup (adds an `EnhancedInput` dep).
- `add_property_access_node` (reflective `K2Node_PropertyAccess` for thread-safe property reads), `set_function_thread_safe` (mark a Blueprint function `BlueprintThreadSafe`). `scaffold_locomotion_anim_values` now emits a fully-wired thread-safe body via Property Access and can target a named function graph.
- `get_component_details` falls back to inherited native components (reports `is_inherited_native`, `skeletal_mesh`, `anim_class`, `animation_mode`); `get_blueprint_info` adds `native_component_count`; `get_inherited_component_override` reads the effective component template value + source; `seed_data_asset` gained `read_back_values`; `get_cdo_properties` routes through the shared reflection reader as the canonical verify-after-write path.

**New in v0.22.0 (unified component resolver — issue #116, PR #102 part 1):**
- **Component reads now report this Blueprint's values, not the C++ parent's.** `get_component_details` and
  `get_components` read the Blueprint's OWN class-default object; they previously read `ParentClass`'s CDO, so
  every inherited component reported Epic's native constructor defaults (a capsule overridden to 96 read back 88).
- **New response fields.** `get_component_details` returns `source`, `resolved_component`, and `note`
  (`is_inherited_native` is retained and equals `source == "cdo_native"`). `get_components` returns
  `inherited_components[]` (components declared on a parent Blueprint's construction script, with
  `defining_class` / `has_override` / `source`) and `native_components_source`. `set_component_property` returns
  `source`, `resolved_component`, and `persisted`. `get_inherited_component_override`'s `source` values changed
  from `cdo_native` / `ich` / `scs` to the five-value set below.
- **`source` values:** `scs`, `cdo_native`, `ich_override`, `inherited_scs`, `parent_cdo_fallback`.
  `parent_cdo_fallback` means the Blueprint has no compiled generated class and the values shown are native
  defaults — the accompanying `note` says so.
- **`set_component_property` can now write components inherited from a parent Blueprint**, via an Inheritable
  Component Handler override on the child. `persisted: false` means the value equalled the inherited default and
  the override record was pruned on compile.
- **Aliases** resolve on all four actions and carry their own target class: `Mesh`/`SkeletalMesh`, `StaticMesh`,
  `CharacterMovement`/`Movement`, `Capsule`/`CapsuleComponent`, `Root`/`RootComponent`. An alias matching more
  than one component of its class is reported as an ambiguity with candidate names rather than silently
  resolving to the first.
- **`get_inherited_component_override` accepts `asset_path` as an alias for `bp_path`.**

> For full param schemas, call `describe_query("action_schema", target_namespace="blueprint", target_action="<name>")` (or `monolith_discover("blueprint", detail=true)`). Plain `monolith_discover("blueprint")` is terse — action names + one-line descriptions only. The action surface is too broad to enumerate here without bloat — high-traffic actions are documented below; the rest are listed and discoverable.

**Action categories:**

| Category | Actions | Examples |
|----------|---------|----------|
| Graph inspection | 9 | `list_graphs`, `get_graph_data`, `get_graph_summary`, `get_execution_flow`, `search_nodes`, `get_node_details` |
| Variables | 9 | `get_variables`, `add_variable`, `remove_variable`, `rename_variable`, `set_variable_type`, `set_variable_defaults`, `add_local_variable`, `remove_local_variable`, `add_replicated_variable` |
| Components | 7 | `get_components`, `get_component_details`, `add_component`, `remove_component`, `rename_component`, `reparent_component`, `set_component_property`, `duplicate_component` |
| Functions / Macros / Events | 12 | `get_functions`, `add_function`, `remove_function`, `rename_function`, `add_macro`, `remove_macro`, `rename_macro`, `add_event_dispatcher`, `remove_event_dispatcher`, `set_function_params`, `set_event_dispatcher_params`, `get_function_signature` |
| Interfaces | 4 | `implement_interface`, `remove_interface`, `get_interfaces`, `scaffold_interface_implementation` |
| Node ops | 11 | `add_node` (v0.21.0: `K2Node_SwitchEnum` user-enum resolution via `enum`/`enum_path`; `K2Node_CallFunction` resolves BP-defined funcs), `remove_node`, `connect_pins` (v0.21.0: cross-graph node-ID disambiguation, suggests `graph_name`), `disconnect_pins`, `set_pin_default`, `set_node_position`, `resolve_node` (shares both v0.21.0 resolvers), `add_event_node`, `add_comment_node`, `promote_pin_to_variable`, `add_nodes_bulk` |
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
| Layout | 2 | `auto_layout`, `export_graph` |
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

### `blueprint.build_blueprint_from_spec`

The crown jewel — author an entire Blueprint (parent class, variables, components, functions, event graph nodes, connections) from a single JSON spec. Validates and compiles in one call. See `describe_query("action_schema", target_namespace="blueprint", target_action="build_blueprint_from_spec")` for the full spec schema.

### `blueprint.spawn_blueprint_actor`

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `blueprint_path` | string | **required** | Package path of the Blueprint to spawn |
| `location` | array | optional | `[x, y, z]` world location |
| `rotation` | array | optional | `[pitch, yaw, roll]` |
| `scale` | array | optional | `[x, y, z]` |
| `folder_path` | string | optional | Outliner folder. **Recommended** — all spawned actors should set a folder path |

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
| `remove_data_table_row` | `asset_path`, `row_name`, `save?` | Row delete |
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

Material graph editing, inspection, CRUD, material functions, instances, custom HLSL nodes, PBR pipeline. **63 actions.**

> For full param schemas, call `describe_query("action_schema", target_namespace="material", target_action="<name>")` (or `monolith_discover("material", detail=true)`). Plain `monolith_discover("material")` is terse — names + one-line descriptions only.

**Action categories:**

| Category | Actions | Examples |
|----------|---------|----------|
| Graph inspection | 7 | `get_all_expressions`, `get_expression_details`, `get_full_connection_graph`, `get_expression_pin_info`, `get_expression_connections`, `list_expression_classes`, `get_compilation_stats` |
| Graph CRUD | 12 | `build_material_graph`, `connect_expressions`, `disconnect_expression`, `delete_expression`, `delete_expressions`, `clear_graph`, `move_expression`, `duplicate_expression`, `replace_expression`, `rename_expression`, `set_expression_property`, `auto_layout` |
| Material assets | 7 | `create_material`, `create_material_instance`, `duplicate_material`, `save_material`, `set_material_property`, `get_material_properties`, `recompile_material` |
| Material instances | 6 | `get_material_parameters`, `get_instance_parameters`, `set_instance_parameter`, `set_instance_parameters`, `set_instance_parent`, `clear_instance_parameter`, `list_material_instances` |
| Material functions | 12 | `create_material_function`, `build_function_graph`, `get_function_info`, `export_function_graph`, `set_function_metadata`, `update_material_function`, `delete_function_expression`, `create_function_instance`, `set_function_instance_parameter`, `get_function_instance_info`, `layout_function_expressions`, `rename_function_parameter_group` |
| Custom HLSL | 2 | `create_custom_hlsl_node`, `update_custom_hlsl_node` |
| Spec / templates | 3 | `export_material_graph`, `import_material_graph`, `validate_material` |
| Preview / capture | 2 | `render_preview`, `get_thumbnail` |
| Textures | 5 | `import_texture`, `create_pbr_material_from_disk`, `get_texture_properties`, `preview_texture`, `preview_textures`, `check_tiling_quality` |
| Layers | 1 | `get_layer_info` |
| Batch | 2 | `batch_set_material_property`, `batch_recompile` |
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
- **AnimGraph authoring (14):** `add_apply_additive` / `add_apply_mesh_space_additive` (Apply Additive / mesh-space additive nodes), `add_slot_node` (slot name validated against the skeleton's slot groups), `add_save_cached_pose` / `add_use_cached_pose` (paired by `cache_name`), `set_output_pose_source` (wire a node into the AnimGraph Output / Root result pin), `set_state_result_source` (wire a node into a state machine state's result pin), `add_blend_by_int` (grown to `num_poses` pose pins), `add_blend_by_enum` (Blend Poses by Enum bound to a `UEnum` via `enum_path`; one pose pin per exposed enumerator plus a Default/else pin, skipping the auto `_MAX` sentinel and `Hidden` enumerators; optional `enumerators` exposes a subset), `set_sync_group` (player node sync group name / role / method), `set_layered_blend_bones` (per-bone branch filters on a Layered Blend Per Bone node), `add_anim_control_rig_node` (`control_rig_class`; IO pins regenerate), `add_linked_anim_layer` (`layer_name` + optional `interface_class` / `instance_class`; resolves interface-declared layers first and ABP-native layers as SELF layers otherwise — see below), `add_conduit` (a state-machine conduit whose bound graph is a transition-logic graph, not an anim graph). Plus 3 extensions: `set_anim_node_pin_binding` now bootstraps the binding object on previously-unbound nodes; `auto_layout` gained a Blueprint-Assist-free `builtin` formatter (also the `auto` fallback) so layout works in release builds where Blueprint Assist is compiled out; `set_transition_rule` gained an `expression` kind (compound multi-term `terms[]` — each `{lhs, op, rhs, abs?, negate?}` — folded through Boolean AND/OR via `combine`), extending the existing `bool` / `auto` / `compare` kinds.

**New (Unreleased) — ABP-native animation layers:**
- **`add_anim_layer_graph`** creates an animation layer that belongs to the Animation Blueprint itself: a `UAnimationGraph` carrying `UAnimationGraphSchema` in the ABP's own `FunctionGraphs`, which is what the editor's My Blueprint → **+** → Animation Layer button produces. No `UAnimLayerInterface` asset is required, so ABP variants need not share a layer signature. The anim schema is what makes the anim compiler emit a real `FAnimBlueprintFunction` (`blueprint add_function` yields an inert K2 graph instead), and the Output Pose root node is created for free. Params: `asset_path`, `layer_name`, optional `input_poses` (pose NAMES only — `["InPose"]` or `[{"name": "InPose"}]`, capped at 16, unique ABP-wide), optional `compile` (default `true`). Returns `asset_path`, `graph_name`, `graph_class`, `schema_class`, `node_count`, `input_poses`, `compiled`, `saved`. Refuses a duplicate graph name (it never renames or replaces an incumbent), the reserved name `AnimGraph`, child ABPs, macro libraries, and interface Blueprints.
- **`add_linked_anim_layer` now resolves native layers.** When no implemented `UAnimLayerInterface` declares `layer_name`, it scans the ABP's own animation-schema graphs and binds a SELF layer (null Interface, invalid `InterfaceGuid`, null `InstanceClass`) — mirroring the engine, which derives "self" from interface-lookup failure rather than storing a flag. Interface layers win when a name exists in both; passing `interface_class` disables the native fallback; `instance_class` is rejected for a self layer. A self bind is discriminated in the payload by `interface_class: "<self>"` + `guid_resolved: false`, and `get_linked_layers` shows the node title `"<Layer>\nAnim Layer (self)"`.
- **Ordering constraint:** self-layer pose pins resolve against the ABP's `SkeletonGeneratedClass`, so `add_anim_layer_graph(compile=false)` followed straight away by `add_linked_anim_layer` places a node with no pose pins. Keep the default `compile=true`, or recompile before placing.

**New (Unreleased) — retargeting + locomotion authoring + inspection:**
- **Retarget pose + op-stack tuning (8):** `align_retarget_pose` (auto-align a source/target retarget pose via AutoAlign + SnapBoneToGround), `get_retarget_pose` / `set_retarget_pose` (read/edit a retarget pose — `set` `mode`: `from_reference` or `bone_deltas`; `from_animation` deferred), `get_retarget_chain_settings` / `set_retarget_chain_settings` (read/write a chain's FK & IK op-stack settings — rotation/translation modes, IK blend, etc.), `set_retarget_root_settings` (Pelvis Motion op: vertical scale, floor constraint, affect-IK), `enable_foot_ground_lock` (Speed Planting op foot ground-lock on named IK chains), `set_bone_translation_retargeting` / `get_bone_translation_retargeting` (per-bone `USkeleton` translation-retargeting modes — `Animation` / `Skeleton` / `AnimationScaled` / `AnimationRelative` / `OrientAndScale` — plus a `biped_locomotion` preset on the setter).
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
| Curves | 6 | `add_curve`, `remove_curve`, `set_curve_keys`, `get_curve_keys` (reports `monotonic` + `sign`), `list_curves`, `get_skeleton_curves`, `bake_distance_curve` |
| Notifies | 9 | `add_notify`, `add_notify_state`, `remove_notify`, `set_notify_time`, `set_notify_duration`, `set_notify_track`, `set_notify_properties`, `bulk_add_notify`, `clone_notify_setup` |
| Sync markers | 5 | `get_sync_markers`, `add_sync_marker`, `remove_sync_marker`, `rename_sync_marker`, `derive_foot_sync_markers` (gains a `from_bones` mode) |
| Skeleton | 5 | `get_skeleton_info`, `get_skeletal_mesh_info`, `add_socket`, `remove_socket`, `set_socket_transform`, `get_skeleton_sockets`, `add_virtual_bone`, `remove_virtual_bones`, `compare_skeletons` |
| Skeleton (read/compat, v0.14.10) | 5 | `get_skeleton_preview_attached_assets`, `get_bone_ref_pose`, `get_compatible_skeletons`, `add_compatible_skeleton`, `remove_compatible_skeleton` |
| Montages | 9 | `get_montage_info`, `create_montage`, `set_montage_blend`, `add_montage_section`, `delete_montage_section`, `set_section_next`, `set_section_time`, `add_montage_slot`, `set_montage_slot`, `add_montage_anim_segment`, `create_montage_from_sections` |
| Blend spaces | 10 | `get_blend_space_info`, `create_blend_space`, `create_blend_space_1d`, `create_aim_offset`, `create_aim_offset_1d`, `add_blendspace_sample`, `edit_blendspace_sample`, `delete_blendspace_sample`, `set_blend_space_axis`, `bake_blend_space`, `set_blend_space_interpolation` |
| ABPs | 9 | `get_abp_info`, `create_anim_blueprint`, `get_state_machines`, `get_state_info`, `get_transitions`, `get_blend_nodes`, `get_linked_layers`, `get_graphs`, `get_nodes`, `get_abp_variables`, `get_abp_linked_assets` |
| State machines (write) | 6 | `add_state_to_machine`, `add_transition`, `set_transition_rule` (`bool` / `auto` / `compare` / `expression` kinds — `expression` folds multi-term `terms[]` through Boolean AND/OR), `remove_anim_state`, `set_anim_entry_state`, `remove_anim_transition` |
| ABP graph (write) | 5 | `add_anim_graph_node` (aliases or generic `UAnimGraphNode_Base` class path/name via `node_type` / `node_class`), `connect_anim_graph_pins`, `set_state_animation`, `add_variable_get`, `set_anim_graph_node_property` |
| ABP graph authoring (Unreleased) | 15 | `add_anim_layer_graph` (create an ABP-native animation layer graph — anim schema, auto Output Pose, optional `input_poses`), `add_apply_additive`, `add_apply_mesh_space_additive`, `add_slot_node`, `add_save_cached_pose`, `add_use_cached_pose`, `set_output_pose_source`, `set_state_result_source`, `add_blend_by_int`, `add_blend_by_enum` (Blend Poses by Enum bound to a `UEnum`; one pin per exposed enumerator + Default, skips `_MAX` + `Hidden`), `set_sync_group`, `set_layered_blend_bones`, `add_anim_control_rig_node`, `add_linked_anim_layer`, `add_conduit` |
| Composites | 3 | `get_composite_info`, `add_composite_segment`, `remove_composite_segment`, `create_composite` |
| IKRig / Retarget | 7 | `get_ikrig_info`, `add_ik_solver`, `remove_ik_solver`, `set_ik_rig_bone_settings`, `get_ik_rig_bone_settings`, `get_retargeter_info` (emits `ops[]`), `set_retarget_chain_mapping`, `add_retarget_chain`, `remove_retarget_chain`, `set_retarget_chain_bones` |
| Retarget pose + ops (Unreleased) | 9 | `align_retarget_pose`, `get_retarget_pose`, `set_retarget_pose`, `get_retarget_chain_settings`, `set_retarget_chain_settings`, `set_retarget_root_settings`, `enable_foot_ground_lock`, `set_bone_translation_retargeting`, `get_bone_translation_retargeting` |
| Locomotion / inspection (Unreleased) | 3 | `get_root_motion_speed`, `bind_threadsafe_update_function`, `get_animated_bone_transform` |
| Control Rig | 7 | `get_control_rig_info`, `get_control_rig_variables`, `add_control_rig_element`, `get_control_rig_graph`, `add_control_rig_node`, `connect_control_rig_pins` |
| Anim modifiers | 2 | `apply_anim_modifier` (accepts `properties` + `persist`), `list_anim_modifiers` |
| Physics asset | 3 | `get_physics_asset_info`, `set_body_properties`, `set_constraint_properties` |
| PoseSearch | 13 | `get_pose_search_schema`, `get_pose_search_database`, `add_database_sequence`, `remove_database_sequence`, `get_database_stats`, `create_pose_search_schema`, `create_pose_search_database`, `set_database_sequence_properties`, `add_schema_channel`, `remove_schema_channel`, `set_channel_weight`, `rebuild_pose_search_index`, `set_database_search_mode` |
| Layout / batch | 2 | `auto_layout`, `batch_execute` |

See `Plugins/Monolith/Docs/specs/SPEC_MonolithAnimation.md` for the deep dive.

---

## chooser

ChooserTable discovery, readback, structural validation, deep inspection,
authoring, and reference editing. The namespace is registered from
`MonolithAnimation`, which is the sole owner of its startup and teardown.

The six readback actions accept only a canonical mounted package path
(`/Game/Choosers/CHT_Locomotion`) or matching top-level object path
(`/Game/Choosers/CHT_Locomotion.CHT_Locomotion`). They reject relative paths,
filesystem paths, subobjects, redirectors, case-only aliases, and mismatched
object names. They do not compile, transact, mutate, save, or dirty the package.
Reflected values are serialized to a maximum depth of 3, with at most 128
fields per full struct (16 in compact column summaries) and 256 entries per
full container (8 in compact summaries). Container wrappers preserve the full
`count` and emit `truncated_after` when only the bounded prefix is returned.
Depth-limited structs and containers return
`serialization=depth_limit` without expanding. String, localized-text, and
otherwise unsupported export values cap at 4,096 characters; longer values
return `serialization`, bounded `value`, `original_char_count`, and
`truncated_after`.

| Category | Action | Parameters | Contract |
|---|---|---|---|
| Discovery | `list_chooser_tables` | `path_filter?`, `offset=0`, `limit=200` | Deterministic AssetRegistry page. `path_filter` is an exact mounted package prefix; `limit` is 1–1000. |
| Readback | `get_chooser_table` | `asset_path`, `include_rows=false`, `row_limit=50` | Bounded counts, columns, references, fallback result, and optional rows/cells. Editor row count comes from `ResultsStructs` rather than stale derived arrays, root context inheritance is honored, and fixed reflected arrays expose bounded `count` + `items`. `row_limit` is 1–500. |
| Readback | `list_chooser_columns` | `asset_path` | Column type, input/output classification, disabled state, row-value property/type/count, and bounded reflected fields. |
| Readback | `list_chooser_rows` | `asset_path`, `start_row=0`, `limit=100` | Bounded row page with result, disabled state, and per-column cell values. |
| Readback | `list_chooser_references` | `asset_path`, `offset=0`, `limit=200` | Stably sorted bounded hard/soft references with reflected source locations and exact loaded-object or AssetRegistry-object existence evidence. A loaded or on-disk package without the referenced export is not evidence that the asset still exists. Blueprint `…_C` fallback requires a soft-class property and an exact `GeneratedClassPath` tag. Response fields expose depth, global visit-budget, and result-count completeness. |
| Validation | `validate_chooser_table` | `asset_path` | Non-mutating row-array/column alignment, known result-payload target, and reference-resolution validation. Warnings do not invalidate; errors do. Invalid/null result payloads and any bounded scan stop return `complete=false` and an error. |
| Deep read/edit | `inspect_chooser` | `asset_path`, `include_cells=false`, `recursive=false` | Deep Chooser-aware inspection, optionally including the nested chooser tree. |
| Deep read/edit | `duplicate_chooser_tree` | `source_assets`, `destination_folder`, `remap_rules?` | Two-pass duplicate and nested/result-reference remap. |
| Deep read/edit | `set_context_object_class` | `asset_path`, `context_name_or_index`, `class_path` | Rewrite a class context entry and recompile. |
| Deep read/edit | `set_result_asset_reference` | `asset_path`, `row_or_column`, `asset_path_value` | Rewrite an asset result row and recompile. |
| Deep read/edit | `set_evaluate_chooser_result_reference` | `asset_path`, `row`, `child_chooser_path` | Rewrite an EvaluateChooser child-table reference and recompile. |
| Validation | `validate_chooser` | `asset_path`, `expected_context_class?`, `expected_result_type?` | Compile-based Chooser validation; distinct from non-mutating `validate_chooser_table`. |
| Authoring | `create_chooser_table` | `asset_path`, `output_type=Object`, `output_class?`, `context_class?` | Create an empty table. |
| Authoring | `add_chooser_column` | `asset_path`, `column_kind`, `binding_property?`, `enum_class?` | Add and back-fill an input/output column. |
| Authoring | `add_chooser_row` | `asset_path`, `cells`, `output_psd` | Append one row while growing every parallel row array atomically. |
| Authoring | `set_chooser_cell` | `asset_path`, `column_index`, `row_index`, typed value fields | Replace one typed predicate cell. |

The six discovery/readback actions remain registered without the optional
Chooser plugin because they use AssetRegistry and reflection only. Calls that
must load a `UChooserTable` return `ErrOptionalDepUnavailable` when the class is
unavailable. The existing ten deep/authoring actions also remain discoverable
but return their explicit Chooser-unavailable handler error off-gate.

See `Plugins/Monolith/Docs/specs/SPEC_MonolithAnimation.md` and
`Plugins/Monolith/Skills/unreal-chooser/SKILL.md`.

---

## niagara

Niagara VFX system editing — emitters, modules, params, renderers, HLSL, dynamic inputs, event handlers, sim stages, NPC, effect types, temporal control, stateless-emitter factory. **129 actions** (108 baseline + 1 layout + 9 temporal-control + 1 stateless-emitter factory + 7 issue #64 Tranche 2 search + 2 PR #65 CustomHlsl-text read/write).

> For full param schemas, call `describe_query("action_schema", target_namespace="niagara", target_action="<name>")` (or `monolith_discover("niagara", detail=true)`). Plain `monolith_discover("niagara")` is terse — names + one-line descriptions only.

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
- **Map authoring:** `author_map_settings` (WorldSettings GameMode override + PlayerStarts + actor instances), `create_nav_harness_map` (now with `game_mode_override` + `player_starts`).

### `editor.trigger_build` / `editor.live_compile`

Trigger a Live Coding compile. Aliased — they're the same handler.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `wait` | bool | optional | Block until compile finishes. Default: `false` |

### `editor.get_build_errors`

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `since` | number | optional | Only errors from the last N seconds |
| `category` | string | optional | Filter to a specific log category |
| `compile_only` | bool | optional | Filter to compile categories only. Default: `false` |

### `editor.get_build_status`

Check compile status: `compiling`, `last_result`, `last_compile_time`, `errors_since_compile`, `patch_applied`. *No parameters.*

### `editor.get_build_summary` · `editor.search_build_output` · `editor.get_compile_output`

Build summary, search-build-log-by-pattern, structured compile report. See `describe_query("action_schema", target_namespace="editor", target_action="<name>")` for params.

### `editor.get_recent_logs` · `editor.search_logs` · `editor.tail_log` · `editor.get_log_categories` · `editor.get_log_stats`

Editor log inspection. `search_logs` accepts `pattern`, `category`, `verbosity`, `limit`. `tail_log` and `get_recent_logs` take `count`.

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
| `source_path` | string | **required** | Absolute path to source image (PNG, TGA, EXR, HDR) |
| `destination` | string | **required** | UE asset path |
| `settings` | object | optional | `{compression, srgb, tiling, max_size, lod_group}` |

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

### `editor.delete_assets`

Delete UE assets by path. **Experimental.** Use the `allowed_prefixes` safety guard.

Runs non-interactively: each target's package dirty flag is cleared and any open asset editor is closed before deletion, and the delete itself runs inside an unattended-script guard so the engine never raises a blocking "asset in use" / "save changes" modal (which would freeze an unattended MCP session).

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `asset_paths` | array | **required** | UE asset paths to delete |
| `allowed_prefixes` | array | optional | Restrict to paths starting with one of these (e.g. `["/Game/AgentTraining/"]`) |
| `force` | bool | optional | `false` (default): soft-delete after closing editors. `true`: `ForceDeleteObjects`, nulling referencers |

**Result:** any assets that could not be deleted are returned in a `failed_to_delete` array rather than aborting the call.

> **Known limitation.** A NiagaraScript created and compiled in the same session can't be deleted until its compile state clears — a transient compilation graph holds a reference (the engine's own Content Browser hits the same wall). It becomes deletable after the state clears, e.g. on editor restart.

### `editor.get_viewport_info`

Current editor viewport camera position, rotation, FOV, resolution. *No parameters.*

### `editor.capture_system_gif`

Capture a Niagara system as a sequence of PNG frames with optional GIF encoding via ffmpeg or python.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `asset_path` | string | **required** | Niagara system asset path |
| `duration_seconds` | number | optional | Default: `2.0` |
| `fps` | integer | optional | Default: `15` |
| `resolution` | integer | optional | Default: `256` |
| `output_path` | string | optional | Default: `Saved/Screenshots/Monolith/GIF_<timestamp>` |
| `encoder` | string | optional | `frames_only` (default), `ffmpeg`, or `python` |

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
| `command` | string | **required** | Python source — inline code, single statement, or a file path with optional space-separated args (when `mode=execute_file`) |
| `mode` | string | optional | `execute_file` (default), `execute_statement`, or `evaluate_statement` |
| `unattended` | bool | optional | Set `GIsRunningUnattendedScript=true` to suppress UI dialogs. Default: `false` |
| `file_scope` | string | optional | Scope for `execute_file`: `private` (isolated, default) or `public` (shared with REPL console) |

### `editor.load_level` · NEW in v0.14.9

Close the current persistent level (without saving) and load the specified level by `/Game/...` asset path. Wraps `ULevelEditorSubsystem::LoadLevel`.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `path` | string | **required** | Asset path of the level to load (e.g. `/Game/Maps/L_Backyard`). Must exist. |

---

## asset

Generic asset lifecycle and package-graph workflows. **20 actions.** The namespace is registered by the `MonolithAsset` editor module when `bEnableAsset=true`.

| Action | Kind | Contract |
|--------|------|----------|
| `import_texture_from_bytes` | write | Decode base64 image bytes into an exact `/Game/...` `UTexture2D`; explicit `fail`, `replace`, or `unique` collision policy and strict texture-role/settings validation. |
| `import_font_family` | write | Preflight absolute `.ttf` sources and every output package, then create a composite `UFont` plus `UFontFace` assets; suffixing requires `allow_unique_names=true`. |
| `import_texture_from_file` | write | Import an external image to the exact requested texture path; nested and compatibility top-level `srgb`/`tiling` must be native JSON booleans, and malformed/duplicate settings or unexpected imported packages are errors. |
| `save_asset` | write | Save one loaded asset package and optionally verify persistence by a non-interactive reload. |
| `delete_assets` | write | Guarded, non-interactive deletion with dry-run, allowed-root checks, postconditions, and optional source-control preflight. |
| `move_assets` | write | Guarded exact package moves; defaults to dry-run, never overwrites, and rejects chains/cycles. |
| `cleanup_moved_redirectors` | write | Validate and remove redirectors left by completed exact moves after referencer, destination, file, and source-control checks. |
| `validate_naming_conventions` | read | Report prefix-rule violations beneath a content path. |
| `batch_rename_assets` | write | Preview or apply batch renames through AssetTools. |
| `find_assets` | read | Fuzzy live-AssetRegistry search with bounded integer limits/thresholds and optional adjacent-transposition matching. |
| `list_supported_asset_enrichers` | read | List typed read-only enrichers available to inspection. |
| `inspect_asset` | read | Inspect one asset with typed enrichment and optional reflected references; soft-reference existence is checked for `/Game`, `/Engine`, and plugin mounts, while `/Script` class paths are the intentional non-asset exception. |
| `inspect_assets_batch` | read | Inspect multiple assets with a success/error result for every requested row. |
| `validate_typed_asset` | read | Run typed validation and report warnings without mutation. |
| `register_content_mount_points` | write | Register explicit process-local content mounts; defaults to dry-run and requires exactly one resolver per mount. |
| `plan_package_graph_copy` | read | Traverse AssetRegistry dependencies and emit an exact source-to-destination copy/remap plan. |
| `copy_package_graph_with_remap` | write | Duplicate a planned package graph without overwriting; requires dry-run or explicit confirmation. |
| `copy_package_graph_with_strategy` | write | Plan and orchestrate supported duplicate/advanced/header-patched/opt-in raw-copy strategies; unsupported manual rows remain explicit blockers. |
| `fixup_copied_references` | write | Rewrite reflected hard/soft references inside copied destination packages; requires dry-run or explicit confirmation. |
| `validate_dependency_closure` | read | Verify copied destination packages no longer depend on disallowed external or legacy source roots. |

All `asset` actions require native JSON arrays/objects rather than JSON-encoded strings. Boolean parameters, including nested and compatibility top-level texture settings, must be JSON booleans; strings such as `"true"`/`"false"` return `-32602` instead of being coerced by UE 5.7. Import handlers also reject unknown enum values, malformed integers, and duplicate setting sources with `-32602`; they do not silently choose a default for invalid input. File texture import succeeds only when the exact requested package is produced. Font import uses exact output names by default and performs source/output preflight before creating packages.

Reflected soft-reference inspection first accepts an already loaded object, then queries the live AssetRegistry for mounted content. This applies equally to project, engine, and plugin content roots; an unregistered or missing `/Engine/...` or `/PluginName/...` asset reports `exists=false` and produces `unresolved_soft_reference`. `/Script/...` class paths are valid non-asset references and are not reported as missing.

For exhaustive parameter schemas, call `describe_query("action_schema", target_namespace="asset", target_action="<action>")`. The authoritative implementation contract is [`specs/SPEC_MonolithAsset.md`](specs/SPEC_MonolithAsset.md).

---

## config

INI config file inspection and search. **6 actions.** Read-only.

### `config.resolve_setting`

Get effective value of a config key across the full INI hierarchy.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `file` | string | **required** | Config category (e.g. `Engine`, `Game`, `Input`) |
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

---

## localization

Localization culture discovery and StringTable asset management. **10 actions.** The four read actions never mutate packages. Every write requires either `dry_run=true` or `confirm=true`; `save` is opt-in where applicable. StringTable assets must resolve under `/Game`; CSV paths must remain beneath the project directory without traversing symlinks or junctions. Supplied JSON values are checked against their declared types without scalar coercion, and the `culture_names` and `metadata` complex parameters explicitly reject JSON-encoded strings.

### `localization.list_cultures`

List available cultures known to Unreal internationalization.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `culture_names` | array | optional | Culture names to resolve; omitted returns configured/default culture context |
| `include_derived` | boolean | optional | Include derived cultures when resolving `culture_names`. Default: `true` |

### `localization.list_string_tables`

List StringTable assets under a project content path. Included entries are sorted by key, and one shared entry-row budget is consumed across the returned table summaries rather than being reset for every table.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `path` | string | optional | Content path to scan. Default: `/Game` |
| `include_entries` | boolean | optional | Include capped entry rows. Default: `false` |
| `include_metadata` | boolean | optional | Include per-entry metadata when entries are included. Default: `false` |
| `limit` | integer | optional | Maximum table summaries and aggregate entry rows to return. Default: `100`; clamped to `1..1000` |

When `include_entries=true`, the response includes `entry_budget`, `available_entry_count`, `returned_entry_count`, and `truncated_entry_count`. Entry counts cover the returned table summaries; `matched_count`, `returned_count`, and `truncated_remaining` separately describe table-summary truncation.

### `localization.get_string_table`

Inspect a StringTable asset and return entries sorted by key before the cap is applied.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `asset_path` | string | **required** | StringTable asset path under `/Game` |
| `include_metadata` | boolean | optional | Include per-entry metadata. Default: `true` |
| `limit` | integer | optional | Maximum entries to return. Default: `200`; clamped to `1..1000` |

### `localization.validate_string_table`

Validate a StringTable for empty keys, empty source strings, duplicate-looking keys, and large-output warnings. The response returns at most 200 rows in `issues`; `issue_count` remains the full total, while `returned_issue_count` and `truncated_issue_count` make truncation explicit. `valid` is based on the full total.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `asset_path` | string | **required** | StringTable asset path under `/Game` |

### `localization.create_string_table`

Create a StringTable asset under `/Game`. If `asset_path` includes an explicit object segment, its object name must match the package leaf (for example, `/Game/Localization/ST_UI.ST_UI`).

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `asset_path` | string | **required** | New StringTable asset path under `/Game` |
| `namespace` | string | optional | StringTable namespace; defaults to the asset name |
| `dry_run` | boolean | optional | Preview without writing. Default: `false` |
| `confirm` | boolean | optional | Must be `true` for a non-dry-run write. Default: `false` |
| `save` | boolean | optional | Save the package after creation. Default: `false` |

### `localization.set_string_entry`

Add or replace one StringTable entry and optional string metadata.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `asset_path` | string | **required** | StringTable asset path under `/Game` |
| `key` | string | **required** | Entry key |
| `source_string` | string | **required** | Source string |
| `metadata` | object | optional | String-to-string metadata fields; keys must not have leading/trailing whitespace, use reserved CSV structural names, or collide case-insensitively |
| `dry_run` | boolean | optional | Preview without writing. Default: `false` |
| `confirm` | boolean | optional | Must be `true` for a non-dry-run write. Default: `false` |
| `save` | boolean | optional | Save the package after mutation. Default: `false` |

### `localization.remove_string_entry`

Remove one StringTable entry by key.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `asset_path` | string | **required** | StringTable asset path under `/Game` |
| `key` | string | **required** | Entry key |
| `dry_run` | boolean | optional | Preview without writing. Default: `false` |
| `confirm` | boolean | optional | Must be `true` for a non-dry-run write. Default: `false` |
| `save` | boolean | optional | Save the package after mutation. Default: `false` |

### `localization.set_string_metadata`

Add, replace, or remove one metadata field on a StringTable entry. An empty string is a valid stored value and adding an absent field with that value counts as a change.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `asset_path` | string | **required** | StringTable asset path under `/Game` |
| `key` | string | **required** | Entry key |
| `metadata_key` | string | **required** | Metadata key without leading/trailing whitespace; reserved CSV structural names are rejected |
| `metadata_value` | string | optional | Metadata value to set; required unless `remove=true` |
| `remove` | boolean | optional | Remove `metadata_key` instead of setting it. Default: `false` |
| `dry_run` | boolean | optional | Preview without writing. Default: `false` |
| `confirm` | boolean | optional | Must be `true` for a non-dry-run write. Default: `false` |
| `save` | boolean | optional | Save the package after mutation. Default: `false` |

### `localization.import_string_table_csv`

Import `key`, `source_string`, and optional metadata columns from a CSV into an existing StringTable. Header names must be unique case-insensitively and must not have leading/trailing whitespace. Monolith exports may include the structural `__monolith_metadata_presence_v1` column, whose per-row JSON string array distinguishes a present empty metadata value from an absent field; change detection preserves that distinction instead of comparing empty values alone. The importer validates that list against actual metadata columns; legacy CSV files without the structural column retain the prior rule that an empty metadata cell is absent. It removes only a validated versioned spreadsheet guard and therefore preserves literal leading apostrophes. On UE 5.8, `replace_existing=true` preserves developer notes for keys that are re-imported. If a requested save fails, entries, metadata, notes, and the original package dirty state are restored.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `asset_path` | string | **required** | StringTable asset path under `/Game` |
| `file_path` | string | **required** | Source CSV path under the project directory |
| `replace_existing` | boolean | optional | Clear existing entries before import. Default: `false` |
| `dry_run` | boolean | optional | Preview without writing. Default: `false` |
| `confirm` | boolean | optional | Must be `true` for a non-dry-run write. Default: `false` |
| `save` | boolean | optional | Save the package after mutation. Default: `false` |

### `localization.export_string_table_csv`

Export a StringTable to a CSV under the project directory. When metadata columns exist, export adds the structural `__monolith_metadata_presence_v1` column so present empty strings round-trip without being materialized on rows where the field was absent. Formula-looking cells receive the collision-safe `'__monolith_formula_guard_v1__:` prefix; an existing marker prefix is doubled so import can distinguish protection from literal content, including `'=literal`. Export fails if a metadata key has leading/trailing whitespace or case-insensitively collides with `key`, `source_string`, or the presence column, preventing an ambiguous or destructive re-import.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `asset_path` | string | **required** | StringTable asset path under `/Game` |
| `file_path` | string | **required** | Destination CSV path under the project directory |
| `include_metadata` | boolean | optional | Include metadata columns. Default: `true` |
| `dry_run` | boolean | optional | Preview without writing. Default: `false` |
| `confirm` | boolean | optional | Must be `true` for a non-dry-run write. Default: `false` |

---

## project

Project-wide asset index backed by SQLite + FTS5. **7 actions.**

### `project.search`

Full-text search across all indexed project assets, nodes, variables, and parameters.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `query` | string | **required** | FTS5 search query (supports `AND`, `OR`, `NOT`, `prefix*`) |
| `limit` | integer | optional | Default: `50` |

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

---

## collection

Content Browser collection management backed by Unreal's `CollectionManager`. **13 actions.** Unless stated otherwise, `share_type` accepts `local`, `private`, `shared`, or `system` and defaults to `local` only when omitted. An explicitly empty `share_type` is invalid rather than an alias for the default store.

| Action | Parameters | Behavior |
|--------|------------|----------|
| `collection.list_collections` | `share_type` (`all`) | List collection names, share types, storage modes, resolved asset counts, saved dynamic query text, and optional colors. All returned dynamic collections share one asset enumeration. |
| `collection.get_collection` | `name` (**required**), `share_type` | Return one collection's details, including current resolved `asset_count` and `query_text` for a dynamic collection. |
| `collection.create_collection` | `name` (**required**), `share_type`, `storage_mode` (`static`; `static` or `dynamic`) | Create a collection and return its details. |
| `collection.delete_collection` | `name` (**required**), `share_type`, `force` (`false`) | Delete a collection. Resolved static or dynamic membership is rejected unless `force=true`. |
| `collection.add_assets` | `name` (**required**), `share_type`, `asset_path` or `asset_paths[]` | Add one or more soft object paths to a static collection. At least one path is required. |
| `collection.remove_assets` | `name` (**required**), `share_type`, `asset_path` or `asset_paths[]` | Remove one or more soft object paths from a static collection. At least one path is required. |
| `collection.list_assets` | `name` (**required**), `share_type`, `recursive` (`self`; `self`, `children`, `parents`, or `all`) | Resolve and list matching soft object paths across static and dynamic collections in the selected recursion scope. |
| `collection.contains_asset` | `name` (**required**), `asset_path` (**required**), `share_type`, `recursive` (`self`) | Resolve static and dynamic membership and report whether the selected collection scope contains the normalized path. |
| `collection.set_dynamic_query` | `name` (**required**), `query_text` (**required**), `share_type` | Set a dynamic collection's query text and return the stored query. |
| `collection.get_dynamic_query` | `name` (**required**), `share_type` | Return a dynamic collection's query text. |
| `collection.set_collection_color` | `name` (**required**), `share_type`, `color` (`{r,g,b,a}` in `0..1`; omit to clear) | Set or clear the collection color. Alpha defaults to `1`; the result reports `updated`, `name`, `share_type`, `color_cleared`, and the applied `color` when present without resolving collection membership. |
| `collection.validate_collection_name` | `name` (**required**), `share_type` (`local`; also accepts `all`) | Ask `CollectionManager` whether a name is valid and return its validation error when invalid. |
| `collection.create_unique_collection_name` | `base_name` (**required**), `share_type` | Generate a valid, non-conflicting name without creating the collection. |

All scalar and array element types are validated exactly, and required strings such as `query_text` must be non-empty. Optional enum defaults apply only when the field is absent: explicitly empty `share_type` and `storage_mode` values return `-32602`. A string supplied where a bool, number, object, or array is required is not coerced. Every target-specific action returns JSON-RPC `-32602` when the selected collection does not exist: `list_assets` does not masquerade as a successful empty collection, and `contains_asset` does not convert lookup failure to `contains=false`. Dynamic membership is evaluated against the live Content Browser asset set through `ICollectionContainer::TestDynamicQuery`; core `Name`, package-folder `Path`, `Class`/`Type`, `Collection`/`Tag`, and registered item-attribute expressions are supported. `FContentBrowserItemDataAttributeValue::GetValue<FString>()` safely normalizes the engine's string, name, and text backing variants in both supported engine versions. `Path` excludes the virtual `/All` prefix and asset leaf. One `list_collections` call compiles all requested dynamic filters and resolves them in one asset enumeration; every Content Browser alias is evaluated before successful matches are deduplicated per logical asset and per collection using a compact filter bitset. Nested dynamic collections share per-asset membership caching. A nested evaluation failure or dynamic-reference cycle propagates as an explicit error instead of silently becoming a non-match. A newly created dynamic collection with no saved query resolves to zero assets, including when referenced by another dynamic collection, instead of treating an empty text filter as “match all.” Non-forced deletion uses the same resolved membership, including dynamic query results. Unique-name candidates are checked through `ICollectionContainer::IsValidCollectionName` before success. An Unreal collection operation that fails after validation returns `-32603` and preserves the engine-provided error text when available. Mutating calls reject read-only share types rather than falling back to another scope.

---

## source

Unreal Engine C++ source code navigation. 1M+ symbols indexed. **12 actions** (11 navigation + 1 Reflection Intelligence audit registered cross-namespace in v0.17.0).

### `source.read_source`

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `symbol` | string | **required** | Class, function, or struct name |
| `include_header` | bool | optional | Include the header declaration. Default: `false` |
| `max_lines` | integer | optional | Default: `500` |
| `members_only` | bool | optional | Only show class members. Default: `false` |

### `source.find_references` · `source.find_callers` · `source.find_callees`

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `symbol` | string | **required** | Symbol or function name |
| `ref_kind` | string | optional | (`find_references` only) Filter by reference kind |
| `limit` | integer | optional | Default: `50` |

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
| `file_path` | string | **required** | Source file path |
| `start_line` | integer | optional | Default: `1` |
| `end_line` | integer | optional | Default: end of file |

### `source.trigger_reindex` · `source.trigger_project_reindex`

`trigger_reindex` does a full clean build (engine + shaders + project). `trigger_project_reindex` is incremental (project Source/ + Plugins/ only). Both take *no parameters*.

**Returns:** a text confirmation that indexing started. Since 0.22.0 both report failure honestly: if the run does not start — a reindex is already in flight, the worker thread could not be created, or (for `trigger_project_reindex`) `EngineSource.db` does not exist yet — the action returns an **error** naming the cause instead of a success message. Earlier builds always claimed the reindex had started.

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

## source_control

Provider-backed source-control preparation and bounded Perforce inspection. **11 actions.** The canonical path parameter is `paths`; every path action also accepts the alias `files`, and either field may contain one string or an array of strings. Relative paths resolve from the project directory. A slash-prefixed input is treated as an Unreal package/object path only when its package mount point exists; `/home/...` and other POSIX absolute paths remain filesystem paths.

Boolean options are strict JSON booleans. Pass `true` or `false`; quoted strings, numeric substitutes, and explicit `null` values return `-32602`.

| Action | Parameters | Description |
|--------|------------|-------------|
| `get_capabilities` | none | Active provider identity, installed provider names, and supported action roster |
| `get_status` | `paths` or `files` | Normalized files and provider state (`checked_out`, `added`, `modified`, `conflicted`, capabilities, and other-user checkout details) |
| `checkout` | `paths`/`files`, `dry_run?=false` | Check out through the active Unreal provider |
| `add` | `paths`/`files`, `dry_run?=false` | Mark for add through the active Unreal provider |
| `checkout_or_add` | `paths`/`files`, `dry_run?=false` | Plan or execute checkout for tracked files and add for provider-confirmed untracked files; reports `ok=false` and performs no provider mutation when any path is blocked by another-user checkout, conflict, stale revision, or an unknown/unsupported state |
| `delete` | `paths`/`files`, `dry_run?=false`, `confirm?=false` | Mark for delete; requires `confirm=true` unless dry-running |
| `mark_for_delete` | same as `delete` | Explicitly named alias of the delete provider operation |
| `revert` | `paths`/`files`, `dry_run?=false`, `confirm?=false` | Revert files; requires `confirm=true` unless dry-running |
| `revert_unchanged` | `paths`/`files`, `dry_run?=false`, `confirm?=false` | Revert unchanged files; requires `confirm=true` unless dry-running |
| `list_opened` | `changelist?`, `resolve_packages?=true`, `limit?=200` | Bounded `p4 -ztag opened -m (limit + 1)` with optional local/package resolution; `limit` is integral `1..5000` |
| `map_depot_paths` | `paths` or `files` | Map up to 5,000 depot/client/local/package paths, preserving one result row per input |

`delete`, `mark_for_delete`, `revert`, and `revert_unchanged` return `ok=false` without executing when neither `dry_run=true` nor `confirm=true` is supplied. An unavailable provider is reported explicitly as `available=false`; the action does not edit file attributes or select an alternate provider. `checkout_or_add` distinguishes benign skips (already editable, safely checked out/added, or a not-yet-created path when add-missing is disabled) from blocking skips. Blocking facts are evaluated before those shortcuts, an invalid provider-state result is not treated as proof that a file is untracked, and add never runs after a failed or cancelled checkout.

Perforce mapping is bounded to 128 paths and 24,000 encoded characters per `p4 where` command, at most 40 commands per call, and a 30-second deadline per process. A timeout terminates the process tree, marks the current and unstarted batches with explicit diagnostics, and launches no further batches. Windows and Unix command lines use platform-compatible argument quoting. Both `map_depot_paths` and `list_opened(resolve_packages=true)` return a backend error when a required `p4 where` process cannot launch or times out; they never present that outage as ordinary unmapped rows. `list_opened` exposes `observed_count`, `returned_count`, `sentinel_record_count`, `count_is_lower_bound`, `has_more`, and `truncated`; treat `count` as exact only when `count_is_lower_bound=false`.

---

## pcg

Exact-path PCG discovery, typed graph authoring, complete identity-preserving graph replacement, Blueprint-template assignment, and bounded editor-world component lifecycle control. **28 actions.** The module requires the engine PCG plugin and uses public UE 5.7/5.8 APIs. Project-owned write targets, confirmation gates, structural validation, rollback, save evidence, and handler-owned source-control preparation are part of the action contracts; no default graph, alternate asset, or string-encoded JSON-container fallback is accepted.

For exhaustive parameters and defaults, call `describe_query("action_schema", target_namespace="pcg", target_action="<name>")` or `monolith_discover("pcg", detail=true)`. The focused ownership, bounds, and engine-version contract lives in [`specs/SPEC_MonolithPCG.md`](specs/SPEC_MonolithPCG.md).

PCG schema scalars are type-strict. Pass native JSON strings, booleans, and finite integral numbers within each documented range; quoted booleans/numbers, boolean path values, fractional integers, and out-of-range list limits return invalid params instead of being coerced or clamped. The documented canonical decimal-string form for full-range `int64` user-parameter values is the deliberate exception.

| Action | Kind | Contract |
|--------|------|----------|
| `get_status` | read | PCG readiness plus the current dynamically enumerated `pcg` action roster |
| `list_graph_assets` | read | Bounded AssetRegistry graph discovery |
| `get_graph_asset` | read | One bounded graph metadata row by exact asset path |
| `list_components` | read | Bounded active-editor-world PCG component rows with canonical paths |
| `remap_graph_references` | guarded write | Dry-run/confirm bounded soft-reference migration with strict object `root_remaps` |
| `list_pcg_node_types` | read | Concrete loaded settings classes, aliases, and optional editable-field summaries |
| `create_pcg_graph` | write | Create one project-owned `UPCGGraph`; explicit rerun policy |
| `get_pcg_graph_info` | read | Bounded nodes, pins, edges, settings, positions, and special-node state |
| `add_pcg_node` | write | Typed settings-node creation with strict initial settings and explicit rerun policy |
| `remove_pcg_node` | write | Remove one element node and incident edges; special nodes are rejected |
| `connect_pcg_nodes` | write | Exact pin-label connection after ownership, direction, type, capacity, and cycle checks |
| `disconnect_pcg_nodes` | write | Idempotent exact-edge removal |
| `set_pcg_node_params` | guarded write | Canonical reflection-walker dry-run/apply against one settings object |
| `set_pcg_graph_user_parameters` | guarded write | Atomic bounded scalar graph-parameter schema/default authoring; dry-run by default |
| `set_pcg_subgraph` | guarded write | Exact graph/interface assignment through `SetSubgraph`, with recursion/filter guards and rollback |
| `replace_pcg_graph_contents` | guarded write | Seeded complete donor-state replacement that preserves the target identity, validates exact persistent equality, and requires confirmed commit |
| `validate_pcg_graph` | read | Bounded structural ownership/topology/connectivity diagnostics |
| `create_component` | write | Create and register one exact-name transactional component on an exact user-authored actor |
| `get_component` | read/poll | Exact component graph/configuration/lifecycle state, bounded parameters, optional exact managed-resource count |
| `set_component_graph` | write | Exact graph assignment to an idle ungenerated live component with read-back and optional save |
| `set_blueprint_component_graph` | guarded write | Dry-run/confirm graph assignment to one exact Actor Blueprint SCS PCG template, compile/re-resolve/read-back, and rollback |
| `set_component_settings` | write | Atomic validated seed/activation/partition/trigger configuration |
| `generate_component` | async runtime | Native non-blocking generation schedule; task ids are decimal strings |
| `refresh_component` | async runtime | Native refresh schedule/coalescing with no hidden replace or cancel behavior |
| `cancel_component` | runtime | Generation-only cancellation; refresh, cleanup, and runtime-scheduler ownership fail closed |
| `cleanup_component` | async runtime | Native non-blocking cleanup schedule with explicit remove/reuse intent and truthful coalescing |
| `get_component_output` | read | Independently bounded output/tag/resource/managed-object summaries with truncation evidence |
| `set_component_user_parameters` | guarded write | Atomic strict-scalar graph-instance override apply/reset with dry-run and optional level save |

Generation and cleanup are schedule-and-poll operations. Every available task id is a decimal string paired with a validity flag. Generation ids are readable on UE 5.7/5.8. UE 5.8 also exposes the component's current cleanup task id; UE 5.7 returns `cleanup_task_id_supported=false` and omits that unavailable field, while a newly scheduled cleanup still returns its exact `scheduled_task_id`. Callers poll `get_component` until lifecycle state is idle and must not busy-wait inside one action.

Exact persistent graph replacement is deliberately stricter than copying nodes. It preserves the canonical target package/object and default I/O identities while comparing the complete donor-owned object graph under bounded package-persistent semantics. On UE 5.8+, the target-owned `LastEditedDocuments` editor workspace is preserved; UE 5.7 has no corresponding field. Any failure after mutation attempts snapshot rollback and reports whether state and dirty-bit restoration were verified.

Owning-level save failure is atomic for `create_component`, `set_component_graph`, `set_component_settings`, and `set_component_user_parameters`. Structured error data includes `mutation_attempted`, `rollback_complete`, `changed`, and `save_error`, with `rollback_error` when restoration is incomplete. A verified rollback reports `changed=false` and restores the original dirty bit; an incomplete rollback keeps the package dirty so a surviving live mutation cannot be hidden.

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

## ui

UMG widget Blueprint CRUD, templates, styling, animation (v1 + v2), the schema-driven **Spec / EffectSurface** architecture, settings scaffolding, accessibility, **CommonUI**, and GAS UI bindings. **138 actions** — the UMG + Spec/EffectSurface baseline (66 always-on, incl. the v0.15.0 navigation/conversion gap-closure + headline scaffolders) + 51 CommonUI (registered when `WITH_COMMONUI=1`) + 4 GAS UI binding aliases. The four CommonUI-surface gap-closure actions (`convert_border_to_common`, `convert_textblock_to_common`, `set_action_bar_button_class`, `apply_token_binding`) are `#if WITH_COMMONUI`-gated.

> For full param schemas, call `describe_query("action_schema", target_namespace="ui", target_action="<name>")` (or `monolith_discover("ui", detail=true)`). Plain `monolith_discover("ui")` is terse — names + one-line descriptions only. The surface is large — categories below; the v0.15.0-new actions are flagged.

**Action categories (UMG + Spec baseline, always registered):**

| Category | Actions | Examples |
|----------|---------|----------|
| Widget CRUD | 9 | `create_widget_blueprint`, `get_widget_tree`, `add_widget` (v0.21.0: `parent` alias for `parent_name`), `remove_widget`, `set_widget_property` (accepts `value` alias; v0.21.0: common `UWidget` props allowlisted by default), `compile_widget` (returns `errors[]`/`warnings[]`), `list_widget_types`, `rename_widget`, `dump_blueprint_compile_log` |
| Variable flags (v0.15.0) | 3 | `add_widget_variable`, `set_widget_is_variable`, `list_widget_property_enums` |
| Root / reparent (v0.15.0) | 1 | `reparent_widget_root` |
| Slot / layout | 4 | `set_slot_property` (v0.21.0: grid-slot `row`/`column`/`row_span`/`column_span`), `set_anchor_preset`, `move_widget`, `set_brush` (v0.21.0: `property_name` optional/auto-resolved, `color` alias for `tint_color`) |
| Styling | 6 | `set_font`, `set_color_scheme`, `batch_style`, `set_text`, `set_image`, `setup_list_view` |
| Templates / scaffolds | 13 | `create_hud_element`, `create_menu`, `create_settings_panel`, `create_dialog`, `create_notification_toast`, `create_loading_screen`, `create_inventory_grid`, `create_save_slot_list`, `scaffold_game_user_settings`, `scaffold_save_game`, `scaffold_save_subsystem`, `scaffold_audio_settings`, `scaffold_input_remapping` |
| Headline scaffolders (v0.15.0) | 3 | `scaffold_main_menu`, `scaffold_settings_panel_with_tabs`, `scaffold_pause_menu` |
| Animation v1 | 5 | `list_animations`, `get_animation_details`, `create_animation`, `add_animation_keyframe`, `remove_animation` |
| Animation v2 | 5 | `create_animation_v2`, `add_bezier_eased_segment`, `bake_spring_animation`, `add_animation_event_track`, `bind_animation_to_event` |
| Inspection | 3 | `list_widget_events`, `list_widget_properties`, `get_widget_bindings` |
| Design import | 4 | `import_texture_from_bytes`, `import_font_family`, `set_rounded_corners`, `create_gradient_mid_from_spec` |
| EffectSurface (provider-gated, `-32010` when absent) | 13 | `apply_box_shadow`, `set_effect_surface_corners`, `set_effect_surface_fill`, `set_effect_surface_border`, `set_effect_surface_dropShadow`, `set_effect_surface_innerShadow`, `set_effect_surface_glow`, `set_effect_surface_filter`, `set_effect_surface_backdropBlur`, `set_effect_surface_insetHighlight`, `apply_effect_surface_preset` |
| Spec round-trip | 4 | `build_ui_from_spec`, `dump_ui_spec`, `dump_ui_spec_schema`, `build_menu_from_spec` (v0.15.0) |
| Accessibility | 6 | `scaffold_accessibility_subsystem`, `audit_accessibility`, `set_colorblind_mode`, `set_text_scale`, `apply_high_contrast_variant`, `set_text_scale_binding` |
| Allowlist / diagnostics | 2 | `dump_property_allowlist`, `dump_style_cache_stats` |

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

## gas

Gameplay Ability System integration. **135 actions** across 11 categories — covers the full GAS authoring pipeline. Registration follows `bEnableGAS`; `WITH_GBA` gates only optional Blueprint Attributes support. The separate [`input`](#input) namespace remains registered when `bEnableGAS=false`.

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
| Inspect (PIE) | 6 | `export_gas_manifest`, `snapshot_gas_state`, `get_tag_state`, `get_cooldown_state`, `trace_ability_activation`, `compare_gas_states` |
| UI bindings | 4 | `bind_widget_to_attribute`, `unbind_widget_attribute`, `list_attribute_bindings`, `clear_widget_attribute_bindings` *(also aliased into `ui` namespace — same handlers)* |

### `gas.grant_ability_to_pawn` · NEW in Phase J F8

Grant a `UGameplayAbility` to a pawn's `UAbilitySystemComponent` directly without scaffold-side wiring or `apply_effect` ceremony. See `describe_query("action_schema", target_namespace="gas", target_action="grant_ability_to_pawn")` for params.

See `Plugins/Monolith/Docs/specs/SPEC_MonolithGAS.md` for the deep dive.

---

## input

Enhanced Input asset integration. **10 actions** inspect and author `UInputAction` and `UInputMappingContext` assets. This namespace is owned by `MonolithGAS` but registers before the `bEnableGAS` check, so Enhanced Input asset work remains available even when GAS authoring is disabled.

> For exhaustive live schemas, call `describe_query("action_schema", target_namespace="input", target_action="<name>")` (or `monolith_discover("input", detail=true)`).

| Action | Required params | Purpose |
|--------|-----------------|---------|
| `list_input_actions` | — | List actions under optional `/Game` `path` (omission defaults to `/Game`); `include_details=true` loads value type, triggers, and modifiers. |
| `get_input_action` | `asset_path` | Inspect one `UInputAction`. |
| `create_input_action` | `asset_path` | Create an action or, with `overwrite=true`, update an existing action. |
| `set_input_action_properties` | `asset_path` | Update value type, description, consumption, pause, reservation, or accumulation properties. |
| `list_input_mapping_contexts` | — | List contexts under optional `/Game` `path` (omission defaults to `/Game`); `include_details=true` includes mappings. |
| `get_input_mapping_context` | `asset_path` | Inspect one `UInputMappingContext`. |
| `create_input_mapping_context` | `asset_path` | Create a context or, with `overwrite=true`, update its description. |
| `add_input_mapping` | `context_path`, `action_path`, `key` | Add or update a mapping; optionally clone another mapping or instantiate explicit modifier/trigger classes. |
| `remove_input_mapping` | `context_path`, `action_path`, `key` | Remove matching mappings and report `would_remove_count`. |
| `validate_input_mappings` | — | Validate selected `context_paths`, or all contexts under optional `path`, for missing actions and duplicate-key conflicts. |

All five writers (`create_input_action`, `set_input_action_properties`, `create_input_mapping_context`, `add_input_mapping`, `remove_input_mapping`) share the same mutation contract:

- `dry_run=true` returns the proposed values with `preview_state="proposed"` without creating an object/package, constructing modifier/trigger UObjects, mutating an asset, dirtying a package, or saving.
- A committed write requires `confirm=true`; `save` defaults to `false`.
- Package and asset inputs must resolve within `/Game`; malformed package roots and object paths whose explicit object name differs from the package asset name are rejected. Scalar booleans and strings are type-checked rather than coerced. Schema-declared `modifier_classes`, `trigger_classes`, and `context_paths` accept either a JSON array or the registry's documented JSON-encoded-array compatibility form; recovered array elements are still required to be non-empty strings.
- A semantic no-op does not call `Modify()`, dirty a package, or save it.
- Unsaved confirmed creation is fully transactional. Undo removes the new asset from its package path and the Asset Registry; the transaction retains the transient object through GC so Redo restores the same object and proposed state.
- `save=true` is an explicit disk-persistence boundary. If disk save fails after a mutation, the action returns an error with `error.data` containing the normal action result plus `save_failed=true`, `mutation_committed=true`, `partial_mutation=true`, `retry_safe=false`, `save_error`, and retry guidance.
- `add_input_mapping` reuses the existing action+key mapping unless `allow_duplicate=true`. Source cloning requires `source_context_path`, `source_action_path`, and `source_key` together. If those fields match more than one source row, the call fails as ambiguous unless `source_mapping_index` selects an exact matching row. Explicit `modifier_classes` / `trigger_classes` replace cloned or existing arrays; an explicit empty array clears them. Dry-run validates soft class-path syntax and any already-loaded class, returns `class_resolution="deferred_until_confirm"`, and defers package/class/CDO loading until a confirmed call.
- `validate_input_mappings` distinguishes omission from selection: omitted `context_paths` scans the optional `path`, while an explicit empty array checks zero contexts and does not fall back to a project-wide scan. Each array element normalizes Windows `\` separators before canonical `/Game` validation.
- Removing an absent mapping is a successful no-op with `would_remove_count=0`.

See `Plugins/Monolith/Docs/specs/SPEC_MonolithGAS.md` and `Plugins/Monolith/Skills/unreal-input/SKILL.md` for the module contract and task workflow.
## gamefeatures
Game Feature plugin/data inspection and guarded instanced-action authoring. The
full compiled and inspection-enabled surface has **15 actions**:
- if the target does not explicitly enable the engine `GameFeatures` plugin,
  `WITH_MONOLITH_GAMEFEATURES=0` and only `get_status` is registered;
- when compiled with `GameFeatures`, `get_status` plus eight writer actions are
  registered by default;
- setting `bEnableGameFeatureActions=true` and restarting the editor registers
  the six additional inspection actions.
| Action | Core parameters | Description |
|--------|-----------------|-------------|
| `get_status` | none | Report compile state, inspection/creation flags, loaded modules, discovered plugin count, and the registered/available action rosters |
| `list_plugins` | `limit?=50`, `include_engine?=false` | List bounded GameFeature-style plugin descriptors and candidate data assets |
| `find_game_feature_data` | `plugin_name?`, `asset_path?` | Resolve one plugin or asset path to Asset Registry metadata; multiple unmatched data assets require an explicit `asset_path` |
| `describe_game_feature_data` | plugin/asset selector plus bounded reflection options | Load one resolved `UGameFeatureData` and summarize its instanced actions/properties |
| `list_action_classes` | `limit?=100`, optional module/name/property filters | List loaded `UGameFeatureAction` subclasses and bounded editable property schemas |
| `describe_action_set` | `action_set_path`, bounded action/property options | Summarize an existing ActionSet-style asset's instanced `Actions` array |
| `add_action_set_input_mapping` | `action_set_path`, `mapping_context_path`, optional class/name/priority/save/dry-run controls | Idempotently create/reuse an input-mapping action and mapping entry |
| `set_primary_asset_scan` | `game_feature_data_path`, `primary_asset_type`, optional class/directory/asset/save/dry-run controls | Create or update one `PrimaryAssetTypesToScan` entry |
| `add_game_feature_data_input_mapping` | `game_feature_data_path`, `mapping_context_path`, optional class/name/priority/save/dry-run controls | Add an input-mapping action directly to `UGameFeatureData` |
| `add_game_feature_data_widgets` | `game_feature_data_path`, optional layout/widget arrays and property-name overrides | Add validated class plus GameplayTag widget/layout entries |
| `add_game_feature_data_components` | `game_feature_data_path`, `actor_class`, `component_class`, optional flags/save/dry-run controls | Add one actor/component request to an AddComponents-style action |
| `add_game_feature_data_gameplay_cue_paths` | `game_feature_data_path`, `directory_path` or `directory_paths[]`, optional class/name/save/dry-run controls | Add slash-prefixed GameplayCue directories idempotently |
| `add_game_feature_data_abilities` | `game_feature_data_path`, `actor_class`, optional ability/attribute/ability-set arrays | Add validated actor-scoped grant entries to an AddAbilities-style action |
| `remove_game_feature_data_action` | `game_feature_data_path` plus index, name, and/or class selector | Preview or remove the first/all matching instanced action entries |
| `validate_plugin` | `plugin_name` | Check descriptor presence, enabled `GameFeatures` dependency, content root, data asset, and reserved creation gate |
All eight writers operate only on existing assets, preflight the complete edit
on a transient action copy before mutation, support `dry_run=true`, and save
only when `save=true`. A supplied `action_name` is an exact name-and-class
selector; rejected and idempotent no-op calls do not dirty the package. Soft
class fields enforce their reflected `MetaClass`, and ability-related object
types are checked in dry-run and real-write paths alike. The namespace does not
activate, deactivate, create, rename, or delete Game Feature plugins. See
[`specs/SPEC_MonolithGameFeatures.md`](specs/SPEC_MonolithGameFeatures.md) for
full parameter and result contracts.

---

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

See `Plugins/Monolith/Docs/specs/SPEC_MonolithAI.md` for the deep dive — it's a long one.

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
| Sound Cue graph | 9 | `create_sound_cue`, `get_sound_cue_graph`, `add_sound_cue_node`, `remove_sound_cue_node`, `connect_sound_cue_nodes`, `set_sound_cue_first_node`, `set_sound_cue_node_property`, `list_sound_cue_node_types`, `find_sound_waves_in_cue`, `validate_sound_cue` |
| Sound Cue spec / templates | 8 | `build_sound_cue_from_spec`, `create_random_sound_cue`, `create_layered_sound_cue`, `create_looping_ambient_cue`, `create_distance_crossfade_cue`, `create_switch_sound_cue`, `duplicate_sound_cue`, `delete_audio_asset` |
| Preview | 3 | `preview_sound`, `stop_preview`, `get_sound_cue_duration` |
| Perception bindings | 4 | `bind_sound_to_perception`, `unbind_sound_from_perception`, `get_sound_perception_binding`, `list_perception_bound_sounds` |
| MetaSound assets | 3 | `create_metasound_source`, `create_metasound_patch`, `create_metasound_preset` |
| MetaSound graph | 12 | `add_metasound_node`, `remove_metasound_node`, `connect_metasound_nodes`, `disconnect_metasound_nodes`, `add_metasound_input`, `add_metasound_output`, `set_metasound_input_default`, `add_metasound_interface`, `get_metasound_graph`, `list_metasound_connections`, `add_metasound_variable`, `set_metasound_node_location` |
| MetaSound discovery (Builder-side) | 6 | `list_available_metasound_nodes`, `get_metasound_node_info`, `find_metasound_node_inputs`, `find_metasound_node_outputs`, `get_metasound_input_names` |
| MetaSound document introspection (v0.14.10, PR #18) | 12 | `list_metasounds`, `list_metasound_documents`, `get_metasound_document`, `get_metasound_summary`, `inspect_metasound_node_instance`, `get_metasound_document_connections`, `get_metasound_document_variables`, `get_metasound_user_parameters`, `search_metasound_document_nodes`, `get_metasound_info`, `get_metasound_dependencies`, `validate_metasound` |
| MetaSound spec / templates | 6 | `build_metasound_from_spec`, `create_oneshot_sfx`, `create_looping_ambient_metasound`, `create_synthesized_tone`, `create_interactive_metasound`, `create_metasound_preset` |

### `audio.create_test_wave` · NEW in Phase J F18

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `path` | string | **required** | Destination asset path under `/Game/` |
| `frequency_hz` | number | optional | Sine frequency (20.0 to 20000.0). Default: `440.0` |
| `duration_seconds` | number | optional | Clip length (0.05 to 5.0). Default: `0.5` |
| `sample_rate` | integer | optional | Allowlist `{22050, 44100, 48000}`. Default: `44100` |
| `amplitude` | number | optional | Peak amplitude in `(0.0, 1.0]`. Default: `0.5` |

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

## dataflow

Bounded, read-only `UDataflow` asset discovery, graph/node-schema inspection,
integrity validation, variable inspection, and editor-comment membership.
**8 actions.** Implemented by `MonolithDataflow` with explicit
`DataflowCore`/`DataflowEngine` dependencies.

| Action | Key params | Notes |
|--------|-----------|-------|
| `get_status` | — | Exact eight-action roster, Dataflow plugin/module state, and explicit authoring/evaluation/regeneration=false flags |
| `list_assets` | `package_path?`, `limit?` | AssetRegistry-only bounded discovery; reports count completeness and never loads assets |
| `get_dataflow_graph` | `asset_path`, `node_limit?`, `connection_limit?`, `pin_limit?`, `property_limit?`, `include_properties?` | Independent node/connection slices, explicit nested truncation, registered/declared pin provenance, and package-dirty postconditions |
| `list_dataflow_node_types` | `filter?`, `common_only?`, `limit?`, `include_pins?`, `pin_limit?` | Deterministic category/type ordering with exact counts and bounded optional pin schemas |
| `get_dataflow_node_schema` | `type_name`, `include_properties?`, `pin_limit?`, `property_limit?` | One case-exact registered factory type; case-only substitution returns `node_type_case_mismatch` |
| `validate_dataflow_graph` | `asset_path`, `node_scan_limit?`, `connection_scan_limit?`, `issue_limit?` | Bounded structural validation; incomplete scans emit `validity_status=incomplete` and omit `valid` |
| `list_dataflow_variables` | `asset_path`, `limit?` | Bounded property-bag descriptors and scalar values with explicit value-read, omission, and truncation status |
| `list_dataflow_comments` | `asset_path`, `comment_limit?`, `node_limit?`, `graph_node_scan_limit?` | Bounded comment/membership rows; rejects comparison budgets above 1,000,000 |

Scalar params require exact JSON types; unknown keys, fractional integers, and
out-of-range limits fail with `-32602`. `package_path` is `/Game` or a
canonical directory below it. `asset_path` is an exact, case-sensitive
`/Game/.../Asset.Asset` object path and rejects shorthand names, file
extensions, backslashes, subobjects, redirects, and alternate resolved
objects. All numeric limits publish inclusive `minimum` and `maximum` fields in
their discoverable action schemas.

Every dynamic response also shares a fixed aggregate budget of 4,096 returned
rows across top-level and nested collections. Reflected/free-form strings
passed through the bounded reader additionally share 1,048,576 characters.
`output_returned_row_count`, `output_rows_truncated`,
`output_returned_bounded_text_character_count`,
`output_bounded_text_truncated`, and `output_budget_exhausted` distinguish
aggregate truncation from the individual action limits; fixed-size GUID and
contract metadata remain bounded by the row ceiling. Asset-backed reads
capture and verify package dirty state even when the loaded object is rejected
for case, redirect, or type mismatch.

Editable node/default properties and property-bag variables expose bounded
scalar values only. Dynamic containers, structs, and fixed arrays report an
explicit `value_read_status` such as `omitted_container` or `omitted_struct`
and omit `value`; the module never calls an unbounded generic export path for
those values.

See `Plugins/Monolith/Docs/specs/SPEC_MonolithDataflow.md`.

---

## Core action-schema helper

`FParamSchemaBuilder::Range(name, min, max)` adds inclusive machine-readable
`minimum` and `maximum` fields to an already-declared registered-action
parameter. It is a discovery contract only: action handlers must still perform
strict runtime type and range validation.

`FMonolithParamSchema::IsUniversalResponseShapingParam(name)` identifies the
five exact, case-sensitive parameters consumed after action dispatch:
`_fields`, `_omit`, `_compact_json`, `_row_fields`, and `_path_fields`.
Action-local strict readers call this helper so they preserve typo rejection
without rejecting the framework's universal response-shaping contract.
## gameplay_message
Read-only GameplayMessageRouter availability, exact channel/payload validation, and bounded static broadcaster/listener source tracing. **5 actions.** Implemented by `MonolithGameplayMessage` without a compile-time dependency on `GameplayMessageRuntime` or `GameplayMessageNodes`.
| `get_status` | — | Exact plugin, runtime/nodes module, subsystem class, async-listener class, listener-handle struct, and match-enum availability |
| `describe_listener_contract` | — | Reflected functions, match types, lifetime rules, and equal-or-derived broadcast payload compatibility with the listener's accepted parent struct |
| `validate_message_struct` | `message_struct`, `require_blueprint_type?`, `require_no_object_references?` | Exact non-redirecting `UScriptStruct` identity, structure size, metadata, and recursive object-reference checks |
| `validate_channel_contract` | `channel_tag`, `message_struct?`, `match_type?`, `require_registered_tag?`, `require_blueprint_type?` | Canonical tag syntax, exact registration/case, case-sensitive match type, and optional payload validation |
| `trace_channel_usage` | `channel_tag?`, `source_root?`, `source_roots?`, `include_monolith_source?`, `include_engine_gameplay_message_sources?`, `max_files?`, `max_results?`, `include_line_text?` | Deterministic path-sorted file selection; comment/token-boundary-safe per-call candidates; argument-local match inference; filtered ancestor `PartialMatch` evidence; physical path enforcement; truncation-safe orphan status; source excerpts are opt-in; no PIE, listeners, or broadcasts |

Scalar params require exact JSON types. Object paths reject whitespace, backslashes, extensions, subobjects, redirects, case changes, and resolved-path substitutions. Channel tags may be a valid single segment but reject leading, trailing, or internal empty dot segments. `max_files` is `1..5000`; `max_results` is `1..1000`; values are never clamped. Trace output separates sorted `files_selected` from files actually loaded in `files_scanned`, and reports total eligible enumeration in `limits.eligible_files_enumerated` under a 100,000-file safety bound. `summary.orphan_analysis_complete=false` means bounds or skipped input made absence claims indeterminate, so no orphan candidate is emitted.
| `trace_channel_usage` | `channel_tag?`, `source_root?`, `source_roots?`, `include_monolith_source?`, `include_engine_gameplay_message_sources?`, `max_files?`, `max_results?`, `include_line_text?` | Per-call bounded lexical candidates; nested project-plugin roots; physical path enforcement; truncation-safe orphan status; source excerpts are opt-in; no PIE, listeners, or broadcasts |
Scalar params require exact JSON types. Object paths reject whitespace, backslashes, extensions, subobjects, redirects, case changes, and resolved-path substitutions. Channel tags may be a valid single segment but reject leading, trailing, or internal empty dot segments. `max_files` is `1..5000`; `max_results` is `1..1000`; values are never clamped. `summary.orphan_analysis_complete=false` means bounds or skipped input made absence claims indeterminate, so no orphan candidate is emitted.
See `Plugins/Monolith/Docs/specs/SPEC_MonolithGameplayMessage.md`.
## interchange
Guarded asset import/export pipeline. Read-only actions validate sources and inspect import metadata; every filesystem or asset mutation requires `confirm=true` unless `dry_run=true`.
|--------|------------|-------|
| `get_supported_formats` | none | Lists known source formats, runtime module availability, allowed roots, and mutation policy. |
| `can_import` | `source_file`; optional `destination_path`, `allow_external` | Normalizes harmless folder whitespace/trailing separators, then validates source existence, extension support, a registered Interchange translator or legacy factory, link-safe root policy, and destination package syntax without mutation. |
| `can_reimport` | `asset_path` | Reports the result and source paths from `FReimportManager::CanReimport`. |
| `get_import_data` | `asset_path` | Returns the reflected source-file rows for an existing asset. |
| `import_asset` | `source_file`, `destination_path`, `conflict_policy`; optional `allow_external`, `confirm`, `dry_run` | Imports one source with an explicit `fail`, `overwrite`, or uniquely resolved `rename` policy for single-output formats. Scene/mesh sources require `fail` plus a dedicated destination proven empty by complete bounded inspection. |
| `import_assets` | `source_files`, `destination_path`, `conflict_policy`; optional `allow_external`, `confirm`, `dry_run` | Imports sources sequentially and returns one structured row per source. Dry-run reserves each successful single-output row's prospective package so later same-name rows predict the same result as confirmation. A multi-source batch rejects scene/mesh inputs because secondary output packages cannot be predicted safely. |
| `import_scene` | same as `import_asset` | Requires a scene-capable source, registered scene factory, `conflict_policy=fail`, and a dedicated empty destination. |
| `import_mesh` | same as `import_asset` | Configures and verifies a static-mesh import under the multi-output destination guard. |
| `import_skeletal_mesh` | same as `import_asset` | Configures FBX skeletal import explicitly and verifies a skeletal-mesh result under the multi-output destination guard. |
| `import_texture` | same as `import_asset` | Requires a texture source and verifies a texture result. |
| `import_audio` | same as `import_asset` | Requires a wave-audio source and verifies a sound-wave result. |
| `update_reimport_path` | `asset_path`, `source_file`; optional `source_file_index`, `allow_external`, `confirm`, `dry_run` | Requires a registered handler, requires an exact integer source index when present, updates the path, and verifies handler readback. |
| `reimport_asset` | `asset_path`; optional `source_file`, `source_file_index`, `allow_external`, `confirm`, `dry_run` | Validates every retained handler source and an exact integer replacement index. A provided replacement source must be an exact non-empty string and must match the target asset kind plus a registered backend before Unreal's reimport manager is called. |
| `reimport_assets` | `asset_paths`; optional `allow_external`, `confirm`, `dry_run` | Reimports assets sequentially after validating each handler-reported source path. |
| `export_asset` | `asset_path`, `file_path`; optional `replace_existing`, `allow_external`, `confirm`, `dry_run` | Resolves at most 256 native exporter-declared outputs in the requested directory, validates every collision/root, exports into isolated staging, rejects missing, linked, or undeclared staged results, and promotes the complete set with rollback backups. Promotion and restoration moves use the engine's no-retry/no-error-dialog mode. Script exporters are rejected because their output set cannot be bounded. |
`export_asset` returns a stable transaction shape containing `output_file_count`, per-file `output_files`, `mutation_attempted`, `exporter_succeeded`, `commit_succeeded`, `rollback_complete`, `partial_mutation`, `staging_cleanup_complete`, promotion/restoration counts, and `retained_paths`, including explicit not-attempted defaults during preflight errors and successful dry runs. A failed export or fully rolled-back commit returns `status="error"` without changing final outputs. An incomplete restore returns `status="partial_export"`, preserves staging and backups, and lists only recovery files that actually exist in `retained_paths`; it never advertises an absent destination. Every expected staged path is checked for symlink or junction traversal after the exporter returns and before promotion. Rollback force-removes a promoted read-only file before restoring its backup. Post-export staging inspection visits at most 256 immediate entries and never descends into exporter-created directories. Cleanup deletes only a completely scanned flat staging directory; an over-budget result or any directory is preserved and reported as incomplete instead of being recursively traversed, and cleanup failures list the staging directory only while it actually remains on disk.
Typed mesh, skeletal-mesh, texture, and audio imports snapshot the destination before mutation. If Unreal returns no object of the requested class, Monolith attempts to remove every newly returned asset. A complete cleanup returns `status="error"`, `rollback_complete=true`, and `partial_mutation=false`; retained pre-existing, unmanaged, or undeletable results return `status="partial_import"` and `partial_mutation=true`. The row includes `rollback_candidates`, `rolled_back_assets`, `pre_existing_import_results`, `unmanaged_import_results`, rollback counts, and `dirty_packages_before_rollback` so callers can decide whether a retry is safe.

---

## bulk_fill

Reflection-walker bulk property fill across 12 per-namespace adapters. **2 actions.** Framework dispatcher in `MonolithCore` (0.15.0); each adapter self-registers from its owning module — zero compile-time linkage from core into adapter modules.

### `bulk_fill_query.apply`

Apply a JSON-tree fill to an asset via the target namespace's adapter. Walks the target's reflection schema, supports preview-without-persist and strict promotion of silent drops.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `target_namespace` | string | **required** | Adapter namespace: `blueprint`, `gas`, `ui`, `ai`, `niagara`, `material`, `audio`, `mesh`, `animation`, `logicdriver`, `combograph` (plus the sibling `inventory` adapter when present) |
| `target` | string | **required** | Asset path or adapter-defined target (e.g. `/Game/Items/DA_HealingPotion`) |
| `tree` | object | **required** | Nested JSON of properties to walk against the target's reflection schema |
| `dry_run` | boolean | optional | Validate only — emit would-be writes but do not persist. Default: `false` |
| `strict` | boolean | optional | Promote silent drops / clamps / unknown-fields to hard errors. Default: `false` |

Returns an `FDryRunReport`-shaped result (`FieldWrites` / `SilentDrops` / `Clamps` / `Errors`).

### `bulk_fill_query.list_namespaces`

List `target_namespace` values the bulk_fill registry currently knows about (one row per registered adapter). *No parameters.*

---

## describe

Read-only schema introspection for the same 12 adapters, plus action-param introspection. **3 actions.** Companion to `bulk_fill` (0.15.0).

### `describe_query.schema`

Return a rich `FSchemaDescriptor` tree (type names, ImportText forms, enum-value lists, clamp ranges, nested children) for an asset/action via its namespace adapter.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `target_namespace` | string | **required** | Adapter namespace whose schema should be introspected |
| `target` | string | **required** | Asset path or action name to describe |

### `describe_query.list_targets`

List the asset paths / action names the describe adapter can introspect for a given `target_namespace`.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `target_namespace` | string | **required** | Adapter namespace whose introspection inventory should be listed |

### `describe_query.action_schema`

Return a registered ACTION's param schema (names, types, required, defaults, aliases, descriptions) by `(target_namespace, action)` — so callers stop trial-and-erroring param names.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `target_namespace` | string | **required** | Namespace that owns the action (e.g. `blueprint`, `ui`) |
| `action` | string | **required** | Action name whose param schema to return (e.g. `add_nodes_bulk`) |

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

<a id="sibling-plugins"></a>

## Sibling Plugins

Sibling plugins live **beside** Monolith (not inside it) and register their own namespaces into Monolith's MCP action registry at startup. They ship as **separate plugins with separate releases** — they are not bundled in the Monolith zip.

If you're building a sibling plugin yourself, read `Plugins/Monolith/Docs/SIBLING_PLUGIN_GUIDE.md` for the architectural pattern, build setup, and reflection requirements.

| Sibling plugin | Namespace | Actions | Status | Repo |
|---|---|---|---|---|
| External sibling plugin | Custom | Varies | Registers its own namespace at startup and ships through its own repo/channel. | Outside `Plugins/Monolith/` |

**Why these aren't in the in-tree count:** the in-tree count (the approximate `~1,400+ / 26+` figure) counts only modules shipped inside the public `Monolith-vX.Y.Z.zip` release. Sibling plugins live in their own folders, ship via their own channels (or stay private), and may or may not be installed in any given consumer's project. Their absence is not a degraded state — Monolith is fully functional without them.

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

### Town Generation (experimental)

```
generate_floor_plan → create_building_from_grid → generate_facade → generate_roof → furnish_room → validate_building
```

> **Experimental.** `bEnableProceduralTownGen=true` required. Known geometry issues remain.

---

## Discovery Pattern (use this first)

Before writing any client code:

1. `monolith_discover()` — list all namespaces and their actions.
2. `monolith_discover("<namespace>")` — list one namespace's action names + one-line descriptions (terse). Pass `detail=true` to inline all param schemas.
3. `describe_query("action_schema", target_namespace="<ns>", target_action="<name>")` — get one action's full param schema.
4. `project_query("search", {query: "..."})` — find assets by name/type.
5. `source_query("search_source", {query: "..."})` — verify UE 5.7 API signatures.

**Golden rule:** never fabricate action names. The cogitator will be displeased.

---

## Offline Fallback (editor not running)

When the editor is closed but you still need to query Monolith:

- **`Plugins/Monolith/Binaries/monolith_query.exe`** — standalone C++ tool, read-only. The canonical offline path. Serves the read-only `project` / `source` / `config` namespaces plus the full **20-action Reflection Intelligence surface**.
- **`python Plugins/Monolith/Scripts/monolith_offline.py`** — stdlib-only dev fallback, kept byte-for-byte in lockstep with the exe.

Both invoke the same SQLite indexes the live MCP uses.

**Reflection Intelligence offline parity.** All four RI namespaces are now fully servable offline — `cppreflect` (6 actions), `network` (4), `decision` (5), `risk` (5) — and emit JSON **byte-identical to the live MCP server** (same field names, types, ordering, row data, `%.17g` float formatting, and base64 cursor tokens). Earlier builds covered only 4 of the 20 with divergent shapes; the phantom `risk.list_hotspots` action has been removed. Two intentional, documented differences from the live payload remain (not bugs): the offline CLI adds a top-level `success` flag (its in-band status channel — the live MCP carries success/error out-of-band, so live has no `success` key; the nested DATA payload is byte-identical), and wall-clock fields (`cutoff_unix` / `since_unix` and the `risk.get_release_window_hotspots` cursor whose filter-hash includes them) differ by the run-time gap across process invocations on both live and offline.

`Scripts/verify_offline_parity.py` byte-diffs exe vs py across all 20 RI actions as a ship-blocking gate in `make_release.ps1`; `Scripts/check_offline_exe_fresh.py` flags a stale exe by comparing its `--version` `source_hash` against a fresh hash of `monolith_query.cpp`, and is likewise a ship-blocking gate in `make_release.ps1`. It hashes `monolith_query.cpp` only — a failure originating in `ThirdParty/sqlite3.c`, a ThirdParty header, or the toolchain is covered by `build.bat`'s exit code instead.

---

## Transport Notes

- Claude Code's MCP transport is `"http"`, not `"streamableHttp"`.
- Some clients serialize nested `params` objects to a JSON **string** instead of a nested object — detect and deserialize back.
- The HTTP server lives on `http://localhost:<port>`. Port is published in `monolith_status` output.
- For Claude Code specifically, the **MCP auto-reconnect proxy** at `Scripts/monolith_proxy.py` survives editor restarts. See `Plugins/Monolith/Docs/Installation.md` for setup.

---

## Conditional Module Gating Reference

| Module | Gate | Actions when ungated |
|--------|------|----------------------|
| MonolithGAS | `bEnableGAS` / optional `WITH_GBA` | `bEnableGAS=false` removes `gas` but keeps all 10 `input` actions; `WITH_GBA=0` removes only optional Blueprint Attributes behavior |
| MonolithGAS | `WITH_GBA` (GameplayAbilities plugin) | 0 |
| MonolithGameFeatures | `WITH_MONOLITH_GAMEFEATURES` plus `bEnableGameFeatureActions` | 1 status-only without the dependency; 9 compiled default; 15 with inspection enabled |
| MonolithComboGraph | `WITH_COMBOGRAPH` (ComboGraph marketplace plugin) | 0 |
| MonolithLogicDriver | `WITH_LOGICDRIVER` (Logic Driver Pro marketplace plugin) | 0 |
| MonolithAI | `WITH_STATETREE` + `WITH_SMARTOBJECTS` (engine plugins) | 0 |
| MonolithUI CommonUI | `WITH_COMMONUI` | 42 (UMG baseline only) |
| MonolithAudio MetaSound | `WITH_METASOUND` | Sound Cue + CRUD + batch (no MetaSound graph) |
| MonolithMesh town gen | `bEnableProceduralTownGen` (Editor Preferences, default `false`) | 195 (core mesh only) |
| MonolithAsset | `bEnableAsset` (Editor Preferences, default `true`) | 0 |

---

## Cross-References

- **SPEC_CORE.md** — Master Monolith spec, action count audit, pipelines, architecture
- **SPEC_Monolith*.md** — Per-module deep specs in `Plugins/Monolith/Docs/specs/`
- **SIBLING_PLUGIN_GUIDE.md** — How to build a sibling plugin against `FMonolithToolRegistry`
- **CHANGELOG.md** — Release-by-release change history (Keep a Changelog format)
- **Wiki** — User-facing tutorials at `https://github.com/tumourlove/monolith/wiki`

---

*The Omnissiah's blessing be upon every action call. May your discovery never return an empty array.*
