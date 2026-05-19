# Monolith — MonolithCore Module

**Parent:** [SPEC_CORE.md](../SPEC_CORE.md)
**Engine:** Unreal Engine 5.7+
**Version:** 0.14.9 (Beta)

---

## MonolithCore

**Dependencies:** Core, CoreUObject, Engine, HTTP, HTTPServer, Json, JsonUtilities, Slate, SlateCore, DeveloperSettings, Projects, AssetRegistry, EditorSubsystem, UnrealEd

### Classes

| Class | Responsibility |
|-------|---------------|
| `FMonolithCoreModule` | IModuleInterface. Starts HTTP server, registers core tools, owns `TUniquePtr<FMonolithHttpServer>` |
| `FMonolithHttpServer` | Embedded MCP HTTP server. JSON-RPC 2.0 dispatch over HTTP. Fully stateless (no session tracking). `tools/list` response embeds per-action param schemas in the `params` property description (`*name(type)` format, `*` = required) so AI clients see param names without calling `monolith_discover` first |
| `FMonolithToolRegistry` | Central singleton action registry. `TMap<FString, FRegisteredAction>` keyed by "namespace.action". Thread-safe — releases lock before executing handlers. Validates required params from schema before dispatch (skips `asset_path` — `GetAssetPath()` handles aliases itself). Returns descriptive error listing missing + provided keys. Action rows carry execution policy metadata documented in [SPEC_MonolithActionExecutionPolicy.md](SPEC_MonolithActionExecutionPolicy.md), infer conservative mutating defaults for legacy production registrations, and developer overrides can update known action policies at runtime for local testing. |
| `FMonolithJsonUtils` | Static JSON-RPC 2.0 helpers. Standard error codes (-32700 through -32603). `SuccessResponse` uses an empty object for null results. Declares `LogMonolith` category |
| `FMonolithAssetUtils` | Asset loading with 4-tier fallback: StaticLoadObject(resolved) -> PackageName.ObjectName -> FindObject+_C suffix -> ForEachObjectWithPackage |
| `FMonolithResourceRegistry` | Read-only MCP resource registry for `resources/list` and `resources/read`. Default docs are loaded before registration, so unreadable or empty docs are not advertised. Pagination emits `nextCursor` only when another page exists and omits it on the exhausted page |
| `UMonolithSettings` | UDeveloperSettings (config=Monolith). ServerPort, bAutoUpdateEnabled, DatabasePathOverride, EngineSourceDBPathOverride, EngineSourcePath, 10 module enable toggles + `bEnableProceduralTownGen` (experimental, default false) (functional — checked at registration time), LogVerbosity. Settings UI customized via `FMonolithSettingsCustomization` (IDetailCustomization) with re-index buttons for project and source databases |
| `UMonolithUpdateSubsystem` | UEditorSubsystem. GitHub Releases auto-updater. Shows dialog window with full release notes on update detection. Downloads zip, cross-platform extraction (PowerShell on Windows, unzip on Mac/Linux). Stages to Saved/Monolith/Staging/, hot-swaps on editor exit via FCoreDelegates::OnPreExit. Current version always from compiled MONOLITH_VERSION (version.json only stores pending/staging state). Release zips include pre-compiled DLLs. |
| `FMonolithActionExecutionGuard` | Central action-dispatch scope and audit owner. Records duration, policy-gated dirty package deltas, transaction status, post-edit validation status, and rollback status without raw payload logging or rollback claims. Audit rows include registry execution policy metadata from [SPEC_MonolithActionExecutionPolicy.md](SPEC_MonolithActionExecutionPolicy.md). The optional advanced slice is the bounded ToolCall ledger in [SPEC_MonolithToolCallLedger.md](SPEC_MonolithToolCallLedger.md). |
| `FMonolithToolResultUtils` | MCP `tools/call` result-envelope helper. Preserves legacy text JSON and adds settings-gated `structuredContent` / `_meta` fields when `bEnableStructuredToolResults=true`. First slice contract is documented in [SPEC_MonolithStructuredToolResults.md](SPEC_MonolithStructuredToolResults.md). |
| `FMonolithMcpSessionTracker` | Bounded, in-memory MCP session observer for redacted `MCP-Session-Id` diagnostics. First slice contract is documented in [SPEC_MonolithMcpSessionMode.md](SPEC_MonolithMcpSessionMode.md). |
| `FMonolithCoreTools` | Registers 4 core actions |

### Helpers

