# Monolith — MonolithCore Module

**Parent:** [SPEC_CORE.md](../SPEC_CORE.md)
**Engine:** Unreal Engine 5.7+
**Version:** 0.21.2 (Beta)

---

## MonolithCore

**Dependencies:** Core, CoreUObject, Engine, HTTP, HTTPServer, Json, JsonUtilities, Slate, SlateCore, DeveloperSettings, Projects, AssetRegistry, EditorSubsystem, UnrealEd

### Classes

| Class | Responsibility |
|-------|---------------|
| `FMonolithCoreModule` | IModuleInterface. Starts HTTP server, registers core tools, owns `TUniquePtr<FMonolithHttpServer>` |
| `FMonolithHttpServer` | Embedded MCP HTTP server. JSON-RPC 2.0 dispatch over HTTP. Fully stateless (no session tracking). `tools/list` response embeds per-action param schemas in the `params` property description (`*name(type)` format, `*` = required) so AI clients see param names without calling `monolith_discover` first |
| `FMonolithToolRegistry` | Central singleton action registry. `TMap<FString, FRegisteredAction>` keyed by "namespace.action". Thread-safe — releases lock before executing handlers. Validates required params from schema before dispatch (skips `asset_path` — `GetAssetPath()` handles aliases itself). Returns descriptive error listing missing + provided keys. `GetCatalogFingerprint()` canonicalizes sorted action metadata, JSON schemas, and MCP annotations into the stable `sha256:<16 hex>` catalog version used by conditional discovery |
| `FMonolithJsonUtils` | Static JSON-RPC 2.0 helpers. Standard error codes (-32700 through -32603). Declares `LogMonolith` category |
| `FMonolithAssetUtils` | Asset loading with 4-tier fallback: StaticLoadObject(resolved) -> PackageName.ObjectName -> FindObject+_C suffix -> ForEachObjectWithPackage |
| `UMonolithSettings` | UDeveloperSettings (config=Monolith). ServerPort, bAutoUpdateEnabled, DatabasePathOverride, EngineSourceDBPathOverride, EngineSourcePath, 10 module enable toggles + `bEnableProceduralTownGen` (experimental, default false) (functional — checked at registration time), LogVerbosity. Settings UI customized via `FMonolithSettingsCustomization` (IDetailCustomization) with re-index buttons for project and source databases |
| `UMonolithUpdateSubsystem` | UEditorSubsystem. GitHub Releases auto-updater. Shows dialog window with full release notes on update detection. Downloads zip, cross-platform extraction (PowerShell on Windows, unzip on Mac/Linux). Stages to Saved/Monolith/Staging/, hot-swaps on editor exit via FCoreDelegates::OnPreExit. Current version always from compiled MONOLITH_VERSION (version.json only stores pending/staging state). Release zips include pre-compiled DLLs. |
| `FMonolithCoreTools` | Registers 5 core actions (`discover`, `status`, `update`, `reindex`, and the separately implemented `guide`) |
| `FBulkFillSpec` | USTRUCT (`BlueprintType`). Input shape for `bulk_fill_query("apply")`: `target_namespace`, `target` (asset path or class), nested JSON `tree`, plus `dry_run` / `strict` toggles. Same shape consumed by every per-namespace adapter registered via `FMonolithBulkFillRegistry`. |
| `FDryRunReport` | USTRUCT (`BlueprintType`). Output shape returned when `dry_run=true`. Carries per-field `FieldWrites`, `SilentDrops` (with reason — covers `FGameplayAttribute`-rename hazard class and other type-mismatch / unknown-field cases), `Clamps` (engine clamp annotations such as the AI `lose_sight_radius >= 1.1 × sight_radius` rule), `Errors`. Promoted to hard error and transaction-rollback when `strict=true`. |
| `FSchemaDescriptor` | USTRUCT (`BlueprintType`). Output shape returned by `describe_query("schema")`. Recursive tree of `FFieldDescriptor` nodes: type name, ImportText sample form, `range_min` / `range_max`, `enum_values`, `conditional_on` discriminators (for tagged-union fields like GE modifier magnitudes), nested struct/array/map/set children. Authoritative source for legal `set_cdo_properties` / `bulk_fill` payload grammar. |
| `FMonolithReflectionWalker` | **Write-side** reflection-walker primitive that drives every bulk-fill adapter. Recursively walks UE 5.7 `FProperty` / `FStructProperty` / `FArrayProperty` / `FMapProperty` / `FSetProperty` / `FObjectProperty` / `FSoftObjectProperty` / `FEnumProperty` against a JSON tree. `InspectTree(...)` returns an `FDryRunReport` without mutation; `ApplyTree(...)` performs the writes under the caller-owned transaction. Single source of truth for ImportText parsing, type coercion, and clamp/enum validation. (Has no FProperty→JSON *reader* — that is the complementary `FMonolithReflectionReader` below.) |
| `FMonolithReflectionReader` | **Read-side** counterpart to `FMonolithReflectionWalker` (new 2026-06-07; `Reflection/MonolithReflectionReader.{h,cpp}`). Static helper `PropertyToJsonValue(FProperty*, const void* ValuePtr)` (+ a `PropertiesToJsonObject(UObject*)` convenience) that serializes an `FProperty` value to JSON — handling `FSoftObjectProperty` / `FSoftClassProperty` / `FStructProperty` recursion / `FArrayProperty` / `FInstancedStruct` / gameplay-tag structs. Pure reflection read (no mutation, no transaction, never dirties a package). **Dedup:** this single implementation replaced two prior copies of the same serializer — the CDO-action copy (`MonolithCDOInternal::PropertyToJsonValue` in `MonolithBlueprintCDOActions.cpp`) and the DataAsset-indexer copy (`FDataAssetIndexer::PropertyToJsonValue` in `MonolithIndex`). Both call sites plus `seed_data_asset`'s `read_back_values` and `get_cdo_properties` now share this one reader. Deliberately a separate file from the write-side walker so the read/write split stays legible. |
| `FMonolithBulkFillRegistry` | String-keyed singleton dispatcher. Per-namespace adapters call `RegisterAdapter(namespace, fn)` from their owning module's `StartupModule`; the central `bulk_fill_query("apply")` / `describe_query("schema")` handlers look up the adapter by `target_namespace` and delegate. **Zero compile-time linkage from `MonolithCore` into adapter modules** — preserves the Issue #30 / #32 hazard class (no hard imports of optional/conditional sibling modules). When a namespace adapter is absent or its module-level `WITH_*` gate is off, the registry returns a typed error rather than silently no-op'ing. |
| `FMonolithDryRunGuard` | RAII helper toggling the dry-run mode on a per-call basis. Adapters opt into the framework's `dry_run:true` preview-without-persist semantics by constructing this guard at the top of `ApplyTree`; on destruction the guard discards any provisional writes if dry-run was set. |

