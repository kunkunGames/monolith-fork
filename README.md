# Monolith

**One plugin. Every Unreal domain. Zero dependencies.**

[![UE 5.7 / 5.8](https://img.shields.io/badge/Unreal-5.7%20%2F%205.8-blue)](https://unrealengine.com)
[![License: MIT](https://img.shields.io/badge/License-MIT-green.svg)](LICENSE)
[![MCP](https://img.shields.io/badge/Protocol-MCP-purple)](https://modelcontextprotocol.io)

---

## Why I built this

Most MCP integrations for Unreal register every action as a separate tool. That floods the AI's context window with hundreds of tool names before you've asked a single question — and the actually useful stuff gets buried. I built Monolith because I wanted my AI to spend its context on my problem, not on memorising a tool catalogue.

One plugin. One MCP endpoint. A handful of namespace-dispatch tools instead of ~2102. The AI calls `monolith_discover()` and `monolith_guide()` when it needs to know what's available, and otherwise just hits `blueprint_query("create_asset", ...)`, `material_query("compile", ...)`, and so on.

> **Platform:** Windows, macOS, Linux.

## Why Monolith?

Most MCP integrations register every action as a separate tool, which floods the AI's context window and buries the actually useful stuff. Monolith uses a **namespace dispatch pattern** instead: each domain exposes a single `{namespace}_query(action, params)` tool, and a central `monolith_discover()` call lists everything available. A small set of dispatch tools fronts a large capability surface — **~2102 in-tree actions across 77 namespaces** in this release (run `monolith_discover()` for the authoritative live count; sibling plugins can add more when loaded). The AI gets oriented fast and spends its context on your actual problem.

## What Can It Actually Do?

**Blueprint (121 actions)** — Full programmatic control of every Blueprint in your project. Create from any parent class, build entire node graphs from a JSON spec, add/remove/connect/disconnect nodes in bulk, manage variables, components, functions, macros, and event dispatchers. Implement interfaces, reparent hierarchies, edit construction scripts, read/write CDO properties on any Blueprint or DataAsset. The auto-layout engine uses a modified Sugiyama algorithm so AI-generated graphs actually look clean. Compare two Blueprints side-by-side, scaffold from templates, manage user defined structs and enums. New in 0.15.0: a **dataset read/edit pack** for round-tripping DataTables, CurveTables, and StringTables — read rows with the row struct schema inline, bulk upsert with dry-run previews, row CRUD, JSON/CSV import/export, plus `seed_data_asset` to create and populate a DataAsset in one atomic call. Also new: `add_property_access` (get/set a UPROPERTY on any foreign class by name, resolved to its real type), `override_parent_function` (override an overridable parent function that returns a value), and `save_dirty_assets` to flush a batch of edits. Hand the AI a description and it builds the whole thing — or point it at an existing Blueprint and it'll surgically rewire what you need.

**Material (63 actions)** — Create materials, material instances, and material functions from scratch. Build entire PBR graphs programmatically — add expressions, connect pins, auto-layout, recompile. Drop in custom HLSL nodes. Import textures from disk and wire them directly into material slots. Batch-set properties across dozens of instances at once. Render material previews and thumbnails without leaving the AI session. Preview textures with full metadata, check tiling quality with anti-tiling analysis, batch delete expressions, clear entire graphs. Full material function support: create, build internal graphs, export/import between projects. Get compilation stats, validate for errors, inspect shader complexity. Covers the full material workflow from creation to validation.

**Animation (125 actions)** — The entire animation pipeline, end to end. Create and edit sequences with bone tracks, curves, notifies, and sync markers. Build montages with sections, slots, blending config, and anim segments. Set up 1D/2D Blend Spaces and Aim Offsets with sample points. **Animation Blueprint graph writing** — add states to state machines, create transitions, set transition rules, add and connect anim graph nodes, set state animations. As of 0.15.0, `add_anim_graph_node` resolves arbitrary concrete custom AnimGraph node classes by path or name (not just the built-in node-type aliases), so AI can wire up nodes from any plugin's `UAnimGraphNode_Base` subclass. AI can build ABP locomotion setups programmatically, not just read them. PoseSearch integration: create schemas and databases, configure channels, rebuild the search index. Control Rig graph manipulation with node wiring and variable management. Physics Asset editing for body and constraint properties. IK Rig and Retargeter support — chain mapping, solver configuration, the works. Skeleton management with sockets, virtual bones, and curves. 125 actions covering the full animation pipeline.

**Niagara (109 actions)** — Full system and emitter lifecycle — create, duplicate, configure, compile, save. Module CRUD with override-preserving reorder so you don't blow away artist tweaks. Complete dynamic input lifecycle: attach inputs, inspect the tree, read values, remove them. Event handler and simulation stage CRUD. Niagara Parameter Collections with full param management. Effect Type creation with scalability and culling configuration. Per-quality-level scalability settings. Renderer helpers for every type — mesh assignment, ribbon presets (trail, beam, lightning, tube), SubUV and flipbook setup. Data interface configuration and property inspection handles JSON arrays and structs natively. Diff two systems to see exactly what changed. Clone overrides between modules, duplicate modules, discover parameter bindings, inspect module outputs, rename user parameters. Batch execute with read-only optimization so queries don't trigger unnecessary recompiles. Full `export_system_spec` and `import_system_spec` with merge mode. As of 0.15.0, `get_system_summary` / `get_emitter_summary` take a `detail_level` for richer event-aware payloads, and `validate_system` reasons about inter-emitter event chains — thanks to @middle233's PR #60. Covers the full Niagara workflow from system creation to final polish.

**UI (164 actions)** — Widget Blueprint CRUD with full widget tree manipulation. UMG baseline plus explicit session/request UI work context, Animation v2, animation read inspection and guarded animation delta edits, EffectSurface, Spec Builder, Type Registry, hoisted Design Import, visual artifact evidence, markup-to-UISpec conversion, UISpec diff/patch workflows, authored layout measurement, dynamic-material lifecycle lint, conditional CommonUI actions (`WITH_COMMONUI`), plus 4 GAS attribute-binding aliases. 0.15.0 closed a big chunk of the MCP gap here: close-the-loop primitives (`rename_widget`, `add_widget_variable`, `audit_focus_chain`, `list_widget_property_enums`, `describe_widget_type_schema`, `convert_textblock_to_common`, `dump_blueprint_compile_log` and friends), headline scaffolders (`scaffold_main_menu`, `scaffold_settings_panel_with_tabs`, `scaffold_pause_menu`, `build_menu_from_spec`), and gap-closure actions like `set_widget_navigation_bulk`, `dump_widget_navigation`, `convert_border_to_common`, `reparent_widget_root`, and `set_widget_is_variable`. Pre-built templates for common game UI: HUD elements, menus, settings panels, confirmation dialogs, loading screens, inventory grids, save slot lists, notification toasts. Style everything — brushes, fonts, color schemes, batch style operations, and RetainerBox effect-material binding with exact texture-parameter proof. Create keyframed widget animations, inspect timelines/time-slices, and apply confirm-gated scalar float-key deltas without adding duplicate external Sequencer action names. Full game scaffolding: settings systems, save/load, audio config, input remapping, accessibility features. Run accessibility audits, verify widget PNG artifacts, audit dynamic-material lifetime, measure layout overlap/safe-zone risks, set up colorblind modes, configure text scaling. Covers the full UI workflow from design-data import to accessibility.

**Editor (60 actions)** — Trigger full UBT builds or Live Coding compiles, read build errors and compiler output, search and tail editor logs, get crash context after failures. Capture preview screenshots of materials, Niagara systems, static meshes, skeletal meshes, widgets, material grids, and debug overlays. Inspect material PBR slots and texture channels without rendering. Capture ordered PNG frame sequences, encode already-captured PNG frame sequences, and produce adaptive 60 fps GIFs for temporal review, create blank maps, query module status, list/run UE automation tests, inspect editor selections and asset context, plus a Python escape-hatch and persistent-level swap for automation flows. The AI can compile your code, read the errors, fix the C++, recompile, and verify the fix — all without you touching the editor.

**Config (11 actions)** — Full INI resolution chain awareness: Base, Platform, Project, User. Ask what any setting does, where it's overridden, what the effective value is, and how it differs from the engine default. Search across all config files at once. Perfect for performance tuning sessions where you want the AI to just sort out your INIs.

**Source (11 actions)** — Search over 1M+ Unreal Engine C++ symbols instantly. Read function implementations, get full class hierarchies, trace call graphs (callers and callees), verify include paths — all against a local index, fully offline. The native C++ indexer runs automatically on editor startup. No Python, no setup. Optionally index your project's own C++ source for the same coverage on your code. The AI never has to guess at a function signature again.

**Project (7 actions)** — SQLite FTS5 full-text search across every indexed asset in your project. Find assets by name, type, path, or content. Trace references between assets. Search gameplay tags. Get detailed asset metadata. The index updates live as assets change and covers marketplace/Fab plugin content too — 15 deep indexers registered including DataAsset subclasses.

**Mesh (194 actions)** — The biggest module by far. 194 core actions across 22 capability tiers, plus 45 procedural town generation actions that are disabled by default (work-in-progress, and unless you're willing to dig in and help improve it, best left alone for now — they're not in the advertised public count). Mesh inspection and comparison. Full actor CRUD with scene manipulation. Physics-based spatial queries (raycasts, sweeps, overlaps) that work in-editor without PIE. Level blockout workflow with auto-matching and atomic replacement. GeometryScript mesh operations (boolean, simplify, remesh, LOD gen, UV projection). Horror spatial analysis — sightlines, hiding spots, ambush points, zone tension, pacing curves (WIP). Accessibility validation with A-F grading. Lighting analysis (WIP), audio/acoustics with Sabine RT60 and stealth maps (WIP), performance budgeting (WIP). Decal placement with storytelling presets. Level design tools for lights, volumes, sublevels, prefabs, HISM instancing. Tech art pipeline for mesh import (now with optional skeletal-mesh + animation import via `import_mesh`, thanks to @4698to's PR #58), LOD gen, texel density, collision authoring. Context-aware prop scatter on any surface. Procedural geometry — parametric furniture (15 types), horror props (7 types), architectural structures, mazes, pipes, terrain. Genre preset system for any game type. Encounter design with patrol routes, safe room evaluation, and scare sequence generation. Full accessibility reporting.

**GAS (135 actions)** — Complete Gameplay Ability System integration. 131 GAS-namespace actions plus 4 widget attribute-binding actions also aliased into the `ui` namespace. Create and manage Gameplay Abilities with activation policies, cooldowns, costs, and tags. Full AttributeSet CRUD — both C++ and Blueprint-based (via optional GBA plugin). Ships with `ULeviathanVitalsSet` AttributeSet template (Phase J F4) so projects without GBA still get a working starter set. Gameplay Effect authoring with modifiers, duration policies, stacking, period, and conditional application. Ability System Component (ASC) management — grant/revoke abilities, apply/remove effects, query active abilities and effects. Gameplay Tag utilities. Gameplay Cue management — create, trigger, inspect cues for audio/visual feedback. Target data generation and targeting tasks. Input binding for ability activation. Runtime inspection and debugging tools. Scaffolding actions that generate complete GAS setups from templates. Accessibility-focused infinite-duration GEs for reduced difficulty modes.

**AI (243 actions)** — The most comprehensive AI tooling available through any MCP server. Full lifecycle management for Behavior Trees, Blackboards, State Trees, Environment Query System (EQS), Smart Objects, AI Controllers, AI Perception, Navigation, and runtime debugging. Crown jewel actions: `build_behavior_tree_from_spec` and `build_state_tree_from_spec` — hand the AI a JSON description of your desired AI behavior and it builds the entire asset programmatically. Phase J shipped BT crash hardening (F1) and BT graph + perception inspection helpers (F8). Create BT nodes (tasks, decorators, services), wire them into trees, configure blackboard keys, set up EQS queries with generators and tests, define Smart Object slots with behavior configs, configure perception senses (sight, hearing, damage, touch), manage navigation filters and query filters, inspect and debug AI at runtime during PIE. Scaffolding actions generate complete AI setups from templates — patrol AI, combat AI, companion AI, and more. 243 actions across 15 categories. Conditional on State Tree and Smart Objects plugins (both ship with UE) — gated via `WITH_STATETREE` and `WITH_SMARTOBJECTS` (Phase J F22 retrofit). Optional Mass Entity and Zone Graph integration for large-scale AI.

**Logic Driver (66 actions)** — Full integration with Logic Driver Pro, a marketplace state machine plugin. State machine CRUD — create, inspect, compile, delete. Graph read/write — add states, transitions, configure properties, set transition rules. Node configuration for state nodes, conduit nodes, and transition events. Runtime/PIE control — start, stop, query active states, trigger transitions. One-shot `build_sm_from_spec` builds complete state machines from a JSON specification. JSON spec import/export for templating and version control. Scaffolding actions generate common patterns (door controller, health system, AI patrol, dialogue system, elevator, puzzle, inventory). Component management — add/configure Logic Driver components on actors. Text graph visualization for debugging. Discovery actions list available node classes and templates. Reflection-only integration (no direct C++ API linkage) — works with any Logic Driver Pro version. Conditional on `#if WITH_LOGICDRIVER` — auto-detected at build time.

**ComboGraph (13 actions)** — Integration with the ComboGraph marketplace plugin for visual combo tree editing. Graph CRUD — create, inspect, validate combo graphs. Node and edge management — add combo nodes with montage animations, wire them with edges, configure effects and cues. GAS cross-integration — scaffold combo abilities that bridge ComboGraph with Gameplay Ability System. Reflection-only integration, conditional on `#if WITH_COMBOGRAPH`.

**Audio (98 actions)** — Editor-time audio asset authoring across the full UE audio pipeline. 82 baseline audio-namespace actions plus 4 perception-binding actions (`bind_sound_to_perception` and friends, Phase J integration) plus 12 v0.14.10 MetaSound document introspection actions (PR #18 by @alakangas — read-side complement to the existing Builder API). Full CRUD on the 5 configurable audio asset types — SoundAttenuation, SoundClass, SoundMix, SoundConcurrency, SoundSubmix. Sound Cue graph construction — add nodes (22 types), wire them, set properties via reflection. MetaSound Builder API integration for programmatic MetaSound authoring — nodes, pins, graph inputs/outputs, interfaces, variables. MetaSound document introspection (v0.14.10) for read-only inspection of any on-disk MetaSound asset — list, document walk, summary, node instance details, connections, variables, user parameters, search, info, dependencies, validation. Crown jewels: `build_sound_cue_from_spec` and `build_metasound_from_spec` — declarative JSON-to-graph in a single call. Batch operations for class/attenuation/submix/concurrency/compression/looping/virtualization across dozens of assets at once. Audio health checks — find unused sounds, missing attenuation, unassigned classes. Built-in `create_test_wave` (Phase J F18) generates a sine SoundWave on demand for diagnostic work. Phase J F11 added a hardened audio asset validator. Five template Sound Cues (random, layered, looping ambient, distance crossfade, switch) and four template MetaSounds (oneshot SFX, looping ambient, synth tone, interactive). SoundWave inspection is read-only; reflection-based property edits still work for batch sound wave tuning. MetaSound features gated on `#if WITH_METASOUND` — graceful degradation when absent.

---

## What it does

Monolith exposes **~2102 actions across 77 in-tree namespaces** through a namespace-dispatch pattern: each domain registers a single `{namespace}_query(action, params)` tool, and a central `monolith_discover()` lists everything available. (Exact counts are intentionally approximate — query `monolith_discover()` for the live figure.)

Covered domains: Blueprints, Materials, Animation, Niagara, Mesh, UI (incl. CommonUI), AI (Behavior Trees, State Trees, EQS, Smart Objects, Perception, Navigation), Gameplay Ability System, Logic Driver state machines, ComboGraph combo trees, Audio (Sound Cues + MetaSounds), Editor control (UBT builds, log capture, scene capture, asset preview & inspection), Engine source search (1M+ symbols, fully offline), Project asset search (SQLite FTS5), INI config, Level Sequences, a `bulk_fill` / `describe` reflection framework for deep property writes, a `monolith_guide` self-onboarding tool for your AI, plus the new v0.17.0 **Reflection Intelligence** layer: `decision` (architectural decision-record harvest), `risk` (repo-level hotspot + co-change + conditional-gate signals), `cppreflect` (UE 5.7 UHT reflection-edge queries cross-joined with the asset registry), `network` (replication inspection — replicated classes, RPCs, OnRep handlers, unbalanced-handler audits), `pipeline` (read-only composer actions for PR review + release pre-flight), and `reflect` (index maintenance — a project-only force-rebuild of the reflection tables). The `cppreflect` and `network` indexers scan your project plugins by default, so replicated classes and RPCs declared in plugins are in scope without extra setup; enabled marketplace plugins are gated behind a setting, and Epic engine built-ins stay excluded.

**MCP LLM Ergonomics** (also new in v0.17.0): universal response shaping (`_fields` / `_omit` / `_compact_json`) on every action, schema-tagged param kinds with automatic `\` → `/` rewrite on asset paths, `did_you_mean` fuzzy match on dispatch errors, MCP `tools/list` annotations (read-only / destructive / idempotent hints), `source_query` cursor pagination, and a proxy-side JSONL call log. The whole point is to let your AI spend less context recovering from typos and trial-and-error.

**New in v0.19.0:** an **LLM C++ authoring ergonomics pack** in the `source` namespace — eight read-only lookups so your AI resolves an include path, exact signature, deprecation status, Build.cs deps, header lint, or a UCLASS stub in one round-trip instead of reading raw source (`get_include_path`, `get_signature`, `check_deprecations`, `verify_symbols`, `find_example_usage`, `suggest_build_cs_deps`, `lint_header`, `generate_class_stub`), plus `fix_hints` on `editor.get_build_errors`. A parser fix that finally indexes allman-brace plain classes/structs **tripled the engine source index** (~300K → ~967K symbols), so lookups for `FCollisionShape`, `FScopeLock`, `FPaths` and ~40K other engine types now actually return. Plus **live-PIE introspection + driving** in the `editor` namespace (`pie_get_object_properties`, `pie_call_function`, `pie_set_control_rotation`, `pie_inject_input_action`, `pie_possess_spectator_free`), programmatic stat-group readout (`get_stat_group_values`), time-series PIE sampling and anim-node binding read/write (`animation`), a Blueprint variable-reference census (`blueprint.find_variable_references`) and contract reconciliation, and first-class T3D asset-text export (`project.export_asset_text`). The `tools/list` manifest is ~40% smaller (duplicated action lists dropped from dispatcher descriptions), and two first-launch fixes land (MonolithMesh now delay-loads GeometryScripting; the deep indexer no longer asserts on UserDefinedStruct fields with unresolved types — issue #70, thanks @aggitti).

**Unreleased:** an **AnimGraph-authoring pack** in the `animation` namespace — apply-additive / mesh-space-additive nodes, slot nodes (validated against the skeleton's slot groups), save/use cached pose, output-pose and state-result wiring, blend-by-int, sync groups, layered-blend-per-bone filters, Control Rig anim-graph nodes, linked anim layers, and state-machine conduits — plus blend-space baking + interpolation control, state-machine teardown (remove states / transitions / re-point entry), IK-solver removal, and a Blueprint-Assist-free `auto_layout` formatter that works in release builds.

**New in v0.18.1:** a from-scratch **Motion Matching authoring pack** across the `animation`, `chooser`, and `blueprint` namespaces — Pose Search schema / database primitives, mirror data tables, chooser-table authoring, the AnimBP motion-matching graph + foot-IK, thread-safe AnimBP authoring (reflective Property Access, a thread-safe function flag, and an exec-driven chooser feeding the Motion Matching database), character/actor scaffolding, and a retarget create/run pack. Plus a **PIE / profiling harness** (async PIE-smoke sessions, CSV / Insights profiling brackets, clip + anim-frame capture, map authoring, nav rebuild/validate), **state-machine authoring + live anim-instance telemetry**, a generic **AI controller that runs a BehaviorTree on possess** with movement-driving BT task classes, inherited-native-component inspection, and live DataAsset field read-back.

**New in v0.18.0:** Niagara HLSL direct-editing — read and overwrite the HLSL source on a `CustomHlsl` node (`get_custom_hlsl_text` / `set_custom_hlsl_text`), plus simulation-stage / event-handler selectors on the module-stack actions and a ParameterMap bridge for `create_module_from_hlsl` (PR #65, thanks @middle233). Niagara also gains a search & discovery pack (`search_by_parameter`, `search_by_data_interface`, `query_niagara`, `find_similar_systems`, `search_by_material`, `find_niagara_references`, `list_system_data_interfaces`).

Full per-namespace breakdown: **[Tool Reference (wiki)](https://github.com/tumourlove/monolith/wiki/Tool-Reference)**.

Works with **Claude Code**, **Cursor**, **Cline**, or any MCP-compatible client. Windows, macOS, Linux.

Local developer checkouts can report additional namespaces when sibling/private plugins are loaded. Those actions ship from their owning plugins and are not part of the public Monolith release count.

---

## Quick install

**1. Drop into Plugins/**

```bash
cd YourProject/Plugins
git clone https://github.com/tumourlove/monolith.git Monolith
```

(Or grab the [latest release zip](https://github.com/tumourlove/monolith/releases) and extract to the same path. The release zip includes precompiled DLLs so Blueprint-only projects can open the editor immediately without rebuilding. Monolith builds on **UE 5.7 and 5.8** from a single source tree — but the precompiled DLLs are engine-locked, so Blueprint-only users grab the zip for their engine, `Monolith-vX.Y.Z-UE5.7.zip` or `-UE5.8.zip`. Building from source works on either.)

**2. Configure MCP.** On Windows, the recommended Codex/Claude setup is the
validated native proxy in user-level MCP configuration:

```powershell
powershell -ExecutionPolicy Bypass -File Plugins\Monolith\Scripts\onboard_monolith.ps1 `
  -Targets Codex,Claude -Execute -ReplaceMcpConfig
```

The onboarding script validates `Binaries/monolith_proxy.current.json`, the
source-addressed proxy leaf, its full SHA-256, and its `--version` identity
before recording the exact absolute path. Do not type a source hash manually.
Clients that explicitly require project scope can add `-ProjectMcpConfig`; keep
machine-local absolute paths out of tracked project files. The native proxy has
no Unreal Engine or game DLL dependency. Healthy calls go to the live editor,
while a fixed read-only surface remains available through the immutable
Query/catalog generation selected by `monolith_query.current.json` during
editor transport outages. For **Cursor/Cline**, **macOS/Linux**, or the
**Python fallback**, see the [Installation wiki page](https://github.com/tumourlove/monolith/wiki/Installation).

**3. Connect, then open the editor for editor-backed work.** The proxy can initialize and expose its read-only fallback before UE starts. Open the editor for asset/world mutation and live runtime actions; wait 30-60 seconds for the first-launch index. When you see `Monolith MCP server listening on port 9316` in the Output Log (filter `LogMonolith`), `monolith_status()` should switch from the offline catalog status to the live editor status without restarting the AI client.

macOS / Linux:
```json
{
  "mcpServers": {
    "monolith": {
      "command": "Plugins/Monolith/Scripts/monolith_proxy.sh",
      "args": []
    }
  }
}
```

> **No proxy?** Use direct HTTP instead — you'll just need to restart Claude Code each time the editor restarts:
> ```json
> {"mcpServers": {"monolith": {"type": "http", "url": "http://localhost:9316/mcp"}}}
> ```

**For Cursor / Cline:**

```json
{
  "mcpServers": {
    "monolith": {
      "type": "streamableHttp",
      "url": "http://localhost:9316/mcp"
    }
  }
}
```

> Cursor and Cline handle server restarts natively — the proxy isn't needed.

### Step 3: Open the editor

Open your `.uproject` as normal. On first launch:

1. Monolith auto-indexes your project (30-60 seconds depending on size — go get a coffee)
2. Open the **Output Log** (Window > Developer Tools > Output Log)
3. Filter for `LogMonolith` — you'll see the server start up and the index complete

When you see `Monolith MCP server listening on port 9316`, you're in business.

### Step 4: Connect your AI

1. Open **Claude Code** (or your MCP client) from your project directory.
2. The client starts Monolith from its configured user-level MCP entry, or from an explicitly opted-in project `.mcp.json`.
3. Sanity check: ask *"What Monolith tools do you have?"*

You should get back a list of namespace tools (`blueprint_query`, `material_query`, etc.). If you do, everything's working.

### Step 5: Add project instructions for your AI

Different AI coding assistants use different conventions for project-instructions files (`CLAUDE.md` for Claude Code, `AGENTS.md` for Codex, `.cursorrules` for Cursor, `.github/copilot-instructions.md` for Copilot, plus a long tail). Those conventions evolve faster than a static template can keep up — so rather than ship a template that grows stale, the recommended workflow is to ask your AI directly.

Practical prompt to feed your assistant once Monolith is installed and running:

> *"I've installed the Monolith Unreal plugin. It exposes ~2102 actions across 77 namespaces (`blueprint`, `material`, `animation`, `niagara`, `mesh`, `ui`, `gas`, `ai`, `audio`, `console`, `chaos_fracture`, etc.) over an in-process MCP HTTP listener at `http://localhost:9316/mcp`. What's the best-practice format for a project-instructions file for [your assistant — e.g. `CLAUDE.md`, `AGENTS.md`, `.cursorrules`]? It should help with action discovery via `monolith_discover()` and `monolith_guide()`, asset-path conventions like `/Game/Path/Asset`, and verifying UE 5.7 APIs via `source_query` before writing code."*

Whatever your AI generates, drop it at the appropriate path for your toolchain. The action counts and workflow notes from this README's earlier sections are usable input.

### Step 6: (Optional) Index your project's C++ source

Engine source indexing is automatic — `source_query` works immediately with no setup.

If you also want your AI to search your **own project's C++ source** (find callers, callees, and class hierarchies across your own code):

1. Install **Python 3.10+**
2. Run `python Plugins/Monolith/Scripts/index_project.py` from your project root
3. Your project source gets indexed into `EngineSource.db` alongside engine symbols
4. To re-run the indexer without leaving the editor: `source_query("trigger_project_reindex")`

### Verify it's alive

With the editor running, hit this from any terminal:

```bash
curl -X POST http://localhost:9316/mcp \
  -H "Content-Type: application/json" \
  -d '{"jsonrpc":"2.0","id":1,"method":"tools/list"}'
```

You'll get a JSON response listing all Monolith tools. If you get "connection refused", the editor isn't running or something went sideways — check the Output Log for `LogMonolith` errors.

### (Optional) Install Claude Code skills

Monolith ships domain-specific workflow skills for Claude Code:

```bash
cp -r Plugins/Monolith/Skills/* ~/.claude/skills/
```

---

## Standalone tools

Two zero-dependency C++ executables ship in `Binaries/` and work without the editor:

```
Monolith.uplugin
  MonolithCore          — HTTP server, tool registry, discovery, auto-updater, monolith_guide, bulk_fill + describe ergonomics framework (monolith meta 5 + bulk_fill 2 + describe 3 actions)
  MonolithBlueprint     — Blueprint read/write, variable/component/graph CRUD, node operations, compile, CDO reader, dataset read/edit pack (DataTable/CurveTable/StringTable) (121 actions)
  MonolithMaterial      — Material inspection + graph editing + CRUD + material functions + tiling quality (63 actions)
  MonolithAnimation     — Animation sequences, montages, ABPs (incl. custom anim-graph nodes), PoseSearch, IKRig, Control Rig (125 actions)
  MonolithNiagara       — Niagara particle systems, dynamic inputs, event handlers, sim stages, event-aware summaries, scalability (109 actions)
  MonolithMesh          — Mesh inspection, scene manipulation, spatial queries, blockout, procedural geometry, horror/accessibility (194 core actions + 45 experimental town gen, off by default)
  MonolithAI            — Behavior Trees, Blackboards, State Trees, EQS, Smart Objects, Controllers, Perception, Navigation (243 actions)
  MonolithEditor        — Build triggers, log capture, compile output, crash context, scene/material-grid/overlay capture, material PBR and texture-channel inspection, ordered frame capture plus existing-frame adaptive GIF encoding, blank map factory, module status, automation test list/run, Python escape-hatch, level swap (60 actions)
  MonolithConfig        — Config/INI resolution and search (11 actions)
  MonolithIndex         — SQLite FTS5 deep project indexer, marketplace content, 15 asset indexers (7 actions)
  MonolithAsset         — Asset lifecycle, import, hygiene, inspection, validation, and batch rename helpers (12 actions)
  MonolithSource        — Native C++ engine source indexer, call graphs, class hierarchy, hot-reload-aware reindex (11 actions)
  MonolithUI            — UI widget Blueprint CRUD, templates, styling, animation v1+v2, animation read inspection/delta, explicit work context, EffectSurface, Spec Builder, Type Registry, visual artifact evidence, markup-to-UISpec conversion, UISpec diff/patch workflows, authored layout measurement, CommonUI, scaffolders + gap-closure (162 actions)
  MonolithGAS           — Gameplay Ability System: abilities, effects, attributes, ASC, tags, cues, targeting, ULeviathanVitalsSet template (135 actions)
  MonolithChaosFracture — Chaos/Geometry Collection status and asset/component listing (3 actions)
  MonolithLogicDriver   — Logic Driver Pro state machines: SM CRUD, graph read/write, JSON spec, scaffolding (66 actions)
  MonolithComboGraph    — ComboGraph combo trees: graph CRUD, nodes, edges, effects, cues (13 actions)
  MonolithAudio         — Audio asset CRUD, Sound Cue + MetaSound graph building + document introspection, batch ops, templates, AI perception binding, sine test wave (98 actions)
  MonolithAudioRuntime  — Runtime sub-module supplying perception classes for audio.bind_sound_to_perception (0 MCP actions)
  MonolithBABridge      — Blueprint Assist integration bridge (0 MCP actions — IModularFeatures only)
  MonolithLevelSequence — Level Sequence introspection: full per-LS binding inventory (legacy Possessable/Spawnable + UE 5.7 UMovieSceneCustomBinding family), Director Blueprint functions/variables, event-track bindings, cross-sequence reverse lookup (8 actions)
```

- **`monolith_proxy-<source-hash>.exe`** — source-addressed immutable MCP stdio↔HTTP proxy selected by `monolith_proxy.current.json`; the manifest verifies the full artifact SHA-256. Keeps AI sessions alive across editor restarts without colliding with a locked older proxy image.
- **`monolith_query-<source-hash>.exe` + `monolith_catalog-<semantic-hash>.json`** — immutable offline Query generation selected together by `monolith_query.current.json`. Serves the engine source index, project asset index, and Reflection Intelligence surface (`decision` / `risk` / `cppreflect` / `network`) without launching UE. The fixed `monolith_query.exe` remains a direct-CLI compatibility copy; release and proxy routing use the manifest-selected pair.

**~2102 in-tree actions across 77 namespaces** (v0.20.3 public release; `monolith_discover()` / `monolith_status()` are the authoritative live catalog; the per-namespace breakdown is runtime-discovered, not hand-maintained here). 45 town-gen experimental actions are disabled by default (`bEnableProceduralTownGen=false`); enabling them raises the count. This figure EXCLUDES sibling-plugin actions — sibling/private plugins ship through their own repos or channels and are not in the public release zip. Live editors with sibling plugins loaded report higher counts.

### Tool Reference

| Namespace | Tool | Actions | Description |
|-----------|------|---------|-------------|
| `monolith` | `monolith_discover` | — | List available actions per namespace |
| `monolith` | `monolith_status` | — | Server health, version, index status |
| `monolith` | `monolith_reindex` | — | Trigger full project re-index |
| `monolith` | `monolith_update` | — | Check or install updates |
| `monolith` | `monolith_guide` | — | Section-keyed onboarding guide for your AI (onboarding / recipes / decisions / errors / skills_map / gotchas) with a live registry overlay |
| `blueprint` | `blueprint_query` | 121 | Full Blueprint CRUD — read/write graphs, variables, components, functions, nodes, compile, CDO properties, auto-layout, dataset read/edit pack (DataTable/CurveTable/StringTable + `seed_data_asset`), cross-class property access, parent-function overrides |
| `material` | `material_query` | 63 | Inspection, editing, graph building, material functions, previews, validation, tiling quality, texture preview, CRUD |
| `animation` | `animation_query` | 125 | Montages, blend spaces, ABPs, skeletons, bone tracks, PoseSearch, IKRig, Control Rig, ABP/ControlRig writes, custom anim-graph nodes |
| `niagara` | `niagara_query` | 109 | Systems, emitters, modules, parameters, renderers, HLSL, dynamic inputs, event handlers, sim stages, effect types, scalability, event-aware summaries + `validate_system` event-chain reasoning |
| `mesh` | `mesh_query` | 194 (+45) | Mesh inspection, scene manipulation, spatial queries, blockout, GeometryScript, horror analysis, lighting, audio, performance, procedural geometry, encounter design, mesh import (incl. skeletal + animation). Town gen 45 actions registered only when `bEnableProceduralTownGen=true` (not in the public count) |
| `ai` | `ai_query` | 243 | BT, BB, State Trees, EQS, Smart Objects, Controllers, Perception, Navigation, runtime debugging, scaffolding. Conditional on `WITH_STATETREE` + `WITH_SMARTOBJECTS` |
| `gas` | `gas_query` | 135 | Gameplay Ability System — abilities, effects, attributes (incl. `ULeviathanVitalsSet`), ASC, tags, cues, targeting, input, inspect, scaffold. Conditional on `WITH_GBA` for Blueprint AttributeSets |
| `chaos_fracture` | `chaos_fracture_query` | 3 | Chaos/Geometry Collection module status plus Geometry Collection asset and component listing |
| `dataflow` | `dataflow_query` | 8 | Read-only Dataflow/Chaos graph discovery — asset listing, bounded graph and node-schema reads, duplicate/broken-connection validation. Graph readers conditional on `WITH_MONOLITH_DATAFLOW` |
| `logicdriver` | `logicdriver_query` | 66 | Logic Driver Pro state machines — SM CRUD, graph read/write, JSON spec, scaffolding, components. Conditional on `WITH_LOGICDRIVER` |
| `combograph` | `combograph_query` | 13 | ComboGraph combo trees — graph CRUD, nodes, edges, effects, cues, ability scaffolding. Conditional on `WITH_COMBOGRAPH` |
| `audio` | `audio_query` | 98 | Sound asset CRUD, Sound Cue + MetaSound graph building (Builder API write-side), MetaSound document introspection (read-side, v0.14.10 +12 from PR #18 by @alakangas), batch ops, audio health checks, templates, sine test wave, AI perception binding. MetaSound features conditional on `WITH_METASOUND` |
| `ui` | `ui_query` | 149 | UMG widget CRUD, templates, styling, animation v1+v2 plus read-only overview/timeline/time-slice inspection and guarded scalar float-key deltas, explicit session/request work context, EffectSurface, Spec Builder, Type Registry, visual artifact evidence, markup-to-UISpec conversion, authored layout measurement, settings scaffolding, headline scaffolders, navigation/conversion gap-closure, accessibility. CommonUI 51 actions conditional on `WITH_COMMONUI`. 4 GAS attribute-binding aliases also live here |
| `editor` | `editor_query` | 60 | Build triggers, error logs, compile output, crash context, scene/material-grid/overlay capture, material PBR and texture-channel inspection, ordered frame capture and adaptive 60 fps GIF encoding from new or existing PNG frames, blank map factory, module status, selection/context inspection, UE automation test list/run, Python escape-hatch, persistent-level swap |
| `config` | `config_query` | 6 | INI resolution, explain, diff, search |
| `console` | `console_query` | 6 | Live `IConsoleManager` console object discovery, EngineSource.db snapshot refresh, FTS5 search, exact lookup, health, and live console execution |
| `project` | `project_query` | 7 | Deep project search — FTS5 across all indexed assets including marketplace plugins |
| `asset` | `asset_query` | 12 | Asset lifecycle, import from file/bytes, font family ingest, hygiene, inspection, validation, fuzzy find, and batch rename helpers |
| `source` | `source_query` | 11 | Native C++ engine source lookup, call graphs, class hierarchy, project reindex, hot-reload-aware refresh |
| `bulk_fill` | `bulk_fill_query` | 2 | Reflection-walker bulk property fill across 12 per-namespace adapters — `apply` (dry-run-able tree write), `list_namespaces` |
| `describe` | `describe_query` | 3 | Read-only schema introspection for the same 12 adapters — `schema`, `list_targets`, `action_schema` (any registered action's full param schema) |
| `level_sequence` | `level_sequence_query` | 8 | Level Sequence inspection: full binding inventory (one row per Guid×BindingIndex with kind classification — legacy Possessable/Spawnable + UE 5.7 UMovieSceneSpawnableActorBinding / Replaceable / Custom), Director Blueprint own functions (user / custom_event / sequencer_endpoint) and variables, event-track bindings with Director-function resolution, cross-sequence reverse lookup of function callers |

---

## Auto-updater

Off by default as of v0.14.6. Opt in via **Auto Update Enabled** in Editor Preferences > Plugins > Monolith — checks GitHub Releases on editor startup, selects the matching per-engine asset, verifies the downloaded zip's SHA256 against `Monolith-SHA256-UE5.7:` / `Monolith-SHA256-UE5.8:` when present (legacy `Monolith-SHA256:` remains accepted; otherwise warns and proceeds), swaps the plugin on editor exit (after a Y/N prompt). See [Auto-Updater wiki](https://github.com/tumourlove/monolith/wiki/Auto-Updater).

---

## Network exposure

Monolith starts a local HTTP server on port 9316 to receive MCP traffic. UE's `FHttpServerModule` does **not** expose a bind-address parameter, so the listener is reachable on all network interfaces, not just `127.0.0.1`. CORS is restricted to localhost origins (which blocks browser-based cross-origin reads) but does **not** block direct HTTP requests from other devices on the same LAN.

If you work on an untrusted network: either add a Windows Firewall rule blocking inbound TCP on port 9316 from non-loopback addresses, or untick **MCP Server Enabled** in Editor Preferences > Plugins > Monolith and restart the editor.

See [SECURITY.md](SECURITY.md) for the full threat model and disclosure policy.

---

## Documentation

- **[Wiki](https://github.com/tumourlove/monolith/wiki)** — installation variants, tool reference, connecting your AI, configuration, auto-updater, FAQ, skills, optional modules, engine source index details, mesh module deep dive, horror level design, procedural geometry, genre presets, test status
- **[API_REFERENCE.md](Docs/API_REFERENCE.md)** — full per-action parameter reference, regenerated from the live registry each release
- **[SPEC_CORE.md](Docs/SPEC_CORE.md)** — technical specification and architecture; per-module specs at [`Docs/specs/`](Docs/specs/)
- **[CHANGELOG.md](CHANGELOG.md)** — version history, contributor credits, breaking-change notes
- **[CONTRIBUTING.md](CONTRIBUTING.md)** — dev setup, coding conventions, how to add new actions, PR process

---

## Contributing

Contributions welcome. See [CONTRIBUTING.md](CONTRIBUTING.md). Every release [CHANGELOG](CHANGELOG.md) names the PR authors and issue reporters whose work shipped — credit goes where it's due.

---

## License

[MIT](LICENSE) — see [ATTRIBUTION.md](ATTRIBUTION.md) for credits.
