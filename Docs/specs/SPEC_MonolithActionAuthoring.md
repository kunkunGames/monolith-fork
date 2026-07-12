# Authoring or Modifying a Monolith MCP Action

**Parent:** [SPEC_MonolithCore.md](SPEC_MonolithCore.md)
**Engine:** Unreal Engine 5.7+
**Status:** Accepted authoring guide
**Owner module:** MonolithCore
**Scope:** One canonical, source-cited procedure for adding or changing an action in a `Source/Monolith<Module>` module — registration, parameter schema, handler, execution policy, result shape, tests, `monolith_discover`/schema verification, public-contract stability, and docs/skill sync. Worked end-to-end on the candidate gap `scene.capture_viewport`.

---

## 1. Architecture Orientation (read before editing)

Every action is a `(namespace, action)` pair held in the process-global `FMonolithToolRegistry` singleton, reached via `FMonolithToolRegistry::Get()` ([`MonolithToolRegistry.h:156`](../../Source/MonolithCore/Public/MonolithToolRegistry.h); impl [`MonolithToolRegistry.cpp:449-453`](../../Source/MonolithCore/Private/MonolithToolRegistry.cpp)). Domain modules register their actions at module `StartupModule()`. The HTTP/MCP server and the offline `Binaries/monolith_query.exe` both dispatch through `FMonolithToolRegistry::ExecuteAction` ([`MonolithToolRegistry.cpp:599-1015`](../../Source/MonolithCore/Private/MonolithToolRegistry.cpp)).

A handler is a static function bound through `FMonolithActionHandler`, a one-param delegate returning `FMonolithActionResult` ([`MonolithToolRegistry.h:75`](../../Source/MonolithCore/Public/MonolithToolRegistry.h)):

```cpp
DECLARE_DELEGATE_RetVal_OneParam(FMonolithActionResult, FMonolithActionHandler, const TSharedPtr<FJsonObject>& /* Params */);
```

| Concept | Type | File |
|---------|------|------|
| Registry singleton | `FMonolithToolRegistry` | `MonolithToolRegistry.h:153-302` |
| Handler delegate | `FMonolithActionHandler` | `MonolithToolRegistry.h:75` |
| Action result | `FMonolithActionResult` | `MonolithToolRegistry.h:8-72` |
| Execution policy | `FMonolithActionExecutionPolicy` | `MonolithToolRegistry.h:78-89` |
| Per-action metadata | `FMonolithActionInfo` | `MonolithToolRegistry.h:105-126` |
| Param schema builder | `FParamSchemaBuilder` | `MonolithParamSchema.h:52-254` |
| Dispatch guard | `FMonolithActionExecutionGuard` | `MonolithActionExecutionGuard.cpp` |

The full work order is: locate the module → wire `RegisterAll` → call `RegisterAction` → build the schema → write the handler → pick (or let inference pick) the execution policy → verify dispatch behavior → add tests → verify via discover/schema → sync docs and skills. Sections 2-12 follow that order.

---

## 2. STEP 1 — Locate or create the owning module

Actions live in `Source/Monolith<Module>/Private/Monolith<Area>Actions.cpp`, with a `static RegisterAll()` or `static RegisterActions(FMonolithToolRegistry&)` declared in the paired header (e.g. `Source/MonolithSource/Public/MonolithSourceActions.h` declares `static void RegisterAll()`).

Choose the namespace that already owns the domain. Per `CLAUDE.md` Monolith MCP rule, only create a new `Source/Monolith*` module — with handler, schema, tests, and spec/docs — when no existing namespace fits. Do **not** bolt unrelated verbs into a foreign namespace, and do **not** substitute `editor_query("run_python")` or ad-hoc editor scripting for a missing capability (`CLAUDE.md` Monolith MCP).

---

## 3. STEP 2 — Wire `RegisterAll` at module startup

Two wiring patterns exist; both call `FMonolithToolRegistry::Get()` and both gate on the matching `UMonolithSettings::bEnable<Module>` flag.

### 3a. Pattern A — plain, namespace-scoped shutdown

`FMonolithSourceModule::StartupModule()` guards on `bEnableSource`, calls `FMonolithSourceActions::RegisterAll()`, and `ShutdownModule()` unregisters the whole namespace ([`MonolithSourceModule.cpp:10-23`](../../Source/MonolithSource/Private/MonolithSourceModule.cpp)):

```cpp
void FMonolithSourceModule::StartupModule()
{
    if (!GetDefault<UMonolithSettings>()->bEnableSource) return;
    FMonolithSourceActions::RegisterAll();
    FMonolithSourceContextActions::RegisterAll();
}

void FMonolithSourceModule::ShutdownModule()
{
    FMonolithToolRegistry::Get().UnregisterNamespace(TEXT("source"));
    FMonolithToolRegistry::Get().UnregisterNamespace(TEXT("bridge"));
}
```

Use Pattern A when one module solely owns its namespace(s).

### 3b. Pattern B — owner-tagged, preferred for shared namespaces

`FMonolithImageGenModule::StartupModule()` guards on `bEnableImageGen`, then registers under an owner tag; `ShutdownModule()` removes only that owner's actions ([`MonolithImageGenModule.cpp:9-31`](../../Source/MonolithImageGen/Private/MonolithImageGenModule.cpp)):

```cpp
FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
Registry.RegisterOwnedActions(TEXT("MonolithImageGen"), [](FMonolithToolRegistry& OwnedRegistry)
{
    FMonolithImageGenActions::RegisterActions(OwnedRegistry);
});
// ShutdownModule:
FMonolithToolRegistry::Get().UnregisterOwner(TEXT("MonolithImageGen"));
```

