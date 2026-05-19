# Monolith - MonolithDataflow Module

**Parent:** [SPEC_CORE.md](../SPEC_CORE.md)
**Engine:** Unreal Engine 5.7+
**Version:** 0.14.10
**Status:** Implemented

---

## 1. Purpose

`MonolithDataflow` owns the `dataflow` namespace for optional Dataflow discovery. The first slice is read-only and dependency-light: it reports module availability and lists Dataflow-like assets through AssetRegistry metadata only. It does not include Dataflow headers, load graph assets, evaluate graphs, regenerate assets, or require the Dataflow plugin at compile time.

---

## 2. Ownership

| Class | Responsibility |
|-------|----------------|
| `FMonolithDataflowModule` | Registers and unregisters the `dataflow` namespace. |
| `FMonolithDataflowActions` | Implements read-only status and AssetRegistry listing handlers. |

| Dependency | Purpose |
|------------|---------|
| `MonolithCore` | Tool registry, action result, and parameter schema contracts. |
| `AssetRegistry` | Read-only Dataflow-like asset discovery. |
| `Json`, `JsonUtilities` | Action response payloads. |

---

## 3. Action Surface

| Action | Params | Description |
|--------|--------|-------------|
| `dataflow.get_status` | none | Reports Dataflow/Chaos graph module availability, implemented actions, and future action boundaries. |
| `dataflow.list_assets` | `package_path`?, `limit`? | Lists Dataflow-like registry rows under `/Game`. |

---

## 4. Safety Contract

| Gate | Requirement |
|------|-------------|
| Optional dependency | No Dataflow include paths or module dependencies. |
| Path boundary | `package_path` must be `/Game` or below `/Game/`. |
| Read-only behavior | Actions must not load, evaluate, regenerate, save, or mutate Dataflow assets. |
| Output bounds | `limit` clamps to `1..500`. |

---

## 5. Verification Gates

| Gate | Evidence |
|------|----------|
| Registration | `FMonolithDataflowModule::StartupModule` registers `dataflow.get_status` and `dataflow.list_assets`. |
| Routing cleanup | `MonolithMesh` no longer registers `mesh.get_dataflow_status` or `mesh.list_dataflow_assets`. |
| UE 5.7 build | Full plugin UBT build must succeed with the engine root resolved from `GO.uproject`. |
