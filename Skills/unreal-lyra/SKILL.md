---
name: unreal-lyra
description: Use when inspecting, validating, or guarded-authoring Lyra framework assets via Monolith MCP (lyra namespace) - Lyra Experience graphs, map DefaultGameplayExperience, Experience defaults, idempotent Experience/ActionSet component-entry authoring and cleanup, PawnData/ActionSet composition, inventory/equipment/weapon definitions, team setup, cosmetics character parts, UserFacingExperience hosting/session metadata/map reachability, and GamePhase tag-domain validation. For raw GameFeatureData action authoring use unreal-gamefeatures; for UMG/CommonUI widgets use unreal-ui; for generic asset copy/remap use unreal-asset.
---

# unreal-lyra

Drives the **`lyra`** namespace via `lyra_query(action, params)` for reusable Lyra semantic inspection, validation, and guarded writes.

## Discovery

The live catalog is authoritative:

```text
monolith_discover({ namespace: "lyra" })
monolith_discover({ namespace: "lyra", action: "validate_experience_bundle", mode: "schema" })
```

## When To Use

Use this skill when the target is a Lyra-level contract rather than one raw asset primitive:

- Experience graph inspection: `DefaultPawnData`, `GameFeaturesToEnable`, instanced `Actions`, and composed `ActionSets`.
- Experience bundle validation before replacing a project commandlet, including optional PawnData, ActionSet, and GameFeature plugin contract checks.
- Guarded Experience writes: `DefaultPawnData`, replacement `ActionSets`, replacement `GameFeaturesToEnable`, and idempotent reflected `GameFeatureAction_AddComponents.ComponentList` add/update or cleanup on an Experience/ExperienceActionSet.
- UserFacingExperience validation before hosting through Lyra/CommonSession front-end flows.
- Map `ALyraWorldSettings.DefaultGameplayExperience` validation before relying on map-only fallback travel.
- UserFacingExperience `MapID` reachability validation and optional map-default Experience comparison; note that playlist hosting passes `ExperienceID` through URL options, so map default mismatches are only strict blockers when requested.
- Guarded UserFacingExperience writes for map/experience IDs, tile metadata, loading widget, max players, session mode, lobby, voice, and presence flags.
- GamePhase tag-domain inspection and `ULyraGamePhaseAbility.GamePhaseTag` coverage validation.
- Static CDO/reflection diagnostics for `ULyraPawnData`, inventory items, equipment definitions, weapon-like item chains, team creation defaults, and cosmetic character-part actor classes.

Use a different skill for:

- **unreal-gamefeatures**: writing `UGameFeatureData` actions such as AddWidgets, AddInputContextMapping, AddAbilities, and PrimaryAsset scan entries.
- **unreal-ui**: adding CommonUI widgets, UIExtension points, PrimaryGameLayout layers, and activatable widgets.
- **unreal-asset**: generic package copy/remap, asset save/delete/rename, or dependency closure work.
- **unreal-config**: `.ini` OnlineSubsystem/EOS settings.

## Action Reference

Param notation: `name*` required, `name?` optional, `name=val` default. Confirm schemas with live discovery before calling.

