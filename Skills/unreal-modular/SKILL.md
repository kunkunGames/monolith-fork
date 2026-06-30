---
name: unreal-modular
description: Use when inspecting Unreal ModularGameplay / ModularGameplayActors receiver lifecycle and GameFeature AddComponents target readiness via Monolith MCP (modular namespace). Covers UGameFrameworkComponentManager availability, known receiver classes, extension events, and actor/component target validation. For authoring GameFeatureData AddComponents entries use unreal-gamefeatures; for Lyra Experience composition use unreal-lyra.
---

# Unreal Modular Gameplay Workflows

Drives the **`modular`** namespace for read-only ModularGameplay diagnostics. These actions are useful before adding GameFeature component requests, because `UGameFrameworkComponentManager` only affects actors that opt into receiver lifecycle.

## Discovery

Confirm live actions and schemas before calling:

```text
monolith_discover({ namespace: "modular" })
monolith_discover({ namespace: "modular", action: "validate_add_component_targets", mode: "schema" })
monolith_discover({ namespace: "modular", action: "trace_game_framework_extension_events", mode: "schema" })
```

## Actions

| Action | Key Params | Purpose |
| --- | --- | --- |
| `get_status` | none | Report `ModularGameplay` / `ModularGameplayActors` plugin and module availability, reflected component manager and known receiver classes, canonical extension events, and expected receiver lifecycle phases. |
| `describe_extension_receiver_lifecycle` | `actor_class?` | List known receiver classes and optionally classify an actor class as a known ModularGameplayActors receiver or `not_proven_by_reflection`. |
| `validate_add_component_targets` | `actor_class*`, `component_class*`, `require_modular_receiver?=true` | Validate an AddComponentRequest target pair. The actor must be an `AActor`, the component must be a `UActorComponent`, both must be concrete/non-deprecated, and by default the actor must be a known ModularGameplayActors receiver subclass. |
| `trace_game_framework_extension_events` | `source_root?`, `source_roots?`, `actor_class?`, `include_engine_modular_sources?=false`, `max_results?=200` | Lexically trace `UGameFrameworkComponentManager` receiver, component-request, handler, extension-event, and Lyra project event-name call sites in C++ source without running PIE or mutating state. |

## Workflow

Before using `gamefeatures.add_game_feature_data_components`, call:

```text
modular.validate_add_component_targets({
  "actor_class": "/Script/ModularGameplayActors.ModularCharacter",
  "component_class": "/Script/MyGame.MyPawnComponent"
})
```

If the actor is a project-specific class that manually calls `AddGameFrameworkComponentReceiver`, pass `require_modular_receiver=false` to downgrade the receiver proof to a warning. Do not treat that as full proof; it only means Monolith did not reject the actor/component class types.

When receiver readiness or extension-event timing matters, add a static trace pass:

```text
modular.trace_game_framework_extension_events({
  "source_root": "Source/LyraGame",
  "max_results": 50,
  "include_line_text": false
})
```

Use `include_engine_modular_sources=true` when you need to compare project call sites against the engine/plugin lifecycle contract. The trace is source-text based: it skips comment-only lines and reports candidate call sites, event-name constants, and function context, but it does not prove runtime reachability, branch coverage, local-player state, or GameFeature activation.

These actions do not spawn actors, activate GameFeatures, create component requests, register extension handlers, send extension events, or modify project runtime code.
