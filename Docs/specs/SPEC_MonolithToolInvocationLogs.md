# Monolith — Tool Invocation Daily Logs

**Parent:** [../SPEC_CORE.md](../SPEC_CORE.md)
**Status:** Writer implemented through format v3; proxy/query/action v3 verified; offline reader implemented (`Analyzer/analyze_invocation_logs.py`, contract in [SPEC_MonolithInvocationLogAnalyzer.md](SPEC_MonolithInvocationLogAnalyzer.md))
**Scope:** Proxy, offline query, and editor action invocation diagnostics
**Created:** 2026-05-20
**Doc reconciled with code:** 2026-05-21 (format v3 field pass: proxy/query/action correlation, routing, workflow, timing, and child-process records)

---

## 1. Purpose

Monolith needs local, append-only daily logs that show how agents call Monolith through the MCP proxy, offline query CLI, and live editor action registry. The logs are for local diagnostics and Monolith improvement signals. They are not remote telemetry, not a replacement for returned tool output, and not a replacement for the existing in-memory ToolCall ledger.

This document is the implementation contract and current writer verification record. Format v3 adds explicit row identity, parent/previous linkage, routing context, workflow phase, phase timing, environment context, result shape, and child-process timing on top of the v2 `trace_id` / `span_id` / `return_summary` contract. Daily files are append-only, so a date partition can contain older `format_version` 1 or 2 rows from long-running proxy/editor processes alongside newer v3 rows. Readers must treat `format_version` as a per-record schema discriminator, not as a file-level property.

The current logs collect enough writer-side evidence to answer the target goal: infer what tool an agent called, why it likely chose that route, how it continued after each result, and where time accumulated. The reader is the offline analyzer `Analyzer/analyze_invocation_logs.py` (own spec: [SPEC_MonolithInvocationLogAnalyzer.md](SPEC_MonolithInvocationLogAnalyzer.md)); section 5.2 is the v3 collection contract that readers consume.

The desired daily files live under `Plugins/Monolith/Logs/yyyyMMdd/` by default:

| Surface | File |
|---|---|
| MCP stdio proxy | `Logs/yyyyMMdd/proxy.jsonl` |
| Offline query CLI | `Logs/yyyyMMdd/query.jsonl` |
| Editor action dispatch | `Logs/yyyyMMdd/action.jsonl` |

Each file is JSON Lines: one compact JSON object per invocation, appended sequentially.

## 1.1 Implementation Readiness

The 2026-05-20 source review found the feature practical to implement, but only if the first slice covers the real entrypoints agents use today.

| Area | Finding | Required handling |
|---|---|---|
| Proxy | `Scripts/monolith_proxy.bat` can select Python or Node before any native proxy path. | P0 must implement script proxy logging parity; C++ proxy parity alone is not enough. |
| Query | `monolith_query.exe` writes directly to stdout/stderr and fatal paths can exit early. | Capture/replay streams without changing observed output or exit code, and make fatal dispatch paths loggable. |
| Action | `FMonolithToolRegistry::ExecuteAction` can reject calls before handler dispatch. | Log unknown action, profile block, schema rejection, missing params, strict-param failures, and handler results when action logging is enabled. |
| Locking | Registry locks and log file locks solve different problems. | Do not perform file I/O under the registry lock; use process-local guards plus a shared exclusive `.lock` file create/delete protocol for cross-runtime proxy/query appends. |
| Existing diagnostics | Action audit and `bEnableAdvancedToolCallRecords` are useful but not durable proxy/query/action records. | Keep daily JSONL logging independent and do not change audit or MCP response shapes. |
| Analysis | `agent_signal` fields are inferred hints. | Use them for aggregate Monolith improvements, not as a single-record verdict. |

Writer completion requires all three surfaces plus docs/template sync and the verification gates in section 9. The current source implements the format v3 writer contract, but existing append-only date folders may still contain mixed v1/v2/v3 rows until all long-running proxy/editor processes are restarted. Fresh-process v3 append checks are part of the verification gates below.

## 2. Current Code Facts

| Surface | Current entrypoint | Current behavior |
|---|---|---|
| C++ proxy | `Tools/MonolithProxy/monolith_proxy.cpp` | `handle_tools_call` validates, deduplicates via `record_tool_call`, injects `_monolith_trace_id` and `_monolith_parent_span_id`, forwards requests to the editor HTTP MCP server, and persists `proxy.jsonl`. |
| Script proxy | `Scripts/monolith_proxy.bat`, `Scripts/monolith_proxy.py`, `Scripts/monolith_proxy.js` | The Windows launcher probes Python, `python3`, Node, then `py`; Python and Node emit schema-compatible `proxy.jsonl` records and inject `_monolith_trace_id` plus `_monolith_parent_span_id`. |
| Offline query | `Tools/MonolithQuery/monolith_query.cpp` | `main` captures/replays stdout/stderr, logs namespace/action dispatch and fatal paths, and inherits `MONOLITH_TRACE_ID` plus `MONOLITH_PARENT_SPAN_ID` when an editor action launches it. |
| Editor action | `Source/MonolithCore/Private/MonolithToolRegistry.cpp` | `FMonolithToolRegistry::ExecuteAction` validates lookup/profile/schema, releases the registry lock, sets action trace context, exports `MONOLITH_TRACE_ID` and `MONOLITH_PARENT_SPAN_ID` during handler execution, and logs `action.jsonl`. |
| Live source actions | `Source/MonolithSource/Private/MonolithSourceActions.cpp` | Some `source` actions shell out to `Binaries/monolith_query.exe`, so one live `source_query` can legitimately produce both `action.jsonl` and `query.jsonl`. |

## 3. Configuration

| Setting | Default | Surface | Notes |
|---|---|---|---|
| `UMonolithSettings::bEnableDailyLog` | `false` | Editor action | `action.jsonl` is opt-in. Disabled mode must not create `action.jsonl`. |
| `MONOLITH_TOOL_LOG_ENABLED` | enabled when unset | Proxy/query | `0` disables proxy/query logs. Unset or `1` enables them. |
| `MONOLITH_TOOL_LOG_DIR` | unset | Proxy/query/action | Overrides the log root for smoke tests, CI, and temporary diagnostics. The editor action logger also honors it (`LogRoot()` reads it before falling back to the plugin `Logs/` dir). |
| `MONOLITH_TOOL_LOG_MAX_FIELD_BYTES` | unset | Proxy/query/action | Optional per-field truncation override. Unset keeps the default 256 KiB bound; invalid values fall back to the default; valid values are clamped to 1 KiB through 16 MiB. |

