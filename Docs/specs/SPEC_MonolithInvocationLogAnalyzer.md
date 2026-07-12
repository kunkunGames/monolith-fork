# Monolith Invocation Log Analyzer

**Parent:** [SPEC_MonolithToolInvocationLogs.md](SPEC_MonolithToolInvocationLogs.md)
**Status:** Implemented (`Analyzer/analyze_invocation_logs.py`, 2026-06-08, commit e2db9d81); verified against fixtures, a 7-day real log window, and the mixed v1/v2 `Logs/20260520` folder on 2026-06-12 (verification record: [SPEC_MonolithToolInvocationLogs.md](SPEC_MonolithToolInvocationLogs.md) §12)
**Scope:** Local Python analysis tool for `Logs/yyyyMMdd/{proxy,query,action}.jsonl`
**Created:** 2026-06-07

---

## 1. Purpose

Monolith already writes durable daily invocation logs for the MCP proxy, offline
query CLI, and live editor action registry. Those logs are useful only if agents
can turn them into actionable source material for improving Monolith actions,
schemas, routing, latency, and result quality.

This spec defines a read-only Python analyzer that sequentially scans the local
Monolith log tree and produces human and machine-readable findings. The first
implementation target is:

```powershell
python Analyzer/analyze_invocation_logs.py --log-root Logs
```

The analyzer must answer four practical questions:

1. Which Monolith actions or schema fixes are needed because agents repeatedly
   fail, fall back, or route around existing actions?
2. Which calls are redundant, duplicated, retried without progress, or
   double-counted through action-to-query child processes?
3. Where does time accumulate across proxy, action, query, validation, handler,
   child-process, and post-result gaps?
4. Which logs, traces, or result shapes should become source evidence for
   targeted Monolith improvements?
5. Which candidates have the highest expected ROI after separating routine
   heartbeat, test, and maintenance noise from user-facing workflow problems?

The analyzer is local diagnostics tooling. It is not telemetry, not an editor
action, not a replacement for returned tool output, and not allowed to mutate
`Logs/`, `Saved/EngineSource.db`, `Saved/ProjectIndex.db`, or `Saved/graph.db`.

## 2. Existing Contracts

The analyzer consumes the writer contract in
[`SPEC_MonolithToolInvocationLogs.md`](SPEC_MonolithToolInvocationLogs.md).
Daily files live under `Logs/yyyyMMdd/` by default:

| Surface | File | Role |
|---|---|---|
| Proxy | `proxy.jsonl` | MCP proxy request/response, routing context, client context |
| Query | `query.jsonl` | Offline `monolith_query.exe` namespace/action calls |
| Action | `action.jsonl` | Editor registry action validation, handler, and child query records |

Important reader rules:

| Contract | Requirement |
|---|---|
| Mixed schema | Treat `format_version` as a per-record discriminator. A single file can contain v1, v2, and v3 rows. |
| Append-only logs | Do not require file-level finalization or exclusive access. Current-day files can grow while being read. |
| Redaction | Preserve logger redaction and truncation boundaries. Do not reconstruct secrets or hidden payloads. |
| Child process | A live editor source action can legitimately produce one `action.jsonl` row plus one child `query.jsonl` row for the same trace. |
| Status shape | `status=error` can mean unknown action, schema rejection, profile block, handler error, or logging failure; use `agent_signal` and return summaries for cause. |

The existing in-memory `monolith.analyze_tool_call_records` action is a
separate short-lived diagnostic surface. This spec covers durable JSONL analysis
only.

## 3. Goals

### P0 - Read and Normalize

- Scan one or more log roots in date order, then file order: `proxy.jsonl`,
  `action.jsonl`, `query.jsonl`.
- Stream JSONL without loading whole files into memory.
- Continue past malformed lines and emit parse findings unless `--strict` is
  passed.
- Normalize v1/v2/v3 rows into a canonical event model.
- Assign stable local handles to records even when older rows lack `record_id`.

### P1 - Timeline and Trace Reconstruction

- Group records by `trace_id` where available.
- Link records using `span_id`, `parent_span_id`, `previous_record_id`,
  `process_instance_id`, `session_key`, and timestamp fallback.
