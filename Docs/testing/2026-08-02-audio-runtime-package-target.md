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
| UE 5.7 `RunUAT BuildPlugin` (`UnrealEditor` + `UnrealGame`) | PASS | UAT exit 0 (`BUILD SUCCESSFUL`); Editor 531/531, Development game 5/5, Shipping game 5/5. Package: `D:\P4\MonolithAudioRuntimeUE57Package`. Logs: `C:\Users\12336\AppData\Roaming\Unreal Engine\AutomationTool\Logs\D+Engine+UE_5.7\UBA-Unreal*-Win64-*.txt`. |
| UE 5.8 `RunUAT BuildPlugin` (`UnrealEditor` + `UnrealGame`) | PASS | UAT exit 0 (`BUILD SUCCESSFUL`); Editor 531/531, Development game 5/5, Shipping game 5/5. Package: `D:\P4\MonolithAudioRuntimeUE58Package`. Logs: `C:\Users\12336\AppData\Roaming\Unreal Engine\AutomationTool\Logs\D+Engine+UE_5.8\UBA-Unreal*-Win64-*.txt`. |
| Focused runtime automation | N/A | The change only restores explicit include ownership; it changes no runtime branch or public behavior. The failing monolithic compile target is the direct regression gate. |
| `git diff --check` | PASS | No whitespace errors after documentation finalization. |

### 3.1 Artifact identities

| Engine/target | Artifact | SHA-256 |
|---|---|---|
| UE 5.7 Editor | `Binaries\Win64\UnrealEditor-MonolithAudioRuntime.dll` | `505D29118B4A25573DA24C85847AD97CB1430A9EBF0DCED63A04D1BF1DBF088B` |
| UE 5.7 Game Development | `MonolithAudioRuntimeModule.cpp.obj` | `B1B18C65AC952293CE401710E219A2D6B449F384EC067524FA146649BFE5FAA4` |
| UE 5.7 Game Shipping | `MonolithAudioRuntimeModule.cpp.obj` | `A206E0DF0ACDAF51FA80D56E665E8A2DBE3251DC8F43BF31FECB86F4874B6684` |
| UE 5.8 Editor | `Binaries\Win64\UnrealEditor-MonolithAudioRuntime.dll` | `BB65ED933B1F904F9858ADA8101A300AF39B435E2D7BEDE0E5BA53F6AF2D95AF` |
| UE 5.8 Game Development | `MonolithAudioRuntimeModule.cpp.obj` | `E6013A69E7CD7C24D1E2A465460FFB0A8E46C9D1268913C7A89A4E760C22F4EC` |
| UE 5.8 Game Shipping | `MonolithAudioRuntimeModule.cpp.obj` | `265036DA92E3F24FB4D022520C93AC548FE12C6828D4F2BC3FE85D123119CADF` |
