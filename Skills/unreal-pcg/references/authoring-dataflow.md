# UE 5.8 PCG intent and data-flow planning

Use this reference to translate a procedural-content request into a graph plan before calling generic Monolith PCG authoring actions. It is a planning guide, not a node or action catalog. The live editor decides which `UPCGSettings` classes, editable properties, pins, and direct data-type connections exist.

## Authority and boundaries

1. Call `monolith_discover({ namespace: "pcg" })` and require the expected live surface before planning mutations. The checked-in contract has 28 actions, but a rebuilt or differently configured editor may expose another surface.
2. Call the per-action schema for every action used. Never invent an action or parameter from this guide.
3. Resolve unfamiliar nodes with `list_pcg_node_types`. Treat its concrete class path and property descriptors as discovery evidence, not as permission to write every reflected field.
4. Read actual nodes, pins, labels, types, and settings with `get_pcg_graph_info`. Never infer a pin label from a node title, class name, screenshot, or a different engine version.
5. Monolith authors arbitrary loaded settings nodes through `add_pcg_node`, `set_pcg_node_params`, dedicated `set_pcg_subgraph` assignment, and exact pin connections. `replace_pcg_graph_contents` is a mechanical whole-graph copy from an already-authored source into an existing target identity, not a semantic mega-action that designs or chooses a graph for the caller.
6. Scalar source-graph user-parameter schema/default authoring is supported through `set_pcg_graph_user_parameters`; component graph-instance scalar overrides remain a separate `set_component_user_parameters` step.

## Convert intent into an explicit contract

Write a short graph contract before selecting nodes. A useful contract answers all of these questions:

| Question | Evidence required before authoring |
|---|---|
| What owns the graph and where may it be saved? | Exact project-owned graph path and intended owning actor/component. |
| What is the source data? | Existing graph input contract or a live-discovered source settings class. |
| What does one source item represent? | Point, spatial input, actor-derived data, or another type reported by the actual pins. |
| Which items survive? | An explicit spatial, metadata, density, or project rule; do not substitute a silent default. |
| How are surviving items changed? | Required transform, attribute, seed, or other settings expressed as exact discovered fields. |
| What is materialized? | Graph output data, generated actors/components, or another result with a measurable acceptance criterion. |
| What must remain deterministic? | Inputs, component seed, graph settings, existing overrides, and expected comparison fields. |
| What are the cost bounds? | Expected source count, acceptable output count, and allowed generated-resource count. |

If any answer is unknown, inspect the graph, actor, or live node catalog first. Do not hide missing design data with an arbitrary node, asset, class, seed, or property value.

## Plan the data flow in stages

Use the smallest graph that expresses the requested result. Describe it as stages before naming settings classes:

```text
source
-> selection or sampling
-> optional filtering
-> optional attribute/transform work
-> realization or graph output
```

Not every graph needs every stage. Each stage must have a reason and a measurable postcondition.

### 1. Source

Identify whether data comes from the graph input, a generated primitive set, or a project/world source represented by a loaded settings class. Query the live node catalog using the user's domain terms, inspect candidate descriptions/properties, then add only the selected concrete class.

For an existing graph, begin from `get_pcg_graph_info`; preserve a valid source contract unless the requested change explicitly replaces it. A special node is addressed only by the returned `__input__` or `__output__` identity and actual pin label.

### 2. Selection or sampling

State the desired distribution and bounds in domain terms before setting properties: count, spacing, extents, allowed region, exclusion rule, or comparable project constraint. Discover a node whose loaded settings and pins can express that rule. If the catalog does not expose a suitable settings class, report the capability gap rather than approximating it with an unrelated node.

### 3. Filtering

Add a filter only when a rule removes or separates data. Record which input field drives the decision and what should happen to rejected data. Monolith never inserts a required PCG filter implicitly: when pin compatibility reports that a filter is required, discover and add an explicit suitable node or stop with the incompatibility.

### 4. Attribute or transform work

Apply only fields required by the contract. First run `set_pcg_node_params` with `dry_run=true`, inspect the complete field report, then commit the same property tree. Coordinate-space, metadata, class-valued, nested, or project-defined fields must come from live descriptors and read-back; do not copy property names or enum text from memory.

Graph user-parameter schema is not a generic graph/settings property write. Use `set_pcg_graph_user_parameters`, dry-run first, and provide an explicit non-null default for every add/update/type-change. Supported types are `bool`, `byte`, `int32`, `int64`, `float`, `double`, `name`, and `string`; unknown removals and duplicate/conflicting names fail closed. Define the source schema before relying on component override automation.

