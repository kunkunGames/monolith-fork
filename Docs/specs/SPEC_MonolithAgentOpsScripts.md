# Monolith — Agent Ops Scripts (MCP Recovery, Watchdog, Index Freshness, Branch Hygiene)

**Parent:** [../SPEC_CORE.md](../SPEC_CORE.md)
**Status:** Implemented and verified 2026-06-11; branch hygiene report added 2026-06-29; MCP watchdog, scheduled index maintenance, interactive Task Scheduler startup guidance, and `monolith_watchdog.exe` wrapper added 2026-07-03; watchdog readable console/JSONL logging updated 2026-07-04
**Scope:** `Scripts/recover_mcp.ps1`, `Scripts/watch_mcp.ps1`, `Scripts/monolith_watchdog.py`, `Scripts/build_monolith_watchdog.ps1`, `Binaries/monolith_watchdog.exe`, `Scripts/check_index_freshness.ps1`, `Scripts/prune_invocation_logs.ps1`, `Scripts/report_stale_branches.ps1`; the offline invocation-log reader contract lives in [SPEC_MonolithToolInvocationLogs.md](SPEC_MonolithToolInvocationLogs.md)
**Created:** 2026-06-11

---

## 1. Purpose

Recurring agent workflows were documented only as prose (CLAUDE.md sections 12 and 14, `Skills/monolith-mcp/SKILL.md`): recovering the editor-backed MCP endpoint, supervising that endpoint during long agent sessions, walking the index health -> repair -> re-verify chain, and inspecting stale branch buildup without taking destructive GitHub actions. These are fragile, order-dependent shell sequences — the case where the Agent Skills guidance prefers a deterministic script over re-derived instructions. These scripts give every agent the same floor: one invocation, line-oriented machine-readable result output, and documented exit codes. `watch_mcp.ps1` uses readable `[pid:<mcp-pid>][yyyy-MM-dd HH:mm:ss][EventType]` console watchdog lines and appends the same structured events to `Logs\<yyyyMMdd>\watchdog.jsonl`; the other scripts retain their documented `RESULT=` terminal lines.

They intentionally do not replace live MCP namespace actions. Agents call task-specific MCP actions through their MCP client, and the runtime catalog stays the only source of truth for action names and schemas. The exception is `watch_mcp.ps1`'s scheduled maintenance pass, which invokes the existing `bridge` namespace actions to keep the asset/source indexes warm while the editor-backed endpoint is already healthy.

The MCP and index scripts are Windows-host helpers (they drive `RunHeadlessEditor.bat` and `Binaries/monolith_query.exe`, which are Windows surfaces in this checkout). The branch-hygiene report uses only the Git CLI and is intentionally non-destructive. The invocation-log reader is cross-platform Python.

## 2. Scripts

| Script | Purpose | Writes anything? |
|---|---|---|
| `Scripts/recover_mcp.ps1` | Probe MCP `/health`; when down, launch the host project's headless editor wrapper with headless-safe editor settings and wait for the endpoint | No repo/DB writes; launches an editor process |
| `Scripts/watch_mcp.ps1` | Long-running MCP availability watchdog; when `/health` is down and the editor process is gone or a headless editor stays unhealthy, run the primary editor UBT build, restart-triggered index maintenance, and recover the endpoint; when healthy, run scheduled asset/source/graph maintenance | Writes UBT/source commandlet logs under `Saved/Monolith/Watchdog/`; launches or restarts a headless editor process; maintenance writes `Saved/ProjectIndex.db`, `Saved/EngineSource.db`, and/or `Saved/graph.db` |
| `Scripts/monolith_watchdog.py` / `Binaries/monolith_watchdog.exe` | Recognizable console wrapper for `watch_mcp.ps1`; accepts an absolute project root argument and stays alive so Task Manager shows `monolith_watchdog.exe` | No repo/DB writes; launches a child PowerShell watchdog process |
| `Scripts/build_monolith_watchdog.ps1` | Build `Binaries/monolith_watchdog.exe` from the Python wrapper through an isolated PyInstaller venv under `Saved/Build/MonolithWatchdog` | Writes build scratch files under `Saved/Build/MonolithWatchdog/` and overwrites `Binaries/monolith_watchdog.exe` |
| `Scripts/check_index_freshness.ps1` | `source`/`project` health -> stale detection -> repair recommendation; `-Execute` runs warning-indicated repairs and re-verifies | `-Execute` writes `Saved/EngineSource.db` / `Saved/ProjectIndex.db` through `monolith_query.exe` |
| `Scripts/prune_invocation_logs.ps1` | Retention pruning for `Logs/yyyyMMdd` folders (age and/or total-size rules); dry-run by default | `-Execute` deletes pruned date folders |
| `Scripts/report_stale_branches.ps1` | Non-destructive report for remote branch cleanup review candidates: merged into base, stale by date, no-op-like, numeric suffix/id token, protected prefix | No; delete commands are printed as suggestions only |
| `Analyzer/analyze_invocation_logs.py` | Offline reader for daily invocation logs (contract: [SPEC_MonolithInvocationLogAnalyzer.md](SPEC_MonolithInvocationLogAnalyzer.md)) | Writes reports under `Saved/Monolith/LogAnalysis/` only |

