# Monolith — Agent Ops Scripts (MCP Recovery, Watchdog, Index Freshness, Branch Hygiene)

**Parent:** [../SPEC_CORE.md](../SPEC_CORE.md)
**Status:** Implemented and verified 2026-06-11; branch hygiene report added 2026-06-29; MCP watchdog, scheduled index maintenance, interactive Task Scheduler startup guidance, and `monolith_watchdog.exe` wrapper added 2026-07-03; watchdog readable console/JSONL logging updated 2026-07-04; pending CL 1086 adds DLL-lock preflight, consecutive build-failure backoff, project-bound health validation, and availability-first restart maintenance (2026-07-10 verification, 2026-07-11 adversarial hardening); pending CL 1135 adds the measured-latency health-request budget and cross-script forwarding contract (2026-07-12); pending CL 1198 adds durable MCP-host role classification shared with the Core listener startup gate and preserves headless launch overrides through the host wrapper's environment contract (2026-07-18); submitted CL 1309 adds persistent server/index activation awareness and pending CL 1312 changes missing-state defaults to enabled while preserving explicit Stop opt-outs (2026-07-25)
**Scope:** `Scripts/monolith_activation_state.ps1`, `Scripts/mcp_host_role.ps1`, `Scripts/recover_mcp.ps1`, `Scripts/watch_mcp.ps1`, host `Build/BatchFiles/PostBuildSourceIndex.bat`, `Scripts/monolith_watchdog.py`, `Scripts/build_monolith_watchdog.ps1`, `Binaries/monolith_watchdog.exe`, `Scripts/check_index_freshness.ps1`, `Scripts/prune_invocation_logs.ps1`, `Scripts/report_stale_branches.ps1`; the offline invocation-log reader contract lives in [SPEC_MonolithToolInvocationLogs.md](SPEC_MonolithToolInvocationLogs.md)
**Created:** 2026-06-11

---

## 1. Purpose

Recurring agent workflows were documented only as prose (CLAUDE.md sections 12 and 14, `Skills/monolith-mcp/SKILL.md`): recovering the editor-backed MCP endpoint, supervising that endpoint during long agent sessions, walking the index health -> repair -> re-verify chain, and inspecting stale branch buildup without taking destructive GitHub actions. These are fragile, order-dependent shell sequences — the case where the Agent Skills guidance prefers a deterministic script over re-derived instructions. These scripts give every agent the same floor: one invocation, line-oriented machine-readable result output, and documented exit codes. `watch_mcp.ps1` uses readable `[pid:<mcp-pid>][yyyy-MM-dd HH:mm:ss][EventType]` console watchdog lines and appends the same structured events to `Logs\<yyyyMMdd>\watchdog.jsonl`; the other scripts retain their documented `RESULT=` terminal lines.

They intentionally do not replace live MCP namespace actions. Agents call task-specific MCP actions through their MCP client, and the runtime catalog stays the only source of truth for action names and schemas. The exception is `watch_mcp.ps1`'s scheduled maintenance pass, which invokes the existing `bridge` namespace actions to keep the asset/source indexes warm while the editor-backed endpoint is already healthy.

The MCP and index scripts are Windows-host helpers (they drive `RunHeadlessEditor.bat` and `Binaries/monolith_query.exe`, which are Windows surfaces in this checkout). The branch-hygiene report uses only the Git CLI and is intentionally non-destructive. The invocation-log reader is cross-platform Python.

## 2. Scripts

| Script | Purpose | Writes anything? |
|---|---|---|
| `Scripts/monolith_activation_state.ps1` | Shared strict resolver for plugin/project `DefaultMonolith.ini`, generated per-user `Saved/Config/WindowsEditor/Monolith.ini`, and read-only legacy fallback; dot-sourced by recovery/watchdog scripts and callable as a CLI feature probe (`server` or `indexing`) | No; the editor console commands remain the only writers/migrators |
| `Scripts/mcp_host_role.ps1` | Shared pure command-line helpers for exact-project durable-host vs planned-exit automation classification; dot-sourced by recovery and watchdog scripts | No |
| `Scripts/recover_mcp.ps1` | Read durable server activation first; report no-mutation `MCP_DISABLED` while off, otherwise probe MCP `/health` and launch the host project's headless editor wrapper with headless-safe settings when enabled but down | No repo/DB writes; launches an editor process only while server activation is enabled |
| `Scripts/watch_mcp.ps1` | Long-running activation-aware MCP watchdog; perform no endpoint/build/process mutation while server activation is off, apply bounded availability recovery while on, and run restart/scheduled asset/source maintenance only while indexing activation is also on | Writes UBT/source commandlet logs under `Saved/Monolith/Watchdog/`; may launch or restart a headless editor only while server-enabled; maintenance writes `Saved/ProjectIndex.db` and/or `Saved/EngineSource.db` only while indexing-enabled |
| Host `Build/BatchFiles/PostBuildSourceIndex.bat` | Explicit editor-down source maintenance; consults durable indexing activation before creating a request marker or worker, then delegates to the project-mode `MonolithReindex` commandlet whose successful index completion owns the affected CRG projection/cache refresh | Deliberately not wired to `SpeedEditor.Target.cs`: persisted editor catch-up is the automatic owner, and launching a detached commandlet immediately before the editor would race two `EngineSource.db` writers. No work while disabled; explicit callers keep the editor down until `.postbuild.lock` disappears. Never runs a second full `repair_crg_cache` pass or the retired `build_crg_graph` action. |
| `Scripts/monolith_watchdog.py` / `Binaries/monolith_watchdog.exe` | Recognizable console wrapper for `watch_mcp.ps1`; accepts an absolute project root argument and stays alive so Task Manager shows `monolith_watchdog.exe` | No repo/DB writes; launches a child PowerShell watchdog process |
| `Scripts/build_monolith_watchdog.ps1` | Build `Binaries/monolith_watchdog.exe` from the Python wrapper through an isolated PyInstaller venv under `Saved/Build/MonolithWatchdog` | Writes build scratch files under `Saved/Build/MonolithWatchdog/` and overwrites `Binaries/monolith_watchdog.exe` |
| `Scripts/check_index_freshness.ps1` | `source`/`project` health -> stale detection -> repair recommendation; `-Execute` runs validated health-indicated repairs and re-verifies | `-Execute` writes `Saved/EngineSource.db` / `Saved/ProjectIndex.db` through `monolith_query.exe` |
| `Scripts/prune_invocation_logs.ps1` | Retention pruning for `Logs/yyyyMMdd` folders (age and/or total-size rules); dry-run by default | `-Execute` deletes pruned date folders |
| `Scripts/report_stale_branches.ps1` | Non-destructive report for remote branch cleanup review candidates: merged into base, stale by date, no-op-like, numeric suffix/id token, protected prefix | No; delete commands are printed as suggestions only |
| `Analyzer/analyze_invocation_logs.py` | Offline reader for daily invocation logs (contract: [SPEC_MonolithInvocationLogAnalyzer.md](SPEC_MonolithInvocationLogAnalyzer.md)) | Writes reports under `Saved/Monolith/LogAnalysis/` only |