Static subgraph identity is not a generic editable settings field. Add a concrete `UPCGSubgraphSettings` node, then call `set_pcg_subgraph` with the exact case-sensitive canonical object path of the project-owned graph or graph instance; redirectors and path aliases are intentionally rejected. Dry-run before commit, and treat active overrides, graph-customization filter rejection, or any self/recursive hierarchy as hard design errors rather than bypassing them with reflected writes.

### 5. Realization and output

Decide whether the graph must expose data through graph output or create managed world resources. Define the expected result before connecting the last edge. For actor realization, use the exact discovered class-valued property contract and exact project asset/class path; do not fall back to a default class when the requested class cannot be resolved.

Require an output connection when downstream graph users depend on graph output. A graph that intentionally performs only managed realization may still need project-specific acceptance evidence; do not equate structural validity alone with a useful result.

## Select node types without guessing

For every unfamiliar stage:

1. Query `list_pcg_node_types` with a narrow domain term and `include_properties=true`.
2. Inspect result truncation and raise the bounded limit only if the candidate set is incomplete.
3. Prefer an exact concrete class path when a friendly name is ambiguous.
4. Add the node with a stable authored title and `existing_policy=return_existing` only when rerun behavior is required and an exact class/title match is acceptable.
5. Read the node back through `get_pcg_graph_info(include_settings=true)`.
6. Use only returned input/output pin labels.
7. Dry-run settings, apply, read back, and compare to the planned contract.

The verified lifecycle sample is a concrete example, not a universal template:

```text
PCGCreatePointsGridSettings
-> PCGTransformPointsSettings
-> PCGSpawnActorSettings
-> graph output
```

Those settings classes are confirmed by the checked-in sample and tests. For surface, spline, landscape, metadata, density, subgraph, GPU, or project-defined work, discover the current loaded settings classes and schemas; do not derive a class name from those concepts.

## Connect topology from read-back

Use this loop for each edge:

```text
get_pcg_graph_info
-> choose exact source node/output pin and target node/input pin
-> connect_pcg_nodes
-> read back topology
```

`connect_pcg_nodes` accepts only direct compatibility. If the response reports a required filter or conversion, add an explicit discovered node. It will not silently replace a single-connection input; disconnect the exact old edge first when replacement is intentional. Repeating the same exact edge is an idempotent `already_connected` result.

## Review the semantic shape

Before execution, challenge the graph with these questions:

- Is every node required by the written graph contract?
- Does every branch terminate in an intentional consumer or graph output?
- Are selection and realization counts bounded by known inputs rather than hope?
- Are class and asset references exact and project-owned where required?
- Are coordinate space, seed, settings, and graph-instance overrides explicit enough to reproduce the result?
- Did any truncated catalog, property, pin, edge, or settings response influence the design?
- Does `validate_pcg_graph` pass with `require_output_connection` and `require_no_isolated_nodes` set according to the actual contract?

Structural validation proves ownership and topology invariants. It does not prove that the chosen settings express the user's intended procedural design. That requires execution and outcome verification from `verification-recipes.md`.

## Promote a verified donor into a canonical graph identity

Use `replace_pcg_graph_contents` only after the source graph has already passed the semantic review above. Resolve distinct exact clean project-owned `UPCGGraph` source and target assets, verify that the donor is not recursive and does not contain the target in its static-subgraph hierarchy, inspect complete bounded topology on both, and run the replacement with its default `dry_run=true`. The structurally validated preview must report the expected source counts, target before counts, preserved target identity, and exact persistent-property verification. Commit the identical request only with `dry_run=false` and `confirm=true`, then reload and repeat complete read-back/validation. This path preserves the canonical target object/package identity, default input/output objects, and the target-owned `UPCGGraphInterface::LastEditedDocuments` editor workspace while copying the source's donor-owned graph-level settings, user-parameter property bag, special-node pin contracts, element/settings/editor subobjects, cross-node references, embedded subobjects, and exact edges. `LastEditedDocuments` is the explicit identity-bound root-state exception: it is excluded from donor/target equality and the target value must survive preview, commit, rollback, save, and reload; it does not permit any broader node-, pin-, or type-specific comparison skip. The action is not a substitute for authoring an unknown graph, and it must never be approximated with raw package-file overwrite or a partial node loop.
