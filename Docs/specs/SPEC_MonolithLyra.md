# Monolith - MonolithLyra Module

**Parent:** [SPEC_CORE.md](../SPEC_CORE.md)
**Engine:** Unreal Engine 5.7+
**Version:** 0.20.3
**Owner module:** MonolithLyra
**Namespace:** `lyra`
**Status:** Implemented semantic validation and guarded write slice

---

## 1. Purpose

`MonolithLyra` owns reusable Lyra semantic inspection, validation, and guarded authoring that is higher level than raw `gamefeatures`, `ui`, `asset`, or `editor` primitives. The current implementation slice covers Lyra Experience and UserFacingExperience contracts that were previously handled by project commandlets, guarded idempotent `GameFeatureAction_AddComponents.ComponentList` authoring and cleanup on Experiences/ExperienceActionSets, read-only map `ALyraWorldSettings.DefaultGameplayExperience` and playlist-to-map reachability validation, read-only deep Experience bundle validation, read-only Lyra GamePhase tag/domain validation, and read-only P2 inspectors for PawnData, inventory/equipment/weapon definitions, team setup, and cosmetic character parts.

The module does not include Lyra headers and does not hard-link `LyraGame`. It resolves `/Script/LyraGame.*` types reflectively, so non-Lyra projects can still load Monolith and receive explicit unavailable-class diagnostics when calling `lyra` actions.

---

## 2. Ownership

| Class | Responsibility |
| --- | --- |
| `FMonolithLyraModule` | Registers and unregisters the `lyra` namespace. |
| `FMonolithLyraActions` | Implements status, Experience graph inspection/validation, map default Experience validation, UserFacingExperience map reachability validation, guarded Experience default writes, guarded idempotent component-entry add/update and removal, UserFacingExperience inspection/validation, read-only GamePhase tag/domain validation, read-only PawnData/inventory/equipment/weapon/team/cosmetic inspectors, and guarded UserFacingExperience writes. |

| Dependency | Purpose |
| --- | --- |
| `MonolithCore` | Tool registry, action result, parameter schemas, reflection reader, dry-run report JSON, and bulk-fill registry. |
| `AssetRegistry` | Asset metadata support for future Lyra graph expansion. |
| `GameplayTags` | Read-only `GamePhase` tag-domain inspection and `GamePhaseTag` validation. |
| `Projects` | Optional GameFeature plugin descriptor lookup for deep Experience bundle validation. |
| `Engine`, `UnrealEd` | Editor-only UObject loading, `UAssetManager`, `UPrimaryDataAsset`, and reflection. |
| `Json`, `JsonUtilities` | Action response payloads. |

---

## 3. Action Surface

