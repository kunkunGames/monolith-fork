# Monolith 0.15.0 Compact Merge Audit Spec

| Field | Value |
|-------|-------|
| Status | Implemented - compact merge contract verified |
| Owner | Monolith |
| Scope | P4 CL 448; git range `a14b72a26b8ab4506655701d8c334d89eef79df0..9063f18da54e6ed531466c7ddae16a6206f03889` |
| Date | 2026-06-10 |
| Goal | Record which 0.15.0 merge surfaces are retained, compacted, deferred, or removed before submitting CL 448. |
| Non-goal | This document does not replace live `monolith_find` / `monolith_discover` / `describe` schemas. It defines the retained compact merge shape and verification gates. |

---

## 1. Objective

The 0.15.0 merge brings useful capabilities, but several additions overlap with existing Monolith-native systems or introduce parallel implementations that would raise maintenance cost if submitted as-is.

The compact merge target is:

1. Keep the native Monolith architecture: central action registry, discoverable schemas, domain-owned handlers, optional-dependency gates, and explicit editor transactions.
2. Keep high-value workflows that collapse many fragile MCP calls into one deterministic call.
3. Remove, gate, or explicitly mark surfaces that only wrap another action without committing work, return stub success, or duplicate a mature builder path.
4. Extract repeated adapter mechanics before adding more per-domain bulk-fill code.
5. Preserve role separation between `monolith_find`, `monolith_discover`, `describe`, and `monolith_guide` so routing, schema, write-shape, and workflow prose do not collapse into one overloaded tool.
6. Keep namespace, module, and action ownership cohesive: modules own domain code, namespaces expose coherent public verbs, and actions do one verified job with reusable implementation behind them.
7. When several implementation paths are possible, choose the highest-ROI path: the one that removes repeated agent round-trips, prevents common failures, or consolidates duplicated code with the least new public surface.
8. Treat P0 gates as submit blockers, not follow-up notes.
9. Submit CL 448 only after docs, live registry counts, and verification evidence agree.

---

## 2. Evidence Baseline

| Evidence | Current finding | Impact |
|----------|-----------------|--------|
| P4 CL | `p4 describe -s 448` identifies the pending changelist as `Monolith 0.15.0 upgrade` with wide edits across Core, Blueprint, UI, Niagara, domain adapters, docs, tools, and binaries. | The merge is not a narrow patch; it needs an architectural audit before submit. |
| Git range | `a14b72a26..9063f18d` includes 123 changed files under `Source`, `Docs`, `Tools`, `.jules`, `README.md`, `CHANGELOG.md`, and `Monolith.uplugin`, with about 17k insertions. | Review must classify features by capability family instead of by file only. |
| Build history | A fresh `<Project>Editor Win64 Development` UBT build passed on 2026-06-10 after the compact-merge code changes. | Code compiles after curation. |
| Source-index health | `Binaries\monolith_query.exe source health` is readable; the remaining warning is the existing orphan-reference warning, with CRG parity OK. | Source-index parity no longer blocks this spec; orphan cleanup is outside this compact-merge pass. |
| Existing docs | `Docs/SPEC_CORE.md`, `Docs/API_REFERENCE.md`, and this verification note now distinguish static in-tree reference counts from the live full-project 2026-06-10 registry snapshot. | Exact action/schema truth remains runtime `monolith_find`, `monolith_discover`, and `describe`; docs record the verified snapshot and deliberate conditional deltas. |

### 2.1 Source Audit Coverage Matrix

The audit classified every source module touched by `a14b72a26..9063f18d`. Counts below are changed files under `Source/<Module>` in that range; binary DLLs, docs, `.jules`, `README.md`, `CHANGELOG.md`, `Monolith.uplugin`, and `Tools/MonolithQuery/monolith_query.cpp` were reviewed as support surfaces rather than runtime source modules.