P0 editor action logging follows only `bEnableDailyLog`. Proxy/query logging intentionally defaults on so agent behavior can be observed in no-editor and proxy-only workflows. Documentation must show `MONOLITH_TOOL_LOG_ENABLED=0` as the explicit disable path.

The C++ property fallback remains `false`, but this checkout opts in through `Config/DefaultMonolith.ini` and the Monolith plugin config by setting `bEnableDailyLog=True`. Agents using `BatchFiles\RunHeadlessEditor.bat` therefore get editor-side `action.jsonl` records without passing Monolith-specific command-line overrides.

Proxy/query default-on is intentionally separate from editor action logging. Editor settings are not available to script proxy and offline CLI processes unless a later shared-config mirror is added.

`MONOLITH_TOOL_LOG_DIR` is part of the P0 proxy/query contract because tests and headless agent workflows need an isolated log root. It is the root under which the daily folder is created, so a proxy smoke on 2026-05-20 writes `<override>/20260520/proxy.jsonl`. It does not change enable semantics: if `MONOLITH_TOOL_LOG_ENABLED=0`, no proxy/query log should be written even when a log directory override is present.

## 3.1 Implementation Summary

The document now reduces the implementation decision to three points:

1. **Collect v3 data now:** proxy, query, and action writers emit v3 records with correlation, routing, workflow, timing, environment, and summary fields.
2. **Analyze offline first:** the reader is `Analyzer/analyze_invocation_logs.py`; it consumes v3 first and gracefully degrades for v1/v2 rows in append-only date folders. Editor-action or native-CLI readers remain optional follow-ups.
3. **Keep controls split:** proxy/query remain default-on through `MONOLITH_TOOL_LOG_ENABLED`; editor action logging remains controlled by `bEnableDailyLog`.

## 4. Record Contract

Top-level fields (required unless a per-surface note says otherwise):

| Field | Type | Notes |
|---|---|---|
| `format_version` | integer | Current writer schema is `3`; append-only files can contain v1, v2, and v3 rows together. |
| `surface` | string | `proxy`, `query`, or `action`. |
| `sequence` | integer | Per-process monotonic sequence. |
| `record_id` | string | Stable unique id per JSONL row; preferred row handle for parent/previous/retry links. |
| `trace_id` | string | Cross-surface correlation id. Proxy generates it and forwards `_monolith_trace_id`; action propagates it to child query via `MONOLITH_TRACE_ID`. |
| `span_id` | string | Per-record span id for ordering and deduping within a trace. |
| `parent_span_id` | string | Parent span when known. Proxy-to-action uses `_monolith_parent_span_id`; action-to-query uses `MONOLITH_PARENT_SPAN_ID`. |
| `session_key` | string | Redacted/stable session grouping when available; use `stateless` when no MCP session id exists. |
| `process_instance_id` | string | Stable id for the current proxy/query/editor process lifetime. |
| `call_index` | integer | Per-process/session monotonic call index where available. |
| `previous_record_id` | string | Previous row in the same process/session when known. |
| `time_since_previous_ms` | number | Gap from the previous row in the same process/session when known. |
| `start_time` / `end_time` | string | Local ISO 8601 timestamp with offset. |
| `duration_ms` | number | Wall-clock duration. |
| `pid` | integer | Process id. |
| `thread_id` | string/integer | Best-effort. |
| `status` | string | `success` or `error`. Rejections and blocks keep `status=error`; the specific cause is carried in `agent_signal.outcome` (for example `validation_rejected`, `profile_blocked`). (`rejected` / `logging_error` are not currently emitted by any surface.) |
| `client` | object | MCP/client/runtime identity when known. Emitted by the proxy and query surfaces; the editor `action` surface omits it. |
| `call` | object | Tool, namespace/action, argv, and redacted arguments. |
| `return` | object | Redacted/truncated response, result, stdout/stderr, or error. |
| `return_summary` | object | Compact analyzer-friendly summary of result/error counts, top-level keys, bytes, and truncation state. |
| `routing_context` | object | Stable routing evidence for likely intent and namespace/action selection path. |
| `workflow` | object | Coarse workflow step and retry/recovery/discovery links. |
| `phase_timing` | object | Measured per-surface phase timings. Missing subfields mean "not measured", not zero. |
| `environment` | object | Low-cardinality plugin/editor/index/headless context when known. |
| `child_process` | object | Action-side summary for synchronous child `monolith_query.exe` calls when present. |
| `redaction` | object | Redaction/truncation metadata. |
| `agent_signal` | object | Best-effort improvement signals. |

Recommended context fields inside `client`, `call`, `return_summary`, or `agent_signal`:

| Field | Notes |
|---|---|
| `plugin_version` / `server_version` | Version string when available; useful when comparing behavior across Monolith updates. |
| `engine_version` / `project_name` | Editor-side environment identity when available. |
| `jsonrpc_id` | JSON-RPC correlation when present; omit duplicate return ids when they match the call id. |
| `namespace_source` | Whether namespace/action came from a split tool, direct namespace tool, alias rewrite, offline CLI, or child process. Emitted as `routing_context.namespace_source`, not as a free-standing `call`/`client` field. |
| `profile` / `policy` | Active tool profile and action execution policy summary when already known. |
| `validation_phase` | `lookup`, `profile`, `schema`, or `dispatch`. (`post_edit` is reserved for future use and is not currently emitted.) |
| `db_path` / `db_health` | Query-side database path and lightweight health/staleness signal when already computed. |

Example:

```json
{"format_version":3,"surface":"action","record_id":"rec-...","sequence":42,"trace_id":"trace-...","span_id":"span-...","parent_span_id":"span-...","process_instance_id":"proc-...","call_index":42,"start_time":"2026-05-20T14:52:11.120+09:00","end_time":"2026-05-20T14:52:11.138+09:00","duration_ms":18.0,"pid":12345,"thread_id":123,"status":"success","routing_context":{"decision_source":"direct","namespace_source":"domain_query","inferred_intent":"source_lookup","intent_confidence":"medium"},"workflow":{"step":"inspect"},"phase_timing":{"lookup_ms":0.1,"schema_ms":0.4,"handler_ms":15.9,"log_prepare_ms":0.8},"call":{"tool_name":"source_query","namespace":"source","action":"search_source","arguments":{"query":"UObject","limit":5},"validation_phase":"dispatch","retry_signature":"sha256:..."},"return_summary":{"success":true,"result_shape":"object","argument_bytes":29,"result_bytes":38,"result_top_keys":["items"]},"redaction":{"argument_bytes":29,"result_bytes":38},"agent_signal":{"outcome":"success"}}
```

