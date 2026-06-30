# SPEC_MonolithGameSettings

| Field | Value |
| --- | --- |
| Module | `MonolithGameSettings` |
| Namespace | `settings` |
| Type | Editor |
| Status | Current |

---

## 1. Purpose

`MonolithGameSettings` provides reusable read-only diagnostics for Epic's `GameSettings` plugin. It reports reflected registry/screen/setting/visual-data contracts, validates static dynamic-path inputs without linking against the optional GameSettings runtime plugin, and validates Enhanced Input player-mappable key-binding asset semantics without touching project-specific Lyra classes.

---

## 2. Ownership

| Area | Owner |
| --- | --- |
| GameSettings plugin/module/class availability | `settings.get_status` |
| Registry tree class contract | `settings.describe_registry_tree` |
| Setting class validation | `settings.validate_setting_class_contract` |
| Dynamic getter/setter path shape validation | `settings.validate_data_source_bindings` |
| Visual-data asset validation | `settings.validate_visual_data` |
| Player-mappable input setting validation | `settings.validate_player_mappable_input_settings` |

---

## 3. Actions

| Action | Params | Behavior |
| --- | --- | --- |
| `settings.get_status` | none | Reports `GameSettings` plugin/module availability, known core classes, and static data-source limitation notes. |
| `settings.describe_registry_tree` | optional `screen_class`, optional `registry_class`, optional `setting_class` | Describes reflected `UGameSettingScreen`, `UGameSettingRegistry`, `UGameSetting`, and `UGameSettingCollection` contracts without creating a runtime registry. |
| `settings.validate_setting_class_contract` | `setting_class`, optional `require_concrete=false`, `require_value_setting=false`, `require_collection=false` | Validates `UGameSetting` parentage, abstract/deprecated status, and requested value/collection roles. |
| `settings.validate_data_source_bindings` | optional `getter_path`, `setter_path`, `dynamic_paths`, `require_getter=true`, `require_setter=false` | Validates dotted dynamic-path shapes and reports that `FGameSettingDataSourceDynamic` resolves against a `ULocalPlayer` at runtime. |
| `settings.validate_visual_data` | `asset_path`, optional `require_entry_widgets=false`, `require_detail_extensions=false` | Loads a `UGameSettingVisualData` asset/object read-only and reports entry-widget/detail-extension map counts. |
| `settings.validate_player_mappable_input_settings` | optional `config_path`, `config_paths`, `context_path`, `context_paths`, optional semantic requirement toggles | Loads `UPlayerMappableInputConfig` and/or `UInputMappingContext` assets read-only and validates the Lyra/GameSettings key-binding contract: non-empty config names, contexts, player-mappable rows, valid actions/keys, display names, and duplicate mapping names per default/profile scope. |

---

## 4. Constraints

| Constraint | Requirement |
| --- | --- |
| Runtime isolation | Do not add a compile-time dependency on `GameSettings`, `CommonUI`, `CommonInput`, or `LyraGame`; inspect those contracts by reflection only. `EnhancedInput` is an allowed compile-time dependency for public input asset APIs used by `settings.validate_player_mappable_input_settings`. |
| Mutability | Actions are read-only and must not create registries, instantiate local players, call setters, call `Apply`, call `SaveChanges`, or save assets. |
| Data-source limits | `FGameSettingDataSourceDynamic::DynamicPath` is private/non-UPROPERTY, so first-slice validation is shape/static only unless a caller supplies explicit paths. |
| Registry tree limits | `UGameSettingRegistry::TopLevelSettings` and `RegisteredSettings` are protected; this module describes contracts and class compatibility rather than layout hacking live registry internals. |
| Player-mappable limits | The validator reads authored config/context assets and profile overrides. It does not create a `ULocalPlayer`, register `FMappableConfigPair`, query current custom user bindings, or build a live GameSettings registry tree. |

---

## 5. Verification

| Check | Required result |
| --- | --- |
| Build | `SpeedEditor Win64 Development` compiles `UnrealEditor-MonolithGameSettings.dll` via the engine resolver from `Speed.uproject`. |
| Automation | `Monolith.GameSettings.RegistryAndValidation` passes with zero warnings and zero errors. |
| Drift guard | `Scripts/check_skill_catalog_drift.ps1 -Skill unreal-game-settings` reports `RESULT=OK`. |
