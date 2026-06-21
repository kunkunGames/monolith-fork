# ModelContextProtocol to Monolith ROI Survey

Status: Draft
Date: 2026-06-21
Owner: Monolith
Scope: UE 5.8 `D:\Engine\UE_5.8\Engine\Plugins\Experimental\ModelContextProtocol` compared against `D:\P4\game\Plugins\Monolith`

---

## 1. Purpose

Re-survey UE 5.8 `ModelContextProtocol` against current Monolith source and list only the remaining high-ROI work. This record supersedes stale ROI status in ignored PRD scratch documents where current Monolith source now proves several earlier P1/P2/P3 slices landed.

Non-goals:

| Item | Decision |
|------|----------|
| Copy Epic NoRedist source | Rejected |
| Add a second MCP server stack | Rejected |
| Make `ToolsetRegistry` or `ModelContextProtocol` mandatory | Rejected |
| Port legacy `UModelContextProtocolToolLibrary` assets | Rejected |
| Add outbound analytics | Rejected |

---

## 2. UE 5.8 Inventory

The UE plugin source inventory, excluding `Binaries` and `Intermediate`, is:

| Module | Files | Lines | Useful surfaces |
|--------|------:|------:|-----------------|
| `ModelContextProtocol` | 25 | 2489 | JSON-RPC server, Streamable HTTP headers, SSE write primitives, tools/resources handlers, progress/cancel semantics |
| `ModelContextProtocolEditor` | 15 | 1120 | `ToolsetRegistry` adapter, deferred toolset meta-tools, hash mapping commandlet, asset factories |
| `ModelContextProtocolEditorTests` | 9 | 1254 | Deferred/eager ToolsetRegistry behavior contracts |
| `ModelContextProtocolEngine` | 17 | 1770 | typed tool results, client config writers, legacy BP tool library wrappers |
| `ModelContextProtocolEngineTests` | 3 | 167 | engine result/config behavior contracts |
| `ModelContextProtocolTests` | 15 | 3308 | protocol/server/resource/tool-result behavior contracts |

Useful UE evidence:

| UE feature | Evidence |
|------------|----------|
| Long-lived SSE-style writes | `ModelContextProtocolServer.cpp` uses `EHttpServerResponseFlags::MultipleWriteStream` / `HasAdditionalWrites` and queues follow-up server events. |
| Request cancellation | `ModelContextProtocolServer.cpp` handles `notifications/cancelled`, finds active requests, and calls `Tool->CancelAsync`. |
| Progress notifications | `ModelContextProtocolServer.cpp` extracts `_meta.progressToken` and ticks progress updates through the open stream. |
| Typed content blocks | `ModelContextProtocolToolResults.h/.cpp` support `Text`, `Image`, `Audio`, `ResourceLink`, `EmbeddedResource`, and `StructuredContent`. |
| Engine media helpers | `ModelContextProtocolEngineToolResults.cpp` converts `UTexture2D`/`FImageView` and `USoundWave` into typed image/audio results. |
| Resources | `ModelContextProtocolResources.cpp` supports text and base64 blob resource contents. |
| Toolset adapter | `ModelContextProtocolToolsetRegistryAdapter.cpp` wraps `ToolsetRegistry` tools and registers `list_toolsets`, `describe_toolset`, and `call_tool`. |
| Client config writers | `ModelContextProtocolClientConfig.cpp` writes config for Claude Code, Cursor, VSCode, Gemini, and Codex. |
| Hash mapping | `ModelContextProtocolToolHashMappingCommandlet.cpp` writes a stable tool/toolset hash map. |
| Legacy BP tool assets | `UModelContextProtocolToolLibrary` and `UModelContextProtocolToolAsyncAction` are marked deprecated in favor of `UToolsetDefinition` / `ToolsetRegistry`. |

---

## 3. Monolith Mapping

