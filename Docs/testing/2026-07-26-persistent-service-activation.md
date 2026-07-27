# Persistent Service Activation Verification

| Metadata | Value |
|---|---|
| Date | 2026-07-26 (final merged-config review reruns: 2026-07-27) |
| Target | `tumourlove/monolith` `master` |
| Engine | Unreal Engine 5.8 (`Speed.uproject` `EngineAssociation`) plus UE 5.7 compatibility host |
| Host project | UE 5.8: `D:\P4\MonolithActivationBuildHost\ActivationHost.uproject`; UE 5.7 final review: `D:\P4\MonolithPR114UE57Host\MonolithPR113UE57Host.uproject` |
| Scope | Persistent `Monolith.StartServer` and `Monolith.StartIndexing` activation |

---

## 1. Contract Under Test

| Area | Required behavior |
|---|---|
| Defaults | Project defaults come from `Config/DefaultMonolith.ini`. |
| User state | Generated overrides live in `Saved/Config/<Platform>/Monolith.ini`. |
| Independence | Changing server activation does not overwrite indexing activation, and vice versa. |
| Invalid data | An explicit malformed boolean fails closed instead of silently using a permissive fallback. |
| Migration | `Saved/Monolith/Activation.ini` is migrated once to the generated user config. |
| Cache | Project-default changes participate in the cache key, in-process writes invalidate immediately, and external edits are revalidated within one second. An unreadable input fails closed inside that interval but is retried afterward even if its timestamp is unchanged. Accepted activation keys replace stale dirty values before a later flush without replacing the fully merged `GConfig` hierarchy or losing unrelated settings. |
| Server lifecycle | Start and stop commands persist the next-launch choice and apply it to the current process; a one-second cache plus a one-second ticker reconcile external activation edits within two seconds worst-case. |
| Port ownership | A listener present before bind is rejected; this process must not report success or write a sentinel for another Editor's port. |
| Shared listener cleanup | If the bounded post-bind probe reports failure after the UE listener bound, cleanup must unbind only Monolith routes, keep unrelated UE listeners running, and allow same-port retry through the retained router. |
| Live port change | Restart must reject a different configured port before unbinding working routes or removing the owned sentinel. |
| Sentinel cleanup | A transient deletion failure retains sentinel ownership so a later Stop or shutdown can retry; ownership clears only after deletion or confirmed absence. Startup removes a sentinel only when its numeric PID is the current or a no-longer-running process, and preserves a live foreign owner, malformed owner, or file replaced during validation. |
| Index lifecycle | Start and stop commands gate automatic source and asset writers while preserving read access to existing databases. |
| External writer deactivation | Revalidated `IndexingEnabled=False` state must block new source and asset writes even when process-local hooks were already enabled. |
| Source writer start failure | Thread-creation and writer-open failures must broadcast completion so the subsystem clears indexing state and reopens the readable DB. |
| Re-index response | Project and source re-index actions must report the writer's actual acceptance, including process-local deactivation and thread-start failures. |
| Settings re-index eligibility | Project and source Settings buttons must use the subsystem's effective process-local writer state, not only the durable activation value. |
| Policy | `bMcpServerEnabled`, `bEnableIndex`, and `bEnableSource` remain authoritative hard gates. |
| First source bootstrap | A missing engine-source database is created only after an explicit persistent indexing start, not merely because a fresh install inherits the project default. |

---

## 2. Build Verification

The plugin was mounted through a directory junction at
`D:\P4\MonolithActivationBuildHost\Plugins\Monolith` so the dirty Speed checkout
and other Monolith worktrees were not modified.

The protected project entry point was run with:

```powershell
$env:UE_PROJECT = "D:\P4\MonolithActivationBuildHost\ActivationHost.uproject"
$env:UE_EDITOR_TARGET = "ActivationHostEditor"
$env:P4_BUILD_CHANGELIST = "1325"
$env:ALLOW_RUNNING_EDITOR = "1"
$env:SKIP_BINARY_PREP = "1"
$env:SKIP_BINARIES_RECONCILE = "1"
$env:SKIP_EDITOR_LAUNCH = "1"
$env:MONOLITH_RELEASE_BUILD = "1"
& "D:\P4\MonolithActivationBuildHost\Build\BatchFiles\BuildGameEditorAndRun.bat"
```

