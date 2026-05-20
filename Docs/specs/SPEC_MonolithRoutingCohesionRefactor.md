# Monolith - Agent Routing, Action Contract & Cohesion Refactor Plan

**Parent:** [../SPEC_CORE.md](../SPEC_CORE.md)
**Engine:** Unreal Engine 5.7+
**Status:** Partially implemented (P0-P5 complete; P6 Core/Source high-traffic slice complete; R6 readiness/freshness complete; P7 partial with module/build-rule/test-support helper slices, first Index/UI decohesion slices, Core monolith management schema-validation coverage, expanded Mesh/AI/GAS/UI/Animation/Blueprint/ComboGraph/Interchange/LevelSequence/SourceControl/Config/Localization/LogicDriver/Index domain schema-validation slices, and small optional/read-only namespace validation slices)
**Audit date:** 2026-05-19
**Owner modules:** `MonolithCore`, `MonolithSource`, `MonolithIndex`, all `Source/Monolith*` domain modules
**Scope:** `Source/Monolith*`, MCP `tools/list` / `tools/call` surfaces, `monolith.*` discovery actions, action result payload conventions, agent guideline docs, per-module specs and tests

---

## 0. Implementation Status

| Phase | Status | Evidence |
|-------|--------|----------|
| P0 | Complete | Contract accepted and linked from `Docs/TODO.md`. |
| P1 | Complete | `FMonolithProjectionSpec` / `FMonolithProjectionUtils` added with automation tests; `MonolithCoreTools` reuses the shared projection helpers. |
| P2 | Complete | `monolith.find` and projection-aware `monolith.discover` implemented with profile metadata and compact global summary. |
| P3 | Complete | `{namespace}_query` tool descriptions are compact; agent docs route through `monolith_find` + schema-targeted `monolith_discover`; deferred catalog actions are documented as advanced/internal. |
| P4 | Complete | Legacy `MonolithSourceActions.cpp` action-level `content[]` builders removed; source lookup/read/index trigger actions now return structured payloads with common limits and row fields. |
| P5 | Complete | Source and Project/Index review actions preserve existing semantic fields and add common `success`, `count`, and `truncated` fields via the shared Core helper. |
| P6 | Complete for Core/Source high-traffic slice | `FParamSchemaBuilder` supports validation opt-in plus enum/range metadata; `FMonolithToolRegistry` enforces typed params for opted-in schemas. Core routing actions and high-traffic Source lookup/read actions now advertise and enforce ranges/enums/types. |
| R6 | Complete | `FMonolithIndexFreshnessUtils` centralizes ProjectIndex/EngineSource DB path and file freshness reporting; `monolith.status`, `get_readiness_status`, and live `index.get_index_status` expose index mtime/size/age. Readiness summarizes `source.health` and `project.health` including CRG warning counts. |
| P7 | Partial | Review-result helper reuse, simple module registration helper, shared Build.cs optional dependency probe helpers, expanded `MonolithTestSupport` param-guard migration for Core/Source/Mesh/AI/GAS/UI/Animation/Blueprint/ComboGraph/Interchange/LevelSequence/SourceControl/Config/Localization/LogicDriver/Index and small optional/read-only namespaces, first Index action-registration split, first UI module-file decohesion slice, Core monolith management schema-validation coverage, and expanded Mesh/AI/GAS/UI/Animation/Blueprint/ComboGraph/Interchange/LevelSequence plus Water/NDisplay/Paper2D/PCG/ChaosFracture/WorldConditions/Dataflow/GameFeatures/Slate/SourceControl/Config/Localization/LogicDriver/Index schema-validation slices are in place; broader cross-module test migration, additional Mesh/UI decohesion, and wider domain schema validation remain follow-up work. |

---

## 1. Goals

1. Maximize code reuse and cohesion without breaking public action names.
2. Make the agent routing path unambiguous: one fast way to find the right namespace/action, one clear fallback path when the editor is unavailable.
3. Make action responses return only the data an agent asked for by default, with explicit projection controls for larger payloads.
4. Keep Monolith's runtime action catalog live and authoritative; static docs should route agents to live discovery instead of duplicating per-action catalogs.
5. Preserve current MCP compatibility: existing `{namespace}_query` tools and action names remain registered.

### Non-Goals