| Symbol | Header | Responsibility |
|--------|--------|---------------|
| `MonolithCore::ValidatePackagePath(const FString&)` | `MonolithPackagePathValidator.h` (inline) | Wraps `FPackageName::IsValidLongPackageName` with an empty-string-on-success / error-msg-on-failure contract. Rejects empty input, double-slash (`//Game/...`), missing `/Game/` root, trailing slash, illegal chars. Added `dv.367` after a fatal `UObjectGlobals.cpp:1012` ensure from a malformed `//Game/...` JSON payload reaching `CreatePackage`. Currently routed at four sites: `HandleCreateWidgetBlueprint` (direct crash site), `MonolithAIInternal::GetOrCreatePackage` (~17 AI callers), `MonolithGASInternal::GetOrCreatePackage` (~6 GAS callers), `MonolithMaterialActions` (~5 Material callers). ~29 of 80 `CreatePackage` call sites guarded; remaining ~51 sites across MonolithBlueprint / MonolithLogicDriver / MonolithUITemplateActions / MonolithCommonUI* / MonolithMesh are follow-up backlog. |

### Actions (4 — namespace: "monolith")

| Action | MCP Tool | Description |
|--------|----------|-------------|
| `discover` | `monolith_discover` | List available tool namespaces and their actions. Optional `namespace` filter |
| `status` | `monolith_status` | Server health: version, uptime, port, action count, engine_version, project_name |
| `update` | `monolith_update` | Check/install updates from GitHub Releases. `action`: "check" or "install" |
| `reindex` | `monolith_reindex` | Trigger project re-index. Defaults to incremental (hash-based delta); pass `force=true` for full wipe-and-rebuild (via reflection to MonolithIndex, no hard dependency) |

---

### ToolCall Ledger

`bEnableAdvancedToolCallRecords` is the default-off setting for redacted ToolCall records and local analysis. The implemented first slice is documented in [SPEC_MonolithToolCallLedger.md](SPEC_MonolithToolCallLedger.md).

The implementation stays local and bounded, does not persist raw params or result payloads, and preserves the current `monolith.list_recent_action_audit` response shape for compatibility.

### Action Execution Policy Metadata

Action registration stores an execution policy object and exposes it through `monolith.discover`, deferred domain descriptions, recent audit rows, and advanced ToolCall records. Legacy registrations without explicit policies are classified by conservative action-name inference: read-like names keep `read_only`, while every other implicit default legacy action falls forward to `transaction_optional`. The guard now enforces the low-risk parts of mutating policies: dirty-package tracking is skipped for `read_only` actions, enabled when the policy requests it, transaction policies open a central UE transaction scope around the handler, and `post_edit_validate` runs a post-handler validator before returning success. `monolith.set_action_execution_policy` can update a known action's policy for local developer testing. Automatic asset rollback still reports unavailable; validator failure returns a structured action error and can cancel the central transaction record without claiming a revert.

### UE 5.7 Automation Compile Compatibility

MonolithCore automation tests must compile under Unreal Engine 5.7 both in non-unity and adaptive unity builds. JSON test code must use the UE 5.7 `FJsonObject::TryGetField` / `GetField(FStringView, EJson)` shape instead of dereferencing `GetField(TEXT(...))->Type` through an overload that no longer matches wide string literals. File-local test helpers must use unique names even inside anonymous namespaces because adaptive unity can include multiple test `.cpp` files into one generated translation unit.

### MCP Resources

`bEnableMcpResources` is the default-off setting for read-only MCP `resources/list` and `resources/read` support. The implemented first slice is documented in [SPEC_MonolithMcpResources.md](SPEC_MonolithMcpResources.md).

The first implementation exposes only explicit Monolith providers, does not read arbitrary caller-provided filesystem paths, and keeps all resource payloads bounded.

### Structured Tool Results

`bEnableStructuredToolResults` is the default-off setting for MCP `structuredContent` output on `tools/call` responses. The implemented first slice is documented in [SPEC_MonolithStructuredToolResults.md](SPEC_MonolithStructuredToolResults.md).

The implementation preserves the legacy `content[]` text JSON response for compatibility while adding structured fields only when configured. `monolith.get_mcp_server_status` reports the feature as `active_structured_content` when the setting is enabled because result shaping is evaluated per `tools/call`.

### MCP Session Mode

`bEnableMcpSessionMode` is the default-off setting for MCP session/request observation, progress, and cancellation. The implemented first slice is documented in [SPEC_MonolithMcpSessionMode.md](SPEC_MonolithMcpSessionMode.md).

The implementation stays process-local and redacted: it observes session headers, protocol version, method names, and tool names, but does not store raw session ids, request params, result payloads, auth headers, cookies, bearer tokens, or API keys. Progress notifications and in-flight cancellation remain follow-up work.

### MCP Compatibility Options

`monolith.set_mcp_compatibility_options` supports the implemented safe compatibility slice documented in [SPEC_MonolithMcpCompatibilityOptions.md](SPEC_MonolithMcpCompatibilityOptions.md).

The implementation exposes only a safe browser CORS toggle between `loopback_only` and `disabled`. Legacy SSE/message routes, wildcard CORS, and arbitrary origin allowlists remain out of scope.