## 3. `recover_mcp.ps1` Contract

Sequence: GET `<McpUrl with /health>` (3s timeout, 200 = up) -> resolve host checkout root (`-ProjectRoot`, else walk up from the script until a `*.uproject` is found) -> require `Build/BatchFiles/RunHeadlessEditor.bat` -> launch unless a real editor instance already exists -> poll `/health` until 200 or `-TimeoutSec`.

| Parameter | Default | Notes |
|---|---|---|
| `-McpUrl` | `MONOLITH_URL` env var, else `http://localhost:9316/mcp` | Same endpoint resolution as `Scripts/monolith_proxy.py` |
| `-TimeoutSec` | 600 | Editor boot budget after the launch step |
| `-PollIntervalSec` | 5 | `/health` probe interval |
| `-ProbeOnly` | off | Report up/down only; when down, include health failure reason, MCP-port listener count/PIDs, editor/headless candidate PIDs, and next action; never launches |
| `-ForceLaunch` | off | Launch even when editor-server candidate processes exist |
| `-ProjectRoot` | upward search | Explicit host checkout root |

Behavior notes:

- The duplicate-launch guard inspects process command lines: `UnrealEditor.exe` / `UnrealEditor-Cmd.exe` instances running with `-game` or `-server` can never bind the MCP port and are not counted as a booting editor. Process existence comes from `Get-Process` (CIM alone can transiently report a live process as missing); CIM only classifies command lines, and a process whose command line cannot be read stays a candidate.
- `-ProbeOnly` preserves the down-path exit code 2 but no longer collapses all failures into bare `MCP_DOWN`: the result line includes `reason`, optional HTTP `status_code`, sanitized `detail`, `listener_count`, `listener_pids`, `listener_owners`, `editor_candidate_count`, `editor_candidate_pids`, `headless_candidate_count`, `headless_candidate_pids`, and `next_action`. Listener detection uses `Get-NetTCPConnection` with local filtering and falls back to `netstat` if CIM listener enumeration fails.
- The wrapper is started through `Start-Process` with its own hidden console and the wrapper process is awaited with .NET `WaitForExit` (60s bound). Piping the wrapper would block on the stdio handles the backgrounded editor inherits; sharing this script's console group would forward a later Ctrl/kill of the script's tree to the editor as `ConsoleCtrl`; and Windows PowerShell 5.1's `Start-Process -Wait` waits on the whole descendant tree — including the backgrounded editor — which also exposes the editor to the caller's process-tree kill.
- Launches use an isolated `Saved\HeadlessMcp\Config\WindowsEditor\EditorPerProjectUserSettings.ini` plus command-line `-ini:` overrides that force `AssetEditorOpenLocation=NewWindow`, `CleanShutdown=True`, and `RestoreOpenAssetTabsOnRestart=NeverRestore`. This prevents stale asset-editor restore/docked-toolkit state from opening a blocking modal in the headless editor.
- While polling, the loop watches the editor-server candidate process set; two consecutive empty samples (debounced against transient CIM misses) stop the script early with `RESULT=EDITOR_EXITED` (exit 6) and the newest editor log path instead of waiting out the full timeout.
- When the wrapper is missing, the script reports `RESULT=BLOCKED` and stops. It never substitutes a direct `UnrealEditor.exe` launch; `Build\BatchFiles\RunHeadlessEditor.bat` owns engine resolution and launch arguments.
- On timeout it prints the newest `Saved/HeadlessMcp/Logs/HeadlessEditor-*.log` path for inspection.
- MCP client configuration is untouched; the existing Monolith proxy detects the server transition (`/health` poll) and refreshes its tool list. Re-run `monolith_status()` before namespace actions.