| Area | UE behavior worth learning from | Current Monolith status | Remainder |
|------|---------------------------------|-------------------------|-----------|
| Async jobs | Tools can run asynchronously and be cancelled by request id. | `FMonolithAsyncJobRegistry`, `monolith.get_job`, `monolith.cancel_job`, and `monolith.reindex` job ids exist behind `bEnableAsyncJobs`; `ai.rebuild_zone_graph` job ids additionally require `bEnableZoneGraphRebuildJob`. Tests exist in `MonolithAsyncJobRegistryTests.cpp`, `MonolithAsyncJobActionsTests.cpp`, and `MonolithZoneGraphRebuildJobTests.cpp`. | Complete cooperative cancellation adoption in every long-running producer. Do not re-implement the registry. |
| Progress | UE streams `notifications/progress` over an open response. | `FMonolithProgressRegistry` and `monolith://progress/active` resource exist. Progress is poll-delivered because `GET /mcp` is single-shot SSE. | Add a real long-lived push transport before promising live notifications. |
| Request cancellation | UE maps `notifications/cancelled` to active request cancellation. | `FMonolithCancellationRegistry` and `notifications/cancelled` handling exist; opt-in actions can poll cancellation state. | Bridge cancellation consistently into async jobs and long-running action checkpoints. |
| Session lifecycle | UE enforces session/protocol headers and keeps active request state. | `FMonolithMcpSessionTracker` tracks redacted lifecycle state and gates sessions behind `bEnableMcpSessionMode`; `tools.listChanged` can be advertised with a revision. | Server-push `notifications/tools/list_changed` still waits on a push transport. |
| Typed media | UE supports image/audio/resource-link/embedded-resource result blocks. | `FMonolithToolContentBlock`, `FMonolithActionResult::MediaBlocks`, gated typed media emission, and first adopter `imagegen.generate_image` exist. Only `image` and `audio` emit. | Add `resource_link` / embedded-resource blocks and more adopters where the bytes already exist. |
| Resources | UE serves `resources/list` / `resources/read`, text, and blob. | `FMonolithResourceRegistry` supports `resources/list`, `resources/read`, bounded text, bounded blob, and `IMonolithResourceProvider`; `MonolithSource` has the first provider. | Add subscriptions/templates only after push transport and URI-lifetime policy are clear. |
| Deferred tool surface | UE hides large ToolsetRegistry catalogs behind deferred meta-tools. | Monolith has a native deferred domain catalog and tool-profile filtering; do not replace this with ToolsetRegistry. | Only finish `MonolithToolsetBridge` if a source/dev UE 5.8 bridge is explicitly needed. |
| ToolsetRegistry bridge | UE can import ToolsetRegistry tools into MCP. | `MonolithToolsetBridge` currently compiles as an inert optional shell and never hard-links public builds to `ToolsetRegistry`. | Implement `toolset.list_toolsets`, `toolset.describe_toolset`, and `toolset.call_toolset_tool` behind compile/runtime gates if required. |
| Client config | UE writes configs for several clients. | Monolith has onboarding/readiness and proxy scripts, but no exact UE-style multi-client config writer in this survey result. | Medium-low ROI unless onboarding failures show repeated manual config mistakes. |
| Tool hash map | UE can emit tool/toolset hashes. | Monolith has discovery, profiles, action metadata coverage, invocation logs, and ToolCall ledger work. | Low ROI unless stable external docs or clients need compact IDs. |

---

## 4. High-ROI Remaining Work

