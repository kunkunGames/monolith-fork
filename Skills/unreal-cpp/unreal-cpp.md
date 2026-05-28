---
name: unreal-cpp
description: Use when writing or debugging Unreal Engine C++ code via Monolith MCP — engine API lookup, signature verification, include paths, source reading, class hierarchies, config resolution. Triggers on C++, header, include, UCLASS, UFUNCTION, UPROPERTY, Build.cs, linker error.
---

# Unreal C++ Development Workflows

**28 source actions** via `source_query()`, **6 config actions** via `config_query()` (see `unreal-config`).

```
monolith_discover({ namespace: "source" })
monolith_discover({ namespace: "config" })
```

## Source Actions

| Action | Key Params | Purpose |
|--------|-----------|---------|
| `search_source` | `query` | Find symbols across engine source |
| `read_source` | `symbol` | Read engine source for a symbol |
| `get_class_hierarchy` | `symbol` | Inheritance tree |
| `find_callers` | `symbol` | Who calls this function |
| `find_callees` | `symbol` | What this function calls |
| `find_references` | `symbol` | All references to a symbol |
| `get_module_info` | `symbol` | Module dependencies, build type |
| `get_symbol_context` | `symbol` | Definition + surrounding context |
| `read_file` | `file_path` | Raw engine source file |
| `trigger_reindex` | -- | Full engine source re-index |
| `trigger_project_reindex` | -- | Incremental project-only re-index |

## Code Review & Risk (use BEFORE making review claims)

| Action | Key Params | Purpose |
|--------|-----------|---------|
| `risk_score` | `symbol`, `min_tier`? | Change-risk tier for a symbol + its dependents |
| `review_context` | `symbol`, `direction`?, `detail_level`? | Reviewer context bundle (callers/callees/types) |
| `review_hotspots` | `kind`?, `min_lines`? | Project-wide hotspots: fan_in/fan_out/risk/large/override |
| `impact_radius` | `symbol`, `edge_kinds`?, `direction`?, `max_depth`? | Blast radius across call/type/inheritance/override edges |
| `find_overrides` | `symbol`, `direction`?, `max_depth`? | Override-only traversal for virtual/override function edits |
| `find_unused` | `kind`?, `min_confidence`? | Candidate unused functions/classes/structs |
| `detect_changes` | `changed_paths`/`diff_file`/`diff_stdin` | Symbols impacted by a diff |
| `pre_merge_check` | `changed_paths`?, `include_unused`? | Combined impact + unused pre-merge summary |
| `snapshot` / `diff_snapshots` | `label` / `before`,`after` | Capture and diff source-graph snapshots |

## CRG graph & index maintenance

| Action | Key Params | Purpose |
|--------|-----------|---------|
| `search_crg_graph` | `query`, `kind`? | Search the CRG-compatible graph node export (FTS5, LIKE fallback) |
| `build_crg_graph` / `rebuild_crg_graph` | `execute`, `force`? | Explicitly build/rebuild `Saved\graph.db` from `EngineSource.db`; temp-DB validate + atomic replace, skips when source signature is current unless forced |
| `crg_graph_health` | -- | CRG graph schema/FTS/export health; reserved flow/community/risk tables are not populated health criteria |
| `health` | `include_counts`? | Source index health |
| `repair_fts` | `target`?, `execute` | Rebuild FTS when search looks stale |
| `repair_crg_cache` | `scope`?, `execute` | Rebuild CRG projection/cache plus signature-aware override edge cache |

Default C++ lookup/review work should use `search_source`, `risk_score`, `review_context`, and `health`. `build_crg_graph --execute` is graph export/search maintenance, not routine setup; live editor/MCP execute returns `status=started` plus `poll_action=source.crg_graph_health`, while offline CLI execute remains synchronous. `risk_score` and `review_context` read the EngineSource `crg_*` projection/cache, not `Saved\graph.db`.
`impact_radius` defaults to `call|type|inheritance`. For virtual method edits, call `find_overrides` with a qualified symbol such as `UActorComponent::BeginPlay`, or explicitly pass `edge_kinds=call|type|inheritance|override` when override traversal should be mixed into the broader blast radius; unqualified method names can match several same-name class methods and are useful only when that broad fan-out is intentional.
`find_overrides`, `impact_radius`, `risk_score`, `review_context`, and `review_hotspots kind=override` use the `source_override_edges` cache when `source.health` shows `source_override_edges_version=1`; otherwise they fall back to query-time signature matching. If only the override cache/version is stale, run `source repair_crg_cache --scope=override_edges --execute`; use full `source repair_crg_cache --execute` for stale `crg_nodes`, `crg_edges`, or `crg_node_metrics` parity.
Use `review_hotspots kind=override` to find high-fanout virtual/override methods before broad API changes. `Saved\graph.db` flow/community/risk auxiliary tables are reserved placeholders and are not source-risk inputs.

## Common Workflows

```
// Find and read an API
source_query({ action: "search_source", params: { query: "ApplyDamage" } })
source_query({ action: "read_source", params: { symbol: "UGameplayStatics::ApplyDamage" } })

// Learn idiomatic usage from Epic's code
source_query({ action: "find_callers", params: { symbol: "UPrimitiveComponent::SetCollisionEnabled" } })

// Check override blast radius before editing a virtual function
source_query({ action: "find_overrides", params: { symbol: "UActorComponent::BeginPlay", direction: "in", max_depth: 2 } })
source_query({ action: "review_hotspots", params: { kind: "override", limit: 10 } })

// Resolve config/CVar
config_query({ action: "resolve_setting", params: { file: "DefaultEngine", section: "/Script/Engine.RendererSettings", key: "r.Lumen.TraceMeshSDFs" } })
config_query({ action: "explain_setting", params: { setting: "r.DefaultFeature.AntiAliasing" } })
```

## Build.cs Gotchas

| Error | Fix |
|-------|-----|
| `LNK2019` for `UDeveloperSettings` | Add `"DeveloperSettings"` module (separate from `Engine`) |
| `LNK2019` for any UE type | Check module with `get_module_info`, add to Build.cs |
| Missing `#include` | Use `search_source` to find correct header -- never guess |
| Template instantiation | Check if type needs `_API` export macro |

## UE 5.7 Notes

- `FSkinWeightInfo`: `uint16` for `InfluenceWeights` (not uint8), `FBoneIndexType` for bones
- `CreatePackage` with same path returns existing in-memory package -- use unique names
- Live Coding: `.cpp` body changes only -- header changes require editor restart + UBT build

## Rules

- **Never guess** `#include` paths or signatures -- always verify with `source_query`
- Search action is `search_source` (not `search`)
- Source index: engine Runtime/Editor/Developer + plugins + shaders (1M+ symbols)
- Use `find_callers` for idiomatic usage, `get_symbol_context` for quick definition lookup
- Use `config_query("explain_setting")` before changing unfamiliar CVars
- Non-existent actions: `get_include_path`, `get_function_signature`, `get_deprecation_warnings`