## 4.1 Schema Compatibility

Readers and analyzers must be lenient because the logs are append-only diagnostic files, not a transactional database.

| Case | Required reader behavior |
|---|---|
| Mixed `format_version` rows in one date folder | Parse each line independently and dispatch by row version. Do not reject an entire file because one row is old. |
| `format_version` 1 | Treat as legacy. `trace_id`, `span_id`, and `return_summary` may be absent; `agent_signal` may contain duplicated byte fields or `retry_signature`. Derive what is possible from `call`, `return`, `redaction`, and timestamps. |
| `format_version` 2 | Treat as legacy compact output. Expect `trace_id`, `span_id`, and `return_summary` on fresh v2 writer output, but do not expect v3 correlation/routing/timing fields. |
| `format_version` 3 | Current writer output. Expect `record_id`, `trace_id`, `span_id`, `process_instance_id`, `routing_context`, `workflow`, `phase_timing`, `environment`, and `return_summary` where the surface can derive them. Empty optional fields are intentionally omitted. |
| Unknown future version | Keep the raw row, extract shared fields (`surface`, `start_time`, `duration_ms`, `call`, `status`, `agent_signal`) when possible, and mark unsupported fields as unknown. |
| Missing optional fields | Treat as unknown, not false. For example, missing `repeat_within_window` means "not emitted", not "definitely not repeated". |
| Unknown extra fields | Preserve or ignore; never fail parsing solely because a writer added fields. |

Analyzer reports should include a per-file schema mix summary (`v1_count`, `v2_count`, `v3_count`, `unknown_count`) so stale long-running processes are visible.

## 5. Agent Signal Fields

`agent_signal` is a diagnostic hint surface. It must not become the source of truth for tool success.

| Field | Notes |
|---|---|
| `outcome` | `success`, `tool_error`, `jsonrpc_error`, `validation_rejected`, `profile_blocked`, `editor_unavailable`, or `unknown`. Emission is surface-specific: action emits `success` / `tool_error` / `validation_rejected` / `profile_blocked`; proxy emits `success` / `jsonrpc_error` / `tool_error` / `editor_unavailable` / `unknown`; query emits `success` / `tool_error`. `proxy_error` is reserved and not currently emitted by any surface. |
| `error_code` / `error_class` | Normalized failure code and bucket when known. |
| `hints_returned` | Count of hints/related actions returned. Only the action surface computes a real value (`Hints` + `RelatedActions`); proxy and query always write `0` because hints live inside the opaque MCP/stdout payload they do not parse. Analyzers must read `hints_returned` on proxy/query rows as "unknown", not "zero hints returned". |
| `repeat_within_window` | Process-local repeated call indicator; omitted when false. Currently produced by proxy dedup only; the editor action surface does not compute it yet. |
| `improvement_tags` | Stable tags for Monolith improvement analysis. |

`retry_signature` stays under `call`. Byte sizes stay under `redaction` and `return_summary`. Empty strings, nulls, empty arrays, and empty objects are omitted from new records.

Tags currently emitted: `missing_action`, `schema_confusing`, `repeated_call` (proxy only), `editor_unavailable` (proxy only), `slow_action`, `large_result`, `escape_hatch` (action `editor.run_python` only), `profile_blocked`.

Tags reserved but not yet emitted by any surface: `discovery_gap`, `repeated_failure`. The proxy records routing context and retry signatures, but analyzers should derive discovery gaps and repeated-failure patterns from multiple rows before treating them as improvement findings.

## 5.1 Analysis Usage Contract

### First reader

`Analyzer/analyze_invocation_logs.py` is the implemented offline reader (landed 2026-06-08, commit e2db9d81; full contract in [SPEC_MonolithInvocationLogAnalyzer.md](SPEC_MonolithInvocationLogAnalyzer.md)). It never mutates the log root, follows the section 4.1 leniency rules (per-row `format_version` dispatch, parse-warning counting without file rejection, per-surface version counts in `findings.json` `summary.counts_by_surface_version`), classifies heartbeat/test/maintenance noise, and writes `summary.md`, `findings.json`, and CSV tables under the `--out` directory.

```text
python Analyzer/analyze_invocation_logs.py --log-root Logs --out Saved/Monolith/LogAnalysis/latest --format markdown,json,csv
python Analyzer/analyze_invocation_logs.py --log-root Logs --since 20260605 --until 20260611 --out Saved/Monolith/LogAnalysis/recent7d
python Analyzer/analyze_invocation_logs.py --log-root <dir> --out <out>   # isolated MONOLITH_TOOL_LOG_DIR roots
```

### Current vs target analysis capability

These daily logs exist to answer four questions: **what** tool an agent called, **why** it likely called it, **how** it continued, and **where** time accumulated. Against current `format_version` 3 records, all four are answerable enough to build the first local analyzer, with the caveats below. Against legacy `format_version` 1 or 2 rows, analyzers must fall back to the older fields each row actually contains.

| Goal | Now (v3) | Mechanism today | Remaining caveat |
|---|---|---|---|
| **What** was called | Full | `call.namespace`/`action`/`tool_name`, `surface`, `return_summary.result_shape` | Legacy rows need normalization. |
| **Why** it was called | Improved | `routing_context.decision_source`, `namespace_source`, `matched_discovered_action`, `inferred_intent`, `agent_signal.improvement_tags` | This remains inference from routing/result metadata, not private agent reasoning. |
| **How** the agent continued | Improved | `record_id`, `previous_record_id`, `time_since_previous_ms`, `process_instance_id`, `parent_span_id`, `workflow.*` | True multi-turn grouping depends on process/session continuity; stateless calls still require timestamp/process fallback. |
| **Where** time accumulated | Improved | `phase_timing.*`, `duration_ms`, `child_process.exec_process_ms`, cross-surface spans | `log_write_ms` is optional/usually absent because a row cannot include a post-append measurement from the same write. |

Correctness caveats for analyzers:

- **Child-query double counting is now identifiable:** a live `source` action can run `monolith_query.exe` synchronously, so the action record's `duration_ms` includes child query time. Use `child_process.exec_process_ms` and the child query row's `parent_span_id` to separate editor handler time from child-process time before building bottleneck totals.
- **Session/timeline grouping is best effort:** `trace_id` groups one tool call and downstream work. `process_instance_id`, `session_key`, `previous_record_id`, and `time_since_previous_ms` reconstruct per-process or per-session timelines, but stateless proxy/editor paths still cannot prove all rows belong to one human/agent turn.
- **Catalog fetches are invisible:** the proxy logs only `tools/call`. `tools/list` (catalog refresh), `initialize`, and `ping` are not logged, so an analyzer cannot see when an agent (re)pulled the tool list — a routing signal for the **Why** question. `monolith_find` / `monolith_discover` are themselves `tools/call`, so those discovery steps do remain visible.
- **`return_summary` remains surface-shaped:** `result_shape` gives a common enum, but action/proxy/query still expose different top-key and count fields. A cross-surface analyzer must normalize those shapes before comparing result size or item count.