- Produce per-trace timelines that show proxy to action to child query flow.
- Detect likely action-to-query double-counting and attribute child query time
  to the parent action when `parent_span_id` or `child_process` evidence exists.
- Report orphan records, broken parent links, and mixed-version traces.

### P2 - Improvement Findings

Produce findings for:

- Needed actions or namespaces.
- Confusing or missing schemas.
- Redundant processing and retries.
- Slow actions and delayed workflows.
- Noisy, large, truncated, empty, or duplicate results.
- Environment, index, profile, and editor-availability problems.
- High-ROI candidates ranked by error count, cumulative time, repeat count,
  fallback/escape-hatch evidence, and current implementation risk.

Each finding must include evidence records, severity, confidence, a concrete
recommendation, and a stable category.

### P3 - Source Artifacts

The analyzer output should be usable as source input for follow-up Monolith
work. Machine-readable reports must include enough bounded evidence to create
issues, specs, PR descriptions, or future automated triage without rereading
raw logs.

## 4. Non-Goals

- Do not edit, compact, rotate, delete, or rewrite `Logs/`.
- Do not call the live MCP server or require the Unreal Editor.
- Do not read or mutate Monolith SQLite DBs.
- Do not infer hidden chain-of-thought or agent private reasoning.
- Do not automatically create actions, schemas, branches, commits, or PRs.
- Do not treat a single inferred intent as proof; aggregate evidence is
  required before recommending action changes.

## 5. Command Line Contract

Initial command:

```powershell
python Analyzer/analyze_invocation_logs.py --log-root Logs
```

Recommended options:

| Option | Default | Meaning |
|---|---|---|
| `--log-root <path>` | `Logs` | Root containing `yyyyMMdd` folders, or a direct folder with JSONL files. Repeatable. |
| `--since yyyyMMdd` | unset | Include date folders on or after this date. |
| `--until yyyyMMdd` | unset | Include date folders on or before this date. |
| `--files proxy,action,query` | all | Limit surfaces. |
| `--out <path>` | `Saved/Monolith/LogAnalysis/<timestamp>` | Output directory. Must not be under `Logs/`. |
| `--format markdown,json,csv` | `markdown,json` | Report formats to emit. |
| `--top <n>` | `50` | Maximum rows per ranked table. |
| `--max-lines <n>` | unset | Stop after N records for smoke tests. |
| `--category <name>` | all | Limit findings to a category. Repeatable. |
| `--strict` | false | Fail on malformed JSON or unsupported required fields. |
| `--emit-normalized-jsonl` | false | Write normalized records for downstream analysis. |
| `--include-raw-snippets` | false | Include bounded raw snippets from already-redacted fields. |
| `--include-paths` | false | Preserve full local paths in output; otherwise shorten or hash where possible. |
| `--min-severity <level>` | `info` | Filter findings by severity. |
| `--include-heartbeats` | false | Include routine heartbeat/status records in top findings. By default they are summarized but excluded from ROI ranking. |
| `--include-synthetic-tests` | false | Include known synthetic test actions such as `__missing_action_*` in missing-action recommendations. |
| `--recent-days <n>` | `3` | Recency window = the last N present date folders. Drives `still_open` and `recency_score` (see 7.6). |
| `--fix-boundary yyyyMMdd` | unset | Dates on or after the boundary count as `recent`; everything earlier is `historical`. Overrides `--recent-days` so per-item fix dates can be respected. |
| `--rank-by-recency` | false | Re-rank ranked findings by `recency_score` instead of the lifetime `score`. Default off keeps the legacy ordering; the recency fields are emitted either way. |

The command exits with:

| Exit code | Meaning |
|---|---|
| `0` | Analysis completed, no internal errors. Findings can still be present. |
| `1` | Analyzer input or output contract failure. |
| `2` | Strict-mode parse/schema failure. |

## 6. Canonical Event Model

The parser should normalize every accepted JSONL row into:

| Field | Source |
|---|---|
| `source_file` | Physical JSONL file path, shortened in reports unless `--include-paths`. |
| `line_number` | 1-based line number. |
| `surface` | Record `surface` or file-derived fallback. |
| `format_version` | Record `format_version` or `0` for legacy/unknown. |
| `record_key` | `record_id`, else stable hash of file, line, surface, timestamp, and call identity. |
| `trace_id` | Record `trace_id` when present. |
| `span_id` | Record `span_id` when present. |
| `parent_span_id` | Record `parent_span_id` when present. |
| `previous_record_id` | Record `previous_record_id` when present. |
| `process_instance_id` | Record `process_instance_id` when present. |
| `session_key` | Record `session_key` when present. |
| `start_time` / `end_time` | Parsed timestamps when present. |
| `duration_ms` | Record duration or timestamp-derived fallback. |
| `status` | `success`, `error`, or `unknown`. |
| `namespace` / `action` | From `call.namespace`, `call.action`, tool name, argv, or parser fallback. |
| `tool_name` | MCP tool name or offline executable action label. |
| `argument_fingerprint` | Stable hash of normalized bounded argument data. |
| `retry_signature` | `workflow.retry_signature` or action plus argument fingerprint fallback. |
| `return_summary` | Bounded summary object. |
| `agent_signal` | Bounded signal object. |
| `phase_timing` | Phase timing object. |
| `environment` | Environment object. |
| `child_process` | Child process object. |
| `parse_warnings` | Missing or degraded fields. |

Field extraction must be defensive. Missing nested objects are normal in older
records and should degrade to warnings, not crashes.

## 7. Analysis Modules

### 7.1 Action Gap Analysis

Purpose: identify places where Monolith probably needs a new action, schema
repair, better docs, or routing improvements.

Signals:

- `agent_signal.improvement_tags` contains `missing_action`,
  `schema_confusing`, `routing_confusing`, or similar tags.
- `agent_signal.outcome` is `validation_rejected`, `unknown_action`,
  `profile_blocked`, or `handler_error`.
- Repeated schema failures for the same namespace/action and missing parameter.
- Repeated use of broad `monolith.find`, `monolith.discover`, source text
  search, or raw query patterns immediately before a more specific task.
- Repeated fallback from live action to offline query for the same intent.
- Error text indicates a parameter name mismatch, for example a caller using
  `namespace` when the schema expects `target_namespace`.

Output:

- `needed_action` findings for missing or weak surfaces.
- `schema_fix` findings for parameter or validation confusion.
- `routing_doc` findings where current actions exist but agents route poorly.

### 7.2 Duplicate and Retry Analysis

Purpose: find redundant processing that costs time or causes noisy workflows.

Signals:

- Same `retry_signature` repeated within a configurable time window.
- Same namespace/action plus argument fingerprint repeated by one session or
  process without an intervening different result.
- Same search result shape and same large/truncated payload repeated.
- Action row plus child query row counted as two independent expensive calls.
- Repeated malformed calls with identical error class.
- Duplicate source/project result rows, including repeated unknown or empty
  symbol names.

`duplicate_retry` is failure-specific: only canonical records whose `status` is
not `success` contribute to its count, score, CSV row, and evidence. v3 loggers
also stamp `retry_signature` on successful calls, but a repeated successful
signature is not itself proof of a retry loop; successful duplicate-work
findings require separate result-shape or workflow-sequence evidence.

Output:

- `duplicate_call` findings for exact or near-exact repeats.
- `retry_loop` findings when errors repeat without progress.
- `double_counted_child_query` findings for parent/child attribution.
- `noisy_result_duplicate` findings for duplicate rows inside one result.

### 7.3 Latency Analysis

Purpose: find slow actions and delayed agent workflows.

Metrics:

- Counts, p50, p90, p95, p99, max duration by surface, namespace, action,
  status, and environment profile.
- Phase timing attribution from `phase_timing`.
- Parent action vs child query time attribution from `child_process`.
- Gaps from `time_since_previous_ms` and timestamp fallback.
- Slow error paths, not only slow successful paths.

Default thresholds:

