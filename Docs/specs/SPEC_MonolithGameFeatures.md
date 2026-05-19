# Monolith - MonolithGameFeatures Module

**Parent:** [SPEC_CORE.md](../SPEC_CORE.md)
**Engine:** Unreal Engine 5.7+
**Version:** 0.14.10
**Owner module:** MonolithGameFeatures
**Namespace:** `gamefeatures`
**Status:** Implemented first slice
**Feature class:** Read-only optional engine-plugin inspection

---

## 1. Decision

`MonolithGameFeatures` owns the `gamefeatures` namespace. Game Feature plugin
inventory is an optional engine-plugin domain, so the action surface lives in a
dedicated module instead of being mounted from `MonolithIndex`.

The first slice is read-only and intentionally dependency-light. It must not add
a compile dependency on Epic's experimental UE 5.8
`GameFeaturesToolset`/`ToolsetRegistry` modules, and it must not create files,
assets, plugin descriptors, or activation requests.

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
| `FMonolithGameFeaturesModule` | Registers `gamefeatures.get_status` unconditionally and registers the detailed inspection actions only when `bEnableGameFeatureActions=true`. |
| `FMonolithGameFeatureActions` | Implements read-only Game Feature status, plugin inventory, GameFeatureData lookup/summary, and validation handlers. |

| Dependency | Purpose |
|------------|---------|
| `MonolithCore` | Tool registry, action result, settings, and parameter schema contracts. |
| `AssetRegistry` | Read-only `GameFeatureData` discovery and metadata rows. |
| `Projects` | Plugin descriptor inspection via `IPluginManager` and `FPluginReferenceDescriptor`. |
| `UnrealEd`, `Engine` | Editor-only action host and UObject reflection for bounded summaries. |
| `Json`, `JsonUtilities` | Action response payloads. |

---

## 3. Scope

| Area | First slice behavior |
|------|----------------------|
| Settings | `gamefeatures.get_status` is always registered. `bEnableGameFeatureActions=false` gates the four detailed inspection actions on editor restart. `bAllowGameFeaturePluginCreation=false` is reserved and reported by status only. |
| Discovery | `gamefeatures.get_status` reports flags, module availability, plugin-root scan paths, registered actions, and actions available after opt-in. |
| Plugin inventory | `gamefeatures.list_plugins` returns bounded GameFeature-style plugin descriptors that declare an enabled `GameFeatures` dependency, with project `Plugins/GameFeatures` path and descriptor metadata treated as diagnostic hints. |
| Data asset lookup | `gamefeatures.find_game_feature_data` resolves by plugin name or asset path using AssetRegistry metadata, without loading assets. |
| Data asset summary | `gamefeatures.describe_game_feature_data` explicitly loads a resolved `GameFeatureData` asset and returns a capped reflected summary of its action objects. |
| Validation | `gamefeatures.validate_plugin` checks descriptor presence, enabled `GameFeatures` dependency, content root, likely GameFeatureData asset, and creation gate state. |
| Creation | Not implemented in this slice. Any creation action must be a later spec and require explicit confirmation plus overwrite guards. |

---

## 4. Action Contracts

| Action | Params | Result |
|--------|--------|--------|
| `get_status` | none | `{enabled, inspection_enabled, creation_allowed, gamefeatures_module_loaded, plugin_count, scan_roots[], registered_actions[], available_when_enabled[]}` |
| `list_plugins` | `limit`, `include_engine` | `{plugins[], count, returned_count, limit, truncated}` with descriptor path redaction, `declares_gamefeatures_dependency`, and content root summaries |
| `find_game_feature_data` | `plugin_name` or `asset_path` | `{found, plugin_name, game_feature_data, reason?}` |
| `describe_game_feature_data` | `plugin_name` or `asset_path` | `{asset, action_count, actions[], raw_object_graph_dumped=false}` |
| `validate_plugin` | `plugin_name` | `{plugin_name, ok, plugin, checks[], warnings[], next_actions[]}` |

`list_plugins` and `find_game_feature_data` must prefer metadata and filesystem
descriptors over UObject loading. Returned paths are normalized to project- or
engine-relative forms where possible and never expose unrelated absolute host
paths.

---

## 5. JSON Examples

Default `gamefeatures.get_status` with detailed inspection disabled:

```json
{
  "namespace": "gamefeatures",
  "mode": "read_only",
  "enabled": false,
  "inspection_enabled": false,
  "creation_allowed": false,
  "gamefeatures_module_loaded": false,
  "plugin_count": 1,
  "scan_roots": ["<project>/Plugins/GameFeatures"],
  "registered_actions": ["get_status"],
  "available_when_enabled": ["list_plugins", "find_game_feature_data", "describe_game_feature_data", "validate_plugin"]
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

The first slice is read-only. It must not:

- call GameFeature plugin activation/deactivation APIs;
- create, overwrite, rename, or delete plugin files;
- load arbitrary caller-provided filesystem paths;
- dump full asset object graphs;
- expose absolute private paths when a project- or engine-relative path is enough.

If later work adds creation, the creation path must require
`bEnableGameFeatureActions=true`, `bAllowGameFeaturePluginCreation=true`,
profile approval, `confirm=true`, strict plugin-name validation, and an explicit
generated-file manifest.

---

## 7. Verification

Focused tests should cover:

| Case | Expected result |
|------|-----------------|
| Disabled setting | Only `gamefeatures.get_status` is registered after restart when `bEnableGameFeatureActions=false`; the four detailed inspection actions are absent. |
| Empty project | Status and list actions succeed with `count=0`. |
| Descriptor fixture | A descriptor with enabled `GameFeatures` plugin dependency is discovered and redacted; `Plugins/GameFeatures` path fallback is only a hint. |
| AssetRegistry fixture | GameFeatureData candidates are found by class/path metadata without loading assets. |
| Invalid plugin name | Validation reports a stable error/warning and never touches disk. |
| Creation flag | Status reports creation disabled until both flags are true; no creation action is registered in this slice. |

---

## 8. Acceptance

- `MonolithGameFeatures` owns the `gamefeatures` namespace and keeps Game Feature
  inspection out of `MonolithIndex` and `MonolithMesh`.
- The namespace gives agents useful Game Feature inventory and validation without
  requiring Epic experimental Toolsets.
- Default behavior exposes only a read-only status action; detailed inspection
  remains opt-in.
- Creation remains reserved and cannot run through this PR.
- `Docs/SPEC_CORE.md`, `Docs/API_REFERENCE.md`, and a focused testing note
  describe the actual implemented surface.
