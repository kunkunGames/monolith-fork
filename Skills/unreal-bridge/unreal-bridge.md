---
name: unreal-bridge
description: "Use to bridge assets and C++ source via Monolith MCP: map Blueprint/asset nodes to their backing C++ symbols and search asset-to-symbol relationships. Triggers on asset symbol, bridge, asset to source, blueprint to cpp, backing class, native parent, symbol of asset."
---

# unreal-bridge

**5 actions** via `bridge_query(action, params)`. Action names below are the live registry surface; call `monolith_discover` for exact parameter schemas.

## Discovery

```
monolith_discover({ namespace: "bridge" })                      # all actions in this namespace
monolith_discover({ namespace: "bridge", action: "<action>", mode: "schema" })  # exact params
```

## Action Reference

### Source Context (5)

| Action | Purpose |
|--------|---------|
| `build_attachment` | Materialize a bridge.search_items result into a bounded prompt attachment |
| `get_index_status` | Report local project/source index readiness for Monolith bridge searches |
| `search_asset_symbols` | Read-only RX-6 bridge between ProjectIndex assets and EngineSource symbols |
| `search_items` | Search local indexed assets and source entries for mention-style prompt context |
| `start_indexing` | Start local project asset and/or source indexing for bridge search |

## Notes

- This reference is generated from the live `RegisterAction` surface. If an action is missing or renamed, re-run `monolith_discover({ namespace: "bridge" })` — the catalog is the source of truth.
- Pass `mode: "schema"` to `monolith_discover` for required/optional params and types before calling an action.