Daily logs are meant to answer questions such as:

- Which namespaces/actions are agents actually using?
- Which calls fail because the catalog, schema, or examples are confusing?
- Which missing capabilities cause agents to retry, call the wrong namespace, or fall back to generic escape hatches?
- Which calls produce results too large for comfortable agent use?
- Which editor-unavailable or DB-stale states block common workflows?

The analysis layer must aggregate records locally and treat `agent_signal` as a hint, not a verdict. Improvement work should be based on repeated patterns across records plus code/catalog review, not on one inferred tag. Analyzers should use `return_summary`, `routing_context`, `workflow`, and `phase_timing` as primary inputs when available; raw `return` payloads are bounded diagnostic evidence and should be read only as a fallback or for drill-down.

Initial metrics worth deriving from JSONL:

| Metric | Availability | Use |
|---|---|---|
| Calls by namespace/action/surface | v2+ | Find high-traffic tools and underused or misplaced namespaces. |
| Error class by action | v2+ | Prioritize schema fixes, missing actions, and bad diagnostics. |
| Repeated call/failure clusters | v3 preferred (`previous_record_id`, `time_since_previous_ms`, `retry_signature`) | Detect confusing workflows and retry loops. |
| Discover-to-action proximity | v3 (`routing_context` + recent find/discover fields where available) | Estimate whether `monolith_find` / `monolith_discover` is helping agents route correctly. |
| Large result/truncation rate | v2+ | Identify actions that need pagination, summaries, or narrower defaults. |
| Source/query dual-surface count | v3 preferred (`trace_id` + `parent_span_id` + `child_process`) | Confirm expected child query usage without treating it as duplicate telemetry. |
| Per-phase / cross-surface time breakdown | v3 (`phase_timing`, `child_process`) | Locate bottlenecks within a surface and across proxy/action/query. |

## 5.2 Analysis Data Collection Contract

Analysis implementation is deferred. The format v3 log writers collect enough structured context to reconstruct agent work as a timeline:

1. What tool did the agent call?
2. Why did the agent likely call it?
3. How did the agent continue after success, failure, discovery, or fallback?
4. Where did time accumulate across proxy, editor action, child query, DB work, and log I/O?

This section defines the data collection contract. It is intentionally additive: missing fields must not break readers, and v1/v2 records remain valid historical input. Timeline, intent, and bottleneck analysis should prefer fresh v3 proxy/query/action records and degrade gracefully for older rows.

### 5.2.1 Correlation Fields

| Field | Surface | Requirement | Notes |
|---|---|---|---|
| `record_id` | all | Emitted in v3. | Stable unique id per JSONL row; use for parent/previous/retry links. |
| `trace_id` | all | Emitted in v2+. | Groups one MCP tool call and its downstream editor/query work. |
| `span_id` | all | Emitted in v2+. | Unique span for this row. |
| `parent_span_id` | action/query | Emitted in v3 when known. | Action parent is the proxy span; child query parent is the action span. |
| `session_key` | proxy/action | Emitted in v3 when available. | Redacted MCP session key, never raw `MCP-Session-Id`. Use `stateless` when absent. |
| `process_instance_id` | all | Emitted in v3 at process start. | Distinguishes sequence reuse after process restart. |
| `call_index` | proxy/action | Emitted in v3. | Supports ordering even when clocks are close. |
| `previous_record_id` | proxy/action/query | Emitted in v3 best effort. | Previous log row in the same process/session, for timeline reconstruction. |
| `time_since_previous_ms` | proxy/action/query | Emitted in v3 best effort. | Helps detect agent think time vs tool execution time. |

`sequence` remains process-local compatibility data. New analyzers should prefer `record_id`, `trace_id`, `span_id`, and timestamps.

**Cross-process span transport.** Format v3 transports both trace and parent span across the live call path:

- Proxy → action: inject `_monolith_parent_span_id` (the proxy span) alongside `_monolith_trace_id`; the HTTP server adopts it as the action record's `parent_span_id`.
- Action → child query: export `MONOLITH_PARENT_SPAN_ID` (the action span) alongside `MONOLITH_TRACE_ID`; `monolith_query.exe` records it as the query record's `parent_span_id`.

These additions are the prerequisite for end-to-end timeline reconstruction and for attributing time across proxy, action, and child query.

### 5.2.2 Routing And Intent Context

Logs cannot know the agent's private reasoning. They can still collect routing evidence that lets an analyzer infer likely intent.

| Field | Surface | Values / Shape | Purpose |
|---|---|---|---|
| `routing_context.decision_source` | proxy/action | `direct`, `after_find`, `after_discover`, `retry_after_error`, `fallback`, `child_query`, `unknown` | Explains why this tool was likely selected. |
| `routing_context.namespace_source` | proxy/action/query | `core_tool`, `domain_query`, `alias_rewrite`, `split_tool`, `offline_cli`, `child_process` | Explains how tool name became namespace/action. |
| `routing_context.recent_find_trace_id` | proxy/action | string | Last observed `monolith_find` trace in the process/session. |
| `routing_context.recent_discover_trace_id` | proxy/action | string | Last observed `monolith_discover` trace in the process/session. |
| `routing_context.matched_discovered_action` | proxy/action | boolean | True when a following action matches a recent discovered namespace/action. |
| `routing_context.followed_hint` | proxy/action | boolean | True when current call matches previous `hints` / `related_actions`. |
| `routing_context.inferred_intent` | proxy/action/query | stable enum | Coarse classification such as `schema_discovery`, `source_lookup`, `asset_search`, `build_diagnostics`, `error_recovery`, `mutation`, `verification`. |
| `routing_context.intent_confidence` | proxy/action/query | `low`, `medium`, `high` | Confidence of automatic classification. |

Implementation notes:

- Script proxies maintain a small in-memory per-session/process ring of recent `monolith_find`, `monolith_discover`, errors, hints, and related actions. The native C++ proxy emits a compatible basic routing context.
- Action logs the normalized namespace/action and validation phase; proxy context is transported through the HTTP request when available, and action supplements context from registry metadata.
- Query marks `namespace_source=child_process` when `MONOLITH_TRACE_ID` or `MONOLITH_PARENT_SPAN_ID` is present.
- Do not record free-form agent thoughts. Use stable enums and trace links.

