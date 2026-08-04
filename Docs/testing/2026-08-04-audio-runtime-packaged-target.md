# MonolithAudioRuntime Packaged-Target Verification

**Date:** 2026-08-04
**Scope:** `Source/MonolithAudioRuntime`
**Engines:** Unreal Engine 5.7 and 5.8

---

## 1. Regression

`MonolithAudioPerceptionStatics.cpp` uses `GEngine`, `UEngine::GetWorldFromContextObject`, and `EGetWorldErrorMode`; `MonolithAudioRuntimeModule.cpp` uses `IMPLEMENT_MODULE`. Previously, both translation units depended on transitive declarations supplied by Editor shared-PCH or unity-build context. Packaged and monolithic targets do not guarantee those declarations.

## 2. Contract

| Translation unit | Direct dependency | Owning header |
|---|---|---|
| `MonolithAudioPerceptionStatics.cpp` | `GEngine`, `UEngine::GetWorldFromContextObject`, `EGetWorldErrorMode` | `Engine/Engine.h` |
| `MonolithAudioRuntimeModule.cpp` | `IMPLEMENT_MODULE` | `Modules/ModuleManager.h` |

The fix changes compile-time dependency ownership only; it does not change runtime behavior or module dependencies.

## 3. Verification

| Gate | Expected result | Result |
|---|---|---|
| Repository diff audit | Only direct includes and synchronized documentation change | Pass — `git diff --check` returned no errors |
| UE 5.7 plugin package build | `RunUAT BuildPlugin` succeeds | Pass — Editor, UnrealGame Win64 Development, and UnrealGame Win64 Shipping; UAT exit 0 |
| UE 5.8 plugin package build | `RunUAT BuildPlugin` succeeds | Pass — Editor, UnrealGame Win64 Development, and UnrealGame Win64 Shipping; UAT exit 0 |

Both engine roots were resolved from validation-host `.uproject` `EngineAssociation` values through `Build/BatchFiles/Script/ResolveUnrealEngine.ps1`; no engine path was selected manually. Final packaged artifacts are outside the source checkout under `D:\P4\MonolithValidation20260804\01-audio\UE57-Win64` and `D:\P4\MonolithValidation20260804\01-audio\UE58-Win64`.

The authoritative command shape for each engine was:

```powershell
& $runUat BuildPlugin -Plugin=<worktree>\Monolith.uplugin -Package=<external-output> -TargetPlatforms=Win64 -Rocket
```

An earlier unrestricted UE 5.7 attempt reached a successful 434/434 Editor compile before requesting Android, which is unavailable on the validation host. A concurrent retry was discarded after MSVC exhausted virtual memory. The final serialized, external-output builds above supersede both environment-only attempts.
