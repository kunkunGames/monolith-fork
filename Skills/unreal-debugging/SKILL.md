---
name: unreal-debugging
description: Use when diagnosing Unreal Engine failures via Monolith MCP (editor namespace) — POST-FAILURE forensics over editor/output logs, crash context, stack traces, assertions, ensures, and common UE error patterns. unreal-debugging owns reading and diagnosing failure logs; for actively compiling/Live-Coding or first-line build-error fixing use unreal-build, for slowness/profiling use unreal-performance, to verify a C++ signature or include use unreal-cpp. Triggers on debug, crash, log, output log, editor crash, stack trace, callstack, assertion, ensure, check() failed, exception, GPU crash, freeze, hang, why did it crash, search log for, build error, compile error.
---

# Unreal Debugging Workflows

Editor diagnostic actions via `editor_query()`. unreal-build, unreal-debugging, and unreal-performance all drive the live **editor** namespace — confirm action names and schemas against the live catalog before calling, never trust a memorized name.

## Discovery

```text
monolith_discover({ namespace: "editor" })                                          # list editor-namespace actions
monolith_discover({ namespace: "editor", action: "get_crash_context", mode: "schema" })  # exact params
monolith_discover({ namespace: "editor", action: "search_logs", mode: "schema" })        # exact params
```

## When to use / Use a different skill for

This skill owns **post-failure forensics** — reading and diagnosing editor/output logs, crash context, stack traces, assertions, and ensures. Route elsewhere when:

- Actively compiling, recompiling, Live-Coding, or fixing a first-line build/link error → `unreal-build`
- Slowness, frame-time, draw-call, or shader-cost profiling (not a crash) → `unreal-performance`
- Verifying a C++ signature, include path, or symbol while root-causing → `unreal-cpp`

(`unreal-build`, `unreal-debugging`, and `unreal-performance` overlap on the `editor`/build seam — this skill is the one for *after it already failed*.)

## Action Reference

Param notation: `name*` required, `name?` optional, `name=val` default, `a/b/c` allowed, `[w]` mutates (transaction-wrapped). Signatures are a snapshot of the live catalog — for the exact full schema call `monolith_discover` with `mode: "schema"` (the discover-first block above is the authority).

| Action | Purpose | Params |
|--------|---------|--------|
| `trigger_build` / `live_compile` [w] | Live Coding compile (`live_compile` is an alias) | `wait?=false` |
| `get_build_errors` | Compile errors/warnings, bucketed compile vs other. Window precedence: marker > iso > seconds > last compile | `since_marker?` `since_iso?` `since_seconds?` `since?` `clear_baseline?=false` `category?` `compile_only?=false` `exclude_categories?` |
| `get_build_status` | Compiling / last_result / errors_since_compile / patch_applied | (none) |
| `get_build_summary` | Last build errors, warnings, time | (none) |
| `search_build_output` | Search build log output by pattern | `pattern*` `limit?=100` |
| `get_recent_logs` | N most recent log entries | `count?=50` |
| `search_logs` | Search by pattern, category, verbosity | `pattern?` `category?` `verbosity?` (error/warning/log/verbose) `limit?=100` |
| `tail_log` [w] | Last N log lines (like `tail -f`) | `count?=50` |
| `get_log_categories` | All active log categories | (none) |
| `get_log_stats` | Error/warning counts by verbosity level | (none) |
| `get_compile_output` | Structured compile report: result, time, error/warning counts, patch status | (none) |
| `get_live_coding_diagnostics` | Normalized Live Coding availability, compile result, freshness, bounded log excerpts | `max_log_entries?` (≤200) |
| `get_crash_context` | Last crash/ensure context: stack trace, system info | (none) |