| Finding | Default threshold |
|---|---|
| Slow call | Above p95 for same action or above 5 seconds when no baseline exists. |
| Very slow call | Above p99 for same action or above 30 seconds. |
| Slow validation/rejection | Validation or pre-dispatch failure above 1 second. |
| Long continuation gap | `time_since_previous_ms` above 60 seconds inside same session/process. |
| Child query dominance | Child query time is above 70 percent of parent action duration. |

Output:

- `slow_action` findings with phase breakdown.
- `slow_error_path` findings when failing calls are unexpectedly expensive.
- `delayed_continuation` findings where agents pause after a result.
- `child_query_bottleneck` findings for source/bridge/project actions that
  spend most time in `monolith_query.exe`.

### 7.4 Result Quality Analysis

Purpose: identify output shapes that make agents call more tools than needed.

Signals:

- Large `return_summary.payload_bytes`, truncation flags, or high top-level key
  counts.
- Empty result sets followed by broad discovery or fallback queries.
- High warning/error counts in otherwise successful results.
- Duplicate result rows, repeated `<unknown>` names, or missing identity fields.
- Result lacks stable handles, paths, counts, cursors, or `next_actions` where
  the action family normally supports them.

Output:

- `large_result` findings.
- `empty_result_followed_by_fallback` findings.
- `missing_result_identity` findings.
- `result_noise` findings.

### 7.5 Environment and Index Analysis

Purpose: distinguish tool design problems from runtime environment problems.

Signals:

- Editor unavailable or headless capability limits.
- Tool profile blocks.
- Stale, missing, or mismatched index health in `environment.index_health`.
- Mixed process versions or mixed `format_version` distribution on one date.
- Repeated DB path or source/project index warnings.

Output:

- `environment_blocker` findings.
- `index_health` findings.
- `mixed_schema` findings.
- `profile_block` findings.

### 7.6 ROI Ranking and Noise Classification

Purpose: keep the report focused on work that is likely to improve agent
throughput or reduce repeated failures.

The current logs contain legitimate low-value noise, especially status polling,
synthetic validation tests, and source-index maintenance. If these records are
not separated before ranking, they dominate the report and hide user-facing
workflow problems.

Noise classes:

| Class | Default handling | Examples |
|---|---|---|
| `heartbeat` | Summarize counts and duration, exclude from ROI ranking unless `--include-heartbeats` is passed. | `monolith.status`, `monolith_status` proxy calls |
| `synthetic_test` | Summarize separately, exclude from missing-action recommendations unless `--include-synthetic-tests` is passed. Two primary signals on v3 rows: `environment.is_automation_test == true` (stamped from `GIsAutomationTesting`, so it covers only in-process C++ automation) and `routing_context.client_kind == "benchmark"` (self-declared by the out-of-process benchmark runners, which send hallucinated action names and typo fixtures on purpose — without it those rows rank as `needed_action` demand). Synthetic argument markers and per-action fixture whitelists remain only as fallback for legacy rows without either stamp. | `__missing_action_for_headless_log_test`, `__cc05_dispatch_ns__`, `worldgen.get_blockout_volumse` (benchmark typo fixture) |
| `maintenance` | Include in ROI ranking only when repeated, slow, or failing. | `source.repair_crg_cache`, `source.build_crg_graph`, `source.health` |
| `expected_slow_domain` | Rank by error/retry rate first, duration second. | image generation actions |
| `escape_hatch` | Always rank when frequent enough to suggest missing first-class actions. | `editor.run_python` |

Recommended score inputs:

| Signal | Weighting intent |
|---|---|
| `error_count` | High weight. Repeated errors directly block workflows. |
| `validation_rejected_count` | High weight when concentrated on one schema or parameter alias. |
| `unknown_action_count` | High weight after synthetic tests are removed. |
| `total_duration_ms` | High weight for maintenance/source actions; lower weight for expected slow domains. |
| `repeat_count` | High weight when identical retry signatures recur without new evidence. |
| `escape_hatch_count` | High weight because it usually means agents cannot find or trust first-class actions. |
| `large_or_duplicate_result_count` | Medium weight unless followed by fallback calls. |
| `implementation_risk` | Manual adjustment bucket: low, medium, high. Prefer low-risk fixes with strong evidence. |