| Source module | Changed files | Main audited surfaces | Compact merge conclusion |
|---------------|---------------|-----------------------|--------------------------|
| MonolithCore | 17 | Bulk-fill actions, describe actions, registry, reflection walker, dry-run guard, guide tool, HTTP/tool result integration. | Keep core framework; harden callback locking, dry-run contract, shared adapter helpers, and docs/count truth. |
| MonolithBlueprint | 19 | CDO wrappers, DataAsset seed, bulk-fill adapter, DataTable/CurveTable/StringTable packs, node/property access changes, compile/error surfacing. | Keep explicit dataset actions and reflection-backed aliases; collapse CDO/DataAsset wrappers onto one backend. |
| MonolithUI | 16 | UI spec builder, CommonUI scaffolders/actions, navigation, token binding, registry actions, style service, UI bulk-fill adapter. | Keep UI spec builder as source of truth; keep concrete CommonUI fixes; gate or mark stub/audit-only paths. |
| MonolithNiagara | 4 | Niagara action expansion, parameter/curve/DI/scalability/user-parameter behavior, bulk-fill adapter, module registration. | Keep native graph/editor operations with targeted smoke tests for parameter-map and GUID instability. |
| MonolithAI | 4 | AI advanced action changes, AI bulk-fill adapter, module registration. | Keep adapter pattern if it uses shared helpers and preserves optional-gated domain semantics. |
| MonolithAnimation | 6 | Animation actions/layout/ABP write changes, animation bulk-fill adapter, module registration. | Keep verified PoseSearch/data-shaped writes; keep audit-only notify/template paths out of committed-success claims. |
| MonolithAudio | 4 | Audio batch changes, audio bulk-fill adapter, module registration. | Keep domain data writes; keep MetaSound paths behind existing optional dependency policy. |
| MonolithMaterial | 3 | Material module registration, MIC parameter bulk writes, `BuildMaterialGraph` audit wrapper. | Keep MIC parameter writes; gate or rename graph-build audit wrapper until it commits through `material::build_material_graph`. |
| MonolithMesh | 5 | Build/module changes, tech-art actions, mesh bulk-fill adapter. | Keep only verified data-shaped mesh operations; avoid growing a second geometry authoring backend. |
| MonolithGAS | 4 | Build gate, GAS module owner registration, AttributeInit DataTable adapter, optional-dependency stub branch. | Keep AttributeInit semantics and clean optional errors; extract repeated class/row helpers if reused. |
| MonolithComboGraph | 4 | Build gate, combo graph module, combo bulk-fill adapter. | Keep adapter only for domain-specific combo graph batches; share generic adapter mechanics. |
| MonolithLogicDriver | 4 | Build gate, LogicDriver module, state-machine bulk-fill adapter. | Keep adapter behind existing optional dependency behavior; share generic adapter mechanics. |
| MonolithBABridge | 3 | Build/module bridge changes and formatter integration. | Treat as integration-only; no new MCP surface, so exclude from action-count expansion. |
| MonolithEditor | 2 | Editor action and selection action changes. | Keep if they support verification/workflow ergonomics; no separate compacting issue found. |
| MonolithLevelSequence | 1 | Build file touch. | Treat as support/build-surface drift, not a 0.15.0 bulk-fill capability. |
| MonolithWorldGen | 4 | Build file and mesh worldgen action touches. | Treat separately from 0.15.0 bulk-fill merge; do not let experimental worldgen behavior justify public action-count claims. |

The dedicated bulk-fill adapter sweep found 11 in-tree adapter pairs: AI, Animation, Audio, Blueprint, ComboGraph, GAS, LogicDriver, Material, Mesh, Niagara, and UI. Inventory/sibling adapters mentioned in existing docs are outside this in-tree source audit and should remain documented as sibling/private surfaces unless their source is part of the submitted changelist.

---

## 3. Monolith-Native Compatibility Rules

| Rule | Required behavior | Reject or defer when |
|------|-------------------|----------------------|
| Central registration | Actions register through `FMonolithToolRegistry`; framework write/introspection calls route through `FMonolithBulkFillRegistry`. | A domain introduces an untracked side dispatcher or bypasses registry-owned discoverability. |
| Owner-scoped teardown | Modules that register many actions should use `RegisterOwnedActions` and `UnregisterOwner`; adapters unregister through `FMonolithBulkFillRegistry::UnregisterAdapter`. | Shutdown uses broad namespace deletion that can remove sibling or alias actions unintentionally. |
| Optional dependency discipline | Optional modules keep action availability predictable through compile gates or clean optional-dependency errors. | A build flavor silently loses an advertised action or hard-links an optional marketplace plugin. |
| Explicit editor mutation | Mutating actions validate first, use one transaction per logical batch, call `Modify`, broadcast editor refresh hooks where required, and mark packages dirty. | An action performs partial writes, per-cell transactions, or returns success from a no-op path. |
| Schema first | New generic write paths expose `describe` or action schema data that is machine-usable. | Callers must infer JSON shape from prose only. |
| Index/search boundary | `ProjectIndex.db` and `project.search` are read-optimized discovery/review caches; `describe` owns live writable shape; `bulk_fill` owns mutation. | A write path treats indexed property text as authoritative schema, validation, or current object state. |
| Builders over reflection for authored assets | Durable asset builders remain the source of truth for complex graph/spec creation. Reflection bulk fill is for data-shaped edits, not replacing graph builders. | A bulk-fill adapter forks a second material/UI/Niagara graph builder. |
| Compact API surface | Compatibility aliases may exist, but they must clearly delegate to a single backend and be documented as aliases. | Two public actions implement the same mutation with diverging semantics. |
| Routing/prose separation | `monolith_find` finds actions, `monolith_discover` exposes action schemas, `describe` exposes writable target shapes, and `monolith_guide` explains workflow intent/order/recovery. | `MONOLITH_GUIDE.md` restates action catalogs, duplicates `monolith_find` ranking, or becomes the place agents must search for exact action names. |
| Cohesive namespace/module/action boundaries | A namespace is a public domain contract, a module is the code ownership unit, and an action is a narrow command/query with a verified contract. | A namespace becomes a misc bucket, a module owns unrelated domains, or an action branches into several unrelated workflows by mode flags. |
| ROI-first expansion | Prefer work that collapses many fragile calls, removes repeated backend code, or prevents high-frequency errors. | A low-use action adds public surface, docs, tests, and maintenance without reducing failures or repeated work. |

