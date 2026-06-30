# SPEC_MonolithModular

| Field | Value |
| --- | --- |
| Owner | Monolith |
| Module | `MonolithModular` |
| Namespace | `modular` |
| Status | Implemented read-only diagnostics and source-trace slice |
| Last Updated | 2026-06-30 |

---

## 1. Purpose

`MonolithModular` provides reusable read-only diagnostics for Unreal projects that use `UGameFrameworkComponentManager`, `ModularGameplay`, `ModularGameplayActors`, and GameFeature AddComponents-style receiver/component contracts.

The module replaces project-specific checks around “will this AddComponent request actually reach actors?” with explicit class, lifecycle-readiness, and static extension-event source trace reports. It does not edit GameFeatureData, runtime actors, `CommonGame`, `LyraGame`, or project gameplay modules.

---

## 2. Module Contract

| Contract | Behavior |
| --- | --- |
| Dependencies | Depends only on `Core`, `CoreUObject`, `Engine`, `MonolithCore`, `Json`, `JsonUtilities`, and `Projects`. |
| Optional systems | `ModularGameplay`, `ModularGameplayActors`, and their classes are detected by plugin/module status and reflection. |
| Mutation | None. All `modular` actions are read-only diagnostics. |
| Receiver proof | Source-proven `ModularGameplayActors` subclasses are classified as receiver-lifecycle ready. `ModularGameModeBase` and `ModularGameMode` remain reported as unproven unless source calls are found, because this checkout's ModularGameplayActors game-mode classes do not call the receiver lifecycle functions. Arbitrary `AActor` subclasses are reported as `not_proven_by_reflection` unless a source trace shows relevant call sites. |
| Source trace | `trace_game_framework_extension_events` performs bounded lexical C++ source scanning for receiver lifecycle calls, extension handlers, component requests, extension-event sends, canonical event names, and Lyra project event-name constants. It skips comment-only lines and returns candidate call sites, not runtime proof. |
| Runtime behavior | No PIE, no actor spawning, no component creation, no handler registration, no extension-event sending, and no GameFeature activation. |

---

## 3. Actions

| Action | Params | Result |
| --- | --- | --- |
| `modular.get_status` | none | Plugin/module availability, reflected `UGameFrameworkComponentManager` and `ModularGameplayActors` class availability, canonical extension events, and known receiver lifecycle rows. |
| `modular.describe_extension_receiver_lifecycle` | optional `actor_class` | Known receiver lifecycle phases plus optional actor-class classification as `known_modular_receiver` or `not_proven_by_reflection`. |
| `modular.validate_add_component_targets` | `actor_class`, `component_class`, optional `require_modular_receiver=true` | Structured `ok`, class summaries, checks, issues, matched known receiver class, and receiver lifecycle status for AddComponentRequest target pairs. |
| `modular.trace_game_framework_extension_events` | optional `actor_class`, `source_root`, `source_roots`, `include_engine_modular_sources=false`, `include_monolith_source=false`, `max_results=200`, `max_files=2000`, `include_line_text=true` | Structured static-source trace of GameFrameworkComponentManager receiver calls, component requests, extension handlers, extension-event sends, event-name constants, matched receiver classification, result counts, limitations, and trace contract. |

---

## 4. Validation Rules

| Rule | Severity |
| --- | --- |
| `actor_class` must load and be an `AActor` subclass. | error |
| `component_class` must load and be a `UActorComponent` subclass. | error |
| Actor and component classes must not be abstract or deprecated. | error |
| If `require_modular_receiver=true`, `actor_class` must be a known `ModularGameplayActors` receiver subclass. | error |
| If `require_modular_receiver=false`, an unproven receiver is reported as a warning, not an error. | warning |

Source-proven receiver classes include `ModularPawn`, `ModularCharacter`, `ModularPlayerController`, `ModularPlayerState`, `ModularGameStateBase`, `ModularGameState`, and `ModularAIController`. `ModularPlayerController` sends the ready event from `ReceivedPlayer`; the other source-proven actor classes send it from `BeginPlay`. `ModularGameModeBase` and `ModularGameMode` are listed as unproven when present because no receiver add/ready/remove calls were found in their source.

Trace rules:

| Rule | Severity |
| --- | --- |
| At least one source root must be resolved. | error |
| At least one source file must be scanned. | error |
| No trace matches is reported as a warning, because some projects may not use ModularGameplay source call sites in the selected roots. | warning |
| Trace rows are lexical candidates and do not prove runtime reachability, branch coverage, GameFeature activation, or local-player state. | limitation |

---

## 5. Verification

| Check | Evidence |
| --- | --- |
| Registry and validation contract | `Monolith.Modular.RegistryAndValidation` verifies the four actions register as read-only, default status returns receiver rows, a known receiver/component pair passes, plain `AActor` fails receiver-proof by default, `AActor` as component class fails, and a LyraGame source trace returns match/event/limitation arrays. |
| Build | `SpeedEditor Win64 Development` should compile `MonolithModular` as an Editor module through the engine resolver from `Speed.uproject`. |
| Scope | The module has no compile-time dependency on `ModularGameplay` or `ModularGameplayActors`; it uses reflection and static source text scanning only. |

---

## 6. Non-Goals

| Non-Goal | Reason |
| --- | --- |
| Creating component requests | Use existing GameFeatures authoring or project runtime code. This namespace diagnoses target readiness only. |
| Runtime proof of arbitrary native function bodies | Source trace reports candidate call sites only; runtime proof requires explicit PIE/runtime verification. |
| Activating GameFeature plugins | Runtime activation and PIE smoke tests are separate, explicitly requested verification surfaces. |
