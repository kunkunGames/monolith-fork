# Monolith — MonolithGameFeatures Module

**Parent:** [SPEC_CORE.md](../SPEC_CORE.md)
**Engine:** Unreal Engine 5.7+
**Version:** 0.21.3 (Beta)
**Owner module:** MonolithGameFeatures
**Namespace:** `gamefeatures`
**Status:** Implemented expanded instanced-action authoring slice
**Feature class:** Optional engine-plugin inspection plus guarded ActionSet/GameFeatureData instanced-action authoring

---

## 1. Decision

`MonolithGameFeatures` owns the `gamefeatures` namespace. Game Feature plugin
inventory is an optional engine-plugin domain, so the action surface lives in a
dedicated module instead of being mounted from `MonolithIndex`.

The inspection surface is dependency-light. It must
not add a compile dependency on Epic's experimental UE 5.8
`GameFeaturesToolset`/`ToolsetRegistry` modules, and it must not create plugin
descriptors or activation requests. The namespace also exposes guarded authoring
actions for already-existing ActionSet and GameFeatureData assets: it creates or
updates instanced feature actions, edits bounded known property arrays, and saves
the owning package only when requested.

Reference implementation review: UE 5.8
`Engine/Plugins/Experimental/Toolsets/GameFeaturesToolset` identifies a Game
Feature plugin by an enabled `GameFeatures` plugin dependency in the descriptor,
then resolves the plugin's `GameFeatureData`. Monolith follows that descriptor
contract for inventory and validation while keeping UE 5.7 compatibility by
using `IPluginManager`, `AssetRegistry`, and bounded reflection.

---

## 2. Ownership

| Class | Responsibility |
|-------|----------------|
| `FMonolithGameFeaturesModule` | Registers `gamefeatures.get_status` plus eight guarded writer actions unconditionally, and registers the detailed inspection actions only when `bEnableGameFeatureActions=true`. |
| `FMonolithGameFeatureActions` | Implements Game Feature status, plugin inventory, GameFeatureData lookup/summary, reflected action-class discovery, ActionSet summaries, validation handlers, and guarded instanced-action authoring. |

| Dependency | Purpose |
|------------|---------|
| `MonolithCore` | Tool registry, action result, settings, and parameter schema contracts. |
| `GameFeatures`, `GameplayTags` | `UGameFeatureData` PrimaryAsset scan editing, AddInputContextMapping/AddWidgets/AddComponents/AddGameplayCuePath/AddAbilities action authoring, and GameplayTag validation for widget slots. |
| `AssetRegistry` | Read-only `GameFeatureData` discovery and metadata rows. |
| `Projects` | Plugin descriptor inspection via `IPluginManager` and `FPluginReferenceDescriptor`. |
| `UnrealEd`, `Engine` | Editor-only action host and UObject reflection for bounded summaries. |
| `Json`, `JsonUtilities` | Action response payloads. |

`GameFeatures` is an optional plugin dependency. `MonolithGameFeatures.Build.cs`
only links its API when the target project explicitly enables `GameFeatures`
and `MONOLITH_RELEASE_BUILD` is not `1`. Other targets compile a status-only
stub, so installing Monolith does not force an engine plugin onto every host.

---

## 3. Scope