| Exit code | Meaning |
|---|---|
| 0 | Endpoint up (already up, or came up after launch) |
| 2 | Down and `-ProbeOnly` was requested |
| 3 | Blocked: host root or `RunHeadlessEditor.bat` not found |
| 4 | Blocked: wrapper exited non-zero |
| 5 | Timeout waiting for `/health` |
| 6 | Editor process exited before `/health` answered (crash/shutdown; the newest editor log path is printed) |

## 4. `watch_mcp.ps1` Contract

Sequence: repeat GET `<McpUrl with /health>` (3s timeout, 200 = up). If up, print `[pid:<mcp-pid>][yyyy-MM-dd HH:mm:ss][McpUp] uptime=<days>D/<hh>:<mm>:<ss>`, run the daily maintenance pass if due, and sleep. If down, resolve the host checkout root. When no editor-server candidate process remains, run the primary editor UBT build derived from the host `.uproject`, run restart-triggered source/graph maintenance that does not require the live editor, then call `recover_mcp.ps1 -ForceLaunch` to relaunch and wait. After `/health` returns, run restart-triggered asset maintenance through the live `bridge` action before the watchdog returns to the normal loop. When an editor-server candidate still exists, do not run UBT first; delegate to `recover_mcp.ps1` without `-ForceLaunch` so the one-shot recovery contract owns wait/timeout diagnostics. If that recovery fails and the remaining candidate is a headless editor (`-NullRHI`, `Saved\HeadlessMcp`, or `HeadlessEditor-*` command line), stop only that unhealthy headless process and run the same build -> pre-restart index -> forced recover -> post-health asset maintenance sequence. Dedicated-server/user editor processes are not stopped by this branch.

| Parameter | Default | Notes |
|---|---|---|
| `-McpUrl` | `MONOLITH_URL` env var, else `http://localhost:9316/mcp` | Same endpoint resolution as `recover_mcp.ps1` |
| `-PollIntervalSec` | 15 | Healthy-state probe interval |
| `-RecoverTimeoutSec` | 600 | Passed to `recover_mcp.ps1 -TimeoutSec`; also bounds how long a live-but-unhealthy headless editor is tolerated before the watchdog restarts it |
| `-RecoverPollIntervalSec` | 5 | Passed to `recover_mcp.ps1 -PollIntervalSec` |
| `-NoBuildBeforeRestart` | off | Skips the UBT step; for launch diagnostics only, not normal agent operation |
| `-ProbeOnly` | off | Report one health/process sample and exit; never builds, recovers, launches, or runs index maintenance |
| `-MaxRestartAttempts` | 0 | 0 means unlimited; otherwise exits with `[pid:<mcp-pid>][yyyy-MM-dd HH:mm:ss][RestartLimit] attempts=<n>` after the bound is reached |
| `-Once` | off | Run one probe/recover cycle and exit, suitable for smoke checks |
| `-DisableDailyReindex` | off | Disable scheduled asset/source/graph maintenance |
| `-DailyReindexTime` | `05:00` | HH:mm start time interpreted in `-DailyReindexTimeZone` |
| `-DailyReindexTimeZone` | `Korea Standard Time` | Windows time-zone id; default is KST |
| `-DailyReindexMode` | `incremental` | `incremental` or `full` |
| `-DailyReindexTargets` | `assets,source,graph` | Any subset of `assets`, `source`, `graph` |
| `-DailyReindexWaitTimeoutSec` | 1800 | Maximum wait for asset/source indexing to become idle before graph maintenance |
| `-DailyReindexWaitPollSec` | 10 | Poll interval for `bridge.get_index_status` while waiting |
| `-DailyReindexActionTimeoutSec` | 120 | HTTP timeout for one MCP action call |
| `-DailyGraphCooldownSeconds` | 1800 | Passed to `source build_crg_graph --execute`; `0` disables the graph cooldown only for diagnostics |
| `-RunDailyReindexNow` | off | Run one maintenance pass as soon as the endpoint is healthy, ignoring the daily schedule; combine with `-Once` for a smoke test |
| `-SkipRestartReindex` | off | Skip restart-triggered maintenance |
| `-RestartReindexMode` | `incremental` | `incremental` or `full` for restart-triggered maintenance |
| `-RestartReindexTargets` | `assets,source,graph` | Any subset of `assets`, `source`, `graph` for restart-triggered maintenance |
| `-ProjectRoot` | upward search | Explicit host checkout root |

Build behavior:

