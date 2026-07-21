---
name: unreal-pcg
description: Use when creating, editing, inspecting, validating, or migrating Unreal PCG graphs and assigning graphs to exact Blueprint PCG-component templates or editor-world UPCGComponent instances through Monolith MCP. Covers typed graph authoring, node/pin/settings work, validation, exact-path template/live component configuration, asynchronous generate/poll/refresh/cancel/cleanup, bounded output/resource inspection, graph-instance user-parameter overrides, and guarded reference remapping. For fixed blockout generation use unreal-worldgen.
---

# unreal-pcg

Drive the Monolith `pcg` namespace for typed PCG graph-asset work, scalar graph user-parameter schema/default authoring, exact-SCS Blueprint PCG-template assignment, and exact-path editor-world component execution. The checked-in surface has **28 actions**, including **11 component actions**. Treat `monolith_discover({ namespace: "pcg" })` as authoritative for the running editor.

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

## Expert workflow routing

After live action discovery, read only the references required by the request. These files teach planning and verification; they are not action catalogs. The running `monolith_discover` catalog and per-action schema remain authoritative, and an action, parameter, node class, property, or pin must never be inferred from a reference.

- Read [`references/authoring-dataflow.md`](references/authoring-dataflow.md) before designing a new graph, changing topology, choosing unfamiliar settings nodes, translating a world-building intent into PCG data flow, or reviewing whether a graph is semantically well formed. A mechanical read or exact known-property edit does not require it.
- Read [`references/runtime-debug-performance.md`](references/runtime-debug-performance.md) before choosing `generation_trigger` or partitioning, making a determinism claim, diagnosing empty/wrong output, investigating interrupted lifecycle work, or evaluating graph cost. It explicitly separates observable Monolith evidence from UE behavior that this 28-action surface does not profile.
- Read [`references/verification-recipes.md`](references/verification-recipes.md) before completing persistent graph/component work, changing more than one node or lifecycle field, or accepting generated output. Use its smallest applicable recipe; metadata-only inspection does not require a mutation recipe.
- For a full author-and-run request, read the references in the order above. Re-run live discovery after an editor rebuild or reconnect rather than relying on the checked-in 28-action snapshot.

## Action reference

