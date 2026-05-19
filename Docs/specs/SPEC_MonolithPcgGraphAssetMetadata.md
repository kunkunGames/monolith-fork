# Monolith PCG Graph Asset Metadata First Slice

**Parent:** [SPEC_MonolithPCG.md](SPEC_MonolithPCG.md)
**Engine:** Unreal Engine 5.7+
**Status:** Implemented in `MonolithPCG`

---

## 1. Scope

This slice extends the Monolith-native PCG visibility surface without adding a
hard dependency on the PCG plugin. The actions live in the dedicated `pcg`
namespace and are registered by `FMonolithPCGModule`.

| Action | Purpose |
|--------|---------|
| `pcg.get_status` | Report optional PCG module/type availability and current/future action boundaries. |
| `pcg.list_graph_assets` | List PCG graph-like AssetRegistry rows under `/Game`. |
| `pcg.get_graph_asset` | Return bounded AssetRegistry metadata for one PCG graph-like asset under `/Game`. |
| `pcg.list_components` | List PCG-like components in the current editor world by reflected class identity. |

---

## 2. Contract

`pcg.get_graph_asset` is read-only and AssetRegistry-only. It accepts:

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `asset_path` | string | yes | PCG graph-like package or object path under `/Game`. |
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
| Path boundary | Reject paths outside `/Game`. |
| Mutation boundary | Do not load, save, execute, compile, or mutate PCG graph assets. |
| Output bounds | Clamp tag rows and truncate long tag values. |

---

## 4. Verification

| Gate | Evidence |
|------|----------|
| Registration | `FMonolithPCGActions::RegisterActions` registers `pcg.get_graph_asset`. |
| Parameter guard | Automation test rejects an out-of-project asset path. |
| Build | `MonolithPCG` compiles under Unreal Engine 5.7 with no PCG dependency. |
