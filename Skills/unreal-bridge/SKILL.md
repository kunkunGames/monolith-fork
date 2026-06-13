---
name: unreal-bridge
description: Use when you need to CROSS between an Unreal asset and its backing C++ source via Monolith MCP — map a Blueprint/asset node to the C++ symbol behind it and search asset-to-symbol relationships. Owns ONLY the asset to symbol crossing layer. For C++ source search/hierarchy alone use unreal-cpp; for asset references/dependencies within the project use unreal-project-search; for reflection metadata such as UCLASS/UPROPERTY use unreal-reflection-intel. Triggers on bridge namespace, asset symbol, asset to source, blueprint to cpp, BP to C++, what class backs this asset, find source for asset, which cpp implements, asset's parent class, native base of blueprint, native parent, backing class, symbol for /Game path, symbol of asset.
---

# unreal-bridge

**5 actions** via `bridge_query(action, params)`. Action names below are the live registry surface; call `monolith_discover` for exact parameter schemas.

## Discovery

```
monolith_discover({ namespace: "bridge" })                      # all actions in this namespace
monolith_discover({ namespace: "bridge", action: "<action>", mode: "schema" })  # exact params
```

## When to use

Use this skill only for the CROSSING between an asset and its backing C++ source — answering "what class backs this asset?", "find the source for this asset path", "which .cpp implements this Blueprint?", "what is the native parent of this BP?".

### Use a different skill for

- **unreal-cpp** — pure C++ source search, signatures, includes, or class hierarchy with no asset crossing.
- **unreal-project-search** — asset references and dependencies between assets within the project (asset-to-asset, not asset-to-symbol).
- **unreal-reflection-intel** — reflection metadata (UCLASS/UPROPERTY/UFUNCTION, replication, decisions) rather than asset-to-symbol links.

## Action Reference

Param notation: `name*` required, `name?` optional, `name=val` default, `a/b/c` allowed values, `[w]` mutates. Signatures are a snapshot of the live catalog — for the exact, full, current schema of any action call `monolith_discover` with `mode: "schema"`.

### Source Context (5)

| Action | Purpose | Params (req* opt? =default) |
|--------|---------|------------------------------|
| `search_asset_symbols` | Read-only RX-6 bridge between ProjectIndex assets and EngineSource symbols | `asset_path?` `symbol?` `limit=20` `detail_level=minimal` (minimal/standard); seed with at least one of asset_path or symbol |
| `search_items` | Search local indexed assets and source entries for mention-style prompt context | `query*` `limit=24` `include_assets=true` `include_source=true` |
| `build_attachment` [w] | Materialize a bridge.search_items result into a bounded prompt attachment | `item_id*` (from search_items) `context_lines=12` `max_chars=12000` |
| `get_index_status` | Report local project/source index readiness for Monolith bridge searches | `include_stats=false` |
| `start_indexing` [w] | Start local project asset and/or source indexing for bridge search | `scope=all` (assets/source/all) `full=false` |

## Common Workflows

### Cross from an asset to its backing C++ symbol (and back)
Bridge searches read the local ProjectIndex (assets) and EngineSource (symbols) indexes, so confirm readiness first, then query the crossing both directions.

1. `bridge_query("get_index_status", { "include_stats": true })` — check that the project/source indexes are ready for bridge searches. If readiness is incomplete, go to step 2; otherwise skip to step 3.
2. `bridge_query("start_indexing", { "scope": "all", "full": false })` *(mutates [w])* — start local asset + source indexing for bridge search. Re-run step 1 until status reports ready before searching; `full: true` forces a from-scratch rebuild.
3. `bridge_query("search_asset_symbols", { "asset_path": "/Game/Maps/Interactable/BP_Wave", "limit": 20, "detail_level": "standard" })` — seed with the asset path to find the native symbol(s) backing it (what class backs this asset / native parent).
4. `bridge_query("search_asset_symbols", { "symbol": "AActor", "limit": 20, "detail_level": "standard" })` — seed with the symbol instead to find assets linked to a given C++ class. You may seed with `asset_path`, `symbol`, or both, but at least one is required.
5. Read the link quality on each result before trusting it: bridge links are **heuristic**, so check `confidence`, the `reasons` array (why the link was made), and the top-level `lexical_only` flag (true means only lexical/name matching was available — treat those hits as weaker and verify with **unreal-cpp**).

### Pull asset/source context into a prompt attachment
1. `bridge_query("search_items", { "query": "BP_Wave interactable", "limit": 24, "include_assets": true, "include_source": true })` — find mention-style asset/source entries; note the `item_id` of the entry you want.
2. `bridge_query("build_attachment", { "item_id": "<id from step 1>", "context_lines": 12, "max_chars": 12000 })` *(mutates [w])* — materialize that item into a bounded prompt attachment (`item_id` must come from a prior `search_items` result).

## Notes

- This reference is generated from the live `RegisterAction` surface. If an action is missing or renamed, re-run `monolith_discover({ namespace: "bridge" })` — the catalog is the source of truth.
- Pass `mode: "schema"` to `monolith_discover` for required/optional params and types before calling an action.