### Helpers

| Symbol | Header | Responsibility |
|--------|--------|---------------|
| `MonolithCore::ValidatePackagePath(const FString&)` | `MonolithPackagePathValidator.h` (inline) | Wraps `FPackageName::IsValidLongPackageName` with an empty-string-on-success / error-msg-on-failure contract. Rejects empty input, double-slash (`//Game/...`), missing `/Game/` root, trailing slash, illegal chars. Added `dv.367` after a fatal `UObjectGlobals.cpp:1012` ensure from a malformed `//Game/...` JSON payload reaching `CreatePackage`. Currently routed at three sites: `HandleCreateWidgetBlueprint` (direct crash site), `MonolithAIInternal::GetOrCreatePackage` (~17 AI callers), `MonolithGASInternal::GetOrCreatePackage` (~6 GAS callers). ~24 of 80 `CreatePackage` call sites guarded; remaining ~56 sites across MonolithBlueprint / MonolithMaterial / MonolithLogicDriver / MonolithUITemplateActions / MonolithCommonUI* / MonolithMesh are follow-up backlog. |

### Actions (5 — namespace: "monolith")

| Action | MCP Tool | Description |
|--------|----------|-------------|
| `discover` | `monolith_discover` | List available tool namespaces and their actions. Optional `namespace` filter. **Per-namespace branch is terse by default** (action name + one-line description; param schemas omitted). Optional params: `detail` (bool, default false — `true` inlines every action's full `params` schema), `verbose` (alias for `detail`), `filter` (case-insensitive substring on name OR full description), `offset`/`limit` (opt-in pagination; `limit=0` = ALL), and `if_version` (compact unchanged-catalog response). See "Terse per-namespace discover" and "Conditional catalog response" below |
| `status` | `monolith_status` | Server health: version, port, action count, engine_version, project_name, plus `catalog_version`, `catalog_action_count`, and `catalog_namespace_count` |
| `update` | `monolith_update` | Check/install updates from GitHub Releases. `action`: "check" or "install" |
| `reindex` | `monolith_reindex` | Trigger project re-index. Defaults to incremental (hash-based delta); pass `force=true` for full wipe-and-rebuild (via reflection to MonolithIndex, no hard dependency) |
| `guide` | `monolith_guide` | Serve the section-keyed editorial onboarding and cross-namespace workflow guide with a live registry overlay |

#### Terse per-namespace discover

`monolith_discover(namespace)` is **terse by default**: for each action it returns `action` (name) + a one-line `description` only. The full per-action `params` JSON-Schema is NOT emitted by default — fetch a single action's schema with `describe_query action_schema` (the lazy-fetch target, ~54 tokens) or inline every action's schema with `detail=true`. Terse mode cuts per-namespace discover payload by ≥70% vs the pre-change shape (the win is dropping the eager `params` object, not truncating the action list).

**One-line description trim (terse only).** Each `description` is trimmed to its first sentence (sentence terminator at index ≥25 followed by a space or end-of-string), else hard-capped at 150 chars on a word boundary, with an ASCII `"..."` suffix appended when trimmed; already-short descriptions are returned verbatim (no suffix). The FULL untrimmed description is preserved in detail mode and via `describe_query action_schema`.

**Optional params:**

| Param | Type | Default | Meaning |
|-------|------|---------|---------|
| `detail` | bool | `false` (terse) | `true` inlines every action's full `params` schema — reproduces the pre-change response shape byte-for-byte (`action`/`description`/`category`/`params` per action). Canonical flag. |
| `verbose` | bool | unset | Accepted ALIAS for `detail` (read only when `detail` is unset). `verbose=true` == `detail=true`. |
| `filter` | string | — | Case-insensitive substring matched against the action name OR the FULL description. Applied after any `category` filter, before pagination. |
| `if_version` | string | — | Version from a previously cached `status`/`discover` result. If it equals the live catalog version, return the compact unchanged response instead of the requested full listing. |
| `offset` | int | `0` | Opt-in pagination start, clamped to `[0, total]`. Only meaningful with `limit > 0`. |
| `limit` | int | `0` (= ALL) | `0` = no cap (the COMPLETE post-filter action list — no action hidden). Any `limit > 0` clamps to `[0, total]`. Pagination is purely OPT-IN. |

**Top-level response fields:** `catalog_version` (every normal response); `total` (namespace-filtered responses; post-filter count); `next_offset` (only when a positive `limit` was supplied AND more remain); `schema_hint` (terse only). The `schema_hint` string is: `Param schemas omitted. Call describe_query(action_schema, target_namespace="<ns>", target_action="<name>") for one action's full schema, or pass detail=true to inline all.`

Apart from the additive top-level `catalog_version`, the full `discover()` (no namespace) and namespace-filtered legacy shapes are unchanged when callers omit `if_version`. `describe_query action_schema` remains the lazy-fetch target for one action's full schema.

#### Conditional catalog response

The registry fingerprint covers sorted `namespace.action` keys, descriptions, categories, canonicalized parameter schemas, per-action MCP annotations, and namespace-dispatcher annotations. Registration order and `TMap`/JSON-object iteration order therefore cannot create a false catalog change. Registering, unregistering, or changing any discovery/tool-contract metadata does change the version.

When a non-empty `if_version` exactly matches the live fingerprint, `monolith_discover` returns:

```json
{
  "status": "unchanged",
  "catalog_version": "sha256:0123456789abcdef",
  "total_actions": 1400,
  "namespaces": ["animation", "blueprint", "material"]
}
```

The namespace names are sorted and the response is bounded below 1 KiB for the standard plugin surface. A stale or omitted version follows the normal discovery path and reports the current `catalog_version`. A non-string `if_version` fails with JSON-RPC invalid params (`-32602`) rather than silently behaving as an uncached request.

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
