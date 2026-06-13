---
name: unreal-level-sequences
description: Use when inspecting Unreal Level Sequences and Sequencer cinematics via Monolith MCP (level_sequence) — list every binding (legacy Possessables/Spawnables and UE 5.7 UMovieSceneCustomBinding subclasses), read the Director Blueprint functions/variables, walk event-track bindings, and reverse-lookup which sections fire a director function project-wide. To read or edit the Director Blueprint graph itself use unreal-blueprints; to edit a montage or anim sequence a track plays use unreal-animation; to spawn or place the actors a sequence possesses use unreal-scene. Triggers on level sequence, sequencer, cinematic, cutscene, LevelSequence, possessable, spawnable, custom binding, MovieSceneSpawnableActorBinding, binding references, director blueprint, director function, event track, FMovieSceneEvent, which sequence calls this function, what is bound in this sequence.
---

# Unreal Level Sequence Workflows

Drives the **`level_sequence`** namespace via `level_sequence_query()` to inspect Level Sequences (Sequencer cinematics): binding inventory, Director Blueprint functions/variables, and event-track wiring. **7 read-only inspection actions** — the table below is a snapshot, so discover the live action set and confirm each action's schema before calling it.

## Discovery

```
monolith_discover({ namespace: "level_sequence" })                              // all actions in this namespace
monolith_discover({ namespace: "level_sequence", action: "list_bindings", mode: "schema" })  // exact params for one action
```

Indexes **every** Level Sequence — including those with no Director. Each binding is classified by inspecting both the legacy `UMovieScene` structures (`FMovieScenePossessable` / `FMovieSceneSpawnable`) and the UE 5.7 `UMovieSceneSequence::GetBindingReferences()` chain, so modern custom bindings (`MovieSceneSpawnableActorBinding`, `MovieSceneReplaceableActorBinding`, etc.) report as `spawnable` / `replaceable` rather than the upgrade-stub `possessable`. Director Blueprint introspection layers on top: own functions (including synthetic `SequenceEvent__ENTRYPOINT<DirBP>_N` UFunctions UE generates for "Quick Bind" event entries), variables, and event-track wiring.

## When to use / Use a different skill for

- **This skill (unreal-level-sequences):** inspect a sequence's bindings/tracks/event-track wiring and its Director's functions/variables; reverse-lookup which sections fire a director function.
- **unreal-blueprints:** to read or edit the Director Blueprint's own graph — functions, variables, nodes, pins — as a Blueprint (the Director is a regular `UBlueprint`; see the combine workflow below).
- **unreal-animation:** to edit a montage or anim sequence that a Sequencer animation track plays, versus inspecting the binding/section wiring here.
- **unreal-scene:** to spawn, move, or place the actors a sequence possesses/spawns in the live level, versus inspecting the sequence's bindings.

## Key Parameters

- `asset_path` — full Level Sequence object path (e.g. `/Game/Cinematics/LS_Intro.LS_Intro`); matches `ULevelSequence::GetPathName()`
- `asset_path_filter` — optional glob (`*` and `?`) for `list_directors` and `find_director_function_callers`; converted to SQL `LIKE`
- `kind` (on `list_director_functions`) — `user` / `custom_event` / `sequencer_endpoint` / `event` (alias for the latter two) / `all`
- `kind` (on `list_bindings`) — `possessable` / `spawnable` / `replaceable` / `custom` / `all`
- `function_name` — exact, case-sensitive name for `find_director_function_callers`

## Action Reference

Param notation: `name*` required, `name?` optional, `name=val` default, `a/b/c` allowed values, `[w]` mutates (transaction-wrapped). This namespace is read-only — none of the actions below mutate. Signatures are a snapshot of the live catalog — for the exact full schema call `monolith_discover` with `mode: "schema"` (the Discovery block above stays the authority).

| Action | Signature | Purpose |
|--------|-----------|---------|
| `list_bindings` | `asset_path*` `kind?` (`possessable`/`spawnable`/`replaceable`/`custom`/`all`) | Every binding inside one LS (one row per `Guid×BindingIndex`), regardless of event-track presence. Each row carries `name`, `kind`, `bound_class`, `custom_binding_class` (e.g. `MovieSceneSpawnableActorBinding`) and `custom_binding_pretty` when a `UMovieSceneCustomBinding` is attached, plus `track_count` and a `kind_counts` breakdown |
| `list_directors` | `asset_path_filter?` (glob `*`/`?`) | All LSes with a Director BP, ordered by path. Each row: `ls_path`, `director_bp_name`, `function_count`, `variable_count` |
| `get_director_info` | `asset_path*` | One Director's summary: `function_breakdown` grouped by kind, `variable_count`, `event_bindings.{total,resolved}`, sample of up to 10 functions ordered user → custom_event → sequencer_endpoint |
| `list_director_functions` | `asset_path*` `kind?` (`user`/`custom_event`/`sequencer_endpoint`/`event`/`all`) | Director's own functions with parsed signatures. Inherited base-class methods and compiler-generated `ExecuteUbergraph*` are not indexed (own-only, matches `blueprint_query` convention) |
| `list_director_variables` | `asset_path*` | Director's own variables (name + K2-formatted type) in declaration order |
| `list_event_bindings` | `asset_path*` | All event-track entries grouped by binding GUID. Each binding shows kind (now correctly `spawnable` / `replaceable` for UE 5.7 custom bindings), bound class, and an array of sections (`trigger` / `repeater`) with the Director function each fires (resolved name + kind + signature when matched) |
| `find_director_function_callers` | `function_name*` (exact, case-sensitive) `asset_path_filter?` (glob `*`/`?`) | Cross-sequence reverse lookup. Returns every event-track section across the project that fires the named function, with LS path and binding context (binding GUID/name/kind/bound class, section kind, resolved bool) |