### 2.1 Activation-state reader

`monolith_activation_state.ps1` resolves the same hierarchy as `UMonolithSettings`: built-in true -> `Plugins/Monolith/Config/DefaultMonolith.ini` -> project `Config/DefaultMonolith.ini` for `bServerEnabledByDefault` / `bIndexingEnabledByDefault`, then generated `Saved/Config/WindowsEditor/Monolith.ini` `[Monolith.UserActivation] ServerEnabled/IndexingEnabled`. If a generated key is absent, older `Saved/Monolith/Activation.ini` is a read-only fallback until the editor migrates it. The parser accepts case-insensitive `true`/`false` and numeric `1`/`0`; malformed values fail closed. Dot-sourced callers use `Get-MonolithActivationState -Root <project>`. Direct CLI use selects `-Feature server|indexing`: exit `0` means enabled, exit `2` means disabled or malformed, and exit `3` means the project root cannot be resolved. The script never creates, migrates, or changes config.

## 3. `recover_mcp.ps1` Contract

Sequence: resolve the host checkout root (`-ProjectRoot`, else walk up from the script until a `*.uproject` is found) -> load `monolith_activation_state.ps1` and `mcp_host_role.ps1` -> if server activation is off, return `RESULT=MCP_DISABLED desired_enabled=false mutation=none` with exit `0` before any health probe or launch -> otherwise GET `<McpUrl with /health>` with the independently bounded `-HealthTimeoutSec` -> validate the complete health JSON contract and bind its PID exclusively to the TCP listener PID set and to a durable Unreal Editor process whose command line names that exact `.uproject` -> when a transport timeout leaves one exclusive listener owned by a live, readable, exact-project durable editor, classify `trusted_busy`, skip launch/config mutation even with `-ForceLaunch`, and poll within `-TimeoutSec` -> when an exact-project planned-exit automation editor is active, classify `ephemeral_automation` and wait without mutation for it to exit and the port to clear -> otherwise require the listener port to be provably empty -> require `Build/BatchFiles/RunHeadlessEditor.bat` -> recheck that the port is empty immediately before launch -> launch unless a current-project durable editor-server candidate already exists -> poll until the same validated health/identity contract succeeds or `-TimeoutSec` expires. The Speed host wrapper acquires the project build lock with a one-second timeout and runs the completed-receipt launch preflight; a concurrent build therefore returns wrapper failure without starting an editor, and a later recovery invocation retries from a clean state. HTTP 200 alone is never readiness.

| Parameter | Default | Notes |
|---|---|---|
| `-McpUrl` | `MONOLITH_URL` env var, else `http://localhost:9316/mcp` | Same endpoint resolution as `Scripts/monolith_proxy.py` |
| `-HealthTimeoutSec` | 5 | Absolute timeout for each individual `/health` HTTP request; validated range is 1--60 seconds |
| `-TimeoutSec` | 600 | Editor boot budget after the launch step |
| `-PollIntervalSec` | 5 | `/health` probe interval |
| `-ProbeOnly` | off | Report up/down only; when down, include health failure reason, MCP-port listener count/PIDs, editor/headless candidate PIDs, and next action; never launches |
| `-ForceLaunch` | off | Launch even when editor-server candidate processes exist |
| `-ProjectRoot` | upward search | Explicit host checkout root |

Behavior notes:

- `RESULT=MCP_DISABLED` is intentional desired state, not endpoint failure. `-ProbeOnly`, normal recovery, and `-ForceLaunch` all return without probing, building, editing config, or launching. The next action is the editor-console command `Monolith.StartServer`; scripts never write the durable flag.
- A healthy response must be a JSON object with `status="ok"`, the requested port, a non-empty version, non-negative uptime, positive integer PID/action count, and `mcp_transport.primary_route="/mcp"`. Its PID must be the **only unique owning PID** across all listener rows for that port (multiple IPv4/IPv6 rows from that one PID are allowed) and resolve to `UnrealEditor.exe` / `UnrealEditor-Cmd.exe` with this checkout's exact `.uproject` in the command line. Invalid JSON, partial look-alike JSON, a spoofed/non-owning PID, a multi-owner PID set, another process, and another checkout are not promoted to healthy. Listener ownership uses `Get-NetTCPConnection` with a validated `netstat.exe` fallback and fails closed if neither can establish ownership.
- `-HealthTimeoutSec` applies only to one `/health` GET. It does not widen the editor boot budget, polling interval, wrapper wait, MCP action timeout, or any mutation gate. A transport timeout is not readiness, but it is not evidence that an otherwise exact OS identity became foreign: one exclusive listener PID must still be live, be `UnrealEditor.exe` / `UnrealEditor-Cmd.exe`, expose a readable eligible editor-mode command line for this exact `.uproject`, and match the listener PID. Only that case becomes `MCP_BUSY`; it never launches or mutates. Missing/unreadable identity, wrong executable/project/mode/PID, inconsistent/multi-owner listeners, and invalid/non-200 HTTP stay blocked.
- The duplicate-launch guard inspects process command lines through the shared host-role helper: readable `UnrealEditor.exe` / `UnrealEditor-Cmd.exe` instances count only when they name this checkout's exact `.uproject` and are durable. `-game`, `-server`, `-run=...` commandlets, `-TestExit`, `Automation RunTests`/`RunAll` `ExecCmds`, explicitly MCP-disabled editors, and other projects do not block recovery as durable candidates. Process existence comes from `Get-Process` (CIM alone can transiently report a live process as missing); a process whose command line cannot be read stays a conservative durable candidate but is never accepted as the health PID identity.
- Any occupied port without either a fully accepted health identity or the exact `trusted_busy` OS identity is reported as `RESULT=BLOCKED reason=foreign_or_untrusted_mcp_endpoint` before config writes or process launch. This includes invalid HTTP 200, non-200 HTTP, non-HTTP/raw TCP, multi-owner, inconsistent, and unreadable listener states. `-ProbeOnly` returns `RESULT=MCP_BUSY` (exit 2) for exact trusted-busy, the same blocked result (exit 3) for an occupied/unreadable or mismatched port, and `RESULT=MCP_DOWN` (exit 2) for a provably empty down port.
- On the provably empty down path, `-ProbeOnly` does not collapse failures into bare `MCP_DOWN`: the result line includes `reason`, optional HTTP `status_code`, sanitized `detail`, `listener_count`, `listener_pids`, `listener_owners`, `editor_candidate_count`, `editor_candidate_pids`, `headless_candidate_count`, `headless_candidate_pids`, and `next_action`. Listener detection uses `Get-NetTCPConnection` with local filtering and falls back to `netstat.exe` only when that command resolves and exits successfully.
- The wrapper is started through `Start-Process` with its own hidden console and the wrapper process is awaited with .NET `WaitForExit` (60s bound). Headless INI overrides cross the batch boundary only through the wrapper's documented `UE_EDITOR_EXTRA_ARGS` environment contract; `recover_mcp.ps1` appends to any caller value, launches without `Start-Process -ArgumentList`, and restores the exact prior process-environment value in `finally`. This avoids a second `cmd.exe` parsing boundary that can split quoted paths or reinterpret `:`/`[` tokens. Piping the wrapper would block on the stdio handles the backgrounded editor inherits; sharing this script's console group would forward a later Ctrl/kill of the script's tree to the editor as `ConsoleCtrl`; and Windows PowerShell 5.1's `Start-Process -Wait` waits on the whole descendant tree — including the backgrounded editor — which also exposes the editor to the caller's process-tree kill.
- Launches use an isolated `Saved\HeadlessMcp\Config\WindowsEditor\EditorPerProjectUserSettings.ini` plus command-line `-ini:` overrides that force `AssetEditorOpenLocation=NewWindow`, `CleanShutdown=True`, and `RestoreOpenAssetTabsOnRestart=NeverRestore`. This prevents stale asset-editor restore/docked-toolkit state from opening a blocking modal in the headless editor.
- While polling, the loop watches the editor-server candidate process set; two consecutive empty samples (debounced against transient CIM misses) stop the script early with `RESULT=EDITOR_EXITED` (exit 6) and the newest editor log path instead of waiting out the full timeout.
- When the wrapper is missing, the script reports `RESULT=BLOCKED` and stops. It never substitutes a direct `UnrealEditor.exe` launch; `Build\BatchFiles\RunHeadlessEditor.bat` owns engine resolution and launch arguments.
- On timeout it prints the newest `Saved/HeadlessMcp/Logs/HeadlessEditor-*.log` path for inspection.
- MCP client configuration is untouched; the existing Monolith proxy detects the server transition (`/health` poll) and refreshes its tool list. Re-run `monolith_status()` before namespace actions.

### 3.1 Durable MCP host classification and handoff

The recovery and watchdog scripts dot-source `mcp_host_role.ps1` and apply the same process-role boundary to health-PID acceptance, duplicate-launch candidates, trusted-busy state, and headless restart candidates:

| Editor process role | Durable MCP host? | Recovery/watchdog behavior |
|---|---:|---|
| Normal exact-project editor | yes | May satisfy the exclusive listener/health identity contract and blocks a duplicate durable launch. |
| Exact-project `RunHeadlessEditor.bat` editor (`-NullRHI` / `Saved\HeadlessMcp`) | yes | May satisfy health and is the only role the watchdog may stop after the bounded unhealthy-headless gate. |
| Commandlet (`IsRunningCommandlet()` or `-run=...`) | no | Registry-only/offline work may continue, but the process cannot own the durable listener and is never a recovery candidate. |
| `-TestExit` process | no | Treated as bounded/self-terminating test infrastructure, never as endpoint readiness or a booting durable editor. |
| `-ExecCmds` containing `Automation RunTests ...` or `Automation RunAll ...` | no | Treated as bounded automation even if it names the exact project; it cannot satisfy health identity, block the durable launch as an editor candidate, or enter the headless-stop set. |
| `-game`, `-server`, explicitly MCP-disabled, or foreign-project process | no | Retains the existing exclusion/fail-closed behavior. |

