# MonolithAudioRuntime package-target verification

**Date:** 2026-08-02
**Scope:** `MonolithAudioRuntime` explicit Unreal Engine header ownership for monolithic game builds
**Branch:** `jules/codex/audio/runtime-package-includes`

---

## 1. Failure reproduced

`RunUAT BuildPlugin` on Unreal Engine 5.7 compiled the complete `UnrealEditor` target (531/531 actions) and then failed in `UnrealGame Win64 Development`:

| Translation unit | Failure | Owning UE header |
|---|---|---|
| `MonolithAudioRuntimeModule.cpp` | `IMPLEMENT_MODULE` did not parse in the monolithic target | `Modules/ModuleManager.h` |
| `MonolithAudioPerceptionStatics.cpp` | `GEngine` and `EGetWorldErrorMode` were undeclared | `Engine/Engine.h` |

The affected files were byte-identical to `contrib/master`, so this was a pre-existing package-target defect rather than a regression in another follow-up branch.

---

## 2. Root fix

Each translation unit now includes the public header that owns the symbol it uses. This removes reliance on Editor-only shared-PCH/transitive include behavior without adding dependencies, fallbacks, or target-specific branches.

---

## 3. Verification

| Gate | Result | Evidence |
|---|---|---|
| UE 5.7 `RunUAT BuildPlugin` (`UnrealEditor` + `UnrealGame`) | PENDING | Package and log paths will be recorded after the exact-head run. |
| UE 5.8 `RunUAT BuildPlugin` (`UnrealEditor` + `UnrealGame`) | PENDING | Package and log paths will be recorded after the exact-head run. |
| Focused runtime automation | PENDING | Exact test filter and result will be recorded after the exact-head run. |
| `git diff --check` | PENDING | Run after documentation finalization. |
