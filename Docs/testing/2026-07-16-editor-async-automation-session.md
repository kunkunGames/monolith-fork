# Editor Async Automation Session Verification

**Date:** 2026-07-16
**Status:** Implementation and static contract review complete; first protected build proved C++ compilation and exposed the engine distribution's missing AutomationController import library, so the module contract was corrected to public-interface include plus dynamic load; protected rebuild and live-editor E2E pending parent orchestration
**Changelist:** 1175

---

## 1. Scope

Add a frame-loop-safe asynchronous editor automation session contract so latent and PIE automation tests, including `Speed.UI.UMG.Convergence.CaptureCanonicalPIE`, can be started, polled, and boundedly stopped through Monolith without re-entering the Unreal frame loop.

---

## 2. Static verification

| Gate | Result | Evidence |
|------|--------|----------|
| Session ownership | PASS | `MonolithAutomationSession.cpp` uses `IAutomationControllerManager` discovery, exact enabled-path selection, `RunTests(true)`, result observation, and owned `StopTests()` only. |
| Frame-loop safety | PASS | The session contains no direct controller/worker tick, framework latent/network execution, editor tick, or world tick. |
| Structured results | PASS | Poll/final reports contain run counters and per-test identity, state, duration, errors, warnings, and bounded log snippets. |
| Lifecycle cleanup | PASS | Exact delegate handles, ticker cleanup, prior controller-setting restoration, module-shutdown cleanup, run/discovery deadlines, and five-second stop grace are implemented. |
| Run-slot integrity | PASS | Both start actions reject every second request with `automation_busy` before no-match recording; focused tests compare active/current/last/history identity before and after rejection, and finished-run recording clears the active slot only when the finished object owns `CurrentAutomationRun`. |
| Engine module linkage | CORRECTED | The first two protected builds compiled the new sources, then exposed nonexistent `UnrealEditor-AutomationController.lib` and `UnrealEditor-AutomationTest.lib` imports. `MonolithEditor.Build.cs` now uses `PrivateIncludePathModuleNames` plus `DynamicallyLoadedModuleNames` for both engine-owned runtime-loaded DLLs, avoiding a false static-link contract. |
| Focused tests | ADDED | `Monolith.Editor.Automation.AsyncNoMatchPoll` verifies busy rejection plus run-slot/poll identity when invoked inside a Monolith run, while preserving immediate terminal no-match/poll/history verification when independently launched without one. `Monolith.Editor.Automation.AsyncNestedStartGuard` verifies the same run-slot identity contract for a positive nested start. |

---

## 3. Pending protected verification

The parent task owns the protected `Build\BatchFiles\BuildGameEditorAndRun.bat` invocation and live-editor run, so this implementation subtask did not build or launch the editor. After the protected build, run:

1. The two focused `Monolith.Editor.Automation.Async*` contract tests.
2. `editor.start_automation_tests` with prefix `Speed.UI.UMG.Convergence.CaptureCanonicalPIE`.
3. Poll the returned `run_id` until terminal and verify the canonical PIE capture result/evidence.
4. Exercise an owned cancellation run and verify `stopping` transitions to terminal `stopped` within the bounded grace period.

Screenshot and Discord upload: N/A for this implementation handoff. The live canonical PIE E2E is pending and must perform the project screenshot/Discord evidence workflow when executed.
