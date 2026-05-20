# Monolith — Tool Invocation Daily Logs

**Parent:** [../SPEC_CORE.md](../SPEC_CORE.md)
**Status:** Implemented / P0 verified
**Scope:** Proxy, offline query, and editor action invocation diagnostics
**Created:** 2026-05-20

---

## 1. Purpose

Monolith needs local, append-only daily logs that show how agents call Monolith through the MCP proxy, offline query CLI, and live editor action registry. The logs are for local diagnostics and Monolith improvement signals. They are not remote telemetry, not a replacement for returned tool output, and not a replacement for the existing in-memory ToolCall ledger.

This document is the implementation contract and current P0 verification record. Later P1/P2 items such as retention, analyzer actions, shared config mirroring, max-byte settings, and trace propagation remain future work.

The desired daily files live under `Plugins/Monolith/Logs/` by default:

| Surface | File |
|---|---|
| MCP stdio proxy | `yyyyMMdd_proxy.log` |
| Offline query CLI | `yyyyMMdd_query.log` |
| Editor action dispatch | `yyyyMMdd_action.log` |

Each file is JSON Lines: one compact JSON object per invocation, appended sequentially.

## 1.1 Implementation Readiness

The 2026-05-20 source review found the feature practical to implement, but only if the first slice covers the real entrypoints agents use today.

| Area | Finding | Required handling |
|---|---|---|
| Proxy | `Scripts/monolith_proxy.bat` can select Python or Node before any native proxy path. | P0 must implement script proxy logging parity; C++ proxy parity alone is not enough. |
| Query | `monolith_query.exe` writes directly to stdout/stderr and fatal paths can exit early. | Capture/replay streams without changing observed output or exit code, and make fatal dispatch paths loggable. |
| Action | `FMonolithToolRegistry::ExecuteAction` can reject calls before handler dispatch. | Log unknown action, profile block, schema rejection, missing params, strict-param failures, and handler results when action logging is enabled. |
| Locking | Registry locks and log file locks solve different problems. | Do not perform file I/O under the registry lock; use process-local and cross-process append guards for log writes. |
| Existing diagnostics | Action audit and `bEnableAdvancedToolCallRecords` are useful but not durable proxy/query/action records. | Keep daily JSONL logging independent and do not change audit or MCP response shapes. |
| Analysis | `agent_signal` fields are inferred hints. | Use them for aggregate Monolith improvements, not as a single-record verdict. |

P0 completion requires all three surfaces plus docs/template sync and the verification gates in section 9. The current checkout has passed those P0 gates, except that the broader GoGameEditor target has an unrelated game-module link blocker noted in section 12.

## 2. Current Code Facts

| Surface | Current entrypoint | Current behavior |
|---|---|---|
| C++ proxy | `Tools/MonolithProxy/monolith_proxy.cpp` | `handle_tools_call` validates, deduplicates via `record_tool_call`, and forwards requests to the editor HTTP MCP server. It does not persist durable invocation logs. |
| Script proxy | `Scripts/monolith_proxy.bat`, `Scripts/monolith_proxy.py`, `Scripts/monolith_proxy.js` | The Windows launcher probes Python, `python3`, Node, then `py`; these script paths are active supported paths and must get P0 logging parity with the C++ proxy. |
| Offline query | `Tools/MonolithQuery/monolith_query.cpp` | `main` parses namespace/action and dispatches static maps. Many actions write directly to stdout/stderr, and `die()` currently exits immediately. |
| Editor action | `Source/MonolithCore/Private/MonolithToolRegistry.cpp` | `FMonolithToolRegistry::ExecuteAction` validates lookup/profile/schema, releases the registry lock, runs the handler, and returns `FMonolithActionResult`. |
| Live source actions | `Source/MonolithSource/Private/MonolithSourceActions.cpp` | Some `source` actions shell out to `Binaries/monolith_query.exe`, so one live `source_query` can legitimately produce both `action.log` and `query.log`. |

## 3. Configuration

