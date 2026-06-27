# Monolith - MonolithDataflow Module

**Parent:** [SPEC_CORE.md](../SPEC_CORE.md)
**Engine:** Unreal Engine 5.7+
**Version:** 0.14.10
**Status:** Implemented

---

## 1. Purpose

`MonolithDataflow` owns the `dataflow` namespace for optional Dataflow discovery and read-only graph inspection. The always-on slice reports module availability and lists Dataflow-like assets through AssetRegistry metadata only. When UE 5.7 Dataflow runtime headers are available and `MONOLITH_RELEASE_BUILD` is not set, Monolith also compiles bounded graph, node factory, variable, comment, and validation inspectors. The module never mutates, evaluates, regenerates, saves, or marks Dataflow assets dirty.

---

## 2. Ownership

| Class | Responsibility |
|-------|----------------|
| `FMonolithDataflowModule` | Registers and unregisters the `dataflow` namespace. |
| `FMonolithDataflowActions` | Implements read-only status, AssetRegistry listing, and optional Dataflow graph inspection handlers. |

| Dependency | Purpose |
|------------|---------|
| `MonolithCore` | Tool registry, action result, and parameter schema contracts. |
| `AssetRegistry` | Read-only Dataflow-like asset discovery. |
| `UnrealEd` | Editor graph node/comment types used by optional graph inspection when Dataflow is compiled in. |
| `DataflowCore`, `DataflowEngine` | Optional compile-time graph inspection dependencies, enabled only when runtime source headers are present and `MONOLITH_RELEASE_BUILD!=1`. |
| `Json`, `JsonUtilities` | Action response payloads. |

---

## 3. Action Surface

| Action | Params | Description |
|--------|--------|-------------|
| `dataflow.get_status` | none | Reports Dataflow/Chaos graph module availability, implemented actions, and future action boundaries. |
| `dataflow.list_assets` | `package_path`?, `limit`? | Lists Dataflow-like registry rows under `/Game`. |
| `dataflow.get_dataflow_graph` | `asset_path`, `node_limit`?, `connection_limit`?, `include_properties`? | Loads one `UDataflow` asset and returns bounded graph nodes, pins, editable property snapshots, and connections whose endpoints are included in the returned node slice without mutation. |
| `dataflow.list_dataflow_node_types` | `filter`?, `common_only`?, `limit`?, `include_pins`? | Lists registered Dataflow node factory types and optional default pin summaries. |
| `dataflow.get_dataflow_node_schema` | `type_name`, `include_properties`? | Returns schema, pins, and optional editable default properties for one registered Dataflow node type. |
| `dataflow.validate_dataflow_graph` | `asset_path` | Reports duplicate node identifiers and broken connection references without mutation. |
| `dataflow.list_dataflow_variables` | `asset_path` | Lists `UDataflow` property bag variables, type metadata, and serialized values. |
| `dataflow.list_dataflow_comments` | `asset_path`, `node_limit`? | Lists Dataflow editor comment boxes and bounded contained-node hints. |

---

## 4. Safety Contract

| Gate | Requirement |
|------|-------------|
| Optional dependency | `dataflow.get_status` and `dataflow.list_assets` do not include Dataflow headers. Graph inspection actions compile only with `WITH_MONOLITH_DATAFLOW=1`; `MONOLITH_RELEASE_BUILD=1` forces the optional dependencies off. |
| Path boundary | `package_path` must be `/Game` or below `/Game/`. |
| Asset boundary | `asset_path` resolves to a Dataflow object path and rejects missing/non-Dataflow assets with structured errors. |
| Read-only behavior | Actions may load Dataflow assets for inspection but must not evaluate, regenerate, save, or mutate Dataflow assets. |
| Output bounds | Asset `limit` and graph/comment `node_limit` clamp to `1..500`; graph `connection_limit` clamps to `1..5000`; node type `limit` clamps to `1..1000`. |

---

## 5. Verification Gates

| Gate | Evidence |
|------|----------|
| Registration | `FMonolithDataflowModule::StartupModule` always registers `dataflow.get_status` and `dataflow.list_assets`; when `WITH_MONOLITH_DATAFLOW=1`, it also registers the six graph inspection actions. |
| Routing cleanup | `MonolithMesh` no longer registers `mesh.get_dataflow_status` or `mesh.list_dataflow_assets`. |
| UE 5.7 build | Full plugin UBT build must succeed with the engine root resolved from the host `.uproject`. |
| Release build | UBT build with `MONOLITH_RELEASE_BUILD=1` must succeed and omit hard Dataflow runtime dependencies. |