| Non-goal | Reason |
|----------|--------|
| Removing existing public action names | Too much downstream client risk. |
| Replacing the MCP HTTP transport | This plan is about routing/contracts inside the existing transport. |
| Bulk-changing every domain handler in one pass | Too risky; use opt-in helpers and migrate high-traffic domains first. |
| Storing raw request params or raw result payloads in logs | Existing privacy posture must remain. |
| Hand-maintaining a full action catalog in `AGENTS.md`, `CLAUDE.md`, or templates | Action counts and schemas drift; live discovery is the source of truth. |

---

## 2. Audit Inputs

| Input | Result |
|-------|--------|
| Source tree scan | 30 modules, 712 C++/C#/header files, about 263k lines under `Plugins/Monolith/Source`. |
| Largest modules by lines | `MonolithMesh` 57.8k, `MonolithUI` 32.0k, `MonolithAI` 26.8k, `MonolithGAS` 24.1k, `MonolithBlueprint` 15.1k. |
| Registration scan | 1692 `RegisterAction(` call sites or references; `MonolithMesh`, `MonolithAI`, `MonolithGAS`, `MonolithAnimation`, `MonolithUI`, and `MonolithBlueprint` dominate. |
| Param parsing scan | 1610 `FParamSchemaBuilder(` uses, 1898 `TryGetStringField`, 812 `TryGetNumberField`, 433 `TryGetBoolField`, 608 "Missing required" strings. |
| Test scaffold scan | 10 `*ParamGuardTests.cpp` and 6 `*RegistryContractTests.cpp` repeat similar guard/contract patterns. |
| Result payload scan | 14 action-level `content[]` builders; 13 are in `MonolithSource`, separate from the transport-level MCP result envelope. |
| Offline source DB | `monolith_query.exe source health` opens `EngineSource.db`; core schema/FTS/CRG parity checks pass, with an existing orphan-reference warning. |
| Existing plan docs | `TODO.md` already links this spec; `SPEC_MonolithStructuredToolResults.md` and `SPEC_MonolithActionExecutionPolicy.md` are implemented first slices to build on. |

---

## 3. Current Architecture Snapshot

### 3.1 Dispatch and Discovery

| Component | Current behavior | Assessment |
|-----------|------------------|------------|
| `FMonolithToolRegistry` | Central singleton keyed by `namespace.action`; stores handler, param schema, category, and execution policy. | Good foundation. |
| `FMonolithHttpServer::HandleToolsList` | Exposes core actions as `monolith_*` tools and every non-core namespace as `{namespace}_query(action, params)`. Domain tool descriptions include all action names. | Compatible but bloated; not ideal as the primary agent catalog. |
| `FMonolithHttpServer::HandleToolsCall` | Normalizes `monolith_*` and `{namespace}_query` dispatch, accepts nested `params` object or JSON-encoded params string. | Valuable client compatibility logic; should be retained. |
| `monolith.discover` | Always-on catalog action. Without namespace, returns namespaces plus action names. With namespace, returns action descriptions, params, category, and execution policy. | Should become the canonical catalog surface, but needs projection/query controls. |
| Deferred catalog actions | `list_domains`, `describe_domain`, `load_domain`, `get_loaded_domains` exist only when `bEnableDeferredDomainCatalog` registered them. | Useful experiment, but confusing as an agent entrypoint because availability is settings-dependent. |
| Tool profiles | Active profile filters action visibility and can override descriptions. | Good mechanism, but discovery responses need clearer "profile-filtered" metadata by default. |
| Readiness | `get_readiness_status` checks server, registry, project/source DB file freshness, project/source health, CRG warning summaries, project index setting/module, editor actions, optional modules. | Agent can now see whether routing should trust the current indexes before action execution. |

### 3.2 Code, Project, and Context Search

| Namespace | Role | Current state |
|-----------|------|---------------|
| `source` | Engine/project C++ symbols, references, callers/callees, source FTS, review graph. | Strong but older read actions still return text under action-level `content[]`. |
| `project` | Asset index search, references, asset details, gameplay tags, project review graph. | Newer actions generally return structured rows and limits. |
| `context` | Agent-friendly search across indexed assets/source plus bounded attachments. | Best current model for compact defaults, limits, truncation, and follow-up materialization. |
| Offline CLI | `Binaries/monolith_query.exe source <action>` reads `EngineSource.db` without editor runtime. | Required fallback when editor/MCP is not running; should be documented as the code-find fallback. |