`*` means required; `[w]` mutates persistent editor data; `[x]` mutates live component execution state.

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
| `set_pcg_graph_user_parameters` `[w]` | `asset_path*`, `upsert?`, `remove?`, `dry_run?=true`, `save?=true` | Atomically add/update/type-change/remove up to 256 scalar graph parameters. Every upsert requires `name`, supported `type`, and explicit `default_value`; full-range `int64` uses a canonical decimal string. |
| `set_pcg_subgraph` `[w]` | `asset_path*`, `node_id*`, `subgraph_asset_path*`, `dry_run?=false`, `save?=true` | Assign an exact project-owned graph or graph instance to `UPCGSubgraphSettings` through UE's `SetSubgraph` API, with filter/recursion guards, exact read-back, incident-topology preservation, and rollback. |
| `replace_pcg_graph_contents` `[w]` | `source_asset_path*`, `target_asset_path*`, `dry_run?=true`, `confirm?=false`, `save?=true`, `node_limit?=5000`, `edge_limit?=20000` | Replace the target's donor-owned persistent graph object state from a distinct exact clean project-owned `UPCGGraph`, preserving target identity and its identity-bound `LastEditedDocuments` workspace through seeded CoreUObject duplication. Recursive sources and donors containing the target fail closed. Commit requires `dry_run=false` and `confirm=true`; exact no-op skips save. |
| `validate_pcg_graph` | `asset_path*`, connectivity options, `issue_limit?` | Validate node ownership/settings and edge endpoints, direction, type, capacity, duplicates, cycles, ids, isolation, and output connectivity with bounded issue arrays. |
| `remap_graph_references` `[w]` | `asset_path*`, `root_remaps*`, guards/bounds | Dry-run or confirm a bounded soft-reference migration, including scalar property-bag paths. |
| `create_component` `[w]` | `actor_path*`, `component_name?=PCGComponent`, `existing_policy?=fail`, `graph_asset_path?`, `seed?=42`, `activated?=true`, `partitioned?=false`, `generation_trigger?=on_demand`, `generate_on_drop_when_on_demand?=false`, `save?=true` | Create and register one transactional `UPCGComponent` on an exact user-authored actor path in canonical engine order; return its canonical component path. `APCGPartitionActor` is rejected. Policy is `fail`/`return_existing`; trigger is `on_load`/`on_demand`/`at_runtime`; seed is signed 32-bit. |
| `get_component` | `component_path*`, `include_user_parameters?=true`, `user_parameter_limit?=256` (1-256), `include_managed_resource_count?=false` | Poll one exact component's graph/settings/state. Task fields are `generation_task_valid`/`generation_task_id` and `cleanup_task_valid`/`cleanup_task_id`; ids are always strings. Exact managed-resource enumeration is opt-in because it is potentially expensive. |
| `set_component_graph` `[w]` | `component_path*`, `graph_asset_path*`, `save?=true` | Assign the exact graph to an idle, ungenerated component after editability and graph-instance-cycle checks, verify read-back, and optionally save the owning level. |
| `set_blueprint_component_graph` `[w]` | `blueprint_asset_path*`, `component_name*`, `graph_asset_path*`, `dry_run?=true`, `confirm?=false`, `save?=true` | Resolve one exact Actor Blueprint SCS `UPCGComponent` template, preview by default, then assign on explicit confirmation; compile and re-resolve the reconstructed template, require exact read-back, optionally save, and restore/recompile on failure. |
| `set_component_settings` `[w]` | `component_path*`, `seed?`, `activated?`, `partitioned?`, `generation_trigger?`, `generate_on_drop_when_on_demand?`, `save?=true` | Atomically update an explicit valid final component configuration on an idle, ungenerated component and verify normalized read-back. `generate_on_drop_when_on_demand=true` requires `on_demand`; partitioning requires `CanPartition()`. |
| `generate_component` `[x]` | `component_path*`, `force?=false` | Call native `GenerateLocalGetTaskId`, preserving UE `ShouldGenerate` dirty-regeneration semantics, and return immediately with any 64-bit task id as a decimal string. |
| `refresh_component` `[x]` | `component_path*` | Schedule or coalesce native component refresh and return current state without waiting; replacement/cancellation is intentionally unsupported. |
| `cancel_component` `[x]` | `component_path*` | Request cancellation of the current generation task only. Active refresh/cleanup and runtime-scheduler ownership are rejected. |
| `cleanup_component` `[x]` | `component_path*`, `remove_components?=true` | Schedule native cleanup, report any cleanup task id as a string, and never busy-wait. If cleanup is already running, the response separates the new requested mode from the unobservable in-flight mode. |
| `get_component_output` | `component_path*`, `output_limit?=100`, `tag_limit?=100`, `include_managed_resources?=true`, `resource_limit?=100`, `managed_object_limit?=100` (all limits 1-500) | Read deterministic bounded tagged-data and managed-resource summaries when idle. Each row returns sorted `tags` plus `tag_count`, `tag_returned`, `tag_limit`, and `tags_truncated`. |
| `set_component_user_parameters` `[w]` | `component_path*`, `values?` object, `reset?` array, `dry_run?=false`, `save?=true` | Stage and atomically commit/reset bounded strict-scalar graph-instance overrides, verify canonical read-back, and optionally save the owning level. String/export text is capped at 4,096 characters; floating point must be finite/in range. `int64` accepts an integral JSON number only in the exact 53-bit range or a canonical decimal string across the full signed 64-bit range; read-back stays an exact decimal string. |

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

For a `UPCGSubgraphSettings` node, do not attempt to write `SubgraphInstance` or `SubgraphOverride` through `set_pcg_node_params`. Call `set_pcg_subgraph` with the exact graph/interface path, run it first with `dry_run=true`, then commit the identical request with `dry_run=false`. The action rejects an active override, parent-filter rejection, self-reference, and recursive parent/child hierarchies; successful commit verifies the exact interface and concrete graph before save.