Output:

- `high_roi_candidate` findings with rank, score components, evidence counts,
  likely owner namespace, recommended first fix, and deferred follow-up.
- A `noise_summary` table so excluded heartbeat/test traffic remains visible.
- A `maintenance_loop` finding when expensive index/CRG actions repeat with the
  same arguments and no freshness reason is visible in the logs.
- An `escape_hatch_replacement` finding when `editor.run_python` or similar
  generic actions are frequent enough to justify mining their bounded payloads
  for new action contracts.

#### Recency dimension (still-open vs newly-quiet)

Lifetime counts alone are recency-blind: an action fixed weeks ago keeps a high
lifetime score even though it stopped failing, so a one-shot fix can be
mis-ranked as "open and worsening" (this happened to `describe.action_schema`
and `imagegen.generate_image_via_ima2` during backlog authoring). Every
per-action finding therefore carries a `recency` block derived from a
**recent vs historical** split of that action's calls/errors.

- **Window.** The recent window is the last `--recent-days` *present* date
  folders (default 3), or — with `--fix-boundary yyyyMMdd` — every folder on or
  after the boundary. `--fix-boundary` lets a reviewer respect a known per-item
  fix date instead of the rolling default. The window is computed over dated
  folders only; non-dated (`direct`) files never count as recent.
- **Metric.** Error-shaped findings (schema, high-error, unknown-action, retry,
  expected-slow-domain) key `still_open` on **recent errors**; cost/activity
  findings (`maintenance_loop`, `large_result`, `slow_action`,
  `child_query_bottleneck`) key it on **recent calls**, because the CRG
  maintenance loop bleeds wall-time with ~0 errors and must not read as closed.
- **`still_open` and no-data vs zero-error.** For error-metric findings,
  `still_open=true` when recent errors > 0; `false` (`newly_quiet`/`stable_quiet`)
  when there are recent calls but no recent errors; and `null`
  (`no_recent_data`) when the action had **no calls** in the window — a missing
  signal is not a passing one (an editor action with 0 calls on the latest days
  is *no-data*, not a proven fix).
- **`recency_status`** is one of `still_open`, `regressed` (recent error rate ≥
  historical), `newly_quiet` (was loud, now quiet), `stable_quiet`, or
  `no_recent_data`.
- **Scoring.** `recency_score = score * weight(recency_status)`. It is always
  emitted but only changes ranking when `--rank-by-recency` is passed, so the
  default `score`/`rank` ordering is unchanged for existing consumers.
- **Views.** `findings.json` gains a top-level `recency` object with
  `still_open`, `regressions`, `newly_quiet`, and `no_recent_data` lists (rank
  order), and the markdown report adds matching sections plus a `Recency`
  column on the High ROI Backlog. These are additive; legacy fields and the
  default rank/score are unchanged.

## 8. Output Contract

Each run writes an output directory outside `Logs/`:

```text
Saved/Monolith/LogAnalysis/<timestamp>/
  summary.md
  findings.json
  action_stats.csv
  slow_calls.csv
  duplicates.csv
  parse_warnings.csv
  normalized.jsonl        # only when --emit-normalized-jsonl is passed
```

### 8.1 Console Summary

Console output should stay short:

```text
Scanned 3 date folders, 9 files, 12842 records.
Findings: 4 high, 12 medium, 31 low, 18 info.
Top categories: slow_action=14, schema_fix=8, duplicate_call=7.
Report: Saved/Monolith/LogAnalysis/20260607-143022/summary.md
```

### 8.2 Markdown Report

`summary.md` contains:

1. Run inputs and limits.
2. Record counts by date, file, surface, format version, and status.
3. Top findings by severity.
4. High-ROI backlog, with heartbeat and synthetic-test noise excluded by
   default.
5. Action gap recommendations.
6. Duplicate/retry findings.
7. Latency tables and phase attribution.
8. Result quality findings.
9. Environment/index findings.
10. Noise summary for heartbeat, synthetic tests, and maintenance loops.
11. Parse warnings and unsupported schema notes.
12. Appendix of bounded evidence references.

