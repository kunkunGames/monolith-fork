# Monolith - MonolithPCG Module

**Parent:** [SPEC_CORE.md](../SPEC_CORE.md)
**Engine:** Unreal Engine 5.7+
**Version:** 0.14.10
**Status:** Implemented

---

## 1. Purpose

`MonolithPCG` owns the `pcg` namespace for optional PCG discovery. The first slice is intentionally read-only and dependency-light: it uses AssetRegistry metadata, module-status probes, and reflected class names only. It does not include PCG headers, does not load PCG graph assets, and does not require the PCG plugin at compile time.

---

## 2. Ownership

| Class | Responsibility |
|-------|----------------|
| `FMonolithPCGModule` | Registers and unregisters the `pcg` namespace. |
| `FMonolithPCGActions` | Implements read-only PCG status, graph-asset listing, single graph metadata, and component reflection handlers. |

| Dependency | Purpose |
|------------|---------|
| `MonolithCore` | Tool registry, action result, and parameter schema contracts. |
| `AssetRegistry` | Read-only PCG graph-like asset discovery and bounded metadata rows. |
| `UnrealEd`, `Engine` | Current editor-world access for reflected PCG-like component listing. |
| `Json`, `JsonUtilities` | Action response payloads. |

---

## 3. Action Surface

| Action | Params | Description |
|--------|--------|-------------|
| `pcg.get_status` | none | Reports optional PCG module availability, reflected type presence, implemented actions, and future action boundaries. |
| `pcg.list_graph_assets` | `package_path`?, `limit`? | Lists PCG graph-like registry rows under `/Game`. |
| `pcg.get_graph_asset` | `asset_path`, `include_tags`?, `tag_limit`? | Returns one bounded PCG graph-like registry row and optional bounded tags under `/Game`. |
| `pcg.list_components` | `limit`? | Lists PCG-like components in the current editor world by reflected class identity. |

The single-asset response contract is documented in [SPEC_MonolithPcgGraphAssetMetadata.md](SPEC_MonolithPcgGraphAssetMetadata.md).

---

## 4. Safety Contract

| Gate | Requirement |
|------|-------------|
| Optional dependency | No PCG or PCGEditor include paths, module dependencies, graph loads, or graph execution. |
| Path boundary | `package_path` and `asset_path` must be `/Game` or below `/Game/`. |
| Read-only behavior | Actions must not call `GetAsset()` on graph assets, mutate packages, create transactions, save assets, compile graphs, or execute PCG. |
| Output bounds | `limit` clamps to `1..500`; `tag_limit` clamps to `0..200`; long tag values are truncated. |

---

## 5. Verification Gates

| Gate | Evidence |
|------|----------|
| Registration | `FMonolithPCGModule::StartupModule` registers `pcg.get_status`, `pcg.list_graph_assets`, `pcg.get_graph_asset`, and `pcg.list_components`. |
| Parameter guard | `FMonolithParamGuardPCGGraphAssetRejectsUnsafePathTest` rejects filesystem paths before AssetRegistry lookup. |
| UE 5.7 build | Full plugin UBT build must succeed with the engine root resolved from the host `.uproject`. |