An excluded automation or test process is never killed and never overwritten by recovery. When its exact-project identity and planned-exit role are readable, the unavailable-endpoint classifier returns `ephemeral_automation` instead of the generic foreign-listener state. `-ProbeOnly` reports `RESULT=MCP_EPHEMERAL_AUTOMATION` and exits 2 with `mutation=none`. Full recovery polls within `-TimeoutSec`: a separately validated durable host may win the handoff immediately; otherwise recovery waits until the planned-exit candidates are gone and the port is provably clear, then launches the normal durable headless host. A foreign/ambiguous listener that appears during the wait still fails closed with exit 3, and an automation process that does not exit within the budget returns `RESULT=MCP_TIMEOUT`/exit 5. The watchdog emits `RESULT=EPHEMERAL_AUTOMATION_ACTIVE`, performs no build/recover/launch/stop mutation, and retries on the next normal poll (`-ProbeOnly`/`-Once` exits 2). With the matching `MonolithCore` startup role gate compiled, those bounded roles do not start the listener in the first place, so automation/build windows cannot transiently impersonate endpoint continuity. A separately running durable editor remains authoritative; otherwise the bounded process exits before the wrapper acquires the clear port. This is an ownership handoff, not a fallback listener or alternate editor path.

| Exit code | Meaning |
|---|---|
| 0 | Endpoint up (already up, or came up after launch) |
| 2 | Down, exact-project trusted-busy, or exact-project ephemeral automation and `-ProbeOnly` was requested |
| 3 | Blocked: host root, shared host-role helper, or `RunHeadlessEditor.bat` not found, or an occupied/unreadable MCP listener lacks a fully trusted exclusive project/process identity |
| 4 | Blocked: wrapper exited non-zero |
| 5 | Timeout waiting for `/health`, including an ephemeral automation process that did not exit within `-TimeoutSec` |
| 6 | Editor process exited before `/health` answered (crash/shutdown; the newest editor log path is printed) |

## 4. `watch_mcp.ps1` Contract

Sequence: resolve the expected checkout once, then repeat GET `<McpUrl with /health>` with the independently bounded `-HealthTimeoutSec` and apply the same complete JSON/PID/project identity and durable-host role contract as `recover_mcp.ps1` (section 3.1). If valid, print `[pid:<mcp-pid>][yyyy-MM-dd HH:mm:ss][McpUp] uptime=<days>D/<hh>:<mm>:<ss>`, reset any trusted-busy streak, run the daily maintenance pass if due, and sleep. If a transport failure leaves one exclusive listener PID whose live executable and readable command line prove the exact current-project durable editor identity, print `TrustedEditorBusy`, perform no build/recover/launch/stop mutation, and retry with exponential sleep capped by `-TrustedBusyBackoffMaxSec`. If exact-project `-TestExit` or Automation `RunTests`/`RunAll` process candidates exist, print `EphemeralAutomationActive`, perform no mutation, and wait for their planned exit even when the port is clear. Otherwise, if down, inspect only current-project durable editor-server candidates. When none remain, run the primary editor UBT build derived from the host `.uproject`, attempt restart-triggered source maintenance, then call `recover_mcp.ps1 -ForceLaunch` to relaunch and wait. Source index completion maintains the EngineSource CRG projection and graph-node FTS in the same database; the watchdog never runs a separate graph export. A pre-restart maintenance failure is logged but cannot prevent availability recovery; after health returns the watchdog retries all requested restart-maintenance targets through the live path. A post-recovery maintenance failure remains explicit but does not turn a successful endpoint recovery into a restart failure. When a current-project durable editor-server candidate still exists, do not run UBT first; delegate to `recover_mcp.ps1` without `-ForceLaunch`. If that recovery fails and the remaining candidate is a current-project headless editor (`-NullRHI`, `Saved\HeadlessMcp`, or `HeadlessEditor-*` command line), stop only that unhealthy headless process and run the restart sequence. Dedicated-server, commandlet, planned-exit automation, other-project, and user-facing editor processes are never stopped by this branch.

