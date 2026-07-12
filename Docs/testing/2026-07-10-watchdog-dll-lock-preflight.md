# Watchdog DLL-Lock Build Preflight And Build-Failure Backoff Verification

**Date:** 2026-07-10
**Project:** Speed
**Area:** MonolithAgentOps / `Scripts/watch_mcp.ps1`
**Change:** Before every pre-restart UBT invocation the watchdog write-probes the bounded link-output set and skips the build with `[BlockedDllLocked]` (exit 10 in `-Once` mode) when another process holds any output; consecutive `BUILD_FAILED`/`BLOCKED_DLL_LOCKED` outcomes escalate the probe sleep (doubling from `-PollIntervalSec`, capped by new `-BuildFailureBackoffMaxSec`, default 600). New `-ProbeBuildLocksOnly` switch probes and exits without building. Handoff card MCP-P0-08 (rev.2 §0a scope additions).

---

## 1. Incident Basis

`Logs/20260706..20260709/watchdog.jsonl` currently contain 426 `BuildFailed` events (28 + 57 + 150 + 191; `restartAttempt` reached 155). Including the 20 events appended on 2026-07-10 gives 446. Joining those 446 events to their UBT logs classifies 290 as `LNK1104`, 100 as `UnauthorizedAccessException` (primarily `UnrealEditor.modules`), 20 as `LNK1181`, and 36 as other compile/link failures. DLLs held by running processes that are deliberately not editor-server candidates (`-game`/`-server` clients are excluded by `Get-EditorServerCandidates`) are therefore the largest confirmed failure class, not the only class. The lock preflight directly avoids the `LNK1104` work; the generic consecutive-failure backoff bounds repetition of the remaining classes.

## 2. Probe Design Note

A DLL mapped into a running process denies **write** access with a sharing violation but allows read access; the first probe draft (read + `FileShare::None`) therefore reported zero locks against a live editor. The pending CL 1086 probe opens each candidate with `FileAccess::Write` + maximal sharing (`ReadWrite|Delete`) — exactly the access the linker needs — and treats `IOException` as a process lock. Read-only/access-denied files are counted in `readonlyCount` and are not treated as process locks, because 116 prior watchdog builds succeeded with read-only files present. Every other per-file exception is retained in the returned `ProbeErrors` collection. Any `ProbeErrors` entry makes the scan incomplete, emits bounded `[BuildLockProbeFailed]` diagnostics (first eight entries, formatter capped at 500 characters), and exits 11 without invoking UBT.

## 3. Commands And Results

| Check | Command / Action | Result |
|-------|------------------|--------|
| PowerShell parse | `[System.Management.Automation.Language.Parser]::ParseFile` over `Scripts/watch_mcp.ps1` | Passed: 0 parse errors. |
| Locked path (live editor holding DLLs) | `pwsh -File Scripts/watch_mcp.ps1 -ProbeBuildLocksOnly -ProjectRoot D:\P4\speed` at 22:10 with editor pid 45468 alive | Passed: `[BuildLocksPresent] lockedCount=63 readonlyCount=0 holderPidsBestEffort=45468`, first locked file list includes `UnrealEditor-CommonGame.dll` (the 2026-07-09 LNK1104 file), exit 2. |
| Clear path + real restart sequence | Watchdog scheduled task restarted at 22:17:19 while the editor was down (DLLs free) | Passed: new instance `watchdogPid=52592` ran `RestartSequenceStart` → no `BlockedDllLocked` (preflight found 0 locks) → `BuildStart` → `BuildSucceeded` (UBT-20260710_221723.log) → pre-restart reindex → recover, i.e. the preflight does not block builds when outputs are free. |
| Candidate set sanity | `-ProbeBuildLocksOnly` clear-path run (22:09, pre-fix read-probe build) | `candidateCount=63` link outputs enumerated from the bounded globs (project `Binaries\Win64` + one/two-level plugin `Binaries\Win64`); no recursive Plugins scan. |
| Focused regression | `pwsh` 7.6.3 and Windows PowerShell 5.1, each with Pester 3.4, ran `Scripts/tests/WatchMcpChildProcess.Tests.ps1` | Passed 9/9 on each runtime. New cases prove multi-owner listener rejection, a non-health listener blocks before UBT, and an unexpected write-probe exception is retained in `ProbeErrors` and returns exit 11 without UBT. |
| Static checks | `python Scripts/ci_static_checks.py --config .github/monolith-static-ci.json --github check` | The isolated 2026-07-10 CL verification passed with 0 blocking findings. The `secrets` advisory on `watch_mcp.ps1` was pre-existing (13 pattern matches in the HEAD version and 13 after the change; the diff adds none). The 2026-07-11 shared integration checkout rerun exited 1 because of repository-wide findings from other pending work; among this audit's target files, only the same `watch_mcp.ps1` advisory matched. |
| Spec sync | `Docs/specs/SPEC_MonolithAgentOpsScripts.md` §4 | Updated in the same changelist: parameter rows (`-ProbeBuildLocksOnly`, `-BuildFailureBackoffMaxSec`), build-behavior bullets (preflight + backoff), exit-code rows (0/2 probe reuse, new 10). |