| Setting | Default | Surface | Notes |
|---|---|---|---|
| `UMonolithSettings::bEnableDailyLog` | `false` | Editor action | `action.log` is opt-in. Disabled mode must not create `action.log`. |
| `MONOLITH_TOOL_LOG_ENABLED` | enabled when unset | Proxy/query | `0` disables proxy/query logs. Unset or `1` enables them. |
| `MONOLITH_TOOL_LOG_DIR` | unset | Proxy/query | Overrides the log root for smoke tests, CI, and temporary diagnostics. |
| `MONOLITH_TOOL_LOG_MAX_FIELD_BYTES` | unset | Proxy/query P1 | Overrides per-field truncation when implemented. |

P0 editor action logging follows only `bEnableDailyLog`. Proxy/query logging intentionally defaults on so agent behavior can be observed in no-editor and proxy-only workflows. Documentation must show `MONOLITH_TOOL_LOG_ENABLED=0` as the explicit disable path.

The C++ property fallback remains `false`, but this checkout opts in through `Config/DefaultMonolith.ini` and the Monolith plugin config by setting `bEnableDailyLog=True`. Agents using `BatchFiles\RunHeadlessEditor.bat` therefore get editor-side `action.log` records without passing Monolith-specific command-line overrides.

Proxy/query default-on is intentionally separate from editor action logging. Editor settings are not available to script proxy and offline CLI processes unless a later shared-config mirror is added.

`MONOLITH_TOOL_LOG_DIR` is part of the P0 proxy/query contract because tests and headless agent workflows need an isolated log root. It does not change enable semantics: if `MONOLITH_TOOL_LOG_ENABLED=0`, no proxy/query log should be written even when a log directory override is present.

## 4. Record Contract

Required top-level fields:

| Field | Type | Notes |
|---|---|---|
| `format_version` | integer | Starts at `1`. |
| `surface` | string | `proxy`, `query`, or `action`. |
| `sequence` | integer | Per-process monotonic sequence. |
| `start_time` / `end_time` | string | Local ISO 8601 timestamp with offset. |
| `duration_ms` | number | Wall-clock duration. |
| `pid` | integer | Process id. |
| `thread_id` | string/integer | Best-effort. |
| `status` | string | `success`, `error`, `rejected`, or `logging_error`. |
| `client` | object | MCP/client/runtime identity when known. |
| `call` | object | Tool, namespace/action, argv, and redacted arguments. |
| `return` | object | Redacted/truncated response, result, stdout/stderr, or error. |
| `redaction` | object | Redaction/truncation metadata. |
| `agent_signal` | object | Best-effort improvement signals. |

Recommended context fields inside `client`, `call`, or `agent_signal`:

| Field | Notes |
|---|---|
| `plugin_version` / `server_version` | Version string when available; useful when comparing behavior across Monolith updates. |
| `engine_version` / `project_name` | Editor-side environment identity when available. |
| `jsonrpc_id` / `trace_id` | Correlation fields. P0 may rely on JSON-RPC id and process metadata; explicit trace propagation can be P1. |
| `namespace_source` | Whether namespace/action came from a split tool, direct namespace tool, alias rewrite, or offline CLI. |
| `profile` / `policy` | Active tool profile and action execution policy summary when already known. |
| `validation_phase` | `lookup`, `profile`, `schema`, `dispatch`, or `post_edit`. |
| `db_path` / `db_health` | Query-side database path and lightweight health/staleness signal when already computed. |

Example:

```json
{"format_version":1,"surface":"action","sequence":42,"start_time":"2026-05-20T14:52:11.120+09:00","end_time":"2026-05-20T14:52:11.138+09:00","duration_ms":18.0,"pid":12345,"thread_id":"game","status":"success","client":{"name":"unknown","version":"","proxy_runtime":"none"},"call":{"tool_name":"source_query","namespace":"source","action":"search_source","arguments":{"query":"UObject","limit":5},"retry_signature":"sha256:..."},"return":{"success":true,"result":{"items":[]},"result_bytes":12},"redaction":{"applied":false,"truncated":false},"agent_signal":{"outcome":"success","improvement_tags":[],"discovery_context":"unknown","repeat_within_window":false}}
```

## 5. Agent Signal Fields

`agent_signal` is a diagnostic hint surface. It must not become the source of truth for tool success.

