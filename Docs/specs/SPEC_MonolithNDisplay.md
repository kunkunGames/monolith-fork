# Monolith - MonolithNDisplay Module

**Parent:** [SPEC_CORE.md](../SPEC_CORE.md)
**Engine:** Unreal Engine 5.7+
**Version:** 0.14.10
**Status:** Implemented

---

## 1. Purpose

`MonolithNDisplay` owns the `ndisplay` namespace for optional nDisplay / DisplayCluster config discovery. The first slice is read-only and dependency-light: it reports DisplayCluster module availability and lists nDisplay config-like assets through AssetRegistry metadata only. It does not include DisplayCluster headers, load configs, edit projection policies, or save assets.

---

## 2. Ownership

| Class | Responsibility |
|-------|----------------|
| `FMonolithNDisplayModule` | Registers and unregisters the `ndisplay` namespace. |
| `FMonolithNDisplayActions` | Implements read-only status and config asset listing handlers. |

| Dependency | Purpose |
|------------|---------|
| `MonolithCore` | Tool registry, action result, and parameter schema contracts. |
| `AssetRegistry` | Read-only nDisplay / DisplayCluster config-like asset discovery. |
| `Json`, `JsonUtilities` | Action response payloads. |

---

## 3. Action Surface

| Action | Params | Description |
|--------|--------|-------------|
| `ndisplay.get_status` | none | Reports optional DisplayCluster module availability and future action boundaries. |
| `ndisplay.list_config_assets` | `package_path`?, `limit`? | Lists DisplayCluster/nDisplay config-like registry rows under `/Game`. |

---

## 4. Safety Contract

| Gate | Requirement |
|------|-------------|
| Optional dependency | No DisplayCluster include paths or module dependencies. |
| Path boundary | `package_path` must be `/Game` or below `/Game/`. |
| Read-only behavior | Actions must not load configs, mutate nodes/viewports/projection policies, create transactions, or save packages. |
| Output bounds | `limit` clamps to `1..500`. |

---

## 5. Verification Gates

| Gate | Evidence |
|------|----------|
| Registration | `FMonolithNDisplayModule::StartupModule` registers `ndisplay.get_status` and `ndisplay.list_config_assets`. |
| Ownership cleanup | `MonolithMesh` no longer registers or unregisters the `ndisplay` namespace. |
| UE 5.7 build | Full plugin UBT build must succeed with the engine root resolved from the host `.uproject`. |
