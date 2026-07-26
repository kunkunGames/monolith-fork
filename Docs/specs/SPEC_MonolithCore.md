# Monolith — MonolithCore Module

**Parent:** [SPEC_CORE.md](../SPEC_CORE.md)
**Engine:** Unreal Engine 5.7+
**Version:** 0.21.3 (Beta)

---

## MonolithCore

**Dependencies:** Core, CoreUObject, Engine, HTTP, HTTPServer, Json, JsonUtilities, Slate, SlateCore, DeveloperSettings, Projects, AssetRegistry, EditorSubsystem, UnrealEd

### Classes

| Class | Responsibility |
|-------|---------------|
| `FMonolithCoreModule` | IModuleInterface. Resolves persistent server activation, reconciles external config/policy changes through a one-second settings cache and one-second ticker, activates/deactivates HTTP routes, owns `TUniquePtr<FMonolithHttpServer>`, and removes only the sentinel file written by this process. Sentinel ownership is released only after deletion succeeds or absence is confirmed, so transient delete failures remain retryable |
| `FMonolithHttpServer` | Embedded MCP HTTP server. JSON-RPC 2.0 dispatch over HTTP. Fully stateless (no session tracking). Owns only Monolith route bindings; the underlying UE HTTP listener is process-shared and retained for safe same-port reuse because UE 5.7/5.8 has no public per-port stop API. `tools/list` response embeds per-action param schemas in the `params` property description (`*name(type)` format, `*` = required) so AI clients see param names without calling `monolith_discover` first |
| `FMonolithToolRegistry` | Central singleton action registry. `TMap<FString, FRegisteredAction>` keyed by "namespace.action". Thread-safe — releases lock before executing handlers. Validates required params from schema before dispatch (skips `asset_path` — `GetAssetPath()` handles aliases itself). Returns descriptive error listing missing + provided keys |
| `FMonolithJsonUtils` | Static JSON-RPC 2.0 helpers. Standard error codes (-32700 through -32603). Declares `LogMonolith` category |
| `FMonolithAssetUtils` | Asset loading with 4-tier fallback: StaticLoadObject(resolved) -> PackageName.ObjectName -> FindObject+_C suffix -> ForEachObjectWithPackage |
| `UMonolithSettings` | UDeveloperSettings (config=Monolith). Owns project policy plus the cached project-default/per-user activation resolver. ServerPort, bAutoUpdateEnabled, DatabasePathOverride, EngineSourceDBPathOverride, EngineSourcePath, module enable toggles + `bEnableProceduralTownGen` (experimental, default false), LogVerbosity. Settings UI re-index buttons are disabled while indexing activation or the corresponding hard policy is off |
| `UMonolithUpdateSubsystem` | UEditorSubsystem. GitHub Releases auto-updater. Shows dialog window with full release notes on update detection. Downloads zip, cross-platform extraction (PowerShell on Windows, unzip on Mac/Linux). Stages to Saved/Monolith/Staging/, hot-swaps on editor exit via FCoreDelegates::OnPreExit. Current version always from compiled MONOLITH_VERSION (version.json only stores pending/staging state). Release zips include pre-compiled DLLs. |
| `FMonolithCoreTools` | Registers 4 core actions |
| `FBulkFillSpec` | USTRUCT (`BlueprintType`). Input shape for `bulk_fill_query("apply")`: `target_namespace`, `target` (asset path or class), nested JSON `tree`, plus `dry_run` / `strict` toggles. Same shape consumed by every per-namespace adapter registered via `FMonolithBulkFillRegistry`. |
| `FDryRunReport` | USTRUCT (`BlueprintType`). Output shape returned when `dry_run=true`. Carries per-field `FieldWrites`, `SilentDrops` (with reason — covers `FGameplayAttribute`-rename hazard class and other type-mismatch / unknown-field cases), `Clamps` (engine clamp annotations such as the AI `lose_sight_radius >= 1.1 × sight_radius` rule), `Errors`. Promoted to hard error and transaction-rollback when `strict=true`. |
| `FSchemaDescriptor` | USTRUCT (`BlueprintType`). Output shape returned by `describe_query("schema")`. Recursive tree of `FFieldDescriptor` nodes: type name, ImportText sample form, `range_min` / `range_max`, `enum_values`, `conditional_on` discriminators (for tagged-union fields like GE modifier magnitudes), nested struct/array/map/set children. Authoritative source for legal `set_cdo_properties` / `bulk_fill` payload grammar. |
| `FMonolithReflectionWalker` | **Write-side** reflection-walker primitive that drives every bulk-fill adapter. Recursively walks UE 5.7 `FProperty` / `FStructProperty` / `FArrayProperty` / `FMapProperty` / `FSetProperty` / `FObjectProperty` / `FSoftObjectProperty` / `FEnumProperty` against a JSON tree. `InspectTree(...)` returns an `FDryRunReport` without mutation; `ApplyTree(...)` performs the writes under the caller-owned transaction. Single source of truth for ImportText parsing, type coercion, and clamp/enum validation. (Has no FProperty→JSON *reader* — that is the complementary `FMonolithReflectionReader` below.) |
| `FMonolithReflectionReader` | **Read-side** counterpart to `FMonolithReflectionWalker` (new 2026-06-07; `Reflection/MonolithReflectionReader.{h,cpp}`). Static helper `PropertyToJsonValue(FProperty*, const void* ValuePtr)` (+ a `PropertiesToJsonObject(UObject*)` convenience) that serializes an `FProperty` value to JSON — handling `FSoftObjectProperty` / `FSoftClassProperty` / `FStructProperty` recursion / `FArrayProperty` / `FInstancedStruct` / gameplay-tag structs. Pure reflection read (no mutation, no transaction, never dirties a package). **Dedup:** this single implementation replaced two prior copies of the same serializer — the CDO-action copy (`MonolithCDOInternal::PropertyToJsonValue` in `MonolithBlueprintCDOActions.cpp`) and the DataAsset-indexer copy (`FDataAssetIndexer::PropertyToJsonValue` in `MonolithIndex`). Both call sites plus `seed_data_asset`'s `read_back_values` and `get_cdo_properties` now share this one reader. Deliberately a separate file from the write-side walker so the read/write split stays legible. |
| `FMonolithBulkFillRegistry` | String-keyed singleton dispatcher. Per-namespace adapters call `RegisterAdapter(namespace, fn)` from their owning module's `StartupModule`; the central `bulk_fill_query("apply")` / `describe_query("schema")` handlers look up the adapter by `target_namespace` and delegate. **Zero compile-time linkage from `MonolithCore` into adapter modules** — preserves the Issue #30 / #32 hazard class (no hard imports of optional/conditional sibling modules). When a namespace adapter is absent or its module-level `WITH_*` gate is off, the registry returns a typed error rather than silently no-op'ing. |
| `FMonolithDryRunGuard` | RAII helper toggling the dry-run mode on a per-call basis. Adapters opt into the framework's `dry_run:true` preview-without-persist semantics by constructing this guard at the top of `ApplyTree`; on destruction the guard discards any provisional writes if dry-run was set. |

