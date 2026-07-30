---
name: unreal-gamefeatures
description: Use when inspecting Game Feature plugins and modular gameplay or authoring guarded GameFeatureAction entries via Monolith MCP (gamefeatures namespace) - list/find GameFeature plugins, describe GameFeatureData/ActionSet assets and reflected feature actions, validate plugin descriptors, discover GameFeatureAction classes, and add/set/remove ActionSet/GameFeatureData instanced actions for input mapping, primary asset scanning, widgets, components, GameplayCue paths, and Lyra ability grants. For the gameplay logic/components a feature ADDS use unreal-blueprints; for .uplugin descriptors or .ini/cvar gates use unreal-config; for GAS ability/effect asset authoring use unreal-gas. Triggers on game feature, game features, modular gameplay, GameFeatureData, GFP, game feature plugin, game feature action, feature action, GameFeatureAction, input mapping action, validate plugin descriptor, what plugins are active.
---

# unreal-gamefeatures

Drives the **`gamefeatures`** namespace via `gamefeatures_query(action, params)`: bounded inspection of GameFeature plugins/GameFeatureData/ActionSet assets plus guarded instanced-action authoring. The 15 actions below are a snapshot — discover first so you never call a stale or guessed name.

## Discovery

```
monolith_discover({ namespace: "gamefeatures" })                      # all actions in this namespace
monolith_discover({ namespace: "gamefeatures", action: "<action>", mode: "schema" })  # exact params
```

## When to use / Use a different skill for

Use this skill to inspect GameFeature plugins and modular gameplay — listing/finding plugins, summarizing GameFeatureData or ActionSet assets and their bounded feature-action reflection, validating a plugin descriptor or creation gate, discovering loaded `UGameFeatureAction` subclasses, and adding/updating/removing guarded instanced feature actions on existing ActionSet/GameFeatureData assets.

Use a different skill for:

- **unreal-blueprints** — when editing the actual gameplay logic, components, or actor graphs a Game Feature ADDS, rather than inspecting the feature plugin itself.
- **unreal-config** — when editing the plugin descriptor (`.uplugin`) or the `.ini`/cvar settings that gate a Game Feature, rather than reading its inspection state.
- **unreal-gas** — when creating or editing GAS ability/effect/attribute assets. Use this skill only to attach already-authored ability-related assets to a `GameFeatureData` through AddAbilities-style action entries.

## Action Reference

Param notation: `name*` required, `name?` optional, `name=val` default, `a/b/c` allowed, `[w]` mutates. Signatures are a snapshot of the live catalog — for the exact full schema call `monolith_discover` with `mode: "schema"`. The discover-first block above is the authority.

> Gate note: `get_status` plus the eight `[w]` writer actions are always registered. The six inspection actions require `bEnableGameFeatureActions` to be registered (see "Limited surface" and Gotchas). The signatures below are transcribed from the `gamefeatures` `RegisterAction` / `FParamSchemaBuilder` calls in `MonolithGameFeatures/Private/MonolithGameFeatureActions.cpp`.

### Game Feature (15)

