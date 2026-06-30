# SPEC_MonolithLoading

| Field | Value |
| --- | --- |
| Module | `MonolithLoading` |
| Namespace | `loading` |
| Type | Editor |
| Status | Current |

---

## 1. Purpose

`MonolithLoading` provides reusable read-only diagnostics for Epic's `CommonLoadingScreen` plugin and optional Lyra loading-widget handoff. It reads reflected manager/settings/CVar state and, when PIE or game world exists, asks the live `ULoadingScreenManager` for the authoritative debug reason without editing runtime game code.

---

## 2. Ownership

| Area | Owner |
| --- | --- |
| CommonLoadingScreen availability | `loading.get_status` |
| Processor interface/task inventory | `loading.describe_loading_processors` |
| Reason/settings/CVar contract validation | `loading.validate_loading_reason_contract` |
| Live blocker trace | `loading.trace_loading_screen_blockers` |

---

## 3. Actions

| Action | Params | Behavior |
| --- | --- | --- |
| `loading.get_status` | optional `include_settings=true`, `include_cvars=true`, `include_lyra_handoff=false` | Reports `CommonLoadingScreen`, `CommonStartupLoadingScreen`, manager/interface/task/settings classes, CDO settings, CVars, config, and optional Lyra handoff classes. |
| `loading.describe_loading_processors` | optional `include_all_implementers=false`, `class_filter`, `max_objects=100` | Describes `ULoadingProcessInterface` and `ULoadingProcessTask`, and can list loaded implementer classes as candidate inventory. |
| `loading.validate_loading_reason_contract` | optional `include_known_lyra=true`, `strict=false` | Validates manager reason getter reflection, processor interface availability, settings class, CVars, and optional Lyra handoff classes. |
| `loading.trace_loading_screen_blockers` | optional `world_context=pie`, `include_settings=true`, `include_cvars=true`, `include_processor_candidates=true`, `include_lyra_handoff=true`, `max_candidates=64` | Finds PIE/game world, reads the live manager debug reason if available, and returns `pie_not_running`/`world_not_running` cleanly when no runtime world exists. |

---

## 4. Constraints

| Constraint | Requirement |
| --- | --- |
| Runtime isolation | Do not include private `CommonLoadingScreenSettings.h` or compile-link against `CommonLoadingScreen`/`LyraGame`; use class-path reflection and CDO export only. |
| Mutability | Actions are read-only and must not register processors, create tasks, set CVars, alter widgets, start PIE, or save assets. |
| Processor reason limits | `ILoadingProcessInterface::ShouldShowLoadingScreen(FString&)` is native-only, not a UFUNCTION; processor rows are candidate inventory unless a future hard-dependency path is explicitly added. |
| Runtime status | The authoritative live reason is `ULoadingScreenManager::GetDebugReasonForShowingOrHidingLoadingScreen()` when a manager subsystem instance exists. |

---

## 5. Verification

| Check | Required result |
| --- | --- |
| Build | `SpeedEditor Win64 Development` compiles `UnrealEditor-MonolithLoading.dll` via the engine resolver from `Speed.uproject`. |
| Automation | `Monolith.Loading.RegistryAndValidation` passes with zero warnings and zero errors. |
| Drift guard | `Scripts/check_skill_catalog_drift.ps1 -Skill unreal-loading` reports `RESULT=OK`. |