| Parameter | Default | Notes |
|---|---|---|
| `-McpUrl` | `MONOLITH_URL` env var, else `http://localhost:9316/mcp` | Same endpoint resolution as `recover_mcp.ps1` |
| `-HealthTimeoutSec` | 5 | Absolute timeout for each individual `/health` HTTP request; validated range is 1--60 seconds and the same value is forwarded to every `recover_mcp.ps1` child |
| `-TrustedBusyBackoffMaxSec` | 60 | Validated 1--3600-second cap for exact-project trusted-busy retry sleep; the streak starts at `PollIntervalSec`, doubles to this bound, performs no mutation, and resets on a valid health result |
| `-PollIntervalSec` | 15 | Healthy-state probe interval |
| `-RecoverTimeoutSec` | 600 | Passed to `recover_mcp.ps1 -TimeoutSec`; also bounds how long a live-but-unhealthy headless editor is tolerated before the watchdog restarts it |
| `-RecoverPollIntervalSec` | 5 | Passed to `recover_mcp.ps1 -PollIntervalSec` |
| `-NoBuildBeforeRestart` | off | Skips the UBT step; for launch diagnostics only, not normal agent operation |
| `-ProbeOnly` | off | Report one health/process sample and exit; never builds, recovers, launches, or runs index maintenance |
| `-ProbeBuildLocksOnly` | off | Probe the UBT link outputs for process locks and exit: `[BuildLocksClear]` exit 0 when free, `[BuildLocksPresent]` exit 2 when locked, `[BuildLockProbeFailed]` exit 11 when an unexpected write-probe exception makes the scan incomplete. Never builds, recovers, launches, or runs index maintenance |
| `-MaxRestartAttempts` | 0 | 0 means unlimited. Past the bound the watchdog stops attempting restarts and escalates the probe sleep (exponential backoff, capped by `-RestartLimitBackoffMaxSec`) while logging `[RestartLimit] attempts=<n> backoffSeconds=<s>`; it does not spin one attempt per poll. A later healthy probe logs `[RestartAttemptsReset]` and clears the budget |
| `-RestartLimitBackoffMaxSec` | 600 | Upper bound for the escalated probe sleep after `-MaxRestartAttempts` is exceeded |
| `-BuildFailureBackoffMaxSec` | 600 | Upper bound for the escalated probe sleep after consecutive pre-restart build failures or locked-DLL build blocks; the streak doubles the sleep from `-PollIntervalSec` and resets on a successful build or healthy probe |
| `-RecoverInvokeGraceSec` | 300 | One `recover_mcp.ps1` child invocation is killed after `-RecoverTimeoutSec + -RecoverInvokeGraceSec` seconds and reported as `[RecoverTimeout]` with recover exit code 124; the watchdog itself keeps running |
| `-Once` | off | Run one probe/recover cycle and exit, suitable for smoke checks |
| `-DisableDailyReindex` | off | Disable scheduled asset/source maintenance |
| `-DailyReindexTime` | `05:00` | HH:mm start time interpreted in `-DailyReindexTimeZone` |
| `-DailyReindexTimeZone` | `Korea Standard Time` | Windows time-zone id; default is KST |
| `-DailyReindexMode` | `incremental` | `incremental` or `full` |
| `-DailyReindexTargets` | `assets,source` | Any subset of `assets`, `source` |
| `-DailyReindexWaitTimeoutSec` | 1800 | Maximum wait for asset/source indexing to become idle before completing maintenance |
| `-DailyReindexWaitPollSec` | 10 | Poll interval for `bridge.get_index_status` while waiting |
| `-DailyReindexActionTimeoutSec` | 120 | HTTP timeout for one MCP action call |
| `-RunDailyReindexNow` | off | Run one maintenance pass as soon as the endpoint is healthy, ignoring the daily schedule; combine with `-Once` for a smoke test |
| `-SkipRestartReindex` | off | Skip restart-triggered maintenance |
| `-RestartReindexMode` | `incremental` | `incremental` or `full` for restart-triggered maintenance |
| `-RestartReindexTargets` | `assets,source` | Any subset of `assets`, `source` for restart-triggered maintenance |
| `-ProjectRoot` | upward search | Explicit host checkout root |

Build behavior:

- The watchdog loads the same activation reader and rechecks it on every loop. Server-disabled state logs one `MCP_DISABLED` transition and performs no health probe, build, recovery, launch, or stop; a long-running watchdog resumes automatically when `Monolith.StartServer` persists true.
- Restart and daily source/asset maintenance separately require indexing activation. When off, each path returns success after a structured `indexing_activation_disabled` skip and leaves existing DBs unchanged. Server supervision can remain enabled independently.
- Watchdog console output starts with `[pid:<mcp-pid>][yyyy-MM-dd HH:mm:ss][EventType]`. `RESULT=` selects the event name and is not repeated as a field; exact `pid=` becomes the prefix; `uptime_seconds=` is rendered as `uptime=<days>D/<hh>:<mm>:<ss>`; `McpUp` omits duplicated version/tool-count fields. Example: `[pid:50844][2026-07-04 00:27:20][McpUp] uptime=0D/00:04:32`.
- Long watchdog payloads are split after the header when the payload is over 240 characters or already contains line breaks. The following retained example is historical evidence from the retired graph-export phase; it illustrates formatting only and is not a current event contract:

```text
[pid:45652][2026-07-04 00:27:48][PreRestartReindexGraphDone]
exitCode=0 detail="{ \"after\": { \"edges\": 1125252, \"files\": 89551 }, \"graph_db\": \"D:\\P4\\speed\\Plugins\\Monolith\\Saved\\graph.db\" }"
```

