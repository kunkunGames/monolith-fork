---
name: unreal-project-search
description: Use when searching the indexed Unreal project via Monolith MCP (project namespace) — FTS5 asset search, discovery by name/type, asset-to-asset reference and dependency tracing, impact radius, gameplay-tag lookup, and asset-level risk/review-context/hotspots. For C++ symbol/text search, callers/callees, or class hierarchy use unreal-cpp; to cross from a found asset/BP node to its backing C++ symbol use unreal-bridge; for UCLASS/UPROPERTY reflection metadata or replication audits use unreal-reflection-intel; to act on a found asset (save/rename/move/delete/metadata) or fuzzy live find use unreal-asset; to gather results into an editor Collection use unreal-collection. Triggers on project search, find asset, search project, asset references, find references, where is this used, who references, dependencies, impact radius, find by type, FTS, full-text search, gameplay tags, asset risk score, review hotspots, find unused assets, ProjectIndex.
---

# Unreal Project Search Workflows

Drives the **project** namespace via `project_query()` over a deep, indexed project graph — FTS asset search, references, dependencies, type filtering, gameplay tags, and asset-level review/risk.

## Discovery

```
monolith_discover({ namespace: "project" })                                  // all project actions
monolith_discover({ namespace: "project", action: "search", mode: "schema" })  // exact params
```

## When to use / Use a different skill for

- **This skill (unreal-project-search / project namespace):** find project ASSETS by name or type, trace asset-to-asset references and dependencies, compute impact radius, look up GameplayTags, and gather asset-level risk/review-context/hotspots.
- **unreal-cpp** — the search target is C++ SOURCE (symbol/text search, callers/callees, references, class hierarchy), not project assets.
- **unreal-bridge** — cross from a found asset or Blueprint node to its backing C++ symbol (asset-to-symbol), versus asset-to-asset reference tracing here.
- **unreal-reflection-intel** — UCLASS/UPROPERTY/UFUNCTION reflection metadata or replication audits, versus FTS asset content search.
- **unreal-asset** — ACT on a found asset (save/rename/move/delete/metadata) or do a fuzzy live find, versus indexed project-wide search/references.
- **unreal-collection** — gather search results into an editor Collection, versus running the search itself.

## Asset Path Conventions

All asset paths follow UE content browser format (no .uasset extension):

| Location | Path Format | Example |
|----------|------------|--------|
| Project Content/ | `/Game/Path/To/Asset` | `/Game/Materials/M_Rock` |
| Project Plugins/ | `/PluginName/Path/To/Asset` | `/MassProjectile/Materials/M_Example` |
| Engine Plugins | `/PluginName/Path/To/Asset` | `/Niagara/DefaultAssets/SystemAssets/NS_Default` |

**Note:** For project plugins, the path starts with the plugin name as configured in the .uplugin file's "MountPoint" — which defaults to `/<PluginName>/`. Most plugins mount their Content/ folder there directly.

## Action Reference

Full per-action parameter signatures — grouped by category (Search & inspect, Gameplay tags, Review & risk, Snapshots & maintenance) — live in [`references/actions.md`](references/actions.md). Param notation: `name*` required, `name?` optional, `name=val` default, `a/b/c` allowed, `[w]` mutates (transaction-wrapped — for `impact_radius`/`risk_score`/`pre_merge_check`/`snapshot`/`repair_*` this is index-cache bookkeeping, not asset edits). Signatures are a snapshot of the live catalog — for the exact full schema call `monolith_discover` with `mode: "schema"` (the Discovery block above stays the authority).

> Asset **Collections** moved to their own namespace — see `unreal-collection`.
> The same `risk_score` / `review_context` / `impact_radius` review surface exists for C++ in `unreal-cpp` (`source` namespace).

## FTS5 Search Syntax

The `search` action uses SQLite FTS5 under the hood. By default it searches asset metadata, graph nodes, variables, parameters, DataTable row names, level actors, and curated supplemental values such as Blueprint comments and pin/default text.

| Pattern | Meaning |
|---------|---------|
| `BP_Enemy` | Match exact token |
| `BP_*` | Prefix match |
| `"BP_Enemy Health"` | Exact phrase |
| `BP_Enemy OR BP_Ally` | Either term |
| `BP_Enemy NOT Health` | Exclude term |
| `BP_Enemy NEAR/3 Health` | Terms within 3 tokens |

Search results expose provenance fields so agents can judge relevance without fetching the whole asset:

| Field | Meaning |
|-------|---------|
| `match_source` | One of `asset`, `node`, `variable`, `parameter`, `datatable_row`, `actor`, `supplemental_value` |
| `match_table` | Backing SQLite/FTS table for the hit |
| `match_field` | Field or object name that matched |
| `match_object_path` | Node/object path or asset path associated with the hit |
| `match_value` | Matched value payload when available |

### Actions