| Result | Evidence |
|---|---|
| PASS | `Result: Succeeded` |
| PASS | `[BuildSpeedEditorAndRun] Build succeeded.` |
| PASS | `PROTECTED_BUILD_EXIT=0` |
| PASS | `MonolithCore`, `MonolithIndex`, `MonolithSource`, and `MonolithEditor` compiled and linked in the full build. |
| PASS | Review-fix sources produced newer `UnrealEditor-MonolithCore.dll`, `UnrealEditor-MonolithIndex.dll`, and `UnrealEditor-MonolithSource.dll` outputs in the isolated UE 5.8 host. |
| PASS | The final UE 5.8 review-fix sequence recompiled `MonolithSettings.cpp` and `MonolithSettingsActivationTests.cpp`, relinked `UnrealEditor-MonolithCore.dll`, and returned protected-wrapper exit 0. |
| PASS | The Settings writer-gate review fix recompiled `MonolithSettingsCustomization.cpp`, both index subsystem implementations, and the focused source test; it relinked `MonolithEditor`, `MonolithIndex`, and `MonolithSource` under UE 5.8 with protected-wrapper exit 0. |
| PASS | A clean detached worktree at the exact final production/test commit compiled and linked all 430 UE 5.7 actions with `Result: Succeeded` and protected-wrapper exit 0 in `D:\P4\MonolithPR114UE57Host`. |
| PASS | The clean UE 5.7 host then rebuilt and relinked the same Settings writer-gate sources and modules from the final production/test tree, with protected-wrapper exit 0; the only later amendment was this evidence-path update. |

The disposable UE 5.7 content-only host used a copied protected wrapper. Its
copy evaluates `SKIP_EDITOR_LAUNCH=1` before requiring a project-specific
Editor executable, because an installed engine reports
`D:\Engine\UE_5.7\Engine\Binaries\Win64\UnrealEditor.exe` as the output. No
Speed or Monolith repository build script was changed for that harness detail.

An initial full compile exposed a runtime-selected `UE_LOG` format-string error
in `MonolithIndexSubsystem.cpp`. The call was replaced with explicit success and
failure log branches, and the complete plugin build then passed.

The first occupied-port test revision also treated UE 5.8
`FSocket::GetAddress` as returning `bool`; the protected build caught the test-only
signature mismatch. The assertion was moved to the assigned port value, and the
final rebuild compiled and relinked the test successfully.

The external-activation reconciliation revision initially stored the core ticker
handle as a general `FDelegateHandle`. UE 5.7 and UE 5.8 require
`FTSTicker::FDelegateHandle` for `AddTicker` and `RemoveTicker`; the protected
build exposed the C2679/C2664 mismatch, the member type was corrected, and the
full affected-module rebuild passed.

---

## 3. Automation Verification

Command:

```powershell
& "D:\Engine\UE_5.8\Engine\Binaries\Win64\UnrealEditor.exe" `
  "D:\P4\MonolithActivationBuildHost\ActivationHost.uproject" `
  -Unattended -RenderOffscreen -NoP4 -NoSplash -NoSound `
  '-ExecCmds=Automation RunTests Monolith.Activation; Quit' `
  '-TestExit=Automation Test Queue Empty' `
  '-ReportExportPath=D:\P4\MonolithActivationBuildHost\Saved\Automation\PR114ReviewMergedConfig3UE58'