| Action | Params | Purpose |
|--------|--------|---------|
| `get_status` | _(none)_ | Report GameFeatures inspection availability, write-action registration, flags, module status, and discovered plugin count. |
| `list_plugins` | `limit?=50` `include_engine?=false` | List GameFeature-style plugins from plugin descriptors and AssetRegistry metadata. Read-only; no activation or file writes. |
| `find_game_feature_data` | `plugin_name?` `asset_path?` | Resolve a GameFeature plugin name or asset path to bounded GameFeatureData AssetRegistry metadata without loading arbitrary paths. |
| `describe_game_feature_data` | `plugin_name?` `asset_path?` `include_action_properties?=true` `editable_only?=true` `include_values?=true` `property_limit?=40` `max_value_chars?=512` | Load and summarize one GameFeatureData asset, including bounded reflected feature-action summaries and optional editable property values. |
| `list_action_classes` | `limit?=100` `module?` `name_contains?` `include_abstract?=false` `editable_only?=true` `include_default_values?=false` `property_limit?=40` `max_value_chars?=512` | List loaded `UGameFeatureAction` subclasses and bounded editable reflected properties for authoring discovery. |
| `describe_action_set` | `action_set_path*` `action_limit?=50` `include_action_properties?=true` `editable_only?=true` `include_values?=true` `property_limit?=40` `max_value_chars?=512` | Load an ActionSet-style asset and summarize its instanced `Actions` array. |
| `[w] add_action_set_input_mapping` | `action_set_path*` `mapping_context_path*` `action_class_path?=/Script/LyraGame.GameFeatureAction_AddInputContextMapping` `priority?=0` `action_name?` `remove_null_actions?=true` `save?=true` `dry_run?=false` | Create or update an instanced Add Input Mapping-style action on an existing ActionSet asset and add the requested InputMappingContext idempotently. |
| `[w] set_primary_asset_scan` | `game_feature_data_path*` `primary_asset_type*` `asset_base_class?=/Script/CoreUObject.Object` `has_blueprint_classes?` `is_editor_only?` `directories?` `specific_assets?` `save?=true` `dry_run?=false` | Create or update one `PrimaryAssetTypesToScan` entry on an existing GameFeatureData asset. |
| `[w] add_game_feature_data_input_mapping` | `game_feature_data_path*` `mapping_context_path*` `action_class_path?=/Script/LyraGame.GameFeatureAction_AddInputContextMapping` `priority?=0` `action_name?` `remove_null_actions?=true` `save?=true` `dry_run?=false` | Create or update an Add Input Mapping-style action directly on GameFeatureData and add the requested InputMappingContext entry. |
| `[w] add_game_feature_data_widgets` | `game_feature_data_path*` `layouts?` `widgets?` `action_class_path?=/Script/LyraGame.GameFeatureAction_AddWidgets` `action_name?` property-name overrides? `remove_null_actions?=true` `save?=true` `dry_run?=false` | Create or update an AddWidgets-style action directly on GameFeatureData and add layout/widget class plus GameplayTag entries. |
| `[w] add_game_feature_data_components` | `game_feature_data_path*` `actor_class*` `component_class*` `action_class_path?=/Script/GameFeatures.GameFeatureAction_AddComponents` `action_name?` `client_component?=true` `server_component?=true` `addition_flags?=0` `remove_null_actions?=true` `save?=true` `dry_run?=false` | Create or update an AddComponents action directly on GameFeatureData and add one actor/component request entry. |
| `[w] add_game_feature_data_gameplay_cue_paths` | `game_feature_data_path*` `directory_path?` `directory_paths?` `action_class_path?=/Script/LyraGame.GameFeatureAction_AddGameplayCuePath` `action_name?` `directory_array_property?=DirectoryPathsToAdd` `remove_null_actions?=true` `save?=true` `dry_run?=false` | Create or update an AddGameplayCuePath-style action directly on GameFeatureData and add GameplayCue directory path entries. |
| `[w] add_game_feature_data_abilities` | `game_feature_data_path*` `actor_class*` `ability_classes?` `attribute_sets?` `ability_sets?` `action_class_path?=/Script/LyraGame.GameFeatureAction_AddAbilities` `action_name?` `remove_null_actions?=true` `save?=true` `dry_run?=false` | Create or update an AddAbilities-style action directly on GameFeatureData and add actor-scoped ability, attribute-set, and ability-set grants. |
| `[w] remove_game_feature_data_action` | `game_feature_data_path*` `action_index?` `action_name?` `action_class_path?` `remove_all?=false` `save?=true` `dry_run?=false` | Remove one or more existing GameFeatureData `Actions` entries by index, instanced object name, and/or action class. |
| `validate_plugin` | `plugin_name*` | Validate a plugin descriptor, content root, GameFeatureData asset, and creation gate state. Read-only. |

`find_game_feature_data` and `describe_game_feature_data` mark both params optional in the schema, but the handler requires at least one of `plugin_name` or `asset_path` and errors otherwise; `asset_path` accepts either a package path or an object path. Plugin-name resolution uses a descriptor-declared data asset or one unique indexed candidate; multiple unmatched candidates require `asset_path`. `list_plugins` `limit` is clamped to 1..200.
All writer actions support `dry_run=true`; use it before applying when editing shared assets. They require existing assets of the expected shape and preflight the complete edit on a transient action copy before committing. Rejected calls do not attach actions or dirty packages.

## Common Workflows

**Flag-gated surface:** `get_status` and the eight writer actions are always registered. The six inspection actions require `bEnableGameFeatureActions` to be registered — start every recipe with `get_status` to learn inspection availability, write-action availability, module status, and discovered plugin count; if `enabled` is false the inspection actions are listed under `available_when_enabled` and not registered. All steps use only Action Reference actions with their real params.

### Add an ActionSet InputMappingContext

```
1. gamefeatures_query({ action: "add_action_set_input_mapping", params: {
     "action_set_path": "/SpeedCore/TagChase/ActionSets/LAS_TagChase_Gameplay",
     "mapping_context_path": "/Game/Input/Mappings/IMC_Default",
     "priority": 0,
     "dry_run": true
   }})
2. Re-run with "dry_run": false when the result reports the intended created_action/added_mapping/updated_priority plan.
3. Inspect with project/export or gamefeatures describe actions as appropriate.
```

