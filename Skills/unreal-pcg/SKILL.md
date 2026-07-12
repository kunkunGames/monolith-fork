---
name: unreal-pcg
description: Use when creating, editing, inspecting, validating, or migrating Unreal PCG graph assets through Monolith MCP. Covers typed UPCGGraph creation, reflected node-type discovery, node CRUD, pin wiring, strict node-settings writes, topology/settings read-back, structural validation, graph discovery, component listing, and guarded copied-graph reference remapping. For PCG component assignment or generation execution confirm a newer live action first; for fixed blockout generation use unreal-worldgen.
---

# unreal-pcg

Drive the Monolith `pcg` namespace for typed PCG graph-asset work. The checked-in surface has **14 actions**. Treat `monolith_discover({ namespace: "pcg" })` as authoritative for the running editor.

## Start here

```text
monolith_discover({ namespace: "pcg" })
monolith_discover({ namespace: "pcg", action: "<action>", mode: "schema" })
pcg_query("get_status", {})
```

Use a different skill when the target is:

- fixed blockout, building, facade, road, or furnishing generation: `unreal-worldgen`;
- a geometry/Chaos Dataflow graph: `unreal-dataflow`;
- a source StaticMesh/SkeletalMesh asset: `unreal-mesh`;
- actor placement or live-level spatial work: `unreal-scene`.

## Action reference

`*` means required; `[w]` mutates an asset.

| Action | Key params | Purpose |
|---|---|---|
| `get_status` | none | Report the typed PCG module state and live registered actions. |
| `list_graph_assets` | `package_path?`, `limit?` | Discover graph assets through AssetRegistry. |
| `get_graph_asset` | `asset_path*`, `include_tags?`, `tag_limit?` | Read bounded registry metadata without loading graph topology. |
| `list_components` | `limit?` | List PCG-like components in the editor world. |
| `list_pcg_node_types` | `query?`, `include_properties?`, `property_limit?`, `limit?` | Resolve concrete loaded `UPCGSettings` types and editable fields. |
| `create_pcg_graph` `[w]` | `asset_path*`, `existing_policy?=fail`, `save?=true` | Create a project-owned `UPCGGraph`; use `return_existing` for rerunnable automation. |
| `get_pcg_graph_info` | `asset_path*`, `include_settings?`, `settings_fields?`, `property_limit?`, `array_limit?`, `node_limit?`, `edge_limit?`, `pin_limit?`, `response_item_limit?` | Read special/element nodes, pins, edges, positions, settings classes, and bounded values. |
| `add_pcg_node` `[w]` | `asset_path*`, `node_type*`, `node_title?`, `position?`, `properties?`, `existing_policy?=fail`, `save?=true` | Add a typed settings node; a stable title plus `return_existing` makes the step rerunnable. |
| `remove_pcg_node` `[w]` | `asset_path*`, `node_id*`, `save?=true` | Remove an element node and incident edges; special graph input/output nodes are protected. |
| `connect_pcg_nodes` `[w]` | graph, source node/pin, target node/pin, `save?` | Validate ownership, direction, direct data compatibility, acyclicity, and target capacity, then add an edge idempotently. |
| `disconnect_pcg_nodes` `[w]` | graph, source node/pin, target node/pin, `save?` | Remove one exact edge; a missing edge returns `not_connected`. |
| `set_pcg_node_params` `[w]` | `asset_path*`, `node_id*`, `properties*`, `dry_run?`, `save?` | Strictly preflight and write editable settings through the canonical reflection walker. |
| `validate_pcg_graph` | `asset_path*`, connectivity options, `issue_limit?` | Validate node ownership/settings and edge endpoints, direction, type, capacity, duplicates, cycles, ids, isolation, and output connectivity with bounded issue arrays. |
| `remap_graph_references` `[w]` | `asset_path*`, `root_remaps*`, guards/bounds | Dry-run or confirm a bounded soft-reference migration, including scalar property-bag paths. |

## Author a rerunnable graph

```text
pcg_query("create_pcg_graph", {
  "asset_path": "/Game/PCG/PCG_Scatter",
  "existing_policy": "return_existing",
  "save": true
})

pcg_query("add_pcg_node", {
  "asset_path": "/Game/PCG/PCG_Scatter",
  "node_type": "PCGAddTagSettings",
  "node_title": "AddGameplayTag",
  "existing_policy": "return_existing",
  "position": [320, 0],
  "save": true
})

pcg_query("set_pcg_node_params", {
  "asset_path": "/Game/PCG/PCG_Scatter",
  "node_id": "AddGameplayTag",
  "properties": { "TagsToAdd": "Gameplay.PCG" },
  "dry_run": true
})
# Repeat with dry_run=false after inspecting the field report.
```

Connect only labels returned by `get_pcg_graph_info`. Use `__input__` and `__output__` for the special nodes. Repeat `connect_pcg_nodes` safely: an existing edge returns `already_connected` without saving again. Finish with `get_pcg_graph_info(include_settings=true)` and `validate_pcg_graph`.

## Migrate copied graph references

Run `remap_graph_references` with `dry_run=true`, inspect every candidate and target-resolution row, then repeat with `dry_run=false`, `confirm=true`, and `save=true`. Finish with `asset.validate_dependency_closure` and another dry-run; an idempotent migration reports no remaining candidates.

## Safety and gotchas

- Keep authoring under project-owned mounted packages. Exact object paths must match the package leaf and may not address subobjects.
- Discover unfamiliar settings with `list_pcg_node_types(include_properties=true)`, then use `set_pcg_node_params(dry_run=true)` before committing.
- Provide exact class paths when a friendly node type is ambiguous. Discovery includes native and currently loaded project settings classes; unloaded Blueprint-generated settings may require loading first.
- Do not treat `UPCGGraph::AddEdge`/`AddLabeledEdge` return values as success. Monolith validates pins first and confirms the resulting topology.
- Connections that require a PCG filter/conversion node are not inserted implicitly. Add that node explicitly. A single-connection input is never silently replaced; disconnect its existing edge first.
- Settings writes honor the concrete object's `CanEditChange` rules and full nested property chain. A dry-run stages the same strict write on a transient duplicate; commit emits property-specific editor callbacks and restores the exact prior values/topology/dirty state if validation or save fails.
- Graph mutations run the same bounded structural validator before persistence. This pre-save check is the commit boundary; a late validator is intentionally not used because it could report failure after the package file had already changed.
- Use `pin_limit` and `response_item_limit` when inspecting large or project-defined settings graphs. Check `response_truncated`, per-node pin truncation flags, and returned counts before assuming a response is complete.
- `remap_graph_references` is a migration surface, not a substitute for typed node/settings editing.
- Component graph assignment, generation polling/output inspection, cleanup, and graph user-parameter schema editing are not part of this 14-action snapshot. Confirm the live catalog before assuming they exist.