### 5.2.3 Workflow Step Context

Add `workflow` to help reconstruct how agents move through a task.

| Field | Surface | Values / Shape | Purpose |
|---|---|---|---|
| `workflow.step` | all | `discover`, `inspect`, `execute`, `verify`, `recover`, `fallback`, `maintenance`, `unknown` | High-level phase for timeline charts. |
| `workflow.retry_of_record_id` | proxy/action/query | string | Links repeated call to original row. |
| `workflow.recovery_from_record_id` | proxy/action | string | Links a corrective call to the prior error row. |
| `workflow.discovery_root_record_id` | proxy/action | string | Links action back to the discovery call that likely selected it. |
| `workflow.outcome_continued` | analyzer-derived later | `next_success`, `next_error`, `abandoned`, `retried`, `switched_tool` | Not collected by writers; analyzers compute it. |

Writers should collect only facts available at call time. Fields like `outcome_continued` require future records and should be analyzer-derived, not written by the invocation logger.

### 5.2.4 Phase Timing

`duration_ms` is not enough to locate bottlenecks. Add `phase_timing` objects with best-effort timings. Missing phase fields are acceptable; recorded values must be measured, not guessed.

Proxy phases:

| Field | Notes |
|---|---|
| `phase_timing.parse_ms` | JSON/message extraction and tool-name parsing. |
| `phase_timing.rewrite_ms` | Alias/split-tool rewrite and trace injection. |
| `phase_timing.dedup_ms` | Repeat-call guard cost. |
| `phase_timing.http_roundtrip_ms` | POST to editor MCP endpoint, including editor processing. |
| `phase_timing.fallback_ms` | Local offline/error fallback generation. |
| `phase_timing.log_prepare_ms` | Redaction, bounding, summary construction. |
| `phase_timing.log_write_ms` | Optional. Only emit if the surface can measure append time without lying in the same row. |

Action phases:

| Field | Notes |
|---|---|
| `phase_timing.lookup_ms` | Registry lookup and action existence checks. |
| `phase_timing.profile_ms` | Tool profile allow/deny check. |
| `phase_timing.alias_ms` | Param alias rewrite. |
| `phase_timing.schema_ms` | Required/unknown/type/range/enum validation. |
| `phase_timing.handler_ms` | Handler execution. |
| `phase_timing.post_edit_ms` | Post-edit validation and guard bookkeeping. |
| `phase_timing.log_prepare_ms` | Redaction, bounding, summary construction. |
| `phase_timing.log_write_ms` | Optional. Only emit if the surface can measure append time without lying in the same row. |

Query phases:

| Field | Notes |
|---|---|
| `phase_timing.parse_args_ms` | CLI parse and validation. |
| `phase_timing.db_resolve_ms` | DB path resolution and freshness checks done by the CLI. |
| `phase_timing.db_open_ms` | SQLite open/init time. |
| `phase_timing.action_exec_ms` | Namespace action execution. |
| `phase_timing.stdout_capture_ms` | Capture/replay overhead. |
| `phase_timing.log_prepare_ms` | Redaction, bounding, summary construction. |
| `phase_timing.log_write_ms` | Optional. Only emit if the surface can measure append time without lying in the same row. |

Child-process wrappers such as live `source` actions that call `monolith_query.exe` should additionally record:

| Field | Surface | Notes |
|---|---|---|
| `child_process.executable` | action | Basename only, for example `monolith_query.exe`. |
| `child_process.argv_summary` | action | Namespace/action and non-sensitive flags, not full raw command if it may contain paths/secrets. |
| `child_process.exec_process_ms` | action | Wall-clock process duration. |
| `child_process.exit_code` | action | Return code. |
| `child_process.stdout_bytes` / `stderr_bytes` | action | Raw byte counts. |
| `child_process.trace_id` / `span_id` | action/query | Child query trace/span linkage when known. |

`child_process.exec_process_ms` is the prerequisite for separating editor handler time from the synchronous child query time; without it, the action `duration_ms` double-counts the child query and cross-surface bottleneck attribution is unreliable.

### 5.2.5 Result And Payload Summaries

`return` can stay bounded for diagnostics, but analyzers should rely on `return_summary`.

Minimum common fields for analyzer-friendly summaries:

| Field | Notes |
|---|---|
| `return_summary.success` / `exit_code` / `is_error` | Surface-appropriate success indicator. At least one should be present. |
| `return_summary.result_shape` | Stable shape enum: `empty`, `text`, `object`, `list`, `mcp_content`, `error`, `mixed`. |
| `return_summary.argument_bytes` / `result_bytes` | Payload size signals. Query may split stdout/stderr bytes; analyzers should normalize them into total result bytes. |
| `return_summary.truncated` | True when any logged payload field was replaced by a bounded envelope. |
| `return_summary.result_top_keys` / `stdout_top_keys` / `response_top_keys` | Top-level shape preview without requiring raw payload parsing. |
| `return_summary.error_code` / `error_class` / `error_message` | Compact failure information; long messages stay bounded. |

Domain-specific additions where derivable:

| Field | Notes |
|---|---|
| `return_summary.items_count` | Generic rows/items count. |
| `return_summary.assets_count` | Asset rows count when recognized. |
| `return_summary.symbols_count` | Source symbol rows count when recognized. |
| `return_summary.references_count` | Reference/caller/callee rows count when recognized. |
| `return_summary.warnings_count` / `errors_count` | Structured warning/error counts. |
| `return_summary.hints_count` / `related_actions_count` | Routing recovery hints. |
| `return_summary.truncated_fields` | Names of fields replaced by bounded envelopes. |
| `return_summary.affected_paths_count` | Count only by default; list paths only if already safe/redacted. |

Do not parse large source snippets or asset payloads with ad-hoc string matching in the logger. Prefer structured result fields already returned by actions. If a surface can only return text today, record `result_shape="text"` and byte counts rather than guessing item counts from prose.

### 5.2.6 Environment And State Context

Add low-cardinality context that helps compare runs without exposing secrets.