- Watchdog console output starts with `[pid:<mcp-pid>][yyyy-MM-dd HH:mm:ss][EventType]`. `RESULT=` selects the event name and is not repeated as a field; exact `pid=` becomes the prefix; `uptime_seconds=` is rendered as `uptime=<days>D/<hh>:<mm>:<ss>`; `McpUp` omits duplicated version/tool-count fields. Example: `[pid:50844][2026-07-04 00:27:20][McpUp] uptime=0D/00:04:32`.
- Long watchdog payloads are split after the header when the payload is over 240 characters or already contains line breaks. Example:

```text
[pid:45652][2026-07-04 00:27:48][PreRestartReindexGraphDone]
exitCode=0 detail="{ \"after\": { \"edges\": 1125252, \"files\": 89551 }, \"graph_db\": \"D:\\P4\\speed\\Plugins\\Monolith\\Saved\\graph.db\" }"
```

- `BuildFailed` keeps the exit code and prints the UBT log path as a trailing detail instead of `log=<path>`, for example `[pid:45652][2026-07-04 00:27:48][BuildFailed] exitCode=6, D:\P4\speed\Saved\Monolith\Watchdog\UBT-20260704_002020.log`.
- Every watchdog event is also appended as one JSON object per line to `Plugins\Monolith\Logs\<yyyyMMdd>\watchdog.jsonl` with `timestamp`, `displayTimestamp`, `pid`, `event`, `fields`, and `message`.
- The script resolves the engine root through the host checkout's `Build\BatchFiles\Script\ResolveUnrealEngine.ps1` and the host `.uproject` `EngineAssociation`. It does not hard-code engine paths or substitute another checkout.
- The editor target is the first `Source\*Editor.Target.cs` name when present, otherwise `<ProjectName>Editor`.
- The UBT command is `<Target> Win64 Development "-Project=<uproject>" -WaitMutex -NoHotReloadFromIDE`.
- UBT output is written under `Saved\Monolith\Watchdog\UBT-<timestamp>.log`.
- The script never runs UBT while an editor-server candidate process is still alive. For `.cpp` body-only compile checks while the editor is up, agents should use the editor namespace Live Coding flow instead of this watchdog.
- If an editor-server candidate is alive but `/health` still cannot recover before `-RecoverTimeoutSec`, the watchdog checks for headless-only candidates, stops those `-NullRHI`/`Saved\HeadlessMcp` processes, and only then runs the normal restart sequence. Non-headless `-server` or user-facing editor processes are not killed.
- Restart-triggered source maintenance runs before relaunch through `UnrealEditor-Cmd.exe <uproject> -run=MonolithReindex -mode=project -unattended -nopause -nosplash -nullrhi`, writing `Saved\Monolith\Watchdog\MonolithReindex-<timestamp>.log`. `-RestartReindexMode full` maps to `-mode=full`.
- Restart-triggered graph maintenance also runs before relaunch through `Binaries\monolith_query.exe source build_crg_graph --execute --cooldown_seconds=<N>`; full mode adds `--force`.
- Restart-triggered asset maintenance cannot run before a non-commandlet editor exists because `UMonolithIndexSubsystem` intentionally skips DB open in commandlet mode. The watchdog therefore defers only the asset portion, relaunches the headless editor, waits for `/health`, then runs live `bridge.start_indexing(scope="assets", full=<RestartReindexMode>)` and waits through `bridge.get_index_status` before returning to the normal loop.

Daily maintenance behavior:

- The default schedule is once per selected time-zone date at `05:00` KST. If the watchdog starts after the scheduled time, the first healthy probe on that date runs the pass; failed attempts are not retried in a tight loop.
- Asset/source maintenance uses the live Monolith action path first: `bridge.start_indexing({ scope: "all|assets|source", full })`, then `bridge.get_index_status` until the requested indexes are idle or `-DailyReindexWaitTimeoutSec` expires. The JSON-RPC helper reads `structuredContent` first and falls back to JSON `content.text` for older tool responses.
- Graph maintenance uses the local Monolith checkout's `Binaries\monolith_query.exe source build_crg_graph --execute --cooldown_seconds=<N>` after asset/source indexing is idle. `full` mode adds `--force`; incremental mode preserves the cooldown and parity-skip gates so `Saved\graph.db` stays a derived export cache instead of a repeated rebuild sink.
- `-ProbeOnly` never runs maintenance. `-Once -RunDailyReindexNow` runs recovery if needed, then performs one maintenance pass and returns exit 8 on maintenance failure.

Interactive Task Scheduler startup:

- For developer workstations, the persistent watchdog should be registered as a per-user Task Scheduler job, not as a Windows Service. Use an interactive logon trigger so the watchdog inherits the developer user's profile, checkout paths, Perforce/Git environment, and display session. Avoid `LocalSystem` and avoid "Run whether user is logged on or not" when the intended operator model is a visible watchdog window.
- The scheduled action should run `Binaries\monolith_watchdog.exe "<checkout>"`, with the working directory set to the host project root. The wrapper intentionally remains alive while `watch_mcp.ps1` runs, so Task Manager shows a recognizable `monolith_watchdog.exe` process instead of only `powershell.exe`.
- Use `-MultipleInstances IgnoreNew` and `-ExecutionTimeLimit ([TimeSpan]::Zero)` so repeated logon triggers do not spawn duplicate watchdogs and Task Scheduler does not stop the long-running supervisor after the default runtime limit.

Reference registration for the Speed checkout:

```powershell
$taskName = 'Monolith MCP Watchdog - Speed'
$projectRoot = 'D:\P4\speed'
$watchdogExe = Join-Path $projectRoot 'Plugins\Monolith\Binaries\monolith_watchdog.exe'
$user = if ($env:USERDOMAIN) { "$env:USERDOMAIN\$env:USERNAME" } else { $env:USERNAME }

$action = New-ScheduledTaskAction -Execute $watchdogExe -Argument "`"$projectRoot`"" -WorkingDirectory $projectRoot
$trigger = New-ScheduledTaskTrigger -AtLogOn -User $user
$principal = New-ScheduledTaskPrincipal -UserId $user -LogonType Interactive
$settings = New-ScheduledTaskSettingsSet -MultipleInstances IgnoreNew -RestartCount 3 `
    -RestartInterval (New-TimeSpan -Minutes 1) -ExecutionTimeLimit ([TimeSpan]::Zero) `
    -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries

Register-ScheduledTask -TaskName $taskName -Action $action -Trigger $trigger `
    -Principal $principal -Settings $settings `
    -Description 'Keeps the Speed Monolith MCP endpoint supervised in an interactive watchdog PowerShell window.' `
    -Force
Start-ScheduledTask -TaskName $taskName
```

Verification and removal:

```powershell
Get-ScheduledTask -TaskName 'Monolith MCP Watchdog - Speed'
Invoke-RestMethod http://localhost:9316/health
Unregister-ScheduledTask -TaskName 'Monolith MCP Watchdog - Speed' -Confirm:$false
```

| Exit code | Meaning |
|---|---|
| 0 | Endpoint up, or recovery succeeded in `-Once` mode |
| 2 | Down and `-ProbeOnly` was requested |
| 3 | Blocked: host root, `.uproject`, resolver, UBT, or recover script missing |
| 4 | UBT build failed before restart |
| 7 | Restart limit reached |
| 8 | Index maintenance failed in `-Once` mode |
| other | `recover_mcp.ps1` exit code when `-Once` is used and recovery fails |

## 5. `check_index_freshness.ps1` Contract

Sequence per namespace (`-Target source|project|all`, default all): `monolith_query.exe <ns> health --include-counts=true` -> parse `status` / `warnings` -> extract the repair hint each warning itself carries (`... -> repair_crg_cache` format) -> print exact offline and live repair commands. With `-Execute`: run the deduplicated warning-indicated repairs (`repair_crg_cache`, `repair_fts` only) through the offline CLI, then re-run health and print `VERIFY db=<ns> before=<status> after=<status>`.

| Parameter | Default | Notes |
|---|---|---|
| `-Target` | `all` | `source`, `project`, or `all` |
| `-Execute` | off | Run warning-indicated repairs instead of only reporting |
| `-AllowLiveEditor` | off | Permit `-Execute` while the MCP `/health` endpoint answers |
| `-QueryExe` | `<plugin>/Binaries/monolith_query.exe` | Override for copied binaries |
| `-McpUrl` | `MONOLITH_URL` env var, else `http://localhost:9316/mcp` | Used only to detect a live editor |

Behavior notes:

- Only the repair the health warning itself names is ever auto-run. Health problems without a `repair_*` hint (missing DBs, schema errors) are reported and left to indexing/bootstrap paths — the script does not mask missing data with substitutes.
- `-Execute` against a live editor endpoint is refused (`RESULT=REFUSED`, exit 6) unless `-AllowLiveEditor` is passed; the printed live alternative (`<ns>_query("repair_crg_cache", { "execute": true })`) is the preferred path while the editor is up.
- `repair_fts` maps to `project repair_fts --target=all --execute` for project and `source repair_fts --execute` for source.