### 3.3 Result Envelope

| Layer | Current behavior | Issue |
|-------|------------------|-------|
| Transport envelope | `FMonolithToolResultUtils` builds MCP `content[]`, `isError`, and optional `structuredContent` / `_meta`. | Good; keep this as the only MCP envelope builder. |
| Domain action payloads | Most handlers return structured JSON objects. Some older source/context actions build their own `content[]`. | Domain payloads and MCP transport payloads are mixed; structured clients get nested text instead of rows. |
| Large payload controls | Some actions use `limit`, `max_results`, `detail_level`, `include_*`, or `truncated`; no shared convention. | Agents cannot predict which knob reduces payload size. |

---

## 4. Findings

### 4.1 Agent Routing Is Still More Ambiguous Than Necessary

| Finding | Evidence | Impact |
|---------|----------|--------|
| Multiple catalog surfaces overlap | `tools/list`, `monolith.discover`, `get_effective_discovery`, `get_mcp_discovery_state`, deferred `list_domains` / `describe_domain`. | Agents can waste turns choosing a discovery path instead of acting. |
| `tools/list` domain descriptions are too heavy | Domain tool description is built by concatenating every action name in the namespace. | Large tool descriptions increase prompt cost and make search harder. |
| `monolith.discover` lacks narrow discovery modes | Namespace discovery returns every action row and schema; global discovery returns all action names. | The agent cannot ask for "top matches for this task" or "schema for exactly this action" in one bounded call. |
| Static action counts drift | `Monolith.uplugin`, `SPEC_CORE.md`, and generated/runtime counts disagree across recent work. | Agents should not trust static counts; docs should make live discovery authoritative. |

### 4.2 Code Reuse and Cohesion Are Uneven

| Finding | Evidence | Impact |
|---------|----------|--------|
| Module startup/shutdown boilerplate repeats | Every module file has the same register/log/unregister shape, with bigger custom versions in Mesh/UI/Index. | New domains copy patterns by hand; shutdown namespace lists can drift. |
| Optional dependency probing repeats in Build.cs | AI, Mesh, Audio, GAS, UI, LogicDriver, ComboGraph, LevelSequence, Dataflow, Index, WorldConditions each repeat release-gate/path probing. | Same bug class can recur per module. |
| Param parsing is partly centralized but mostly manual | Schema aliases and required checks exist in registry, but type/range/enum checks remain in handlers. | Error text and validation semantics vary by action. |
| Test scaffolds duplicate guard loops | 10 param guard tests and 6 registry contract tests repeat local runners. | Coverage is useful but expensive to maintain. |
| `MonolithMesh` and `MonolithUI` are still broad aggregates | Mesh is 98 files / 57.8k lines; UI is 113 files / 32.0k lines, with many feature packs and inline registration. | High-coordination modules; harder to reason about ownership and public contracts. |

### 4.3 Action Results Are Not Consistently Minimal

| Finding | Evidence | Impact |
|---------|----------|--------|
| Source lookup actions often return text blobs | `MonolithSourceActions.cpp` builds action-level `content[]` in 13 sites. | `structuredContent` cannot expose useful rows without parsing text. |
| Projection knobs are inconsistent | Some actions use `limit`, others `max_results`; some use `detail_level`, `include_*`, `max_chars`, or no controls. | Agents cannot reliably request "minimal" output across domains. |
| Diagnostics and payloads are mixed | Many actions return warnings, next actions, logs, source snippets, rows, and summaries in ad hoc shapes. | Clients cannot safely extract only the user-relevant rows. |
| Attachment-style outputs are not isolated enough | `index.build_attachment` correctly materializes large text on demand; older source reads return text directly. | Search actions and materialization actions should be separate. |

---

## 5. Target Contracts

### 5.1 Agent Routing Contract

Agents should use this ladder:

| Step | Action | Contract |
|------|--------|----------|
| 1 | `monolith.status` or `monolith.get_readiness_status` | Confirm server, registry, source/project indexes, and CRG freshness. |
| 2 | `monolith.find` | Fuzzy task-to-action search across namespaces, descriptions, categories, and aliases. |
| 3 | `monolith.discover` | Canonical schema/catalog surface. Supports bounded global, namespace, category, and single-action modes. |
| 4 | `{namespace}_query` | Execute selected domain action with explicit params. |
| 5 | `index.search_items` / `index.build_attachment` | Find and materialize bounded asset/source context. |
| 6 | Offline `monolith_query.exe source ...` | Code-find fallback when editor/MCP is unavailable. |