| Field | Surface | Notes |
|---|---|---|
| `environment.plugin_version` | proxy/action/query | Monolith version when available. |
| `environment.binary_build_utc` | action | ISO-8601 link timestamp of the running `MonolithCore` module binary. Distinguishes records from a stale editor binary vs a rebuilt one; `plugin_version` alone cannot (2026-07-04: HEAD fixes were live in source while the running editor still served old behavior). |
| `environment.engine_version` | action | UE version when available. |
| `environment.project_name_hash` | action/query | Hash or redacted project identifier, not raw user-specific path. |
| `environment.headless` | action | True when `-NullRHI`, unattended, or configured headless wrapper is detected. |
| `environment.p4_enabled` | action | Source control provider availability summary. |
| `environment.index_health` | action/query | Compact `ok`, `missing`, `stale`, or `unknown` for relevant DBs. |
| `environment.active_profile_id` | action | Tool profile id already available in execution guard. |
| `environment.is_automation_test` | action | Present and `true` only while `GIsAutomationTesting` is set. Primary synthetic-fixture signal for the analyzer; absent on legacy rows and normal calls. |
| `environment.automation_test_name` | action | Full automation test name when the framework exposes a current test; only written alongside `is_automation_test`. |

### 5.2.7 Privacy And Redaction Rules

- Never log API keys, bearer tokens, cookies, session ids, private keys, or authorization headers.
- Store raw MCP session ids only as redacted display plus stable hash key.
- Prefer counts, hashes, short stable enums, and bounded previews over raw source snippets or asset payloads.
- Treat Windows paths, asset paths, and source file paths as project context. They may be logged when needed for local diagnostics, but analyzers should not require full paths when a hash/count/extension/category is enough.
- `reason` and `inferred_intent` fields must be generated from tool routing/result metadata, not from hidden agent chain-of-thought.

### 5.2.8 Example v3 Record

```json
{"format_version":3,"surface":"action","record_id":"rec-...","trace_id":"trace-...","span_id":"span-...","parent_span_id":"span-...","session_key":"md5:...","process_instance_id":"proc-...","call_index":17,"previous_record_id":"rec-...","time_since_previous_ms":420.5,"start_time":"2026-05-20T14:52:11.120+09:00","end_time":"2026-05-20T14:52:11.138+09:00","duration_ms":18.0,"pid":12345,"thread_id":123,"status":"success","routing_context":{"decision_source":"after_discover","namespace_source":"domain_query","matched_discovered_action":true,"inferred_intent":"source_lookup","intent_confidence":"medium"},"workflow":{"step":"inspect","discovery_root_record_id":"rec-..."},"phase_timing":{"lookup_ms":0.1,"profile_ms":0.1,"schema_ms":0.4,"handler_ms":15.9,"log_prepare_ms":0.8,"log_write_ms":0.7},"call":{"tool_name":"source_query","namespace":"source","action":"search_source","arguments":{"query":"UObject","limit":5},"validation_phase":"dispatch","retry_signature":"sha256:..."},"return_summary":{"success":true,"result_shape":"object","items_count":5,"symbols_count":5,"argument_bytes":29,"result_bytes":2048,"result_top_keys":["items","count"]},"redaction":{"argument_bytes":29,"result_bytes":2048},"agent_signal":{"outcome":"success"}}
```

### 5.2.9 Analyzer Input Priority

When building the first analyzer, consume v3 fields in the order of the question they unlock:

1. **How the agent continued (timeline):** `process_instance_id`, `session_key`, `previous_record_id`, `time_since_previous_ms`, then `parent_span_id`.
2. **Where time accumulates (bottlenecks):** `phase_timing.*` per surface, then `child_process.exec_process_ms` to separate editor handler time from synchronous child query time.
3. **Why the agent called (intent):** `routing_context.decision_source`, `recent_find_trace_id` / `recent_discover_trace_id`, then `inferred_intent`.

The **What** goal is satisfied by v2+ rows; v3 adds stronger linkage and summaries for analysis quality.

## 6. Surface Contracts

### 6.1 Proxy

Proxy logs wrap MCP `tools/call` handling. They record JSON-RPC id, original tool name, forwarded tool name, redacted arguments, response/error, proxy runtime (`cpp`, `python`, or `node`), a generated `trace_id`, and a proxy `span_id`. In the script proxies (`python`, `node`) — the P0 path — `tool_name_original` and `tool_name_forwarded` are always identical because they forward the tool name unchanged; only the native `cpp` proxy rewrites some names (for example to `editor_query`), so the distinction is currently meaningful only there. An analyzer must not assume a forwarded-name rewrite was observable on the path an agent actually used. The forwarded JSON-RPC object carries `_monolith_trace_id` and `_monolith_parent_span_id` so action logs can share the same trace and link back to the proxy span without changing the MCP-visible tool schema.

P0 must cover the active script proxy path selected by `Scripts/monolith_proxy.bat`; implementing only `monolith_proxy.exe` is not sufficient for the current checkout.

### 6.2 Query

Query logs wrap `monolith_query.exe <namespace> <action> ...`. They record argv, parsed namespace/action/options, effective DB path, stdout, stderr, fatal error, exit code, and `return_summary`. When launched from an editor action, they inherit the action trace through `MONOLITH_TRACE_ID` and link to the action span through `MONOLITH_PARENT_SPAN_ID`; direct CLI calls generate their own trace.

The CLI compatibility contract is strict: stdout, stderr, and exit code observed by callers must remain unchanged. Fatal paths such as `die()` must become loggable without changing command semantics.

### 6.3 Action

Action logs wrap editor-side `FMonolithToolRegistry::ExecuteAction`. They record normalized namespace/action, effective params after alias handling, validation phase (`lookup`/`profile`/`schema`/`dispatch`), result/error/hints/warnings, routing/workflow context, phase timing, child-query process summaries, and the active trace/span context. Post-edit validation outcome is not yet captured in the record.

File I/O must not run while the registry lock is held. Pre-dispatch failures such as unknown action, profile block, missing param, and strict param rejection must still emit `action.jsonl` when `bEnableDailyLog=true`.

## 7. Redaction and Truncation

Redact case-insensitive key fragments recursively:

```text
authorization, bearer, token, api_key, apikey, password, passwd, secret, cookie, private_key, session_id
```

Default limits:

| Item | Limit |
|---|---|
| Serialized arguments field | 256 KiB |
| Serialized return field | 256 KiB |
| Full log line target | 1 MiB (advisory; only the per-field 256 KiB bounds are enforced by the writers) |

When truncating, preserve `truncated=true`, `original_bytes`, `sha256` of the original serialized payload, and a bounded `preview` (head of the serialized payload). Non-truncated records omit hash fields.

## 8. Implementation Order

