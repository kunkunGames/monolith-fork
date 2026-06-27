---
name: unreal-console
description: Use when inspecting or using Unreal's IConsoleManager console object registry via Monolith MCP - live console variables, console commands, registered help text, snapshot search, guarded command execution, post-command logs, command sequences, scoped CVars, and console screenshot evidence. Use this for cvars plus command entries and non-variable console objects; use unreal-config for .ini hierarchy/config file resolution, and unreal-performance for profiling-driven tuning decisions. Triggers on console command, console object, cvar, IConsoleManager, exec command, r. cvar, stat command, console help, console hint, command execution.
---

# unreal-console

**15 live actions** via `console_query(action, params)`. Runtime-only actions return explicit `live_only` guidance through offline `Binaries\monolith_query.exe console ...`; call `monolith_discover` for exact live parameter schemas before invoking MCP.

## Discovery

```
monolith_discover({ namespace: "console" })
monolith_discover({ namespace: "console", action: "<action>", mode: "schema" })
```

Offline snapshot reads are available through `Binaries\monolith_query.exe console ...` when the editor is unavailable:

```
Binaries\monolith_query.exe console search_objects shadow --limit=10
Binaries\monolith_query.exe console get_object r.ShadowQuality
Binaries\monolith_query.exe console health --include-counts=true
```

## When To Use

Use this skill for the full `IConsoleManager` registry: console variables, console command entries, help/hint text, object type filtering, fast FTS-backed lookup, guarded execution, log-backed verification, and scoped CVar runtime checks.

Use a different skill for:

- **unreal-config** - `.ini` hierarchy resolution, config section reads, project settings, or focused CVar lookup without command/object search.
- **unreal-performance** - profiling-driven CVar or rendering/scalability tuning decisions.
- **unreal-debugging** - log/crash forensics after a console command caused warnings, errors, or crashes.

## Action Reference

Param notation: `name*` required, `name?` optional, `name=val` default, `a/b/c` allowed values, `[w]` mutates or can affect editor/runtime state. Signatures are a snapshot of the live catalog - confirm with `monolith_discover` before calling.

| Action | Purpose | Params |
|--------|---------|--------|
| `list_live_objects` | Enumerate currently registered `IConsoleManager` objects directly from the live editor process. | `query?` `mode=prefix/contains` `object_type=all/variable/command/object` `limit=100` |
| `refresh_snapshot` `[w]` | Capture the live registry into `EngineSource.db` (`console_objects`, `console_objects_fts`, `console_snapshot_meta`) for fast MCP and offline search. | `query?` `mode=prefix/contains` `object_type=all/variable/command/object` `limit=0` |
| `search_objects` | Search the captured snapshot with FTS5 and LIKE fallback; empty query lists snapshot rows. | `query?` `object_type=all/variable/command/object` `limit=100` |
| `get_object` | Read one captured console object by exact name. | `name*` |
| `health` | Report console snapshot schema, row count, FTS parity, and snapshot metadata. | `include_counts=false` |
| `resolve_command` | Resolve a command line against the live registry and target-world route without executing it. | `command*` `target_world=auto/pie/editor` `include_values=true` `include_defaults=true` |
| `execute` `[w]` | Execute a console command through the editor world/PIE context with dry-run, optional registry existence guard, and optional target-world constraint. | `command*` `dry_run=true` `require_known_object=false` `target_world=auto/pie/editor` |
| `get_log_cursor` | Return the live Monolith log-capture cursor for isolating future command output. | none |
| `search_logs_since` | Search only logs emitted after a cursor. | `cursor*` `pattern?` `category?` `verbosity=very_verbose` `limit=200` |
| `wait_for_log` | Wait over live post-cursor logs until expected patterns pass, a rejected pattern appears, or timeout expires. Reject-only absence checks require `mode=assert_absent`. | `cursor*` `pattern?` `expect_log?` `expect_logs?` `reject_log?` `reject_logs?` `mode=expect/assert_absent` `category?` `verbosity=very_verbose` `timeout_ms=3000` `poll_interval_ms=100` `log_limit=200` |
| `execute_and_expect` `[w]` | Execute one command and evaluate expected/rejected post-command log patterns. | `command*` `dry_run=false` `require_known_object=false` `target_world=auto/pie/editor` `expect_log?` `expect_logs?` `reject_log?` `reject_logs?` `settle_ms=100` `log_limit=200` |
| `run_sequence` `[w]` | Run a bounded sequence of command strings or step objects with per-step log expectations, optional step capture, and optional artifact files. Step objects support `reject_log`, `log_limit`, and `capture_*` fields. | `commands*` `dry_run=false` `require_known_object=false` `target_world=auto/pie/editor` `abort_on_failure=true` `settle_ms=100` `log_limit=200` `artifact_dir?` |
| `execute_and_capture` `[w]` | Execute a command, run a screenshot console command such as `HighResShot`, and report the newly created or modified PNG path. Deferred viewport screenshots return `capture_pending` plus `capture_id`. | `command*` `capture_command="HighResShot 1920x1080"` `output_path?` `target_world=auto/pie/editor` `settle_ms=250` `capture_wait_ms=120000` |
| `poll_capture` | Poll a pending command-driven screenshot by `capture_id`; use after `execute_and_capture` or a capture step returns `capture_pending`. | `capture_id*` `consume=false` |
| `diagnose_failure` | Classify a failed console result and suggest concrete next actions. | `result*` |
| `set_cvar_scoped` `[w]` | Temporarily set live CVars, run a sequence, then restore original values before returning. | `cvars*` `commands*` `target_world=auto/pie/editor` `artifact_dir?` |