`RegisterOwnedActions` pushes an owner onto a registration stack so each `RegisterAction` call inside the lambda is tagged for selective removal ([`MonolithToolRegistry.cpp:494-520`](../../Source/MonolithCore/Private/MonolithToolRegistry.cpp), tag applied at `:483-486`). Use Pattern B when several modules contribute actions to one namespace, so each can be unregistered independently.

---

## 4. STEP 3 — Call `RegisterAction`

Signature ([`MonolithToolRegistry.h:166-175`](../../Source/MonolithCore/Public/MonolithToolRegistry.h)):

```cpp
void RegisterAction(
    const FString& Namespace,
    const FString& Action,
    const FString& Description,
    const FMonolithActionHandler& Handler,
    const TSharedPtr<FJsonObject>& ParamSchema = nullptr,
    const FString& Category = FString(),
    const FMonolithActionExecutionPolicy& ExecutionPolicy = FMonolithActionExecutionPolicy::DefaultReadOnly(),
    const FMonolithActionSearchMetadata& SearchMetadata = FMonolithActionSearchMetadata());
```

| Arg | Required | Notes |
|-----|----------|-------|
| `Namespace` | yes | The `_query` dispatcher group, e.g. `source`, `scene`, `imagegen`. |
| `Action` | yes | The verb. Name reads with read verbs, writes with write verbs (drives policy inference — see §8). |
| `Description` | yes | One-line human/agent-facing summary. Surfaced in `monolith_discover`. |
| `Handler` | yes | Bind with `FMonolithActionHandler::CreateStatic(&FMonolith<X>Actions::Handle<Y>)`. |
| `ParamSchema` | optional | `FParamSchemaBuilder()....Build()`; `nullptr` or `MakeShared<FJsonObject>()` means no params. |
| `Category` | optional | Sub-group string within the namespace, e.g. `TEXT("Image")`, `TEXT("Test")`. |
| `ExecutionPolicy` | optional | Defaults to `DefaultReadOnly()`; inference overrides it for write-verb names (§8). |
| `SearchMetadata` | optional | `Keywords`/`Aliases`/`Examples` for `monolith.find` ranking ([`MonolithToolRegistry.h:92-102`](../../Source/MonolithCore/Public/MonolithToolRegistry.h)). |

Canonical read-only registration ([`MonolithSourceActions.cpp:293-307`](../../Source/MonolithSource/Private/MonolithSourceActions.cpp)):

```cpp
Registry.RegisterAction(TEXT("source"), TEXT("search_source"),
    TEXT("Full-text search across Unreal Engine source code and shaders. ..."),
    FMonolithActionHandler::CreateStatic(&FMonolithSourceActions::HandleSearchSource),
    FParamSchemaBuilder()
        .Required(TEXT("query"), TEXT("string"), TEXT("Search query"))
        .Optional(TEXT("scope"), TEXT("string"), TEXT("Search scope (all, engine, shaders)"))
        .Optional(TEXT("limit"), TEXT("integer"), TEXT("Max results"), TEXT("50"))
        // ...
        .Build());
```

Registering an existing `(ns, action)` key logs a `Warning` and overwrites it ([`MonolithToolRegistry.cpp:469-472`](../../Source/MonolithCore/Private/MonolithToolRegistry.cpp)) — one action is registered exactly once per process.

---

## 5. STEP 4 — Build the parameter schema with `FParamSchemaBuilder`

`FParamSchemaBuilder` is fluent (each call returns `*this`); terminate with `.Build()`, which returns the `TSharedPtr<FJsonObject>` internal Monolith param schema ([`MonolithParamSchema.h:214-217`](../../Source/MonolithCore/Public/MonolithParamSchema.h)). Each entry stores `{type, description, required, optional default, optional aliases[], optional kind}` via the private `AddParam` ([`MonolithParamSchema.h:223-253`](../../Source/MonolithCore/Public/MonolithParamSchema.h)).

### 5a. Required / Optional

`.Required(name, type, desc)` and `.Required(name, type, desc, {aliases…})` ([`MonolithParamSchema.h:56-99`](../../Source/MonolithCore/Public/MonolithParamSchema.h)). `.Optional(name, type, desc, default="")`, plus alias and no-default overloads ([`MonolithParamSchema.h:102-123`](../../Source/MonolithCore/Public/MonolithParamSchema.h)). Internal `type` is a string: `string`, `integer`, `number`, `bool`/`boolean`, `object`, `array`; pipe-unions like `array|string` or `array|string|object|number` are honored by validation ([`MonolithToolRegistry.cpp:120-134`](../../Source/MonolithCore/Private/MonolithToolRegistry.cpp)); internal `any` means the registry accepts any JSON value. MCP-facing `inputSchema` fields are produced by the private `MonolithMcpSchemaUtils::BuildInputSchema`, which converts pipe-unions into standard JSON Schema `type` arrays, normalizes internal `bool` to JSON Schema `boolean`, expands internal `any` to the full standard JSON Schema type set, and keeps internal markers such as `required`, `aliases`, `kind`, and `_validate_types` out of the exported property schema. A non-empty `default` is emitted into the schema only when provided ([`MonolithParamSchema.h:231-234`](../../Source/MonolithCore/Public/MonolithParamSchema.h)).

### 5b. Constraints

- `.Enum(name, {values…})` attaches an `enum[]` array ([`MonolithParamSchema.h:68-81`](../../Source/MonolithCore/Public/MonolithParamSchema.h)).
- `.Range(name, min, max)` attaches `minimum`/`maximum` ([`MonolithParamSchema.h:83-91`](../../Source/MonolithCore/Public/MonolithParamSchema.h)).
- `.EnableValidation()` sets `_validate_types=true` on the schema ([`MonolithParamSchema.h:62-66`](../../Source/MonolithCore/Public/MonolithParamSchema.h)). **Without it, `Enum`/`Range`/type are advisory only and NOT enforced** — see the `ValidateTypedParams` gate at [`MonolithToolRegistry.cpp:928-955`](../../Source/MonolithCore/Private/MonolithToolRegistry.cpp).