### 5.2 `monolith.find` Additive Action

Add an always-on core action:

```json
{
  "query": "find caller graph action",
  "namespace": "source",
  "limit": 8,
  "include_schema": false
}
```

Response:

```json
{
  "status": "ok",
  "query": "find caller graph action",
  "matches": [
    {
      "action_id": "source.find_callers",
      "namespace": "source",
      "action": "find_callers",
      "score": 0.92,
      "reason": "action name and description matched caller search",
      "description": "Find all functions that call the given function",
      "mcp_tool": "source_query"
    }
  ],
  "truncated": false,
  "next_actions": ["monolith.discover"]
}
```

Implementation notes:

| Requirement | Detail |
|-------------|--------|
| Input matching | Reuse `FindSimilarActions` for action names, add description/category token scoring. |
| Profile awareness | Only return actions allowed by the active tool profile. |
| Optional domains | Include known optional domain rows with `status=disabled` or `not_installed` when relevant. |
| Schema control | `include_schema=false` by default; when true, include only the matched action schemas. |

### 5.3 `monolith.discover` Projection Contract

Extend the existing action, without removing existing fields:

| Param | Type | Default | Meaning |
|-------|------|---------|---------|
| `namespace` | string | empty | Filter to one namespace. |
| `action` | string | empty | Return one action row and schema. Requires or implies `namespace`. |
| `query` | string | empty | Text filter over action/category/description. |
| `category` | string | empty | Existing category filter. |
| `mode` | string | `summary` | `summary`, `actions`, or `schema`. |
| `fields` | array | empty | Optional allowlist of top-level row fields. |
| `limit` | integer | 100 | Maximum namespace/action rows. |
| `cursor` | string | empty | Cursor for continuation. |
| `include_optional` | bool | true | Include known optional modules. |

Default global discovery must be compact:

```json
{
  "status": "ok",
  "mode": "summary",
  "active_profile_id": "default",
  "namespaces": [
    {"namespace": "source", "action_count": 28, "mcp_tool": "source_query"}
  ],
  "total_actions": 1300,
  "truncated": false
}
```

Single action schema mode:

```json
{
  "status": "ok",
  "mode": "schema",
  "namespace": "source",
  "action": "search_source",
  "mcp_tool": "source_query",
  "description": "...",
  "inputSchema": {"type": "object", "properties": {}},
  "execution_policy": {}
}
```

### 5.4 Domain Result Contract

All action handlers should return structured domain payloads. `FMonolithToolResultUtils` is the only layer that creates MCP `content[]`.

Common fields:

| Field | Requirement |
|-------|-------------|
| `status` | `ok`, `warning`, `error`, `started`, `skipped`, or domain-specific state. |
| `success` | Boolean for easy client filtering; true for non-error results. |
| `input` | Normalized input summary, not raw unbounded params. |
| `limits` | Effective `limit`, `max_results`, `detail_level`, `fields`, `max_chars`, cursor info. |
| `items` / domain rows | Main data array. Prefer domain-specific names only when clearer (`symbols`, `assets`, `bindings`, `warnings`). |
| `count` | Count of returned primary rows. |
| `truncated` | True whenever more data exists than returned. |
| `next_cursor` | Present only when continuation exists. |
| `warnings` | Non-fatal conditions. |
| `next_actions` | Machine-readable action ids, not prose where possible. |

Common request controls:

| Param | Meaning |
|-------|---------|
| `limit` | Row count cap. Prefer this over new `max_results`; accept legacy aliases where already shipped. |
| `detail_level` | `minimal` default, `standard`, `full`. |
| `fields` | Optional allowlist for row fields. |
| `include_diagnostics` | Include debug/status details. Default false. |
| `include_source` / `include_assets` | Existing domain-specific include groups are allowed. |
| `max_chars` | Only for materialized text attachments or file/log reads. |

Rules:

1. Search/list actions default to `detail_level=minimal`.
2. Large text is returned only by explicit read/materialize actions (`read_file`, `build_attachment`, log reads) and must expose `max_chars` or line ranges.
3. Handler payloads do not build `content[]`; transport envelope handles MCP text/structured compatibility.
4. `structuredContent` must be useful without parsing text when `bEnableStructuredToolResults=true`.
5. Existing legacy fields may remain during migration, but new fields must follow the common contract.

