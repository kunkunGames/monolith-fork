# Monolith — TODO

Last updated: 2026-07-21 (EngineSource-backed CRG graph-node search and graph export retirement)

---

### Agent-Usability High-ROI Slice — 2026-07-04

Plan: `Docs/plans/2026-07-04-monolith-remaining-high-roi.md` (baseline) plus the root-level rechecked update of the same name (local live re-verification, revised priorities).

- [x] **P0-0 watchdog terminal-state hardening** — `Scripts/watch_mcp.ps1`: RESTART_LIMIT now backs off exponentially (cap `-RestartLimitBackoffMaxSec`, default 600s) instead of spinning per poll; healthy probes reset the restart budget (`RestartAttemptsReset`); `recover_mcp.ps1` runs as a bounded child killed past `-RecoverTimeoutSec + -RecoverInvokeGraceSec` (`RecoverTimeout`, exit 124); a script-scope trap guarantees `RESULT=FATAL` as the last logged line (exit 9); `WatchdogStart` marks instance boundaries in `watchdog.jsonl`. The Speed workstation task `Monolith MCP Watchdog - Speed` gained a 30-minute repetition trigger so a dead watchdog re-arms within 30 minutes (`IgnoreNew` keeps it single-instance). Spec: `specs/SPEC_MonolithAgentOpsScripts.md`.
- [x] **PR A instrumentation** — action log v3 `environment.is_automation_test` (+`automation_test_name`) from `GIsAutomationTesting`, and `environment.binary_build_utc` (MonolithCore module link timestamp) so analyzers can split stale-binary records from post-rebuild records; `analyze_invocation_logs.py` uses the stamp as the primary synthetic signal (markers/whitelists demoted to legacy fallback); `analyze_session_transcripts.py` reports a measurement caveat (connection-refused failures leave no tool result and are undercounted). Specs: `specs/SPEC_MonolithToolInvocationLogs.md`, `specs/SPEC_MonolithInvocationLogAnalyzer.md`.
- [x] **PR B discover token cost** — `FMonolithToolRegistry::GetCatalogFingerprint()` (profile-visible catalog hash), `monolith.status` `catalog_version`/`catalog_action_count`/`catalog_namespace_count`, `monolith.discover(if_version)` `{status:"unchanged"}` short-circuit (<1KB), `catalog_version` on every discover response.
- [x] **PR C compact error envelope** — `bCompactErrorEnvelope=true` default: one machine-readable copy of `related_actions`/`hints`/`error_data` per failure (structuredContent when enabled, top-level otherwise), no top-level `error_data` flattening, one-line pointer text in structured mode; legacy duplicated shape behind `bCompactErrorEnvelope=false`. Tests: `Monolith.Core.ToolResults.Compact*`.
- [x] **Analyzer cheap-skip classification for cooldown-gated maintenance — retired with graph export (2026-07-21)** — Historical `query:source.build_crg_graph` rows mixed fast skips (`skip_reason=cooldown|parity_fresh`, ~2s) with real builds (~72s). The action and maintenance loop no longer exist, so current ROI ranking must classify those rows as legacy evidence instead of creating analyzer work for a removed path.
- [ ] **Client adoption of the catalog cache routine** — teach `Tools/MonolithProxy` clients and the monolith-mcp skill docs the "status → remember catalog_version → discover(if_version)" routine so repeat discovers stop paying full payloads.
- [ ] **Post-rebuild fresh measurement** — after the running editor picks up this build (verify via `environment.binary_build_utc`), re-run `analyze_invocation_logs.py` (discover bytes 5-day window) and `analyze_session_transcripts.py` (transport split with per-source call-volume context) against the ROI doc's Definition of Done gates.
- [ ] **`editor.trigger_build`/`live_compile` kills the NullRHI headless editor (P4 capability-safe evidence)** — 2026-07-04 11:47 KST: triggering Live Coding against the `-NullRHI` headless MCP editor reported `{started:true}` and then the editor process exited (no crash dump; watchdog re-recovered in ~15s). In headless/null-RHI sessions these actions should return an explicit `unavailable_headless`-class error instead of taking the endpoint down. Live Coding remains valid for visible editors only.
- [ ] **Console-close propagation to the headless editor (P0-1 root-cause evidence)** — Both 2026-07-04 outages end with the editor logging `Engine exit requested (reason: ConsoleCtrl RequestExit)` (08:32:53 and 11:40:41 KST) at the same moment the watchdog console vanished: closing the visible watchdog/RunHeadlessEditor console windows kills the recovered editor via console-control propagation. Candidate fixes: launch the editor hidden/detached in the `RunHeadlessEditor.bat` path (`CREATE_NO_WINDOW`/`DETACHED_PROCESS`), or move the editor out of the closable console's process group so `CTRL_CLOSE` cannot reach it; until then the 30-minute task re-arm trigger bounds each outage.

---

### UnrealMCP M4 — Execution-Context Follow-Up Contracts (2026-06-20)

Spec: [PRD/UnrealMCP/Spec/04_session_progress_cancellation.md]. The execution-context foundation (`FMonolithExecutionContext`) and dispatch wiring have landed — ToolCall records now carry the real `json_rpc_id` / `tool_call_id` / `progress_token`. Two contracts constrain the remaining slices (surfaced by the wiring-slice review):

- [ ] **Session-id slice** — When persistent session mode wires a real `MCP-Session-Id` into the execution context, route it through `FMonolithExecutionContext::RedactSessionId(...)` only; never store or record the raw header. Until then `session_id_redacted` is correctly `"stateless"`.
- [x] **Cancellation transport** — Landed: `FMonolithCancellationRegistry` (request-id-keyed, thread-safe) is the cross-thread cancellation mechanism and `notifications/cancelled` signals it — honoring the contract (cancellation does NOT rely on the thread-local `GetCurrent()`). **Remaining:** make specific long-running async actions (PIE smoke, deep index, batch retarget) poll `FMonolithCancellationRegistry::Get().IsCancellationRequested(json_rpc_id)` and abort cooperatively; cancellation has no effect until an action opts in to polling.
- [x] **Progress reporting transport** — Landed: `FMonolithProgressRegistry` (per-`progressToken`, thread-safe) + the `monolith://progress/active` poll resource. **Remaining (two parts):** (1) make the same long-running async actions call `FMonolithProgressRegistry::Get().Report(...)`; (2) **real-time push** — the embedded UE HTTP server has no long-lived SSE (`HandleGetMcp` returns a single event and closes), so true `notifications/progress` streaming needs a push transport (held-open SSE via a socket-level handler, or forwarding through the `monolith_proxy.py` stdio bridge). Until then progress is poll-only via `resources/read`.
### MonolithCore — UnrealMCP-Port Slice Follow-Ups (deferred)

Specs: [specs/SPEC_MonolithAsyncJobActions.md](specs/SPEC_MonolithAsyncJobActions.md), [specs/SPEC_MonolithTypedMediaResults.md](specs/SPEC_MonolithTypedMediaResults.md), [specs/SPEC_MonolithMcpSessionModeGate.md](specs/SPEC_MonolithMcpSessionModeGate.md). The async-jobs slice is default-on for registry-backed `monolith.reindex`; typed-media / resources / session-mode remain dark-by-default. The items below are the explicit deferrals carried by those slices.

- [x] **Async reindex real completion delegate** — Landed 2026-06-21 and promoted in the P0.6 slice: `monolith.reindex` mints a registry-backed `job_id` by default under `bEnableAsyncJobs=true`, and `UMonolithIndexSubsystem` exposes job-aware full/incremental start functions that drive the row to `Completed` / `Failed` / `Cancelled`. Same pattern still applies to any other fire-and-forget producer that mints a job.
- [ ] **Typed-media `resource_link` content-block kind** — `FMonolithToolResultUtils::BuildMcpToolResult` emits only `image` / `audio` typed-media blocks today (other types are skipped). Add a `resource_link` (and embedded-`resource`) content-block kind so large binary assets can be referenced rather than inlined as base64. Pairs with the MCP resources surface (`bEnableMcpResources`).
- [ ] **`notifications/tools/list_changed` delivery (awaits SSE)** — The P1c session-mode slice tracks a tool-list revision and advertises `tools.listChanged` in `initialize`, but server-push `notifications/tools/list_changed` is NOT delivered: single-shot SSE has no long-lived push channel. Implement actual push once a real Streamable-HTTP SSE transport exists; until then clients must re-`tools/list` to observe a revision change. The blocking transport work is the P1d Stage-2 SSE slice below.
- [ ] **P1d Stage-2 real SSE streaming transport (DEFERRED — feasibility recorded 2026-06-21)** — Server-push SSE over `POST /mcp` (progress notifications, `notifications/*` push including the `notifications/tools/list_changed` item above, incremental tool-result frames). **Engine support is present and verified in UE 5.8** (`EHttpServerResponseFlags::MultipleWriteStream | HasAdditionalWrites`, SPSC `StreamingBodyQueue` + `StreamingBodyComplete`; result-callback reentry capped at 2/stack with a 3rd `checkf`). Remaining Monolith-side blockers after P1a/P1c: (1) **no progress sink** in the synchronous game-thread `FMonolithToolRegistry::ExecuteAction` (`MonolithHttpServer.cpp:1486`) — long actions are opaque until they return; (2) `FMonolithMcpSessionTracker` is a redacted **session** observer, **not** an authoritative per-request `progressToken` + `StreamingBodyQueue` + `AliveGuard` store; (3) **reentrancy hazard** at the single-shot completion loop (`MonolithHttpServer.cpp:462`, `CompleteRead` path) — inline streaming across a batch request would trip the engine `checkf` and break batch coalescing. Streaming must be opt-in/isolated (negotiated by `Accept: text/event-stream` + `progressToken`, dark-by-default) so the default `POST /mcp` route the headless harness uses keeps its single-buffered-response contract. Implementation order and full evidence: [testing/2026-06-21-mcp-sse-streaming-feasibility.md](testing/2026-06-21-mcp-sse-streaming-feasibility.md).
- [ ] **`ai.rebuild_zone_graph` full rebuild if left partial** — Under `bEnableZoneGraphRebuildJob` + `WITH_ZONEGRAPH` + `GEditor`, `RebuildZoneGraph` broadcasts `UE::ZoneGraphDelegates::OnZoneGraphRequestRebuild` and the subsystem handler runs the complete orchestrated rebuild synchronously, so the job is reported `Completed` honestly (it is NOT faked, and FailJob is used when ZoneGraph is compiled out or `GEditor` is absent). Deferred: keep a functional-fixture verification that the broadcast actually drives a full rebuild on a real ZoneGraph world; if any build/config leaves it partial, finish the full rebuild path rather than reporting `Completed`. With the flag off the action keeps its prior `Unavailable` response.

---

### MonolithImageGen — SVG Source and Future MSDF Pipeline (2026-06-05)

