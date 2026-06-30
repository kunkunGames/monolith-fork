---
name: unreal-game-settings
description: Use when inspecting Unreal GameSettings plugin contracts via Monolith MCP — registry/screen/setting class readiness, static dynamic getter/setter path validation, GameSettingVisualData entry/detail-extension maps, and Enhanced Input player-mappable key-binding readiness. Read-only; does not create registries, mutate settings, call Apply/SaveChanges, or edit assets.
---

# Unreal Game Settings

Use the `settings` namespace for GameSettings diagnostics. These actions are read-only and avoid project-specific Lyra or Speed dependencies. GameSettings, CommonUI, CommonInput, and LyraGame contracts are inspected by reflection; the player-mappable input validator reads Enhanced Input assets through the public EnhancedInput API.

## Discovery

```js
monolith_discover({ namespace: "settings", mode: "actions" })
monolith_discover({ namespace: "settings", action: "validate_setting_class_contract", mode: "schema" })
monolith_discover({ namespace: "settings", action: "validate_player_mappable_input_settings", mode: "schema" })
```

## Action Reference

| Action | Params | Use |
| --- | --- | --- |
| `get_status` | none | Check `GameSettings` plugin/module availability, known reflected classes, and static data-source limitations. |
| `describe_registry_tree` | `screen_class?`, `registry_class?`, `setting_class?` | Describe reflected settings screen, registry, setting, and collection contracts without instantiating a runtime registry. |
| `validate_setting_class_contract` | `setting_class*`, `require_concrete?=false`, `require_value_setting?=false`, `require_collection?=false` | Validate a `UGameSetting` class path and optional concrete/value/collection role expectations. |
| `validate_data_source_bindings` | `getter_path?`, `setter_path?`, `dynamic_paths?`, `require_getter?=true`, `require_setter?=false` | Validate dotted dynamic getter/setter path shapes; runtime `ULocalPlayer` resolution is reported as out-of-scope for this read-only slice. |
| `validate_visual_data` | `asset_path*`, `require_entry_widgets?=false`, `require_detail_extensions?=false` | Inspect a `UGameSettingVisualData` object/asset and report entry-widget/detail-extension map counts. |
| `validate_player_mappable_input_settings` | `config_path?`, `config_paths?`, `context_path?`, `context_paths?`, semantic requirement toggles | Validate `UPlayerMappableInputConfig` and/or `UInputMappingContext` player-mappable rows for non-empty config names, contexts, mappable keys, valid actions/keys, display names, and duplicate mapping names. |

## Typical Flow

1. Run `settings.get_status` to confirm GameSettings classes are available.
2. Run `settings.describe_registry_tree` for the target screen/registry/setting class paths.
3. Run `settings.validate_setting_class_contract` for each setting class you plan to author or inspect.
4. Run `settings.validate_data_source_bindings` when dynamic getter/setter paths are known from source or a spec.
5. Run `settings.validate_player_mappable_input_settings` for Lyra-style key-binding settings backed by `UPlayerMappableInputConfig` or standalone `UInputMappingContext` assets.

This skill does not extract private `FGameSettingDataSourceDynamic::DynamicPath` memory. Supply explicit paths when static source scanning or authoring specs provide them.
