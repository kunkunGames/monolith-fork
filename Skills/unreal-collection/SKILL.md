---
name: unreal-collection
description: "Use to manage editor asset Collections via Monolith MCP: create static/dynamic collections, add/remove members, and query collection contents. Triggers on collection, asset collection, static collection, dynamic collection, group assets, collection membership, tag set."
---

# unreal-collection

**13 actions** via `collection_query(action, params)`. Action names below are the live registry surface; call `monolith_discover` for exact parameter schemas.

## Discovery

```
monolith_discover({ namespace: "collection" })                      # all actions in this namespace
monolith_discover({ namespace: "collection", action: "<action>", mode: "schema" })  # exact params
```

## Action Reference

### Asset Collection (13)

| Action | Purpose |
|--------|---------|
| `add_assets` | Add one or more assets to a static Content Browser collection. |
| `contains_asset` | Check whether a collection contains an asset. |
| `create_collection` | Create a static or dynamic Content Browser collection. |
| `create_unique_collection_name` | Create a unique collection name from a base name. |
| `delete_collection` | Delete a Content Browser collection. Non-empty collections require force=true. |
| `get_collection` | Get Content Browser collection details. |
| `get_dynamic_query` | Get query text from a dynamic collection. |
| `list_assets` | List asset paths in a collection. |
| `list_collections` | List Content Browser collections, optionally filtered by share_type. |
| `remove_assets` | Remove one or more assets from a static Content Browser collection. |
| `set_collection_color` | Set or clear a collection color. Omit color to clear. |
| `set_dynamic_query` | Set query text for a dynamic collection. |
| `validate_collection_name` | Validate a collection name for a share type. |

## Notes

- This reference is generated from the live `RegisterAction` surface. If an action is missing or renamed, re-run `monolith_discover({ namespace: "collection" })` — the catalog is the source of truth.
- Pass `mode: "schema"` to `monolith_discover` for required/optional params and types before calling an action.