---

### 3.1 Routing And Workflow Surface Boundaries

| Surface | Owns | Must not own | Example |
|---------|------|--------------|---------|
| `monolith_find` | Task-to-action routing over the live registry. | Cross-namespace recipes, policy prose, or hand-authored workflow education. | "What action handles CommonUI navigation?" -> returns candidate `ui` actions. |
| `monolith_discover` | Exact namespace/action inventory and parameter schemas. | Narrative guidance, multi-step ordering, or ROI judgment. | `monolith_discover("ui")` returns exact action names and params. |
| `describe` | Read-only writable-shape introspection for a target namespace/asset/action. | Asset search or actual mutation. | `describe.schema(target_namespace="blueprint", target="/Game/Data/Item")` returns field paths and types. |
| `monolith_guide` | Editorial workflow intent, ordering, decision matrices, recovery maps, and skill pointers. | Action search, per-namespace catalogs, or schema tables. | "How do I sequence GAS + audio + UI for an ability?" -> points to the recipe and verification calls. |

`MONOLITH_GUIDE.md` should therefore stay short and section-keyed. It may say "use `monolith_find` when the action is unclear" and "use `monolith_discover` before calling an unfamiliar action", but it should not maintain its own ranked action list or duplicate parameter schemas.

### 3.2 Namespace, Module, Action, And Helper Ownership

| Layer | Cohesion rule | Reuse rule |
|-------|---------------|------------|
| Namespace | Public verbs should describe one domain or framework concern, such as `ui`, `blueprint`, `material`, `bulk_fill`, or `describe`. | Cross-namespace behavior goes through shared Core helpers or explicit framework namespaces; do not hide unrelated domain verbs inside a convenient namespace. |
| Module | Source ownership follows Unreal module boundaries and optional dependencies. | Shared code may move down into `MonolithCore` only when it has no domain dependency; otherwise keep it in the owning module or a narrow internal helper. |
| Action | One action should perform one command/query with clear params, dry-run/write behavior, and verification shape. | Compatibility aliases are acceptable only when they delegate to the same backend and do not fork semantics. |
| Helper | Helpers own repeated mechanics, not domain policy. | Extract JSON parsing, report construction, transaction cradles, descriptor roots, and callback dispatch mechanics; keep material/UI/GAS/Niagara rules in their modules. |

### 3.3 ROI Selection Rule

When multiple approaches can solve the same workflow, choose in this order:

1. High ROI: a durable builder or batch action that replaces many fragile sequential calls and has clear verification. Example: `build_ui_from_spec` should remain the UI construction backend; menu scaffolders become presets or delegates.
2. Medium ROI: a domain-specific action that prevents a common engine/editor failure. Example: DataTable row rename/export/import actions are worth keeping because they own editor refresh and row identity semantics.
3. Low ROI: a thin wrapper that only echoes advice, returns a success-like placeholder status, or asks the caller to run another action. Example: Material `BuildMaterialGraph` inside bulk fill is not worth public write-surface status until it actually commits through `material::build_material_graph`.
4. Reject: a broad mode-flag action that combines unrelated workflows to avoid creating the right helper or namespace.

### 3.4 ProjectIndex, Describe, And Bulk-Fill Boundary

`ProjectIndex.db` overlaps with bulk fill and reflection only at the level of discoverable property text. It must not become the write contract.

| Surface | Owns | Must not own |
|---------|------|--------------|
| `project.search` / `ProjectIndex.db` | Read-optimized asset, dependency, graph/content, and supplemental value discovery. Results may be stale until indexing catches up. | Writable schema, validation rules, transaction policy, or current live object truth. |
| `describe.schema` / `describe.action_schema` | Live read-only schema and writable-shape introspection for actions, namespaces, and targets. | Asset/content search, dependency review, or mutation. |
| `bulk_fill.apply` / domain write actions | Validated mutation with dry-run/write reporting and editor-safe transactions. | Search ranking, dependency review, or deriving writable fields from indexed text alone. |

If an implementation needs to jump from search to mutation, the sequence is: `project.search` to find candidates, `describe` to inspect the live writable shape, then `bulk_fill.apply` or a domain action to mutate. Skipping the `describe` step is only allowed when the target action has its own explicit schema and validation path.

---

## 4. Capability Inventory And Merge Decision