### 5.5 Code-Find Contract

| Scenario | Required route |
|----------|----------------|
| Find C++ symbols/API/signatures | `source.search_source`, then `source.get_symbol_context` or `source.read_source`. |
| Find callers/callees/references | `source.find_callers`, `source.find_callees`, `source.find_references`. |
| Search assets | `project.search` or `index.search_items`. |
| Need prompt material | `index.build_attachment`, not a broad read/list action. |
| MCP/editor unavailable | `Plugins/Monolith/Binaries/monolith_query.exe source <action> ...`. |
| Both MCP and offline DB unavailable | Raw file search is last resort only. |

---

## 6. Refactor Workstreams

### R1 - Core Routing and Discovery

| Change | Files | Notes |
|--------|-------|-------|
| Add `monolith.find` | `MonolithCoreTools.{h,cpp}` | Additive, always-on, profile-aware. |
| Extend `monolith.discover` with `mode`, `query`, `action`, `fields`, `limit`, `cursor` | `MonolithCoreTools.cpp`, tests, API docs | Preserve old response fields where possible. |
| Shorten `{namespace}_query` descriptions in `tools/list` | `MonolithHttpServer.cpp` | Keep action enum if needed, but move full catalog detail to discovery. |
| Mark deferred catalog actions as advanced/internal in docs | `SPEC_MonolithCore.md`, API docs | Do not remove them. |
| Add active profile and projection metadata to discovery | `MonolithToolProfileManager`, `MonolithCoreTools` | Make profile filtering explicit. |

### R2 - Result Projection and Minimal Payloads

| Change | Files | Notes |
|--------|-------|-------|
| Add shared result/projection helpers | `MonolithCore/Public/MonolithProjectionUtils.h`, `MonolithCore/Private/MonolithProjectionUtils.cpp` | Implemented as `FMonolithProjectionSpec` / `FMonolithProjectionUtils`; shared `status`, `success`, `input`, `limits`, `count`, `truncated`, `next_cursor`, `next_actions`. |
| Add projection parser | `MonolithCore` | Parses `limit`, legacy `max_results`, `detail_level`, `fields`, `include_diagnostics`, `cursor`, and `max_chars`. |
| Migrate `source.*` read/search actions away from action-level `content[]` | `MonolithSourceActions.cpp` | Implemented for legacy source lookup/read actions and source index triggers. Offline `monolith_query` parity remains a follow-up if CLI output adapters still assume old text-only shapes. |
| Standardize review graph outputs | `MonolithSourceReview`, `MonolithIndexReview`, `FMonolithToolResultUtils` | Implemented for live Source and Project review actions: shared Core helper adds `success`, `count`, and `truncated` without removing semantic arrays. |
| Add golden response tests | `MonolithCore/Private/Tests`, `MonolithSource/Private/Tests`, `MonolithIndex/Private/Tests` | Verify minimal/standard/full and truncation semantics. |

### R3 - Param Validation Reuse

