# Monolith Async Job Cancellation Verification

**Date:** 2026-06-21
**Scope:** `FMonolithAsyncJobRegistry` terminal-state immutability and `ai.rebuild_zone_graph` cancellation checkpoints
**Result:** Passed

---

## 1. Static Checks

Command:

```powershell
python Scripts\ci_static_checks.py --config .github\monolith-static-ci.json --github check
```

Result: exit code 0, `Blocking findings: 0`. Existing advisory findings remain for CRLF text hygiene, missing `.claude\agents`, and skipped live-editor skill drift.

---

## 2. Build

Command:

```powershell
$projectRoot = (Get-Location).Path
$uproject = Join-Path $projectRoot "GO.uproject"
$resolver = Join-Path $projectRoot "BatchFiles\Script\ResolveUnrealEngine.ps1"
$engineRoot = powershell -NoProfile -ExecutionPolicy Bypass -File $resolver -Project $uproject -Output Root
& "$engineRoot\Engine\Binaries\DotNET\UnrealBuildTool\UnrealBuildTool.exe" GoGameEditor Win64 Development "-Project=$uproject" -WaitMutex -NoHotReloadFromIDE
```

Result: `Result: Succeeded`.

Note: the build emitted pre-existing `FCoreDelegates::OnPostEngineInit` deprecation warnings in `Source\MonolithAI\Private\MonolithAIModule.cpp`; the changed async job and ZoneGraph files compiled and linked successfully.

---

## 3. Automation

Command:

```powershell
$report = Join-Path $projectRoot "Saved\Automation\MonolithAsyncJobs_20260621_221312"
$log = Join-Path $projectRoot "Saved\Logs\MonolithAsyncJobs_20260621_221312.log"
& "$engineRoot\Engine\Binaries\Win64\UnrealEditor-Cmd.exe" $uproject -NullRHI -Unattended -NoSplash -NoSound -nop4 -NoSourceControl "-ExecCmds=Automation RunTests Monolith.Core.AsyncJobRegistry+Monolith.AI.ZoneGraphRebuildJob; Quit" "-TestExit=Automation Test Queue Empty" "-ReportExportPath=$report" "-AbsLog=$log" -log
```

Report: `Saved\Automation\MonolithAsyncJobs_20260621_221312\index.json`

| Test | Result |
|---|---|
| `Monolith.AI.ZoneGraphRebuildJob.AsyncJobsDisabled` | Success |
| `Monolith.AI.ZoneGraphRebuildJob.CancelledAfterSubmit` | Success |
| `Monolith.AI.ZoneGraphRebuildJob.Disabled` | Success |
| `Monolith.AI.ZoneGraphRebuildJob.Enabled` | Success |
| `Monolith.Core.AsyncJobRegistry.BoundedRows` | Success |
| `Monolith.Core.AsyncJobRegistry.Cancellation` | Success |
| `Monolith.Core.AsyncJobRegistry.FailAndError` | Success |
| `Monolith.Core.AsyncJobRegistry.Lifecycle` | Success |

Summary: 8 succeeded, 0 failures, total report duration 0.0658 s. This project target compiles with `WITH_ZONEGRAPH=0`, so `Monolith.AI.ZoneGraphRebuildJob.CancelledBeforeBroadcast` is present only for ZoneGraph-enabled targets and was not registered in this run.