### 5c. Path-kind sugar (Survivor D)

`enum EMonolithParamKind { Other, AssetPath, DiskPath, GameplayTag }` ([`MonolithParamSchema.h:22-28`](../../Source/MonolithCore/Public/MonolithParamSchema.h)). The sugar methods (`RequiredAssetPath`/`OptionalAssetPath`, `RequiredDiskPath`/`OptionalDiskPath`, plus alias and `*WithDefault` variants — [`MonolithParamSchema.h:129-212`](../../Source/MonolithCore/Public/MonolithParamSchema.h)) force `type="string"` and tag a `kind` field (only emitted when non-default).

At dispatch ([`MonolithToolRegistry.cpp:828-891`](../../Source/MonolithCore/Private/MonolithToolRegistry.cpp)):

| Kind | Backslash handling |
|------|--------------------|
| `AssetPath` | `\` rewritten to `/` with a `warnings[]` notification (never silent). |
| `DiskPath` | warns only — never rewrites (a real OS path could legitimately contain backslashes). |
| `Other` / `GameplayTag` | pass through untouched. |

Real usage: `OptionalAssetPathWithDefault` / `OptionalAssetPath` ([`MonolithImageGenActions.cpp:2266-2268`](../../Source/MonolithImageGen/Private/MonolithImageGenActions.cpp)), `OptionalDiskPath` ([`MonolithImageGenActions.cpp:2289-2290`](../../Source/MonolithImageGen/Private/MonolithImageGenActions.cpp)), `RequiredDiskPath` ([`MonolithSourceActions.cpp:337`](../../Source/MonolithSource/Private/MonolithSourceActions.cpp)).

### 5d. Worked schema

```cpp
FParamSchemaBuilder()
    .Required(TEXT("symbol"), TEXT("string"), TEXT("Class name"), { TEXT("class_name") }) // canonical + alias
    .Optional(TEXT("direction"), TEXT("string"), TEXT("up | down | both"), TEXT("both"))
    .Optional(TEXT("depth"), TEXT("integer"), TEXT("Max hierarchy depth"), TEXT("5"))
    .Enum(TEXT("direction"), { TEXT("up"), TEXT("down"), TEXT("both") })
    .Range(TEXT("depth"), 1, 16)
    .EnableValidation()  // makes the Enum + Range above enforced at dispatch
    .Build()