| Change | Files | Notes |
|--------|-------|-------|
| Extend schema builder with enum/range/array/object/default metadata | `MonolithParamSchema.h`, tests | First slice complete: `EnableValidation()`, `Enum()`, and `Range()` are additive metadata; old schemas still valid. |
| Add typed param reader | `MonolithParamUtils` or new `MonolithParamReader` | First slice uses registry-level typed validation helpers; a reusable handler-side reader remains follow-up. |
| Registry-level optional type validation | `FMonolithToolRegistry::ExecuteAction` | Implemented for opted-in params. Required and alias checks still run before dispatch; type/range/enum errors return `-32602`. |
| Migrate high-churn domains first | `MonolithSource`, `MonolithIndex`, `MonolithCore`, then Mesh/UI/GAS/AI/Animation/Blueprint/ComboGraph/Interchange/LevelSequence/SourceControl/Config/Localization and small optional/read-only namespaces | Core routing and high-traffic Source lookup/read schemas migrated first. Core now opts its production `monolith` management surface into registry-level top-level validation across routing/discovery/status, update/reindex, MCP/session compatibility, onboarding/readiness/notification settings, execution audit/policy, and tool profile management. Source now opts all 23 `source` actions and all 5 `index` actions into registry-level top-level validation before source DB traversal, CRG review handlers, indexing dispatch, attachment materialization, or asset/source bridge logic. First Mesh slice now opts `get_mesh_info`, `place_storytelling_scene`, `create_parametric_mesh`, `create_fragments`, `generate_roof`, and `analyze_building_site` into registry-level typed validation. First AI slice opts `move_st_state` and `scaffold_patrol_investigate_ai` into registry-level typed validation. First GAS slice opts `input.create_input_action`, `gas.scaffold_weapon_ability`, `gas.create_target_actor`, `gas.configure_target_actor`, and `gas.scaffold_fps_targeting` into registry-level typed validation while keeping nested config/array validation in handlers. First UI slice opts Widget CRUD, Slot, Binding/ListView queries, and all six baseline Styling actions into top-level registry validation while keeping nested/tolerant style payload handling in handlers. First Animation slice opts `set_sequence_properties` and `set_montage_blend` into registry-level typed validation before asset loading. Blueprint slices opt Read/query, Variable, Component, Graph, Node/Pin/Timeline/cache, Utility/Data, Compile/asset, and CDO actions into registry-level typed validation before Blueprint asset loading. ComboGraph opts all 13 `combograph` actions into registry-level top-level validation before ComboGraph assets or GameplayAbility Blueprints are loaded, while nested effect/cue payload checks remain handler-side. Interchange opts all 16 `interchange` actions into registry-level top-level validation before source file probing, asset import-data reads, import/reimport/export tasks, or write confirmation logic. LevelSequence opts all 26 `level_sequence` / `movie_render` actions into registry-level top-level validation before replay path inspection, Level Sequence asset loading, Director/index queries, optional Anim Mixer reflection, Movie Render Pipeline queue access, or render mutation logic. Water, NDisplay, Paper2D, PCG, ChaosFracture, and WorldConditions opt their 18 read-only/optional actions into registry-level top-level validation before AssetRegistry scans, editor-world reflection, optional dependency probes, or condition-query serialization. Dataflow, GameFeatures, and Slate opt 19 registered read-only/optional actions into registry-level top-level validation before Dataflow asset loading, GameFeature descriptor/data inspection, live Slate traversal, ref resolution, or screenshot capture. SourceControl opts all seven `source_control` actions into registry-level top-level validation before provider state queries or checkout/add/revert operations. Config and Localization opt all 20 `config` / `localization` actions into registry-level top-level validation before config file reads, plugin/CVar enumeration, culture lookup, StringTable asset loading, CSV file access, or write-gate handling. LogicDriver opts all 66 `logicdriver` actions into registry-level top-level validation before Logic Driver asset, graph, PIE, component, JSON import, or scaffold handlers run. Index opts all 23 `project` actions and all 13 `collection` actions into registry-level top-level validation before asset DB traversal, collection manager mutation, specialized asset inspection, or CRG review handlers run. `set_widget_property.value` and `set_cdo_property.value` now advertise their actual JSON union shapes instead of string-only or `any` schemas; `build_blueprint_from_spec` collection params now advertise `array|string` to preserve existing serialized-array fallback. |

### R4 - Module Cohesion and Registration Reuse

| Change | Files | Notes |
|--------|-------|-------|
| Add domain module registration helper | `MonolithCore` or header-only support | First slice implemented as `FMonolithModuleRegistration`; simple single-namespace modules now share register/count and namespace cleanup plumbing. |
| Use explicit namespace list per module | Each module file | First slice uses helper-owned namespace lists for Dataflow, PCG, Water, NDisplay, Interchange, Paper2D, Slate, and Index shutdown (`project` / `collection`). Multi-namespace/custom modules remain follow-up. |
| Move inline action lambdas out of module files | Example: UI `dump_style_cache_stats` | First UI slice implemented: `dump_style_cache_stats` moved from `MonolithUIModule.cpp` to `Style/MonolithUIStyleDiagnosticsActions.{h,cpp}`. Module startup now only calls the action registrar. |
| Move bulk action registration out of module files | Example: Index `project` actions | First Index slice implemented: `MonolithIndexModule.cpp` now delegates 19 `project` actions plus collection mounting to `Actions/ProjectActionRegistration.{h,cpp}` and only logs counts / unregisters namespaces. |
| Continue Mesh/UI decohesion | `MonolithMesh`, `MonolithUI` | First UI diagnostic action split complete. Continue splitting services/helpers internally while keeping public action names stable. |
| Add `MonolithTestSupport` | `MonolithCore/Public/MonolithTestSupport.h`, representative tests | Expanded slice implemented: shared param guard and registry contract table runners, plus scoped temp namespace cleanup. `MonolithModuleRegistrationTests.cpp`, Source source/index top-level param guard tests, Mesh terrain/inspection/decal/procedural/roof/fragments param guard tests, AI StateTree/scaffold param guard tests, GAS input/scaffold/target param guard tests, UI widget/slot/binding/baseline styling top-level param guard tests, Animation sequence/montage top-level param guard tests, Blueprint Read/query, Variable, Component, Graph, Node/Pin/Timeline/cache, Utility/Data, Compile/asset, CDO top-level param guard tests, ComboGraph top-level param guard tests, Interchange top-level param guard tests, LevelSequence/MovieRender top-level param guard tests, Water/NDisplay/Paper2D/PCG/ChaosFracture/WorldConditions/Dataflow/GameFeatures/Slate top-level param guard tests, SourceControl top-level param guard tests, Config/Localization top-level param guard tests, LogicDriver top-level param guard tests, and Index project/collection top-level param guard tests now use the shared runners. |