| Action | Params | Purpose |
| --- | --- | --- |
| `get_status` | none | Report reflected Lyra type availability and current/future action surface. |
| `describe_experience_graph` | `experience_path*` | Describe a Lyra Experience graph without mutation. |
| `validate_experience_bundle` | `experience_path*`, `require_default_pawn_data?=true`, `require_action_sets?=false`, expected members, `validate_default_pawn_data?=false`, `require_pawn_class?=true`, `require_pawn_ability_sets?=false`, `require_pawn_input_config?=false`, `require_default_camera_mode?=false`, `validate_action_sets?=true`, `require_action_set_actions?=false`, `disallow_null_actions?=true`, `validate_action_classes?=true`, `require_action_set_game_features?=false`, `validate_game_feature_plugins?=false` | Validate the Experience contract, optional PawnData internals, ActionSet action entries/classes, and GameFeature plugin descriptors. |
| `describe_user_facing_experience` | `user_facing_experience_path*` | Describe a UserFacingExperience hosting contract. |
| `validate_user_facing_experience` | `user_facing_experience_path*`, `require_resolved_primary_assets?=false` | Validate map/experience IDs, max players, lobby, voice, and presence consistency. |
| `validate_map_default_experience` | `map_path*`, `expected_experience_id?`, `require_default_experience?=true`, `require_lyra_world_settings?=true`, `require_matching_experience?=true` | Validate that a map loads, uses LyraWorldSettings when required, and has a resolvable `DefaultGameplayExperience` matching an optional expected LyraExperienceDefinition primary asset id. |
| `validate_user_facing_map_reachability` | `user_facing_experience_path*`, `require_resolved_primary_assets?=false`, `require_map_default_experience?=false`, `require_lyra_world_settings?=true`, `require_matching_map_default_experience?=false` | Validate that a UserFacingExperience `MapID` resolves to a map and optionally compare its map fallback `DefaultGameplayExperience` to the playlist `ExperienceID`. |
| `describe_gameplay_tag_domain` | `root_tag?="GamePhase"`, `include_children?=true`, `max_tags?=256` | Describe a GameplayTag root domain, source metadata, child tags, comments, and truncation status. |
| `validate_game_phase_flow` | `root_tag?="GamePhase"`, `path_filter?`, `phase_ability_paths?`, `expected_phase_tags?`, `disallow_duplicate_tags?=false`, `max_assets?=256` | Validate reflected Lyra game phase ability classes and their `GamePhaseTag` values against the registered tag domain and expected tag coverage; `path_filter` enables Blueprint asset scanning. |
| `describe_team_setup` | `team_creation_component_class?="/Script/LyraGame.LyraTeamCreationComponent"` | Describe reflected Lyra team creation defaults without spawning teams. |
| `describe_inventory_item` | `item_definition_path*` | Describe `ULyraInventoryItemDefinition` display text and instanced fragments. |
| `describe_equipment_definition` | `equipment_definition_path*` | Describe `ULyraEquipmentDefinition` instance type, granted ability sets, and spawned actors. |
| `describe_weapon_definition` | `item_definition_path*`, `require_equippable_fragment?=true` | Follow a weapon-like inventory item through its equippable fragment to equipment and weapon instance compatibility. |
| `describe_pawn_initialization_graph` | `pawn_data_path*` | Describe `ULyraPawnData` pawn class, ability sets, tag mapping, input config, and camera mode. |
| `validate_pawn_data_contract` | `pawn_data_path*`, `require_pawn_class?=true`, `require_ability_sets?=false`, `require_input_config?=false`, `require_default_camera_mode?=false`, `expected_pawn_class?` | Validate required PawnData fields and optional expected pawn-class derivation. |
| `describe_character_part_graph` | `part_classes?` | Describe Lyra character-part component/settings reflected classes and optional actor part classes. |
| `validate_character_part_assets` | `part_classes?`, `require_non_empty?=true` | Validate supplied character-part actor classes as loadable, concrete `AActor` classes. |
| `set_experience_defaults` | `experience_path*`, `default_pawn_data?`, `action_sets?`, `game_features_to_enable?`, `dry_run?=false`, `confirm?=false`, `save?=false`, `strict?=true` | Guarded reflected write for Lyra Experience defaults. |
| `add_experience_component_entry` | exactly one of `experience_path?` or `action_set_path?`, `actor_class*`, `component_class*`, `action_name?`, `client_component?=true`, `server_component?=true`, `addition_flags?=0`, `dry_run?=false`, `confirm?=false`, `save?=false` | Idempotently create/reuse an AddComponents action, add a missing actor/component pair, or update its client/server/flags fields. A supplied `action_name` is exact; unique naming occurs only when it is omitted. |
| `remove_experience_component_entry` | `experience_path?`, `action_set_path?`, `action_index?`, `action_name?`, `actor_class?`, `component_class?`, `component_index?`, `dry_run?=false`, `confirm?=false`, `save?=false` | Guarded removal of `GameFeatureAction_AddComponents.ComponentList` entries. |
| `set_user_facing_experience` | `user_facing_experience_path*`, `map_id?`, `experience_id?`, `extra_args?`, `tile_title?`, `tile_subtitle?`, `tile_description?`, `tile_icon?`, `loading_screen_widget?`, `is_default_experience?`, `show_in_front_end?`, `record_replay?`, `max_player_count?`, `session_mode?`, `use_lobbies?`, `use_lobbies_voice_chat?`, `use_presence?`, `dry_run?=false`, `confirm?=false`, `save?=false`, `strict?=true` | Guarded reflected write for UserFacingExperience hosting/session/UI fields. |

