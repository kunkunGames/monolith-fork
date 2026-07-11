# Watchdog DLL-Lock Build Preflight And Build-Failure Backoff Verification

**Date:** 2026-07-10
**Project:** Speed
**Area:** MonolithAgentOps / `Scripts/watch_mcp.ps1`
**Change:** Before every pre-restart UBT invocation the watchdog write-probes the bounded link-output set and skips the build with `[BlockedDllLocked]` (exit 10 in `-Once` mode) when another process holds any output; consecutive `BUILD_FAILED`/`BLOCKED_DLL_LOCKED` outcomes escalate the probe sleep (doubling from `-PollIntervalSec`, capped by new `-BuildFailureBackoffMaxSec`, default 600). New `-ProbeBuildLocksOnly` switch probes and exits without building. Handoff card MCP-P0-08 (rev.2 §0a scope additions).

---

## 1. Incident Basis

`Logs/2026070[6-9]*/watchdog.jsonl` recorded 444 `BuildFailed` events (191 on 2026-07-09 alone, `restartAttempt` up to 155). Every inspected UBT log failed with `LNK1104` on project/plugin DLLs (`UnrealEditor-CommonGame.dll`, `UnrealEditor-SpeedCoreRuntime.dll`) held by running processes that are deliberately not editor-server candidates (`-game`/`-server` clients are excluded by `Get-EditorServerCandidates`), so the watchdog judged `editorMissing` and re-ran UBT into the same lock every cycle.

## 2. Probe Design Note

A DLL mapped into a running process denies **write** access with a sharing violation but allows read access; the first probe draft (read + `FileShare::None`) therefore reported zero locks against a live editor. The shipped probe opens each candidate with `FileAccess::Write` + maximal sharing (`ReadWrite|Delete`) — exactly the access the linker needs — and treats only `IOException` as a process lock. Read-only files (Perforce state, ACL) are counted in `readonlyCount` and are not treated as process locks, because 116 prior watchdog builds succeeded with read-only files present.

## 3. Commands And Results

| Check | Command / Action | Result |
|-------|------------------|--------|
| PowerShell parse | `[System.Management.Automation.Language.Parser]::ParseFile` over `Scripts/watch_mcp.ps1` | Passed: 0 parse errors. |
| Locked path (live editor holding DLLs) | `pwsh -File Scripts/watch_mcp.ps1 -ProbeBuildLocksOnly -ProjectRoot D:\P4\speed` at 22:10 with editor pid 45468 alive | Passed: `[BuildLocksPresent] lockedCount=63 readonlyCount=0 holderPidsBestEffort=45468`, first locked file list includes `UnrealEditor-CommonGame.dll` (the 2026-07-09 LNK1104 file), exit 2. |
| Clear path + real restart sequence | Watchdog scheduled task restarted at 22:17:19 while the editor was down (DLLs free) | Passed: new instance `watchdogPid=52592` ran `RestartSequenceStart` → no `BlockedDllLocked` (preflight found 0 locks) → `BuildStart` → `BuildSucceeded` (UBT-20260710_221723.log) → pre-restart reindex → recover, i.e. the preflight does not block builds when outputs are free. |
| Candidate set sanity | `-ProbeBuildLocksOnly` clear-path run (22:09, pre-fix read-probe build) | `candidateCount=63` link outputs enumerated from the bounded globs (project `Binaries\Win64` + one/two-level plugin `Binaries\Win64`); no recursive Plugins scan. |
| Static checks | `python Scripts/ci_static_checks.py --config .github/monolith-static-ci.json --github check` | Passed with blocking findings: 0. Advisory `secrets` warning on `watch_mcp.ps1` is pre-existing (13 pattern matches in the HEAD version and 13 after the change; the diff adds none). |
| Spec sync | `Docs/specs/SPEC_MonolithAgentOpsScripts.md` §4 | Updated in the same changelist: parameter rows (`-ProbeBuildLocksOnly`, `-BuildFailureBackoffMaxSec`), build-behavior bullets (preflight + backoff), exit-code rows (0/2 probe reuse, new 10). |

## 4. Backoff Behavior (Design-Verified)

`Get-BuildFailureBackoffSeconds` doubles from `PollIntervalSec` starting at the second consecutive failure: 15s, 30s, 60s, 120s, 240s, 480s, then the 600s cap. Against the 2026-07-09 profile (191 failed UBT runs in one day) the same fault now costs at most ~6 skipped-build probes per hour after warm-up, and `BLOCKED_DLL_LOCKED` skips the UBT invocation entirely (each previously burned a ~25-40s UBT run). The streak resets on `BuildSucceeded` or a healthy probe (`RestartAttemptsReset` now also clears it). A full lock→backoff→unlock→build live drill requires taking the endpoint down while a game client holds DLLs; deferred to the next natural outage, tracked by the `BlockedDllLocked` event name in `watchdog.jsonl`.

## 5. Operational Note

During verification the watchdog scheduled task (`Monolith MCP Watchdog - Speed`) was found with its triggers disabled while a stale instance ran detached; the instance was stopped, the task re-enabled and started at 22:17:19, and the first cycle recovered the endpoint with the new code loaded. If the trigger disable was intentional, re-disable with `Disable-ScheduledTask -TaskName 'Monolith MCP Watchdog - Speed'` — but the loop that motivated disabling is the class this change closes.