| Area | Current behavior |
|------|----------------------|
| Compiled surface | When the target enables `GameFeatures`, the module exposes 9 default actions and all 15 actions when inspection is enabled. Without the optional dependency it registers only `get_status`, which reports `dependency_state=unavailable`. |
| Settings | `gamefeatures.get_status`, `add_action_set_input_mapping`, `set_primary_asset_scan`, `add_game_feature_data_input_mapping`, `add_game_feature_data_widgets`, `add_game_feature_data_components`, `add_game_feature_data_gameplay_cue_paths`, `add_game_feature_data_abilities`, and `remove_game_feature_data_action` are always registered. `bEnableGameFeatureActions=false` gates the six detailed inspection actions on editor restart. `bAllowGameFeaturePluginCreation=false` is reserved and reported by status only. |
| Discovery | `gamefeatures.get_status` reports flags, module availability, plugin-root scan paths, registered actions, and actions available after opt-in. |
| Plugin inventory | `gamefeatures.list_plugins` returns bounded GameFeature-style plugin descriptors that declare an enabled `GameFeatures` dependency, with project `Plugins/GameFeatures` path and descriptor metadata treated as diagnostic hints. |
| Data asset lookup | `gamefeatures.find_game_feature_data` resolves by plugin name or asset path using AssetRegistry metadata, without loading assets. A descriptor-declared asset wins; one indexed candidate is unambiguous; multiple candidates without a matching descriptor declaration fail and require `asset_path`. |
| Data asset summary | `gamefeatures.describe_game_feature_data` explicitly loads a resolved `GameFeatureData` asset and returns capped reflected action objects plus optional editable property values. |
| Action class discovery | `gamefeatures.list_action_classes` lists loaded `UGameFeatureAction` subclasses and bounded reflected editable properties/defaults for schema discovery before authoring. |
| ActionSet summary | `gamefeatures.describe_action_set` loads an existing ActionSet-style asset and summarizes its instanced `Actions` array with bounded reflected property values. |
| Validation | `gamefeatures.validate_plugin` checks descriptor presence, enabled `GameFeatures` dependency, content root, likely GameFeatureData asset, and creation gate state. |
| ActionSet input mapping | `gamefeatures.add_action_set_input_mapping` loads an existing asset with an instanced `Actions` array, creates or reuses a compatible action class, adds one `InputMappings` entry idempotently, optionally removes null `Actions`, and saves the package when `save=true`. |
| GameFeatureData primary asset scan | `gamefeatures.set_primary_asset_scan` creates or updates one `PrimaryAssetTypesToScan` entry on an existing `UGameFeatureData` asset, with class/path validation and dry-run support. |
| GameFeatureData input mapping | `gamefeatures.add_game_feature_data_input_mapping` creates or reuses an AddInputContextMapping-style action directly on `UGameFeatureData` and adds one mapping entry idempotently. |
| GameFeatureData widgets | `gamefeatures.add_game_feature_data_widgets` creates or reuses an AddWidgets-style action directly on `UGameFeatureData` and adds layout/widget class plus GameplayTag entries idempotently. |
| GameFeatureData components | `gamefeatures.add_game_feature_data_components` creates or reuses an AddComponents action directly on `UGameFeatureData` and adds one actor/component request entry with client/server/flag metadata. |
| GameFeatureData GameplayCue paths | `gamefeatures.add_game_feature_data_gameplay_cue_paths` creates or reuses an AddGameplayCuePath-style action directly on `UGameFeatureData` and adds one or more cue directory paths. |
| GameFeatureData abilities | `gamefeatures.add_game_feature_data_abilities` creates or reuses an AddAbilities-style action directly on `UGameFeatureData` and adds actor-scoped ability, attribute-set, and ability-set grants. |
| GameFeatureData action removal | `gamefeatures.remove_game_feature_data_action` removes one or more existing `UGameFeatureData.Actions` entries by array index, instanced object name, and/or action class, with dry-run support and bounded removed-action summaries. |
| Instanced-action mutation boundary | Every add/update writer resolves the action and null-removal delta without mutation, applies the complete request to a transient new/duplicated action, and commits the validated property state plus array edits only after preflight succeeds. `action_name` is an exact object-name and requested-class selector; an occupied name outside the selectable `Actions` entry fails explicitly. |
| Creation | Not implemented in this slice. Any creation action must be a later spec and require explicit confirmation plus overwrite guards. |

---

## 4. Action Contracts