```

---

## 6. STEP 5 — Dispatch validation pipeline (gates run before your handler)

`ExecuteAction` runs these gates in order; your handler runs only after all pass ([`MonolithToolRegistry.cpp:599-1015`](../../Source/MonolithCore/Private/MonolithToolRegistry.cpp)):

1. **Action lookup** — unknown action returns `ErrMethodNotFound` with `FindSimilarActions` "did you mean" suggestions ([`:645-676`](../../Source/MonolithCore/Private/MonolithToolRegistry.cpp)).
2. **Tool-profile gate** — `IsActionAllowed` false returns `ErrInvalidRequest` ([`:680-698`](../../Source/MonolithCore/Private/MonolithToolRegistry.cpp)).
3. **Handler-bound check** — null delegate returns `ErrInternalError` ([`:701-716`](../../Source/MonolithCore/Private/MonolithToolRegistry.cpp)).
4. **K2 alias rewrite** — `ApplyAliases` rewrites alias keys to canonical; supplying both canonical and alias => `ErrInvalidParams` collision ([`:722-740`](../../Source/MonolithCore/Private/MonolithToolRegistry.cpp); impl `MonolithParamSchema` `ApplyAliases` at `:153-214`).
5. **Required-param check** — missing required key => error listing missing + provided keys + alias hints ([`:749-826`](../../Source/MonolithCore/Private/MonolithToolRegistry.cpp)). `asset_path` is skipped here (the handler validates it).
6. **Survivor D path-kind `\`→`/`** — AssetPath rewrite / DiskPath warn ([`:828-891`](../../Source/MonolithCore/Private/MonolithToolRegistry.cpp)).
7. **K3 unknown-key detection** — soft `warnings[]` entry, promoted to `ErrInvalidParams` only when env `STRICT_PARAMS=1` ([`:893-926`](../../Source/MonolithCore/Private/MonolithToolRegistry.cpp); `FindUnknownKeys` / `IsStrictParamsEnabled`).
8. **Typed/range/enum validation** — runs only when `_validate_types` is set ([`:928-955`](../../Source/MonolithCore/Private/MonolithToolRegistry.cpp)).

Each rejection calls `FMonolithActionExecutionGuard::RecordRejectedToolCall`.

---

## 7. STEP 6 — Write the handler

A handler is a static method matching `FMonolithActionHandler`:

```cpp
FMonolithActionResult FMonolith<X>Actions::Handle<Y>(const TSharedPtr<FJsonObject>& Params);
```

Read params with `TryGetStringField` / `TryGetNumberField` / `TryGetBoolField` (source handlers use thin `FMonolithSourceReview::PStr`/`PInt`/`PBool` wrappers with defaults — [`MonolithSourceActions.cpp:546-563`](../../Source/MonolithSource/Private/MonolithSourceActions.cpp)).

Even though the registry pre-checks required params, **handlers still defensively re-validate** and return `-32602` (`ErrInvalidParams`) for empty/malformed values — this is the contract the param-guard tests assert ([`HandleReadSource` at `MonolithSourceActions.cpp:1132-1136, 1159`](../../Source/MonolithSource/Private/MonolithSourceActions.cpp)):

```cpp
FString Symbol;
if (!Params->TryGetStringField(TEXT("symbol"), Symbol) || Symbol.IsEmpty())
{
    return FMonolithActionResult::Error(TEXT("'symbol' parameter is required and must be a string"));
}
// ...
return FMonolithActionResult::Error(TEXT("'max_lines' parameter must be a number"), -32602);
```

### 7a. `FMonolithActionResult` shape and helpers

Fields ([`MonolithToolRegistry.h:8-72`](../../Source/MonolithCore/Public/MonolithToolRegistry.h)): `bSuccess`, `Result` (`TSharedPtr<FJsonObject>`), `ErrorMessage`, `ErrorCode`, plus optional structured slots `RelatedActions[]`, `Hints[]`, `ErrorData` (all empty by default so existing responses stay byte-identical).

| Helper | Effect |
|--------|--------|
| `Success(obj)` | success result |
| `Error(msg, code = -32603)` | error; default `ErrInternalError`. Use `-32602` for invalid params, `-32000` for app errors |
| `.WithHint(s)` / `.WithRelatedAction(s)` / `.WithRelatedActions(arr)` | append follow-up guidance / "did you mean" |
| `.WithErrorData(obj)` / `.WithRetryWith(args)` / `.WithDidYouMean(cands)` | structured error recovery |

Chainable example ([`MonolithSourceActions.cpp:551`](../../Source/MonolithSource/Private/MonolithSourceActions.cpp)):

```cpp
return FMonolithActionResult::Error(TEXT("'symbol' parameter is required"), -32602);
```

JSON-RPC codes: `-32603` `ErrInternalError` (default), `-32602` `ErrInvalidParams` (param-guard tests assert this), `-32000` app/runtime error, `ErrMethodNotFound` for unknown action, `ErrInvalidRequest` for profile-blocked.

### 7b. Result-object conventions

- A `content` array of `{type:"text", text:…}` MCP-style blocks for human-readable output ([`MonolithSourceActions.cpp:1251-1257`](../../Source/MonolithSource/Private/MonolithSourceActions.cpp)), plus structured fields.
- Recovery affordances: `next_actions` (suggested `ns.action` strings), `match_status`, `count`.
- Async/started mutations return `{status:"started", job_id, poll_action, next_actions}`.
- **Mutating asset handlers must put result asset paths in fields the guard recognizes** (`SetStringField("asset_path", …)` — e.g. [`MonolithImageGenActions.cpp:2001, 2905`](../../Source/MonolithImageGen/Private/MonolithImageGenActions.cpp)) so source-control prepare and post-edit validation can find the target (recognized field lists at [`MonolithActionExecutionGuard.cpp:50-101, 278-302`](../../Source/MonolithCore/Private/MonolithActionExecutionGuard.cpp)).

---

## 8. STEP 7 — Choose the execution policy (read-only vs mutating)

Policy struct ([`MonolithToolRegistry.h:78-89`](../../Source/MonolithCore/Public/MonolithToolRegistry.h); `DefaultReadOnly` + `ToJson` at [`MonolithToolRegistry.cpp:394-416`](../../Source/MonolithCore/Private/MonolithToolRegistry.cpp)):

```cpp
struct FMonolithActionExecutionPolicy
{
    FString PolicyId = TEXT("read_only");
    bool bDefaulted = true;
    bool bDirtyPackageTracking = false;
    bool bTransactionWrapping = false;
    bool bPostEditValidation = false;
    bool bEnforced = false;
};
```

Supported policy ids and behavior ([SPEC_MonolithActionExecutionPolicy.md:61-71](SPEC_MonolithActionExecutionPolicy.md)):

| Policy id | Dirty tracking | Transaction | Post-edit validate |
|-----------|----------------|-------------|--------------------|
| `read_only` | No | No | No (fast path; no package scans) |
| `track_dirty_packages` | Yes | No | No |
| `transaction_optional` | Yes | Yes | No |
| `transaction_required` | Yes | Yes | No |
| `post_edit_validate` | Yes | Yes | Yes (validator failure => structured error, no auto-rollback) |

### 8a. Inference — most registrations omit the policy

`InferExecutionPolicy` runs at registration ([`MonolithToolRegistry.cpp:418-447`](../../Source/MonolithCore/Private/MonolithToolRegistry.cpp)). If the requested policy is an implicit default **and** the action name is read-like (`get/list/find/search/read/validate/preview/can/describe/detect/analyze/compare/check/health/status/diff/review/inspect/estimate/explain/query/resolve/is/has` — `IsReadLikeActionName` at [`:50-80`](../../Source/MonolithCore/Private/MonolithToolRegistry.cpp)) it stays `read_only`; otherwise it falls forward to `transaction_optional` (dirty + txn + enforced — `MakeInferredMutationPolicy` at [`:136-146`](../../Source/MonolithCore/Private/MonolithToolRegistry.cpp)). Namespace `policytest` is exempt.

**Rule of thumb:** name reads with read verbs, name writes with write verbs (`create/place/save/import/generate/connect/edit/set`), and pass an explicit policy only when you need `post_edit_validate` or want to override inference. A Blueprint/UMG-mutating action should pass an explicit `post_edit_validate` policy so the built-in compiler validator runs.

---

## 9. STEP 8 — How the policy is enforced (no per-handler work)

`ExecuteAction` constructs `FMonolithCrashBreadcrumb::FScopedCapture` ([`MonolithToolRegistry.cpp:964`](../../Source/MonolithCore/Private/MonolithToolRegistry.cpp)), which calls `FMonolithActionExecutionGuard::Get().BeginAction` **before** the handler and `EndAction` in its destructor ([`MonolithCrashBreadcrumb.cpp:238, 259`](../../Source/MonolithCore/Private/MonolithCrashBreadcrumb.cpp)).

`BeginAction` ([`MonolithActionExecutionGuard.cpp:373-427`](../../Source/MonolithCore/Private/MonolithActionExecutionGuard.cpp)) reads the registered policy, snapshots `/Game` dirty packages if tracking, and runs best-effort source-control prepare on target-like params. If `bTransactionWrapping`, the breadcrumb opens a real `FScopedTransaction` named `Monolith ns.action` ([`MonolithCrashBreadcrumb.cpp:239-246`](../../Source/MonolithCore/Private/MonolithCrashBreadcrumb.cpp)). After the handler, `ExecuteAction` calls `CrashCapture.ApplyPostEditValidation` ([`:1011`](../../Source/MonolithCore/Private/MonolithToolRegistry.cpp)) then `SetOutcome` ([`:1013`](../../Source/MonolithCore/Private/MonolithToolRegistry.cpp)). `EndAction` appends an audit row with `execution_policy` + dirty/transaction/source-control-prepare/post-edit-validation/rollback statuses ([`MonolithActionExecutionGuard.cpp:561-612`](../../Source/MonolithCore/Private/MonolithActionExecutionGuard.cpp)).

### 9a. Source-control prepare (automatic; mutating asset namespaces only)

Gated by `IsAutomaticSourceControlPrepareNamespace` (excludes `asset/bridge/collection/context/monolith/project/source/source_control` — [`MonolithActionExecutionGuard.cpp:1031-1045`](../../Source/MonolithCore/Private/MonolithActionExecutionGuard.cpp)) and `IsAutomaticSourceControlPrepareAction` (read-like / non-asset-write verbs excluded — [`:1047-1089`](../../Source/MonolithCore/Private/MonolithActionExecutionGuard.cpp)). It checks out existing project `.uasset`/`.umap` files referenced by recognized fields before mutation and marks newly-dirtied `/Game` packages for add after, attaching a `source_control_prepare` object ([`SetActionOutcome` at `:500-541`](../../Source/MonolithCore/Private/MonolithActionExecutionGuard.cpp)). An unavailable provider is a non-fatal skip.

### 9b. Post-edit validation (opt-in via `post_edit_validate`)

`RunPostEditValidation` ([`MonolithActionExecutionGuard.cpp:452-498`](../../Source/MonolithCore/Private/MonolithActionExecutionGuard.cpp)) runs a registered validator (`RegisterPostEditValidator` at [`:429-450`](../../Source/MonolithCore/Private/MonolithActionExecutionGuard.cpp)) or the built-in default. The default ([`RunDefaultPostEditValidation` at `:951-1029`](../../Source/MonolithCore/Private/MonolithActionExecutionGuard.cpp)) handles only `blueprint`/`ui` namespaces: it resolves a target from `asset_path/blueprint_path/widget_blueprint/wbp_path/save_path/new_path`, compiles the Blueprint, and fails unless status is `BS_UpToDate`/`BS_UpToDateWithWarnings` with zero errors. **There is no automatic rollback** — on failure the open transaction record is canceled and the action becomes a structured error ([`MonolithCrashBreadcrumb.cpp:314-318`](../../Source/MonolithCore/Private/MonolithCrashBreadcrumb.cpp); SPEC_MonolithActionExecutionPolicy.md:71, 110).

### 9c. Runtime override for local testing

`monolith.set_action_execution_policy` mutates a known action's policy in-process ([`SetActionExecutionPolicy` at `MonolithToolRegistry.cpp:1126-1142`](../../Source/MonolithCore/Private/MonolithToolRegistry.cpp); contract SPEC_MonolithActionExecutionPolicy.md:76-90). The legacy boolean alias `policy.post_edit_validate` is rejected — request validation via `policy_id="post_edit_validate"` + `post_edit_validation=true`.

---

## 10. STEP 9 — Add tests

### 10a. Per-module param-guard automation test

These assert the dispatch contract end-to-end through `FMonolithToolRegistry::Get().ExecuteAction`. Pattern ([`MonolithModelGenParamGuardTests.cpp:10-76`](../../Source/MonolithModelGen/Private/Tests/MonolithModelGenParamGuardTests.cpp)):

```cpp
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithParamGuardModelGenSubmitJobMalformedParamsTest,
    "Monolith.ParamGuard.MonolithModelGen.SubmitJobRejectsMalformedParams",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithParamGuardModelGenSubmitJobMalformedParamsTest::RunTest(const FString& Parameters)
{
    FMonolithModelGenActions::RegisterActions(FMonolithToolRegistry::Get());
    TestTrue(TEXT("action registered"),
        FMonolithToolRegistry::Get().HasAction(TEXT("modelgen"), TEXT("submit_generated_model_job")));

    // empty params / missing-required / empty-required => !bSuccess && ErrorCode == -32602
    TSharedPtr<FJsonObject> EmptyParams = MakeShared<FJsonObject>();
    FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(
        TEXT("modelgen"), TEXT("submit_generated_model_job"), EmptyParams);
    TestFalse(TEXT("empty params should fail"), Result.bSuccess);
    TestEqual(TEXT("invalid params code"), Result.ErrorCode, -32602);

    // ... valid-params case asserts bSuccess and cleans up any created files
    return true;
}
```

Cover: registered, empty params, missing-required, empty-required, invalid-enum (all assert `!bSuccess` and `ErrorCode == -32602`), and a valid-params success path that deletes anything it creates.

### 10b. Registry/catalog test

Catalog behavior is tested in [`MonolithDomainCatalogTests.cpp:20-40, 149`](../../Source/MonolithCore/Private/Tests/MonolithDomainCatalogTests.cpp): register a throwaway `catalogtest` namespace via `RegisterAction(..., FParamSchemaBuilder().Required(...).Build(), TEXT("Test"))`, assert `describe_domain` `action_count`, then `UnregisterNamespace(TEXT("catalogtest"))`. Param-schema mechanics (alias collision, unknown keys, `STRICT_PARAMS`, typed validation, MCP `inputSchema` conversion, and pipe-union JSON Schema export) are unit-tested in `Source/MonolithCore/Private/Tests/MonolithParamSchemaTests.cpp`.

---

## 11. STEP 10 — Verify via discover / schema

The live `monolith_find` / `monolith_discover` catalog is authoritative for action names and schemas (`CLAUDE.md` Monolith MCP). Confirm:

- `monolith_discover("<namespace>")` lists the new action.
- `monolith_discover({namespace, action, mode:"schema"})` (or `describe_query("action_schema", …)`) returns the exact param schema.
- discover / `describe_domain` rows carry the `execution_policy` object (SPEC_MonolithActionExecutionPolicy.md:96).

When the editor/MCP is unavailable, read-only `source`/`project`/`bridge` equivalents run through `Binaries/monolith_query.exe`:

```powershell
Binaries\monolith_query.exe source search_source UObject --limit=5
```

---

## 12. STEP 11 — Sync docs, spec, and skills in the SAME change

Per `CLAUDE.md` Docs + Monolith items 16-17:

| Surface | When | What |
|---------|------|------|
| `Docs/specs/SPEC_<Module>.md` | always | describe the new/changed action: current behavior, params, result, policy |
| `Docs/API_REFERENCE.md` | public C++/Blueprint API change | document the public surface |
| `Docs/SPEC_CORE.md` | module boundary / dependency change | update §2/§3 + action counts |
| `Skills/<skill>/SKILL.md` | always | action table + param signatures (rows shown as `ns_query({action, params:{…}})` — see [`Skills/unreal-cpp/SKILL.md:91-103`](../../Skills/unreal-cpp/SKILL.md); writable actions marked `[w]`, required params marked `*`) |
| `Skills/README.md` | action count changed | per-skill action counts |
| `Scripts/validate_monolith_skills.ps1` | always | run it to validate repository + installed skill roots |

Then run the static checks (`CLAUDE.md` item 16):

```powershell
python Scripts/ci_static_checks.py --config .github/monolith-static-ci.json --github check
```

---

## 13. DO / DON'T

**DO:**

- Keep one action = one `(ns, action)` registered exactly once per process.
- Name reads with read verbs and writes with write verbs so policy inference is correct (§8).
- Defensively re-validate params in the handler and return `-32602` (§7).
- Use path-kind sugar for `/Game` and disk paths (§5c).
- Return result asset paths under recognized field names so SCP/validation find them (§7b, §9a).
- Add aliases and optional params **additively**.

**DON'T (`CLAUDE.md` Monolith item 9):**

- Change public action contracts or JSON parameter schemas without explicit justification — prefer **additive** optional params and aliases over renaming, removing, or retyping existing params; a rename needs an alias on the old name for back-compat (§14).
- Supply both a canonical key and one of its aliases in the same call (collision => `ErrInvalidParams`).
- Rely on `Enum`/`Range` without `.EnableValidation()`.
- Substitute `editor_query("run_python")` for a missing capability — extend the owning `Source/Monolith*` module instead.

---

## 14. MODIFYING an existing action safely (public-contract stability)

`CLAUDE.md` Monolith item 9: routine refactor / performance / hygiene work must **not** change public action contracts or JSON parameter schemas without explicit justification. Concretely:

| Change | Allowed without justification? | Safe path |
|--------|-------------------------------|-----------|
| Add a new optional param | Yes (additive) | `.Optional(...)` with a default |
| Add an alias for an existing param | Yes (additive) | `.Required(name, type, desc, { TEXT("old_name") })` |
| Rename a param | No | keep the old name as an alias; the canonical becomes the new name |
| Remove a param | No | deprecate via docs; keep accepting it (ignored or aliased) until a justified breaking change |
| Retype a param (e.g. `string` → `integer`) | No | add a new param; keep parsing the old |
| Make an optional param required | No | breaking; needs explicit justification |
| Tighten enum/range under `_validate_types` | No (breaking) | broaden, don't narrow, unless justified |

When a rename is genuinely required, register the new canonical name and list the old name in the `{aliases…}` list so existing callers keep working through the K2 alias rewrite (§6 step 4). Never supply both — that is a collision error.

---

## 15. WORKED EXAMPLE A — read-only search action (`source.search_source`)

- **Registration:** [`MonolithSourceActions.cpp:293-307`](../../Source/MonolithSource/Private/MonolithSourceActions.cpp) — `Required(query)` + several `Optional`s with defaults. No explicit policy; `search` is read-like => stays `read_only` (no txn, no scans).
- **Handler:** `HandleSearchSource` follows the `HandleReadSource` model ([`:1124-1259`](../../Source/MonolithSource/Private/MonolithSourceActions.cpp)) — fetch the DB via `GetDB()`, validate `symbol`/`query` non-empty (else `Error`), build a `content[{type:"text",text}]` block plus structured fields, return `Success`.
- **Dispatcher annotation:** the `source_query` dispatcher is annotated read-only + idempotent via `SetDispatcherAnnotations` ([`:522-527`](../../Source/MonolithSource/Private/MonolithSourceActions.cpp)).

---

## 16. WORKED EXAMPLE B — mutating create/save action (`imagegen.generate_image`)

- **Registration:** [`MonolithImageGenActions.cpp:2171-2190`](../../Source/MonolithImageGen/Private/MonolithImageGenActions.cpp) inside `RegisterActions`, wired through `RegisterOwnedActions("MonolithImageGen")` ([`MonolithImageGenModule.cpp:19-22`](../../Source/MonolithImageGen/Private/MonolithImageGenModule.cpp)). `Required(prompt)` + many `Optional`s with defaults (`provider`, `model`, `aspect_ratio="1:1"`, `asset_path=DefaultGeneratedAssetPath`, `overwrite_policy="unique"`, `save="true"`), `Category="Image"`.
- **Policy:** no explicit policy, but the name starts with write verb `generate` => `InferExecutionPolicy` assigns `transaction_optional` (dirty tracking + UE transaction wrapping, enforced).
- **Handler:** imports a `Texture2D` and saves the package, returning `Success` with `SetStringField("asset_path", package path)` ([`:2001, 2905`](../../Source/MonolithImageGen/Private/MonolithImageGenActions.cpp)) so the guard's source-control prepare can mark the new `.uasset` for add. Note `imagegen` is in the SCP-excluded namespace list ([`MonolithActionExecutionGuard.cpp:1031-1045`](../../Source/MonolithCore/Private/MonolithActionExecutionGuard.cpp)) — the asset-import path owns its own SCP.
- **Contrast:** a Blueprint/UMG mutator should register with an explicit `post_edit_validate` policy so the built-in compiler validator runs (§9b).

---

## 17. WORKED EXAMPLE C (end-to-end) — adding `scene.capture_viewport`

This is the highest-value candidate gap from the coverage analysis: capturing the active editor viewport from an arbitrary free camera and returning the image inline as MCP `ImageContent`, so an agent can SEE arbitrary scene framing. Monolith today has `scene.capture_building_views` (6 diagnostic ortho/perspective PNGs), `scene.capture_floor_plan`, and `slate.capture_widget` (window/widget only, explicitly no level-viewport fallback) — none return a free-camera viewport image inline. (Confirm the exact existing roster via `monolith_discover("scene")` when the editor is up; the live MCP was down at authoring time.)

This is a **read-only** action: it observes and renders the scene without mutating any `/Game` package, so policy inference keeps it on the fast path **only if it is named with a read-like verb**. `capture` is not in the read-verb list ([`MonolithToolRegistry.cpp:50-80`](../../Source/MonolithCore/Private/MonolithToolRegistry.cpp)), so inference would otherwise fall it forward to `transaction_optional`. Pass an explicit `DefaultReadOnly()` policy to keep it on the read-only fast path.

### 17a. Registration + schema

```cpp
// In FMonolithMeshDebugViewActions::RegisterActions(Registry). The MonolithScene module uses
// Pattern B (each *Actions class exposes static RegisterActions(FMonolithToolRegistry&), invoked
// from MonolithSceneModule.cpp), NOT RegisterAll(). capture_floor_plan / capture_building_views
// already register here in MonolithMeshDebugViewActions.cpp.
Registry.RegisterAction(
    TEXT("scene"), TEXT("capture_viewport"),
    TEXT("Render the active editor level viewport from an optional free camera and return the image inline as ImageContent for the agent to view. Read-only; does not mutate packages."),
    FMonolithActionHandler::CreateStatic(&FMonolithMeshDebugViewActions::HandleCaptureViewport),
    FParamSchemaBuilder()
        .Optional(TEXT("camera_location"), TEXT("array"), TEXT("Free-camera world location [x,y,z]. Omit to use the current viewport camera."))
        .Optional(TEXT("camera_rotation"), TEXT("array"), TEXT("Free-camera rotation [pitch,yaw,roll]. Omit to use the current viewport camera."))
        .Optional(TEXT("fov"), TEXT("number"), TEXT("Horizontal field of view in degrees for a perspective capture."), TEXT("90"))
        .Optional(TEXT("width"), TEXT("integer"), TEXT("Capture width in pixels."), TEXT("1280"))
        .Optional(TEXT("height"), TEXT("integer"), TEXT("Capture height in pixels."), TEXT("720"))
        .Optional(TEXT("projection"), TEXT("string"), TEXT("perspective or orthographic."), TEXT("perspective"))
        .Optional(TEXT("return_image"), TEXT("bool"), TEXT("Return inline base64 ImageContent in addition to the saved PNG path."), TEXT("true"))
        .OptionalDiskPath(TEXT("output_path"), TEXT("Optional disk path for the PNG. Defaults under Saved/Screenshots."))
        .Enum(TEXT("projection"), { TEXT("perspective"), TEXT("orthographic") })
        .Range(TEXT("fov"), 5, 170)
        .Range(TEXT("width"), 16, 4096)
        .Range(TEXT("height"), 16, 4096)
        .EnableValidation()  // enforce the enum + ranges above
        .Build(),
    TEXT("Capture"),
    FMonolithActionExecutionPolicy::DefaultReadOnly());  // explicit: 'capture' is not a read-verb, keep it read-only
