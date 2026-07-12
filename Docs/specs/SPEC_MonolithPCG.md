# Monolith - MonolithPCG Module

**Parent:** [SPEC_CORE.md](../SPEC_CORE.md)
**Engine:** Unreal Engine 5.7+
**Version:** 0.20.3
**Status:** Implemented

---

## 1. Purpose

`MonolithPCG` owns 14 `pcg` actions spanning AssetRegistry discovery, typed `UPCGGraph` asset authoring and read-back, structural validation, world-component listing, and guarded copied-graph soft-reference migration. Authoring uses public PCG graph/node/pin APIs rather than reflected writes to graph topology. PCG component assignment/generation and graph user-parameter schema editing remain out of scope.

## 2. Ownership and dependencies

| Class | Responsibility |
|---|---|
| `FMonolithPCGModule` | Registers both baseline and typed authoring surfaces, then unregisters the namespace. |
| `FMonolithPCGActions` | Status, registry discovery/metadata, component listing, and guarded reference remapping. |
| `FMonolithPCGGraphAuthoringActions` | Graph creation, topology/settings read-back, node/edge/settings mutation, and validation. |
| `FMonolithPCGSettingsResolver` | Deterministic resolution of concrete loaded `UPCGSettings` classes and friendly aliases. |

| Dependency | Purpose |
|---|---|
| `MonolithCore` | Registry, schema, result, asset-path, reflection-reader/walker, and dry-run contracts. |
| `PCG` | Public `UPCGGraph`, `UPCGNode`, `UPCGPin`, `UPCGEdge`, and `UPCGSettings` APIs. |
| `AssetRegistry`, `UnrealEd`, `Engine` | Asset creation/registration/save and current editor-world reads. |
| `CoreUObject` `StructUtils/PropertyBag.h` | Dynamic graph-parameter soft-reference migration; no deprecated StructUtils plugin dependency is required. |
| `Json`, `JsonUtilities` | Params and bounded response payloads. |

`Monolith.uplugin` explicitly enables the engine `PCG` plugin. `PCGEditor` is intentionally not linked: graph asset mutation uses runtime PCG public APIs plus editor asset lifecycle APIs.

## 3. Action surface

| Action | Class | Mutation | Summary |
|---|---|---:|---|
| `pcg.get_status` | baseline | no | Typed PCG readiness plus dynamically enumerated current actions. |
| `pcg.list_graph_assets` | baseline | no | Bounded AssetRegistry discovery. |
| `pcg.get_graph_asset` | baseline | no | One bounded registry metadata record. |
| `pcg.list_components` | baseline | no | Bounded editor-world component rows. |
| `pcg.remap_graph_references` | baseline | guarded | Dry-run/confirm bounded reference migration. |
| `pcg.list_pcg_node_types` | authoring | no | Concrete loaded settings classes, aliases, and optional editable-field summaries. |
| `pcg.create_pcg_graph` | authoring | yes | Project-owned graph creation; `existing_policy=return_existing` supports reruns. |
| `pcg.get_pcg_graph_info` | authoring | no | Special/element nodes, pins, edges, positions, settings class, and bounded settings values. |
| `pcg.add_pcg_node` | authoring | yes | Typed node creation with title/position/strict initial settings and rerun policy. |
| `pcg.remove_pcg_node` | authoring | yes | Element-node and incident-edge removal; special nodes rejected. |
| `pcg.connect_pcg_nodes` | authoring | yes | Ownership/direction/type preflight plus topology postcondition; idempotent. |
| `pcg.disconnect_pcg_nodes` | authoring | yes | Exact edge removal with idempotent missing-edge result. |
| `pcg.set_pcg_node_params` | authoring | guarded | Strict canonical reflection-walker dry-run/apply against one settings object. |
| `pcg.validate_pcg_graph` | authoring | no | Structural errors/warnings, isolation, and optional output-connectivity gates. |

The registry-only single-asset contract remains in [SPEC_MonolithPcgGraphAssetMetadata.md](SPEC_MonolithPcgGraphAssetMetadata.md).

## 4. Typed authoring contract