### R5 - Optional Dependency Build Rules

| Change | Files | Notes |
|--------|-------|-------|
| Add reusable Build.cs helper pattern | `MonolithCore.Build.cs` | Implemented as `MonolithBuildRulesSupport`, a rules-assembly helper class shared by sibling `.Build.cs` files without introducing a fake module. |
| Normalize release gate behavior | AI, Mesh, Audio, GAS, UI, LogicDriver, ComboGraph, LevelSequence, Dataflow, Index, WorldConditions, BABridge | First broad slice migrated to `MonolithBuildRulesSupport`; `MONOLITH_RELEASE_BUILD=1` is centralized through `IsReleaseBuild()` / helper-gated probes. |
| Normalize 3-location plugin probes | Project, engine Runtime/Editor/Marketplace/top-level as appropriate | First broad slice migrated for StateTree, SmartObjects, GameplayAbilities, MassEntity, ZoneGraph, MetaSound, GeometryScripting, CommonUI, BlueprintAttributes, ComboGraph, LogicDriver/SMSystem, MovieRenderPipeline, WorldConditions, and BlueprintAssist. |
| Add static check for hard deps | `Scripts/ci_static_checks.py` or equivalent | Catch accidental optional plugin hard-linking. |

### R6 - Readiness and Freshness

| Change | Files | Notes |
|--------|-------|-------|
| Surface project/source index freshness | `monolith.status`, `get_readiness_status`, `index.get_index_status` | Implemented 2026-05-19. `index_freshness` / per-index `freshness` include DB path, exists flag, size, mtime, and age. `index.get_index_status(include_stats=true)` also carries stats/health summaries from live subsystems. |
| Surface CRG cache freshness/parity | Source/project health, readiness | Implemented 2026-05-19. Readiness executes `source.health` / `project.health` when registered and adds compact status, warning count, CRG check counts, and next actions. |
| Add action catalog freshness | Discovery response | Implemented 2026-05-19. Discovery already includes `snapshot_mode=live_registry`, active profile, and profile filtering; `monolith.status` / readiness also expose `action_catalog`. |
| Keep post-build source index path documented | `SPEC_MonolithSource.md`, testing docs | `PostBuildSourceIndex.bat` + commandlet path remains the offline writer. |

---

## 7. Phased Rollout