The live `level_sequence` catalog also registers read-only replay (`get_replay_status`, `list_saved_replays`, `get_saved_replay`), anim-mixer (`get_anim_mixer_status`, `list_anim_mixer_tracks`), and a `ping` smoke test outside this skill's binding/Director inspection scope — `monolith_discover` lists them with schemas if needed.

## Common Workflows

### "What's inside this Level Sequence?"
```
level_sequence_query({ action: "list_bindings", params: { asset_path: "/Game/Cinematics/LS_Intro.LS_Intro" } })
```
Spot modern UE 5.7 spawnables by their `custom_binding_class` (`MovieSceneSpawnableActorBinding` etc.) and the `kind_counts` breakdown.

### "Just the spawnables, please"
```
level_sequence_query({ action: "list_bindings", params: { asset_path: "/Game/Cinematics/LS_Intro.LS_Intro", kind: "spawnable" } })
```

### "Which sequences in module X have a Director?"
```
level_sequence_query({ action: "list_directors", params: { asset_path_filter: "/Game/Cinematics/*" } })
```

### "What does this LS Director do?"
```
level_sequence_query({ action: "get_director_info", params: { asset_path: "/Game/Cinematics/LS_Intro.LS_Intro" } })
level_sequence_query({ action: "list_director_functions", params: { asset_path: "/Game/Cinematics/LS_Intro.LS_Intro", kind: "user" } })
level_sequence_query({ action: "list_director_variables", params: { asset_path: "/Game/Cinematics/LS_Intro.LS_Intro" } })
```

### "Which actor in the level fires which Director function in this LS?"
```
level_sequence_query({ action: "list_event_bindings", params: { asset_path: "/Game/Cinematics/LS_Intro.LS_Intro" } })
```
Returns event-bound bindings grouped by GUID — for each Possessable / Spawnable / master track, see all event-track sections and the Director function each fires.

### "Where else is this Director function called from?"
```
level_sequence_query({ action: "find_director_function_callers", params: { function_name: "OnPlayerEntered" } })
level_sequence_query({ action: "find_director_function_callers", params: { function_name: "SequenceEvent__ENTRYPOINTLS_Intro_DirectorBP_0", asset_path_filter: "/Game/Cinematics/*" } })
```

### Combine with the `blueprint` namespace (unreal-blueprints) for full graph introspection
The Director Blueprint is also a regular `UBlueprint`. For graph-level editing (functions, variables, nodes, pins) switch to the **unreal-blueprints** skill, which reaches the Director via `subobject:` syntax:
```
blueprint_query({ action: "get_blueprint_info", params: { asset_path: "/Game/Cinematics/LS_Intro.LS_Intro:LS_Intro_DirectorBP" } })
blueprint_query({ action: "get_graph_summary", params: { asset_path: "...:LS_Intro_DirectorBP", graph_name: "Sequencer Events" } })
```
`level_sequence_query` covers Sequencer-side metadata (bindings, event-track structure); the `blueprint` namespace covers BP graph-level details.

## Rules

- **Read-only namespace.** No write actions in this MVP — purely inspection.
- **Path format strict.** Pass the full object path returned by `ULevelSequence::GetPathName()` (e.g. `/Module/.../File.File`), not just `/Module/.../File`. Errors include a hint.
- **`list_bindings` ≠ `list_event_bindings`.** `list_bindings` is the full binding inventory of one LS regardless of event tracks (use it to see scene composition and spot modern custom bindings). `list_event_bindings` filters to event-bound rows and joins them to the Director functions they fire.
- **UE 5.7 binding kinds.** Modern custom bindings classify by their UCLASS hierarchy: `UMovieSceneSpawnableBindingBase` → `spawnable`, `UMovieSceneReplaceableBindingBase` → `replaceable`, anything else inheriting `UMovieSceneCustomBinding` → `custom`. Plain possessables remain `possessable`. The `custom_binding_class` field carries the exact UCLASS name when present.
- **`function_name` matching is exact and case-sensitive.** `Start` will not match `start_event`.
- **Glob conversion.** `*` → `%`, `?` → `_`. Single quotes are escaped automatically.
- **`kind` filter values for functions.** Use `event` as an alias when you want both `custom_event` (graph-defined `K2Node_CustomEvent`) and `sequencer_endpoint` (UE-generated `SequenceEvent__ENTRYPOINT*`). Use the precise values when you need to distinguish.
- **`fires_function_id` may be NULL** even when `fires_function_name` is set — this happens for empty trigger sections (no key-frames yet) and the rare cross-Director call. Check the `resolved` boolean in `find_director_function_callers` output.
- **Function counts include synthetic functions.** `function_count` on a Director includes user functions + graph CustomEvents + Sequencer-generated endpoints. Use `list_director_functions` with `kind` to slice.
- **Inherited base-class API is intentionally not indexed.** `ULevelSequenceDirector::OnCreated`, `GetBoundActor`, `GetCurrentTime`, etc. won't show up — this matches `blueprint.get_functions` convention.