```

Notes: all camera params are optional so the bare call captures the current viewport; `projection`/`fov`/`width`/`height` are constrained and only enforced because of `.EnableValidation()`; `output_path` uses `OptionalDiskPath` so a backslash value warns rather than silently failing.

### 17b. Handler sketch

```cpp
FMonolithActionResult FMonolithMeshDebugViewActions::HandleCaptureViewport(const TSharedPtr<FJsonObject>& Params)
{
    if (!GEditor)
    {
        return FMonolithActionResult::Error(
            TEXT("Editor is not available; capture_viewport requires a live editor viewport."), -32000);
    }

    // Defensive re-validation of constrained params (the registry only enforces
    // these when _validate_types is set; the handler still guards explicitly).
    FString Projection = TEXT("perspective");
    Params->TryGetStringField(TEXT("projection"), Projection);
    if (Projection != TEXT("perspective") && Projection != TEXT("orthographic"))
    {
        return FMonolithActionResult::Error(TEXT("'projection' must be perspective or orthographic"), -32602);
    }

    // 1. Resolve the active level-editor viewport client.
    // 2. If camera_location/camera_rotation supplied, apply them to a free camera; else read the current one.
    // 3. Render to an FImage / render target at width x height.
    // 4. Encode PNG, save under output_path (default Saved/Screenshots/<date>/), check encode success.

    bool bReturnImage = true;
    Params->TryGetBoolField(TEXT("return_image"), bReturnImage);

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("saved_path"), SavedPngPath); // disk path of the PNG

    TArray<TSharedPtr<FJsonValue>> Content;
    if (bReturnImage)
    {
        // MCP ImageContent block: {type:"image", data:<base64>, mimeType:"image/png"}
        TSharedPtr<FJsonObject> Image = MakeShared<FJsonObject>();
        Image->SetStringField(TEXT("type"), TEXT("image"));
        Image->SetStringField(TEXT("data"), PngBase64);
        Image->SetStringField(TEXT("mimeType"), TEXT("image/png"));
        Content.Add(MakeShared<FJsonValueObject>(Image));
    }
    TSharedPtr<FJsonObject> Text = MakeShared<FJsonObject>();
    Text->SetStringField(TEXT("type"), TEXT("text"));
    Text->SetStringField(TEXT("text"), FString::Printf(TEXT("Captured viewport to %s"), *SavedPngPath));
    Content.Add(MakeShared<FJsonValueObject>(Text));
    ResultObj->SetArrayField(TEXT("content"), Content);

    return FMonolithActionResult::Success(ResultObj);
}
```

The handler returns the standard `content[]` array carrying both an MCP `image` block (inline base64 the agent can view) and a `text` block reporting the saved path. Because the policy is `read_only`, no dirty-package scan, transaction, or source-control prepare runs (`BeginAction` skips them — [`MonolithActionExecutionGuard.cpp:387-411`](../../Source/MonolithCore/Private/MonolithActionExecutionGuard.cpp)); the `saved_path` is a disk PNG, not a `/Game` package, so it is correctly outside SCP scope.

### 17c. Tests

Extend the existing `Source/MonolithScene/Private/Tests/MonolithSceneParamGuardTests.cpp` (the MonolithScene module's param-guard test) following §10a:

```cpp
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithParamGuardSceneCaptureViewportTest,
    "Monolith.ParamGuard.MonolithScene.CaptureViewportRejectsMalformedParams",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithParamGuardSceneCaptureViewportTest::RunTest(const FString& Parameters)
{
    FMonolithMeshDebugViewActions::RegisterActions(FMonolithToolRegistry::Get());
    TestTrue(TEXT("action registered"),
        FMonolithToolRegistry::Get().HasAction(TEXT("scene"), TEXT("capture_viewport")));

    // invalid enum / out-of-range, with _validate_types on => -32602
    TSharedPtr<FJsonObject> BadProjection = MakeShared<FJsonObject>();
    BadProjection->SetStringField(TEXT("projection"), TEXT("isometric"));
    FMonolithActionResult R = FMonolithToolRegistry::Get().ExecuteAction(
        TEXT("scene"), TEXT("capture_viewport"), BadProjection);
    TestFalse(TEXT("invalid projection should fail"), R.bSuccess);
    TestEqual(TEXT("invalid params code"), R.ErrorCode, -32602);

    // valid params success path (when a viewport is available) — clean up the PNG it writes.
    return true;
}
```

Also confirm the read-only policy: `GetActionExecutionPolicy("scene","capture_viewport").PolicyId == "read_only"` and `bDirtyPackageTracking == false`.

### 17d. Docs / skill sync (§12)

- `Docs/specs/SPEC_MonolithScene.md` (the `scene` namespace owner): add `capture_viewport` to the action table — params, the inline-`ImageContent` result, and the explicit `read_only` policy.
- `Skills/unreal-scene/SKILL.md`: add the row `scene_query({ action: "capture_viewport", params: { camera_location, camera_rotation, fov, width, height } })`, note the inline image return, and mark the new optional params; bump the namespace action count.
- `Skills/README.md`: bump the `unreal-scene` action count.
- Run `Scripts/validate_monolith_skills.ps1` and `python Scripts/ci_static_checks.py --config .github/monolith-static-ci.json --github check`.

---

## 18. Key file map

| File | Holds |
|------|-------|
| `Source/MonolithCore/Public/MonolithToolRegistry.h` | registry + result + policy + handler types |
| `Source/MonolithCore/Public/MonolithParamSchema.h` | `FParamSchemaBuilder` + `FMonolithParamSchema` helpers |
| `Source/MonolithCore/Private/MonolithToolRegistry.cpp` | `RegisterAction`, `InferExecutionPolicy`, `ExecuteAction` validation pipeline |
| `Source/MonolithCore/Private/MonolithActionExecutionGuard.cpp` | `BeginAction`/`EndAction`/SCP/post-edit validation enforcement |
| `Source/MonolithCore/Private/MonolithCrashBreadcrumb.cpp` | `FScopedCapture` wiring of guard + transaction |
| `Source/MonolithSource/Private/MonolithSourceActions.cpp` + `MonolithSourceModule.cpp` | read-only example + Pattern-A wiring |
| `Source/MonolithImageGen/Private/MonolithImageGenActions.cpp` + `MonolithImageGenModule.cpp` | mutating example + Pattern-B owner wiring |
| `Docs/specs/SPEC_MonolithActionExecutionPolicy.md` | policy contract |
| `Source/MonolithModelGen/Private/Tests/MonolithModelGenParamGuardTests.cpp` | param-guard test exemplar |
| `Source/MonolithCore/Private/Tests/MonolithDomainCatalogTests.cpp` | registry/catalog test exemplar |
