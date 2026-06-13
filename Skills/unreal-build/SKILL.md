---
name: unreal-build
description: Use when building, compiling, recompiling, or fixing first-line build errors in Unreal Engine projects via Monolith MCP - decides Live Coding vs UBT from what changed and drives the editor namespace (get_build_status, trigger_build, get_build_errors). unreal-build owns first-line compile/build-error fixing; for post-build log/crash/stack-trace forensics use unreal-debugging, for C++ API/signature/include correctness before building use unreal-cpp, for post-build profiling use unreal-performance. Triggers on build, compile, rebuild, recompile module, hot reload, Live Coding, UBT, UnrealBuildTool, link error, linker error, build target, build failed, build error, compile error, incremental build.
---

# Unreal Build — Smart Build Decision Guide

Decides Live Coding vs full UBT from what changed, then drives the build through Monolith's **editor** namespace (`get_build_status`, `trigger_build`, `get_build_errors`) or a direct UBT command line when the editor is closed.

## Discover first

This skill drives the live **editor** namespace via direct `editor_query` calls. Confirm action names and schemas against the live catalog before calling — never trust a memorized name:

```text
monolith_discover('editor')                       # list editor-namespace actions
describe_query('action_schema', namespace='editor', action='trigger_build')
describe_query('action_schema', namespace='editor', action='get_build_errors')
```

## Build action reference (editor namespace)

Param notation: `name*` required, `name?` optional, `name=val` default, `a/b/c` allowed values, `[w]` mutates state (transaction-wrapped). Signatures are a snapshot of the live catalog — for the exact, full, current schema of any action call `monolith_discover` with mode `schema` (`describe_query('action_schema', namespace='editor', action='<name>')`).

| Action | Purpose | Params (req* opt? =default) |
|---|---|---|
| `get_build_status` | Read compile state: compiling, last_result, last_compile_time, errors_since_compile, patch_applied | *(none)* |
| `get_build_summary` | Summary of last build (errors, warnings, time) | *(none)* |
| `get_compile_output` | Structured compile report: result, time, compile-category log lines, error/warning counts, patch status | *(none)* |
| `get_live_coding_diagnostics` | Normalized Live Coding availability, compile result, diagnostic freshness, bounded log excerpts | `max_log_entries?` (default 50, max 200) |
| `[w] trigger_build` | Trigger a Live Coding compile | `wait?=false` (block until compile finishes) |
| `[w] live_compile` | Alias for `trigger_build` | `wait?=false` |
| `get_build_errors` | Errors/warnings in a time window, bucketed compile_errors vs other_errors. Window precedence: `since_marker` > `since_iso` > `since_seconds`/`since` > last compile | `since_marker? since_iso? since_seconds? since? clear_baseline?=false category? compile_only?=false exclude_categories?` |
| `search_build_output` | Search build log output by pattern | `pattern* limit?=100` (max 1000) |

Notes on `get_build_errors` params: `since_marker` reports only errors after the latest log line containing that token (highest precedence); `since_iso` is an absolute ISO-8601 cutoff (e.g. `2026-06-06T12:00:00Z`); `since_seconds` (legacy alias `since`) is a relative last-N-seconds window; `clear_baseline=true` stamps a fresh "ignore prior noise" baseline and returns immediately; `category` narrows the query to one log category; `compile_only=true` narrows to compile categories (`LogLiveCoding`/`LogCompile`/`LogLinker`); `exclude_categories` is an array kept out of the headline `error_count` (default `[LogPython, LogMonolith]`, still returned).

## When to use this skill

Use **unreal-build** when you are compiling/recompiling and fixing the first wave of build errors at build time.

**Use a different skill for:**

- **unreal-debugging** — the build (or runtime) already failed and you need log searching, crash context, or stack-trace forensics. The boundary: unreal-build owns getting the compile to run and fixing first-line compile/link errors; unreal-debugging owns log/crash/stack-trace investigation once a build has failed or the editor crashes.
- **unreal-cpp** — the issue is C++ API/signature/include/`Build.cs` correctness *before* building (engine API lookup, header/include paths, class hierarchies).
- **unreal-performance** — the build succeeds but you need profiling, shader/draw-call stats, or optimization.