| Capability family | Files / surfaces | Decision | Rationale |
|-------------------|------------------|----------|-----------|
| Core bulk fill framework | `MonolithBulkFillActions`, `MonolithBulkFillRegistry`, `MonolithBulkFillTypes`, `bulk_fill.apply`, `bulk_fill.list_namespaces` | Keep and harden | A central JSON-tree write dispatcher is Monolith-native and avoids per-namespace MCP tool sprawl. |
| Describe framework | `describe.schema`, `describe.list_targets`, `describe.action_schema`, `FSchemaDescriptor` | Keep | Schema introspection is the correct companion to generic write APIs. `action_schema` also reinforces registry truth. |
| Reflection walker | `FMonolithReflectionWalker`, `FBulkFillSpec`, `FDryRunReport` | Keep as shared primitive after test hardening | Existing CDO, DataAsset, DataTable, and adapter surfaces need one canonical reflection writer and report format. Existing reflection automation bodies are still placeholder-only, so write/no-side-effect coverage is a P0 prerequisite before broader reuse claims. |
| BulkFillRegistry callback dispatch | `DispatchBulkFill`, `DispatchDescribe`, `DispatchListTargets` | Keep, but fix before submit | Current dispatch invokes external adapter callbacks while holding the registry lock. Snapshot the callback under lock, release the lock, then invoke. |
| Dry-run guard | `FMonolithDryRunGuard` | Keep after contract cleanup | Current guard reads flags and serializes reports; rollback behavior is adapter transaction responsibility. Docs and code comments must not imply automatic write rollback unless implemented. |
| Domain bulk-fill adapters | AI, Animation, Audio, Blueprint, ComboGraph, GAS, LogicDriver, Material, Mesh, Niagara, UI adapters | Keep a compact tier | The adapter pattern is useful, but repeated helpers must move to shared Core helpers before more domains are added. |
| Blueprint CDO/DataAsset bulk wrappers | `set_cdo_properties`, `describe_cdo_schema`, `seed_data_asset`, blueprint bulk-fill adapter | Keep as aliases over one backend | These are useful compatibility and ergonomics surfaces if they share `FMonolithReflectionWalker` and do not fork CDO semantics. |
| Blueprint DataTable/CurveTable/StringTable pack | `MonolithBlueprintDataTableActions`, `MonolithBlueprintCurveTableActions`, `MonolithBlueprintStringTableActions` | Keep | These are not generic reflection duplicates; they own table-specific row semantics, editor refresh broadcasts, import/export, rename, duplicate, and curve-mode rules. |
| UI spec builder | `build_ui_from_spec`, `dump_ui_spec`, `dump_ui_spec_schema` | Keep as UI source of truth | This is the native transactional UI construction path. Other scaffolders should generate or delegate to this spec shape. |
| CommonUI scaffolders | `scaffold_main_menu`, `scaffold_settings_panel_with_tabs`, `scaffold_pause_menu`, `build_menu_from_spec` | Keep only as presets or thin orchestration | They overlap with the spec builder. Submission target: they emit canonical UI specs or call the same builder path instead of maintaining a second widget construction stack. |
| UI bulk fill | `MonolithUIBulkFillAdapter`, DataTable row bulk writes, widget property edits | Keep selected paths, compact implementation | DataTable row batching and widget allowlist writes are valuable. The adapter is large and should share transaction, descriptor, target parsing, and failure-report helpers. |
| CommonUI gap-closure actions | Navigation, action bar class, Common widget conversion, root reparent, style actions | Keep concrete non-stub actions | These close real editor gaps and preserve CommonUI optional gates. Stub paths must be marked non-commit or removed from public success claims. |
| Token binding / menu aggregation stubs | `apply_token_binding` MVP stub, `build_menu_from_spec` aggregation placeholders, audit-only adapter modes | Remove, gate, or expose as explicit audit-only | Returning success from non-mutating implementation creates false automation confidence. |
| Material bulk-fill adapter | `MICParameters`, `BuildMaterialGraph` audit wrapper | Split keep/defer | Keep MIC parameter writes through editor-only parameter APIs. Defer or rename `BuildMaterialGraph` adapter as audit-only until it actually drives `material::build_material_graph`. |
| GAS adapter | `AttributeInitDataTable`, `WITH_GBA` stub branch, GE descriptor | Keep with shared helper extraction | Attribute init row generation adds domain semantics. Optional-dependency clean errors fit Monolith policy. Repeated class-resolution helpers should move to a shared GAS internal utility if reused. |
| Niagara expansion | User parameter, DI, curve, scalability, rename/update helpers | Keep with verification focus | Niagara actions are native graph/editor operations. They must be verified against parameter-map and GUID instability risks before final docs count is frozen. |
| Monolith guide | `monolith.guide`, `Docs/MONOLITH_GUIDE.md`, offline `monolith_query.exe monolith guide` | Keep one prose source with strict role separation | One markdown source is good only if it stays editorial: recipes, decisions, recovery, and skill pointers. It must not duplicate `monolith_find`, `monolith_discover`, action catalogs, or schemas. In-editor/offline section-key parity is covered by the 2026-06-10 guide automation. |
| API reference regeneration | `Docs/API_REFERENCE.md` | Defer final regeneration until curation | Action counts are already drifting in multiple docs. Regenerate after the retained public surface is final. |
| Binaries | `Binaries/Win64/*.dll` | Treat as release artifacts, not design evidence | Submit only if the release policy for CL 448 requires binaries and they come from the verified build. |
| `.jules` files | `.jules/forge.md`, `.jules/marshal.md` | Review separately | Agent policy docs are not required for runtime 0.15.0 capability merge. |