- `BuildFailed` keeps the exit code and prints the UBT log path as a trailing detail instead of `log=<path>`, for example `[pid:45652][2026-07-04 00:27:48][BuildFailed] exitCode=6, D:\P4\speed\Saved\Monolith\Watchdog\UBT-20260704_002020.log`.
- Every watchdog event is also appended as one JSON object per line to `Plugins\Monolith\Logs\<yyyyMMdd>\watchdog.jsonl` with `timestamp`, `displayTimestamp`, `pid`, `event`, `fields`, and `message`.
- Terminal-state guarantees (2026-07-04 hardening): every instance logs `[WatchdogStart] watchdogPid=<pid> ...` as its first event so instance boundaries are visible in `watchdog.jsonl`; an unhandled terminating error logs `RESULT=FATAL` as the last line and exits 9; `recover_mcp.ps1` runs as a bounded child process and is killed past `-RecoverTimeoutSec + -RecoverInvokeGraceSec` (`[RecoverTimeout]`, recover exit code 124) instead of hanging the watchdog indefinitely.
- Windows PowerShell 5.1 child exit-code integrity (2026-07-11): `Start-WatchdogChildProcess` materializes the `Start-Process -PassThru` native handle before waiting. Without that read, redirected child processes produced `$null` `ExitCode`; a successful `RESULT=MCP_UP` was then treated as failure and the newly healthy headless editor was killed. `Scripts/tests/WatchMcpChildProcess.Tests.ps1` covers the production helper under Windows PowerShell 5.1/Pester 3.4.
- Health/listener identity hardening (2026-07-11): both recovery paths reject malformed/partial HTTP-200 responses, non-200/non-HTTP occupied ports, listener-enumeration failure, and multi-owner PID sets. A healthy PID must be the port's sole unique owner and the current project's eligible Unreal Editor process. The watchdog checks the empty-port gate before the restart sequence, again immediately before UBT, and again before stopping an unhealthy headless editor; `recover_mcp.ps1` rechecks immediately before launch. Current-project user editors remain recovery candidates but never enter the headless-only stop set.
- Measured-latency health budget and trusted-busy state (2026-07-12): both scripts default `-HealthTimeoutSec` to 5 seconds after a valid project-bound `/health` probe was measured at 2,503 ms and the prior 3-second budget produced real false `Blocked` events. A first 5-second rollout later proved that indexing load can still exceed any fixed short budget: PID 30740 returned `McpUp` at 18:09:02, then timed out at 18:09:22 and the old branch exited. The root fix does not keep increasing the timeout. It distinguishes only a transport failure with one live, readable, exact-project editor/listener identity as `TrustedEditorBusy`, stays alive with capped retry backoff, and performs no mutation; every schema, PID/project/listener, pre-build, pre-launch, and pre-stop rejection gate remains unchanged for mismatched or unprovable identity.
- The script resolves the engine root through the host checkout's `Build\BatchFiles\Script\ResolveUnrealEngine.ps1` and the host `.uproject` `EngineAssociation`. It does not hard-code engine paths or substitute another checkout.
- The editor target is the first `Source\*Editor.Target.cs` name when present, otherwise `<ProjectName>Editor`.
- The UBT command is `<Target> Win64 Development "-Project=<uproject>" -WaitMutex -NoHotReloadFromIDE`.
- UBT output is written under `Saved\Monolith\Watchdog\UBT-<timestamp>.log`.
- The script never runs UBT while an editor-server candidate process is still alive. For `.cpp` body-only compile checks while the editor is up, agents should use the editor namespace Live Coding flow instead of this watchdog.
- DLL-lock preflight (pending CL 1086, 2026-07-10 hardening): before every pre-restart UBT invocation the watchdog write-probes the bounded link-output set (`Binaries\Win64\UnrealEditor-*.dll` at the project root, plus one- and two-level plugin `Binaries\Win64` globs). If any output is held by another process — including `-game`/`-server` clients that are deliberately not editor-server candidates — the build is skipped with `[BlockedDllLocked] lockedCount=<n> readonlyCount=<n> consecutiveBuildFailures=<n> backoffSeconds=<s> holderPidsBestEffort=<pids> lockedFiles=<first-8>` (exit 10 in `-Once` mode) instead of burning a UBT run into LNK1104. Read-only/access-denied files are reported in `readonlyCount` but are not treated as process locks. Any other per-file exception is retained in the `ProbeErrors` collection; a non-empty collection produces `[BuildLockProbeFailed] probeErrorCount=<n> probeErrors=<bounded-first-8>` and exit 11, with no UBT invocation. The current 2026-07-06..10 logs contain 446 `BuildFailed` records: 290 `LNK1104`, 100 `UnauthorizedAccessException`, 20 `LNK1181`, and 36 other compile/link failures. The preflight directly closes the DLL-lock retry class; the backoff only bounds the other classes.
- Consecutive build-failure backoff (2026-07-10 hardening): each `BUILD_FAILED` or `BLOCKED_DLL_LOCKED` outcome increments a streak that doubles the next probe sleep from `-PollIntervalSec` up to `-BuildFailureBackoffMaxSec`. A successful build or a healthy probe resets the streak (the healthy-probe reset shares the `[RestartAttemptsReset]` event).
- If a current-project editor-server candidate is alive but `/health` still cannot recover before `-RecoverTimeoutSec`, the watchdog checks for current-project headless-only candidates, stops those `-NullRHI`/`Saved\HeadlessMcp` processes, and only then runs the normal restart sequence. Non-headless user editors, `-game`/`-server` clients, commandlets, and every process for another project are not killed.
- Restart-triggered source maintenance runs before relaunch through `UnrealEditor-Cmd.exe <uproject> -run=MonolithReindex -mode=project -unattended -nopause -nosplash -nullrhi`, writing `Saved\Monolith\Watchdog\MonolithReindex-<timestamp>.log`. `-RestartReindexMode full` maps to `-mode=full`. Completion refreshes EngineSource native rows, CRG projection/cache, and `source_graph_nodes_fts`; there is no second graph-maintenance command.
- Restart-triggered asset maintenance cannot run before a non-commandlet editor exists because `UMonolithIndexSubsystem` intentionally skips DB open in commandlet mode. The watchdog normally defers that asset portion, relaunches the headless editor, waits for validated health, then runs live `bridge.start_indexing(scope="assets", full=<RestartReindexMode>)`. If offline source maintenance failed before launch, endpoint recovery still proceeds and the post-health pass retries all requested restart targets; maintenance failure stays visible but never causes the recovered editor to be killed or reported as unavailable.

Daily maintenance behavior:

- The default schedule is once per selected time-zone date at `05:00` KST. If the watchdog starts after the scheduled time, the first healthy probe on that date runs the pass; failed attempts are not retried in a tight loop.
- Asset/source maintenance uses the live Monolith action path first: `bridge.start_indexing({ scope: "all|assets|source", full })`, then `bridge.get_index_status` until the requested indexes are idle or `-DailyReindexWaitTimeoutSec` expires. The JSON-RPC helper reads `structuredContent` first and falls back to JSON `content.text` for older tool responses. Source indexing owns EngineSource CRG and graph-node FTS refresh, so a completed source target ends the source-maintenance portion of the pass.
- `-ProbeOnly` never runs maintenance. `-Once -RunDailyReindexNow` runs recovery if needed, then performs one maintenance pass and returns exit 8 on maintenance failure.

Interactive Task Scheduler startup:

- For developer workstations, the persistent watchdog should be registered as a per-user Task Scheduler job, not as a Windows Service. Use an interactive logon trigger so the watchdog inherits the developer user's profile, checkout paths, Perforce/Git environment, and display session. Avoid `LocalSystem` and avoid "Run whether user is logged on or not" when the intended operator model is a visible watchdog window.
- The scheduled action should run `Binaries\monolith_watchdog.exe "<checkout>"`, with the working directory set to the host project root. The wrapper intentionally remains alive while `watch_mcp.ps1` runs, so Task Manager shows a recognizable `monolith_watchdog.exe` process instead of only `powershell.exe`.
- Use `-MultipleInstances IgnoreNew` and `-ExecutionTimeLimit ([TimeSpan]::Zero)` so repeated logon triggers do not spawn duplicate watchdogs and Task Scheduler does not stop the long-running supervisor after the default runtime limit.
- Register a second time trigger repeating every 30 minutes (10-year duration; Task Scheduler rejects `[TimeSpan]::MaxValue` as a repetition duration). With `IgnoreNew` this is a no-op while the watchdog lives, and re-arms a watchdog that died mid-session (closed console window, crash, reboot race) within 30 minutes. The 2026-07-04 incident — supervision silently absent for hours after the interactive instance ended — is the class this closes.

Reference registration for the Speed checkout:

```powershell
$taskName = 'Monolith MCP Watchdog - Speed'
$projectRoot = 'D:\P4\speed'
$watchdogExe = Join-Path $projectRoot 'Plugins\Monolith\Binaries\monolith_watchdog.exe'
$user = if ($env:USERDOMAIN) { "$env:USERDOMAIN\$env:USERNAME" } else { $env:USERNAME }

$action = New-ScheduledTaskAction -Execute $watchdogExe -Argument "`"$projectRoot`"" -WorkingDirectory $projectRoot
$logonTrigger = New-ScheduledTaskTrigger -AtLogOn -User $user
$rearmTrigger = New-ScheduledTaskTrigger -Once -At (Get-Date).AddMinutes(30) `
    -RepetitionInterval (New-TimeSpan -Minutes 30) -RepetitionDuration (New-TimeSpan -Days 3650)
$principal = New-ScheduledTaskPrincipal -UserId $user -LogonType Interactive
$settings = New-ScheduledTaskSettingsSet -MultipleInstances IgnoreNew -RestartCount 3 `
    -RestartInterval (New-TimeSpan -Minutes 1) -ExecutionTimeLimit ([TimeSpan]::Zero) `
    -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries

Register-ScheduledTask -TaskName $taskName -Action $action -Trigger $logonTrigger, $rearmTrigger `
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
| 0 | Endpoint intentionally disabled, endpoint up, recovery succeeded in `-Once` mode, or `-ProbeBuildLocksOnly` found all link outputs free |
| 2 | Down, exact-project trusted-busy, or exact-project ephemeral automation and `-ProbeOnly`/`-Once` was requested, or `-ProbeBuildLocksOnly` found locked link outputs |
| 3 | Blocked: host root, `.uproject`, resolver, shared host-role helper, UBT, or recover script missing, or an occupied/unreadable endpoint lacks trusted exclusive project/process identity |
| 4 | UBT build failed before restart |
| 7 | Restart limit reached |
| 8 | Index maintenance failed in `-Once` mode |
| 9 | Unhandled terminating error; `RESULT=FATAL` is the last logged line |
| 10 | Build skipped in `-Once` mode: UBT link outputs are locked by another process (`[BlockedDllLocked]`) |
| 11 | Build skipped: unexpected DLL write-probe exceptions made the lock scan incomplete (`[BuildLockProbeFailed]`) |
| other | `recover_mcp.ps1` exit code when `-Once` is used and recovery fails; a recover child killed on invoke timeout surfaces as recover exit code 124 |

## 5. `check_index_freshness.ps1` Contract

Sequence per namespace (`-Target source|project|all`, default all): `monolith_query.exe <ns> health --include-counts=true` -> parse `status` / `warnings` plus structured `maintenance_recommendation` and `next_actions` -> validate one bounded repair plan -> print exact offline and live repair commands. With `-Execute`: run only the deduplicated health-indicated repairs (`repair_crg_cache`, `repair_fts` only) through the offline CLI, then re-run health and print `VERIFY db=<ns> before=<status> after=<status>`.

| Parameter | Default | Notes |
|---|---|---|
| `-Target` | `all` | `source`, `project`, or `all` |
| `-Execute` | off | Run validated health-indicated repairs instead of only reporting |
| `-AllowLiveEditor` | off | Permit `-Execute` while the MCP `/health` endpoint answers |
| `-QueryExe` | `<plugin>/Binaries/monolith_query.exe` | Override for copied binaries |
| `-McpUrl` | `MONOLITH_URL` env var, else `http://localhost:9316/mcp` | Used only to detect a live editor |

Behavior notes:

- Only the repair plan named consistently by health is ever auto-run. Health problems without a supported structured plan (missing DBs, schema errors) are reported and left to indexing/bootstrap paths — the script does not mask missing data with substitutes.
- Source FTS plans preserve the exact bounded target from `next_actions`: `graph_nodes`, `symbols`, `console_objects`, or `source`. A missing/unknown target, over-broad `target=all`, retired or unknown repair action, or disagreement between `maintenance_recommendation` flags and `next_actions` emits `REPAIR_REJECTED` and leaves the source plan non-runnable.
- `-Execute` against a live editor endpoint is refused (`RESULT=REFUSED`, exit 6) unless `-AllowLiveEditor` is passed; the printed live alternative preserves `target`/`scope` in its JSON arguments and is the preferred path while the editor is up.
- Project `repair_fts` remains `project repair_fts --target=all --execute`; source repair is always `source repair_fts --target=<bounded-target> --execute`.

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

2026-07-03 watchdog verification (historical, before graph-export retirement):

The graph-refresh cells below are retained verbatim as evidence of what the watchdog previously executed. Current watchdog operation has no graph target or `Saved\graph.db` step.

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

2026-07-04 recovery probe diagnostics follow-up (historical formatting evidence; graph-named sample events are retired):

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