### 8.3 Findings JSON

`findings.json` must be stable enough for follow-up tooling:

```json
{
  "schema_version": 1,
  "generated_at": "2026-06-07T14:30:22+09:00",
  "inputs": {
    "log_roots": ["Logs"],
    "since": null,
    "until": null
  },
  "summary": {
    "records_scanned": 12842,
    "files_scanned": 9,
    "findings_by_severity": {
      "high": 4,
      "medium": 12,
      "low": 31,
      "info": 18
    }
  },
  "findings": [
    {
      "finding_id": "schema_fix:action:describe.action_schema:missing_target_namespace",
      "category": "schema_fix",
      "severity": "medium",
      "confidence": 0.91,
      "title": "describe.action_schema callers use namespace instead of target_namespace",
      "recommendation": "Consider schema aliasing, clearer parameter docs, or routing hints for target_namespace.",
      "evidence": [
        {
          "record_key": "act-...",
          "source_file": "Logs/20260606/action.jsonl",
          "line_number": 12,
          "trace_id": "trace-...",
          "surface": "action",
          "namespace": "describe",
          "action": "action_schema",
          "status": "error",
          "duration_ms": 1.7
        }
      ],
      "sample": {
        "error_class": "missing_param",
        "missing_param": "target_namespace",
        "provided_param": "namespace"
      },
      "still_open": false,
      "recency_status": "newly_quiet",
      "recency_score": 261.0,
      "recency": {
        "metric": "errors",
        "status": "newly_quiet",
        "still_open": false,
        "weight": 0.15,
        "recent_calls": 4,
        "recent_errors": 0,
        "historical_errors": 109,
        "last_call_date": "20260612",
        "recency_score": 261.0
      }
    }
  ]
}
```

Per-action findings carry `action_key`, plus the additive `still_open`,
`recency_status`, `recency_score`, and `recency` fields described in 7.6.
`still_open` is `true`/`false`/`null` (`null` = no recent calls, i.e. no-data,
not a proven fix). The top-level `recency` object groups findings into
`still_open`, `regressions`, `newly_quiet`, and `no_recent_data` lists alongside
the recent-window metadata (`recent_days`, `fix_boundary`, `recent_dates`,
`rank_by_recency`).

Evidence objects should include handles and bounded summaries, not full raw
payloads. Full local paths require `--include-paths`.

## 9. Privacy and Safety

- Never scan outside explicit log roots.
- Never write output under `Logs/`.
- Never search for tokens, API keys, cookies, bearer headers, or private keys.
- Preserve redacted values exactly as redacted.
- Bound raw snippets by byte length and disable them by default.
- Prefer shortened paths in reports. Full paths require `--include-paths`.
- Do not emit raw argument or return payloads unless they are already bounded by
  the logger and `--include-raw-snippets` is set.

## 10. Performance Requirements

- Stream line-by-line with bounded memory.
- Support at least 20 GB of logs on a local SSD without loading all records.
- Use counters, sketches, bounded heaps, and top-N aggregators for global stats.
- Keep only bounded evidence samples per finding key.
- Do not hold file handles longer than needed.
- Treat files appended during analysis as best-effort snapshots.
- Handle locked or partially written current-day files by skipping incomplete
  final lines unless `--strict` is passed.

## 11. Implementation Plan

### P0 - Parser and Scanner

- Add `Analyzer/analyze_invocation_logs.py`.
- Implement date/file discovery and deterministic scan order.
- Implement JSONL streaming, parse warning collection, and strict mode.
- Implement canonical event normalization for v1/v2/v3 records.

### P1 - Aggregation Core

- Add counters by date, surface, format version, status, namespace, and action.
- Add duration aggregators and top-N slow call tracking.
- Add trace grouping with bounded per-trace event references.
- Add parent/child query attribution.
- Add noise classification for heartbeat, synthetic-test, maintenance, expected
  slow-domain, and escape-hatch records before ranking findings.

### P2 - Finding Engine