| Action | Params | Result |
|--------|--------|--------|
| `get_status` | none | `{enabled, inspection_enabled, creation_allowed, gamefeatures_module_loaded, plugin_count, scan_roots[], registered_actions[], available_when_enabled[]}` |
| `list_plugins` | `limit`, `include_engine` | `{plugins[], count, returned_count, limit, truncated}` with descriptor path redaction, `declares_gamefeatures_dependency`, and content root summaries |
| `find_game_feature_data` | `plugin_name` or `asset_path` | `{found, plugin_name, game_feature_data, reason?}` |
| `describe_game_feature_data` | `plugin_name` or `asset_path`, optional `include_action_properties`, `editable_only`, `include_values`, `property_limit`, `max_value_chars` | `{asset, action_count, actions[], raw_object_graph_dumped=false}` |
| `list_action_classes` | optional `limit`, `module`, `name_contains`, `include_abstract`, `editable_only`, `include_default_values`, `property_limit`, `max_value_chars` | `{classes[], count, returned_count, limit, truncated}` |
| `describe_action_set` | `action_set_path`, optional `action_limit`, `include_action_properties`, `editable_only`, `include_values`, `property_limit`, `max_value_chars` | `{asset_path, class, action_count, actions[], actions_truncated}` |
| `add_action_set_input_mapping` | `action_set_path`, `mapping_context_path`, optional `action_class_path`, `priority`, `action_name`, `remove_null_actions`, `save`, `dry_run` | `{created_action, added_mapping, updated_priority, removed_null_actions, actions_before, actions_after, input_mappings_before, input_mappings_after, saved, changed}` |
| `set_primary_asset_scan` | `game_feature_data_path`, `primary_asset_type`, optional `asset_base_class`, `has_blueprint_classes`, `is_editor_only`, `directories`, `specific_assets`, `save`, `dry_run` | `{created_entry, updated_entry, entry_before?, entry_after, saved, changed}` |
| `add_game_feature_data_input_mapping` | `game_feature_data_path`, `mapping_context_path`, optional `action_class_path`, `priority`, `action_name`, `remove_null_actions`, `save`, `dry_run` | `{created_action, added_mapping, updated_priority, removed_null_actions, actions_before, actions_after, input_mappings_before, input_mappings_after, saved, changed}` |
| `add_game_feature_data_widgets` | `game_feature_data_path`, optional `layouts[]`, `widgets[]`, property-name overrides, `action_class_path`, `action_name`, `remove_null_actions`, `save`, `dry_run` | `{created_action, layouts_added, layouts_updated, widgets_added, widgets_updated, entries[], saved, changed}` |
| `add_game_feature_data_components` | `game_feature_data_path`, `actor_class`, `component_class`, optional `action_class_path`, `action_name`, `client_component`, `server_component`, `addition_flags`, `remove_null_actions`, `save`, `dry_run` | `{created_action, added_component, updated_component, components_before, components_after, saved, changed}` |
| `add_game_feature_data_gameplay_cue_paths` | `game_feature_data_path`, `directory_path` or `directory_paths[]`, optional `action_class_path`, `action_name`, `directory_array_property`, `remove_null_actions`, `save`, `dry_run` | `{created_action, paths_added, paths_before, paths_after, paths[], saved, changed}` |
| `add_game_feature_data_abilities` | `game_feature_data_path`, `actor_class`, optional `ability_classes[]`, `attribute_sets[]`, `ability_sets[]`, `action_class_path`, `action_name`, `remove_null_actions`, `save`, `dry_run` | `{created_action, created_entry, abilities_added, attributes_added, attributes_updated, ability_sets_added, ability_entries_before, ability_entries_after, saved, changed}` |
| `remove_game_feature_data_action` | `game_feature_data_path`, at least one of `action_index`, `action_name`, or `action_class_path`, optional `remove_all`, `save`, `dry_run` | `{removed_count, removed_actions[], actions_before, actions_after, saved, changed}` |
| `validate_plugin` | `plugin_name` | `{plugin_name, ok, plugin, checks[], warnings[], next_actions[]}` |

`list_plugins` and `find_game_feature_data` must prefer metadata and filesystem
descriptors over UObject loading. Returned paths are normalized to project- or
engine-relative forms where possible and never expose unrelated absolute host
paths. Plugin-name resolution must not select an arbitrary first AssetRegistry
row when more than one `GameFeatureData` candidate remains.

---

## 5. JSON Examples

Default `gamefeatures.get_status` with detailed inspection disabled:

```json
{
  "namespace": "gamefeatures",
  "mode": "instanced_action_writes",
  "enabled": false,
  "inspection_enabled": false,
  "write_actions_registered": true,
  "creation_allowed": false,
  "gamefeatures_module_loaded": false,
  "plugin_count": 1,
  "scan_roots": ["<project>/Plugins/GameFeatures"],
  "registered_actions": ["get_status", "add_action_set_input_mapping", "set_primary_asset_scan", "add_game_feature_data_input_mapping", "add_game_feature_data_widgets", "add_game_feature_data_components", "add_game_feature_data_gameplay_cue_paths", "add_game_feature_data_abilities", "remove_game_feature_data_action"],
  "write_actions": ["add_action_set_input_mapping", "set_primary_asset_scan", "add_game_feature_data_input_mapping", "add_game_feature_data_widgets", "add_game_feature_data_components", "add_game_feature_data_gameplay_cue_paths", "add_game_feature_data_abilities", "remove_game_feature_data_action"],
  "available_when_enabled": ["list_plugins", "find_game_feature_data", "describe_game_feature_data", "list_action_classes", "describe_action_set", "validate_plugin"]
}
```

`gamefeatures.add_action_set_input_mapping` dry-run:

```json
{
  "dry_run": true,
  "action_set_path": "/SpeedCore/TagChase/ActionSets/LAS_TagChase_Gameplay.LAS_TagChase_Gameplay",
  "mapping_context_path": "/Game/Input/Mappings/IMC_Default.IMC_Default",
  "priority": 0,
  "created_action": true,
  "added_mapping": true,
  "removed_null_actions": 1,
  "changed": true
}
```