| Action | Params | Description |
| --- | --- | --- |
| `lyra.get_status` | none | Reports reflected Lyra class availability and current/future `lyra` actions. |
| `lyra.describe_experience_graph` | `experience_path` | Describes `GameFeaturesToEnable`, `DefaultPawnData`, instanced `Actions`, and composed `ActionSets`. |
| `lyra.validate_experience_bundle` | `experience_path`, optional expected members, `validate_default_pawn_data`, `require_pawn_class`, `require_pawn_ability_sets`, `require_pawn_input_config`, `require_default_camera_mode`, `validate_action_sets`, `require_action_set_actions`, `disallow_null_actions`, `validate_action_classes`, `require_action_set_game_features`, `validate_game_feature_plugins` | Validates the Experience bundle contract, optional PawnData internals, composed ActionSet action arrays, and GameFeature plugin descriptor readiness; returns exact check rows. |
| `lyra.describe_user_facing_experience` | `user_facing_experience_path` | Describes `MapID`, `ExperienceID`, UI metadata, loading widget, max players, and session flags. |
| `lyra.validate_user_facing_experience` | `user_facing_experience_path`, optional `require_resolved_primary_assets` | Validates the hosting contract for map, experience, max players, lobby, voice, and presence fields. |
| `lyra.validate_map_default_experience` | `map_path`, optional `expected_experience_id`, `require_default_experience`, `require_lyra_world_settings`, `require_matching_experience` | Loads a map `UWorld`, verifies optional `ALyraWorldSettings` usage, reads `DefaultGameplayExperience` by reflection, resolves it to a `LyraExperienceDefinition` primary asset id, and optionally compares it to an expected id. |
| `lyra.validate_user_facing_map_reachability` | `user_facing_experience_path`, optional `require_resolved_primary_assets`, `require_map_default_experience`, `require_lyra_world_settings`, `require_matching_map_default_experience` | Validates that a UserFacingExperience `MapID` resolves to a loadable map and reports the map's fallback `DefaultGameplayExperience`; map-default mismatch is a warning by default because playlist hosting sends `ExperienceID` through URL options before WorldSettings fallback. |
| `lyra.describe_gameplay_tag_domain` | optional `root_tag`, `include_children`, `max_tags` | Describes a GameplayTag domain such as `GamePhase`, including root registration, source metadata, child tags, comments, and truncation status. |
| `lyra.validate_game_phase_flow` | optional `root_tag`, `path_filter`, `phase_ability_paths`, `expected_phase_tags`, `disallow_duplicate_tags`, `max_assets` | Validates reflected `ULyraGamePhaseAbility` classes and their `GamePhaseTag` values against the registered tag domain, expected tags, duplicate tag policy, and missing/invalid class paths. |
| `lyra.describe_team_setup` | optional `team_creation_component_class` | Describes a `ULyraTeamCreationComponent` CDO: `TeamsToCreate`, `PublicTeamInfoClass`, and `PrivateTeamInfoClass` without spawning teams. |
| `lyra.describe_inventory_item` | `item_definition_path` | Describes a `ULyraInventoryItemDefinition` class/CDO, `DisplayName`, and instanced fragment rows with common fragment properties. |
| `lyra.describe_equipment_definition` | `equipment_definition_path` | Describes a `ULyraEquipmentDefinition` class/CDO, `InstanceType`, `AbilitySetsToGrant`, and `ActorsToSpawn`. |
| `lyra.describe_weapon_definition` | `item_definition_path`, optional `require_equippable_fragment` | Follows a weapon-like inventory item through `InventoryFragment_EquippableItem.EquipmentDefinition`, describes the equipment definition, and checks whether its `InstanceType` derives from `ULyraWeaponInstance`. |
| `lyra.describe_pawn_initialization_graph` | `pawn_data_path` | Describes a `ULyraPawnData` asset graph: `PawnClass`, `AbilitySets`, `TagRelationshipMapping`, `InputConfig`, and `DefaultCameraMode`. |
| `lyra.validate_pawn_data_contract` | `pawn_data_path`, optional `require_pawn_class`, `require_ability_sets`, `require_input_config`, `require_default_camera_mode`, `expected_pawn_class` | Validates `ULyraPawnData` required fields and optional expected pawn-class derivation, returning check rows and the graph payload. |
| `lyra.describe_character_part_graph` | optional `part_classes` | Describes Lyra character-part component/settings reflected classes and optional character-part actor class paths without spawning cosmetics. |
| `lyra.validate_character_part_assets` | optional `part_classes`, `require_non_empty` | Validates supplied character-part actor class paths as loadable, concrete `AActor` classes. |
| `lyra.set_experience_defaults` | `experience_path`, optional `default_pawn_data`, `action_sets`, `game_features_to_enable`, `dry_run`, `confirm`, `save`, `strict` | Guarded reflected write for `DefaultPawnData`, replacement `ActionSets`, and replacement `GameFeaturesToEnable`; routes mutation through the registered `blueprint` bulk-fill adapter. |
| `lyra.add_experience_component_entry` | exactly one of `experience_path` or `action_set_path`; required `actor_class`, `component_class`; optional `action_name`, `client_component`, `server_component`, `addition_flags`, `dry_run`, `confirm`, `save` | Guarded idempotent authoring for one `GameFeatureAction_AddComponents.ComponentList` actor/component pair. Reuses an existing compatible action (preferring the action that already owns the pair), creates one when absent, adds a missing pair, or updates only client/server/addition flags for an existing pair. A supplied `action_name` is the exact instanced-object `FName`; automatic unique naming is used only when the parameter is omitted. |
| `lyra.remove_experience_component_entry` | optional `experience_path`, `action_set_path`, `action_index`, `action_name`, `actor_class`, `component_class`, `component_index`, plus `dry_run`, `confirm`, `save` | Guarded reflected removal for `GameFeatureAction_AddComponents.ComponentList` entries on an Experience or composed ActionSet. Requires at least one component selector. |
| `lyra.set_user_facing_experience` | `user_facing_experience_path`, optional `map_id`, `experience_id`, `extra_args`, tile fields, loading widget, session flags, `dry_run`, `confirm`, `save`, `strict` | Guarded reflected write for UserFacingExperience hosting/session/UI fields; validates `MapID` and `ExperienceID` primary asset types before dispatch. |

---

## 4. Safety Contract

