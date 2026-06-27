# SPEC_MonolithConsole

**Parent:** [SPEC_CORE.md](../SPEC_CORE.md)
**Engine:** Unreal Engine 5.7+
**Version:** 0.1

---

## MonolithConsole

**Dependencies:** Core, CoreUObject, Engine, MonolithCore, MonolithEditor, MonolithSource, EditorSubsystem, UnrealEd, Slate, SlateCore, Json, JsonUtilities

MonolithConsole exposes Unreal's process-local `IConsoleManager` registry through the `console` namespace. It treats registered entries as `IConsoleObject` instances, then classifies them as variables, commands, or generic objects. This preserves the full surface: `IConsoleVariable` values and flags, `IConsoleCommand` entries, and common help text. The implemented live registry currently has 16 actions. Runtime-only actions return explicit `live_only` guidance in offline `monolith_query.exe` instead of unknown-action failures.

### Classes

| Class | Responsibility |
|-------|----------------|
| `FMonolithConsoleModule` | Registers and unregisters the `console` namespace when `UMonolithSettings::bEnableConsole` is true. |
| `FMonolithConsoleActions` | Owns live registry enumeration, snapshot refresh, snapshot search/get/health, command resolution, guarded command execution, command-plus-log expectations, bounded log waits, sequence smoke runs, command-driven screenshot capture and pending-capture polling, failure diagnosis, and scoped CVar mutation/restore. Screenshot waits that need a later viewport tick return `capture_pending` and are completed by a background file watcher so the game thread can resume ticking. |
| `FMonolithLogCapture` | Provided by `MonolithEditor`; supplies the monotonic log sequence cursor used by post-command log verification. |
| `FMonolithSourceDatabase` | Owns the persisted console snapshot schema and SQL helpers in `EngineSource.db`. `MonolithConsole` uses this public DB surface rather than opening SQLite independently. |

### Actions