When an already-authored donor graph must replace every donor-owned persistent field/topology of a canonical target without changing the target asset identity, use `replace_pcg_graph_contents`; do not overwrite or rename package files. Supply exact source/target paths and bounded counts, leave `dry_run` omitted or true for the structurally validated transient preview, inspect the before/after node/edge/user-parameter counts and `persistent_properties_verified`, then repeat with `dry_run=false`, `confirm=true`, and the intended `save` value. Both packages must be clean exact `UPCGGraph` assets, and the donor must neither be recursive nor contain the target anywhere in its static-subgraph hierarchy. The action copies graph-level properties, `FInstancedPropertyBag`, special input/output settings and pins, element settings/editor properties, embedded subobjects, cross-node references, and exact edges while retaining the target object path/pointer and default input/output object identities. Seeded destination objects are generically reset to their archetype baseline before replay so an archetype-default source value replaces a stale non-default target value even when CoreUObject omits that value from its duplicate delta stream. The reusable source/target inners are matched by an indexed complete outer/name/class lineage built from one nested enumeration per root. Persistent equality then uses a bounded non-recursive object-graph walk: every reachable owned object and duplicable property is processed once through package-persistent archive semantics, owned references compare by relative lineage, and external references compare by exact path. Nested reflected `Transient`/deprecated cache fields are excluded generically as they are during package saving, while editor-only fields that really persist are still compared. The sole explicit target-owned boundary is root `UPCGGraphInterface::LastEditedDocuments`: exclude that identity-bound editor workspace state from donor/target equality and preserve the target's original value through preview, commit, rollback, save, and reload. This boundary does not permit node-, pin-, or other type-specific skips; all donor-owned persistent state remains exact. The comparison fails closed above 250,000 owned objects, 256 outer levels, or 128 MiB of canonical property payload per side; do not replace it with recursive `PPF_DeepComparison`, which repeatedly revisits shared/cyclic PCG inners on large graphs. Property-bag equality is exact over the ordered serialized descriptors and canonically serialized values, not the transient `UPropertyBag` script-struct pointer that UE may reconstruct after duplicate/save/reload; one descriptor or value difference still requires replacement. A failed commit reports whether snapshot/dirty-bit rollback was verified.

For a reusable Actor Blueprint class, assign the graph on the exact SCS `UPCGComponent` template with `set_blueprint_component_graph`; do not mutate an arbitrary live instance and assume that change updates the class default. Supply the canonical Blueprint asset path, exact SCS variable name, and graph path. Leave dry-run omitted or true first, then repeat with `dry_run=false`, `confirm=true`, and the intended `save` value. A commit must compile the Blueprint, re-resolve the reconstructed component template, and verify exact graph-interface read-back; compile, read-back, or save failure triggers graph restoration and a rollback compile.

## Drive an editor-world component

Discover every lifecycle schema first, especially after rebuilding or reconnecting the editor:

```text
monolith_discover({ namespace: "pcg", action: "create_component", mode: "schema" })
monolith_discover({ namespace: "pcg", action: "set_component_graph", mode: "schema" })
monolith_discover({ namespace: "pcg", action: "set_component_user_parameters", mode: "schema" })
monolith_discover({ namespace: "pcg", action: "get_component_output", mode: "schema" })
```

Use `list_components` to obtain canonical identities for existing instances. For a new instance, resolve the exact actor object path, call `create_component`, and carry the returned `component_path` into every subsequent action. The preferred persistent order is create without a graph, apply settings, assign the graph, then poll. This avoids trying another mutation while graph assignment has an editor refresh pending:

```text
pcg_query("create_component", {
  "actor_path": "<exact actor object path>",
  "component_name": "PCG_MonolithSample",
  "existing_policy": "return_existing",
  "seed": 42,
  "activated": true,
  "partitioned": false,
  "generation_trigger": "on_demand",
  "generate_on_drop_when_on_demand": false,
  "save": true
})
# Carry the exact component_path from the response.

pcg_query("set_component_settings", {
  "component_path": "<same exact component path>",
  "seed": 1337,
  "activated": true,
  "partitioned": false,
  "generation_trigger": "on_demand",
  "generate_on_drop_when_on_demand": false,
  "save": true
})

pcg_query("set_component_graph", {
  "component_path": "<returned exact component path>",
  "graph_asset_path": "/Game/PCG/PCG_MonolithSample.PCG_MonolithSample",
  "save": true
})

pcg_query("get_component", {
  "component_path": "<same exact component path>",
  "include_user_parameters": true,
  "user_parameter_limit": 256,
  "include_managed_resource_count": false
})
```

If a workflow assigns the graph before a later settings mutation, call `get_component` in separate requests until `refresh_in_progress=false` before continuing. Do not assume graph assignment is synchronously idle.

Define graph parameters before creating component overrides. Call `set_pcg_graph_user_parameters` with its default `dry_run=true`, inspect the atomic change rows, then repeat with `dry_run=false`. Supported types are `bool`, `byte`, `int32`, `int64`, `float`, `double`, `name`, and `string`; every upsert requires an explicit non-null default, unknown removals fail, and the action never invents a fallback default. After assigning that graph, read exact names through `get_component` before calling `set_component_user_parameters`.