## Workflows

### Find a command or CVar

1. `console_query("search_objects", { query, object_type: "all", limit: 20 })`.
2. If results are stale or empty, run `console_query("refresh_snapshot", { object_type: "all" })` and repeat the search.
3. Use `console_query("get_object", { name })` for exact flags, help text, current value, default value, and variable type.

### Execute a command safely

1. `console_query("resolve_command", { command, target_world: "pie" })` to verify the first token and route when the editor is live.
2. If the editor is unavailable, use `console_query("get_object", { name })` or `console_query("search_objects", { query })` against the latest snapshot.
3. `console_query("execute", { command, dry_run: true, require_known_object: true, target_world: "pie" })` to confirm parse/existence and a live PIE/game world without side effects when gameplay state must be affected.
4. Re-run with `dry_run: false` only when the command is intended to affect editor/runtime state.

### Execute and verify logs

Use `execute_and_expect` for one command and `run_sequence` for smoke workflows. Both actions capture a monotonic log cursor before dispatch, then evaluate only post-command logs. Treat `run_sequence.steps[]` as the durable evidence bundle: each step carries the command, cursor window, execution report, expectation report, returned logs, optional capture data, and optional artifact file paths.

```
console_query("execute_and_expect", {
  "command": "Project.Debug.JumpToCheckpoint",
  "target_world": "pie",
  "require_known_object": true,
  "expect_log": "JumpToCheckpoint"
})

console_query("run_sequence", {
  "target_world": "pie",
  "require_known_object": true,
  "artifact_dir": "Saved/Monolith/ConsoleRuns/ProjectGameplaySmoke",
  "commands": [
    "Project.Run.NewWithSeed 424242",
    {"command": "Project.Debug.SelectScenario 2", "expect_log": "SelectScenario(2) -> ok"},
    {"command": "Project.Debug.JumpToCheckpoint", "expect_log": "checkpoint", "capture": true}
  ]
})
```

Use `wait_for_log` when a command starts async work and the expected log may arrive after the normal settle window:

```
cursor = console_query("get_log_cursor", {})
console_query("execute", {"command": "Project.Debug.DumpState", "target_world": "pie", "dry_run": false})
console_query("wait_for_log", {"cursor": cursor.cursor, "pattern": "Project debug state", "timeout_ms": 3000})
```

For absence-only checks, pass `mode: "assert_absent"` explicitly so the result cannot be mistaken for an async completion wait:

```
console_query("wait_for_log", {
  "cursor": cursor.cursor,
  "mode": "assert_absent",
  "reject_log": "Ensure condition failed",
  "timeout_ms": 3000
})
```

### Capture after a command

Use `execute_and_capture` when a runtime proof image is useful. It executes the command, runs the screenshot console command, then reports the new or modified PNG written under `Saved/Screenshots` without falling back to editor viewport capture. If the result is `status="capture_pending"`, wait briefly outside the MCP call and poll `console_query("poll_capture", {"capture_id": "...", "consume": true})` until it returns `completed=true`. The default watcher window is 120000 ms; pass a larger `capture_wait_ms` for especially expensive captures, up to the 240000 ms live clamp.

### Scoped CVar Test

Use `set_cvar_scoped` for temporary CVar changes during verification. The action preflights every CVar before mutating, restores every successfully changed CVar value and original set-by priority before returning, and reports restore failures as `passed=false`.

```
console_query("set_cvar_scoped", {
  "cvars": {"r.ScreenPercentage": 50},
  "commands": [
    {"command": "Project.Debug.ShowState", "expect_log": "Project.Debug.ShowState", "capture": true}
  ],
  "target_world": "pie",
  "artifact_dir": "Saved/Monolith/ConsoleRuns/ProjectLowRes"
})
```

### Failure Diagnosis

When `execute_and_expect`, `wait_for_log`, `run_sequence`, `execute_and_capture`, or `poll_capture` returns `passed=false`, feed the returned object to `diagnose_failure` before retrying. It distinguishes unknown command, missing PIE world, log expectation miss, rejected log, timeout, pending capture, capture failure, and artifact write failure.

## Notes

- Console registry snapshots are runtime state, not asset content. They are stored in `EngineSource.db` so source/runtime metadata and offline CLI lookup share one database.
- `ConsoleObject` is broader than `ConsoleCommand`: every registered variable and command is an `IConsoleObject`, so object-level indexing avoids losing `IConsoleVariable` help text, flags, current values, and command-only entries.
- Offline `monolith_query.exe console refresh_snapshot`, `resolve_command`, `execute`, `get_log_cursor`, `search_logs_since`, `wait_for_log`, `execute_and_expect`, `run_sequence`, `execute_and_capture`, `poll_capture`, `diagnose_failure`, and `set_cvar_scoped` are live-only guidance actions; they cannot resolve the live registry, inspect live logs, capture or poll pending capture state, diagnose live results, set CVars, or run commands without the editor process.
- Use `target_world: "pie"` for gameplay commands such as `Project.Debug.*`; it fails instead of falling back to the editor world when no PIE/game world exists.