## 4. Backoff Behavior (Design-Verified)

`Get-BuildFailureBackoffSeconds` doubles from `PollIntervalSec` starting at the second consecutive failure: 15s, 30s, 60s, 120s, 240s, 480s, then the 600s cap. Against the 2026-07-09 profile (191 failed UBT runs in one day) the same fault would cost at most ~6 skipped-build probes per hour after warm-up, and `BLOCKED_DLL_LOCKED` would skip the UBT invocation entirely (each `LNK1104` attempt previously burned a ~25-40s UBT run). The streak resets on `BuildSucceeded` or a healthy probe (`RestartAttemptsReset` now also clears it). This progression is code/design verified only: no `BlockedDllLocked` event exists in the current watchdog logs. A full lock→backoff→unlock→build live drill requires taking the endpoint down while a game client holds DLLs and remains an explicit verification gap.

## 5. Recover Child Exit-Code Integrity

The 2026-07-11 live audit exposed a separate self-recovery defect in the same
watchdog path: every inspected `RecoverDone` record had `exitCode=""`. At
01:17 KST, `recover_mcp.ps1` returned `RESULT=MCP_UP`, but the blank exit code
immediately drove `RecoverFailedUnhealthyHeadless` and
`StoppingUnhealthyHeadlessEditors`, killing the healthy editor pid 51108 that
had just answered `/health`.

The root cause is Windows PowerShell 5.1 process-handle behavior. With
`Start-Process -PassThru` plus redirected stdout/stderr, `Process.ExitCode`
remains `$null` if the native handle was never materialized while the child was
alive. `Start-WatchdogChildProcess` now reads `Process.Handle` immediately after
launch, before the bounded wait. This preserves the real child exit code and
prevents `RESULT=MCP_UP` from being misclassified as recovery failure.

`Scripts/tests/WatchMcpChildProcess.Tests.ps1` extracts the production helper
without starting the supervisor loop and verifies under Windows PowerShell
5.1/Pester 3.4 that a redirected child exiting 7 is observed as 7. The expanded
suite originally passed 7/7; the final hardened suite passes 9/9 on both
PowerShell 7.6.3 and Windows PowerShell 5.1, including the listener, probe-error,
and recovery-ordering checks described in §7. After loading the patched script as watchdog pid 49204, editor pid
15096 remained `/health status=ok` through repeated watchdog probes for more
than two minutes; no new blank-exit `RecoverDone` or false headless stop was
observed in that window.

## 6. Operational Note

During verification the watchdog scheduled task (`Monolith MCP Watchdog - Speed`) was found with its triggers disabled while a stale instance ran detached; the instance was stopped, the task re-enabled and started at 22:17:19, and the first cycle recovered the endpoint with the new code loaded. An initial 2026-07-11 audit again observed `Disabled` with no watchdog process, followed by a `Running` task with both triggers enabled. The later rapid recovery loop combined real successful build/reindex/recover work with the blank-exit self-kill described in §5; after the exit-code fix was loaded, the endpoint remained healthy through the bounded observation window. Treat supervision and endpoint readiness as live rollout gates rather than inferring either from code or an earlier task snapshot.

## 7. Adversarial Recovery Hardening

The final CL1086 review found that HTTP 200 alone was an unsafe recovery and
process-ownership boundary. Both `recover_mcp.ps1` and `watch_mcp.ps1` now
require the complete Monolith health contract (`status`, `port`, `version`,
`uptime_seconds`, `tools_registered`, and the primary MCP route), bind the
reported PID as the exclusive unique owner of the TCP listener PID set, require an
`UnrealEditor`/`UnrealEditor-Cmd` process for the exact
`D:\P4\speed\Speed.uproject`, and reject game, server, commandlet,
MCP-disabled, and foreign-project modes.

An occupied or unreadable port 9316 without accepted health is a hard blocker,
including non-200 HTTP, non-HTTP/raw TCP, inconsistent ownership, and multiple
owning PIDs. The watchdog checks this gate before the restart sequence, again
immediately before UBT, and again before stopping an unhealthy headless editor;
the recovery script rechecks immediately before launch. The scripts therefore
do not build, launch, or kill another process in that state. The earlier live
adversarial PowerShell listener returned the documented blocked exit instead of
being promoted to healthy; the final non-health-listener branch is covered by a
focused mutation-order regression. A real Windows PowerShell `-ProbeOnly` run
against the owned Speed endpoint returned `RESULT=MCP_UP`, exit 0.

Recovery availability is also independent from index-maintenance success.
Pre-restart source/graph maintenance failure is recorded but does not abort MCP
recovery. Once the endpoint is owned and healthy, maintenance is retried through
the live path; a later maintenance failure does not kill the recovered endpoint.
The focused Pester suite simulates this ordering and passed.

The final operational split is explicit: CL1086 prevents repeated locked-output
builds, false child-result self-kills, wrong-project listener trust, and
maintenance-before-availability deadlock. CL1092 separately keeps a four-tool
native control plane available while an owned editor endpoint is down.