| Gate | Requirement |
| --- | --- |
| Monolith-only scope | No runtime `CommonGame`, `PrimaryGameLayout`, or Speed gameplay code changes are required. |
| Optional dependency | No `LyraGame` include or Build.cs dependency. Lyra types are resolved by `/Script/LyraGame.*` class path. |
| Write gate | Mutating actions require `dry_run=true` or `confirm=true`. Without either flag the action fails before asset load or package creation. |
| AddComponents authoring | `add_experience_component_entry` requires exactly one owner target, validates `actor_class` as an `AActor` subclass and `component_class` as a `UActorComponent` subclass, requires at least one of client/server, and bounds `addition_flags` to `uint8`. Duplicate matching pairs are explicit errors instead of creating another entry. |
| Explicit action naming | When `action_name` is supplied, the action reuses or creates that exact `FName` without adding a numeric suffix. A same-name direct child that is incompatible or orphaned from the owner's `Actions` array is an explicit `-32602` error during dry-run and commit. `MakeUniqueObjectName` is used only when `action_name` is omitted. |
| Dry-run behavior | `dry_run=true` dispatches inspection/planning only and reports action create/reuse plus component add/update intent without changing CDOs, DataAssets, arrays, dirty state, or packages. |
| GamePhase diagnostics | `describe_gameplay_tag_domain` and `validate_game_phase_flow` are read-only and only inspect registered tags, loaded native classes, explicit class/asset paths, and Blueprint CDOs under caller-provided `path_filter`. |
| Map/playlist diagnostics | `validate_map_default_experience` and `validate_user_facing_map_reachability` are read-only. They load map assets and DataAssets for inspection only, do not save or dirty packages, and do not treat map default Experience mismatch as a playlist-hosting blocker unless the caller sets `require_matching_map_default_experience=true`. |
| Deep Experience diagnostics | `validate_experience_bundle` can inspect referenced PawnData and ActionSets through reflected object/default data, count null action entries, validate action classes against `UGameFeatureAction`, and optionally report GameFeature plugin descriptor presence/enabled state. It does not activate GameFeatures or mutate packages. |
| P2 Lyra inspectors | `describe_team_setup`, `describe_inventory_item`, `describe_equipment_definition`, `describe_weapon_definition`, `describe_pawn_initialization_graph`, `validate_pawn_data_contract`, `describe_character_part_graph`, and `validate_character_part_assets` are read-only. They inspect class default objects, reflected UPROPERTY values, and caller-supplied class paths; they do not spawn actors, create teams, equip items, initialize pawns, or apply cosmetics. |
| Save behavior | `save=true` only saves after `dry_run=false`, `confirm=true`, and a clean committed report, add/update, or removal. No-op idempotent calls do not dirty or save packages. |
| Secret handling | UserFacingExperience validation does not inspect or print EOS credentials. |
| Error quality | Missing assets/properties are returned as explicit errors or check rows; no silent fallback asset or default value is invented. |

---

## 5. Verification

| Gate | Evidence |
| --- | --- |
| Registration | `Monolith.Lyra.RegistryContract` verifies all twenty-one actions register. |
| Parameter guards | The registry contract test verifies missing required parameters return `-32602`, deep Experience bundle validation returns structured check rows and flag echoes, map/default-experience validators return structured bad-path payloads, GamePhase and character-part validators reject malformed params, read-only inspectors return structured payloads, guarded write actions reject mutation without `dry_run=true` or `confirm=true`, and component-entry authoring rejects ambiguous targets, invalid network-side/flag values, and incompatible actor/component classes before loading the target asset. |
| Action-name contract | `Monolith.Lyra.AddExperienceComponentEntry.ActionNameContract` executes the registered action against a transient ExperienceActionSet and verifies exact-name creation without a suffix, identical-request no-op reuse, and dry-run rejection of an incompatible same-name direct-child orphan. |
| Build | `SpeedEditor Win64 Development` UBT build must succeed with engine root resolved from `Speed.uproject`. |
| Runtime code scope | No files under `Source/LyraGame`, `Source/LyraEditor`, or `Plugins/CommonGame` are changed by this module. |

---

## 6. Deferred Slices

| Candidate | Reason deferred |
| --- | --- |
| `gameplay_message.*` namespace | GameplayMessage broadcaster/listener/payload graph extraction belongs in a separate reusable namespace. |
| Dynamic phase transition graph extraction | The current `lyra.validate_game_phase_flow` validates phase ability tags and expected coverage; deeper source/Blueprint control-flow extraction remains separate from the first read-only GamePhase slice. |
| Runtime inventory/equipment/pawn/team/cosmetic state tracing | The P2 inspectors are static CDO/reflection diagnostics. Runtime equip state, initialized pawn components, actual team assignment, and spawned cosmetic actor tracing remain separate PIE/runtime slices. |