```

| Result | Evidence |
|---|---|
| PASS | `Found 5 automation tests based on 'Monolith.Activation'`. |
| PASS | `Monolith.Activation.FailedServerProbePreservesSharedListeners` confirmed forced Monolith cleanup left an unrelated UE HTTP listener running, reused the retained same-port router, and rejected a live move to another port without unbinding the working old-port routes. |
| PASS | `Monolith.Activation.OccupiedServerPort` completed with `Result={Success}`. |
| PASS | `Monolith.Activation.PersistentState` confirmed accepted activation keys replace stale dirty values and survive a later flush while non-activation values from the merged cache remain intact. It also confirmed failed owned-sentinel deletion remains retryable, startup reclamation distinguishes dead/current/live/malformed/replaced owners, and an unreadable activation input is retried after the one-second cache interval despite an unchanged timestamp. |
| PASS | `Monolith.Activation.ProcessLocalReindexRejection` confirmed project and source re-index entry points return rejection instead of a false successful start and that both Settings eligibility predicates are false in the same process-local stopped state. |
| PASS | `Monolith.Activation.SourceWriterOpenFailureRecovers` observed exactly one completion signal, a non-zero error count, and cleared running state. |
| PASS | `**** TEST COMPLETE. EXIT CODE: 0 ****`; `ACTIVATION_AUTOMATION_EXIT=0`. |
| PASS | Final UE 5.8 evidence: `D:\P4\MonolithActivationBuildHost\Saved\Logs\PR114SettingsWriterGateUE58.log` and `D:\P4\MonolithActivationBuildHost\Saved\Automation\PR114SettingsWriterGateUE58\index.json`; 5 succeeded, 0 failed, every test reported 0 warnings/errors, and the automation command exited 0. |
| PASS | UE 5.7 `-RenderOffscreen` final evidence: `D:\P4\MonolithPR114UE57Host\Saved\Logs\PR114SettingsWriterGateUE57.log` and `D:\P4\MonolithPR114UE57Host\Saved\Automation\PR114SettingsWriterGateUE57\index.json`; 5 succeeded, 0 failed, every test reported 0 warnings/errors, and the automation command exited 0. |
| PASS | On the free startup port, the UE HTTP listener started 101 ms after `MonolithCore` initialization and Monolith confirmed ownership after 204 ms total; the previous blocking-probe delay of about two seconds was absent. |

The port-ownership preflight uses a non-blocking connect with a 100 ms upper
bound. Pending non-blocking connects are evaluated after `WaitForWrite` instead
of treating the initial `Connect` return value as final. The same final binary
rejected the test-owned loopback listener, accepted the free configured port,
and deactivated a deliberately rejected post-bind route set without stopping an
unrelated listener, so the bounded probe does not weaken ownership or shared
transport isolation.

The initial UE 5.7 `-NullRHI` launch hit the engine-owned
`FGenericWindow::GetRestoredDimensions` Slate-layout crash before test discovery.
The same binary and test selection then passed through UE's supported
`-RenderOffscreen` commandlet path; no Monolith frame was present in the
NullRHI crash stack.

The first final UE 5.8 rerun exposed a config-cache key mismatch: the fixture
registered and queried a relative generated-config path while the settings
helper received its absolute form. `SyncCachedActivationFile` now always
canonicalizes its `GConfig` key with
`FConfigCacheIni::NormalizeConfigIniPath`, and the fixture seeds, reads, flushes,
and unloads that same canonical key. The protected UE 5.8/5.7 rebuilds and the
final reruns above then passed all five tests without warnings or errors.

The final review found a second cache issue: replacing the cached Monolith file
with the raw generated leaf removed project/platform sections already merged by
Unreal. `SyncCachedActivationFile` now rebuilds the hierarchy for the live file
and reconciles only `ServerEnabled` and `IndexingEnabled` into the existing
cache. Focused automation seeds a non-activation merged value and proves both
own writes and accepted external edits preserve it while still replacing stale
activation values. Both final engine runs above pass that regression.

The first fresh UE 5.7 build attempt stopped before compilation because another
concurrent UBT process held the engine-global
`C:\Users\12336\AppData\Local\UnrealBuildTool\Log.txt`. No Monolith compiler
action ran in that attempt. After the external owner exited, the same protected
entry point completed the full compatibility build on the exact review source. An initial
successful automation rerun then identified only UE's deprecated
`-ReportOutputPath` command-line warning; the final evidence above uses
`-ReportExportPath` and is warning-free at the automation-controller and
per-test levels.

---

## 4. Console Lifecycle E2E

The real Editor console path was exercised in the isolated host, not only the
settings helper used by the automation test.

| Transition | Result | Evidence |
|---|---|---|
| Occupied port | PASS | A controlled TCP listener owned port 19316 before launch. Startup and explicit Start both failed closed, the automation process exited 0, and `COLLISION_SENTINEL_PRESENT=False`. |
| Stop server | PASS | On owned port 19318, `Monolith.StopServer` unbound Monolith routes, removed the owned sentinel, wrote `ServerEnabled=False`, and completed with `OWNED_STOP_E2E_EXIT=0`; the UE-owned transport may remain bound for unrelated routes and safe same-process reuse. |
| Stop indexing | PASS | `Monolith.StopIndexing` removed source and asset automatic hooks and wrote `IndexingEnabled=False`; the in-flight source writer reported `source_draining=true` and completed with zero errors. |
| Restart while stopped | PASS | The next automation launch logged server/index activation off, while both databases remained available for reads. `INACTIVE_SERVER_STARTS=0` and `INACTIVE_ASSET_INDEX_STARTS=0`. |
| Start server | PASS | With free port 19319, `Monolith.StartServer` bound the listener, wrote the owned sentinel and `ServerEnabled=True`, and completed with `OWNED_START_E2E_EXIT=0`. |
| Start indexing | PASS | `Monolith.StartIndexing` reported both source and asset requests accepted, started the project writer, and wrote `IndexingEnabled=True`. |
| Start-run shutdown | PASS | The verification automation test still passed and the process exited with `START_E2E_EXIT=0`. |
| External server edit | PASS | The final production source tree in one live UE 5.8 process on port 19425: changing generated `ServerEnabled=True` to `False` removed `/health` and the sentinel while the process-shared TCP listener remained reachable; changing it back to `True` restored the route, sentinel, and healthy JSON response without restarting. Evidence: `D:\P4\MonolithActivationBuildHost\Saved\Logs\ExternalActivationFinal.log`. |

The generated host state after the final transition was:

```ini
[Monolith.UserActivation]
ServerEnabled=True
IndexingEnabled=True
```

A preliminary plain-editor `Quit` command did not engage the automation
command-line shutdown path, so that exact isolated-host PID was terminated after
its Stop logs and generated config were captured. The stopped-restart and
Start transitions were then repeated with the automation shutdown path and
completed with exit code 0.

---

## 5. Static and Repository Checks

| Check | Result | Notes |
|---|---|---|
| `git diff --check` | PASS | No whitespace errors. |
| Same-config static differential | PASS | The current checker reported 36 blockers on clean `tumourlove/master` and 36 on the review-fix tree, with `STATIC_INTRODUCED=0` and `STATIC_INTRODUCED_BLOCKERS=0`. |
| Exact hosted command | BLOCKED BY BASE TOOLING | `python Scripts/ci_static_checks.py --config .github/monolith-static-ci.json --github check` cannot run because both referenced files are absent from the target `tumourlove/master` revision. |

The newer checker cannot be treated as an exact drop-in gate for this older base:
its full repository checks expect newer offline-freshness and workflow contracts.
The differential used the same current config and checker for both trees,
disabled only the incompatible `offline_exe_freshness` gate in memory, and
compared complete finding identities. It did not filter or hide a changed-file
finding.

---

## 6. Post-Review Hardening (2026-07-27)

Three defects found in a follow-up self-review were fixed and re-verified on both
engines at the resulting head.

| Defect | Fix | Why it mattered |
|---|---|---|
| `ProbePort` returned `false` whenever `FSocket::SetNonBlocking(true)` failed, so the post-bind probe could never succeed on such a platform and `Start()` would exhaust all attempts on a listener that had actually bound | Fall back to the pre-existing blocking `Connect` when non-blocking setup is unavailable | Probe *setup* failure was being reported as "port not listening", turning a platform capability gap into a permanent startup failure |
| `HandleReindex` read a `bool` return out of a `ProcessEvent` parameter buffer without checking the reflected signature; `MonolithCore` reaches `MonolithIndex` only through reflection, so a future `void` return would silently read `false` from the zeroed buffer | Require `CastField<FBoolProperty>(Func->GetReturnProperty())` and return an explicit module-sync error when absent | A re-index that actually started would have been reported to the caller as `reindex_not_started` |
| `ReconcileHttpServerActivation` carried an unreachable `!bHasResolvedServerActivation` first-tick branch — `StartupModule` sets the flag before `AddTicker`, so it could never be observed false | Removed the branch and the flag; the baseline is resolved before the ticker exists | Dead state suggested a first-tick path that does not exist, making the reconciler harder to review |

A second review pass found two more defects, both fixed in the same follow-up.

| Defect | Fix | Why it mattered |
|---|---|---|
| `ReadConfigFile` returned an empty `FConfigFile` both when the activation file was absent and when it existed but could not be read, because `FConfigFile::Read` returns `void` and its result was never probed. An unreadable file therefore resolved to the project defaults, which are enabled | Probe readability with `FFileHelper::LoadFileToString` and distinguish `Absent` / `Read` / `Unreadable`; an unreadable user file fails closed with both services disabled and is neither migrated nor rewritten | A transient permission or lock failure silently **re-enabled a persistently stopped server**, and the write path would have persisted a file containing only the key being set, reverting the other service to its enabled default |
| After a `MonolithCore` unload/reload, the replacement `FMonolithHttpServer` has no router ownership, so the pre-bind check saw Monolith's own retained listener and refused every start for the rest of the process | Ask `FHttpServerModule` — which outlives the reload — for the port's router. It returns the existing in-process listener without rebinding, and yields nothing for a foreign owner | Persistent activation and `Monolith.StartServer` could not restore routes until the editor exited |

Distinguishing the two listener cases costs one rejected bind on the foreign-owner
path, which UE logs at `Error` level (`HttpListener unable to bind to ...`). That
only happens where startup was going to fail anyway, and the message is accurate,
so `Monolith.Activation.OccupiedServerPort` now expects it.

`Monolith.Activation.ReloadReclaimsRetainedListener` is new and covers the reload
path directly: it starts one instance, stops and destroys it, confirms the UE
listener is still reachable, then asserts a fresh instance reclaims that port.

| Gate | Result | Evidence |
|---|---|---|
| UE 5.7 editor build | PASS | `D:\P4\MonolithPR114ReviewUE57Host`: recompiled `MonolithCoreModule.cpp`, `MonolithCoreTools.cpp`, `MonolithHttpServer.cpp`, `MonolithSettings.cpp`, `MonolithSettingsActivationTests.cpp`; relinked `UnrealEditor-MonolithCore.dll`; `Result: Succeeded` |
| UE 5.7 automation | PASS, 16/16 | `Monolith.Activation` (6) + `Monolith.Source` (10) under `-RenderOffscreen`: every test `Success`, `failed=0`, exit 0 |
| UE 5.8 editor build | PASS | `D:\P4\MonolithActivationBuildHost`: relinked `UnrealEditor-MonolithCore.dll`, `Result: Succeeded` |
| UE 5.8 automation | PASS, 16/16 | Same suite: every test `Success`, `failed=0`, exit 0 |

Both automation hosts pin `ServerPort=19316` in their own `Config/DefaultMonolith.ini`.
Without that, a host started on a machine where a developer's editor already holds
the default 9316 fails to bind and attributes the engine's bind error to whichever
test is running. That is host configuration, not a plugin behavior.

One behavior was documented rather than changed. Because UE exposes no per-port
listener teardown, `Monolith.StopServer` unbinds routes but leaves the TCP
listener bound for the process lifetime. A port-based liveness check is therefore
not a valid Monolith readiness signal after a Stop — the sentinel file is. This
is now stated in `Docs/specs/SPEC_MonolithCore.md` instead of being implied.

### Round 3

| Defect | Fix |
|---|---|
| The source completion handler captured raw `this`; `Deinitialize()` closes the database and deletes the indexer, so a queued game-thread task could call `ReopenDatabase()` on a torn-down subsystem. Reachable because this branch starts a catch-up run from `Initialize()` | `TWeakObjectPtr` + a `bIsShuttingDown` flag checked in the handler, and `OnComplete.Clear()` before `delete Indexer` |
| `StartPreferredIndex` treated "database missing" and "database present but unopenable" identically and fell through to a CLEAN `TriggerReindex()`, so a transient lock became a destructive rebuild | Full bootstrap reserved for `!FileExists`; an existing-but-unopenable database reports an explicit error and leaves the index intact |

### Round 4

| Defect | Fix |
|---|---|
| `bDeferFirstTimeIndex` hard-coded `bExplicitRequest=false` at startup, so an explicit `Monolith.StartIndexing` was treated as an inherited default and the first-time index was re-deferred on every launch | Pass `Activation.bIndexingUserSet`, matching what the source subsystem already does |
| Live Asset Registry callbacks were re-armed only on `bSuccess`, so a cancelled or failed full index left them unregistered while `bAutomaticIndexingEnabled` stayed true — the subsystem looked active but silently dropped every later asset change | Re-arm on every outcome; `RegisterLiveCallbacks()` is already self-guarding and idempotent, so a genuinely deactivated run still leaves them off |
| The index worker queued `OnIndexingFinished` to the game thread capturing raw `this`/`Owner`. `Deinitialize()` joins the thread, which covers every worker-thread `Owner->` access, but not a task queued just before the join — the same hazard class fixed in the source subsystem | `TWeakObjectPtr<UMonolithIndexSubsystem>` at both queue sites, dropped when the subsystem is gone |

Both rounds re-verified at their heads: UE 5.7 and UE 5.8 editor builds succeed and
`Monolith.Activation` + `Monolith.Source` + `Monolith.Index` report 16/16 with 0
failed and exit 0 on each.

### Round 5

The final review addressed two bounded-recovery defects without weakening the
fail-closed contracts:

| Defect | Fix |
|---|---|
| An unreadable activation file cached the failed-closed value with its unchanged timestamp, so the timestamp fast path could preserve both services as disabled for the rest of the editor process after the file became readable | Cache resolutions now carry an unreadable-input retry bit. The normal one-second interval still bounds disk probes, but the next revalidation bypasses the timestamp fast path until a read succeeds |
| A crashed editor could leave `.monolith_running`, and a later editor starting with activation off never took ownership and therefore never removed it | Non-commandlet startup validates the recorded numeric PID before activation is evaluated. It removes only a current-process reload or dead-process sentinel, reads the file again before deletion, and preserves live foreign, malformed, or concurrently replaced files |

`Monolith.Activation.PersistentState` uses deterministic fixtures for both
contracts: a readable config is temporarily forced through the unreadable path
without changing its timestamp, and sentinel liveness predicates cover dead,
live, same-process reload, malformed-owner, and replacement-race cases.

| Gate | Result | Evidence |
|---|---|---|
| UE 5.7 protected editor build | PASS | `D:\P4\MonolithPR114ReviewUE57Host`; recompiled `MonolithSettings.cpp`, `MonolithCoreModule.cpp`, and `MonolithSettingsActivationTests.cpp`, relinked `UnrealEditor-MonolithCore.dll`, `Result: Succeeded`, wrapper exit 0 |
| UE 5.7 focused automation | PASS, 6/6 | `D:\P4\MonolithPR114ReviewUE57Host\Saved\Automation\PR114ReviewRound5FinalUE57\index.json`; log `Saved\Logs\PR114ReviewRound5FinalUE57.log`; 0 failed, 0 warnings, 0 errors, process exit 0 |
| UE 5.8 protected editor build | PASS | Separate exact source-code snapshot/output host `D:\P4\MonolithPR114ReviewUE58Host`; full 436-action compile/link reported `Result: Succeeded`, followed by protected-wrapper exit 0 |
| UE 5.8 focused automation | PASS, 6/6 | `D:\P4\MonolithPR114ReviewUE58Host\Saved\Automation\PR114ReviewRound5FinalUE58\index.json`; log `Saved\Logs\PR114ReviewRound5FinalUE58.log`; 0 failed, 0 warnings, 0 errors, process exit 0 |

---

## 7. Visual and Discord Evidence

Screenshot verification and Discord upload are not applicable. This change has no
visual, UI, gameplay, level, asset-presentation, or editor-panel output; its
observable contract is console lifecycle, generated config state, and index-writer
behavior.