| Rank | Work | Why it is high ROI | Required boundary |
|------|------|--------------------|-------------------|
| P1 | End-to-end cooperative cancellation for long-running producers | Users notice cancel failures during expensive editor operations; Monolith already has both request cancellation and job cancellation primitives. | Producers must poll safe checkpoints and leave honest terminal job state. |
| P1 | Real server-push transport for progress and invalidation | Async jobs, progress registry, resources, and session tracker already exist; push transport unlocks server-originated `notifications/progress`, `notifications/tools/list_changed`, and future resource updates without broad domain work. | Must not fake live notifications through poll-only resources. Start with progress sink contract, per-request stream store, then opt-in SSE transport. |
| P2 | Complete typed-media blocks beyond inline image/audio | UE proves `resource_link` and `embedded_resource`; Monolith already has media slots, resources, and first image adopter. | Keep emission gated and bounded; do not duplicate large payloads when a resource link is better. |
| P2 | Resource templates/subscriptions/updated notifications | Resource registry and provider seam are present; standard MCP resource lifecycle remains incomplete. | Requires P1 push transport and explicit URI lifetime/invalidation policy first. |
| P2 conditional | Finish optional `ToolsetRegistry` bridge | Useful for UE 5.8 source/dev interop; scaffold and flags already exist. | Public builds must stay free of hard `ToolsetRegistry` dependencies. |
| P3 | Client config writer and hash-map polish | UE has usable examples, but Monolith already has onboarding/discovery/logging. | Do only if real onboarding or external-client evidence appears. |

Implementation order inside P1: complete cancellation producer checkpoints first, then start the push-transport foundation (`progress sink contract -> per-request stream store -> opt-in SSE transport`).

### 4.1 P1 Push Transport

Current proof:

| Evidence | Meaning |
|----------|---------|
| `SPEC_MonolithCore.md` says real-time `notifications/progress` is not delivered. | Progress is poll-only today. |
| `FMonolithHttpServer::HandleGetMcp` comments say the SSE endpoint returns one event and closes. | There is no durable server-to-client stream. |
| `SPEC_MonolithMcpSessionModeGate.md` defers `notifications/tools/list_changed`. | Tool-list invalidation has the same transport blocker. |
| UE uses `MultipleWriteStream`, active request state, queued SSE events, and ticked progress. | The implementation pattern is feasible in UE's HTTP layer, but must be adapted to Monolith's synchronous dispatch and guard model. |

Implementation contract:

| Step | Contract |
|------|----------|
| 1 | Create a held-open stream owner keyed by request/session and a safe weak-lifetime guard. |
| 2 | Send `notifications/progress` from `FMonolithProgressRegistry` updates when a live stream exists. |
| 3 | Send `notifications/tools/list_changed` when `FMonolithToolProfileManager` increments the tool-list revision. |
| 4 | Keep poll resources as fallback-observable state, not as fake live transport. |

### 4.2 P1 Cancellation Completion

Current proof:

| Evidence | Meaning |
|----------|---------|
| `notifications/cancelled` reaches `FMonolithCancellationRegistry`. | Request-level transport exists. |
| `monolith.cancel_job` calls `FMonolithAsyncJobRegistry::RequestCancel`. | Job-level cancellation exists. |
| Current docs say neither path interrupts running Unreal work. | Producer checkpoint adoption is the remaining work. |

Implementation contract:

| Producer class | Required work |
|----------------|---------------|
| `monolith.reindex` / index rebuilds | Check request/job cancellation before launching work and at known safe boundaries. |
| Source/project graph export | Stop between external process stages or graph phases; leave terminal job state honest. |
| ZoneGraph / Mass rebuild | Observe `IsCancelRequested(JobId)` before and after editor-side rebuild calls where possible. |
| Generated media/model jobs | Short-circuit before provider/network calls and before import/save when cancellation was requested. |

### 4.3 P2 Typed Media Completion

Current proof:

| Evidence | Meaning |
|----------|---------|
| UE supports `ResourceLink` and `EmbeddedResource`. | The MCP content model supports more than inline image/audio. |
| Monolith skips non-`image`/`audio` blocks and documents `resource_link` as TODO. | The extension point exists but is intentionally incomplete. |
| `imagegen.generate_image` is the first adopter. | New adopters can be incremental and opt-in. |

Implementation contract:

| Feature | Contract |
|---------|----------|
| `resource_link` block | Link to an already registered `monolith://...` resource with no duplicated base64 payload for large files. |
| Embedded resource block | Embed bounded resource content only when size/mime policy allows it. |
| Audio adopter | Let `audio.create_test_wave` or other byte-owning audio actions attach `audio/wav` when requested and gated. |
| Screenshot/preview adopters | Add opt-in image blocks for actions that already produce bounded PNGs. |

### 4.4 P2 Conditional ToolsetRegistry Bridge

Current proof:

| Evidence | Meaning |
|----------|---------|
| UE has `list_toolsets`, `describe_toolset`, and `call_tool`. | Bridge behavior can be tested clean-room. |
| Monolith has inert `MonolithToolsetBridge` and `bEnableToolsetRegistryBridge`. | Build/runtime boundary already exists. |
| Public builds must not hard-link UE Experimental/NoRedist plugins. | Bridge remains source/dev and opt-in only. |

Implementation contract:

| Step | Contract |
|------|----------|
| 1 | Keep `MONOLITH_WITH_TOOLSET_REGISTRY_BRIDGE=0` as the public-build default. |
| 2 | When enabled and headers exist, enumerate Toolsets without registering all tools into the default public Monolith surface. |
| 3 | Execute one ToolsetRegistry tool through `toolset.call_toolset_tool` with Monolith profile checks and stable error normalization. |
| 4 | Add clean-room tests for disabled build, enabled enumeration, execution success, execution error, async result normalization, and profile block. |

---

## 5. Verification Commands

Commands run from `D:\P4\game` or `D:\P4\game\Plugins\Monolith`:

```powershell
$root='D:\Engine\UE_5.8\Engine\Plugins\Experimental\ModelContextProtocol\Source'
$files = Get-ChildItem -LiteralPath $root -Recurse -File -Include *.h,*.cpp,*.cs
$files | Group-Object { ($_.FullName.Substring($root.Length+1) -split '\\')[0] }

rg -n "FListToolsetsTool|FDescribeToolsetTool|FCallTool|ToolsetRegistry|ToolHash|HashMapping|UModelContextProtocolToolLibrary|UModelContextProtocolToolAsyncAction|WriteClientConfiguration" D:\Engine\UE_5.8\Engine\Plugins\Experimental\ModelContextProtocol\Source\ModelContextProtocolEditor D:\Engine\UE_5.8\Engine\Plugins\Experimental\ModelContextProtocol\Source\ModelContextProtocolEngine

rg -n "bEnableAsyncJobs|monolith\.get_job|monolith\.cancel_job|RequestCancel|poll_action|job_id" Source\MonolithCore Source\MonolithAI Docs\specs Docs\API_REFERENCE.md
rg -n "progressToken|notifications/cancelled|tools/list_changed|listChanged|SSE|session" Source\MonolithCore Docs\specs\SPEC_MonolithCore.md Docs\specs\SPEC_MonolithMcpSessionMode.md Docs\specs\SPEC_MonolithMcpSessionModeGate.md
rg -n "FMonolithToolContentBlock|MediaBlocks|bEnableTypedMediaResults|attach_image_block|resource_link" Source\MonolithCore Source\MonolithImageGen Docs\specs Docs\API_REFERENCE.md
rg -n "RegisterBlobResource|IMonolithResourceProvider|resources/list|resources/read|resources/subscribe|resources/templates/list|resources/updated|blob" Source Docs\specs Docs\API_REFERENCE.md
rg -n "ToolsetBridge|ToolsetRegistry|ToolHash|HashMapping|ToolLibrary" Source Docs\specs Docs\API_REFERENCE.md PRD\UnrealMCP
git diff --check
```

No runtime visual verification or Discord screenshot upload was required because this was a source/documentation survey, not gameplay, UI, VFX, material, animation, or asset-presentation work.
