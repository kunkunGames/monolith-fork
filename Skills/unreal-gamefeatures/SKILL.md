---
name: unreal-gamefeatures
description: "Use for Game Features plugins and modular gameplay via Monolith MCP: list, activate/deactivate game feature plugins, and inspect game feature actions. Triggers on game feature, game features, modular gameplay, plugin activate, deactivate plugin, feature action, GFP, game feature data."
---

# unreal-gamefeatures

**5 actions** via `gamefeatures_query(action, params)`. Action names below are the live registry surface; call `monolith_discover` for exact parameter schemas.

## Discovery

```
monolith_discover({ namespace: "gamefeatures" })                      # all actions in this namespace
monolith_discover({ namespace: "gamefeatures", action: "<action>", mode: "schema" })  # exact params
```

## Action Reference

### Game Feature (5)

| Action | Purpose |
|--------|---------|
| `describe_game_feature_data` | Load and summarize one GameFeatureData asset by plugin name or asset path, including bounded reflected action summaries. |
| `find_game_feature_data` | Resolve a GameFeature plugin name or asset path to bounded GameFeatureData AssetRegistry metadata without loading arbitrary paths. |
| `get_status` | Report read-only GameFeatures inspection availability, flags, module status, and discovered plugin count. |
| `list_plugins` | List GameFeature-style plugins using plugin descriptors and AssetRegistry metadata. Read-only; no plugin activation or file writes. |
| `validate_plugin` | Validate a GameFeature plugin descriptor, content root, GameFeatureData asset, and creation gate state. Read-only. |

## Notes

- This reference is generated from the live `RegisterAction` surface. If an action is missing or renamed, re-run `monolith_discover({ namespace: "gamefeatures" })` — the catalog is the source of truth.
- Pass `mode: "schema"` to `monolith_discover` for required/optional params and types before calling an action.