### Discover available GameFeatureAction classes before authoring

```
1. gamefeatures_query({ action: "get_status", params: {} })
2. gamefeatures_query({ action: "list_action_classes", params: {
     "module": "LyraGame",
     "editable_only": true,
     "include_default_values": false
   }})
3. Use the reflected property names from step 2 to choose the narrow writer action, or inspect an existing ActionSet with describe_action_set.
```

### Add common GameFeatureData instanced actions

```
1. gamefeatures_query({ action: "add_game_feature_data_components", params: {
     "game_feature_data_path": "/ShooterCore/ShooterCore",
     "actor_class": "/Script/LyraGame.LyraCharacter",
     "component_class": "/Script/LyraGame.LyraPawnExtensionComponent",
     "dry_run": true
   }})
2. gamefeatures_query({ action: "add_game_feature_data_gameplay_cue_paths", params: {
     "game_feature_data_path": "/ShooterCore/ShooterCore",
     "directory_path": "/GameplayCues",
     "dry_run": true
   }})
3. gamefeatures_query({ action: "add_game_feature_data_abilities", params: {
     "game_feature_data_path": "/ShooterCore/ShooterCore",
     "actor_class": "/Script/LyraGame.LyraCharacter",
     "ability_sets": ["/ShooterCore/AbilitySets/AS_Example"],
     "dry_run": true
   }})
4. Re-run the intended call with "dry_run": false after confirming the result delta.
```

### Remove a wrong GameFeatureData action

```
1. gamefeatures_query({ action: "remove_game_feature_data_action", params: {
     "game_feature_data_path": "/ShooterCore/ShooterCore",
     "action_class_path": "/Script/LyraGame.GameFeatureAction_AddWidgets",
     "remove_all": false,
     "dry_run": true
   }})
2. Check removed_actions[] and actions_after, then re-run with "dry_run": false if the selected entry is correct.
```

### Survey, resolve, describe, and validate one plugin end-to-end (status → list → find → describe → validate)

```
1. gamefeatures_query({ action: "get_status", params: {} })                                                  # availability, flags, module status, discovered plugin count — gate check
2. gamefeatures_query({ action: "list_plugins", params: { "limit": 50, "include_engine": false } })          # GameFeature-style plugins from descriptors + AssetRegistry (read-only); pick a plugin "name"
3. gamefeatures_query({ action: "find_game_feature_data", params: { "plugin_name": "<PluginName>" } })        # resolve name to bounded GameFeatureData metadata (need plugin_name OR asset_path)
4. gamefeatures_query({ action: "describe_game_feature_data", params: { "plugin_name": "<PluginName>" } })    # load + summarize the GFData asset and its bounded reflected feature actions
5. gamefeatures_query({ action: "validate_plugin", params: { "plugin_name": "<PluginName>" } })               # validate descriptor, content root, GFData asset, creation gate (read-only)
```

If `find_game_feature_data` reports multiple matches or a multi-data plugin, pass the specific `asset_path` (package or object path from step 3) to `describe_game_feature_data` instead of `plugin_name`.

## Gotchas / Rules

- `get_status` and the eight writer actions are always registered. `list_plugins`, `find_game_feature_data`, `describe_game_feature_data`, `list_action_classes`, `describe_action_set`, and `validate_plugin` require `bEnableGameFeatureActions` to be registered; when it is off, `get_status` reports `enabled=false` and lists them under `available_when_enabled`.
- Treat `action_name` as an exact object-name and action-class selector. A same-name object of another class or an occupied non-`Actions` inner name is an error; omit `action_name` only when class-based reuse is intended.
- Writer dry-runs and real writes enforce the same referenced class/object/tag types. In particular, reflected soft-class fields enforce `MetaClass`, `initialization_data` must be a DataTable, and `ability_sets` entries must be LyraAbilitySet assets.
- This namespace does NOT activate, deactivate, create, rename, or delete plugins; do that through editor plugin management and `unreal-config` descriptor/INI edits. Its writers are scoped to existing ActionSet/GameFeatureData assets and known property arrays.
- GameFeatureData reflection is bounded — `describe_game_feature_data` summarizes feature actions, it does not deep-load arbitrary referenced assets. Follow up in the owning namespace (e.g. `unreal-gas`, `unreal-blueprints`) for the actual contributed content.
- This reference is generated from the live `RegisterAction` surface. If an action is missing or renamed, re-run `monolith_discover({ namespace: "gamefeatures" })` — the catalog is the source of truth.
- Pass `mode: "schema"` to `monolith_discover` for required/optional params and types before calling an action.