| Exit code | Meaning |
|---|---|
| 0 | All checked indexes ok (or repaired back to ok) |
| 2 | Warnings or health errors found (report mode, or no runnable repair) |
| 3 | Blocked: `monolith_query.exe` missing |
| 4 | `-Execute` ran but warnings remain |
| 6 | `-Execute` refused: MCP endpoint up without `-AllowLiveEditor` |

## 6. `prune_invocation_logs.ps1` Contract

Retention for the daily JSONL logs stays manual (SPEC_MonolithToolInvocationLogs.md section 11); this script is the manual entry point. It considers only folders named exactly `yyyyMMdd` under the log root — loose files and other folders are never touched — and removes the oldest folders outside the retention rules.

| Parameter | Default | Notes |
|---|---|---|
| `-LogRoot` | `<plugin>/Logs` | Log root containing date folders |
| `-KeepDays` | 30 | Keep folders newer than N days; 0 disables the age rule |
| `-MaxTotalMB` | 0 (off) | After the age rule, prune oldest until total size fits |
| `-Execute` | off | Without it, dry run: `PRUNE ... dry_run` lines + `RESULT=DRY_RUN`, exit 2 |

| Exit code | Meaning |
|---|---|
| 0 | Nothing to prune, or pruning completed |
| 2 | Dry run found prunable folders (re-run with `-Execute`) |
| 3 | Blocked: log root missing |

## 7. `report_stale_branches.ps1` Contract

Remote branch hygiene is a reporting-only workflow. The script reads local remote-tracking refs with Git, classifies branches that may be worth human review, and prints `git push <remote> --delete <branch>` suggestions without executing them. It never fetches, pushes, deletes branches, closes PRs, or calls GitHub APIs.

Default base is `origin/master`. A branch is a cleanup review candidate only when it is not protected and at least one signal applies:

- merged into the base ref (`git branch -r --merged <base>`)
- stale by committer date (`-StaleDays`, default 14)
- no-op-like by branch name, commit subject, or empty `git diff --quiet <base>...<branch>`
- generated-looking numeric suffix or large numeric token

Protected prefixes classify branches that should not receive delete suggestions. Defaults include `master`, `main`, `develop`, `release/`, `hotfix/`, `production`, `staging`, `stable`, and `gh-pages`.

| Parameter | Default | Notes |
|---|---|---|
| `-Remote` | `origin` | Remote-tracking namespace under `refs/remotes/<remote>` |
| `-BaseRef` | `<Remote>/master` | Base ref for merged and no-op diff checks |
| `-StaleDays` | 14 | Age threshold; candidate presence does not affect exit code |
| `-ProtectedPrefixes` | built-in protected names/prefixes | Entries ending in `/` are prefix matches; other entries are exact local branch names |
| `-NoDiffCheck` | off | Skips the per-branch `git diff --quiet <base>...<branch>` content no-op check |
| `-ShowAll` | off | Prints non-candidate `KEEP` lines |
| `-Limit` | 0 | Limits printed `CANDIDATE` lines; summary still counts all candidates |

Confidence labels are review priority, not authorization:

| Confidence | Meaning |
|---|---|
| `high` | branch is merged into base or no-op-like |
| `medium` | branch is stale and has a numeric generated-looking token |
| `low` | branch has only one weaker signal such as stale-only or numeric-only |
| `protected` | branch matches a protected exact name or prefix; no delete suggestion printed |

| Exit code | Meaning |
|---|---|
| 0 | Report completed, regardless of how many candidates were found |
| 1 | Script/runtime error: missing Git, invalid repo/ref, unparseable Git output, or a Git command failure |

## 8. Verification Record (2026-06-11)