`gamefeatures.list_plugins`:

```json
{
  "count": 1,
  "returned_count": 1,
  "truncated": false,
  "plugins": [
    {
      "name": "CombatFeature",
      "descriptor_path": "<project>/Plugins/GameFeatures/CombatFeature/CombatFeature.uplugin",
      "content_root": "/CombatFeature",
      "enabled": true,
      "declares_gamefeatures_dependency": true,
      "game_feature_data": "/CombatFeature/CombatFeatureData"
    }
  ]
}
```

`gamefeatures.validate_plugin`:

```json
{
  "plugin_name": "CombatFeature",
  "ok": true,
  "checks": [
    { "name": "descriptor", "ok": true },
    { "name": "gamefeatures_dependency", "ok": true },
    { "name": "content_root", "ok": true },
    { "name": "game_feature_data", "ok": true }
  ],
  "warnings": [],
  "next_actions": ["gamefeatures.describe_game_feature_data"]
}
```

---

## 6. Safety

GameFeatures inspection must remain bounded and the authoring paths must stay
narrowly scoped. The namespace must not:

- call GameFeature plugin activation/deactivation APIs;
- create, overwrite, rename, or delete plugin files;
- load arbitrary caller-provided filesystem paths;
- dump full asset object graphs;
- expose absolute private paths when a project- or engine-relative path is enough.

Writer actions must only edit existing UObject assets of the expected shape,
must validate referenced classes/assets/tags before mutation, call `Modify()` on
the owning object before edits, mark the package dirty only for real changes,
and support `dry_run=true`. Instanced-action writers perform the same property
application against a transient validation copy for dry-run and real writes.
They must reject a same-name action of another class, enforce
`FSoftClassProperty::MetaClass`, enforce `UDataTable` initialization data and
`ULyraAbilitySet` ability-set values, and never double-initialize a struct entry
returned by `FScriptArrayHelper::AddValue()`. Save errors may identify the
package or asset but must not include an absolute host filename.

If later work adds creation, the creation path must require
`bEnableGameFeatureActions=true`, `bAllowGameFeaturePluginCreation=true`,
profile approval, `confirm=true`, strict plugin-name validation, and an explicit
generated-file manifest.

---

## 7. Verification

Focused tests should cover:

| Case | Expected result |
|------|-----------------|
| Disabled setting | `gamefeatures.get_status` plus eight writer actions are registered after restart when `bEnableGameFeatureActions=false`; the six detailed inspection actions are absent. |
| Optional dependency disabled | Only `gamefeatures.get_status` is registered and it reports `dependency_state=unavailable`; no `GameFeatures` headers or modules are linked. |
| Empty project | Status and list actions succeed with `count=0`. |
| Descriptor fixture | A descriptor with enabled `GameFeatures` plugin dependency is discovered and redacted; a `Plugins/GameFeatures` path match is reported only as a discovery heuristic. |
| AssetRegistry fixture | GameFeatureData candidates are found by class/path metadata without loading assets. |
| Ambiguous GameFeatureData fixture | A descriptor match or exactly one candidate resolves; two unmatched candidates fail and require `asset_path`. |
| Invalid plugin name | Validation reports a stable error/warning and never touches disk. |
| Invalid authoring params | Writer actions reject missing paths, incompatible action classes, missing known property arrays, and invalid referenced assets/tags before mutation. |
| Writer preflight failure | An invalid component/class/tag/object input leaves `Actions`, instanced entries, and package dirty state unchanged. |
| Exact action selector | A same-name action of another class fails; a created named action keeps the exact requested name and an idempotent repeat neither appends nor dirties. |
| Soft class metadata | A reflected soft-class field rejects a class outside its `MetaClass`. |
| No-op and update commit | A repeated identical edit reports `changed=false` and leaves the package clean; a real update preserves action identity/name and changes one existing entry. |
| Creation flag | Status reports creation disabled until both flags are true; no creation action is registered in this slice. |

---

## 8. Acceptance

- `MonolithGameFeatures` owns the `gamefeatures` namespace and keeps Game Feature
  inspection out of `MonolithIndex` and `MonolithMesh`.
- The namespace gives agents useful Game Feature inventory and validation without
  requiring Epic experimental Toolsets.
- Default behavior exposes status plus guarded instanced-action authoring;
  detailed plugin inspection remains opt-in.
- Creation remains reserved and cannot run through this PR.
- `Docs/SPEC_CORE.md`, `Docs/API_REFERENCE.md`, and a focused testing note
  describe the actual implemented surface.