1. Add schema constants and redaction/truncation helpers.
2. Add proxy logging to `monolith_proxy.py` and `monolith_proxy.js`; keep C++ proxy schema-compatible.
3. Add native tool logger for `monolith_query.exe`; refactor fatal exits into a loggable top-level path.
4. Add `UMonolithSettings::bEnableDailyLog=false`.
5. Add editor action logger around `FMonolithToolRegistry::ExecuteAction`, with file I/O outside registry locks.
6. Populate `trace_id`, `span_id`, `return_summary`, `agent_signal`, `retry_signature`, and `improvement_tags`.
7. Remove empty optional fields and duplicate analyzer fields from new records.
8. Add tests for default-on proxy/query, env disable, `MONOLITH_TOOL_LOG_DIR`, editor off-by-default, redaction, truncation, query output preservation, script proxy parity, source dual-surface logging, and trace inheritance.

## 9. Verification Gates

| Gate | Pass criteria |
|---|---|
| Editor off by default | Running an editor action with `bEnableDailyLog=false` creates no `action.jsonl`. |
| Proxy/query default on | With `MONOLITH_TOOL_LOG_ENABLED` unset, proxy/query calls create daily logs. |
| Proxy/query env disable | With `MONOLITH_TOOL_LOG_ENABLED=0`, proxy/query calls create no daily logs. |
| Proxy/query log-dir override | With `MONOLITH_TOOL_LOG_DIR` set, proxy/query logs are written under the override path; if logging is disabled, the override path still receives no log. |
| Script proxy parity | The active script proxy path emits the same schema and env semantics as the C++ proxy. |
| Query output preservation | `monolith_query.exe` stdout/stderr/exit code are unchanged by logging. |
| Source dual-surface | A live `source_query` that shells out to `monolith_query.exe` emits both valid action and query records. |
| Trace continuity | Proxy-to-action records share `trace_id`; action records link to proxy `span_id`; action-spawned query records inherit the action `trace_id` and `span_id` as `parent_span_id`. |
| Schema compactness | New format v3 records include `return_summary`, `routing_context`, `workflow`, `phase_timing`, and analyzer-friendly ids while omitting empty optional fields and duplicate `agent_signal.retry_signature` / byte fields. |
| Fresh-process v3 append | After restarting proxy/editor/query processes, new proxy, query, and action calls append `format_version=3` rows to the current date folder. Existing v1/v2 rows may remain earlier in the same file. |
| Mixed-schema reader tolerance | A reader/analyzer test covers a date folder containing v1, v2, and v3 rows and reports schema counts without dropping valid records. (Verified 2026-06-12 with `Analyzer/analyze_invocation_logs.py` against the mixed `Logs/20260520` folder — see §12.) |
| Redaction/truncation | Sensitive values are absent and large payloads stay bounded with metadata. |
| Failure paths | Unknown action, bad params, and editor unavailable preserve existing failure semantics and emit diagnostic records when enabled. |

## 10. Relation To Existing Diagnostics

`bEnableAdvancedToolCallRecords` controls bounded, redacted, in-memory ToolCall records and local analysis actions. Daily invocation logs are persisted JSONL files with bounded call/return payloads. The features can coexist, but neither replaces the other.

`FMonolithActionExecutionGuard` remains responsible for policy-aware action audit, dirty package tracking, transactions, and post-edit validation. Daily logs only observe invocation inputs, outputs, and agent behavior hints.

### 10.1 `bEnableAdvancedToolCallRecords`

`bEnableAdvancedToolCallRecords` is for short-lived editor diagnostics. It keeps a bounded, redacted record set in memory so live actions can inspect recent calls without writing raw payload history to disk. It is useful for immediate debugging and local analysis actions, but it is not a durable audit log.

Daily invocation logs are the durable, date-partitioned JSONL layer. They intentionally capture call/return summaries across proxy, query, and action surfaces so agent behavior can be reviewed after the process exits.

## 11. Open Decisions

| Topic | Decision needed |
|---|---|
| Retention | Resolved 2026-06-12: retention stays manual, with `Scripts/prune_invocation_logs.ps1` as the manual entry point (age and/or total-size rules over `Logs/yyyyMMdd` folders, dry-run by default; contract in [SPEC_MonolithAgentOpsScripts.md](SPEC_MonolithAgentOpsScripts.md)). A writer-side auto-retention setting remains a possible follow-up. |
| Reader/analyzer | Resolved: the first reader is `Analyzer/analyze_invocation_logs.py` (landed 2026-06-08, commit e2db9d81, own spec [SPEC_MonolithInvocationLogAnalyzer.md](SPEC_MonolithInvocationLogAnalyzer.md)); this parent spec stayed on "deferred" until 2026-06-12 because that change did not update it — readers must treat the child spec as authoritative for the analyzer surface. Editor action `monolith.analyze_invocation_logs` and native `monolith_query logs analyze` remain optional follow-ups if live/in-editor aggregation is needed. |
| Shared enable config | Decide how editor `bEnableDailyLog` should mirror to proxy/query without requiring environment variables. |
| Analyzer schema | Decide the first analyzer action/CLI shape for reading mixed v1/v2/v3 logs, grouping by `trace_id` / `parent_span_id` / `previous_record_id` when present, and falling back to timestamp/pid ordering when not. |
| Privacy policy | Decide whether source snippets, asset paths, and user/project identifiers should be logged as raw text, redacted, or hash-only. |

## 12. Verification Record

2026-05-20 P0 verification:

| Gate | Result |
|---|---|
| Static checks | `node --check Scripts\monolith_proxy.js`, Python py_compile, and `Scripts\ci_static_checks.py check` passed with 0 blocking findings. |
| Native proxy/query builds | `Tools\MonolithProxy\build.bat` and `Tools\MonolithQuery\build.bat` succeeded and copied binaries to `Binaries\`; only C4819 codepage warnings were emitted. |
| Proxy default-on/disable/parity | C++/Python/Node proxy smoke calls produced valid `proxy.jsonl` records with redaction and `editor_unavailable` offline outcome; `MONOLITH_TOOL_LOG_ENABLED=0` produced no log. |
| Query default-on/disable/output preservation | `Binaries\monolith_query.exe source health` produced valid `query.jsonl`; disabled mode produced no log; enabled and disabled stdout/stderr matched. |
| Action opt-in/redaction/truncation | `Monolith.Core.ToolInvocationLogger.DailyLogOptInRedaction` passed and verified disabled mode, redaction, large payload truncation metadata, and JSONL parsing. |
| Pre-dispatch failure | `Monolith.Core.ToolInvocationLogger.PreDispatchFailureLogged` passed and verified unknown-action lookup failures are recorded with `error_class=unknown_action`. |
| Source dual-surface | `Monolith.Core.ToolInvocationLogger.SourceChildQueryDualSurface` passed and verified action/query records. |
| Headless action matrix | `BatchFiles\RunHeadlessEditor.bat` launched a `-NullRHI` editor with no Monolith command-line override; `monolith_status`, `monolith_discover`, `source.health`, `source.search_source`, `project.search`, `editor.get_build_errors`, unknown `source` action, and missing-param `source.search_source` appended 8 valid `action.jsonl` records. Success, lookup error, and schema rejection outcomes were all recorded. |
| Proxy-to-action headless path | Python proxy calls to `monolith_status`, successful `source.search_source`, and missing-param `source.search_source` appended 3 proxy records and 3 action records against the same headless editor. |
| Final integrated append | With headless editor daily logging enabled from config, proxy `monolith_status`, query `source health`, and editor actions appended records to `Plugins\Monolith\Logs\20260520\proxy.jsonl`, `query.jsonl`, and `action.jsonl`; appended records parsed and reported expected outcomes. |
| Broader project build | At the time of the P0 run, the project-editor UBT build failed on an unrelated `AProjectPlayerController::CreateVirtualJoystick()` unresolved external in the game module. That symbol is now defined (`Source/<ProjectGameModule>/Private/Framework/ProjectPlayerController.cpp`) and a later project-editor build completed cleanly, so this blocker is resolved. |

2026-05-20 format v2 follow-up:

| Gate | Result |
|---|---|
| Static checks | `node --check Scripts\monolith_proxy.js`, Python py_compile, and `Scripts\ci_static_checks.py check` passed with 0 blocking findings. |
| Native proxy/query builds | `Tools\MonolithProxy\build.bat` and `Tools\MonolithQuery\build.bat` succeeded and copied binaries to `Binaries\`; only C4819 codepage warnings were emitted. |
| Proxy/query v2 smoke | C++ proxy, Python proxy, Node proxy, and direct query calls under isolated `MONOLITH_TOOL_LOG_DIR` emitted format v2 records with `trace_id`, `span_id`, `return_summary`, and no duplicate `agent_signal.retry_signature`; query inherited `MONOLITH_TRACE_ID=trace-smoke-shared`. |
| MonolithCore compile | UBT compiled `MonolithToolInvocationLogger.cpp`, `MonolithToolRegistry.cpp`, `MonolithHttpServer.cpp`, and tests, then failed only at DLL link because a running `UnrealEditor.exe` held `Binaries\Win64\UnrealEditor-MonolithCore.dll`. Superseded by the 2026-05-21 v3 verification below. |

2026-05-21 format v3 follow-up:

| Gate | Result |
|---|---|
| Static checks | `node --check Scripts\monolith_proxy.js`, `uv run --python 3.11 python -m py_compile Scripts\monolith_proxy.py Scripts\ci_static_checks.py`, and `uv run --python 3.11 python Scripts\ci_static_checks.py check` passed with 0 blocking findings. |
| Native proxy/query builds | `Tools\MonolithProxy\build.bat` and `Tools\MonolithQuery\build.bat` succeeded and copied updated binaries to `Binaries\`; only C4819 codepage warnings were emitted. |
| Query v3 smoke | Direct `Binaries\monolith_query.exe source health` under isolated `MONOLITH_TOOL_LOG_DIR` emitted `format_version=3` with `record_id`, `process_instance_id`, `routing_context`, `workflow`, `phase_timing`, `environment`, `return_summary.result_shape`, and inherited `MONOLITH_PARENT_SPAN_ID`. |
| Proxy v3 smoke | C++ proxy, Python proxy, and Node proxy offline smokes emitted `format_version=3` proxy records with routing/workflow/timing summaries and `editor_unavailable` outcome. |
| Cross-runtime log lock | Python proxy, Node proxy, C++ proxy, and C++ query use the same exclusive `.lock` file create/delete protocol; a regression smoke wrote Python, Node, and C++ proxy records sequentially into the same `proxy.jsonl` without timeout. |
| Query default-on/disable/output preservation | Direct query calls under isolated log roots verified unset `MONOLITH_TOOL_LOG_ENABLED` creates v3 `query.jsonl`, `MONOLITH_TOOL_LOG_ENABLED=0` creates no log, and stdout/stderr are identical between enabled and disabled modes. |
| Proxy disable | C++ proxy, Python proxy, and Node proxy calls with `MONOLITH_TOOL_LOG_ENABLED=0` created no `proxy.jsonl` under the override log root. |
| MonolithCore build | Project-editor UBT build completed after closing the editor process that held the previous DLL. |
| Headless action and child query | `BatchFiles\RunHeadlessEditor.bat` launched a `-NullRHI` editor; a live proxy `source_query(crg_graph_health)` produced correlated v3 `proxy.jsonl`, `action.jsonl`, and `query.jsonl` rows with shared `trace_id`, parent span linkage, action `child_process.exec_process_ms`, action/query `inferred_intent=verification`, and query `namespace_source=child_process`. |
| Automation tests | `Monolith.Core.ToolInvocationLogger.DailyLogOptInRedaction`, `Monolith.Core.ToolInvocationLogger.PreDispatchFailureLogged`, and `Monolith.Core.ToolInvocationLogger.SourceChildQueryDualSurface` passed. |

2026-06-12 offline reader verification (`Analyzer/analyze_invocation_logs.py`, landed 2026-06-08 in commit e2db9d81):

| Gate | Result |
|---|---|
| Fixture smoke | `--log-root Analyzer/fixtures/invocation_logs` scanned 2 files / 19 records with 0 parse warnings and wrote `summary.md` + `findings.json`. |
| 7-day real aggregate | `--log-root Logs --since 20260605 --until 20260612` scanned 13 files / 3,280 records / 0 parse warnings and produced surface-status, noise-classification (normal/maintenance/synthetic_test/heartbeat/expected_slow_domain/escape_hatch), environment, and 69 ranked findings. |
| Mixed-schema reader tolerance | `--since 20260520 --until 20260520` parsed the append-only mixed folder (174 records: proxy v1=4, action v1=36, query v1=127 + v2=7) with 0 parse warnings and 0 dropped rows; per-surface version counts are reported in `findings.json` `summary.counts_by_surface_version`. |
| Read-only behavior | Reports were written only under the `--out` directory (`Saved/Monolith/LogAnalysis/...`); `Logs/` was not modified. |

A duplicate stdlib reader was briefly added as `Scripts/analyze_invocation_logs.py` on 2026-06-11 while this spec still claimed the analyzer was deferred; it was removed on 2026-06-12 in favor of the committed `Analyzer/` implementation above.
