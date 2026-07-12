# Monolith PCG Graph Asset Metadata First Slice

**Parent:** [SPEC_MonolithPCG.md](SPEC_MonolithPCG.md)
**Engine:** Unreal Engine 5.7+
**Status:** Implemented in `MonolithPCG`

---

## 1. Scope

This slice defines the AssetRegistry-only PCG visibility actions alongside the
module's typed authoring surface. These reads do not load graph packages. They
live in the dedicated `pcg` namespace and are registered by
`FMonolithPCGModule`. Guarded copied-graph reference migration is specified by
the parent module spec and does not change the read-only contract below.

| Action | Purpose |
|--------|---------|
| `pcg.get_status` | Report optional PCG module/type availability and current/future action boundaries. |
| `pcg.list_graph_assets` | List PCG graph-like AssetRegistry rows under a project-owned content mount. |
| `pcg.get_graph_asset` | Return bounded AssetRegistry metadata for one PCG graph-like asset under a project-owned content mount. |
| `pcg.list_components` | List PCG-like components in the current editor world by reflected class identity. |

---

## 2. Contract

`pcg.get_graph_asset` is read-only and AssetRegistry-only. It accepts:

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `asset_path` | string | yes | PCG graph-like package or object path inside the current project checkout. |
| `include_tags` | bool | no | Include bounded AssetRegistry tag rows. Default: `true`. |
| `tag_limit` | integer | no | Maximum tag rows to return. Clamped to `0..200`; default `50`. |

The response includes package/object identity, class path, loaded state,
`source="asset_registry"`, `read_only=true`, and tag count/truncation metadata
when tags are requested. Tag values are truncated per field so the action cannot
dump unbounded editor metadata.

---

## 3. Guardrails

| Guardrail | Requirement |
|-----------|-------------|
| Dependency boundary | Do not include PCG headers or add PCG/PCGEditor dependencies. |
| Path boundary | Reject filesystem paths, engine packages, external mounts, and packages outside the current project checkout. |
| Mutation boundary | Do not load, save, execute, compile, or mutate PCG graph assets. |
| Output bounds | Clamp tag rows and truncate long tag values. |

---

## 4. Verification

| Gate | Evidence |
|------|----------|
| Registration | `FMonolithPCGActions::RegisterActions` registers `pcg.get_graph_asset`. |
| Parameter guard | Automation test rejects an out-of-project asset path. |
| Build | `MonolithPCG` compiles under Unreal Engine 5.7+ with an explicit PCG dependency for its typed authoring surface; these metadata handlers remain AssetRegistry-only. |