---

## 5. Duplicate And Unnecessary Function Findings

| Finding | Evidence | Required outcome |
|---------|----------|------------------|
| Adapter helper duplication | Every bulk-fill adapter repeats variants of `MakeResolveFailureReport`, `fill_kind` extraction, unknown-kind errors, `RegisterAdapter`, `UnregisterAdapter`, descriptor roots, and target asset loading. | Add a Core helper layer before adding new adapters or expanding existing ones. |
| Transaction cradle duplication | Blueprint, UI, Material, GAS, DataTable, and CurveTable handlers each hand-roll transaction/modify/dirty/editor-refresh patterns. | Provide shared helpers for common UObject, DataTable, CurveTable, WidgetBlueprint, and MIC mutation patterns where engine-specific broadcasts can still be explicit. |
| Lock held during adapter callback | `FMonolithToolRegistry::ExecuteAction` snapshots handlers before invocation, but `FMonolithBulkFillRegistry` dispatch currently calls adapter functions under the registry lock. | Align registry behavior: copy `TFunction` under lock, unlock, then call. |
| Stub success surfaces | Material `BuildMaterialGraph` adapter audits and instructs callers to run `material_query('build_material_graph')`; UI token binding and menu aggregation have MVP-stub/deferred behavior. | Public action docs must say audit-only or the action should return a non-mutating status that cannot be mistaken for a committed edit. |
| Guide parser duplication | In-editor guide and offline query both parse H2 sections and hold canonical section ordering separately. | Keep one markdown source and add parser parity tests/fixtures, or move the shared parser to code both tools can consume. |
| Guide/action-search overlap risk | `MONOLITH_GUIDE.md` is useful editorial prose, but it sits near `monolith_find`/`monolith_discover` in the `monolith` namespace. | Keep guide prose workflow-only; exact action lookup remains `monolith_find`, exact params remain `monolith_discover`, and writable shapes remain `describe`. |
| Boundary erosion risk | Bulk-fill adapters, UI scaffolders, guide prose, and compatibility aliases can each grow into broad "do everything" surfaces. | Require every retained item to state whether it is a namespace contract, module implementation, action contract, or shared helper. |
| Low-ROI public surface | Stub/audit-only paths and thin wrappers add count, docs, tests, and support cost without reducing real agent failures. | Use the ROI selection rule: ship durable builders and high-frequency error reducers first; gate, rename, or remove low-ROI surfaces. |
| Builder overlap in UI | CommonUI scaffolders and `build_menu_from_spec` create higher-level widget flows that overlap with `build_ui_from_spec`. | Preserve one canonical UI build backend; scaffolders become recipes/presets. |
| CDO wrapper overlap | `set_cdo_property`, `set_cdo_properties`, `seed_data_asset`, and blueprint bulk-fill adapter all touch reflected object data. | Keep wrappers only if they delegate to the same reflection writer and expose consistent dry-run/report semantics. |
| Reflection test gap | `Leviathan.Monolith.Reflection.*` automation entries currently contain placeholder success bodies. | Replace stubs with real write, inspect, nested property, unknown field, and dry-run-no-side-effects assertions before treating reflection as a proven shared write primitive. |
| ProjectIndex/write-schema overlap risk | `ProjectIndex.db` indexes property/default/content values for search, while `describe` and bulk fill also expose property-oriented workflows. | Keep ProjectIndex as discovery/review cache only; every write path must validate against live `describe` data or an explicit action schema. |
| Count and log drift | `SPEC_CORE.md`, `API_REFERENCE.md`, module startup logs, and live discover counts already disagree in documented places. | Freeze retained surface, run live discover, then update counts/log text in the same changelist. |
| `describe.list_targets` weak contract | Registry supports list-target callbacks, but most adapters only register bulk + describe callbacks. | Resolved as optional inventory: callers use `bulk_fill.list_namespaces` and `describe.schema` for adapter support; target lists may report `inventory_supported=false`. |

---

## 6. Compact Target Architecture

The retained design should look like this:

| Layer | Responsibility | Concrete requirement |
|-------|----------------|----------------------|
| Core registry | Own action discovery, execution policy inference, aliases, unknown-param handling, owner teardown. | Keep `RegisterOwnedActions` and `UnregisterOwner`; do not regress local owner-based module shutdown. |
| Routing tools | Own task routing and schema discovery surfaces without editorial bloat. | `monolith_find` and `monolith_discover` remain machine-routing surfaces; `monolith_guide` remains editorial. |
| Bulk-fill registry | Own namespace-to-adapter routing only. | Store adapters, list namespaces, copy callbacks before invocation, and never know domain classes. |
| Bulk-fill shared helpers | Own repeated adapter mechanics. | Add shared helpers for failure reports, `fill_kind` dispatch, top-level descriptor construction, JSON object/array extraction, asset load/cast reports, and report serialization. |
| Reflection walker | Own generic UPROPERTY tree inspection/write semantics. | Keep all generic CDO/DataAsset/DataTable row property writes on `FMonolithReflectionWalker`. |
| Project index | Own read-optimized search, dependency review, and derived CRG/index projections. | Never supply mutation schema or write validation; mutation paths must re-check live object/action schema. |
| Domain adapters | Translate domain vocabulary into Core primitives and editor-safe mutation calls. | Adapters should be small: target resolution, domain validation, call shared helper/walker/native editor API, return `FDryRunReport`. |
| Durable builders | Own graph/widget/asset creation workflows. | Material graph, UI spec, Niagara graph, Sound Cue/MetaSound, and Blueprint graph builders remain explicit action handlers, not reflection-only bulk writes. |
| Docs and guide | Explain workflows without duplicating action schema tables. | `MONOLITH_GUIDE.md` remains editorial; SPEC/API docs remain authoritative for counts and contracts after curation. |
| ROI gate | Own the decision between competing implementations. | Prefer durable builders, high-frequency error reducers, and shared helper extraction before adding new public verbs. |

### 6.1 Proposed Shared Helper Set

| Helper | Purpose |
|--------|---------|
| `FMonolithBulkFillReportUtils` | `MakeResolveFailureReport`, field-write helpers, silent-drop helpers, warning normalization. |
| `FMonolithBulkFillJsonUtils` | Required/optional string/object/array extraction with consistent report errors. |
| `FMonolithBulkFillKindDispatcher` | Map `fill_kind` strings to handler callbacks and produce consistent missing/unknown-kind errors. |
| `FMonolithBulkFillDescriptorUtils` | Top-level namespace descriptor and common union/child descriptor construction. |
| `FMonolithEditorMutationUtils` | Small scoped helpers for `RF_Transactional`, `Modify`, `PostEditChange`, `MarkPackageDirty`, and package/editor refresh hooks where the type supports them. |

These helpers should stay in Core only if they do not pull domain module dependencies into Core. Domain-specific details stay in the domain module.

---

## 7. Required Changes Before CL 448 Submit

| Priority | Required change | Acceptance criteria |
|----------|-----------------|---------------------|
| P0 | Fix `FMonolithBulkFillRegistry` dispatch lock behavior. | Adapter callbacks are copied under `AdapterLock`, lock is released, then callback executes. |
| P0 | Reconcile dry-run wording. | Docs and comments state exactly what `FMonolithDryRunGuard` does today, or code implements rollback semantics before docs claim it. |
| P0 | Replace reflection placeholder tests with real assertions. | `Leviathan.Monolith.Reflection.*` tests prove write behavior, unknown-field reporting, nested/container handling, and dry-run no-side-effects instead of returning placeholder success. |
| P0 | Classify non-mutating adapter/action paths. | Audit-only or stub paths cannot return a response that automation will read as a committed edit. |
| P0 | Enforce ProjectIndex/describe/bulk-fill boundary. | Search results may locate candidates only; write schema and validation come from live `describe` or explicit action schemas, not indexed property text. |
| P0 | Add shared adapter helper layer or reduce duplicate adapters. | Repeated failure report, fill-kind parsing, descriptor root, and transaction cradle logic have one owner before new adapters expand. |
| P0 | Preserve owner-scoped registration. | Modules using local owner registration still call `RegisterOwnedActions` and `UnregisterOwner`; adapter registration remains separate. |
| P0 | Preserve guide/find/discover/describe role separation. | `MONOLITH_GUIDE.md` contains no action catalog or schema table duplication; it points agents to `monolith_find`, `monolith_discover`, and `describe` for those roles. |
| P0 | Document namespace/module/action ownership for retained surfaces. | Each retained capability has one public namespace, one owning module, one narrow action contract, and shared helpers only for repeated mechanics. |
| P0 | Apply ROI triage before adding or keeping public verbs. | High-ROI workflow reducers and failure reducers ship first; low-ROI wrappers, stubs, and audit-only surfaces are gated, renamed, or removed. |
| P0 | Freeze and reconcile action counts. | `monolith_discover`, `Docs/SPEC_CORE.md`, per-module specs, `Docs/API_REFERENCE.md`, and startup log strings agree or document a deliberate conditional delta. |
| P1 | Convert CommonUI scaffolders to spec presets or thin delegates. | Scaffolders do not maintain a second widget construction backend. |
| P1 | Define `describe.list_targets` contract. | Resolved as optional inventory: each retained namespace may implement listing, or may return `inventory_supported=false` / `optional_inventory_not_implemented`; callers must not treat an empty target list as lack of adapter support. |
| P1 | Add guide parser parity coverage. | In-editor `monolith.guide` and offline `monolith_query.exe monolith guide` return the same section keys from `Docs/MONOLITH_GUIDE.md`. |
| P1 | Add targeted smoke assets or fixtures. | At least one dry-run and schema call exists for each retained high-risk adapter family. |
| P2 | Review `.jules` docs and binaries separately. | Non-runtime docs and binaries are either intentionally included in CL 448 or moved to a release-artifact path. |

