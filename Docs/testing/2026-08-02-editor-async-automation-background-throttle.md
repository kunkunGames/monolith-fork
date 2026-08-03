# Editor Async Automation Background-Throttle Verification

**Date:** 2026-08-02

**Status:** Submitted; protected build, focused automation, background-editor completion, bounded stop, state restoration, and dirty-state audit passed

**Changelist:** 1433 (submitted; pending number 1422)

---

## 1. Scope

Keep Monolith-owned asynchronous `AutomationController` runs responsive when the Unreal Editor is not foreground, while preserving the operator's exact prior editor performance setting after every terminal path.

---

## 2. Confirmed root cause and contract

| Area | Confirmed root cause | Correction |
| --- | --- | --- |
| Readiness timeout | With `UEditorPerformanceSettings::bThrottleCPUWhenNotForeground` enabled, the background editor ran at 3 FPS. Unreal's stateful `FWaitForInteractiveFrameRate` readiness gate requires at least 10 FPS, so otherwise valid async runs remained in `preparing` until `readiness_timeout`. | `FBackgroundCPUThrottleScope` temporarily disables only the in-memory background CPU throttle for the Monolith-owned run. |
| State ownership | Permanently changing or saving the operator's editor preference would leak automation policy into normal editor use. | The scope snapshots the exact prior boolean and restores it on completion, failure, explicit stop, timeout, module shutdown, and defensive inactive cleanup; it never calls `SaveConfig`, `Modify`, or `PostEditChange`. |
| Observability | A terminal timeout alone did not distinguish worker discovery, interactive-frame readiness, or execution failure. | Full run JSON records whether background throttling was initially enabled, whether the scope remains active, and whether terminal cleanup restored it. |

---

## 3. Verification gates

| Gate | Result | Evidence |
| --- | --- | --- |
| Focused automation | PASS | Original run `automation-20260802T135719Z-2C7600CA` and final post-link run `automation-20260802T151500Z-7B8A531F` each ran `Monolith.Editor.Automation.*`: 6/6 passed with zero errors and zero warnings. This includes the scope's enabled/disabled prior-state restoration, repeated activation/restoration, async nested-start guard, no-match history, status, and stop contracts. |
| Protected build | PASS | The task build used `Build\BatchFiles\BuildGameEditorAndRun.bat` with `P4_BUILD_CHANGELIST=1422` and `SKIP_EDITOR_LAUNCH=1`; `SpeedEditor Win64 Development` succeeded with exit code 0 in 79.7 seconds. A later protected whole-editor build over the same CL1422 source completed all 569 actions and provided the final linked editor used by both final post-link runs. The launcher resolved UE 5.8 from `Speed.uproject` and validated project DLLs and module manifests. |
| Live background-editor completion | PASS | Original unfocused-editor run `automation-20260802T135741Z-C58093E1` reported `background_cpu_throttle_was_enabled=true`, reached 47.23 FPS for five seconds, passed `Speed.AssetLoading.Runtime.*` 2/2, and restored the setting. Final post-link run `automation-20260802T151511Z-75AE77C9` independently passed 2/2 with `background_cpu_throttle_was_enabled=true`, `background_cpu_throttle_scope_active=false`, and `background_cpu_throttle_restored=true` at terminal completion. |
| Bounded stop and reuse | PASS | Schema-valid stop probe `automation-20260802T135850Z-3BDA9C31` restored the prior setting before execution. A subsequent unfocused-editor run, `automation-20260802T135948Z-5550B499`, again began with `background_cpu_throttle_was_enabled=true`, passed 2/2, and reported terminal restoration, proving the previous stop did not leak the override. |
| Persistent-setting safety | PASS | The implementation contains no config-save/edit notification path. Live `editor.list_dirty_packages` returned `count=0` after the focused suite, completion run, stop probe, and reuse run; every subsequent session observed the original throttle as enabled. |
| Static / ownership audit | PASS WITH BASELINE | Targeted `git diff --check` passed. Hosted static CI reported four pre-existing out-of-scope blockers (missing generated catalog snapshot, stale offline query binary, and two inventory/contract consequences) and no CL1422-specific finding; those unrelated dirty-worktree assets were not edited or moved. |
| Screenshot / Discord | N/A | This is non-visual automation infrastructure; no presentation state changed. |

---

## 4. Acceptance

Pending CL1422 passed the exact P4/Git ownership re-audit and was submitted as CL1433 on the current linked module.