| Action | Params | Description |
|--------|--------|-------------|
| `list_live_objects` | `query`, `mode`, `object_type`, `limit` | Enumerate currently registered console objects directly from `IConsoleManager::Get().ForEachConsoleObjectThatStartsWith/Contains`. Runs on the game thread when called from another thread. |
| `refresh_snapshot` | `query`, `mode`, `object_type`, `limit` | Capture the live registry into `EngineSource.db` tables `console_objects`, `console_objects_fts`, and `console_snapshot_meta`. The snapshot is replace-all and records `captured_at` plus the source label. |
| `search_objects` | `query`, `object_type`, `limit` | Search the captured snapshot through FTS5 with LIKE fallback. Returns object type, flags, help, values, and snapshot timestamp. |
| `get_object` | `name` | Read one captured console object by exact name. |
| `health` | `include_counts` | Report schema availability, object count, FTS parity, and snapshot metadata. |
| `execute` | `command`, `dry_run`, `require_known_object`, `target_world` | Execute a console command through the editor/PIE world context. Defaults to dry-run. `require_known_object=true` checks the command root against the live registry before execution. `target_world=auto` preserves the PIE-first/editor-fallback route, `target_world=pie` requires a live PIE/game world, and `target_world=editor` forces the editor world. |
| `resolve_command` | `command`, `target_world`, `include_values`, `include_defaults` | Resolve the first token of a command line against the live `IConsoleManager` registry and report object metadata plus target-world availability without executing. |
| `get_log_cursor` | none | Return the current live log sequence cursor from `FMonolithLogCapture`. The cursor is monotonic within the editor session and is used to isolate logs emitted after a command. |
| `search_logs_since` | `cursor`, `pattern`, `category`, `verbosity`, `limit` | Return only log entries with sequence greater than `cursor`, with optional substring/category/verbosity filters. |
| `execute_and_expect` | `command`, `dry_run`, `require_known_object`, `target_world`, `expect_log`, `expect_logs`, `reject_log`, `reject_logs`, `settle_ms`, `log_limit` | Execute one command via the same route as `execute`, collect only post-command logs, and return `passed` plus per-pattern match reports. Expectation failures are result data (`passed=false`), not transport errors. |
| `wait_for_log` | `cursor`, `pattern`, `expect_log`, `expect_logs`, `reject_log`, `reject_logs`, `mode`, `category`, `verbosity`, `timeout_ms`, `poll_interval_ms`, `log_limit` | Poll live post-cursor logs until expected patterns pass, rejected patterns appear, or timeout expires. Reject-only absence checks require `mode=assert_absent`; otherwise a caller could confuse "no bad log appeared yet" with async completion. |
| `run_sequence` | `commands`, `dry_run`, `require_known_object`, `target_world`, `abort_on_failure`, `settle_ms`, `log_limit`, `artifact_dir` | Execute up to 100 command strings or step objects. Step objects support `command`, `dry_run`, `require_known_object`, `target_world`, `expect_log`, `expect_logs`, `reject_log`, `reject_logs`, `settle_ms`, `log_limit`, `capture`, `capture_command`, `capture_wait_ms`, and `capture_output_path`. Each step gets its own pre-command cursor, execution report, post-step logs, expectation report, and optional step-level screenshot capture. The response is the artifact/evidence bundle for the sequence: `steps[].cursor`, `steps[].next_cursor`, `steps[].execution`, `steps[].expectations`, `steps[].logs`, optional `steps[].capture`, and optional `artifact` file paths must be explicit data, not prose-only claims. |
| `execute_and_capture` | `command`, `capture_command`, `output_path`, `require_known_object`, `target_world`, `settle_ms`, `capture_wait_ms` | Execute one command, then run a screenshot console command such as `HighResShot 1920x1080`. The action reports the newly created or modified PNG under `Saved/Screenshots` and optionally copies it to `output_path`; it does not fall back to editor viewport capture. If the screenshot writer needs a later game tick, the action returns `status="capture_pending"`, `capture_id`, and `capture_poll_action="console.poll_capture"` instead of blocking the game thread. |
| `poll_capture` | `capture_id`, `consume` | Poll a pending `execute_and_capture` or step-level `run_sequence` capture. Completed results return `captured`, `capture_not_found`, or `copy_failed`, plus `capture_path`, optional `output_path`, wait timing, and `passed`. `consume=true` removes completed records after reading. |
| `diagnose_failure` | `result` | Classify failed console result payloads into concrete causes such as unknown command, missing PIE world, log expectation failure, rejected log match, timeout, pending capture, capture failure, or artifact write failure, with next-action hints. |
| `set_cvar_scoped` | `cvars`, `commands`, `dry_run`, `require_known_object`, `target_world`, `abort_on_failure`, `settle_ms`, `log_limit`, `artifact_dir` | Preflight every requested live console variable, temporarily set values, run a command sequence, then restore original values and original set-by priority even when the sequence fails. |

### DB Surface

Console snapshots live in `Plugins/Monolith/Saved/EngineSource.db`, not `ProjectIndex.db`, because console objects are runtime/source metadata rather than assets.

| Table | Purpose |
|-------|---------|
| `console_objects` | One row per captured object. Primary key is `name`; fields include `object_type`, `help`, `flags`, enable/deprecation booleans, variable value/default/type/set-by, source, and capture time. |
| `console_objects_fts` | FTS5 index over name, type, help, value, default value, variable type, and set-by metadata. |
| `console_snapshot_meta` | Single-row capture metadata: capture time, source label, object count, and console schema version. |

`MonolithSourceSchema.h` is the schema source of truth. `FMonolithSourceDatabase::EnsureConsoleObjectSchema`, `ReplaceConsoleObjectSnapshot`, `SearchConsoleObjects`, `GetConsoleObject`, and `ComputeConsoleHealth` are the only supported write/read helpers for this surface.

### Live vs Offline

Live MCP:

- `list_live_objects`, `refresh_snapshot`, `resolve_command`, `execute`, `get_log_cursor`, `search_logs_since`, `wait_for_log`, `execute_and_expect`, `run_sequence`, `execute_and_capture`, `poll_capture`, `diagnose_failure`, and `set_cvar_scoped` require the editor process because the registry, live world, log ring buffer, screenshot writer, pending-capture state, and CVar state exist only in process memory.
- `search_objects`, `get_object`, and `health` read the persisted snapshot and work after a previous refresh.

Offline CLI:

- `Binaries\monolith_query.exe console search_objects <query>` reads `Saved\EngineSource.db`.
- `Binaries\monolith_query.exe console get_object <name>` reads one captured row.
- `Binaries\monolith_query.exe console health --include-counts=true` reports snapshot/schema health.
- `refresh_snapshot`, `resolve_command`, `execute`, `get_log_cursor`, `search_logs_since`, `wait_for_log`, `execute_and_expect`, `run_sequence`, `execute_and_capture`, `poll_capture`, `diagnose_failure`, and `set_cvar_scoped` return live-only guidance offline.

### Invariants

- The registry is collected as `IConsoleObject`, not only `IConsoleCommand`, because variables and commands share the same console registry and both carry searchable help/flags.
- Snapshot refresh is bounded by caller filters and limits; `limit=0` means no explicit action cap for a full snapshot.
- Offline search must not invent live state. It reports missing schema or missing snapshot as a structured health/search condition with live refresh guidance.
- Command execution is not a search action. It remains explicit, mutation-risk-bearing, and dry-run by default.
- Gameplay command verification must pass `target_world=pie` so missing PIE state is reported as an action error instead of silently falling back to the editor world.
- Log expectations are scoped by `FMonolithLogCapture` sequence, not by substring-only global search, so repeated smoke runs do not match stale logs from earlier commands.
- `run_sequence` evidence must stay structured. Per-step cursors, execution payloads, expectation reports, returned logs, optional captures, and optional artifact files are the accepted evidence; callers should not rely on human-readable summaries to prove a command sequence passed.
- `execute_and_capture` only accepts a new or modified screenshot file produced by the console capture command. It compares path, timestamp, and file size before/after the capture command so reused screenshot names are still detected. It does not substitute `editor.capture_level_viewport`, because editor viewport capture can miss PIE UI overlays. When called on the game thread and the file is not available immediately, it must return `capture_pending` and let `poll_capture` finish the file/copy check after normal viewport ticks resume; it must not block the game thread waiting for a future `HighResShot` frame.
- `set_cvar_scoped` must validate every requested CVar before mutating any value. It must restore every successfully set CVar before returning, including the original `ECVF_SetByMask` priority. Restore failures are explicit result data and make `passed=false`.

### Verification

- Source DB test coverage: `Monolith.IndexGuard.Source.ConsoleSnapshot.SearchAndHealth`.
- Offline CLI coverage: `Tools\MonolithQuery\build.bat`, `Scripts\check_offline_exe_fresh.py`, and `monolith_query.exe console --help/search_objects/get_object/health` against a temp DB or captured `EngineSource.db`.
- Console action coverage: `Monolith.Console.Actions.ExecuteValidation` and `Monolith.Console.Actions.SequenceValidation`.
- CLI live-only coverage: `monolith_query.exe --catalog-self-check`, `monolith_query.exe console resolve_command ...`, `monolith_query.exe console wait_for_log ...`, `monolith_query.exe console diagnose_failure`, `monolith_query.exe console set_cvar_scoped`, and `monolith_query.exe console run_sequence --help` prove runtime-only entries are listed and return explicit guidance instead of unknown-action errors.
- Static CI: `python Scripts/ci_static_checks.py --config .github/monolith-static-ci.json --github check`.
- UE compile: full project/plugin build after changing `MonolithConsole` C++ or `MonolithSourceDatabase` headers.
- PIE command-routing verification: [../../../Docs/testing/2026-06-27-monolith-console-nodemap-runtime.md](../../../Docs/testing/2026-06-27-monolith-console-nodemap-runtime.md).