| Action | Purpose |
|--------|---------|
| `audit_orphan_assets` | List /Game/.../*.uasset assets with ZERO IAssetRegistry referencers AND zero entries in cpp_asset_edges. |

## Common Workflows

### 1. Find-and-trace: from a name to its blast radius

Search for the asset, list what references it (both directions), then bound the dependency blast radius before a rename/delete/refactor.

```
1. project_query({ action: "search", params: { query: "M_Skin*", include_content: false } })
2. project_query({ action: "find_references", params: { asset_path: "/Game/Materials/M_Skin" } })
3. project_query({ action: "impact_radius", params: { asset_path: "/Game/Materials/M_Skin", direction: "in", max_depth: 2, max_results: 200 } })
```

`find_references` returns assets that reference OR are referenced by the seed; `impact_radius` is a bounded BFS (`[w]` here is index-cache bookkeeping, not an asset edit). To cross from a found asset/BP node into its backing C++ symbol use **unreal-bridge**; for C++ symbol callers/callees use **unreal-cpp**.

### 2. Find-by-type plus gameplay-tag lookup

List every asset of a type (optionally filtered by module), then resolve which assets carry a GameplayTag.

```
1. project_query({ action: "find_by_type", params: { asset_type: "WidgetBlueprint", limit: 100 } })
2. project_query({ action: "list_gameplay_tags", params: { prefix: "Ability.", limit: 100 } })
3. project_query({ action: "search_gameplay_tags", params: { query: "Damage", limit: 100 } })
```

`search_gameplay_tags` returns the referencing assets per tag; `find_by_type` accepts `module` to scope to a plugin.

### 3. Review-hotspots pass before a change/merge

Rank project-wide hotspots, score the change-risk of a specific seed, then pull a token-efficient review package.

```
1. project_query({ action: "review_hotspots", params: { kind: "risk", limit: 50 } })
2. project_query({ action: "risk_score", params: { asset_path: "/Game/Blueprints/BP_Player", min_tier: "medium" } })
3. project_query({ action: "review_context", params: { asset_path: "/Game/Blueprints/BP_Player", direction: "both", max_depth: 2, detail_level: "standard" } })
```

The same `risk_score` / `review_context` / `impact_radius` review surface exists for C++ in **unreal-cpp** (`source` namespace); use it when the review target is source rather than an asset.

### Find any asset by name
```
project_query({ action: "search", params: { query: "BP_Player*" } })
```

### Find all Blueprints in the project
```
project_query({ action: "find_by_type", params: { asset_type: "Blueprint" } })
```

### Find all assets referencing a material
```
project_query({ action: "find_references", params: { asset_path: "/Game/Materials/M_Skin" } })
```

### Find references to a plugin asset
```
project_query({ action: "find_references", params: { asset_path: "/MassProjectile/Materials/M_Example" } })
```

### Get detailed metadata for an asset
```
project_query({ action: "get_asset_details", params: { asset_path: "/Game/Blueprints/BP_Player" } })
```

### Check index health
```
project_query({ action: "health", params: { include_counts: true } })
```

### Find all Niagara systems
```
project_query({ action: "find_by_type", params: { asset_type: "NiagaraSystem" } })
```

### Find assets by variable or parameter name
```
project_query({ action: "search", params: { query: "Health" } })
```

### Search only asset/node names when content matches are too broad
```
project_query({ action: "search", params: { query: "Health", include_content: false } })
```

## Supported Asset Types

The index covers these types for `find_by_type`:
- `Blueprint`, `WidgetBlueprint`, `AnimBlueprint`
- `Material`, `MaterialInstance`, `MaterialFunction`
- `NiagaraSystem`, `NiagaraEmitter`
- `AnimSequence`, `AnimMontage`, `BlendSpace`
- `Texture2D`, `StaticMesh`, `SkeletalMesh`
- `DataTable`, `CurveTable`, `SoundWave`

## Project-Intelligence Search Complements

`project_query` and `source_query` search assets and source symbols. The Reflection Intelligence (RI) namespaces are deterministic, $0-LLM search complements that answer higher-level structural and historical questions about the project's own artefacts. Scope: project game module + project plugins (marketplace gated, Epic engine built-ins excluded).

- **`cppreflect_query`** — search the C++ reflection graph: `find_class_specifier` (every UCLASS carrying a specifier — token-forgiving, alias-maps `Blueprintable`->`IsBlueprintBase`, case-insensitive; pair with `list_class_specifiers` to discover the queryable token vocabulary), `find_interface_impls` (every C++ UCLASS implementing a UINTERFACE), plus `get_uclass` / `list_uproperties` / `list_ufunctions` for a specific class.
- **`decision_query`** — find architectural decision records mined from the markdown corpus: `list_decisions` (filter by `path_filter` / `status` / `min_confidence`), `get_decision`, `find_supersession_chain`, `find_referent_decisions`, `list_stale`.
- **`risk_query`** — find git-derived hotspots and co-change relationships: `get_release_window_hotspots`, `get_hotspot_score`, `get_cochange_pairs`, `get_file_churn`, `list_conditional_gates`.

## Tips

- The index is built on first launch and auto-updates — use `monolith_reindex()` to force rebuild
- FTS5 search covers asset names, node names, variable names, parameter names, DataTable row names, level actors, Blueprint comments, pin/default text, and curated supplemental values
- Prefer the default `include_content=true` for discovery; use `include_content=false` only when content hits are too noisy for a name/type lookup
- Use `find_references` to understand dependency chains before deleting or renaming assets
- Combine with domain-specific tools: search first, then inspect with `blueprint_query`, `material_query`, etc.
- `health` reports FTS parity and stale index symptoms; if stale, run `repair_fts execute=true` or trigger `monolith_reindex()` depending on the warning
- `get_stats` shows last index time — if stale, trigger `monolith_reindex()`
- RI reflection tables refresh on Live Coding / lazy first-call; force a project-only rebuild with `reflect_query("rebuild_reflection_index")`
- Call `monolith_discover('namespace')` to see required/optional params for every action