| Field | Notes |
|---|---|
| `outcome` | `success`, `tool_error`, `jsonrpc_error`, `proxy_error`, `validation_rejected`, `profile_blocked`, `editor_unavailable`, or `unknown`. |
| `error_code` / `error_class` | Normalized failure code and bucket when known. |
| `hints_returned` | Count of hints/related actions returned. |
| `discovery_context` | `unknown`, `recent_find`, `recent_discover`, or `none_observed`; best-effort and process-local in P0. |
| `retry_signature` | Redacted/hash signature for grouping. |
| `repeat_within_window` | Process-local repeated call indicator in P0. |
| `argument_bytes` / `result_bytes` | Serialized payload size before truncation. |
| `improvement_tags` | Stable tags for Monolith improvement analysis. |

Initial `improvement_tags`: `missing_action`, `schema_confusing`, `discovery_gap`, `repeated_call`, `repeated_failure`, `editor_unavailable`, `slow_action`, `large_result`, `escape_hatch`, `profile_blocked`.

## 5.1 Analysis Usage Contract

Daily logs are meant to answer questions such as:

- Which namespaces/actions are agents actually using?
- Which calls fail because the catalog, schema, or examples are confusing?
- Which missing capabilities cause agents to retry, call the wrong namespace, or fall back to generic escape hatches?
- Which calls produce results too large for comfortable agent use?
- Which editor-unavailable or DB-stale states block common workflows?

The analysis layer must aggregate records locally and treat `agent_signal` as a hint, not a verdict. Improvement work should be based on repeated patterns across records plus code/catalog review, not on one inferred tag.

Initial metrics worth deriving from JSONL:

| Metric | Use |
|---|---|
| Calls by namespace/action/surface | Find high-traffic tools and underused or misplaced namespaces. |
| Error class by action | Prioritize schema fixes, missing actions, and bad diagnostics. |
| Repeated call/failure clusters | Detect confusing workflows and retry loops. |
| Discover-to-action proximity | Estimate whether `monolith_find` / `monolith_discover` is helping agents route correctly. |
| Large result/truncation rate | Identify actions that need pagination, summaries, or narrower defaults. |
| Source/query dual-surface count | Confirm expected child query usage without treating it as duplicate telemetry. |

## 6. Surface Contracts

### 6.1 Proxy

Proxy logs wrap MCP `tools/call` handling. They record JSON-RPC id, original tool name, forwarded tool name, redacted arguments, response/error, client metadata, and proxy runtime (`cpp`, `python`, or `node`).

P0 must cover the active script proxy path selected by `Scripts/monolith_proxy.bat`; implementing only `monolith_proxy.exe` is not sufficient for the current checkout.

### 6.2 Query

Query logs wrap `monolith_query.exe <namespace> <action> ...`. They record argv, parsed namespace/action/options, effective DB path, stdout, stderr, fatal error, and exit code.

The CLI compatibility contract is strict: stdout, stderr, and exit code observed by callers must remain unchanged. Fatal paths such as `die()` must become loggable without changing command semantics.

### 6.3 Action

Action logs wrap editor-side `FMonolithToolRegistry::ExecuteAction`. They record normalized namespace/action, effective params after alias handling, validation phase, result/error/hints/warnings, and post-edit validation outcome where available.

File I/O must not run while the registry lock is held. Pre-dispatch failures such as unknown action, profile block, missing param, and strict param rejection must still emit `action.log` when `bEnableDailyLog=true`.

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
| Full log line target | 1 MiB |

When truncating, preserve `truncated=true`, `original_bytes`, and `sha256` of the original serialized payload.

## 8. Implementation Order

1. Add schema constants and redaction/truncation helpers.
2. Add proxy logging to `monolith_proxy.py` and `monolith_proxy.js`; keep C++ proxy schema-compatible.
3. Add native tool logger for `monolith_query.exe`; refactor fatal exits into a loggable top-level path.
4. Add `UMonolithSettings::bEnableDailyLog=false`.
5. Add editor action logger around `FMonolithToolRegistry::ExecuteAction`, with file I/O outside registry locks.
6. Populate `client`, `agent_signal`, `retry_signature`, and `improvement_tags`.
7. Add tests for default-on proxy/query, env disable, `MONOLITH_TOOL_LOG_DIR`, editor off-by-default, redaction, truncation, query output preservation, script proxy parity, and source dual-surface logging.