### 7.1 Recommended Implementation Order

Use this order when turning the spec into code changes:

1. Fix `FMonolithBulkFillRegistry` callback locking first. This removes the highest-risk correctness issue without changing public behavior.
2. Replace reflection automation stubs and clean `FMonolithDryRunGuard` wording. Do not expand reflection-backed write surfaces until dry-run/no-side-effect behavior is proven.
3. Gate, rename, or remove non-mutating public success surfaces, including audit-only Material graph bulk-fill and UI MVP stubs.
4. Lock the ProjectIndex boundary: search can find candidates, but live `describe` or action schemas validate writes.
5. Extract only domain-free Core helper mechanics, then migrate two representative adapters before sweeping all adapters. Use one data-shaped adapter and one editor-object adapter to prove the helper shape.
6. Rebuild, rediscover, and update counts/docs only after the retained public surface is final.

---

## 8. Verification Gates

CL 448 should not be submitted until these gates pass after any code curation:

Latest focused compact-merge contract pass: [../testing/2026-06-10-compact-merge-p0.md](../testing/2026-06-10-compact-merge-p0.md).

### 8.1 2026-06-10 Verification Snapshot

| Gate | Verified result |
|------|-----------------|
| Full C++ build | PASS. `<Project>Editor Win64 Development` built through UBT after the final code changes. |
| Registry health | PASS. Live `monolith_status()` reported version `0.15.0`, 2047 actions, and 45 namespaces in the fully loaded project. |
| MCP tool surface | PASS. `tools/list` exposed 77 MCP tools, including 44 `_query` namespace dispatch tools. |
| Routing boundary smoke | PASS. `monolith_find` routes tasks, `monolith_discover` returns exact schemas, `describe` returns writable shapes, and `monolith_guide` returns editorial workflow prose. |
| Discover schema mode | PASS. `monolith_discover(namespace="material", action="build_material_graph", mode="schema")` returned the exact action schema without unknown-param warnings. |
| Core framework smoke | PASS. `bulk_fill.list_namespaces` returned 11 in-tree adapters; `describe.schema(target_namespace="material")` returned a namespace-level descriptor without requiring `target`; `describe.list_targets(material)` returned optional inventory metadata. |
| Reflection automation | PASS. 5 reflection walker tests succeeded with 0 failures. |
| Adapter and non-mutating smoke | PASS. Material `BuildMaterialGraph` remains audit-only with `would_apply=false`; UI menu placeholder paths return non-mutating statuses. |
| Dataset smoke | PASS. DataTable guard tests had 0 failures; warning-only status is from repeated test action registration messages. |
| UI smoke | PASS. UI path cache and UI spec builder suites had 0 failures after widget-variable GUID reconciliation. |
| ProjectIndex boundary | PASS. `project.search` remains discovery-only; `project health` returned `ok` after CRG cache repair. |
| Guide parity | PASS. In-editor and offline guide surfaces expose the same section keys from `Docs/MONOLITH_GUIDE.md`. |