### Persistent service activation

| Command | Immediate effect | Persistent effect |
|---------|------------------|-------------------|
| `Monolith.StartServer` | Activates routes on `ServerPort` if policy allows and the port is not already owned; an occupied pre-bind port fails closed and never creates this process's sentinel | Writes `ServerEnabled=True` to generated user `Monolith.ini` |
| `Monolith.StopServer` | Unbinds Monolith routes and removes this process's sentinel without stopping unrelated UE HTTP listeners | Writes `ServerEnabled=False` |
| `Monolith.Restart` | Rebinds or recovers running/activated routes on the retained same-port transport | None; refuses when server activation is off |
| `Monolith.StartIndexing` | Enables source/asset hooks, explicitly permits a missing source-DB bootstrap, and starts each subsystem's cheapest correct catch-up | Writes `IndexingEnabled=True` |
| `Monolith.StopIndexing` | Removes queued/live writer hooks; active jobs drain safely | Writes `IndexingEnabled=False` |

The two user keys are independent and stored under `[Monolith.UserActivation]` in `Saved/Config/<Platform>/Monolith.ini`. Project defaults are `bServerEnabledByDefault` and `bIndexingEnabledByDefault` in `Config/DefaultMonolith.ini`. Hard policy gates always win, malformed explicit values fail closed, and only a one-time migration reads `Saved/Monolith/Activation.ini`. Server activation is a desired state: a one-second settings cache and one-second core ticker re-resolve external edits and policy changes within two seconds worst-case, stopping or starting routes on transition. When a disk edit is accepted, Monolith reloads the config hierarchy and reconciles only the two effective activation keys into the existing merged `GConfig` file. Removed activation overrides therefore fall back through the normal hierarchy, stale dirty activation values cannot overwrite the accepted disk state, and unrelated inherited or pending settings are preserved. If an allowed Start cannot bind, the persisted choice remains enabled for retry on a later launch, but the failed process does not claim active route or sentinel ownership. Rejected post-bind probes unbind Monolith routes and retain the UE-owned router for same-port retry; they never call process-wide `StopAllListeners()`. A live `ServerPort` change is rejected before active routes or the owned sentinel are changed, with restart guidance because UE exposes no safe per-port listener teardown. An inherited indexing default keeps a healthy source DB current but does not launch the expensive first engine-wide source bootstrap; the explicit Start command does.