## 9. Verification Gates

| Gate | Pass criteria |
|---|---|
| Editor off by default | Running an editor action with `bEnableDailyLog=false` creates no `action.log`. |
| Proxy/query default on | With `MONOLITH_TOOL_LOG_ENABLED` unset, proxy/query calls create daily logs. |
| Proxy/query env disable | With `MONOLITH_TOOL_LOG_ENABLED=0`, proxy/query calls create no daily logs. |
| Proxy/query log-dir override | With `MONOLITH_TOOL_LOG_DIR` set, proxy/query logs are written under the override path; if logging is disabled, the override path still receives no log. |
| Script proxy parity | The active script proxy path emits the same schema and env semantics as the C++ proxy. |
| Query output preservation | `monolith_query.exe` stdout/stderr/exit code are unchanged by logging. |
| Source dual-surface | A live `source_query` that shells out to `monolith_query.exe` emits both valid action and query records. |
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
| Retention | P0 keeps logs manually managed. A later setting should cap by age, total bytes, or both. |
| Reader/analyzer | Decide whether the first reader is editor action `monolith.analyze_invocation_logs`, offline `monolith_query logs analyze`, or both. |
| Shared enable config | Decide how editor `bEnableDailyLog` should mirror to proxy/query without requiring environment variables. |
| Trace propagation | Decide whether proxy injects trace metadata into HTTP headers, JSON-RPC params, or a sidecar map. |
| Privacy policy | Decide whether source snippets, asset paths, and user/project identifiers should be logged as raw text, redacted, or hash-only. |
| Schema compatibility | Define analyzer behavior for missing optional fields and future `format_version` changes. |

## 12. Verification Record

2026-05-20 P0 verification:

| Gate | Result |
|---|---|
| Static checks | `node --check Scripts\monolith_proxy.js`, Python py_compile, and `Scripts\ci_static_checks.py check` passed with 0 blocking findings. |
| Native proxy/query builds | `Tools\MonolithProxy\build.bat` and `Tools\MonolithQuery\build.bat` succeeded and copied binaries to `Binaries\`; only C4819 codepage warnings were emitted. |
| Proxy default-on/disable/parity | C++/Python/Node proxy smoke calls produced valid `*_proxy.log` records with redaction and `editor_unavailable` offline outcome; `MONOLITH_TOOL_LOG_ENABLED=0` produced no log. |
| Query default-on/disable/output preservation | `Binaries\monolith_query.exe source health` produced valid `*_query.log`; disabled mode produced no log; enabled and disabled stdout/stderr matched. |
| Action opt-in/redaction/truncation | `Monolith.Core.ToolInvocationLogger.DailyLogOptInRedaction` passed and verified disabled mode, redaction, large payload truncation metadata, and JSONL parsing. |
| Pre-dispatch failure | `Monolith.Core.ToolInvocationLogger.PreDispatchFailureLogged` passed and verified unknown-action lookup failures are recorded with `error_class=unknown_action`. |
| Source dual-surface | `Monolith.Core.ToolInvocationLogger.SourceChildQueryDualSurface` passed and verified action/query records. |
| Headless action matrix | `BatchFiles\RunHeadlessEditor.bat` launched a `-NullRHI` editor with no Monolith command-line override; `monolith_status`, `monolith_discover`, `source.health`, `source.search_source`, `project.search`, `editor.get_build_errors`, unknown `source` action, and missing-param `source.search_source` appended 8 valid `action.log` records. Success, lookup error, and schema rejection outcomes were all recorded. |
| Proxy-to-action headless path | Python proxy calls to `monolith_status`, successful `source.search_source`, and missing-param `source.search_source` appended 3 proxy records and 3 action records against the same headless editor. |
| Final integrated append | With headless editor daily logging enabled from config, proxy `monolith_status`, query `source health`, and editor actions appended records to `Plugins\Monolith\Logs\20260520_proxy.log`, `_query.log`, and `_action.log`; appended records parsed and reported expected outcomes. |
| Broader project build | Primary GoGameEditor UBT build was attempted but failed on unrelated `AGoPlayerController::CreateVirtualJoystick()` unresolved external in `GoGame`. |