Spec: [specs/SPEC_MonolithImageGen.md](specs/SPEC_MonolithImageGen.md#8-svg--vector-source-extension). The feature extends `imagegen` with sanitized SVG source generation/import/validation for web and Unreal editor source assets plus explicit editor-time Multi-channel Signed Distance Field Texture2D conversion. SVG source actions write source `.svg` files and provenance sidecars only; `generate_msdf_from_svg` is the conversion boundary for precomputed runtime-ready MSDF textures/materials.

- [x] **P1 SVG source actions** — Added `imagegen.generate_svg`, `imagegen.import_generated_svg`, and `imagegen.validate_svg` with bounded XML/SVG sanitizer, `web|editor|msdf_source` profiles, redacted prompt provenance, deterministic source mirroring under `GeneratedImages/Vector`, and sidecar metadata.
- [x] **P1 verification** — Added sanitizer, geometry, deterministic generation, import round-trip, profile readiness, and `msdf_ready` fixture coverage. Evidence: `Saved/Automation/MonolithImageGenSvgSource_20260605/index.json` reports 6/6 `MonolithImageGen.SvgSource` tests passing with no warnings or failures.
- [x] **P3 MSDF Texture2D pipeline** — Added `imagegen.generate_msdf_from_svg` with validated `msdf_source` input, local CPU MSDF PNG baking, Texture2D import/settings (`TC_Masks`, `sRGB=false`, no mipmaps, UI group, clamp, `NeverStream`, max texture size), source PNG mirror/provenance, pixel/channel sample validation, and optional material preview render verification.
- [x] **P3 verification** — Evidence: `Saved/Automation/MonolithImageGenSvgSourceMsdf_20260605/index.json` reports 9/9 `MonolithImageGen.SvgSource` tests passing with no warnings or failures, including Texture2D settings, MSDF pixel/channel samples, invalid-source rejection, and rendered material preview decoding.
- [ ] **P2 raster preview decision** — Choose and verify a deterministic SVG rasterizer before registering `imagegen.rasterize_svg_to_texture`; conversion output must enter Unreal through PNG bytes and `asset.import_texture_from_bytes`.

---

### Headless MCP Launch — PLAN (2026-05-20)

Plan spec: [specs/SPEC_MonolithHeadlessMcpLaunch.md](specs/SPEC_MonolithHeadlessMcpLaunch.md). The feature uses a project-owned batch wrapper to start a full editor process with rendering disabled by default (`-NullRHI`) while MCP client configuration remains on the existing proxy/HTTP MCP path. It is not commandlet-based, does not pass Monolith-specific editor args, keeps P4 enabled, and does not replace offline `monolith_query.exe`.

- [x] **P1 project batch wrapper** — `D:\P4\speed\Build\BatchFiles\RunHeadlessEditor.bat` resolves the engine from `.uproject`, spawns the full editor with rendering disabled by default (`-NullRHI`), does not pass `-NoP4`, does not pass Monolith-specific editor args, and leaves MCP proxy ownership to the existing MCP client configuration.
- [x] **P1.5 MCP watchdog supervisor + scheduled/restart index maintenance** — `Scripts\watch_mcp.ps1` keeps probing `/health` for long agent sessions. If the endpoint is down and no editor-server process remains, it runs the host project's primary editor UBT build, pre-launch `MonolithReindex -mode=project` source DB maintenance, and delegates restart to `Scripts\recover_mcp.ps1`; once `/health` returns, it runs live `bridge.start_indexing(scope=assets)` and waits before resuming. If a headless `-NullRHI` / `Saved\HeadlessMcp` editor process is alive but `/health` stays unhealthy until `-RecoverTimeoutSec`, it stops only that headless process and reruns the same build/reindex/relaunch sequence. Recover uses headless-safe editor settings (`AssetEditorOpenLocation=NewWindow`, clean shutdown, never restore open asset tabs) to avoid stale asset-editor modal loops. While healthy, it runs one configurable daily asset/source maintenance pass, defaulting to `05:00` KST: live `bridge.start_indexing` plus `bridge.get_index_status` idle wait. Historical implementation evidence included a pre-launch cooldown-gated `Saved\graph.db` refresh and daily asset/source/graph pass; those graph steps are retired now that source indexing maintains `source_graph_nodes_fts` inside EngineSource. Spec: [specs/SPEC_MonolithAgentOpsScripts.md](specs/SPEC_MonolithAgentOpsScripts.md).
- [ ] **P1.6 ProjectIndex commandlet for true pre-launch asset maintenance** — `ProjectIndex.db` asset indexing currently lives in `UMonolithIndexSubsystem`, which intentionally skips DB open in commandlet mode to avoid build/cook lock failures. `watch_mcp.ps1` therefore runs asset maintenance immediately after MCP `/health` returns, not before process launch. Add a dedicated guarded ProjectIndex commandlet or shared non-subsystem index runner if future watchdog/release pipelines need asset DB indexing before any long-lived headless editor process is started.
- [ ] **P2 runtime diagnostics** — Add `monolith.get_runtime_environment` and expose headless/null-RHI capability profile without breaking existing `monolith.status` consumers.
- [ ] **P3 lifecycle helpers** — Add read-only instance/log diagnostics; defer guarded shutdown until there is a reliable process marker.
- [ ] **P4 capability-safe action behavior** — Audit viewport, Slate, PIE, and capture actions so headless/null-RHI sessions return explicit `unavailable_*` errors instead of silent empty results.
- [ ] **P5 verification** — Add parser/unit tests and a Windows headless MCP smoke run against the host `.uproject`.

---

### Tool Invocation Log Data Collection — PLAN (2026-05-20)

Plan spec: [specs/SPEC_MonolithToolInvocationLogs.md](specs/SPEC_MonolithToolInvocationLogs.md#52-analysis-data-collection-contract). Analyzer implementation has started at `Analyzer/analyze_invocation_logs.py` with compact fixtures under `Analyzer/fixtures/invocation_logs/`. The analyzer contract is [specs/SPEC_MonolithInvocationLogAnalyzer.md](specs/SPEC_MonolithInvocationLogAnalyzer.md), including log-grounded high-ROI priorities for heartbeat noise, source maintenance loops, schema confusion, missing-action triage, escape-hatch replacement mining, expected-slow-domain error ranking, large result projection, and high-error-rate action health. This plan is only about collecting enough structured data for future analysis of what agents call, why they likely call it, how they continue, and where latency accumulates.

- [x] **P1 correlation upgrade** — Added `record_id`, `parent_span_id`, `session_key`, `process_instance_id`, `call_index`, `previous_record_id`, and `time_since_previous_ms` while preserving v1/v2 reader compatibility.
- [x] **P2 routing context** — Added `routing_context` with `decision_source`, `namespace_source`, recent find/discover trace links where available, discovery matching, and coarse `inferred_intent`.
- [x] **P3 workflow context** — Added `workflow.step`, retry/recovery/discovery links, and left continuation outcomes analyzer-derived.
- [x] **P4 phase timing** — Added measured proxy/action/query `phase_timing` fields and child-process timing for editor actions that shell out to `monolith_query.exe`.
- [x] **P5 richer summaries** — Extended `return_summary` with result shape, recognized counts where derivable, warning/error counts, truncation fields, and payload byte counts.
- [x] **P6 environment context** — Added low-cardinality version/headless/profile/index-health context without logging secrets or raw session ids.
- [x] **P7 verification** — Added schema tests and smoke logs proving proxy -> action -> child query records can reconstruct a timeline and identify bottleneck phases.

---

### Agent Tool-Call & MCP Reliability Backlog — PLAN (2026-06-14)

Spec: [specs/SPEC_MonolithToolCallReliabilityBacklog.md](specs/SPEC_MonolithToolCallReliabilityBacklog.md). Curated output of running `Analyzer/analyze_invocation_logs.py` over the full `Logs/` window (2026-05-20 → 2026-06-14, 43,205-record snapshot), ground-truthing each candidate against current source, then re-checking the latest real-call days — and itself adversarially re-reviewed (6 dimensions, 30 confirmed findings) to correct counts/attributions. Most loud findings are already fixed and their log evidence is historical: `describe.action_schema` (alias build ~06-08), `editor.run_python`, `blueprint.get_component_details` (06-08), `source.read_file`/`read_source` param aliases, offline `--help`, `source.trigger_project_reindex`, and `imagegen` rate-limit **classification** (committed 06-13 `f5c0873b`; the 06-10 burst subsided before the fix). `source_control` strict-type errors stopped recurring but have **no** source fix. Only the items below bleed on the latest days.

- [x] **P0 analyzer recency + no-data dimension (DONE 2026-06-15)** — Added `--recent-days` (default 3) / `--fix-boundary` / `--rank-by-recency`, per-finding `still_open`/`recency_status`/`recency_score` (calls-metric for cost findings such as the CRG loop, errors-metric for the rest), an explicit `no_recent_data` (vs zero-error) state, and top-level `recency.{still_open,regressions,newly_quiet,no_recent_data}` views + markdown sections in `Analyzer/analyze_invocation_logs.py`. Additive, behind flags; default `score`/`rank` unchanged. Verified on the live 26-day window: CRG loop → `still_open`, `describe.action_schema`/`imagegen` → `newly_quiet`. Spec: `SPEC_MonolithInvocationLogAnalyzer.md` §5/§7.6/§8.3.
- [x] **P1 CRG maintenance-loop cooldown gate (HISTORICAL, DONE 2026-06-15; retired 2026-07-21)** — Added a `cooldown_seconds` (default 1800) gate + `metadata.last_build_epoch` stamp to `build_crg_graph` in `Tools/MonolithQuery/monolith_query.cpp`: skips the `graph.db` rebuild with `skip_reason=cooldown` when the last build is within the window even if a preceding `repair_crg_cache` churned the source signature; `force` bypasses, `cooldown_seconds=0` disables, and the existing signature check still fast-skips (`skip_reason=parity_fresh`). The chaining caller is **not in-repo** (agent/schedule behavior per §12; `check_index_freshness.ps1` only issues health-gated `repair_crg_cache`). Built via `Tools/MonolithQuery/build.bat`; verified cooldown-skip (0.84 s), `cooldown_seconds=0` disable→rebuild, and `last_build_epoch` write on a `graph.db` copy. This evidence is preserved, but the builder/cooldown contract is no longer active. Specs: `SPEC_MonolithSource.md`, `CLAUDE.md` §13.
- [x] **logicdriver discover routing gap (DONE 2026-06-14, CL 723)** — Added a `logicdriver` entry to `GetKnownOptionalModules()` (`MonolithCoreTools.cpp`) so `monolith.discover {namespace:logicdriver}` returns `Success{status,hint}` instead of `-32602` (11× on 06-13). Build `Result: Succeeded`. Record: `Docs/testing/2026-06-14-proxy-retry-and-schema-routing.md`. Follow-up **(DONE 2026-06-15)**: audited every gated namespace. **WITH_\*-plugin-gated** (whole module compiles out when an optional plugin is absent — the common case the logicdriver bug hit): `gas`/`combograph`/`logicdriver` only, all listed → **this class is complete**. **bEnable\*-setting-gated** (empty only when a user disables a default-true toggle): a large class (`audio`/`ai`/`material`/`blueprint`/`sprite`/`imagegen`/`ui`/`asset`/`config`/`mesh`/`scene`/`leveldesign`/`modelgen`/`level_instance`/`hlod`/`level_sequence`/`movie_render`) is intentionally **not** enumerated (user-initiated, lower priority; preferred remedy is an always-on `get_status` per module). Namespaces with an always-on `get_status`/list (`world_conditions`/`dataflow`/`gamefeatures`/`slate`/`water`/`chooser`/`animation`/`niagara`) are safe. Both classes are documented in `GetKnownOptionalModules()`. (An initial pass wrongly claimed the setting-gated namespaces were always-on; corrected after adversarial review.)
- [~] **P2 EngineSource.db symbol/file index-coverage gap — `coverage_miss` hint DONE 2026-06-15** — `source.read_source`/`read_file` now return `error.data.error_class=coverage_miss` + a hint + `next_actions` (`source.search_source` / `source.trigger_project_reindex` / `source.health`) instead of a bare not-found, on **both** the editor (`MonolithSourceActions.cpp`, UBT build `Result: Succeeded`) and offline (`monolith_query.cpp`, runtime-verified) surfaces, so a well-formed-but-absent symbol/path routes to reindex rather than an `editor.run_python` fallback. **Still open:** quantify scope-vs-stale and widen the indexer / make `index_health` detect the misses (editor-side). Spec: `SPEC_MonolithSource.md`.
- [x] **P2 binary-lag CI gate (DONE 2026-06-15)** — Wired `Scripts/check_offline_exe_fresh.py` into `Scripts/ci_static_checks.py` as the config-gated `offline_exe_freshness` check (`.github/monolith-static-ci.json`): a present, runnable, hash-mismatched `Binaries/monolith_query.exe` is a **blocker**; an absent or non-executable exe (e.g. a Windows PE binary on a Linux CI runner) is an advisory skip so headless CI stays green. Verified end-to-end: editing `monolith_query.cpp` → exactly 1 `offline-exe-fresh` blocker → `build.bat` rebuild → 0 blockers. Spec: `SPEC_MonolithQueryHelp.md`.
- [x] **6C session-transcript reader (DONE 2026-06-14)** — `Analyzer/analyze_session_transcripts.py` parses Codex/Claude rollouts (structured tool-result payloads only). Measured: 1,180 files, 376 Monolith results, **137 errors of which 136 (99.3%) are transport/availability** ("Transport send error" to 9316), 0 server-captured. Record: `Docs/testing/2026-06-14-mcp-availability-session-transcripts.md`. Overturns the earlier crude-grep guess that schema errors dominated.
- [~] **P1 MCP endpoint availability (top agent item) — partial, re-measured 2026-07-05** — 136 transport failures across 20 of 26 sessions (06-14), 246/48 sessions (roi-20260703 baseline), 100% invisible to server logs. **Done (CL 722):** `monolith_proxy.py`/`.js` now retry send-side connection failures (flicker self-heals) — helps proxy-routed clients (Claude Code). **Done (2026-07-03):** `Scripts\watch_mcp.ps1` supervises the editor-backed endpoint for long local agent sessions, rebuilds/restarts after editor death, restarts unhealthy headless `-NullRHI` editors, applies modal-safe recover settings, and runs restart-triggered source plus post-health asset maintenance. Historical rollout evidence included source/graph maintenance and keeping asset/source/graph DBs warm; the graph-specific steps are retired, while the availability measurement remains valid. **Post-watchdog measurement (2026-07-05, since 20260703):** 266 results / 44 sessions, **1 transport failure**; proxy-routed Claude path **0/218** transport-failed. Attribution is bounded: Codex call volume collapsed (48 vs baseline thousands) and ConnectionRefused-at-handshake is structurally undercounted (analyzer `measurement_caveat`), so the *proxy* improvement is proven and the *Codex-direct* improvement is not. Record: [testing/2026-07-05-mcp-availability-post-watchdog.md](testing/2026-07-05-mcp-availability-post-watchdog.md). **Still open:** (a) native `monolith_proxy.exe` send-retry (Codex-direct class — the residual failure path); (b) `9316` flicker while a non-headless editor is alive (ConsoleCtrl console-close propagation — see below); (c) watchdog `-NullRHI` over-kill (see below).
- [ ] **Watchdog `-NullRHI` recover kill is over-broad (NEW 2026-07-05)** — `watch_mcp.ps1` recover kills a `-NullRHI` editor when `9316` stays unhealthy, but the heuristic matches on `-NullRHI` and cannot distinguish an unhealthy MCP editor from a **legitimate non-MCP headless run** (`Automation RunTests`, commandlets) that never intended to serve `9316`. Observed 2026-07-05: a live watchdog killed an in-progress automation editor mid-init (no clean shutdown marker). Fix: gate the kill on the MCP-editor signature (`Saved\HeadlessMcp` log path / actual `9316` listener ownership), not on `-NullRHI` presence; until then, stop the watchdog around commandlet/automation runs. Contract: `SPEC_MonolithAgentOpsScripts.md`.
- [ ] **Editor `9316` flicker — ConsoleCtrl console-close propagation (from 2026-07-04 §0.1-1)** — closing the visible watchdog/console window propagates `ConsoleCtrl RequestExit` to the recovered editor, taking editor + endpoint down together. Fix: launch/detach the recovered editor outside the closable console process group so a console-window close does not signal it. Contract: `SPEC_MonolithAgentOpsScripts.md`.
- [x] **read_source path-key reroute (DONE 2026-06-14, CL 723)** — `HandleReadSource` now reroutes to `read_file` when `file_path`/`path` is given with `symbol` absent (167 lifetime errors). Build `Result: Succeeded`; `SPEC_MonolithSource.md` synced.
- [x] **Offline search `--query` alias (DONE 2026-07-03)** — `Saved\Monolith\LogAnalysis\roi-20260703` showed fresh schema-confusion regressions where agents called `project search --query=...` and `source search_source --query=...`, but the offline CLI only accepted positional search text. `Tools\MonolithQuery\monolith_query.cpp` now resolves positional, `--query`, and `--q` for both searches; `monolith_query_help.h` documents the option forms. Built with `Tools\MonolithQuery\build.bat`; verified positional, inline, and space-separated option forms plus missing-query errors. Record: [testing/2026-07-03-monolith-query-search-query-alias.md](testing/2026-07-03-monolith-query-search-query-alias.md).
- [x] **Blueprint `resolve_node reliable` string literal compatibility (DONE 2026-07-03)** — `Saved\Monolith\LogAnalysis\roi-20260703` showed `blueprint.resolve_node` schema regressions from agent calls that sent `reliable:"true"` for CustomEvent dry-runs. The public schema now accepts `bool|string` for `resolve_node.reliable` only, and the handler accepts boolean plus the literal strings `true/false/1/0/yes/no` while rejecting arbitrary strings such as `maybe`. `add_node` and other mutating bool params remain strict. Verified by Live Coding compile, automation tests, and live MCP calls. Record: [testing/2026-07-03-blueprint-resolve-node-reliable-string.md](testing/2026-07-03-blueprint-resolve-node-reliable-string.md).
- [~] **P3 large-result projection** — imagegen blind-retry is closed: `generate_image_via_ima2` rate-limit responses now seed a retry-signature cooldown and identical requests short-circuit before provider I/O; source_control coercion is closed by the 2026-07-04 additive input-tolerance fix. `source.find_overrides` (live + offline CLI) and `blueprint.search_functions` are closed by the 2026-07-04 additive `offset`/`cursor`/`fields` list-projection contract (`total`/`returned`/`next_cursor`/`projection`, unknown-field warnings; see `Docs\testing\2026-07-04-find-overrides-search-functions-projection.md`). Evidence recheck (2026-07-04, 46-day log window): the headline `>=209KB` numbers were stale or synthetic — find_overrides 276KB was the offline CLI on 2026-05-29 before the 06-08 `detail_level=minimal` default (1.2–2.4KB avg since), search_functions 259/194KB rows were `limit=1000000` limit-guard automation fixtures (now stamped `is_automation_test`), and all four actions had zero live calls in the 20260629–20260704 window. Remaining `audio.list_available_metasound_nodes`/`paper2d.list_assets` caps stay queued at low priority on that corrected evidence. The three fresh large-result candidates from the same window are closed (2026-07-04, same day): `monolith.find` (52 calls, avg 37KB → `planning_detail=compact` default with counts instead of `precondition_details`/`planning_signals`, plus `offset`/`cursor`/`fields` and the common `total`/`returned`/`next_cursor`/`projection` contract; `Monolith.Core.FindProjection`), `editor.list_automation_tests` (2MB single call → paged, default 500/max 5000 with `offset`/`cursor` and the common contract), and `monolith.get_action_metadata_coverage` (max 171KB → additive `detail=summary` keeps totals+gate and replaces per-bucket rows with counts; default stays `full` for the CI gate).
- [x] **monolith.discover large-result planning/schema projection (2026-07-03)** — `monolith.discover` namespace action listings now default to `planning_detail=compact` and `schema_detail=compact`, preserving params/status/counts while omitting `precondition_details`, `planning_signals`, full search metadata, full action descriptions, and per-param descriptions unless `planning_detail=full` / `schema_detail=full` is requested. `detail=true` listings are capped to the default 50-row page even when a larger limit is requested. This closes the ROI finding where `detail=true limit=1000` namespace discovery returned large payloads for broad action browsing; focused `mode=schema` remains the preferred full detail path for one action.
- [~] **P1 generic `monolith.execute_plan` — v1 DONE 2026-07-04, v2 DONE 2026-07-05, recipes open** — `FMonolithPlanExecutor` (`Source/MonolithCore/Private/MonolithPlanExecutor.{h,cpp}`) executes a validated sequential plan (max 25 steps) through the normal `ExecuteAction` pipeline with `"$steps.<id>.result.<path>"` reference resolution, `dry_run` plan reports, a `confirm` gate for mutating steps, an `allow_destructive` gate, `stop_on_error`, and per-step result size caps; children log under the plan's trace/span. **v2 (2026-07-05):** numeric array-index reference segments (`"$steps.s1.result.items.0.name"`), and a `transaction` param (`auto`/`off`) that wraps a mutating plan in one outermost editor transaction and **cancels it when `stop_on_error` halts the plan** — undoable object edits roll back (`rolled_back:"editor_transaction"` per step), while saves/disk/source-control/external-process effects do not (`transaction.caveat`); the response reports `transaction.{mode,state}`. Tests `Monolith.Core.PlanExecutor.*` (7: v1 5 + `ResolvesArrayIndexReferences`, `TransactionWrapsAndCancels`) plus live status→discover(if_version) and array-index chains. Records: [testing/2026-07-04-find-projection-execute-plan.md](testing/2026-07-04-find-projection-execute-plan.md), [testing/2026-07-05-execute-plan-v2.md](testing/2026-07-05-execute-plan-v2.md). **Open (v3 / recipes):** promote repeated agent sequences (BP variable→function→node→compile; asset duplicate→reference-fixup→save→source_control.checkout_or_add) into documented `monolith.guide` plan recipes, and per-step nested `transaction` policy when a caller wants a failing step isolated without rolling back earlier committed steps.

---

### Monolith Native Fallback-Gap Queue — ACTIVE (2026-07-03)

Spec/guidance: [MONOLITH_GUIDE.md](MONOLITH_GUIDE.md), [specs/SPEC_MonolithPractitionerWorkflowROI.md](specs/SPEC_MonolithPractitionerWorkflowROI.md), [specs/SPEC_MonolithToolCallReliabilityBacklog.md](specs/SPEC_MonolithToolCallReliabilityBacklog.md). Agents should execute tasks in this order: first-class Monolith action/workflow > Unreal commandlet > direct reflected/CDO property access or modification > Unreal Python. Any fallback below first-class Monolith is a signal for future native action/workflow coverage.

- [ ] **Fallback-gap intake discipline** — When a task cannot be completed with an existing Monolith action, add or update a concrete TODO entry in this section before finishing. Each entry must include the missing typed action/workflow, the fallback used, the target namespace/module, evidence (`LogAnalysis`, `SessionAnalysis`, or exact task blocker), and the verification proof required to close it. Do not record secrets, raw transcripts, or routine task journals.
- [ ] **Promote repeated `editor.run_python` escape-hatch clusters** — Use `Analyzer\analyze_invocation_logs.py --rank-by-recency --category escape_hatch_replacement` to find bounded repeated scripts, then replace them with typed namespace actions or workflow actions. Current ROI evidence from `Saved\Monolith\LogAnalysis\roi-20260703` shows repeated clusters around asset library, level actor, asset load, Blueprint, subsystem, material, widget blueprint, and AssetRegistry patterns.
- [ ] **Close server-blind availability gaps from session transcripts** — Use `Analyzer\analyze_session_transcripts.py` over `.codex\sessions` and `.claude\projects` to find direct-client transport failures that never reach `action.jsonl`; feed recurrent availability or reconnect failures into endpoint/server/proxy/client TODOs rather than treating them as action-schema bugs.
- [ ] **Headless Live Coding change detection is dead (`editor.live_compile`/`trigger_build` no-op)** — On the headless `-NullRHI` MCP editor (Speed workstation, 2026-07-11), `editor.live_compile` reports "Live coding succeeded, no code changes detected" for on-disk `.cpp` changes (both tmp+rename and in-place rewrites; sources hours newer than the loaded DLL). Fallback used: watchdog UBT rebuild on the next editor restart. Needed: either a typed diagnosis/reason in `get_live_coding_diagnostics` (e.g. watcher not armed headless) or a first-class `editor.rebuild_module` path that works headless. Evidence: `Docs/testing/2026-07-11-benchmark-contract-failfast-and-n3-guards.md` §6.
- [ ] **Source CRG completion durability and typed physical-DB repair** — During the 2026-07-12 submission gate, `source.trigger_project_reindex` completed its native/scoped-CRG worker phase, but the editor received an exit request while `FinishIndexingOnGameThread` was still running synchronous `RepairCrgCache(true)`. Reopen preserved native rows but exposed zero derived metrics/override rows; counted `source.health` detected the mismatch and an uninterrupted `source.repair_crg_cache --execute` restored logical parity. A later SQLite audit found orphan `never used` pages that no first-class action could repair, so the endpoint was stopped and offline `VACUUM INTO` was used as the narrow fallback. Target: `source` / `MonolithSource`. Needed: honor scoped/full/override `COMMIT` return values, expose a terminal asynchronous completion state after all maintenance, add a subprocess interruption/reopen regression, strengthen override-row freshness checks, and provide a single-writer-guarded typed physical integrity/repair action. Close proof: commit failure returns non-OK; interruption/reopen never publishes partial derived state; `Indexer complete` is documented/tested as non-terminal; a corrupted-copy dry-run/execute fixture ends with full SQLite integrity, clean counted Source health, and strict offline parity. Evidence: `Docs/testing/2026-07-11-native-proxy-offline-fallback.md` §6.
- [ ] **`value_domain` schema-quality sweep (522 actions)** — SchemaCompleteness baseline-20260711 (score 0.9395) puts `value_domain_rate` at 0.6979: constrained params missing enum lists/numeric ranges/descriptions. Fully-failing small namespaces first (`paper2d`, `pcg`, `network`, `artifact`, `ndisplay`, `water`, `metahuman`, `chaos_fracture`, `cloth`, `dataflow` — 1–4 param-bearing actions each), then the large namespaces by rate. Verification: re-run the SchemaCompleteness scan and record the `value_domain_rate` delta in `Benchmarks/SchemaCompleteness/RESULTS.md`.

---

### Monolith CRG Residual ROI — TRUST SLICE IMPLEMENTED (2026-06-15)

Spec: [specs/SPEC_MonolithCrgResidualRoi.md](specs/SPEC_MonolithCrgResidualRoi.md). The 2026-06-15 adversarial CRG audit historically accepted keeping the source/project CRG-inspired query surfaces and derived `crg_*` projection caches while treating `Saved\graph.db` export/search as conditional rather than a default maintenance cost. The 2026-07-21 caller/ROI decision advances that boundary: keep the CRG query meaning, serve `search_crg_graph` through EngineSource `source_graph_nodes`/`source_graph_nodes_fts`, and remove the separate export. The implemented ROI remains correctness and trust: duplicate/missing-path source rows no longer dominate ranked review context, project sensitivity no longer treats `Design`/`Assignment` as signing risk, and stale project CRG parity is visible and repairable.

- [x] **P0 fixtures for the retained trust slice** — Added focused fixtures for known-path source symbol priority, `Design`/`Assignment` sensitivity false positives, positive sensitive tokens, and stale project `assets != crg_nodes` parity. Maintenance/export SLO evidence remains part of P4.
- [x] **P1 source de-dup and path provenance** — `search_source`, `symbols_by_name`, `source.review_context`, and EngineSource-backed `source.search_crg_graph` prefer known file paths, expose `path_status`, and suppress duplicate missing-path rows without collapsing real symbol identity.
- [x] **P2 project sensitivity tokenization** — Replaced broad substring matching with token-boundary/camel-case aware matching plus matched-token provenance so `Design`/`Assignment` do not imply signing risk while `Signature`/`Crypto`/`Hash` still score.
- [x] **P3 project CRG freshness and transactional repair** — `project.health --include-counts=true` warns on stale CRG parity and `project.repair_crg_cache --execute` repairs the copied stale-DB fixture back to healthy parity.
- [x] **P3.1 scoped project CRG refresh after asset indexing (DONE 2026-06-16)** — `UMonolithIndexSubsystem` now collects changed package paths during startup delta and live Asset Registry drains, then calls `FMonolithIndexReview::RefreshCrgCacheForAssets` after the authoritative `assets`/`dependencies` transaction commits. Existing projection tables refresh only the changed assets plus one-hop dependency/referencer neighbors; deleted assets recover neighbor ids from old CRG edges before the stale node is removed; missing projection tables bootstrap through full repair. `Monolith.IndexGuard.Project.RefreshCrgCacheForAssetsScoped` verifies node/edge/metric parity, duplicate-edge prevention, and delete-neighbor metric recomputation. Evidence: `Saved\Automation\MonolithCrgScopedRefresh_20260616_R11\index.json` (5/5 focused tests passed) and `Docs\testing\2026-06-16-monolith-crg-scoped-refresh.md`.
- [x] **P3.2 duplicate-free project source reindex + scoped source CRG refresh (DONE 2026-06-16)** — `FMonolithSourceIndexer` now prunes existing project `Source` and project-plugin `Source` rows before incremental project source indexing, deleting orphan symbols, dependent source references, FTS rows, override edges, deprecations, and `crg_*` projection rows for that slice while preserving engine symbols. `InsertModule`/`InsertFile` re-select canonical ids after duplicate unique-key inserts, so new symbols are not attached to stale `last_insert_rowid()` file ids. `FMonolithSourceDatabase::RefreshCrgCacheForFiles` refreshes changed symbols plus one-hop/pending-prune neighbors without a full CRG rebuild when projection tables exist. `Monolith.IndexGuard.Source.PruneIndexedFilesUnderRootsRemovesProjectSlice` verifies cleanup, duplicate-free reinsertion, orphan cleanup, scoped CRG parity, clean source health, and old-neighbor risk metric recomputation. Evidence: `Saved\Automation\MonolithCrgScopedRefresh_20260616_R11\index.json` (5/5 focused tests passed), `Saved\Logs\MonolithReindexScopedRefresh_20260616_R10.log` (project reindex 31.9s commandlet / 39.3s wall; scoped CRG refresh 1934 files, 12494 affected symbols), and `source.health --include-counts=true --include-deep-checks=true` via `monolith_query.exe` hash `ecfaa97ca26b8192` (`status=ok`, orphan and CRG parity checks clean).
- [x] **P4 maintenance/export SLO — closed by elimination (2026-07-21)** — Routine review and watchdog paths cannot rebuild `Saved\graph.db` because the builder/export path is removed. Historical cooldown/build timings remain analyzer evidence only; current graph-node search pays the existing EngineSource read/FTS cost.
- [x] **P5 graph export remove decision (2026-07-21)** — Caller census confirmed the only effective graph artifact consumer was File/Symbol node search; flow/community/risk auxiliary tables were unused and risk/review actions already depended on EngineSource native/`crg_*` data. Preserve `search_crg_graph` semantics through EngineSource VIEW/FTS, remove `build_crg_graph`, `rebuild_crg_graph`, `repair_crg_graph`, `crg_graph_health`, `graph_db`, and watchdog coupling, and keep `risk_score`, `review_context`, `impact_radius`, `review_hotspots`, and `find_overrides` unchanged.

---

### Routing, Action Contract, Cohesion & Code-Find Refactor — PLAN (2026-05-19)

Plan spec: [specs/SPEC_MonolithRoutingCohesionRefactor.md](specs/SPEC_MonolithRoutingCohesionRefactor.md). Additive and compatibility-preserving: public `{namespace}_query` tools and action names remain stable while routing, projection, and reuse contracts are tightened.

- [x] **P0** — Accept the refactor contract: `monolith.find` + projection-aware `monolith.discover` become the canonical agent catalog path; static docs route to live discovery instead of duplicating action catalogs.
- [x] **P1** — Core response helpers: `FMonolithProjectionSpec` / `FMonolithProjectionUtils` parse common projection controls and build common `status`, `success`, `input`, `limits`, `count`, `truncated`, `next_cursor`, and `next_actions` fields.
- [x] **P2** — Add `monolith.find`, then extend `monolith.discover` with `mode=summary|actions|schema`, `query`, `action`, `fields`, `limit`, `cursor`, and profile/filter metadata.
- [x] **P3** — Trim `tools/list` namespace descriptions and update agent routing guidelines to prefer `monolith_find` + schema-targeted `monolith_discover`.
- [x] **P4** — Migrated legacy Source lookup/read/index trigger actions away from action-level `content[]` into structured rows/objects with common projection fields and explicit bounded text for read/materialize actions.
- [x] **P5 review result parity (2026-05-19)** — Source and Project review handlers preserve existing semantic payloads and add common `success`, `count`, and `truncated` fields through `FMonolithToolResultUtils::AddReviewResultCommonFields`.
- [x] **P6 Core/Source high-traffic schema validation slice (2026-05-19)** — `FParamSchemaBuilder` supports `EnableValidation()`, `Enum()`, and `Range()`; `FMonolithToolRegistry::ExecuteAction` rejects opted-in type/range/enum violations with `-32602`.
- [x] **P7 Source/index schema validation slice (2026-05-20)** — All 23 live `source` actions and all 5 live `index` actions opt into registry-level top-level validation before source DB traversal, CRG review handlers, indexing dispatch, attachment materialization, or asset/source bridge logic.
- [x] **P7 Core monolith schema validation slice (2026-05-20)** — Core-owned production `monolith` actions in `FMonolithCoreTools`, `RegisterMonolithExecutionGuardActions`, and `FMonolithToolProfileActions` opt into registry-level top-level validation before routing/discovery/status, update/reindex, MCP/session compatibility, onboarding/readiness/notification settings, execution audit/policy, or tool profile handlers run. `MonolithParamSchemaTests.cpp` now covers Core registry contracts and malformed-param guard cases through `MonolithTestSupport`.
- [x] **R6 readiness/freshness slice (2026-05-19)** — `FMonolithIndexFreshnessUtils` centralizes ProjectIndex/EngineSource DB path and file freshness reporting. `monolith.status`, `monolith.get_readiness_status`, and `index.get_index_status` expose DB existence, path, size, mtime, and age.
- [x] **R7 indexing freshness slice (2026-05-20)** — Successful ProjectIndex and EngineSource indexing completion now rebuilds the matching CRG projection/cache automatically. Live Coding / hot-reload completion continues to trigger incremental project source indexing so post-build C++ symbols refresh in `EngineSource.db`.
- [x] **R8 namespace-domain source split (2026-05-20)** — Mesh-family namespace implementations moved into their owning source modules: `scene` -> `MonolithScene`, `leveldesign` -> `MonolithLevelDesign`, `worldgen` -> `MonolithWorldGen`, and `modelgen` -> `MonolithModelGen`. Mesh validation remains under `mesh`; `MonolithWorldGen` owns its GeometryScript handle pool and all `worldgen` registrations.
- [ ] **P7 follow-up** — Continue broader cross-module test migration, `MonolithMesh` / `MonolithUI` decohesion, and wider domain schema validation.

---

### MonolithDataflow — Read-Only Graph Inspection (2026-05-19)

- [x] `MonolithDataflow` owns Dataflow graph inspection under the `dataflow` namespace instead of mounting it from `MonolithMesh`.
- [x] `dataflow.get_status` and `dataflow.list_assets` remain always-on and dependency-light.
- [x] `dataflow.get_dataflow_graph`, `dataflow.list_dataflow_node_types`, `dataflow.get_dataflow_node_schema`, `dataflow.validate_dataflow_graph`, `dataflow.list_dataflow_variables`, and `dataflow.list_dataflow_comments` compile only when `WITH_MONOLITH_DATAFLOW=1`.
- [ ] **Functional Dataflow fixture** — Run editor automation against a real `UDataflow` asset containing nodes, variables, comments, and connections once a stable fixture is available.

---

### MonolithLevelSequence — Anim Mixer Read-Only Probe (2026-05-19)

- [x] `get_anim_mixer_status` — Reflection-only status probe for Epic's UE 5.8 Experimental `MovieSceneAnimMixer` plugin/modules/classes; UE 5.7 builds keep no hard dependency.
- [x] `list_anim_mixer_tracks` — Load a Level Sequence and list reflected Anim Mixer tracks/layers/sections/child-track counts when the optional plugin is present.
- [ ] **UE 5.8 functional sample** — Verify against a real Level Sequence containing `MovieSceneAnimationMixerTrack` once a UE 5.8 project sample is available.

---

### MonolithGameFeatures — Inspection + Instanced Action Authoring COMPLETE (2026-05-19, updated 2026-06-30)

- [x] `MonolithGameFeatures` owns the `gamefeatures` namespace instead of mounting Game Feature inspection from `MonolithIndex` or `MonolithMesh`.
- [x] `gamefeatures.get_status`, `add_action_set_input_mapping`, `set_primary_asset_scan`, `add_game_feature_data_input_mapping`, `add_game_feature_data_widgets`, `add_game_feature_data_components`, `add_game_feature_data_gameplay_cue_paths`, `add_game_feature_data_abilities`, and `remove_game_feature_data_action` are always registered; `list_plugins`, `find_game_feature_data`, `describe_game_feature_data`, `list_action_classes`, `describe_action_set`, and `validate_plugin` register only when `bEnableGameFeatureActions=true`.
- [x] `describe_game_feature_data`, `list_action_classes`, and `describe_action_set` provide bounded reflected `UGameFeatureAction` property summaries so callers can discover Lyra/CommonGame feature-action shapes before using writer actions.
- [x] Creation remains reserved behind `bAllowGameFeaturePluginCreation`; no create, activate, deactivate, overwrite, delete, or descriptor mutation action is registered.

---

### MonolithSlate — Read-Only Inspector COMPLETE (2026-05-19)

- [x] `MonolithSlate` owns the `slate` namespace instead of mounting live Slate window/widget inspection under `MonolithUI`.
- [x] `slate.get_inspector_status` is always registered; `list_windows`, `snapshot_widgets`, `describe_widget`, `capture_widget`, and `wait_for_widget` register only when `bEnableSlateInspectorActions=true`.
- [x] Input automation remains deferred; no click, hover, key, text input, or widget mutation action is registered.

---

### MonolithWater — Read-Only Discovery COMPLETE (2026-05-19)

- [x] `MonolithWater` owns the `water` namespace instead of mounting Water discovery from `MonolithMesh`.
- [x] `water.get_status` and `water.list_bodies` use reflected class names and module-status probes only; there is no hard Water, WaterEditor, Landscape, or LandscapeEditor dependency.
- [x] Water actor, spline, zone, buoyancy, landscape, and rebuild mutations remain future work; this slice is read-only.

---

### v0.14.3 Changes (2026-04-24)

#### MonolithCore — HTTP Bind Retry + Restart

- [x] `FMonolithHttpServer::Start()` — HTTP bind retry with TCP port probe (5 attempts, exponential backoff). Prevents startup failures when the port is briefly held by a prior editor session.
- [x] `FMonolithHttpServer::Restart()` — New method for runtime recovery without editor restart.
- [x] `Monolith.Restart` console command — Wired to Restart() for manual recovery.
- [x] Added Sockets + Networking module dependencies to MonolithCore.

#### MonolithAnimation — 4 New Node Types + add_variable_get (115 → 116 actions)

- [x] `add_anim_graph_node` — 4 new node types: TwoBoneIK, ModifyBone, LocalToComponentSpace, ComponentToLocalSpace.
- [x] `add_variable_get` — New action (K2Node_VariableGet) for placing variable getter nodes in anim graphs.

#### MonolithBlueprint — EditCradle + Issue #29 Fix

- [x] **MonolithBlueprintEditCradle** — Recursive property-edit cradle for nested struct/array properties. Wired into `set_cdo_property`, `create_data_asset`, `create_blueprint`. Fixes cross-package TObjectPtr serialization (nested structs/arrays with TObjectPtr references to other packages were silently dropped by FLinkerSave's harvest walk on save).
- [x] **Issue [#29](https://github.com/tumourlove/monolith/issues/29) — FIXED.** Root cause: nested struct/array CDO property edits with cross-package TObjectPtr references (e.g., IMC DefaultKeyMappings referencing InputAction assets) were silently dropped on save. EditCradle fires PreEditChange/PostEditChange at the correct property depth so FOverridableManager tracks the edit and FLinkerSave harvests all referenced objects. Credit: @danielandric for thorough repro.

---

### MonolithUI M0.5 — CommonUI Action Pack, 50 Actions COMPLETE (2026-04-19, v0.14.0)

- [x] Phase 1 — Foundation: Build.cs 3-location CommonUI detection, `#if WITH_COMMONUI` scaffolding, `FMonolithActionInfo.Category` + backwards-compatible `RegisterAction`, `discover()` category filter, shared Pattern 1/2/3 helpers (asset create, widget-tree load/mutate/compile, runtime UFunction invoke, property-from-JSON), 9 category stubs, module wiring, dynamic action-count log (dv.245–251)
- [x] **CommonFramework diagnostics/authoring (DONE 2026-06-30)** — Added always-on `ui.get_common_framework_status`, `ui.add_primary_game_layout_layer`, and `ui.describe_common_widget_blueprint` for Lyra Common plugin/module/class/struct reflection, `PrimaryGameLayout` parentage, `PrimaryGameLayout` CommonActivatable layer container authoring, `UIExtensionPointWidget` tags, and CommonActivatableWidgetContainerBase layer candidates without hard-linking optional Lyra Common plugins or modifying CommonGame runtime code. `get_common_framework_status` covers CommonUI, CommonGame, UIExtension, CommonUser, CommonLoadingScreen, GameSettings, GameplayMessageRouter, ModularGameplayActors, and GameSubtitles. Evidence: `Docs/testing/2026-06-30-monolith-common-framework-ui.md`.
- [x] Phase 2 Category A — Activatable Lifecycle (8 actions): create_activatable_widget, create_activatable_stack, create_activatable_switcher, configure_activatable, push/pop/get_state [RUNTIME], set_activatable_transition (dv.253)
- [x] Phase 2 Category B — Buttons + Styling (9 actions): convert_button_to_common, configure_common_button, create_common_button/text/border_style, apply_style_to_widget, batch_retheme, configure_common_text/border (dv.256)
- [x] Phase 2 Category C — Input/Actions/Glyphs (7 actions): create_input_action_data_table, add_input_action_row, bind_common_action_widget, create_bound_action_bar, get/set_input_type [RUNTIME], list_platform_input_tables (dv.257)
- [x] Phase 3 Category D — Navigation/Focus (5 actions): set_widget_navigation, set_initial_focus_target, force_focus [RUNTIME], get_focus_path [RUNTIME], request_refresh_focus [RUNTIME] (dv.260)
- [x] Phase 3 Category E — Lists/Tabs/Groups (7 actions): setup_common_list_view, create_tab_list_widget, register_tab [RUNTIME], create_button_group [RUNTIME], configure_animated_switcher, create_widget_carousel, create_hardware_visibility_border (dv.261)
- [x] Phase 4 Categories F+G+I (10 actions): configure_numeric_text, configure_rotator, create_lazy_image, create_load_guard, show_common_message [RUNTIME], configure_modal_overlay, enforce_focus_ring, wrap_with_reduce_motion_gate, set_text_scale_binding, apply_high_contrast_variant (dv.262)
- [x] Phase 5 Category H — Audit + Lint (4 actions): audit_commonui_widget, export_commonui_report, hot_reload_styles [RUNTIME, EXPERIMENTAL], dump_action_router_state [RUNTIME, EXPERIMENTAL] (dv.266)
- [x] Infrastructure collateral: `MonolithIndexLog.h` moved Private → Public (transitive include bug exposed)
- [x] Version bump 0.13.2 → 0.14.0 in both `Monolith.uplugin` and `MonolithCoreModule.h`

#### MonolithUI M0.5 — Testing Pending (M0.5.1)

- [ ] **Functional testing** — All 50 CommonUI actions need PIE-session test pass. Editor automation tests for Categories A/B/C/D/E/F/G/H/I.
- [ ] **Conditional compilation test** — re-verify clean compile with `MONOLITH_RELEASE_BUILD=1` env var (WITH_COMMONUI=0 path).
- [ ] **Bridge integration test** — author a minimal main menu WBP via the new actions end-to-end (keystone test for Claude Design bridge M1).
- [ ] **Category A runtime interaction tests** — push-during-transition, pop-while-empty, push-modal-over-modal, clear-during-transition.

#### MonolithUI M0.5 — Known Limitations (shipped, documented)

- `convert_button_to_common` does NOT auto-transfer UButton children to UCommonButtonBase — UCommonButtonBase uses internal widget tree, not AddChild. Callers must rewire manually.
- `set_initial_focus_target` requires the target WBP to expose a `DesiredFocusTargetName` or `InitialFocusTargetName` FName UPROPERTY and override `NativeGetDesiredFocusTarget`. Action errors out if neither property exists.
- `show_common_message` is fire-and-forward — async result-binding (Yes/No) requires dialog WBP to handle internally. No MCP-side delegate routing yet.
- `dump_action_router_state` cannot read `UCommonUIActionRouterBase::CurrentInputLocks` (private-transient in engine). Engine PR candidate (M0.7).

---

### MonolithAudio Module — 81 Actions, Phases 0-2 COMPLETE (2026-04-08)

- [x] Phase 0 — Core CRUD & Batch (35 actions): Asset CRUD (15) for SoundAttenuation, SoundClass, SoundMix, SoundConcurrency, SoundSubmix. Query & Search (10) with hierarchy inspection, reference queries, audio health checks. Batch Operations (10) with template application.
- [x] Phase 1 — Sound Cue Graph System (21 actions): Sound Cue CRUD, node management, `build_sound_cue_from_spec` (power action), 5 template cues (random, layered, looping, crossfade, switch), validation, preview, delete.
- [x] Phase 2 — MetaSound Graph System (25 actions): MetaSound Source/Patch creation, Builder API integration, node management, `build_metasound_from_spec` (power action), 4 template MetaSounds (oneshot, ambient, synth, interactive), preset, variables.
- [x] Conditional compilation — MetaSound features wrapped in `#if WITH_METASOUND`. Build.cs auto-detects at `Engine/Plugins/Runtime/Metasound`. Sound Cue + CRUD + batch actions always available.
- [x] Settings toggle — `bEnableAudio` in UMonolithSettings (default: true).
- [x] Skill file — `unreal-audio` skill created in `Plugins/Monolith/Skills/`.

#### MonolithAudio — Testing Pending

- [ ] **Functional testing** — All 81 actions need test pass. Sound Cue graph building, MetaSound Builder API, batch operations, query/search.
- [ ] **MetaSound conditional testing** — Verify clean compile and graceful degradation with WITH_METASOUND=0.
- [ ] **Template cue testing** — Verify all 5 Sound Cue templates produce valid, playable cues.
- [ ] **Template MetaSound testing** — Verify all 4 MetaSound templates produce valid, playable sources.

#### MonolithAudio — Future Work (Phase 3-6, ~69 actions)

- [ ] **Phase 3 — Audio Scene & Environment (~18 actions)** — AudioVolume management, submix effect chain CRUD, ambient sound spawning/configuration, audio component inspection, stereo-spatialized detection, audio coverage analysis. Deps: Synthesis module.
- [ ] **Phase 4 — Audio Modulation & Quartz (~18 actions)** — Control bus/mix/generator CRUD, modulation destinations, Quartz clock management. Conditional on `#if WITH_AUDIOMODULATION`. Quartz actions are PIE-only.
- [ ] **Phase 5 — Analysis & Automation (~20 actions)** — Loudness analysis (LUFS from Asset Registry), silence/clipping detection, memory analysis, batch loudness audit, audio system scaffolding, compression suggestions, audio report generation, manifest export/import. Deps: WaveformTransformations module.
- [ ] **Phase 6 — Middleware Bridges (~13 actions)** — Audio middleware detection (Wwise/FMOD/Steam Audio), native audio plugin config inspection, AudioLink factory enumeration. Wwise bridge (5 actions, `#if WITH_WWISE`), FMOD bridge (5 actions, `#if WITH_FMOD`). Reflection-first approach for version-agnostic integration.

---

### MonolithBlueprint — create_data_asset (2026-04-04)

- [x] `create_data_asset` — Create raw UObject instances (DataAssets, MPCs, PhysicalMaterials, CurveFloats, etc.) without Blueprint wrapper. Resolves class by name with U/A prefix fallback. Guards against abstract, deprecated, Actor-derived, and Blueprint classes. 9/9 tests PASS.

---

### MonolithComboGraph Module — 13 Actions, Phase 1 COMPLETE (2026-03-30)

- [x] Phase 1 — Full module implementation (12 actions): Read (list_combo_graphs, get_combo_graph_info, get_combo_node_effects, validate_combo_graph), Create (create_combo_graph, add_combo_node, add_combo_edge, set_combo_node_effects, set_combo_node_cues), Scaffold (create_combo_ability, link_ability_to_combo_graph, scaffold_combo_from_montages).
- [x] Conditional compilation — `#if WITH_COMBOGRAPH` wraps entire module. Compiles clean with WITH_COMBOGRAPH=1 and WITH_COMBOGRAPH=0.
- [x] Settings toggle — `bEnableComboGraph` in UMonolithSettings.
- [x] Reflection-only integration — uses UObject reflection and UComboGraphFactory, no direct C++ API linkage.
- [x] EdGraph sync — write actions update both runtime and editor graphs.
- [x] Skill file — `unreal-combograph` skill created in `Plugins/Monolith/Skills/` and `.claude/skills/`.

#### MonolithComboGraph — Testing Pending

- [ ] **Functional testing** — All 12 actions need test pass with ComboGraph installed (WITH_COMBOGRAPH=1).
- [ ] **Stub testing** — Verify clean compile and graceful degradation with WITH_COMBOGRAPH=0.
- [ ] **GAS cross-integration** — Test `create_combo_ability` and `link_ability_to_combo_graph` with MonolithGAS actions for end-to-end combo+ability workflow.

#### MonolithComboGraph — Future Work

- [ ] **Branching combos** — Support for input-conditional branching (e.g., light vs heavy follow-up from same node).
- [ ] **Combo graph templates** — Preset templates for common combo patterns (3-hit light chain, heavy finisher, dodge cancel).
- [ ] **Runtime inspection** — PIE-only actions to inspect active combo state on actors.
- [ ] **Batch node operations** — `batch_add_nodes` for creating multiple nodes in one call.

---

### MonolithLogicDriver Module — 66 Actions, Phases 1-4 COMPLETE (2026-04-01)

- [x] Phase 1 — Asset CRUD (8 actions): create_state_machine, list_state_machines, delete_state_machine, compile_state_machine, duplicate, rename.
- [x] Phase 2 — Graph Read/Write (20) + Node Config (8): get_sm_structure, add/remove/connect states and transitions, get/set node properties, auto_arrange_graph. Node class config, transition rules, conduits, colors, entry points.
- [x] Phase 3 — Runtime/PIE (7) + JSON/Spec (5) + Discovery (6): PIE start/stop/step, active state inspection, variable access. build_sm_from_spec (power action), export/import JSON, validate/diff specs. Overview, explain, compare, validate, search.
- [x] Phase 4 — Scaffolding (7) + Component (3) + Text Graph (2): 7 scaffold templates (hello_world, horror_encounter, patrol, dialogue, health, interaction, quest). SM component add/configure/inspect. visualize_sm_as_text (Mermaid), export_sm_as_dot (Graphviz).
- [x] Conditional compilation — `#if WITH_LOGICDRIVER` wraps entire module. 3-location Build.cs detection (project plugins, engine marketplace, engine plugins). Compiles clean with WITH_LOGICDRIVER=1 and WITH_LOGICDRIVER=0.
- [x] Settings toggle — `bEnableLogicDriver` in UMonolithSettings (default: true).
- [x] Reflection-only integration — uses UObject reflection, no direct C++ API linkage against Logic Driver Pro binaries.
- [x] Skill file — `unreal-logicdriver` skill created in `.claude/skills/` and `Plugins/Monolith/Skills/`.

#### MonolithLogicDriver — Testing Complete (2026-04-01)

- [x] **Functional testing** — 17/17 tests PASS, 0 FAIL. 6 bugs found and fixed during testing. All 4 phases implemented.
- [x] **Post-implementation polish** — Factory assertion crash fix, delete→re-create crash fix (CollectGarbage), root graph recursive search, initial state detection fix, state naming fix (OnRenameNode), names-lost-on-recompile fix.

#### MonolithLogicDriver — Future Work

- [ ] **Runtime variable templates** — Scaffold common SM variable patterns (health thresholds, timers, counters).
- [ ] **Batch state operations** — `batch_add_states` for creating multiple states in one call.
- [ ] **Cross-module integration** — Link SM transitions to GAS ability triggers, BT task integration.

---

### MonolithGAS Module — 130 Actions, Phases 1-4 COMPLETE (2026-03-29)

- [x] Phase 1 — Abilities (28 actions): CRUD, grant, activate, cancel, spec handles, instancing, tags, costs, cooldowns.
- [x] Phase 2 — Attributes (20) + Effects (26): Attribute sets, get/set values, derived attributes, init/clamping/replication. GE authoring, duration, modifiers, executions, stacking, period, conditional application.
- [x] Phase 3 — ASC (14) + Tags (10) + Cues (10): ASC inspection/config, granted abilities, active effects, owned tags, replication mode. Tag hierarchy, matching, loose tags, containers, queries. Cue notify CRUD (static + actor), cue params, handler lookup.
- [x] Phase 4 — Targets (5) + Input (5) + Inspect (6) + Scaffold (6): Target data handles, actor selection, confirmation. Enhanced Input binding, input tags. Runtime inspection (PIE-only). Common pattern scaffolding.
- [x] Conditional compilation — `#if WITH_GBA` wraps entire module. Compiles clean with WITH_GBA=1 and WITH_GBA=0.
- [x] Settings toggle — `bEnableGAS` in UMonolithSettings.

#### MonolithGAS — Testing Complete (2026-03-30)

- [x] **Functional testing** — 53/53 tests PASS, 0 FAIL. 12 bugs found and fixed during testing. 8 git commits (32c86d7 through 5639dda). PIE runtime tests all passing. Key fixes: IGameplayTagsEditorModule API for tags, three-tier EnsureAssetPathFree guard, BS_BeingCreated suppression for ASC SCS, AR pre-filter on all GetAsset calls, GAS deep indexer in MonolithIndex.
- [x] **GAS deep indexer** — GASIndexer added to MonolithIndex. `project_query("search")` surfaces GAS assets with rich metadata.
- [x] **Skill file** — `unreal-gas` skill created in `.claude/skills/` and `Plugins/Monolith/Skills/`. 130 actions, 10 categories, workflow examples.

#### MonolithGAS — Remaining Work

- [x] **Runtime summary preflight** — `gas.get_runtime_summary` added (2026-05-19). It reports PIE availability, ASC counts, aggregate ability/effect/tag/attribute-set totals, and bounded actor samples without requiring clients to call the heavier snapshot path first.
- [ ] **Template gaps** — `init_player_stats` and `init_enemy_stats` scaffold templates not yet implemented. Should generate attribute sets + initialization GEs for common game archetypes.
- [ ] **Helper deduplication** — Several helper functions are duplicated across action classes (tag container utilities, effect spec builders). Consolidate into shared `MonolithGASHelpers`.
- [ ] **Type-safe reflection** — Attribute set property access uses string-based reflection. Investigate `FGameplayAttribute` direct property pointer for safer access.
- [ ] **Discover enhancement** — `monolith_discover("gas")` should include GAS-specific workflow hints and category groupings in the response metadata.

---

### MonolithMesh Module — 242 Actions (197 core + 45 experimental town gen), ALL 22 PHASES COMPLETE + Proc Geo Overhaul + Procedural Town Generator + Fix Plans v2-v5 (2026-03-30)

- [x] Phase 0-12 — Original 111 actions compiled and tested.
- [x] Phase 13-22 — Expansion 76 actions compiled (lights, volumes, horror intel, tech art, sublevels, context props, proc geo, presets, encounters, polish).
- [x] Proc Geo Overhaul — 5 new actions (list_cached_meshes, clear_cache, validate_cache, get_cache_stats, create_blueprint_prefab). Sweep-based thin walls, auto-collision, collision-aware prop placement, proc mesh caching, blueprint prefabs, door trim frames, floor-aware spawning, human-scale defaults. 187 → 192 actions.
- [x] Procedural Town Generator — 45 new actions across 11 sub-projects (SP1-SP10). Grid-based buildings, floor plans, facades, roofs, city blocks, spatial registry, auto-volumes, terrain adaptation, architectural features, debug views, room furnishing. 196 → 241 mesh actions, 639 → 684 total.
- [x] Fix Plan v2 — 20 fixes across 3 phases (geometry, floor plan, integration). Stair angles, switchbacks, corridor/door widths, per-floor assignment, aspect ratios, footprint validation, exterior entrance guarantee, building_context/wall_openings on arch features, validate_building action. 241 → 242 mesh actions, 684 → 685 total.
- [x] Fix Plan v5 — 7 fixes (2026-03-30): facade reorder, boolean isolation, wall alignment, door clamp, window density, template variety, furniture placement. Town gen still has fundamental geometry issues (wall misalignment, room separation).
- [x] **Town gen marked EXPERIMENTAL** (2026-03-30) — `bEnableProceduralTownGen = false` by default in UMonolithSettings. 45 town gen actions only registered when enabled. Core mesh actions (197) always available.

#### Procedural Town Generator — Fix Plan v2 Follow-Up (2026-03-28)

- [ ] **Door mesh placement** — Place closed door meshes in doorways for horror pacing (doors block sightlines, create tension before opening)
- [ ] **Interior lighting policy** — Define per-room-type lighting defaults for generated buildings (e.g., flickering fluorescent in corridors, warm lamp in bedrooms, no light in storage). Wire into `furnish_room` or separate `light_building` action
- [ ] **Per-building retry in orchestrator** — When `generate_floor_plan` fails for one building in a `create_city_block` call, retry that building with adjusted params instead of failing the entire block
- [ ] **Multiple entrances for large buildings** — Buildings above a size threshold should have 2+ exterior entrances (front + back door, or front + fire exit). Currently only one entrance guaranteed
- [ ] **Batch validation action** — `validate_block` to run `validate_building` across all buildings in a block and return aggregate results

#### Procedural Town Generator — Follow-Up (2026-03-28)

- [ ] **Integration test: full city block** — Generate a complete city block with `create_city_block({buildings: 4, genre: "horror", seed: 42})` and verify all SPs work together end-to-end (SP1 grid → SP2 floor plan → SP3 facade → SP4 roof → SP5 orchestrator → SP6 registry → SP7 volumes → SP10 furnishing)
- [ ] **Performance profiling on 4-8 building blocks** — Profile generation time, mesh actor count, volume count, navmesh build time against the performance budget (target: <30s for 4 buildings, <50 mesh actors, <100 volumes)
- [ ] **Additional building archetypes** — mansion, warehouse, church, school. Add to `Saved/Monolith/BuildingArchetypes/`
- [ ] **Additional facade styles** — Art Deco, Industrial, Modern. Add to `Saved/Monolith/FacadeStyles/`
- [ ] **SP11: Street network generation** — Connecting multiple blocks into a coherent street grid. Road hierarchy (main street, side street, alley), intersections, traffic flow patterns. Would enable full neighborhood-scale generation

#### MonolithMesh — Release / Wiki TODO

- [ ] **Wiki major update** — All 192 mesh actions need wiki documentation. Current wiki covers original modules only.
- [ ] **Genre Preset Authoring Guide** — Dedicated wiki page (or `PRESET_AUTHORING.md`) explaining how LLMs/users create presets for OTHER genres (fantasy, sci-fi, detective, cozy). Include:
  - Room template JSON format with examples
  - Storytelling pattern format (element types, radial distribution)
  - Acoustic profile format (surface absorption/transmission/loudness)
  - Tension profile format (factor weights, threshold mappings)
  - Prop kit format (items, relative positions, spawn_chance)
  - Step-by-step: "How to create a Fantasy Dungeon preset pack"
  - How to test presets via MCP before distributing
  - How to export/import/share preset packs
- [ ] **specs/SPEC_MonolithMesh.md** — Full 241-action reference with param schemas (currently has Phase 1-4 section + proc geo overhaul + Procedural Town Generator SP1-SP10 + validate_building)
- [ ] **MCP.md mesh_query docs** — Tool reference for mesh_query namespace
- [ ] **README update** — Feature highlight for MonolithMesh in the plugin README

#### MonolithMesh — Known Issues / Polish

- [x] **Placement overlap warnings:** DONE — `create_primitive`, `_batch`, `import_layout`, `scatter_props` warn when overlapping existing actors
- [ ] **BP_MonolithBlockoutVolume** — Construction Script for per-RoomType wireframe colors (cosmetic polish)
- [ ] **Discover workflow hints** — Add workflow metadata to `monolith_discover("mesh")` response
- [ ] **Tier-based discover filtering** — `monolith_discover("mesh", {"tier": "audio"})` to reduce token usage

#### MonolithMesh — Context-Aware Prop Placement (NEW)

- [ ] **Surface-aware scatter (`scatter_on_surface`)** — Place props ON specific surfaces, not just floors. Detect shelf tops, table tops, cabinet interiors, wall surfaces via downward/directional traces from the surface actor's bounds. "Place 5 books on this shelf" should work.
- [ ] **Disturbance levels (`set_room_disturbance`)** — Apply a disturbance level to placed props in a volume:
  - `"orderly"` — aligned, evenly spaced, upright
  - `"slightly_messy"` — small random offsets, some tilted 5-15 degrees
  - `"ransacked"` — large random offsets, many tipped over (60-90 degree rotations), some on floor
  - `"abandoned"` — like ransacked + props pushed to edges (simulating years of settling)
  Implementation: iterate placed actors in volume, apply progressive random transforms based on disturbance level. Single undo transaction.
- [ ] **Physics prop sleep state (`configure_physics_props`)** — Set SimulatePhysics=true + bStartAwake=false on designated actors. They sit in their placed position until bumped by player or woken by gameplay event. Essential for interactive horror (knock over a stack of cans, send bottles rolling).
- [ ] **Gravity-settle placement (`settle_props`)** — For each prop: enable physics, simulate for N frames (or until velocity < threshold), capture settled transform, disable physics. Gives organic "someone dropped this here" look. UE5 has `UPhysicsSimulationComponent` or we can use `FPhysicsInterface::Simulate()`.
- [ ] **Themed prop kits** — JSON definitions like room templates but for prop sets: "office_desk_clutter" (papers, pens, mug, monitor, keyboard), "hospital_tray" (syringe, bandages, clipboard). Each kit defines items with relative positions to an anchor point. `place_prop_kit` action.
- [ ] **Wall/ceiling scatter** — Extend `scatter_props` with `surface` param: "floor" (current), "wall" (horizontal trace outward, align to wall normal), "ceiling" (upward trace), "shelf" (trace to named actor's top surface). Paintings, clocks, chains, cables.

#### MonolithMesh — Procedural Geometry (Overhaul COMPLETE 2026-03-28)

See `Docs/plans/2026-03-28-procedural-geometry-wishlist.md` for original wishlist.
- [x] `create_parametric_mesh` — chair, table, shelf, door_frame, stairs, etc. (~15 types). Human-scale defaults (stairs 90/28/18cm, doors 90cm, floor 3cm)
- [x] `create_structure` — room, corridor, L-corridor, T-junction, stairwell, vent. Sweep-based thin walls, door/window/vent trim frames
- [x] `create_building_shell` — multi-story from 2D footprint polygon
- [x] `create_maze` — recursive backtracker, Prim's, Eller's, binary tree
- [x] `create_pipe_network` — sweep circle along path with elbow joints
- [x] `create_fragments` — Voronoi fracture for destruction
- [x] `create_terrain_patch` — Perlin/simplex noise heightmap
- [x] `create_horror_prop` — barricade, debris pile, cage, coffin, broken wall
- [x] Proc mesh caching system — hash-based manifest, `use_cache`/`auto_save` on all proc gen actions, 4 cache management actions (list_cached_meshes, clear_cache, validate_cache, get_cache_stats)
- [x] `create_blueprint_prefab` — dialog-free Blueprint creation from placed actors (replaces create_prefab for MCP workflows)
- [x] Auto-collision on all saved meshes (collision param: auto/box/convex/complex_as_simple/none)
- [x] Collision-aware prop placement (collision_mode: none/warn/reject/adjust on scatter actions)
- [x] Floor-aware spawning (snap_to_floor param, SweepSingle box traces)

#### MonolithMesh — Unified Wishlist from 3 Perspectives (Level Design + Horror + Tech Art)

**P0 — Would use EVERY session (~15 actions, ~80 hours)**

Level Design:
- [ ] `place_light` / `set_light_properties` — spawn + modify lights directly. Lighting IS horror. `suggest_light_placement` advises but can't act
- [ ] `find_replace_mesh` — swap every instance of mesh X with mesh Y across level. Blockout→art pass essential
- [ ] `spawn_volume` — trigger/kill/pain/blocking/nav_modifier/audio/post_process volumes. Can't build a functional level without these
- [ ] `get_actor_properties` / `copy_actor_properties` — read arbitrary component properties, copy settings between actors

Horror Design:
- [ ] `predict_player_paths` — THE multiplier. Auto-generate weighted paths (shortest/safest/curious/cautious) so every horror action works without manual path input
- [ ] `evaluate_spawn_point` — composite score: visibility delay, audio cover, lighting, escape proximity, path commitment
- [ ] `suggest_scare_positions` — optimal locations for scripted events along a path. Scores anticipation, visibility, timing, player agency
- [ ] `evaluate_encounter_pacing` — check spacing/intensity across multiple encounters. Flag back-to-back with no breather

Tech Art:
- [ ] `set_actor_material` / `swap_material_in_level` — assign materials to placed actors. Bridges mesh + material systems
- [ ] `analyze_texel_density` / `compare_texel_density_in_region` — texels/cm consistency. #1 visual quality issue
- [ ] `find_instancing_candidates` / `convert_to_hism` — "SM_Pipe appears 47 times, convert to HISM, save 46 draw calls"
- [ ] `auto_generate_lods` + `set_lod_screen_sizes` — one-shot LOD pipeline for meshes

**P1 — High value, weekly use (~20 actions, ~100 hours)**

Level Design:
- [ ] `build_navmesh` — horror analysis depends on navmesh but we can't trigger a rebuild
- [ ] `manage_sublevel` — create/load/unload/move_actors_to. Horror streaming needs this
- [ ] `place_blueprint_actor` — spawn BP actors with exposed properties ("locked door needing ward key")
- [ ] `select_actors` — control editor selection, focus camera. AI↔human handoff
- [ ] `snap_to_surface` — drop actors onto geometry with normal alignment

Horror Design:
- [ ] `design_encounter` — compose spawn + patrol + exits + sightline breaks + audio zones in one call
- [ ] `suggest_patrol_route` — generate navmesh routes per AI archetype (stalker/patrol/ambusher)
- [ ] `analyze_ai_territory` — score region as AI territory: hiding spots, patrol options, ambush positions
- [ ] `evaluate_safe_room` — score a room: defensible entrance? good lighting? sound isolation?
- [ ] `analyze_level_pacing_structure` — macro tension→release→tension rhythm across entire level

Tech Art:
- [ ] `import_mesh` — FBX/glTF import with settings. Every mesh enters the project here
- [ ] `analyze_material_cost_in_region` — cross-module: mesh placement × material instruction counts
- [ ] `fix_mesh_quality` — auto-fix: remove degenerate tris, weld verts, fix normals (extends analyze_mesh_quality)
- [ ] `set_mesh_collision` / `auto_collision` — write collision back to assets
- [ ] `analyze_lightmap_density` — lightmap texel density + resolution management

**P2 — Workflow accelerators (~15 actions, ~60 hours)**

- [ ] `randomize_transforms` — variation pass on rotation/scale/offset for organic feel
- [ ] `place_spline` — mesh/cable splines for pipes, cables, railings
- [ ] `get_level_actors` — filtered enumeration (class, tag, sublevel, mesh wildcard)
- [ ] `measure_distance` — quick measurement between actors or points
- [x] `create_blueprint_prefab` — replaces `create_prefab` for MCP workflows (dialog-free via HarvestBlueprintFromActors). DONE 2026-03-28
- [ ] `generate_scare_sequence` — procedural scare events with variety + escalation
- [ ] `analyze_framing` — camera composition scoring (leading lines, focal points)
- [ ] `evaluate_monster_reveal` — score reveal quality: silhouette, backlight, distance, partial visibility
- [ ] `validate_horror_intensity` — cap tension for accessibility mode, remove jump scares
- [ ] `generate_hospice_report` — full level audit: intensity caps, rest spacing, cognitive load, one-handed playability

**P3 — Quality of life + future (~10 actions)**

- [ ] `validate_naming_conventions` / `batch_rename_assets`
- [ ] `generate_proxy_mesh` / `setup_hlod`
- [ ] `analyze_texture_budget` — texture streaming pool analysis
- [ ] GeometryScript expansions: `mesh_extrude`, `mesh_subdivide`, `mesh_combine`, `mesh_separate_by_material`, `compute_ao`
- [ ] Integration hooks: AI Director data feed, GAS tension effects, telemetry feedback loop

#### MonolithMesh — Genre Preset System (NEW — Extensibility)

The mesh module ships horror defaults (storytelling patterns, room templates, acoustic profiles, tension scoring). But the SYSTEMS are genre-agnostic. Other LLMs or users should be able to create their own presets for fantasy, sci-fi, detective noir, cozy sim, etc.

**Authoring actions:**
- [ ] `list_storytelling_patterns` — List all available patterns (built-in + user-created)
- [ ] `create_storytelling_pattern` — Author a new pattern JSON: element types, radial distribution, size ranges, spawn chance. Save to `Saved/Monolith/Patterns/`. Example: fantasy "tavern_brawl" (overturned chairs, spilled mead puddles, broken mug fragments)
- [ ] `list_acoustic_profiles` — List available surface acoustic profiles
- [ ] `create_acoustic_profile` — Author a new acoustic property set for a genre. Fantasy: stone_dungeon, wooden_tavern, crystal_cave. Sci-fi: metal_hull, glass_viewport, organic_hive
- [ ] `create_tension_profile` — Define what tension factors mean for a genre. Horror: short sightlines = dread. Fantasy: open vistas = wonder, narrow caves = claustrophobia. Different scoring weights per genre
- [ ] `list_prop_kits` — List available themed prop kits
- [ ] `create_prop_kit` — Author a themed prop kit JSON: items with relative positions, size ranges, spawn chances. "tavern_table_setting" (plates, mugs, candles, food), "sci-fi_console" (screens, buttons, cables)

**Preset packs (JSON bundles in Saved/Monolith/Presets/):**
- [ ] `export_genre_preset` — Bundle all templates + patterns + acoustic profiles + tension config + prop kits into a single distributable JSON/ZIP
- [ ] `import_genre_preset` — Load a genre preset pack, merging with existing presets
- [ ] Ship starter packs: `horror_default` (current), document format for community presets

**Documentation for preset authors:**
- [ ] Write a `PRESET_AUTHORING.md` guide explaining:
  - Room template JSON format (furniture entries, position_pct, size_range)
  - Storytelling pattern JSON format (elements, radial distribution, intensity scaling)
  - Acoustic profile JSON format (surface types, absorption, transmission, loudness)
  - Tension profile JSON format (factor weights, threshold mappings)
  - Prop kit JSON format (items, relative positions, spawn_chance)
  - How to test presets via MCP before distributing
  - Examples for fantasy, sci-fi, detective, cozy genres

**Why this matters:** Monolith becomes not just a tool but a PLATFORM. Horror is Leviathan's genre, but the open-source plugin serves everyone. LLMs working on fantasy games load a fantasy preset pack and immediately have genre-appropriate spatial awareness, environmental storytelling, and room templates. Community-driven expansion without touching C++.

---

### Niagara Expansion — 31 New Actions (2026-03-25)

- [x] **Dynamic Inputs (5):** `list_dynamic_inputs`, `get_dynamic_input_tree`, `remove_dynamic_input`, `get_dynamic_input_value`, `get_dynamic_input_inputs`
- [x] **Emitter Management (3):** `rename_emitter`, `get_emitter_property`, `list_available_renderers`
- [x] **Renderer Configuration (3):** `set_renderer_mesh`, `configure_ribbon`, `configure_subuv`
- [x] **Event Handlers (3):** `get_event_handlers`, `set_event_handler_property`, `remove_event_handler`
- [x] **Simulation Stages (3):** `get_simulation_stages`, `set_simulation_stage_property`, `remove_simulation_stage`
- [x] **Module Outputs (1):** `get_module_output_parameters`
- [x] **Niagara Parameter Collections (5):** `create_npc`, `get_npc`, `add_npc_parameter`, `remove_npc_parameter`, `set_npc_default`
- [x] **Effect Types (3):** `create_effect_type`, `get_effect_type`, `set_effect_type_property`
- [x] **Utilities (5):** `get_available_parameters`, `preview_system`, `diff_systems`, `save_emitter_as_template`, `clone_module_overrides`
- [x] **`move_module` rewrite** — Complete rewrite of move_module to preserve input overrides
- [x] **7 bug fixes** — type fallback warning, spawn shape duplicate, NPC namespace mismatch, + 4 others found during testing
- [x] **Full test pass** — 40/40 PASS, 0 FAIL, 3 SKIPPED. Total Niagara actions: 65 → 96.

---

### Material Function Full Suite (2026-03-25)

- [x] **export_function_graph** — Full graph export with connections, properties, switch details (#7)
- [x] **set_function_metadata** — Update description, categories, library exposure
- [x] **delete_function_expression** — Remove expression(s) from function
- [x] **update_material_function** — Recompile cascade to referencing materials
- [x] **create_function_instance** — Create MFI with parent + optional overrides
- [x] **set_function_instance_parameter** — Set param overrides on MFI
- [x] **get_function_instance_info** — Read MFI parent chain + overrides (11 param types)
- [x] **layout_function_expressions** — Auto-arrange function graph
- [x] **rename_function_parameter_group** — Rename param group
- [x] **create_material_function type param** — Support MaterialLayer/MaterialLayerBlend creation

---

### MonolithUI Module (2026-03-22)

- [x] **NEW: MonolithUI module** — IMPLEMENTED (2026-03-22). 42 actions across 8 action classes in the `ui` namespace (`ui_query` tool). Widget blueprint CRUD, slot layout, HUD/menu templates, styling, UMG animation CRUD, binding inspection, settings scaffolding, accessibility.
- [x] **MonolithUI — MCP testing** — DONE (2026-03-22). All 42 actions PASS. Full test results in TESTING.md.
- [ ] **MonolithUI — Phase 3 deprecation warnings** — `GetBindings` and `FMovieSceneBinding::GetName` may generate deprecation warnings in UE 5.7. Audit and update call sites.
- [x] **MonolithUI — agent skill file** — DONE (2026-03-22). `unreal-ui` skill created in `.claude/skills/` and `Plugins/Monolith/Skills/`, added to `interface-architect` agent definition.
- [x] **MonolithUI — bUIEnabled settings toggle** — DONE (2026-03-22). `UMonolithSettings::bUIEnabled` wired to registration gating (confirmed in SPEC_CORE.md settings table).

---

### Recently Fixed (2026-03-14)

- [x] **NEW: Niagara `set_system_property`** — ADDED (2026-03-14). Sets a system-level property (WarmupTime, bDeterminism, etc.) via reflection.
- [x] **NEW: Niagara `set_static_switch_value`** — ADDED (2026-03-14). Sets a static switch value on a module.
- [x] **NEW: Niagara `list_module_scripts`** — ADDED (2026-03-14). Searches available Niagara module scripts by keyword. Returns matching script asset paths.
- [x] **NEW: Niagara `list_renderer_properties`** — ADDED (2026-03-14). Lists editable properties on a renderer via reflection. Params: `asset_path`, `emitter`, `renderer`.
- [x] **Niagara DI class name auto-prefix** — FIXED (2026-03-14). `set_module_input_di` and `get_di_functions` now auto-resolve DI class names: both `NiagaraDataInterfaceCurve` and `UNiagaraDataInterfaceCurve` are accepted (U prefix stripped/added as needed).
- [x] **Niagara `get_module_inputs` returns DI curve data** — FIXED (2026-03-14). Now returns actual FRichCurve key data for DataInterface curve inputs, not just the DI class name.
- [x] **LinearColor/vector defaults deserialization** — FIXED (2026-03-14). `get_module_inputs` correctly deserializes LinearColor and vector default values from string-serialized JSON fallback, no longer returns zeroed values.
- [x] **Material `disconnect_expression` targeted disconnection** — FIXED (2026-03-14). Now supports disconnecting a specific connection via optional `input_name`/`output_name` params, instead of always disconnecting all connections on the expression.
- [x] **Blueprint `add_node` aliases and K2_ prefix fallback** — FIXED (2026-03-14). `add_node` now resolves common node class aliases (e.g. `CallFunction`, `VariableGet`) and automatically tries the `K2_` prefix for function call nodes when the bare name doesn't resolve.
- [x] **Niagara `list_renderers` returns `type` short name** — FIXED (2026-03-14). The `type` field now returns the short renderer class name (e.g. `SpriteRenderer`) instead of the full UClass path.

### Recently Fixed

- [x] **Niagara `set_emitter_property` SimTarget — "Data missing please force a recompile"** — FIXED (2026-03-13). Raw field assignment on `SimTarget` skipped `PostEditChangeVersionedProperty`, so `MarkNotSynchronized` was never called and `RequestCompile(false)` saw unchanged hash → skipped compilation. Fix: call `PostEditChangeVersionedProperty` + `RebuildEmitterNodes` + `SynchronizeOverviewGraphWithSystem` after SimTarget change. Same pattern applied to `bLocalSpace` and `bDeterminism`.
- [x] **Niagara `list_emitters` missing emitter GUID** — FIXED (2026-03-13). Added `"id": Handle.GetId().ToString()` to the emitter listing. Users now have a stable round-trip token.
- [x] **Niagara `get_system_diagnostics` — NEW ACTION** — Added (2026-03-13). Returns compile errors, warnings, renderer/SimTarget incompatibility, GPU+dynamic bounds warnings, and per-script stats (op count, registers, compile status). Also added `CalculateBoundsMode` to `set_emitter_property`.

- [x] **Niagara `create_system` + `add_emitter` — emitters don't persist** — FIXED (2026-03-13). Replaced raw `System->AddEmitterHandle()` with `FNiagaraEditorUtilities::AddEmitterToSystem()` which calls `RebuildEmitterNodes` + `SynchronizeOverviewGraphWithSystem` after adding the handle. Also added `SavePackage` call in both `HandleCreateSystem` and `HandleAddEmitter`. Custom emitter names applied post-add via `SetName()`.
- [x] **Niagara `create_system_from_spec` — fails with `failed_steps:1`** — FIXED (2026-03-13). Added synchronous `RequestCompile(true)` + `WaitForCompilationComplete()` after each emitter add in the spec flow, before modules are added. Removed redundant async `RequestCompile(false)` from `HandleAddEmitter`. Also added error message capture — failed sub-operations now report in an `"errors"` array instead of silent `FailCount++`.
- [x] **All MCP tools return stale in-memory objects after asset recreate** — FIXED (2026-03-13). `LoadAssetByPath` now queries `IAssetRegistry::GetAssetByObjectPath()` + `FAssetData::GetAsset()` first (reflects editor ground truth), falling back to `StaticLoadObject` only if the Asset Registry has no record. Prevents stale `RF_Standalone` ghosts from shadowing recreated assets.
- [x] **Niagara `set_module_input_value` — namespace warnings on compile** — FIXED (2026-03-13). `MatchedFullName` was assigned the stripped short name instead of the full `Module.`-prefixed name from `In.GetName()`. Same fix applied to `HandleSetModuleInputBinding`. Both now pass the full name to `FNiagaraParameterHandle::CreateAliasedModuleParameterHandle`.
- [x] **Registry-level required param validation** — ADDED (2026-03-13). `FMonolithToolRegistry::Execute()` now validates required params from schema before dispatching to the handler. Checks all schema keys marked `required: true`, skips `asset_path` (handled by `GetAssetPath()` with aliases). Returns error listing missing + provided keys.
- [x] **Niagara param name aliases** — ADDED (2026-03-13). All module write actions (`set_module_input_value`, `set_module_input_binding`, `set_module_input_di`, `set_curve_value`, `remove_module`, `move_module`, `set_module_enabled`, `get_module_inputs`, `get_module_graph`) now accept `module_name` and `module` as aliases for the canonical `module_node` param. `input_name` accepted as alias for `input`.
- [x] **`set_expression_property` PostEditChange fix** — FIXED (2026-03-13). Was calling `PostEditChange()` (no args), which didn't rebuild the material graph. Now calls `PostEditChangeProperty(FPropertyChangedEvent(Prop))` with the actual property so `MaterialGraph->RebuildGraph()` fires and the editor display updates correctly.
- [x] **Auto-recompile + PostEditChange on 4 material write actions** — FIXED (2026-03-13). `set_material_property`, `create_material`, `delete_expression`, and `connect_expressions` now all call `Mat->PreEditChange(nullptr)` + `Mat->PostEditChange()` to trigger recompile and push changes through the material graph system.
- [x] **`tools/list` embeds per-action param schemas** — IMPLEMENTED (2026-03-13). `FMonolithHttpServer::HandleToolsList()` now builds the `params` property description with per-action param documentation in `*name(type)` format (`*` = required). AI clients can see all param names and types from the MCP tool list without calling `monolith_discover` first.

### Minor

- [x] **Material `validate_material` false positive islands** — FIXED (2026-03-09). Added MP_MaterialAttributes + 6 missing properties to AllMaterialProperties, seeded BFS from UMaterialExpressionMaterialAttributeLayers. 0 false positives on standard materials. Layer-blend materials still have a known limitation (implicit layer system connections not traversable via pin graph).
- [x] **Blueprint `get_execution_flow` matches comments before events** — FIXED (2026-03-09). Two-pass FindEntryNode: Pass 1 checks events/functions (prefers K2Node_Event, K2Node_FunctionEntry), Pass 2 is fuzzy fallback that skips EdGraphNode_Comment.

---

## Unimplemented Features (stubs in code)

- [x] **Niagara `create_module_from_hlsl`** — DONE (2026-03-15). Creates standalone NiagaraScript with CustomHlsl node, typed I/O pins, ParameterMap flow. Bypasses unexported APIs via UPROPERTY reflection + Signature-driven pin creation.

- [x] **Niagara `create_function_from_hlsl`** — DONE (2026-03-15). Same path as module, `ENiagaraScriptUsage::Function` with direct typed pin wiring.

- [ ] **SSE streaming** — DEFERRED. `MonolithHttpServer.cpp` SSE endpoint returns a single event and closes. Comment: "Full SSE streaming will be implemented when we need server-initiated notifications."
  - **File:** `Source/MonolithCore/Private/MonolithHttpServer.cpp` (~line 232)

- [x] **C++ source indexer — native port complete** — DONE (2026-03-15). `MonolithSource` module now runs a native C++ indexer via `UMonolithSourceSubsystem`. The Python tree-sitter indexer (`Scripts/source_indexer/`) is legacy and no longer invoked. New action: `trigger_project_reindex` for incremental project-only C++ re-index. Offline query now via standalone `monolith_query.exe` — the previous `UMonolithQueryCommandlet` has been removed.

- [x] **Python indexer: capture full class/struct definitions** — FIXED (2026-03-08). Added UE macro preprocessor that strips UCLASS/USTRUCT/UENUM/UINTERFACE, *_API, GENERATED_BODY() before tree-sitter parsing. 62,059 definitions now captured (was near-zero).

- [x] **Source index: ancestor traversal** — FIXED (2026-03-08). Inheritance table now has 37,010 entries across 34,444 classes. AActor→UObject, APawn→AActor, ACharacter→APawn all working.

---

## Feature Improvements

### MonolithEditor — Runtime Toolset Parity Polish

- [x] **Automation status/history (2026-05-19)** — `editor.run_automation_tests` now records `run_id`, state/progress counters, compact recent history, and last-run summaries. Added `editor.get_automation_run_status`, `editor.stop_automation_tests` structured no-op cancellation contract, and `editor.list_automation_history`.
- [x] **Live Coding diagnostics (2026-05-19)** — `editor.get_live_coding_diagnostics` returns normalized Live Coding state, availability/enabled flags, fresh compile log excerpts, diagnostic freshness, and explicit no-UBT-scrape metadata.
- [x] **Automation async cancellation (2026-07-16)** — `editor.start_automation_tests` / `poll_automation_tests` now use the engine-owned `AutomationController` frame loop for latent and PIE tests, and `stop_automation_tests` performs owned, bounded two-phase cancellation. The synchronous compatibility runner retains `stop_status="unsupported_cancel"` by design.

### Platform

- [ ] **Mac/Linux support** — DEFERRED (Windows-only project). All build-related actions are `#if PLATFORM_WINDOWS` guarded. Live Coding is Windows-only. Update system is Windows-only.

### MonolithIndex / MonolithSource — CRG-Inspired Navigation (P0 IMPLEMENTED 2026-05-16; Projection Cache 2026-05-17)

Spec source: `Docs/specs/SPEC_MonolithIndex.md`, `Docs/specs/SPEC_MonolithSource.md`, and `Docs/API_REFERENCE.md`. Additive actions over the **existing** `dependencies` / `"references"` + `inheritance` graphs. The CRG `nodes`/`edges` idea is now adopted as rebuildable derived SQLite `crg_*` projection/cache tables owned by Monolith; native `assets` and `symbols` tables remain source-of-truth. UBT project-editor build green; `Monolith.IndexGuard.*` automation tests pass headless.

- [x] **P0 — `project.{impact_radius,health,repair_fts,risk_score,review_context}`** — `FMonolithIndexReview` bounded BFS over `dependencies` (`GetDependenciesForAsset`/`GetReferencersOfAsset`), query-time risk, v2-aware health, execute-gated FTS rebuild (gated on `IsIndexing()`).
- [x] **P0 — `source.{impact_radius,health,repair_fts,risk_score,review_context}`** — `FMonolithSourceReview` traversal over quoted `"references"` + `inheritance` (`include` excluded). `source_fts` plain FTS5 → `target=source` degrades to reindex guidance. `review_context` is a new action, distinct from single-item `context.build_attachment`.
- [x] **Projection cache — `project.repair_crg_cache` / `source.repair_crg_cache`** — execute-gated rebuild creates `crg_nodes`, `crg_edges`, `crg_node_metrics`, `crg_meta`; `health` validates table/index/parity/cache meta; `risk_score` uses cached `risk_score` when present and falls back safely on cache miss.
- [x] **P0 — tests/docs** — extended `MonolithIndexQueryTests.cpp` / `MonolithSourceQueryTests.cpp` (`Monolith.IndexGuard.*`); synced `SPEC_MonolithIndex.md` / `SPEC_MonolithSource.md`; corrected the stale `MonolithSourceSchema.h:5` `Scripts/source_indexer` comment.
- [x] **Review Extensions RX-2 (offline CRG cache parity)** — READ PARITY IMPLEMENTED 2026-05-17; OFFLINE WRITER IMPLEMENTED 2026-05-18. Offline `monolith_query.exe` `risk_score` reads `crg_node_metrics` (rebuilt caches use `scoring_version=3` after RX-7, per-item `cache.status=hit`) with safe query-time fallback; offline `health` emits the editor `crg:*` checks (table/parity/orphan/cache_version/scoring_version), cache-absent is `info` (no regression). Offline `repair_crg_cache --execute` rebuilds only derived `crg_*` rows from existing indexed DB tables; dry-run remains read-only.
- [x] **Review Extensions RX-1 (`detect_changes`)** — OFFLINE IMPLEMENTED 2026-05-17; EDITOR PARITY IMPLEMENTED 2026-05-18. Live `source.detect_changes` / `project.detect_changes` and `monolith_query <source|project> detect_changes <path...>` map changed paths to symbols/assets, reuse RX-2 cached risk (or query-time fallback), add bounded depth-1 impact, advisory heuristic test-gaps (source), risk-ordered priorities, minimal/standard. `changed_paths` is VCS-agnostic; no P4/git shell-out. Spec: `Docs/specs/SPEC_MonolithSource.md` and `Docs/specs/SPEC_MonolithIndex.md`.
- [x] **Review Extensions RX-1.1 (`detect_changes` line precision)** — IMPLEMENTED 2026-05-18 (editor + offline). Source `detect_changes` accepts optional `changed_ranges` (`[{path,ranges:[[s,e]...]}]`) and/or unified `diff_text`; with ranges a changed file maps only to symbols overlapping a changed line range (CRG `changes.py:204` rule via the shared `FMonolithSourceDatabase::ParseUnifiedDiffRanges` parser), else unchanged file-level behavior. Offline CLI adds `--diff-file`/`--diff-stdin`/`--ranges=path:s-e`. Closes the single behavioral-fidelity gap from the 2026-05-18 CRG cross-check; project side N/A (binary assets); no VCS shell-out. Spec: `Docs/specs/SPEC_MonolithSource.md`. Tests: `Monolith.IndexGuard.Source.DetectChanges{LinePrecision,NoRangeRegression,DiffParse,Robustness}`.
- [x] **Review Extensions RX-3 (`find_unused`)** — OFFLINE IMPLEMENTED 2026-05-17; EDITOR PARITY IMPLEMENTED 2026-05-18. Live `source.find_unused` / `project.find_unused` and `monolith_query <source|project> find_unused`: advisory dead-symbol / orphan-asset detection with confidence+reasons, recall-first default (`min_confidence` / `--min-confidence=low`), UE reflection/automation/root exclusions. Never mutates.
- [x] **Review Extensions RX-4 (`snapshot` / `diff_snapshots`)** — EDITOR PARITY IMPLEMENTED 2026-05-18; OFFLINE PARITY IMPLEMENTED 2026-05-18. Live `source.snapshot` / `project.snapshot` and offline `monolith_query <source|project> snapshot` store derived CRG node/edge manifests in `crg_snapshots` only with explicit execute; live and offline `diff_snapshots` compare stored/current manifests read-only with capped new/removed node/edge samples and `summary_counts`. No cache rebuild or VCS shell-out.
- [x] **Review Extensions RX-5 (`pre_merge_check`)** — EDITOR PARITY IMPLEMENTED 2026-05-18; OFFLINE PARITY IMPLEMENTED 2026-05-18. Live `source.pre_merge_check` / `project.pre_merge_check` and offline `monolith_query <source|project> pre_merge_check` compose health, detect_changes, and optional find_unused into an advisory pre-merge `decision`, `checks[]`, and `findings[]`. Read-only, VCS-agnostic input; no P4/git shell-out.
- [x] **Review Extensions RX-7 (UE-domain sensitivity scoring)** — IMPLEMENTED 2026-05-17. Project/source/editor/offline risk scoring and CRG cache rebuilds now add a bounded sensitivity factor for save/network/session/store/auth/crypto/exec/file surfaces, surface `raw_counts.sensitivity`, and expect `crg_meta.scoring_version=3`.
- [x] **Review Extensions RX-8 (`review_hotspots`)** — IMPLEMENTED 2026-05-17. Added editor + offline `source.review_hotspots` / `project.review_hotspots` with capped fan-in/fan-out/risk/large ranking, optional advisory questions, and no community/betweenness dependency.
- [x] **Review Extensions RX-6 (`context.bridge_asset_symbols`)** — EDITOR PARITY IMPLEMENTED 2026-05-18; OFFLINE PARITY IMPLEMENTED 2026-05-18. Read-only bridge between ProjectIndex assets and EngineSource symbols with heuristic `confidence` + `reasons[]`; no cross-sqlite mutation, no P4/git shell-out. Offline CLI contract is `monolith_query context bridge_asset_symbols` over `ProjectIndex.db` + `EngineSource.db`, same `links[]`/`warnings[]` shape.
- [ ] **P1/P2 (deferred)** — edge confidence and wiki/PR-bot export.

### MonolithIndex — Incremental Indexer Remaining Work

- [ ] **Implement IndexScoped() for individual sentinels** — DependencyIndexer, AnimationIndexer, and other sentinels currently fall back to per-asset `IndexAsset()`. Implement proper `IndexScoped()` for batch efficiency.
- [ ] **Batched frame-budget deep indexing for >10 assets** — When more than 10 assets need deep indexing, spread work across frames with a per-frame time budget to avoid hitches.
- [ ] **External file deletion detection (FDirectoryWatcherModule)** — AR callbacks don't fire for files deleted outside the editor. Hook `FDirectoryWatcherModule` to detect and reconcile external deletions.

### Niagara Module — Improvements

- [x] **Niagara emitter selectors accept numeric index** — IMPLEMENTED (2026-05-18). Core Niagara actions already accepted `list_emitters` numeric index strings; `auto_layout`'s duplicate emitter resolver now follows the same GUID/name/index contract.

### Animation Module — Wishlist

Priority features identified for future waves:

- [x] **Wave 1 — Read actions (EASY, ~8 actions):** DONE (2026-03-10)
- [x] **Wave 2 — Notify CRUD (EASY, ~4 actions):** DONE (2026-03-10)
- [x] **Wave 3 — Curve CRUD (EASY, ~5 actions):** DONE (2026-03-10)
- [x] **Wave 4 — Skeleton sockets (EASY, ~4 actions):** DONE (2026-03-10) — expanded to 6 actions (added set_blend_space_axis, set_root_motion_settings)
- [x] **Wave 5 — Creation + editing (MODERATE, ~6 actions):** DONE (2026-03-10)
- [x] **Wave 6 — PoseSearch/Motion Matching (MODERATE, ~5 actions):** DONE (2026-03-10)
- [x] **Wave 7 — Anim Modifiers + Composites (MODERATE, ~5 actions):** DONE (2026-03-10)
- [ ] **Wave 8 — IKRig + Control Rig (HARD, ~6 actions):** `get_ikrig_info`, `add_ik_solver`, `get_retargeter_info`, `get_control_rig_info` — requires IKRig/ControlRig module dependencies
- [ ] **Deferred — ABP write ops (HARD):** State machine structural writes (add state/transition) require Blueprint graph mutation, high complexity

---

## Documentation

- [ ] **CI pipeline** — Per Phase 6 plan

---

## Completed

- [x] Core infrastructure (HTTP server, registry, settings, JSON utils, asset utils)
- [x] All 15 domain modules compiling clean on UE 5.7
- [x] SQLite FTS5 project indexer with 14 indexers (Blueprint, Material, Generic, Dependency, Animation, Niagara, DataTable, Level, GameplayTag, Config, Cpp, UserDefinedEnum, UserDefinedStruct, InputAction)
- [x] Python tree-sitter engine source indexer
- [x] Auto-updater via GitHub Releases
- [x] 10 Claude Code skills (including unreal-build and unreal-ui)
- [x] Templates (.mcp.json, CLAUDE.md)
- [x] README, LICENSE, ATTRIBUTION
- [x] HTTP body null-termination fix
- [x] Niagara graph traversal fix (emitter shared graph)
- [x] Niagara emitter lookup hardening (case-insensitive + fallbacks)
- [x] Source DB WAL -> DELETE journal mode fix
- [x] Asset loading 4-tier fallback
- [x] SQL schema creation (BEGIN/END depth tracking for triggers)
- [x] Reindex dispatch fix (FindFunctionByName -> StartFullIndex + UFUNCTION)
- [x] Asset loading crash fix (removed FastGetAsset from background thread)
- [x] Animation `remove_bone_track` — now uses `RemoveBoneCurve(FName)` per bone + child traversal (2026-03-07)
- [x] MonolithIndex `last_full_index` — added `WriteMeta()` call, guarded with `!bShouldStop` (2026-03-07)
- [x] Niagara `move_module` — rewires stack-flow pins only, preserves override inputs (2026-03-07)
- [x] Editor `get_build_errors` — uses `ELogVerbosity` enum instead of substring matching (2026-03-07)
- [x] MonolithIndex SQL injection — all 13 insert methods converted to `FSQLitePreparedStatement` (2026-03-07)
- [x] Animation `LogTemp` -> `LogMonolith` (2026-03-07)
- [x] Editor `CachedLogCapture` dangling pointer — added `ClearCachedLogCapture()` in ShutdownModule (2026-03-07)
- [x] MonolithSource vestigial outer module — flattened structure, deleted stub (2026-03-07)
- [x] Session expiry / reconnection — Removed session tracking entirely. Sessions stored no per-session state and only caused bugs when server restarted. Server is now fully stateless. (2026-03-07)
- [x] Claude tools fail on first invocation — Fixed transport type mismatch in .mcp.json ("http" → "streamableHttp") and fixed MonolithSource stub that wasn't registering actions. (2026-03-07)
- [x] Module enable toggles — settings now checked before registering actions (2026-03-07)
- [x] MCP package CLI cleanup — removed abandoned scaffold (2026-03-07)
- [x] Material action count alignment — skill updated to match C++ reality (2026-03-07)
- [x] Animation action count alignment — skill updated to match C++ reality (2026-03-07)
- [x] Niagara action count alignment — skill updated to match C++ reality (2026-03-07)
- [x] Config action schema documentation — `explain_setting` convenience mode documented in skill (2026-03-07)
- [x] Niagara `reorder_emitters` safety — proper change notifications added (2026-03-07)
- [x] `diff_from_default` INI parsing — rewritten with GConfig/FConfigCacheIni (2026-03-07)
- [x] Config `diff_from_default` enhancement — now compares all 5 config layers (2026-03-07)
- [x] Live Coding trigger action (`editor.live_compile`) — fully implemented (2026-03-07)
- [x] Cross-platform update system — tar/unzip support added (2026-03-07)
- [x] Hot-swap plugin updates — delayed file swap mechanism implemented (2026-03-07)
- [x] Remove phase plan .md files from Source/ — moved to Docs/plans/ (2026-03-07)
- [x] AnimationIndexer — AnimSequence, AnimMontage, BlendSpace indexing (2026-03-07)
- [x] NiagaraIndexer — NiagaraSystem, NiagaraEmitter deep indexing (2026-03-07)
- [x] DataTableIndexer — DataTable row indexing (2026-03-07)
- [x] LevelIndexer — Level/World actor indexing (2026-03-07)
- [x] GameplayTagIndexer — Tag hierarchy indexing (2026-03-07)
- [x] ConfigIndexer — INI config indexing (2026-03-07)
- [x] CppIndexer — C++ symbol indexing (2026-03-07)
- [x] Deep asset indexing — safe game-thread loading strategy implemented (2026-03-07)
- [x] Incremental indexing — delta updates from file change detection (2026-03-07)
- [x] Asset change detection — hooked into Asset Registry callbacks (2026-03-07)
- [x] API reference page — auto-generated API_REFERENCE.md with 119 actions (2026-03-07)
- [x] Contribution guide — CONTRIBUTING.md created (2026-03-07)
- [x] Changelog — CHANGELOG.md created (2026-03-07)
- [x] Clean up MCP/ package — removed abandoned CLI scaffold (2026-03-07)
- [x] `find_callers` / `find_callees` param name fix — `"function"` → `"symbol"` (2026-03-07)
- [x] `read_file` param name fix — `"path"` → `"file_path"` (2026-03-07)
- [x] `read_file` path normalization — forward slash → backslash for DB suffix matching (2026-03-07)
- [x] `get_class_hierarchy` forward-declaration filtering — prefer real definitions over `class X;` (2026-03-07)
- [x] `ExtractMembers` rewrite — brace depth tracking for Allman-style UE code (2026-03-07)
- [x] `get_recent_logs` — accepts both `"max"` and `"count"` param names (2026-03-07)
- [x] `search_config` category filter — changed param read from `"file"` to `"category"` (2026-03-07)
- [x] `get_section` category name resolution — accepts `"Engine"` not just `"DefaultEngine"` (2026-03-07)
- [x] SQLite WAL → DELETE — belt-and-suspenders fix: C++ forces DELETE on open + Python indexer never sets WAL (2026-03-07)
- [x] Source DB ReadOnly → ReadWrite — WAL + ReadOnly silently returns 0 rows on Windows (2026-03-07)
- [x] Reindex absolute path — `FPaths::ConvertRelativePathToFull()` on engine source + shader paths (2026-03-07)
- [x] MonolithHttpServer top-level param merge — params alongside `action` were silently dropped, now merged (2026-03-07)
- [x] UE macro preprocessor — strips UCLASS/USTRUCT/UENUM/UINTERFACE, *_API, GENERATED_BODY() before tree-sitter parsing (2026-03-08)
- [x] Source indexer --clean flag — deletes existing DB before reindexing (2026-03-08)
- [x] Inheritance resolution — 37,010 links across 34,444 classes, full ancestor chains working (2026-03-08)
- [x] Diagnostic counters — definitions/forward_decls/with_base_classes/inheritance_resolved/failed printed after indexing (2026-03-08)
- [x] Preprocessor in ReferenceBuilder — consistent AST for cross-reference extraction (2026-03-08)
- [x] Auto-updater rewrite — tasklist polling, move retry loop 10x3s, errorlevel fix, cmd /c quoting, DelayedExpansion, xcopy /h, rollback rmdir. Windows end-to-end tested v0.4.0→v0.5.0 (2026-03-08)
- [x] Release script `Scripts/make_release.ps1` — sets `"Installed": true` in zip for BP-only users (2026-03-08)
- [x] BP-only support — release zips work without rebuild for Blueprint-only projects (2026-03-08)
- [x] GitHub Wiki — 11 pages: Installation, Tool Reference, Test Status, Auto-Updater, FAQ, Changelog, etc. (2026-03-08)
- [x] Indexer auto-index deferred to `IAssetRegistry::OnFilesLoaded()` — was running too early, only indexing 193/9560 assets (2026-03-09)
- [x] Indexer sanity check — if < 500 assets indexed, skip writing `last_full_index` so next launch retries (2026-03-09)
- [x] Indexer `bIsIndexing` reset in `Deinitialize()` to prevent stuck flag (2026-03-09)
- [x] Index DB changed from WAL to DELETE journal mode (2026-03-09)
- [x] Niagara `trace_parameter_binding` — fixed missing OR fallback for `User.` prefix (2026-03-09)
- [x] Niagara `get_di_functions` — fixed reversed class name pattern, now tries `UNiagaraDataInterface<Name>` (2026-03-09)
- [x] Niagara `batch_execute` — fixed 3 op name mismatches, old names kept as aliases (2026-03-09)
- [x] Niagara actions now accept `asset_path` (preferred) with `system_path` as backward-compat alias (2026-03-09)
- [x] Niagara `duplicate_emitter` accepts `emitter` as alias for `source_emitter` (2026-03-09)
- [x] Niagara `set_curve_value` accepts `module_node` as alias for `module` (2026-03-09)
- [x] NEW: Niagara `list_emitters` action — returns emitter names, index, enabled, sim_target, renderer_count (2026-03-09)
- [x] NEW: Niagara `list_renderers` action — returns renderer class, index, enabled, material (2026-03-09)
- [x] Animation state machine names stripped of `\n` — clean names like "InAir" not "InAir\nState Machine" (2026-03-09)
- [x] Animation `get_state_info` validates required params (machine_name, state_name) (2026-03-09)
- [x] Animation state machine matching changed from fuzzy Contains() to exact match (2026-03-09)
- [x] Animation `get_nodes` now accepts optional `graph_name` filter (2026-03-09)
- [x] NEW: Blueprint `get_graph_summary` — lightweight graph overview (id/class/title + exec connections only, ~10KB vs 172KB) (2026-03-09)
- [x] Blueprint `get_graph_data` now accepts optional `node_class_filter` param (2026-03-09)
- [x] Blueprint `get_variables` now reads default values from CDO (was always empty) (2026-03-09)
- [x] Blueprint indexer CDO fix — same default value extraction fix applied to BlueprintIndexer (2026-03-09)
- [x] Material `export_material_graph` now accepts `include_properties` and `include_positions` params (2026-03-09)
- [x] Material `get_thumbnail` now accepts `save_to_file` param (2026-03-09)
- [x] Niagara `get_compiled_gpu_hlsl` auto-compiles system if HLSL not available (2026-03-09)
- [x] Niagara `User.` prefix stripped in get_parameter_value, trace_parameter_binding, remove_user_parameter, set_parameter_default (2026-03-09)
- [x] Per-action param schemas in `monolith_discover()` output — all current 121 blueprint actions have param documentation (2026-03-09; count reconciled after duplicate-action cleanup on 2026-05-27)
- [x] Niagara `get_module_inputs` — types now use `PinToTypeDefinition` instead of default Vector4f (2026-03-09)
- [x] Niagara `get_ordered_modules` — usage filter now works with shorthands ("spawn", "update"), returns error on invalid values, returns all stages when omitted (2026-03-09)
- [x] Niagara `get_renderer_bindings` — clean JSON output (name/bound_to/type) instead of raw UE struct dumps (2026-03-09)
- [x] Niagara `get_all_parameters` — added optional `emitter` and `scope` filters (2026-03-09)
- [x] Animation `get_transitions` — cast to `UAnimStateNodeBase*` instead of `UAnimStateNode*`, resolves conduit names. Added from_type/to_type fields (2026-03-09)
- [x] Material `validate_material` — seeds BFS from `UMaterialExpressionCustomOutput` subclasses + `UMaterialExpressionMaterialAttributeLayers`, added MP_MaterialAttributes + 6 missing properties. 0 false positives on standard materials (2026-03-09)
- [x] Blueprint `get_execution_flow` — two-pass FindEntryNode: Pass 1 prefers events/functions, Pass 2 fuzzy fallback skips comments (2026-03-09)
- [x] Blueprint `get_graph_summary` all-graphs mode — returns all graphs when graph_name empty, single graph when specified (2026-03-09)
- [x] **CRITICAL: Hot-swap updater deletes Saved/** — swap script moved entire plugin dir to backup, then only preserved .git/.github. EngineSource.db (1.8GB) and ProjectIndex.db were destroyed on cleanup. Fixed: both static .bat script and C++ template (Windows + Mac/Linux) now preserve Saved/ alongside .git (2026-03-10)
- [x] Material `build_material_graph` class lookup — `FindObject<UClass>(nullptr, ClassName)` always returned null. Changed to `FindFirstObject<UClass>(ClassName, NativeFirst)` with U-prefix fallback. Short names like "Constant", "VectorParameter" now resolve correctly (2026-03-10)
- [x] Material `disconnect_expression` missing material outputs — `disconnect_outputs=true` only iterated other expressions' inputs, never checked material output properties. Added `GetExpressionInputForProperty()` loop over `MaterialOutputEntries` (2026-03-10)
- [x] NEW: Material `create_material` — creates UMaterial asset at path with Opaque/DefaultLit/Surface defaults (2026-03-10)
- [x] NEW: Material `create_material_instance` — creates UMaterialInstanceConstant from parent material with parameter overrides (2026-03-10)
- [x] NEW: Material `set_material_property` — sets material properties (blend_mode, shading_model, etc.) via UMaterialEditingLibrary::SetMaterialUsage (2026-03-10)
- [x] NEW: Material `delete_expression` — deletes expression node by name from material graph (2026-03-10)
- [x] NEW: Material `get_material_parameters` — returns scalar/vector/texture/static_switch parameter arrays with values, works on UMaterial and UMaterialInstanceConstant (2026-03-10)
- [x] NEW: Material `set_instance_parameter` — sets scalar/vector/texture/static_switch parameters on MIC (2026-03-10)
- [x] NEW: Material `recompile_material` — forces material recompile via UMaterialEditingLibrary::RecompileMaterial (2026-03-10)
- [x] NEW: Material `duplicate_material` — duplicates material asset to new path via UEditorAssetLibrary::DuplicateAsset (2026-03-10)
- [x] NEW: Material `get_compilation_stats` — returns sampler count, texture estimates, UV scalars, blend mode, expression count. API corrected for UE 5.7 FMaterialResource (2026-03-10)
- [x] NEW: Material `set_expression_property` — sets properties on expression nodes (e.g., DefaultValue on scalar param) (2026-03-10)
- [x] NEW: Material `connect_expressions` — wires expression outputs to expression inputs or material property inputs. Supports expr-to-expr and expr-to-material-property (2026-03-10)
- [x] **Niagara `get_module_inputs` returns all input types** — IMPLEMENTED (2026-03-11). Now uses engine's `FNiagaraStackGraphUtilities::GetStackFunctionInputs` with `FCompileConstantResolver`. Returns floats, vectors, colors, data interfaces, enums, bools — not just static switch pins. Output uses short names (no `Module.` prefix).
- [x] **Niagara `batch_execute` missing write ops** — FIXED (2026-03-11). Dispatch table now covers all 23 write op types. Previously missing 8: `remove_user_parameter`, `set_parameter_default`, `set_module_input_di`, `set_curve_value`, `reorder_emitters`, `duplicate_emitter`, `set_renderer_binding`, `request_compile`.
- [x] **Niagara `set_module_input_di` validation** — FIXED (2026-03-11). Now validates input exists and is DataInterface type before applying. Rejects nonexistent inputs and non-DI type inputs with descriptive errors. `config` param now accepts JSON object (not just string).
- [x] **Niagara `create_system_from_spec` functional** — FIXED (2026-03-11). Was broken — now uses `UNiagaraSystemFactoryNew::InitializeSystem` for proper system creation.
- [x] **Niagara `FindEmitterHandleIndex` auto-select removed** — FIXED (2026-03-11). Was silently auto-selecting the single emitter when a specific non-matching name was passed. Now requires the name to match if one is provided, returning a clear error instead.
- [x] **Niagara write actions accept both short and prefixed input names** — IMPLEMENTED (2026-03-11). `set_module_input_value`, `set_module_input_binding`, `set_module_input_di`, `set_curve_value` all accept both `InputName` (short) and `Module.InputName` (prefixed) forms.
- [x] **CRASH: `add_virtual_bone` no bone validation** — FIXED (2026-03-10). Added `FReferenceSkeleton::FindBoneIndex()` validation for both source and target bones before calling `AddNewVirtualBone()`. Previously created bogus virtual bones with non-existent bones, causing array OOB crash on skeleton access.
- [x] **`set_notify_time` / `set_notify_duration` reject AnimMontage** — FIXED (2026-03-10). Changed `LoadAssetByPath<UAnimSequence>` to `LoadAssetByPath<UAnimSequenceBase>` so montages and composites are accepted. Also made `set_notify_duration` error message include `(total: N)` to match `set_notify_time`.
- [x] **`remove_virtual_bones` false success for non-existent bones** — FIXED (2026-03-10). Now validates each bone name against actual virtual bones before removal. Returns `not_found` array and errors if all names are invalid.
- [x] **`delete_montage_section` allows deleting last section** — FIXED (2026-03-10). Added guard: if montage has only 1 section remaining, returns error "Cannot delete the last remaining montage section".
- [x] **`add_blendspace_sample` generic error on skeleton mismatch** — FIXED (2026-03-10). Added skeleton comparison before adding sample, returns descriptive error naming both skeletons when they don't match.
- [x] **Animation Waves 1-7: 39 new actions** — IMPLEMENTED (2026-03-10). Total animation module: 62 actions + 5 PoseSearch = 67. Waves: 8 read actions, 4 notify CRUD, 5 curve CRUD, 6 skeleton+blendspace, 6 creation+montage, 5 PoseSearch, 5 modifiers+composites. Build errors fixed: BlendParameters private, GetTargetSkeleton removed, UMirrorDataTable forward-decl, GetBoneAnimationTracks deprecated, OpenBracket FText.
- [x] **Blueprint module upgrade: 6 → 46 actions** — IMPLEMENTED (2026-03-13). Added 40 new write actions across 5 categories: Variable CRUD (7), Component CRUD (6), Graph Management (9), Node & Pin Operations (6), Compile & Create (5). Also expanded Read Actions to 13 (added get_components, get_component_details, get_functions, get_event_dispatchers, get_parent_class, get_interfaces, get_construction_script). Total plugin actions: 177 → 217.
- [x] **Offline CLI (`monolith_offline.py`)** — IMPLEMENTED (2026-03-13). Pure Python (stdlib only) CLI that queries `EngineSource.db` and `ProjectIndex.db` directly without the editor running. 14 actions across 2 namespaces: `source` (9 actions, mirrors `source_query`) and `project` (5 actions, mirrors `project_query`). Read-only, zero footprint, zero dependencies. Fallback for when MCP/editor is unavailable. Location: `Saved/monolith_offline.py`.
- [x] **NEW: MonolithUI module** — IMPLEMENTED (2026-03-22). New module at `Source/MonolithUI/`. 42 actions in `ui` namespace (`ui_query` tool). 8 action classes: FMonolithUIActions (7), FMonolithUISlotActions (3), FMonolithUITemplateActions (8), FMonolithUIStylingActions (6), FMonolithUIAnimationActions (5), FMonolithUIBindingActions (4), FMonolithUISettingsActions (5), FMonolithUIAccessibilityActions (4).
- [x] **NEW: MonolithMesh module** — IMPLEMENTED (2026-03-27). New module at `Source/MonolithMesh/`. 46 actions in `mesh` namespace (`mesh_query` tool). 4 action classes: FMonolithMeshInspectionActions (12), FMonolithMeshSceneActions (8), FMonolithMeshSpatialActions (11), FMonolithMeshBlockoutActions (15). MeshCatalogIndexer added to MonolithIndex.
- [x] **Incremental indexer (3-layer architecture)** — IMPLEMENTED (2026-03-28). Startup hash-based delta engine, live AR callbacks on 2s timer, forced full reindex fallback. <1s startup with no changes, ~14K assets hashed in ~20ms.
- [x] **Schema v2 migration** — IMPLEMENTED (2026-03-28). Added `saved_hash` column (Blake3 FIoHash hex) to assets table, `schema_version` meta key. Auto-migrates via `PRAGMA table_info` check + `ALTER TABLE`.
- [x] **Live AR callbacks** — IMPLEMENTED (2026-03-28). Batched Asset Registry delegates (OnAssetsAdded, OnAssetsRemoved, OnAssetRenamed, OnAssetsUpdatedOnDisk) drained on 2s timer with dedup and transactional apply.
- [x] **Plugin content scope fix (bInstalled filter)** — FIXED (2026-03-28). Replaced `bInstalled` filter with explicit path enumeration. DrawCallReducer and NiagaraDestructionDriver now indexed. MeshCatalogIndexer paths corrected.
- [x] **MCP reindex action (incremental default + force param)** — IMPLEMENTED (2026-03-28). `monolith_reindex()` defaults to incremental mode; `force=true` triggers full wipe-and-rebuild.
- [x] **NEW: MonolithGAS module** — IMPLEMENTED (2026-03-29), TESTED (2026-03-30). 130 actions in `gas` namespace (`gas_query` tool). 53/53 tests PASS, 12 bugs fixed (8 commits: 32c86d7-5639dda). Key fixes: IGameplayTagsEditorModule API, EnsureAssetPathFree 3-tier guard, BS_BeingCreated suppression, AR pre-filter, GAS deep indexer. Conditional on `#if WITH_GBA`. Total plugin: 685 → 815 actions, 11 → 12 domains, 14 → 15 MCP tools.
- [x] **NEW: MonolithAI module** — IMPLEMENTED (2026-04-01). 229 actions in `ai` namespace (`ai_query` tool). 24K lines C++, 30 files. Covers BT, BB, State Trees, EQS, Smart Objects, AI Controllers, Perception, Navigation, Runtime/PIE, Scaffolds, Discovery, Advanced. Crown jewels: `build_behavior_tree_from_spec`, `build_state_tree_from_spec`. Conditional on `#if WITH_STATETREE` + `#if WITH_SMARTOBJECTS` (required); `#if WITH_MASSENTITY` + `#if WITH_ZONEGRAPH` (optional).
- [x] **GAS GameplayCue registered-tag coverage audit** — IMPLEMENTED (2026-05-19). `gas::validate_cue_coverage` accepts `include_registered_tags_without_notifies` to walk the registered `GameplayCue` tag subtree and return sorted `registered_tags_without_notifies` rows for tags that have no matching GameplayCue Notify handler. Verified by diff hygiene, static CI, UE 5.7 plugin build, and `MONOLITH_RELEASE_BUILD=1` release guard.
- [ ] **MonolithGAS DataAsset workflow improvements** — SPEC drafted (2026-05-31). Implemented so far: `gas.describe_data_asset_gas_profile`, `gas.validate_data_asset_gas_profile`, `gas.set_data_asset_gas_fields`, `gas.start_event_cue_probe`, `gas.stop_event_cue_probe`, `gas.expect_event_cue`, runtime-summary readiness fields, manifest DataAsset profile embedding, ability Blueprint release/delay validation, source-scan release/cancel validation, and generated input binding Completed/Canceled release paths. Latest verification records a final project-editor build, 8/8 `Monolith.GAS` automation, expected `monolith_query.exe gas` rejection after rollback, and rendered PIE screenshot inspection. Remaining work: activation-policy fixtures, richer ProjectIndex-backed reports via existing `project`/`bridge` routing if needed, and PIE fixture evidence for an actual gameplay event/cue scenario. A dedicated offline `monolith_query.exe gas` namespace is intentionally deferred to preserve Monolith CLI routing cohesion.
- [x] **NEW: MonolithLogicDriver module** — IMPLEMENTED (2026-04-01), TESTED (2026-04-01). 66 actions in `logicdriver` namespace (`logicdriver_query` tool). 17/17 tests PASS, 6 bugs fixed. Reflection-only integration against Logic Driver Pro precompiled binaries. Conditional on `#if WITH_LOGICDRIVER`. 3-location Build.cs detection.
- [x] **Python-to-C++ port: MCP proxy** — IMPLEMENTED (2026-04-01). `monolith_proxy.exe` replaces `monolith_proxy.py` as the primary MCP stdio-to-HTTP proxy. Standalone C++ binary, zero dependencies. Source at `Tools/MonolithProxy/`, build via `build.bat`. Supports env vars: `MONOLITH_URL`, `MONOLITH_SPLIT_EDITOR_QUERY`, `MONOLITH_EDITOR_ACTION_ALLOWLIST`, `MONOLITH_EDITOR_ACTION_DENYLIST`. Python proxy retained as legacy fallback.
- [x] **Python-to-C++ port: offline query tool** — IMPLEMENTED (2026-04-01). `monolith_query.exe` replaces both `MonolithQueryCommandlet` (removed) and `monolith_offline.py` (deprecated) as the primary offline query tool. Standalone C++ binary, no UE runtime, instant startup. Source at `Tools/MonolithQuery/`, build via `build.bat`. 14 actions across source and project namespaces. `monolith_offline.py` retained as legacy fallback.
- [x] **AssetEditing benchmark v5.1 — adversarial hardening + practical expansion** — IMPLEMENTED (2026-06-18). Adversarial audit (35 confirmed findings) + live baseline (`baseline-v5-pre` = 0.967, all 9 failures benchmark/contract defects). Closed scoring holes: `error_path` scores on the offending identifier (`specific_tokens`); `edit_execute` creates are delete-first op_chains (read-back proves THIS run); `add_node` reads back the returned node id (`${self}`); `set_component_property` reads back the value; `add_replicated_variable` asserts the replication flag; CDO writes assert property+value; `duplicate_reject` requires a clean delete-reset first call; empty-`contains` read-backs and empty weighted categories are now build-time errors. New executed coverage: `negative_compile` (set a variable to an invalid struct type → require `error_count>0`; the dangling-VariableGet/duplicate-event breaks were verified silently tolerated by UE), data-pin (typed value) wiring, `set_pin_default` pin literals, add→remove→read-gone delete round-trips. Re-weighted schema-fetch 0.19→0.10, `edit_execute` 0.26→0.31, +`negative_compile` 0.05. Benchmark defects fixed: `bIsActive`→`bComponentActive` (collided with `UActorComponent` native), `validate_blueprint` lint-report shape now scored correctly, interface tasks use the full `/Game` path. Task count 295→305. Files: `Scripts/asset_editing_benchmark.py`, `Benchmarks/AssetEditing/{tasks.jsonl,manifest.json,METRICS.md,README.md,RESULTS.md,test_blueprints.md}`.
- [x] **Blueprint interface-resolver fix (implement_interface / get_interface_functions / remove_interface)** — IMPLEMENTED (2026-06-18). Added `MonolithBlueprintInternal::ResolveInterfaceClass` (resolves C++ interfaces by class name AND Blueprint Interface assets by `/Game` path or short asset name via `<Name>_C` lookup and an AssetRegistry load) and wired it into the three handlers. Previously only native `FindFirstObject<UClass>` name permutations resolved, so a Blueprint Interface's natural identifier returned "Interface class not found" despite the help text promising asset-name support. Surfaced by the AssetEditing benchmark (BEB-083/BEB-155); verified live (both `BPI_TestInterface` and the full path now resolve to 7 functions). Files: `Source/MonolithBlueprint/Private/{MonolithBlueprintInternal.h,MonolithBlueprintGraphActions.cpp,MonolithBlueprintActions.cpp}`.
- [ ] **AssetEditing benchmark — deferred v5.1 practical-coverage items** — DEFERRED (2026-06-18). High-ROD-but-lower-priority audit items not yet landed: `reparent_blueprint`/`reparent_component` executed tasks (need a per-run scratch reset + nested-SCS parent/child read-back); `scaffold_interface_implementation` executed task; `copy_nodes` executed task (divergent `source_asset`/`target_asset` params); `disconnect_pins` (needs a mid-chain connect assertion so a no-op connect+no-op disconnect cannot pass trivially); `promote_pin_to_variable`/`add_macro`/`add_timeline`/`add_local_variable`/`set_variable_type`/`set_node_position` executed tasks. P2/P3 polish: rename `absent:[old_name]` checks, duplicate-* count-delta verification, per-graph node-count growth ceiling, and a per-run cleanup of the `BPB_NegB/F/G/H` and scratch test blueprints under `/Game/Benchmarks/`. Handler boy-scout (status corrected 2026-06-18 after live verification): `HandleDuplicateComponent` silently auto-suffixes an explicit existing `new_name` (`ProbeDst`->`ProbeDst_1`) — **FIXED this changelist** (reject an explicit collision, mirror the add/rename guard; the auto-`_Copy` default still suffixes for convenience). The earlier claim that `add_variable` silently no-ops on an inherited-native-name shadow was **WRONG / unverified** — a live test created `bIsActive` on `BC_TestComponent` successfully, so there is no shadow-collision bug; the fixture var was renamed `bComponentActive` only as a precaution and remains valid. Still open: add a `duplicate_reject`-style regression for `duplicate_component`/`duplicate_graph` (the current `edit_execute` read-back uses a substring `contains`, which a `_1` suffix would pass).
- [x] **Blueprint event-dispatcher action param inconsistency** — FIXED (2026-06-18, surfaced by AssetEditing v5.1 run; verified live). `add_event_dispatcher` takes `name` but `remove_event_dispatcher`/`set_event_dispatcher_params` required `dispatcher_name`, so an agent that added with `name` and removed with `name` got "Missing required param(s): [dispatcher_name]". Fix: `dispatcher_name` is now `Optional` and `name` is registered as an `Optional` alias on both `remove_event_dispatcher` and `set_event_dispatcher_params` (the handler bodies already fell back to `name`, but the param-schema validation layer rejected the call first — registration change was required). Verified: `remove_event_dispatcher name=…` and `dispatcher_name=…` both succeed.
- [x] **Blueprint `duplicate_component` silent-suffix on explicit collision** — FIXED (2026-06-18, surfaced + verified live). `duplicate_component component_name=X new_name=Y` where `Y` already exists silently created `Y_1`; now it returns "A component named 'Y' already exists" when `new_name` is explicit (mirrors the `add_component`/`rename_component` guard). The auto-generated `<name>_Copy` default still suffixes for convenience. `Source/MonolithBlueprint/Private/MonolithBlueprintComponentActions.cpp`. Also a known `setup_fixtures` lesson: fixture edits must `save_asset` to persist across the headless `-nullrhi` editor's intermittent crash/reboot (added in v5.1), otherwise in-memory-only edits (e.g. a renamed variable) are lost before the scored run.
- [x] **source.get_signature over-matches qualified Class::Method** — RESOLVED 2026-06-19 (P4 CL814, live-verified). HandleGetSignature + the shared ResolveFirstSignature (verify_signature) now capture the `Class::` scope and make the column fast-path class-strict (only `QualifiedName == Symbol` overloads); because `EscapeFTS` emits AND-semantics (`"Class"* "Method"*`), the qualified FTS fallback yields no chunks for a nonexistent class → not-found, while templated/inline symbols like `FString::Printf` still resolve. A made-up `Class::Method` now returns `No signature found` + a did-you-mean of the classes that DO declare the method, never a foreign class's signature (`UWorld::SpawnActor` resolves to the real `AActor* UWorld::SpawnActor(...)`, not Monolith's `FMonolithActionResult::SpawnActor`).
- [ ] **SourceIndex benchmark: call-graph symbol lists assume gameplay/GAS source is indexed** — DEFERRED (2026-06-18). Several find_callers/find_callees/find_overrides symbol lists use gameplay/GAS qualified methods (ACharacter::Jump, AController::SetPawn, UGameplayAbility::CanActivateAbility, UAbilitySystemComponent::GiveAbility, UAttributeSet::PreAttributeChange, ...) that are NOT in EngineSource.db (the index covers engine source, not all plugin/GAS source), so find_callers returns "No function found matching ..." and the tasks fail — a benchmark-data assumption, not a handler bug (the handler now resolves any indexed qualified symbol; verified set: FString::Printf, UObject::GetName/GetClass/StaticClass/IsA, UWorld::GetTimerManager/GetGameInstance, FName::ToString, FVector::Size, AActor::BeginPlay/Tick/EndPlay, UActorComponent::*). Fix: replace the unindexed gameplay/GAS symbols with verified-indexed ones (probe find_callers offline first), OR confirm whether the source index SHOULD cover the GameplayAbilities plugin (index-scope question). `_CALLERS_CALLEES_SYMBOLS` was already converted to verified-indexed function symbols (2026-06-18); the remaining lifecycle/GAS lists (source_index_benchmark.py ~892,922,941,955,1023) still need the same treatment.
- [x] **source.risk_score / review_context / impact_radius wrap a not-found as a success envelope** — RESOLVED 2026-06-19 (P4 CL814, live-verified). The shared `WrapReviewResult` helper promotes a `status:"error"` body to a real `FMonolithActionResult::Error` (`isError=true`, code -32602) preserving the body as `error.data` and attaching a `source.search_source` recovery hint/related-action; `status` `ok`/`warning`/`stale` stay successes. Live check: the 3 actions on `ZZ_MadeUp_QQ` now return `isError=true` (status=error, hints=4, related includes `source.search_source`/`monolith.discover`); on `UObject` they stay `isError=false`. This also moves the SourceIndex negative_recovery cells for these actions from `silent_non_error_on_bad_input` (0.0) to a self-correcting structured error (1.0).
- [x] **monolith.discover schema-mode omits the sibling candidates it already computes** — REJECTED 2026-06-19 (false premise). An adversarial verification pass confirmed the claimed `FindSimilarActions` sibling-attachment does not exist in *either* discover mode (`FMonolithCoreTools::HandleDiscover`), so there is no asymmetry to port. No code change; do not re-open.
- [ ] **ParamGuard automation suite: 33/86 pre-existing failures unrelated to dispatch** — TRIAGE (2026-06-19, observed while verifying the asset_path dispatch fix). Running the full `Monolith.ParamGuard` group headless reports 33 failures spanning Audio/AI/GAS/Material/ModelGen/Niagara/ProjectIndex/Blueprint/EditorPreview/MonolithConfig/MonolithUI. Dominant patterns: (a) handlers/actions returning JSON-RPC `-32603` (or `0`) instead of the expected `-32602` (ErrInvalidParams) for malformed/wrong-type params (e.g. MonolithIndexParamGuardTests direct `::Execute` calls expecting `-32602`); (b) missing per-param type validation messages (e.g. GAS `fire_mode must be a string`, Audio `sound_waves`); (c) group-run test isolation noise ("Overwriting existing action: modelgen.*"). NONE bear the asset_path missing_required_param fingerprint and the affected test modules were not rebuilt by the dispatch fix, so these are pre-existing. Triage: re-run failing tests individually to separate genuine handler gaps from group-run interference, then standardize malformed-param rejection on `-32602`.