If the returned `user_parameters` contains a graph-defined scalar that should be overridden, stage the complete batch first. Replace the placeholder key with the exact returned parameter name; this action does not create graph parameter schemas:

```text
pcg_query("set_component_user_parameters", {
  "component_path": "<same exact component path>",
  "values": { "<exact existing parameter name>": 0.35 },
  "reset": [],
  "dry_run": true,
  "save": true
})
# Inspect the canonical read-back, then repeat with dry_run=false.
```

Execution is schedule-and-poll:

```text
pcg_query("generate_component", {
  "component_path": "<same exact component path>",
  "force": false
})
# Record generation_task_valid and the decimal-string generation_task_id. On later MCP calls:
pcg_query("get_component", {
  "component_path": "<same exact component path>",
  "include_user_parameters": false
})
# Repeat separate get_component calls until generating=false and cleaning_up=false.
pcg_query("get_component_output", {
  "component_path": "<same exact component path>",
  "output_limit": 2,
  "tag_limit": 2,
  "include_managed_resources": true,
  "resource_limit": 2,
  "managed_object_limit": 2
})
pcg_query("cleanup_component", {
  "component_path": "<same exact component path>",
  "remove_components": true
})
```

For every output row, inspect `tag_count`, `tag_returned`, `tag_limit`, and `tags_truncated` before treating its sorted `tags` array as complete. Also inspect the top-level output/resource bounds and each resource's managed-object truncation state. Then raise `output_limit`, `tag_limit`, `resource_limit`, and `managed_object_limit` only as needed. Cleanup returns `cleanup_task_valid` and string `cleanup_task_id`; poll `get_component` until idle, then generate once more to prove the saved setup is rerunnable. Use parameterless `refresh_component` for an already generated component. A second refresh request coalesces with the active one; the action has no replace/cancel parameter. Use `cancel_component` only when generation is actually active, never for refresh or runtime-scheduler-owned work.

## Migrate copied graph references

Run `remap_graph_references` with `dry_run=true`, inspect every candidate and target-resolution row, then repeat with `dry_run=false`, `confirm=true`, and `save=true`. Finish with `asset.validate_dependency_closure` and another dry-run; an idempotent migration reports no remaining candidates.

## Safety and gotchas