## Step 1: Check What Changed

Analyze the files you modified. Classify each change:

| Change Type | Build Method |
|---|---|
| `.cpp` body changes only | Live Coding |
| `.h` modified (members, layout, macros) | UBT (editor must close) |
| `.h` added | UBT (editor must close) |
| `.cpp` added | UBT (editor must close) |
| `.cpp` deleted | UBT (editor must close) |
| `.Build.cs` changed | UBT (editor must close) |
| `.uplugin` changed | UBT (editor must close) |

**Rule: If ANY file requires UBT, the whole build requires UBT.**

## Step 2: Check Editor Status

Try calling Monolith MCP: `editor_query({action: 'get_build_status'})` or `monolith_status()`.

- **MCP responds** → Editor is running
- **MCP fails/timeout** → Editor is closed

## Step 3: Execute Build

### Live Coding Path (editor open + .cpp-only changes)

1. Call `editor_query({ action: "trigger_build" })` via MCP
2. Wait ~10 seconds for compilation
3. Call `editor_query({ action: "get_compile_output" })` to check result
4. If errors: call `editor_query({ action: "get_build_errors", params: { compile_only: true } })`

### UBT Path (editor closed OR header/new-file/Build.cs changes)

**If editor is open and UBT is needed:**
> Tell the user: "Header/structural changes detected — Live Coding can't handle these. Please close the editor so I can run a full UBT build, then reopen after."
>
> Do NOT attempt UBT while editor is running. You will get: `"Unable to build while Live Coding is active"`

**When editor is confirmed closed, run the project's primary editor build.** Resolve the engine root from `GO.uproject` `EngineAssociation` — never hard-code a local engine path. Build the `GoGameEditor` target:

```powershell
$projectRoot = (Get-Location).Path
$uproject = Join-Path $projectRoot "GO.uproject"
$resolver = Join-Path $projectRoot "BatchFiles\Script\ResolveUnrealEngine.ps1"
$engineRoot = powershell -NoProfile -ExecutionPolicy Bypass -File $resolver -Project $uproject -Output Root
& "$engineRoot\Engine\Binaries\DotNET\UnrealBuildTool\UnrealBuildTool.exe" GoGameEditor Win64 Development -Project=$uproject -WaitMutex -NoHotReloadFromIDE
```

> **IMPORTANT:** Do NOT hard-code an engine path such as `UE_5.7\Engine\...`; the resolver derives it from `EngineAssociation` so the build follows the project's pinned engine. Do NOT use `Build.bat`.

Check the exit code: `0` = success, non-zero = failure. On failure, scan the output for `error` lines; for deeper log/crash forensics hand off to **unreal-debugging**.

## Decision Matrix (Quick Reference)

| Editor | Changes | Action |
|--------|---------|--------|
| Open | .cpp only | `editor_query("trigger_build")` via MCP |
| Open | .h / new files / Build.cs | Ask user to close editor → UBT |
| Open | .uplugin | Ask user to close editor → UBT |
| Closed | Any | Run UBT directly |

## Live Coding Gotchas

- **Header changes** (new members, class layout, UCLASS/USTRUCT) → requires editor restart + full UBT build
- **New .cpp files** are NOT picked up by Live Coding — UBT required
- **Deleted files** are NOT handled by Live Coding — UBT required
- After triggering Live Coding, **wait ~10s** before checking compile result
- `"Unable to build while Live Coding is active"` → use `editor_query("trigger_build")` instead of UBT, or close editor first
- When in doubt, close editor and use UBT — it always works
- After a successful build, EngineSource.db refreshes through incremental source indexing via the post-build reload hook; if the hook did not fire, route reindex through **monolith-mcp**.