2026-07-10 DLL-lock/backoff verification and 2026-07-11 audit:

| Gate | Result |
|---|---|
| Detailed record | [2026-07-10-watchdog-dll-lock-preflight.md](../testing/2026-07-10-watchdog-dll-lock-preflight.md) records the live locked probe, clear build/recover path, exact incident classification, and remaining gaps. |
| Locked/clear probe | A live editor produced `BuildLocksPresent` with 63 locked candidates including `UnrealEditor-CommonGame.dll`; a later clear path ran UBT successfully and recovered the endpoint. |
| Backoff end-to-end | Not run. No `BlockedDllLocked` event exists in the current logs, so the lock→backoff→unlock behavior is design-verified rather than live-verified. |
| Recover exit-code integrity | Historical `RecoverDone` rows contained blank exit codes; one `RESULT=MCP_UP` was immediately followed by a false unhealthy-headless stop. The production helper now pins the child handle before wait; Windows PowerShell 5.1/Pester 3.4 regression passed 1/1, and the reloaded watchdog held the recovered endpoint healthy through the bounded observation window. |
| Operational supervision | The initial 2026-07-11 audit found the task `Disabled`; it later ran with both triggers enabled. Rapid live recoveries proved build/reindex/relaunch execution, while the blank-exit defect also proved that task state alone is insufficient. Treat task state and endpoint readiness as separate live rollout gates. |
| Health/identity, fail-closed mutation, and availability-ordering regression | PowerShell 7.6.3 and Windows PowerShell 5.1, both with Pester 3.4, ran `Scripts/tests/WatchMcpChildProcess.Tests.ps1`: 9 passed, 0 failed on each runtime. Coverage includes malformed HTTP-200 JSON rejection, required health fields/route/port, fractional uptime, exclusive PID/listener/project binding for both scripts, multi-owner rejection, foreign project and `-game`/`-server`/`-run=` rejection, an occupied non-health listener blocking before UBT, unexpected DLL write-probe exceptions producing `ProbeErrors`/exit 11 without UBT, and recovery continuing with a live retry after pre-restart indexing failure. |
| Hardened live health probe | Against the real `D:\P4\speed` endpoint after the final gate changes, `recover_mcp.ps1 -ProbeOnly` accepted only the exclusive listener-owning `UnrealEditor.exe` PID 62524 (`version=0.20.3`, `tools_registered=1840`) and exited 0; `watch_mcp.ps1 -ProbeOnly -DisableDailyReindex` reported the same PID and exited 0. |
| Foreign-listener boundary | The earlier live temporary loopback listener proved that a complete look-alike health object from a `pwsh` PID is rejected. The final regression adds the missing non-health case: an occupied listener summary with no accepted HTTP health blocks `Invoke-RestartSequence` before restart-attempt accounting or UBT. Production gates now run before build/launch/stop; the new non-200/non-HTTP branch is unit-verified, not claimed as a second live listener drill. |
| PowerShell parse | `System.Management.Automation.Language.Parser.ParseFile` reported 0 parse errors for both `recover_mcp.ps1` and `watch_mcp.ps1`. |
| MCP-P0-08 scope | CL 1086 now closes health JSON/PID/exclusive-listener/project identity, non-HTTP/non-200 occupied-port mutation safety, current-project candidate filtering, commandlet exclusion, explicit DLL `ProbeErrors`, and maintenance-before-availability ordering. Durable ownership history across process restarts remains separate hardening work; unreadable processes and listener state fail closed and are never accepted as a healthy identity. |

2026-07-12 health-request timeout follow-up:

| Gate | Result |
|---|---|
| PowerShell parse | `watch_mcp.ps1`, `recover_mcp.ps1`, and `WatchMcpChildProcess.Tests.ps1` parsed with zero errors under PowerShell 7.6.3 and Windows PowerShell 5.1. |
| Cross-runtime regression | PowerShell 7.6.3/Pester 3.4 passed 19/19 in 13.60 seconds; Windows PowerShell 5.1/Pester 3.4 passed 19/19 in 13.82 seconds with the repository's required `-ExecutionPolicy Bypass`. Existing health identity, listener ownership, mutation-ordering, DLL-lock, and recovery-order tests stayed green. |
| Delayed valid health | A production-function regression delayed a structurally valid, project-bound health result for 3,200 ms, captured `Invoke-WebRequest -TimeoutSec 5`, and accepted the result in both `recover_mcp.ps1` and `watch_mcp.ps1`. Static assertions pin the 1--60 validation range, default 5 seconds, and watchdog-to-recovery forwarding. |
| Trusted-busy adversarial regression | A production `Get-MonolithHealth` test waits 5,200 ms and returns a transport timeout. One exclusive listener PID plus a live/readable exact-project eligible editor becomes `trusted_busy`; wrong executable, foreign project, commandlet, PID mismatch, unreadable command line, multi-owner listener, and invalid HTTP 200 remain blocked. Retry sleeps are 15, 30, 60, 60 seconds at the default cap, the busy branch contains no build/recover/launch/stop call, and a valid health result resets the streak. Full recovery skips launch and waits; `-ProbeOnly`/`-Once` exits 2 without mutation. |
| First fixed-timeout rollout (superseded) | `Binaries\monolith_watchdog.exe D:\P4\speed` started the default script path with `WatchdogStart healthTimeoutSec=5`. It recorded consecutive `McpUp` events at 18:05:01 and 18:05:17 for PID 49268, then recovered fresh PID 30740 with `RecoverDone exitCode=0` at 18:06:53. Post-recovery indexing completed and PID 30740 returned `McpUp` at 18:09:02, but the next loaded probe exceeded five seconds at 18:09:22; the old branch logged `Blocked` and the wrapper/watch/editor chain exited. This live failure motivated the state fix rather than another timeout increase. |