## Common Workflows

### Validate an Experience before replacing a commandlet

```text
lyra_query("describe_experience_graph", {
  "experience_path": "/SpeedCore/TagChase/Experiences/B_TagChaseExperience"
})

lyra_query("validate_experience_bundle", {
  "experience_path": "/SpeedCore/TagChase/Experiences/B_TagChaseExperience",
  "expected_pawn_data": "/SpeedCore/TagChase/PawnData/DA_TagChasePawnData",
  "expected_action_sets": ["/SpeedCore/TagChase/ActionSets/LAS_TagChase_Gameplay"],
  "expected_game_features": ["SpeedCore"],
  "validate_default_pawn_data": true,
  "require_pawn_input_config": true,
  "require_pawn_ability_sets": true,
  "validate_action_sets": true,
  "validate_game_feature_plugins": true
})
```

### Validate a front-end hosting tile

```text
lyra_query("validate_user_facing_experience", {
  "user_facing_experience_path": "/SpeedMaps/System/FrontEnd/Experiences/DA_DefaultUserFacingExperience",
  "require_resolved_primary_assets": true
})

lyra_query("validate_user_facing_map_reachability", {
  "user_facing_experience_path": "/SpeedMaps/System/Playlists/DA_SpeedDefault",
  "require_resolved_primary_assets": true,
  "require_matching_map_default_experience": false
})
```

### Validate a map-only fallback experience

```text
lyra_query("validate_map_default_experience", {
  "map_path": "Map:/SpeedMaps/Maps/L_Playground",
  "expected_experience_id": "LyraExperienceDefinition:B_TagChase_Experience"
})
```

### Validate Lyra GamePhase tags

```text
lyra_query("describe_gameplay_tag_domain", {
  "root_tag": "GamePhase",
  "include_children": true
})

lyra_query("validate_game_phase_flow", {
  "root_tag": "GamePhase",
  "path_filter": "/SpeedCore",
  "expected_phase_tags": [
    "GamePhase.TagChase.RoleSelection",
    "GamePhase.TagChase.HeadStart",
    "GamePhase.TagChase.Chase",
    "GamePhase.TagChase.Results",
    "GamePhase.TagChase.ReturnToLobby"
  ]
})
```

### Inspect PawnData and weapon definitions

```text
lyra_query("describe_pawn_initialization_graph", {
  "pawn_data_path": "/SpeedCore/TagChase/PawnData/DA_TagChasePawnData"
})

lyra_query("validate_pawn_data_contract", {
  "pawn_data_path": "/SpeedCore/TagChase/PawnData/DA_TagChasePawnData",
  "require_pawn_class": true,
  "require_input_config": true
})

lyra_query("describe_weapon_definition", {
  "item_definition_path": "/ShooterCore/Weapons/Rifle/ID_Rifle"
})
```

### Inspect teams and cosmetics without runtime spawn