- Implement the analysis modules in section 7.
- Use deterministic `finding_id` values.
- Record severity, confidence, recommendation, and bounded evidence.
- Implement `high_roi_candidate`, `noise_summary`, `maintenance_loop`, and
  `escape_hatch_replacement` findings.
- Keep thresholds configurable as constants first; add config file support only
  if repeated tuning proves necessary.

### P3 - Reports

- Emit `summary.md`.
- Emit `findings.json`.
- Emit CSV tables for action stats, slow calls, duplicate calls, and parse
  warnings.
- Add optional `normalized.jsonl`.

### P4 - Fixtures and Tests

- Add compact fixtures under `Analyzer/fixtures/invocation_logs/`.
- Include mixed v1/v2/v3 rows, malformed JSON, action-to-query child pairs,
  schema confusion, repeated retry signatures, slow phase timings, large
  truncated returns, and duplicate noisy result rows.
- Add focused tests for parser, aggregation, and findings.

## 12. Acceptance Criteria

The first implementation is acceptable when all criteria pass:

| Area | Criteria |
|---|---|
| Current logs | `python Analyzer/analyze_invocation_logs.py --log-root Logs --out <temp>` reads the current Monolith log root without crashing. |
| Known schema confusion | A fixture modeled after `describe.action_schema` receiving `namespace` instead of `target_namespace` produces a `schema_fix` finding. |
| Mixed schema | A fixture with v1/v2/v3 rows reports schema distribution and does not treat the file as one schema version. |
| Child query | A parent action plus child `query.jsonl` row is not double-counted as two unrelated slow operations. |
| Duplicate retry | Repeated same action plus argument fingerprint produces a duplicate or retry-loop finding. |
| Heartbeat noise | Repeated `monolith.status` records are counted in `noise_summary` but excluded from default high-ROI rankings. |
| Maintenance loop | Repeated slow `source.repair_crg_cache` or `source.build_crg_graph` fixtures produce a `maintenance_loop` finding. |
| Escape hatch | Repeated `editor.run_python` fixtures produce an `escape_hatch_replacement` finding. |
| Malformed line | Non-strict mode reports parse warnings and continues; strict mode exits with code 2. |
| Output | `summary.md`, `findings.json`, and CSV files are created outside `Logs/`. |
| Safety | The analyzer never writes to `Logs/` and never opens SQLite DBs. |
| Static checks | `python -m py_compile Analyzer/analyze_invocation_logs.py` passes. |
| Repo checks | `python Scripts/ci_static_checks.py --config .github/monolith-static-ci.json --github check` passes or reports only unrelated external blockers. |

## 13. Seed Findings From Current Logs

The implementation should be able to detect these real patterns when equivalent
records are present:

| Pattern | Expected category |
|---|---|
| `describe.action_schema` called with `namespace` while the schema expects `target_namespace`. | `schema_fix` |
| Repeated source search results with duplicate unknown symbols or low-identity rows. | `result_noise` |
| Action rows that synchronously launch `monolith_query.exe`. | `child_query_bottleneck` or `double_counted_child_query` |
| Mixed v3 action/query rows across active daily folders. | `mixed_schema` only when older rows appear in the same analyzed range |
| Broad discovery or search calls repeated before specific action selection. | `routing_doc` or `needed_action` depending on evidence |
| `monolith.status` dominating raw record counts. | `noise_summary`, excluded from default ROI ranking |
| Repeated expensive source maintenance calls with identical retry signatures. | `maintenance_loop` |
| Frequent `editor.run_python` calls with `escape_hatch` tags. | `escape_hatch_replacement` |

These are seed expectations, not hardcoded file or line dependencies. Tests
should use minimal fixtures rather than relying on local `Logs/` contents.

## 14. Log-Grounded High-ROI Backlog

This section is based on a read-only scan of the local `Logs/` tree on
2026-06-07. The scan covered 37 JSONL files, 41,459 records, and reported zero
parse errors. The date range was `20260520` through `20260607`; records include
mixed v1/v2/v3 rows on the earliest days and v3 rows afterward.