Crash-breadcrumb forensics (Monolith's own crash ledger, distinct from `get_crash_context`):

| Action | Purpose | Params |
|--------|---------|--------|
| `get_last_crash_reason` | Most recent Monolith crash breadcrumb (tool, action, params, timestamp) | (none) |
| `list_recent_crashes` | Crash breadcrumbs newest-first | `limit?=20` `since?` (ISO8601) `tool?` |
| `get_crash_stats` | Aggregate crash counts grouped by tool/action | `since?` (ISO8601) `group_by?=tool` (tool/action/tool_action) |

## Workflows

### After modifying C++
```
editor_query({ action: "trigger_build", params: {} })
// Wait ~10s for Live Coding
editor_query({ action: "get_build_errors", params: {} })
```

### Recipe — Full crash triage (context → fatal/error logs → stats → handoff)
All actions below are from this skill's Action Reference table; the final two steps hand off to other skills by name.
```
// 1. Pull the last crash/ensure: stack trace + system info
editor_query({ action: "get_crash_context", params: {} })
// 2. Find the fatal line, then widen to errors around it (pattern + verbosity from the table)
editor_query({ action: "search_logs", params: { pattern: "Fatal", verbosity: "error", limit: 20 } })
editor_query({ action: "search_logs", params: { pattern: "Error", verbosity: "error", limit: 50 } })
// 3. Get error/warning counts by verbosity level to gauge severity/scope
editor_query({ action: "get_log_stats", params: {} })
// 4. Narrow noise to the suspect log category once the stack names one
editor_query({ action: "search_logs", params: { category: "LogTemp", verbosity: "error", limit: 50 } })
// 5. HANDOFF: resolve the crashing symbol/signature/include → use unreal-cpp (source_query)
// 6. HANDOFF: recompile the fix → use unreal-build (trigger_build / get_build_errors)
```

### Investigate a crash (quick)
```
editor_query({ action: "get_crash_context", params: {} })
editor_query({ action: "search_logs", params: { pattern: "Fatal", limit: 20 } })
```

### Find specific log output
```
editor_query({ action: "search_logs", params: { pattern: "MyActor", category: "LogTemp", verbosity: "Warning" } })
```

## Common Error Patterns

- **LNK2019/LNK2001:** Missing module in `.Build.cs`. `DeveloperSettings` is separate from `Engine`.
- **Include path errors:** Use `source_query("search_source", ...)` to find correct header. Note: `get_include_path` does NOT exist as an action.
- **Live Coding limits:** Header changes (new members, class layout) require editor restart + UBT build. Only `.cpp` body changes work.
- **Package errors:** `CreatePackage` with same path returns existing in-memory package.

## Reflection Intelligence (context while root-causing)

When a bug is in unfamiliar territory, the Reflection Intelligence (RI) namespaces give deterministic, $0-LLM context before you start guessing. Scope: project game module + project plugins.

- **`cppreflect`** — inspect the reflected surface of the suspect class: `get_uclass`, `list_uproperties`, `list_ufunctions`, `find_interface_impls`, `find_class_specifier` / `list_class_specifiers`.
- **`network`** — when the bug is replication-shaped: `list_replicated_classes`, `list_rpc_functions`, `list_onrep_handlers`, and `audit_unbalanced_onreps` (a `ReplicatedUsing=OnRep_X` with no matching handler is a classic replication-bug source).
- **`decision`** — surface prior architectural decisions for the buggy area before "fixing" something that was a deliberate choice: `list_decisions` (filter by `path_filter`), `get_decision`, `find_supersession_chain`, `find_referent_decisions`, `list_stale`.
- **`risk`** — locate the danger zones: `get_hotspot_score` / `get_release_window_hotspots` (churn+complexity), `get_cochange_pairs` (files that historically change together — likely co-affected), `get_file_churn`, `list_conditional_gates` (find `#if WITH_*` regions that may be gating the bug out).

## Tips

- Log buffer: 10,000 entries, 5 build histories
- Use `search_logs` with category filters to reduce noise
- `get_build_summary` shows trends -- useful for spotting regressions
- Combine with `source_query` for engine internal errors
- `risk("get_cochange_pairs")` predicts which other files a fix may need to touch
- After changing C++ reflection structure, `reflect("rebuild_reflection_index")` refreshes the RI tables (project-only)