### Helpers

| Symbol | Header | Responsibility |
|--------|--------|---------------|
| `MonolithCore::ValidatePackagePath(const FString&)` | `MonolithPackagePathValidator.h` (inline) | Wraps `FPackageName::IsValidLongPackageName` with an empty-string-on-success / error-msg-on-failure contract. Rejects empty input, double-slash (`//Game/...`), missing `/Game/` root, trailing slash, illegal chars. Added `dv.367` after a fatal `UObjectGlobals.cpp:1012` ensure from a malformed `//Game/...` JSON payload reaching `CreatePackage`. Routing is incremental and module-keyed. Current owners: MonolithUI (`HandleCreateWidgetBlueprint`, the original crash site), MonolithAI (`MonolithAIInternal::GetOrCreatePackage`), MonolithGAS (`MonolithGASInternal::GetOrCreatePackage`); MonolithBlueprint, MonolithMaterial and MonolithNiagara are being wired now. Grep the Source tree for `ValidatePackagePath` for the current owner list rather than trusting a count here. |

### Actions (4 — namespace: "monolith")

| Action | MCP Tool | Description |
|--------|----------|-------------|
| `discover` | `monolith_discover` | List available tool namespaces and their actions. Optional `namespace` filter. **Per-namespace branch is terse by default** (action name + one-line description; param schemas omitted). Optional params: `detail` (bool, default false — `true` inlines every action's full `params` schema), `verbose` (alias for `detail`), `filter` (case-insensitive substring on name OR full description), `offset`/`limit` (opt-in pagination; `limit=0` = ALL). See "Terse per-namespace discover" below |
| `status` | `monolith_status` | Server health: version, uptime, port, action count, engine_version, project_name |
| `update` | `monolith_update` | Check/install updates from GitHub Releases. `action`: "check" or "install" |
| `reindex` | `monolith_reindex` | Trigger project re-index. Defaults to incremental (hash-based delta); pass `force=true` for full wipe-and-rebuild (via reflection to MonolithIndex, no hard dependency) |

#### Terse per-namespace discover

`monolith_discover(namespace)` is **terse by default**: for each action it returns `action` (name) + a one-line `description` only. The full per-action `params` JSON-Schema is NOT emitted by default — fetch a single action's schema with `describe_query action_schema` (the lazy-fetch target, ~54 tokens) or inline every action's schema with `detail=true`. Terse mode cuts per-namespace discover payload by ≥70% vs the pre-change shape (the win is dropping the eager `params` object, not truncating the action list).

**One-line description trim (terse only).** Each `description` is trimmed to its first sentence (sentence terminator at index ≥25 followed by a space or end-of-string), else hard-capped at 150 chars on a word boundary, with an ASCII `"..."` suffix appended when trimmed; already-short descriptions are returned verbatim (no suffix). The FULL untrimmed description is preserved in detail mode and via `describe_query action_schema`.

**Optional params:**

| Param | Type | Default | Meaning |
|-------|------|---------|---------|
| `detail` | bool | `false` (terse) | `true` inlines every action's full `params` schema — reproduces the pre-change response shape byte-for-byte (`action`/`description`/`category`/`params` per action). Canonical flag. |
| `verbose` | bool | unset | Accepted ALIAS for `detail` (read only when `detail` is unset). `verbose=true` == `detail=true`. |
| `filter` | string | — | Case-insensitive substring matched against the action name OR the FULL description. Applied after any `category` filter, before pagination. |
| `offset` | int | `0` | Opt-in pagination start, clamped to `[0, total]`. Only meaningful with `limit > 0`. |
| `limit` | int | `0` (= ALL) | `0` = no cap (the COMPLETE post-filter action list — no action hidden). Any `limit > 0` clamps to `[0, total]`. Pagination is purely OPT-IN. |

**Top-level response fields:** `total` (always; post-filter count); `next_offset` (only when a positive `limit` was supplied AND more remain); `schema_hint` (terse only). The `schema_hint` string is: `Param schemas omitted. Call describe_query(action_schema, target_namespace="<ns>", target_action="<name>") for one action's full schema, or pass detail=true to inline all.`

**Unchanged:** the full `discover()` (no namespace) response is untouched. `describe_query action_schema` is the unchanged lazy-fetch target for a single action's full schema.

### Actions (2 — namespace: "bulk_fill")

Framework-level dispatchers for transacted JSON-tree writes against any registered per-namespace adapter. Adapters live in their owning modules (`MonolithBlueprint`, `MonolithGAS`, `MonolithUI`, `MonolithAI`, `MonolithNiagara`, `MonolithMaterial`, `MonolithAudio`, `MonolithMesh`, `MonolithAnimation`, `MonolithLogicDriver`, `MonolithComboGraph`, and an optional sibling-plugin `inventory` adapter); `MonolithCore` owns only the framework primitives and the dispatch surface.

| Action | MCP Tool | Description |
|--------|----------|-------------|
| `apply` | `bulk_fill_query` | Apply a JSON `tree` to `target` under `target_namespace`. Routes to the per-namespace adapter via `FMonolithBulkFillRegistry`. Params: `target_namespace` (string), `target` (asset path or class identifier), `tree` (nested JSON), `dry_run` (bool, default false — returns full `FDryRunReport` without mutation), `strict` (bool, default false — promotes silent drops / clamps / unknown-fields to hard errors and cancels the transaction). Returns the populated `FDryRunReport` either way. |
| `list_namespaces` | `bulk_fill_query` | Enumerate registered adapter namespaces and their `available` flag (false when the owning module's `WITH_*` gate is off — e.g. `gas` adapter `available:false` when `WITH_GBA=0`). No params. Use before `apply` to confirm the target namespace is hot in the current build. |

### Actions (3 — namespace: "describe")

Read-only schema introspection companion to `bulk_fill`. Same 12 adapter namespaces, same registry.

| Action | MCP Tool | Description |
|--------|----------|-------------|
| `schema` | `describe_query` | Return the rich `FSchemaDescriptor` tree for `target` under `target_namespace`. Params: `target_namespace` (string), `target` (asset path or class — empty string returns a root descriptor enumerating every supported `fill_kind` for the namespace). Output includes ImportText sample forms, `range_min` / `range_max`, `enum_values`, `conditional_on` discriminators for tagged-union fields. Authoritative input source for authoring valid `bulk_fill.apply` payloads. |
| `list_targets` | `describe_query` | Enumerate the legal `target` shapes for a namespace's adapter (asset class names, fill_kind discriminators). Params: `target_namespace` (string). |
| `action_schema` | `describe_query` | Return a registered ACTION's param schema (names, types, required, defaults, aliases, descriptions) by `(target_namespace, action)`. Params: `target_namespace` (string), `action` (string). Closes param-name discoverability so callers stop trial-and-erroring param names. |

---