- Keep authoring under project-owned mounted packages. Exact object paths must match the package leaf and may not address subobjects.
- Treat actor and component labels as display text only. Lifecycle mutations require exact active-editor-world paths, and the returned `component_path` is the identity token for later calls. Never guess a short name or retain a raw pointer across calls.
- Never create on an `APCGPartitionActor`. Every component owned by a partition actor is inspection-only regardless of its current local-component flag: `get_component` and `get_component_output` accept it and expose `original_component_path`; the other seven component-targeted lifecycle mutations reject it. Mutate the exact original component instead.
- Component creation follows the engine-safe sequence: validate inputs/save/graph/name, allocate and set initial scalars, add as an instance component, register, apply partitioning, validate the graph-instance chain, assign, verify, then dirty/save. Do not reconstruct this sequence from lower-level actions.
- `set_component_graph` rejects locked/non-editable graph instances and recursive graph-instance chains. `set_component_settings` validates the final combination, including on-demand/drop coupling and actor partition support, before applying any field.
- `set_blueprint_component_graph` is the persistent class-template route. It requires one exact SCS variable on a project-owned Actor Blueprint, previews by default, requires `dry_run=false` plus `confirm=true` to commit, and recompiles/re-resolves the reconstructed template before accepting read-back. It does not fall back to inherited, runtime, or ambiguous components.
- Generation and cleanup are asynchronous schedule-and-poll operations. Check `generation_task_valid`/`cleanup_task_valid`, preserve `generation_task_id`/`cleanup_task_id` as decimal strings, and poll with separate `get_component` calls; do not loop, sleep, manually tick the editor, or busy-wait inside an action.
- `generate_component` deliberately delegates to UE `GenerateLocalGetTaskId`/`ShouldGenerate`; dirty generated state remains eligible for regeneration. Refresh coalesces rather than replacing an active request. When cleanup is already active, inspect `requested_remove_components`, `inflight_remove_components_known`, and `coalescing_status`; never treat the second request's mode as the mode of the existing task. Cancellation is generation-only and refuses refresh, cleanup, and runtime-scheduler-owned components.
- Read generated output only after `get_component` reports `output_accessible=true` (`generating=false`, `cleaning_up=false`, and `refresh_in_progress=false`). `output_limit`, `tag_limit`, `resource_limit`, and `managed_object_limit` are independently required to stay in 1-500. A returned row's sorted `tags` array is complete only when `tags_truncated=false` and `tag_returned=tag_count`.
- Keep `include_managed_resource_count=false` for normal polling. Set it to `true` only when an exact managed-resource count is required; that request explicitly enumerates the resource set. Mutation responses omit user-parameter rows, so fetch them deliberately with `get_component(include_user_parameters=true)`.
- Managed-resource rows are inspection summaries, not loaded asset handles. Do not use output inspection to synchronously load, mutate, or retain resources.
- User-parameter writes target the assigned component graph **instance**, not the source graph schema. Submit one complete batch so it can be staged on a copied property bag and committed or rolled back atomically; clearing an override removes its override identity and restores inheritance. String and export-text inputs/read-back are bounded to 4,096 characters, `float`/`double` must remain finite and `float` must fit its finite range. For `int64`, use a JSON number only inside the exact 53-bit range; use a canonical decimal string such as `"9223372036854775807"` for full-range values. Leading zeroes, `+`, whitespace, `-0`, overflow, and trailing text are rejected without mutation. Check row/value truncation fields before treating the response as complete.
- A value-identical request with `save=true` still crosses the idle/ungenerated mutation guard; it cannot be used to save a generated or otherwise active component. A failed rollback restores prior dirty bits only when rollback is complete; if any live partial mutation survives, the component/actor/level packages remain dirty and the error reports the incomplete rollback.
- The nine mutating/scheduling component actions prepare source control for the exact live-component or Blueprint-template package only after validation/no-op/coalescing/confirmation gates and before the first side effect. Inspect `source_control_prepare` for the handler-owned prepare or explicit transient/provider skip. Asynchronous partition/external packages may appear only after PCG completion, so finish with changelist asset validation rather than assuming the scheduling-time package set is complete.
- Use `save=true` only when component creation, assignment, configuration, or parameter overrides must persist in the owning level. Generate, refresh, cancel, cleanup, status, and output calls change or inspect live execution state and do not imply a saved edit.
- Discover unfamiliar settings with `list_pcg_node_types(include_properties=true)`, then use `set_pcg_node_params(dry_run=true)` before committing.
- Assign static subgraphs only with `set_pcg_subgraph`. Generic reflection does not own `SubgraphInstance`; use an exact project-owned graph or graph-instance path, dry-run first, and inspect exact interface/concrete-graph read-back after commit.
- Replace a complete canonical graph only with `replace_pcg_graph_contents`; raw `.uasset` overwrite, package rename/swap, node-by-node reconstruction, and generic reflected assignment cannot preserve the same identity plus arbitrary inner-object reference topology. Preserve the target-owned `LastEditedDocuments` editor workspace instead of importing donor document paths. Default dry-run is side-effect free, including source-control preparation; commit requires explicit confirmation.
- Provide exact class paths when a friendly node type is ambiguous. Discovery includes native and currently loaded project settings classes; unloaded Blueprint-generated settings may require loading first.
- Do not treat `UPCGGraph::AddEdge`/`AddLabeledEdge` return values as success. Monolith validates pins first and confirms the resulting topology.
- Connections that require a PCG filter/conversion node are not inserted implicitly. Add that node explicitly. A single-connection input is never silently replaced; disconnect its existing edge first.
- Settings writes honor the concrete object's `CanEditChange` rules and full nested property chain. A dry-run stages the same strict write on a transient duplicate; commit emits property-specific editor callbacks and restores the exact prior values/topology/dirty state if validation or save fails.
- Graph mutations run the same bounded structural validator before persistence. This pre-save check is the commit boundary; a late validator is intentionally not used because it could report failure after the package file had already changed.
- Use `pin_limit` and `response_item_limit` when inspecting large or project-defined settings graphs. Check `response_truncated`, per-node pin truncation flags, and returned counts before assuming a response is complete.
- `remap_graph_references` is a migration surface, not a substitute for typed node/settings editing.
- Graph user-parameter **schema** creation/deletion is not part of this surface. Component graph-instance override editing is supported; confirm the live catalog before assuming any newer schema-authoring action exists.