```text
lyra_query("describe_team_setup", {})

lyra_query("validate_character_part_assets", {
  "part_classes": [
    "/Game/Characters/Heroes/Parts/B_HelmetPart.B_HelmetPart_C"
  ]
})
```

### Preview Experience default writes

```text
lyra_query("set_experience_defaults", {
  "experience_path": "/SpeedCore/TagChase/Experiences/B_TagChaseExperience",
  "default_pawn_data": "/SpeedCore/TagChase/PawnData/DA_TagChasePawnData.DA_TagChasePawnData",
  "action_sets": ["/SpeedCore/TagChase/ActionSets/LAS_TagChase_Gameplay.LAS_TagChase_Gameplay"],
  "dry_run": true
})
```

### Preview an ExperienceActionSet component entry

Use `server_component=true` and `client_component=false` for a server-authoritative spawning manager. The dry-run reports whether the action would be created/reused and whether the pair would be added, updated, or already be a no-op.

```text
lyra_query("add_experience_component_entry", {
  "action_set_path": "/SpeedCore/TagChase/ActionSets/LAS_TagChase_Gameplay",
  "actor_class": "/Script/LyraGame.LyraGameState",
  "component_class": "/Script/SpeedCoreRuntime.SPDTagChasePlayerSpawningManagerComponent",
  "client_component": false,
  "server_component": true,
  "addition_flags": 0,
  "dry_run": true
})
```

### Preview a UserFacingExperience hosting update

```text
lyra_query("set_user_facing_experience", {
  "user_facing_experience_path": "/SpeedMaps/System/Playlists/DA_TagChase.DA_TagChase",
  "map_id": "Map:/SpeedMaps/Maps/L_Playground",
  "experience_id": "LyraExperienceDefinition:B_TagChase_Experience",
  "session_mode": "LAN",
  "max_player_count": 16,
  "use_lobbies": false,
  "dry_run": true
})
```

## Rules

- Write actions must be run with `dry_run=true` first. Mutating calls require `confirm=true`; package persistence additionally requires `save=true`.
- `add_experience_component_entry` requires exactly one of `experience_path` or `action_set_path`. It treats actor/component class paths as one idempotency key and fails explicitly when duplicate matching pairs make ownership ambiguous.
- A supplied `action_name` is an exact instanced-object `FName`: the action never appends `_0` or another suffix. Dry-run and commit both fail when the owner already has an incompatible same-name direct child or a same-name AddComponents orphan outside its `Actions` array. Omit `action_name` when automatic `MakeUniqueObjectName` behavior is desired.
- Do not patch runtime CommonGame, PrimaryGameLayout, or project commandlets to compensate for missing Lyra actions. Extend `Plugins/Monolith` instead.
- Missing or invalid data should produce explicit checks/errors. Do not mask it with fallback assets.
- GamePhase diagnostics are read-only. They may report duplicate exact phase tags as warnings unless `disallow_duplicate_tags=true`; Lyra runtime permits shared exact phase tags.
- `validate_game_phase_flow` does not scan every Blueprint by default; pass a narrow `path_filter` such as `/SpeedCore` when Blueprint phase ability discovery is needed.
- `validate_experience_bundle` deep checks are read-only. They inspect referenced PawnData and ActionSet objects, validate action classes against `UGameFeatureAction`, and optionally report plugin descriptor presence/enabled state without activating GameFeatures.
- UserFacingExperience diagnostics must not inspect, print, or infer EOS credential values.
- `validate_user_facing_map_reachability` reports map default experience mismatches as warnings unless `require_matching_map_default_experience=true`, because Lyra playlist hosting passes `ExperienceID` as a URL option before `ALyraWorldSettings` fallback is consulted.
- Static PawnData, inventory/equipment/weapon, team, and character-part inspectors are read-only. They inspect reflected CDO/default data only; they do not spawn teams, equip items, initialize pawns, apply cosmetics, or mutate packages.
