---
name: unreal-collection
description: Use when managing editor asset Collections via Monolith MCP - create static/dynamic collections, add/remove members, set color/dynamic query, and query collection contents. To find or query assets to populate a collection use unreal-project-search; if group/changelist means a source-control changelist use unreal-source-control; gameplay or asset metadata tags are not editor collections. Triggers on collection, asset collection, static collection, dynamic collection, group assets, collection membership, tag set, add to collection, remove from collection, collection contents, saved search collection, asset group, shared collection, local collection.
---

# unreal-collection

**13 actions** via `collection_query(action, params)`. Action names below are the live registry surface; call `monolith_discover` for exact parameter schemas.

## Discovery

```
monolith_discover({ namespace: "collection" })                      # all actions in this namespace
monolith_discover({ namespace: "collection", action: "<action>", mode: "schema" })  # exact params
```

## When to use / Use a different skill for

- Use this skill to create, color, populate, query, or delete Content Browser asset **Collections** and their members.
- To find or query the assets you want to add (FTS search, references, dependencies, type filtering), use **unreal-project-search**, then feed the paths into `add_assets`.
- When "group" or "changelist" means a source-control changelist (Perforce/Git checkout grouping), use **unreal-source-control** — not a Content Browser collection.
- Gameplay tags and asset metadata tags are not editor collections — use **unreal-gas** for gameplay tags and **unreal-asset** for asset metadata.

## Action Reference

Param notation: `name*` required, `name?` optional, `name=val` default, `a/b/c` allowed values, `[w]` mutates. Signatures are a snapshot of the live catalog — for the exact, full, current schema of any action call `monolith_discover` with `mode: "schema"`.

### Asset Collection (13)

| Action | Purpose | Params (req* opt? =default) |
|--------|---------|------------------------------|
| `list_collections` | List Content Browser collections, optionally filtered by share_type. | `share_type=all` (local/private/shared/system/all) |
| `get_collection` | Get Content Browser collection details. | `name*`, `share_type=local` (local/private/shared/system) |
| `list_assets` | List asset paths in a collection. | `name*`, `share_type=local` (local/private/shared/system), `recursive=self` (self/children/parents/all) |
| `contains_asset` | Check whether a collection contains an asset. | `name*`, `asset_path*`, `share_type=local` (local/private/shared/system), `recursive=self` (self/children/parents/all) |
| `get_dynamic_query` | Get query text from a dynamic collection. | `name*`, `share_type=local` (local/private/shared/system) |
| `validate_collection_name` | Validate a collection name for a share type. | `name*`, `share_type=local` (local/private/shared/system/all) |
| `[w] create_collection` | Create a static or dynamic Content Browser collection. | `name*`, `share_type=local` (local/private/shared/system), `storage_mode=static` (static/dynamic) |
| `[w] delete_collection` | Delete a Content Browser collection. Non-empty collections require force=true. | `name*`, `share_type=local` (local/private/shared/system), `force=false` (bool) |
| `[w] add_assets` | Add one or more assets to a static Content Browser collection. | `name*`, `share_type=local` (local/private/shared/system), `asset_path?` (single path), `asset_paths?` (array) |
| `[w] remove_assets` | Remove one or more assets from a static Content Browser collection. | `name*`, `share_type=local` (local/private/shared/system), `asset_path?` (single path), `asset_paths?` (array) |
| `[w] set_dynamic_query` | Set query text for a dynamic collection. | `name*`, `query_text*`, `share_type=local` (local/private/shared/system) |
| `[w] set_collection_color` | Set or clear a collection color. Omit color to clear. | `name*`, `share_type=local` (local/private/shared/system), `color?` (object `{r,g,b,a}` in 0..1; omit to clear) |
| `[w] create_unique_collection_name` | Create a unique collection name from a base name. | `base_name*`, `share_type=local` (local/private/shared/system) |

## Common Workflows

Numbered recipes use only the actions in the table above. Run `monolith_discover` with `mode: "schema"` for exact params before each call. `share_type` must stay consistent across every step of one collection (default `local`).

### Recipe 1 — Build and verify a static collection

1. Find the asset paths to add with **unreal-project-search** (FTS search, references, dependencies, type filtering); collect their `/Game/...` paths — this namespace has no asset-search action.
2. `collection_query("create_collection", { name, share_type: "local", storage_mode: "static" })` `[w]` — create the empty static collection (use `validate_collection_name` first, or `create_unique_collection_name` when the base name may already exist).
3. `collection_query("add_assets", { name, share_type: "local", asset_paths: [ ... ] })` `[w]` — add the harvested paths in one call via `asset_paths` (or a single `asset_path`).
4. `collection_query("contains_asset", { name, asset_path, share_type: "local" })` — spot-check a known member resolved.
5. `collection_query("list_assets", { name, share_type: "local", recursive: "self" })` — list the full membership to confirm the count and contents; widen `recursive` to `children`/`parents`/`all` only when checking a collection hierarchy.

Pitfall — deletion: `collection_query("delete_collection", { name, share_type: "local" })` `[w]` refuses a non-empty collection unless you pass `force: true`. Run `list_assets` first and only set `force: true` when you intend to drop a populated collection.

### Recipe 2 — Build and verify a dynamic (query) collection

1. `collection_query("create_collection", { name, share_type: "local", storage_mode: "dynamic" })` `[w]` — create the collection in dynamic mode so its membership comes from a saved query, not a fixed asset list (`validate_collection_name` first, or `create_unique_collection_name` when the base name may already exist).
2. `collection_query("set_dynamic_query", { name, share_type: "local", query_text: "<query>" })` `[w]` — set the saved-search query that defines membership; build the query against the assets you found with **unreal-project-search**.
3. `collection_query("get_dynamic_query", { name, share_type: "local" })` — read the stored query text back to confirm it persisted exactly as written.
4. `collection_query("list_assets", { name, share_type: "local", recursive: "self" })` — list the assets the query currently resolves to and confirm the count and contents match the intent.

Pitfall — dynamic vs static membership: a dynamic collection has no fixed member list, so `add_assets`/`remove_assets` do not apply; change membership only by editing the query with `set_dynamic_query`. Conversely, a static collection has no query, so `set_dynamic_query`/`get_dynamic_query` are not valid for it. Pick `storage_mode` at `create_collection` time — it cannot be flipped later by these actions. Because the membership is recomputed from the query, `list_assets` reflects the current asset set and changes as matching assets are added or removed elsewhere.

Pitfall — share_type scope: `share_type` selects which collection store the name lives in (`local`/`private`/`shared`/`system`), and a name is unique only within one store. Keep the same `share_type` on every step of one collection; the default `local` is per-user and not committed to source control, so use `shared` when other team members must see the collection (and check it out through **unreal-source-control** if your collection store is versioned).

## Notes

- This reference is generated from the live `RegisterAction` surface. If an action is missing or renamed, re-run `monolith_discover({ namespace: "collection" })` — the catalog is the source of truth.
- Pass `mode: "schema"` to `monolith_discover` for required/optional params and types before calling an action.