| Gate | Result |
|---|---|
| Freshness report mode | Detected the live ProjectIndex CRG parity drift (`project native=10000 crg_nodes=8510`, `valid native edges=260 crg_edges=261`), printed deduplicated offline + live repair commands, exited 2; source reported ok. |
| Freshness execute mode | With the MCP endpoint down, ran `project repair_crg_cache --execute` once (deduplicated from two warnings), re-ran health, reported `VERIFY db=project before=warning after=ok` and `RESULT=REPAIRED`, exited 0. |
| Recovery probe mode | With no MCP server, `-ProbeOnly` reported `RESULT=MCP_DOWN` and exited 2. |
| Recovery guard precision | A running `UnrealEditor.exe <project> -game` instance (which never hosts the editor MCP server) was correctly excluded from the duplicate-launch guard via command-line inspection. |
| Recovery launch + wait | `recover_mcp.ps1` launched `Build/BatchFiles/RunHeadlessEditor.bat` from the resolved host root via detached `Start-Process`, printed the new editor log path, and polled `/health` with 30s `WAITING` heartbeats; `RESULT=MCP_TIMEOUT` (exit 5) and `RESULT=EDITOR_EXITED` (exit 6, detected ~8s after the editor died) were both exercised live. |
| Recovery success path | Verified against a stub `/health` HTTP server: parsed and reported `version`/`tools_registered`/`pid`/`uptime_seconds`, printed the reconnect guidance line, ended with `RESULT=MCP_UP` and exit 0. Headless editor logs from the same session show the real Monolith server binding 9316 at +17s and +7s after wrapper launch, so the live success path is reachable whenever the editor survives boot. |
| Environment blocker (not a script defect) | During verification the Go checkout's headless editor crash-looped ~10s after boot in `FTabManager::SavePersistentLayout` -> `FGenericWindow::GetRestoredDimensions` (`GenericWindow.cpp:113`, engine NullRHI layout-save crash; observed identically across three boots; one earlier boot survived 7 minutes). Until that project/engine issue is fixed, launch-based recovery on that machine state ends in `RESULT=EDITOR_EXITED`. |
| Live-editor refusal path | Not exercised end-to-end in this pass (no pending warnings remained after repair); the gate is enforced by the `RESULT=REFUSED` branch before any repair invocation. |

2026-06-12 follow-up verification:

| Gate | Result |
|---|---|
| Recovery end-to-end with the headless layout-save guard | `recover_mcp.ps1` launched the wrapper; the guarded editor bound MCP at +9s, logged `HeadlessLayoutSaveGuard: CanEverRender=false ...`, and stayed alive past 9 minutes serving actions (pre-guard boots died at 10-20s in the `GenericWindow.cpp:113` layout-save fatal; see SPEC_MonolithEditor.md "Headless layout-save guard"). |
| Recovery success path against the real server | `-ProbeOnly` against the live endpoint reported `version=0.18.1 tools_registered=1722` and ended `RESULT=MCP_UP` with exit 0. A subsequent full launch run (WaitForExit wrapper wait) went launch -> poll -> `RESULT=MCP_UP elapsed_seconds=13` with exit 0 in one invocation, leaving the editor detached from the caller's process tree. |
| Death-detection false positive fixed | A transient empty `Win32_Process` CIM sample had produced `RESULT=EDITOR_EXITED` while the editor was alive and logging; existence now comes from `Get-Process` with a two-sample debounce. |
| Wrapper wait correctness | `Start-Process -Wait` was replaced with .NET `WaitForExit(60s)`: under Windows PowerShell 5.1, `-Wait` waited on the backgrounded editor as a descendant, hanging the script and exposing the editor to the caller's process-tree kill (this, not an engine fault, explained one boot's silent death minutes after launch). |
| Retention dry runs | `prune_invocation_logs.ps1` default (`-KeepDays 30`) kept all 24 date folders and exited 0; `-KeepDays 15` listed 8 prunable folders / 96.25 MB reclaimable, exited 2, and deleted nothing; folder count unchanged afterwards. |

2026-06-29 branch-hygiene verification:

| Gate | Result |
|---|---|
| PowerShell parse | `[System.Management.Automation.Language.Parser]::ParseFile('Scripts\report_stale_branches.ps1', ...)` returned no parse errors. |
| Non-destructive report | `powershell -NoProfile -ExecutionPolicy Bypass -File Scripts\report_stale_branches.ps1 -Limit 5` read `origin` remote-tracking refs, printed five candidate suggestions plus a protected `origin/master` line, and exited 0. |
| Candidate classification sample | The report classified 203 `origin` branches: 201 candidates, one protected, one keep; candidate reasons included `stale_*d`, `no_op_diff`, `no_op_name`, `no_op_subject`, and `numeric_suffix`. |

2026-07-03 watchdog verification:

| Gate | Result |
|---|---|
| PowerShell parse | `watch_mcp.ps1`, `recover_mcp.ps1`, and `check_index_freshness.ps1` parsed with no PowerShell parser errors. |
| Probe-only safety | `watch_mcp.ps1 -ProbeOnly -ProjectRoot D:\P4\speed` reported `RESULT=MCP_DOWN`, `editor_candidate_count=0`, and exited 2 without running UBT, recovery, launch, or index maintenance. |
| Live watchdog restart | Hidden `watch_mcp.ps1 -ProjectRoot D:\P4\speed -MaxRestartAttempts 3 -RecoverTimeoutSec 45` started PID 47796. The log `Plugins\Monolith\Saved\Monolith\Watchdog\watch_mcp-20260703_221538.out.log` shows UBT success, pre-restart `MonolithReindex` success, pre-restart graph refresh success, `RECOVER RESULT=MCP_UP`, `RESULT=RESTART_REINDEX_OK`, and steady `RESULT=MCP_UP` for headless editor PID 31768. |
| Scheduled maintenance | The same watchdog run executed the default due daily pass for `scheduled_date=2026-07-03` at KST, waited for asset/source indexing to become idle, refreshed `Saved\graph.db`, and logged `RESULT=DAILY_REINDEX_OK`. |
| Headless modal mitigation | A previous run opened a headless PID that logged `MODAL_OPEN ... This asset editor has no docked tabs`; `recover_mcp.ps1` now passes `AssetEditorOpenLocation=NewWindow` and the restarted PID 31768 answered `/health` after launch. |
| Health endpoint | `Invoke-RestMethod http://localhost:9316/health` returned `status=ok`, `pid=31768`, `version=0.20.3`, and `tools_registered=1846` after the watchdog run. |
| Analyzer script compile | `python -m py_compile Analyzer\analyze_session_transcripts.py Analyzer\analyze_invocation_logs.py` completed without syntax errors. |
| Text hygiene | The touched script/docs/skill files had no conflict markers or trailing whitespace. |
| Interactive Task Scheduler wrapper | `Binaries\monolith_watchdog.exe D:\P4\speed --dry-run` resolved the child command to Windows PowerShell running `Scripts\watch_mcp.ps1 -ProjectRoot D:\P4\speed`; `-- -ProbeOnly` correctly forwarded `-ProbeOnly` to `watch_mcp.ps1`. |
| `monolith_watchdog.exe` build | `Scripts\build_monolith_watchdog.ps1` created an isolated PyInstaller venv under `Saved\Build\MonolithWatchdog` and built `Binaries\monolith_watchdog.exe`. |
| Task Manager-visible watchdog | The scheduled task action was updated to execute `D:\P4\speed\Plugins\Monolith\Binaries\monolith_watchdog.exe` with argument `"D:\P4\speed"`. Starting the task produced searchable `monolith_watchdog.exe` processes (PyInstaller one-file parent/child) and a child `powershell.exe` running `watch_mcp.ps1 -ProjectRoot D:\P4\speed`. |

2026-07-04 recovery probe diagnostics follow-up:

| Gate | Result |
|---|---|
| PowerShell parse | `[System.Management.Automation.Language.Parser]::ParseFile('Plugins\Monolith\Scripts\recover_mcp.ps1', ...)` returned no parser errors. |
| Probe-only diagnostics | With the local endpoint down, `recover_mcp.ps1 -ProbeOnly` emitted one `RESULT=MCP_DOWN` line with `reason=timeout`, `listener_port=9316`, `listener_count=0`, `editor_candidate_count=0`, `headless_candidate_count=0`, and `next_action=run_watch_mcp_or_recover_mcp`; `$LASTEXITCODE=2`, so the existing down-path contract is unchanged. |

2026-07-04 watchdog log-format verification:

| Gate | Result |
|---|---|
| Watchdog probe after Task Scheduler guidance | `powershell -NoProfile -ExecutionPolicy Bypass -File Plugins\Monolith\Scripts\watch_mcp.ps1 -ProjectRoot D:\P4\speed -ProbeOnly` reported `[pid:50844][2026-07-04 00:27:20][McpUp] uptime=0D/00:04:32` and exited 0. |
| Watchdog readable log format | Formatter smoke tests reported `[pid:45652][2026-07-04 00:27:48][BuildFailed] exitCode=6, D:\P4\speed\Saved\Monolith\Watchdog\UBT-20260704_002020.log` and split a long `PreRestartReindexGraphDone` payload onto the line after its `[pid:45652][2026-07-04 00:27:48][PreRestartReindexGraphDone]` header. |
| Watchdog JSONL logging | The same probe appended one structured JSON object to `Plugins\Monolith\Logs\20260704\watchdog.jsonl` with `event=McpUp`, `pid=50844`, `fields.uptime=0D/00:04:32`, and `message=uptime=0D/00:04:32`. |
