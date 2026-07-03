---
name: unreal-build
description: Use when building, compiling, recompiling, fixing first-line build errors, planning UAT BuildCookRun, or packaging build/screenshot evidence via Monolith MCP - decides Live Coding vs UBT from what changed and drives the editor namespace plus build/artifact/notify operational namespaces. unreal-build owns first-line compile/build-error fixing and guarded build-output evidence workflows; for post-build log/crash/stack-trace forensics use unreal-debugging, for C++ API/signature/include correctness before building use unreal-cpp, for post-build profiling use unreal-performance. Triggers on build, compile, rebuild, recompile module, hot reload, Live Coding, UBT, UnrealBuildTool, BuildCookRun, package build, build artifact, Discord screenshot evidence, link error, linker error, build target, build failed, build error, compile error, incremental build.
---

# Unreal Build — Smart Build Decision Guide

Decides Live Coding vs full UBT from what changed, then drives the build through Monolith's **editor** namespace (`get_build_status`, `trigger_build`, `get_build_errors`) or a direct UBT command line when the editor is closed. It also owns the guarded operational namespaces **`build`**, **`artifact`**, and **`notify`** for BuildCookRun command construction, build-output manifests, screenshot evidence mirroring, and Discord evidence payloads.

## Discover first

This skill drives the live **editor** namespace via direct `editor_query` calls and the operational **build/artifact/notify** namespaces via their namespace query tools. Confirm action names and schemas against the live catalog before calling — never trust a memorized name:

```text
monolith_discover('editor')                       # list editor-namespace actions
monolith_discover('build')                        # BuildCookRun planning/actions
monolith_discover('artifact')                     # build output + screenshot evidence artifacts
monolith_discover('notify')                       # evidence notification payloads
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

## BuildCookRun / Artifact / Evidence actions

These actions are guarded operational helpers. They do not replace the repo's primary verified build command; use them to construct reproducible UAT and evidence workflows without hard-coded engine checkouts or secret-bearing params.

| Namespace.Action | Purpose | Safety contract |
|---|---|---|
| `build.resolve_unreal_engine` | Report the loaded project's engine root plus `RunUAT`/`UBT` paths from the current editor context. | Read-only. |
| `build.run_buildcookrun` | Build a structured UAT `BuildCookRun` command, including optional `-CustomConfig`. | `dry_run=true` default; external UAT launch requires `dry_run=false` and `confirm=true`. |
| `artifact.package_build_outputs` | Scan an archive/build output directory and produce a JSON manifest; optionally write `manifest.json`. | Manifest write requires `dry_run=false`, `write_manifest=true`, and `confirm=true`. |
| `artifact.mirror_screenshot_evidence` | Copy explicit screenshot files or scanned evidence images into an evidence directory. | File copy requires `dry_run=false` and `confirm=true`. |
| `notify.discord_screenshot_evidence` | Build a redacted text-only Discord screenshot evidence payload; webhook is read only from `webhook_env`. | Send requires `send=true`, `dry_run=false`, `confirm=true`, and an environment variable; webhook URLs are never action params. |

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

When the editor is running, inspect Live Coding before attempting any compile. Use `editor_query("get_live_coding_diagnostics")` or `editor_query("get_build_status")` first, then choose:

- `.cpp` body-only change → `editor_query("trigger_build", { "wait": true })` / `editor_query("live_compile", { "wait": true })`, followed by `editor_query("get_compile_output")`.
- Header/API/module descriptor/new-or-deleted source change → close/restart editor and run full UBT; Live Coding cannot make new generated symbols reliable.
- Endpoint down but no editor-server process remains → run `Scripts\watch_mcp.ps1 -Once` or the project's UBT command, then recover the MCP endpoint.

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

**When editor is confirmed closed, run the project's primary editor build.** Resolve the engine root from the host `.uproject` `EngineAssociation` - never hard-code a local engine path. Build the `<Project>Editor` target derived from that `.uproject`:

```powershell
$projectRoot = (Get-Location).Path
$uproject = Get-ChildItem -LiteralPath $projectRoot -Filter *.uproject | Select-Object -First 1
$targetFile = Get-ChildItem -LiteralPath (Join-Path $projectRoot "Source") -Filter *Editor.Target.cs -Recurse | Select-Object -First 1
$editorTarget = if ($targetFile) {
  [System.IO.Path]::GetFileNameWithoutExtension([System.IO.Path]::GetFileNameWithoutExtension($targetFile.Name))
} else {
  "$([System.IO.Path]::GetFileNameWithoutExtension($uproject.Name))Editor"
}
$resolver = Join-Path $projectRoot "Build\BatchFiles\Script\ResolveUnrealEngine.ps1"
$engineRoot = powershell -NoProfile -ExecutionPolicy Bypass -File $resolver -Project $uproject.FullName -Output Root
& "$engineRoot\Engine\Binaries\DotNET\UnrealBuildTool\UnrealBuildTool.exe" $editorTarget Win64 Development "-Project=$($uproject.FullName)" -WaitMutex -NoHotReloadFromIDE
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