These observations should shape the first implementation and the first
Monolith-improvement candidates produced by the analyzer.

| Rank | Evidence | Candidate | Why ROI is high | Analyzer requirement |
|---|---|---|---|---|
| 1 | `action:monolith.status` appears 25,001 times, about 60 percent of all records, with 24,944 identical retry signatures. | Heartbeat/status noise suppression and status-poll loop summary. | Without this filter, every report is skewed toward a healthy low-cost action instead of real work. | Implement `heartbeat` noise class and exclude it from default high-ROI ranking while preserving counts. |
| 2 | `query:source.repair_crg_cache` appears 295 times with about 67,032 seconds cumulative duration; `query:source.build_crg_graph` appears 278 times with about 47,691 seconds cumulative duration; `query:source.health` appears 279 times with about 5,214 seconds cumulative duration. | Source maintenance loop detection and freshness-gate recommendations. | Repeated source/CRG maintenance dominates wall time and likely hides stale-cache or over-eager repair behavior. | Emit `maintenance_loop` findings with cumulative time, repeated signature evidence, and freshness/index context. |
| 3 | 348 validation rejects carry schema-confusing evidence; concentrated examples include `describe.action_schema` missing `target_namespace` 109 times, `source.read_file` missing params 42 times, and `source.read_source` missing params 23 times. | Schema alias/hint backlog. | Small schema/doc fixes can remove repeated failed tool calls and reduce agent retries. | Rank `schema_fix` findings by rejected count and provided-vs-required parameter evidence. |
| 4 | 98 unknown-action errors include real caller confusion such as `project --help`, `source --help`, `source.trigger_project_reindex`, and source graph/search synonyms; synthetic test actions also appear. | Missing-action and synonym triage with synthetic-test exclusion. | Helps distinguish real missing affordances from test rows and names that should route to existing actions. | Classify synthetic namespaces/actions separately, then rank real `unknown_action` groups. |
| 5 | `action:editor.run_python` appears 197 times and every call is tagged `escape_hatch`; 15 calls are validation rejects and several are slow. | Escape-hatch replacement mining. | Frequent generic Python execution usually means first-class actions are missing, undiscoverable, or too weak. | Emit `escape_hatch_replacement` findings and cluster bounded payload hints without exposing raw secrets. |
| 6 | `action:imagegen.generate_image_via_ima2` has 201 calls, about 9,856 seconds cumulative duration, and 87 errors. | Expected-slow-domain error-rate ranking. | Duration alone is expected for generation, but the error rate is high enough to deserve separate ranking. | Mark image generation as `expected_slow_domain`; rank primarily by failures/retries. |
| 7 | Large result examples include `source.find_overrides` at 282,723 bytes, repeated `blueprint.search_functions` at 265,751 bytes, repeated `audio.list_available_metasound_nodes` at 213,195 bytes, and `paper2d.list_assets` at 209,196 bytes. | Projection/pagination and duplicate-result recommendations. | Large repeated payloads increase context pressure and drive follow-up calls. | Emit `large_result`, `noisy_result_duplicate`, and projection/cursor recommendations with bounded samples. |
| 8 | High error-rate actions include `query:<none>.<none>` 52/52 errors, GAS scaffolding actions with 100 percent errors in sampled groups, `blueprint.get_component_details` 61/150 errors, and `query:source.read_source` 118/394 errors. | High-error-rate action health table. | Repeated failed surfaces should be fixed or made self-correcting before adding new actions. | Add an error-rate table with minimum sample threshold and top evidence examples. |

The first implementation should not simply report these exact rows. It should
implement the general detectors above, then prove them using compact fixtures
modeled after these observed patterns.

## 15. Open Decisions

| Decision | Default for P0 |
|---|---|
| Threshold configurability | Use constants and command-line overrides only after real reports show a need. |
| Output retention | Write under `Saved/Monolith/LogAnalysis/`; cleanup remains manual. |
| Report language | Use English keys and headings for stable machine consumption. |
| Live MCP integration | Defer. The analyzer is offline/read-only. |
| CI fixture size | Keep fixtures small and deterministic; do not commit real logs. |