| Phase | Work | Verification gate |
|-------|------|-------------------|
| P0 | Accept this plan and freeze non-goals. | Spec linked from `TODO.md`; no source changes. |
| P1 | Add result/projection helpers and test support. **Complete 2026-05-19.** | `MonolithCore` automation tests compile; helper test source added. |
| P2 | Add `monolith.find` and `monolith.discover` projection modes. **Complete 2026-05-19.** | Golden response coverage added to domain catalog automation tests; command-line automation currently blocked by local editor startup errors. |
| P3 | Trim `tools/list` domain descriptions and update agent guidelines. **Complete 2026-05-19.** | MCP `tools/list` still exposes all tools; action execution unchanged. |
| P4 | Migrate `source.*` legacy text-content actions to structured domain payloads. **Complete 2026-05-19.** | `source.search_source`, `read_source`, `find_*`, `get_*`, `read_file`, and index triggers return structured payloads; read/materialize actions keep bounded `text`. |
| P5 | Migrate project/source review outputs to shared result contract. **Complete 2026-05-19.** | Source and Project review handlers keep existing semantic fields and add common `success/count/truncated` through `FMonolithToolResultUtils::AddReviewResultCommonFields`. |
| P6 | Add schema enum/range/type validation opt-in and migrate high-traffic handlers. **Core/Source high-traffic slice complete 2026-05-19; full Source/index validation complete 2026-05-20; Core management validation complete 2026-05-20.** | `Monolith.ParamSchema.TypedValidation` added; UBT build passes. Error changes are intentional for opted-in Core routing/management, Source lookup/read/review actions, and index readiness/bridge actions. |
| P7 | Collapse module/test/build boilerplate. | Partial: review-result normalization, simple module registration, Build.cs optional dependency probes, expanded Core/Source/Mesh/AI/GAS/UI/Animation/Blueprint/ComboGraph/Interchange/LevelSequence/SourceControl/Config/Localization/LogicDriver/Index and small optional/read-only namespace param-guard migration to `MonolithTestSupport`, Index action-registration hoisting, UI `dump_style_cache_stats` action hoisting, and expanded Core/Source/Mesh/AI/GAS/UI/Animation/Blueprint/ComboGraph/Interchange/LevelSequence/Water/NDisplay/Paper2D/PCG/ChaosFracture/WorldConditions/Dataflow/GameFeatures/Slate/SourceControl/Config/Localization/LogicDriver/Index schema-validation migrations moved repeated guard logic into shared registry contracts. Remaining work is broader cross-module test migration, additional Mesh/UI decohesion, and wider domain schema validation. |

---

## 8. Verification Matrix

| Area | Required evidence |
|------|-------------------|
| Build | `GoGameEditor Win64 Development` UBT build succeeds. |
| Registry | `monolith.status` action count unchanged except additive `monolith.find` and explicitly documented actions. |
| Discovery | `monolith.discover` old calls still work; new modes are bounded and profile-aware. |
| Tools list | `{namespace}_query` tools still dispatch; descriptions no longer carry the whole action catalog. |
| Structured results | With `bEnableStructuredToolResults=true`, migrated actions expose rows in `structuredContent` without text parsing. |
| Legacy compatibility | With `bEnableStructuredToolResults=false`, legacy text JSON remains present. |
| Source fallback | `monolith_query.exe source health` opens `EngineSource.db`; source CLI docs remain correct. |
| Result budgets | Search/list actions obey `limit`, expose `truncated`, and avoid unbounded source/log text. |
| Privacy | ToolCall/session/audit surfaces still avoid raw params, raw payloads, auth headers, cookies, tokens, and secrets. |
| Docs | `SPEC_CORE.md`, `SPEC_MonolithCore.md`, `SPEC_MonolithSource.md`, `API_REFERENCE.md`, `TODO.md`, templates, and project `AGENTS.md` stay in sync when code lands. |

---

## 9. Risks and Mitigations

| Risk | Mitigation |
|------|------------|
| Public contract drift | All public action names remain; result changes are additive or gated by migration notes. |
| Different error text from central validation | Opt-in per action; golden tests for migrated actions. |
| Discovery response bloat persists | Make `summary` the default, add `fields`, `limit`, `cursor`, and single-action schema mode. |
| `tools/list` clients depend on action names in descriptions | Keep action enum in schema initially; shorten descriptions only after client smoke tests. |
| Structured result migration breaks source users | Keep legacy text fields through a deprecation window; add tests for both structured and text compatibility. |
| Module helper macro hides custom behavior | Use helper only for simple modules; Mesh/UI/Index can use explicit registration lists. |
| Build.cs helper is awkward under UBT | Prototype in one optional module first; if sharing C# is brittle, use a documented copy-minimized pattern and static checks. |

---

## 10. First Implementation Slice

The first code slice should be small and high-value:

1. Add `monolith.find`.
2. Add `monolith.discover` `mode=summary|actions|schema`, `action`, `query`, `limit`, and `fields`.
3. Add tests for those response shapes.
4. Update `API_REFERENCE.md` and `SPEC_MonolithCore.md`.
5. Do not migrate domain result payloads in the same slice.

This slice gives agents a fast, unambiguous route to the right action while keeping behavior risk low.