| Gate | Command or method | Expected result |
|------|-------------------|-----------------|
| Full C++ build | Resolve engine via `BatchFiles\Script\ResolveUnrealEngine.ps1`, then run UBT for `<Project>Editor Win64 Development -Project=<Project>.uproject -WaitMutex -NoHotReloadFromIDE`. | Build succeeds without relying on hard-coded engine paths. |
| Registry health | `monolith_status()` through the configured MCP client, or equivalent live health check. | Server is reachable and reports expected version/action counts. |
| Routing boundary smoke | `monolith_find` for action search, `monolith_discover` for params, `describe.schema` for writable shape, and `monolith.guide(section="decisions")` for prose. | Each surface returns its own kind of information without duplicating another surface's output. |
| Discovery count | `monolith_discover()` and per-namespace discover calls. | The live full-project snapshot is recorded as 2047 actions / 45 namespaces. Static in-tree reference tables either match their curated scope or explicitly defer exact schemas/counts to live discovery. |
| Core framework smoke | `bulk_fill.list_namespaces`, `describe.action_schema`, `describe.schema` for representative namespaces. | Available namespaces and schema payloads match retained adapters; target listing is optional inventory. |
| Reflection automation | Run the `Leviathan.Monolith.Reflection.*` automation tests after replacing placeholder bodies. | Tests assert write behavior, nested/container handling, unknown-field reporting, and dry-run no-side-effects. |
| Adapter dry-run smoke | One `bulk_fill.apply` with `dry_run=true` for Blueprint, UI, Material MIC, GAS AttributeInit when available, Niagara, and one optional-gated adapter. | Dry-run reports intended writes, warnings, and silent drops without mutating assets. |
| ProjectIndex boundary smoke | Use `project.search` to find a candidate, `describe.schema` or `describe.action_schema` to fetch writable shape, then dry-run the write path. | Search provenance is not treated as write schema; mutation validation comes from live schema/action validation. |
| Dataset smoke | DataTable read/schema/set/remove/rename/duplicate/export/import, CurveTable key write, StringTable set/remove. | Editor refresh hooks fire and data remains readable after save/reload. |
| UI smoke | `build_ui_from_spec`, selected CommonUI scaffold or preset path, `compile_widget`, focus/navigation audit. | Widget compiles and generated tree matches the spec/preset intent. |
| Guide parity | `monolith.guide(section=...)` and `Binaries\monolith_query.exe monolith guide --section=...`. | Same section keys and no stale section names. |
| Docs sanity | Re-run doc link/count review after final code shape. | `SPEC_CORE`, per-module specs, API reference, and guide do not claim removed/deferred behavior. |

---

## 9. Open Questions

| Question | Why it matters | Suggested decision |
|----------|----------------|--------------------|
| Should CL 448 include rebuilt binary DLLs? | Binaries in a source changelist obscure code review unless they are release-required artifacts. | Include only after final verified build and release policy confirmation. |
| Should every current adapter ship in 0.15.0? | A broad adapter roster increases public support surface. | Ship adapters with verified high-value workflows; exclude audit-only or stub-only paths from the public commit surface until they return explicit non-mutating status or real writes. |
| Should compatibility aliases stay public? | Aliases reduce migration friction but inflate counts and docs. | Keep aliases only when they delegate to a single backend and are marked as compatibility/ergonomic aliases. |
| Which workflow path wins when several exist? | Without a rule, low-ROI wrappers can survive because they are easy to keep. | Use ROI order: durable builder or batch action first, domain-specific error reducer second, thin wrapper/stub last or not at all. |
| Can ProjectIndex property hits drive bulk-fill directly? | Index values can be stale and are optimized for discovery, not validation. | No. Use ProjectIndex to locate candidates, then require `describe` or explicit action schema validation before mutation. |
| Should `describe.list_targets` be mandatory? | Callers may interpret empty target lists as lack of support. | Resolved: optional inventory. Adapters may return `inventory_supported=false` / `optional_inventory_not_implemented`; adapter support is determined by `bulk_fill.list_namespaces` and `describe.schema`, not by target inventory. |
| Should guide parsing be shared code? | Offline CLI cannot always link UE module code. | Use one markdown source plus parser parity fixtures if shared code is impractical. |

---

## 10. Merge Recommendation

Merge CL 448 after the remaining release-artifact policy checks, not as the original broad merge. The compact code/documentation gates in Section 7 are implemented and verified in [../testing/2026-06-10-compact-merge-p0.md](../testing/2026-06-10-compact-merge-p0.md); binary DLL and `.jules` inclusion remain release-scope decisions, not runtime contract blockers.

The 0.15.0 work contains the right Monolith-native direction: centralized bulk fill, schema-driven describe, richer error surfaces, dataset ergonomics, UI close-the-loop actions, and one editorial guide. This compact pass resolves the blocking risks by extracting shared helper mechanics, making audit-only/stub paths explicitly non-mutating, separating ProjectIndex search from write validation, and recording the live registry snapshot alongside static docs.

The compact shape is:

1. Keep Core bulk fill, describe, reflection walker, guide, and registry improvements.
2. Keep `monolith_find`, `monolith_discover`, `describe`, and `monolith_guide` separated by role: routing, schema, writable shape, and editorial workflow.
3. Keep namespace/module/action boundaries cohesive and move only repeated mechanics into shared helpers.
4. Keep domain actions that add real editor/domain semantics.
5. Collapse wrappers and aliases onto one backend per mutation.
6. Use ROI triage before expanding the public surface: durable workflow reducers and common-failure reducers beat thin wrappers.
7. Remove, gate, or rename audit-only/stub paths unless they commit real work or return an explicitly non-mutating status.
8. Keep `ProjectIndex.db` as discovery/review cache only; validate writes through live `describe` or action schemas.
9. Extract shared adapter helpers before further adapter expansion.
10. Rebuild, rediscover, and reconcile docs before submitting CL 448.