1. Normalize the destination to a top-level project-owned mounted package and exact package-leaf object path.
2. Resolve settings types from live reflection. Reject abstract/deprecated classes and ambiguity; accept canonical class paths.
3. Reject hidden/library-internal settings types. Preflight every settings leaf against edit flags, instance `CanEditChange` (including its full property chain), and a transient duplicate written by the canonical strict reflection walker.
4. Apply settings one leaf at a time under matching `PreEditChange(FEditPropertyChain)` / `PostEditChangeChainProperty` callbacks so PCG settings that rebuild pins or cached data receive the same property identity as editor Details-panel edits.
5. Mutate graph structure only through `UPCGGraph::AddNodeOfType`, `RemoveNode`, `AddLabeledEdge`, and `RemoveEdge`. Reject self/cyclic edges, implicit replacement of single-connection inputs, and connections that require an implicit filter/conversion node.
6. Batch graph editor notifications with RAII, prime compilation state before edits, re-enable notifications before emitting the strongest supported external-modification notification, and confirm the actual post-mutation topology. Never interpret `AddLabeledEdge`'s boolean as success.
7. Snapshot the exact root settings values, incident topology, object identity, and package dirty state needed by each mutation. On property, topology, structural-validation, or save failure, explicitly restore that snapshot before returning an error.
8. Run bounded structural validation before every save. This in-handler pre-save check is the authoritative commit boundary; the actions deliberately do not use a late execution-guard validator because a failure after persistence cannot roll back the package file. Save through `UEditorAssetSubsystem::SaveLoadedAsset`, then require a non-empty package file when persistence was requested.
9. Return stable `node_id` values (`__input__`, `__output__`, or the element UObject name), authored titles, pin labels, settings class paths, mutation status, and save evidence.

## 5. Safety and bounds

| Gate | Requirement |
|---|---|
| Package boundary | Only mounted packages whose resolved files are inside the current project directory are accepted for typed mutation. Subobjects and mismatched package/object leaves are rejected. |
| Rerun policy | Create/add default to `fail`. `return_existing` is allowed only for the same graph type or same authored-title settings class. |
| Topology | Special graph input/output nodes cannot be removed. Both endpoints must belong to the graph; labels, direction, and directly compatible PCG data types must match. Self/cyclic edges and implicit single-input replacement are rejected; callers disconnect explicitly first. |
| Settings | Only editable, non-transient, non-deprecated, non-edit-const properties accepted by the concrete settings instance and full property chain are writable. Dry-run stages the same write on a transient duplicate and is side-effect free. |
| Validation | Duplicate ids/edges, ownership, missing settings, endpoint-pin ownership, attachment at both ends, directions/types, single-input overflow, and directed cycles are checked before save. Cycle detection is linear in graph size; issue arrays are bounded and expose total/returned/truncated counts. |
| Bounds | Element nodes are capped at 5,000, graph edges at 20,000, node types at 1,000, and validation issues at 1,000 per returned severity array. Read-back property/array/node/edge/pin bounds are caller-clamped, and `response_item_limit` is shared across node, pin, edge, and nested-settings serialization. |
| Migration | Reference remap defaults dry-run, requires confirmation for writes, enforces project ownership and object/reference caps, and reports unsupported dynamic containers. |

## 6. Verification gates

| Gate | Evidence |
|---|---|
| Registration/policy | `FMonolithPCGGraphAuthoringRegistrationTest` checks all 14 actions and guarded mutation policies; `Guard.PreSaveValidationPolicy` confirms the six graph mutators use the atomic pre-save boundary instead of non-atomic late validation. |
| Type resolution | `FMonolithPCGSettingsResolverTest` verifies exact class-name/path resolution and unknown-type rejection. |
| Lifecycle/guards | Focused automation creates/saves/reloads a graph, persists settings and connections, rejects self/cycle/single-input/title-ambiguity violations, detects half-attached edges, exercises response bounds and disconnect/remove/cleanup, and verifies the pre-save policy; existing path/remap tests retain project-boundary and confirmation coverage. |
| Engine compatibility | Authoring uses APIs present in UE 5.7 and 5.8; 5.8-only `EPCGChangeType::ExternalModification` is version-gated, and edge enumeration scans public pin edge arrays instead of using 5.8-only graph helpers. |
| Benchmark | AssetEditing task `BEB-546` creates/reuses a graph and node, applies settings, repeats edges, reads back topology/settings, and validates the graph without a delete/reset step. |
| Build/live | Run full `SpeedEditor` UBT for structural changes, recover the MCP endpoint, run `Monolith.PCG`, then run the focused PCG AssetEditing module twice. |
