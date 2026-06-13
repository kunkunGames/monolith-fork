---
name: unreal-gamefeatures
description: Use when inspecting Game Feature plugins and modular gameplay via Monolith MCP (gamefeatures namespace) - list/find GameFeature plugins, describe GameFeatureData assets and their reflected feature actions, and validate plugin descriptors. For the gameplay logic/components a feature ADDS use unreal-blueprints; for the .uplugin descriptor or .ini/cvar gates that enable a feature use unreal-config; for GAS abilities/effects a feature contributes use unreal-gas and only inspect the plugin here. Triggers on game feature, game features, modular gameplay, GameFeatureData, GFP, game feature plugin, game feature action, feature action, GameFeatureAction, plugin activate, deactivate plugin, validate plugin descriptor, what plugins are active.
---

# unreal-gamefeatures

Drives the **`gamefeatures`** namespace via `gamefeatures_query(action, params)`: read-only inspection of GameFeature plugins, GameFeatureData assets, and their reflected feature actions. The 5 actions below are a snapshot — discover first so you never call a stale or guessed name.

## Discovery

```
monolith_discover({ namespace: "gamefeatures" })                      # all actions in this namespace
monolith_discover({ namespace: "gamefeatures", action: "<action>", mode: "schema" })  # exact params
```

## When to use / Use a different skill for

Use this skill to inspect GameFeature plugins and modular gameplay — listing/finding plugins, summarizing GameFeatureData assets and their bounded feature-action reflection, and validating a plugin descriptor or creation gate.

Use a different skill for:

- **unreal-blueprints** — when editing the actual gameplay logic, components, or actor graphs a Game Feature ADDS, rather than inspecting the feature plugin itself.
- **unreal-config** — when editing the plugin descriptor (`.uplugin`) or the `.ini`/cvar settings that gate a Game Feature, rather than reading its inspection state.
- **unreal-gas** — when the modular feature contributes GAS abilities, attribute sets, or effects; author those in the GAS namespace and only inspect the plugin here.

## Action Reference

Param notation: `name*` required, `name?` optional, `name=val` default, `a/b/c` allowed, `[w]` mutates. Signatures are a snapshot of the live catalog — for the exact full schema call `monolith_discover` with `mode: "schema"`. The discover-first block above is the authority.

> Gate note: `get_status` is always registered and takes no params. The other four actions require `bEnableGameFeatureActions` to be registered (see "Limited surface" and Gotchas). The signatures below are transcribed from the `gamefeatures` `RegisterAction` / `FParamSchemaBuilder` calls in `MonolithGameFeatures/Private/MonolithGameFeatureActions.cpp`; all five are read-only (no `[w]` mutation).

### Game Feature (5)

| Action | Params | Purpose |
|--------|--------|---------|
| `get_status` | _(none)_ | Report read-only GameFeatures inspection availability, flags, module status, and discovered plugin count. |
| `list_plugins` | `limit?=50` `include_engine?=false` | List GameFeature-style plugins from plugin descriptors and AssetRegistry metadata. Read-only; no activation or file writes. |
| `find_game_feature_data` | `plugin_name?` `asset_path?` | Resolve a GameFeature plugin name or asset path to bounded GameFeatureData AssetRegistry metadata without loading arbitrary paths. |
| `describe_game_feature_data` | `plugin_name?` `asset_path?` | Load and summarize one GameFeatureData asset, including bounded reflected feature-action summaries. |
| `validate_plugin` | `plugin_name*` | Validate a plugin descriptor, content root, GameFeatureData asset, and creation gate state. Read-only. |

`find_game_feature_data` and `describe_game_feature_data` mark both params optional in the schema, but the handler requires at least one of `plugin_name` or `asset_path` and errors otherwise; `asset_path` accepts either a package path or an object path. `list_plugins` `limit` is clamped to 1..200.

## Common Workflows

**Flag-gated surface:** `get_status` is always registered (no params). The other four actions require `bEnableGameFeatureActions` to be registered — start every recipe with `get_status` to learn inspection availability, module status, and discovered plugin count; if `enabled` is false those actions are listed under `available_when_enabled` and not registered. All steps use only Action Reference actions with their real params.

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

- Only `get_status` is always registered. `list_plugins`, `find_game_feature_data`, `describe_game_feature_data`, and `validate_plugin` require `bEnableGameFeatureActions` to be registered; when it is off, `get_status` reports `enabled=false` and lists them under `available_when_enabled`.
- This namespace is read-only inspection. It does NOT activate, deactivate, or write plugins; do that through editor plugin management and `unreal-config` descriptor/INI edits.
- GameFeatureData reflection is bounded — `describe_game_feature_data` summarizes feature actions, it does not deep-load arbitrary referenced assets. Follow up in the owning namespace (e.g. `unreal-gas`, `unreal-blueprints`) for the actual contributed content.
- This reference is generated from the live `RegisterAction` surface. If an action is missing or renamed, re-run `monolith_discover({ namespace: "gamefeatures" })` — the catalog is the source of truth.
- Pass `mode: "schema"` to `monolith_discover` for required/optional params and types before calling an action.
