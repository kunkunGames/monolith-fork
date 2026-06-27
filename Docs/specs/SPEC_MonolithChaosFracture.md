# Monolith - MonolithChaosFracture Module

**Parent:** [SPEC_CORE.md](../SPEC_CORE.md)
**Engine:** Unreal Engine 5.7+
**Version:** 0.14.10
**Status:** Implemented

---

## 1. Purpose

`MonolithChaosFracture` owns the `chaos_fracture` namespace for optional Geometry Collection and Fracture visibility. The first slice is read-only: it probes module/type availability, lists Geometry Collection-like assets, and lists reflected Geometry Collection-like components in the current editor world. It does not include Fracture headers, load Fracture tooling, run fracture operations, or mutate assets.

---

## 2. Ownership

| Class | Responsibility |
|-------|----------------|
| `FMonolithChaosFractureModule` | Registers and unregisters the `chaos_fracture` namespace. |
| `FMonolithChaosFractureActions` | Implements module/type status, asset listing, and component listing handlers. |

| Dependency | Purpose |
|------------|---------|
| `MonolithCore` | Tool registry, action result, and parameter schema contracts. |
| `AssetRegistry` | Read-only Geometry Collection-like asset discovery. |
| `Engine`, `UnrealEd` | Current editor-world component reflection. |
| `Json`, `JsonUtilities` | Action response payloads. |

---

## 3. Action Surface

| Action | Params | Description |
|--------|--------|-------------|
| `chaos_fracture.get_status` | none | Reports optional Geometry Collection / Fracture module and reflected type availability. |
| `chaos_fracture.list_geometry_collection_assets` | `package_path`?, `limit`? | Lists Geometry Collection-like registry rows under `/Game`. |
| `chaos_fracture.list_geometry_collection_components` | `limit`? | Lists reflected Geometry Collection-like components in the current editor world. |

---

## 4. Safety Contract

| Gate | Requirement |
|------|-------------|
| Optional dependency | No GeometryCollection or Fracture include paths or module dependencies. |
| Path boundary | `package_path` must be `/Game` or below `/Game/`. |
| Read-only behavior | Actions must not load Fracture tools, run fracture recipes, create transactions, save assets, or mutate components. |
| Output bounds | `limit` clamps to `1..500`. |

---

## 5. Verification Gates

| Gate | Evidence |
|------|----------|
| Registration | `FMonolithChaosFractureModule::StartupModule` registers the three `chaos_fracture` actions. |
| Routing cleanup | `MonolithMesh` no longer registers `mesh.get_chaos_fracture_status`, `mesh.list_geometry_collection_assets`, or `mesh.list_geometry_collection_components`. |
| UE 5.7 build | Full plugin UBT build must succeed with the engine root resolved from the host `.uproject`. |
