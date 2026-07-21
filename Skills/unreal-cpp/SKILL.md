---
name: unreal-cpp
description: Use when reading, writing, or reviewing Unreal Engine C++ source via Monolith MCP (source namespace) — engine API lookup, signature verification, include paths, reading headers/source, class hierarchies, callers/callees, references, module dependencies, and code-review risk/review-context/impact-radius. For UCLASS/UPROPERTY reflection metadata, replication, or RPC/OnRep audits use unreal-reflection-intel; to cross from an asset/BP node to its C++ symbol use unreal-bridge; for project-asset search use unreal-project-search; for compiling or fixing build errors use unreal-build; for log/crash forensics use unreal-debugging. Triggers on C++, header, include, UCLASS, UFUNCTION, UPROPERTY, Build.cs, linker error, find function, callers of, callees, class hierarchy, where is symbol defined, function signature, read header, module dependency, API lookup, virtual override, risk score, review context, impact radius.
---

# Unreal C++ Development Workflows

Drives the **source** namespace via `source_query()` for engine/project C++ source lookup, reading, hierarchy traversal, and code-review risk. For `.ini`/CVar/config resolution use the **unreal-config** skill (11 `config_query()` actions); the convenience config signatures below cover only the calls shown here.

```
monolith_discover({ namespace: "source" })                               // all source actions
monolith_discover({ namespace: "source", action: "search_source", mode: "schema" })  // exact params
```

## When to use / Use a different skill for

- **This skill (unreal-cpp / source namespace):** read engine or project C++ source, verify a signature, find callers/callees/references, walk a class hierarchy, check module dependencies, and gather code-review risk/impact context.
- **unreal-reflection-intel** — UCLASS/UPROPERTY/UFUNCTION reflection metadata, replication audits, or RPC/OnRep handler checks (as-declared reflected surface, not raw source).
- **unreal-bridge** — cross from an asset/BP node to its backing C++ symbol.
- **unreal-project-search** — the search target is project ASSETS, references, or dependencies, not C++ source.
- **unreal-build** — the task is compiling or fixing build errors rather than reading source.
- **unreal-debugging** — diagnosing a linker/crash error via editor logs or crash context rather than verifying a signature.
- **unreal-config** — read/edit `.ini` settings, sections, or console variables.

## Source Actions

Param notation: `name*` required, `name?` optional, `name=val` default, `a/b/c` allowed values, `[w]` mutates. Signatures are a snapshot of the live catalog — for the exact, full, current schema of any action call `monolith_discover` with `mode: "schema"`.

| Action | Params (req* opt? =default) | Purpose |
|--------|------------------------------|---------|
| `search_source` | `query*`, `scope?` (all/engine/shaders), `mode?` (fts/regex/exact), `module?`, `path_filter?`, `symbol_kind?`, `cursor?`, `limit=50` | Find symbols across engine source |
| `read_source` | `symbol*` (aliases: query/name/path), `include_header=false`, `members_only=false`, `max_lines=500`, `start_line=1`, `end_line?`, `line_count?` | Read engine source for a symbol |
| `get_class_hierarchy` | `symbol*` (alias: class_name), `direction=both` (up/down/both), `depth=5` | Inheritance tree |
| `find_callers` | `symbol*`, `limit=50` | Who calls this function |
| `find_callees` | `symbol*`, `limit=50` | What this function calls |
| `find_references` | `symbol*`, `ref_kind?`, `limit=50` | All references to a symbol |
| `get_module_info` | `module_name*` | Module dependencies, build type |
| `get_symbol_context` | `symbol*`, `context_lines=10` | Definition + surrounding context |
| `read_file` | `file_path*` (alias: path), `start_line=1`, `end_line?`, `line_count?`, `max_lines?` | Raw engine source file |
| `audit_module_dep_reality` | `scan_root?`, `cursor?`, `limit=50` (cap 200) | Find UE type refs whose module is missing from the owner Build.cs deps (catches LNK2019 softptr_uproperty_needs_module_dep) |
| `[w] trigger_reindex` | (no params) | Full engine source re-index (live editor only) |
| `[w] trigger_project_reindex` | (no params) | Incremental project-only re-index (live editor only) |

`get_module_info` takes `module_name`, NOT `symbol`. `read_source`/`read_file` accept either a symbol or a disk path.

`trigger_reindex` / `trigger_project_reindex` need a running editor: offline `Plugins\Monolith\Binaries\monolith_query.exe` rejects them with `live_only` guidance instead of reindexing. When the MCP endpoint is down, recover it first (`powershell -File Plugins\Monolith\Scripts\recover_mcp.ps1`) or use the UBT-build path below.

## Code Review & Risk (use BEFORE making review claims)

All `risk_score`/`impact_radius`/`pre_merge_check`/`snapshot` calls wrap a transaction (`[w]`) for dirty-package tracking even though they are read-style; they still answer review questions, not edit project files.

| Action | Params (req* opt? =default) | Purpose |
|--------|------------------------------|---------|
| `[w] risk_score` | `symbol*`, `min_tier=low` (low/medium/high), `limit=10` | Change-risk tier for a symbol + its dependents |
| `review_context` | `symbol*`, `direction=both` (in/out/both), `max_depth=2`, `max_results=200`, `detail_level=minimal` (minimal/standard) | Reviewer context bundle (callers/callees/types) |
| `review_hotspots` | `kind=all` (fan_in/fan_out/risk/large/override/all), `min_lines=100`, `include_questions=true`, `limit=50` | Project-wide hotspots |
| `[w] impact_radius` | `symbol*`, `edge_kinds=call\|type\|inheritance` (append \|override; \|include warns), `direction=both` (in/out/both), `max_depth=2`, `max_results=200` | Blast radius across call/type/inheritance/override edges |
| `find_overrides` | `symbol*`, `direction=both` (in=child overrides/out=overridden parents/both), `max_depth=2`, `max_results=200`, `detail_level=minimal` (minimal/standard) | Override-only traversal for virtual/override function edits |
| `find_unused` | `kind=all` (function/class/struct/all), `min_confidence=low` (low/medium/high), `limit=100` | Candidate unused functions/classes/structs |
| `detect_changes` | `changed_paths?` (alias: paths; array or comma-string), `changed_ranges?`, `diff_text?`, `detail_level=minimal` (minimal/standard), `max_results=200` | Symbols impacted by a diff |
| `[w] pre_merge_check` | `changed_paths?` (alias: paths), `include_unused=true`, `unused_limit=20`, `detail_level=minimal`, `max_results=200` | Combined impact + unused pre-merge summary |
| `[w] snapshot` | `execute=false`, `label?` (defaults source-<utc_ticks>) | Capture source-graph CRG projection snapshot |
| `diff_snapshots` | `before*`, `after=current`, `limit=100` | Diff two stored/current snapshots |

`detect_changes` has NO `diff_file`/`diff_stdin` params — pass a unified diff via `diff_text`, paths via `changed_paths`, or line-precise edits via `changed_ranges` (`[{path, ranges:[[start,end]]}]`).

## CRG node search & index maintenance

| Action | Params (req* opt? =default) | Purpose |
|--------|------------------------------|---------|
| `search_crg_graph` | `query*`, `kind?`, `limit=20` | Search the CRG-compatible file/symbol node projection in `EngineSource.db` (FTS5, zero-hit LIKE fallback) |
| `health` | `include_counts=false`, `include_deep_checks=false` | Source index health |
| `[w] repair_fts` | `execute=false`, `target=all` (all/symbols/graph_nodes/console_objects/source) | Rebuild FTS when search looks stale (dry-run unless execute) |
| `[w] repair_crg_cache` | `execute=false`, `scope=all` (all/override_edges) | Rebuild CRG projection/cache plus signature-aware override edge cache (dry-run unless execute) |

`execute` is the sole write gate on `repair_fts`/`repair_crg_cache`/`snapshot`; omit it (default `false`) for a safe dry-run preview.

Default C++ lookup/review work should use `search_source`, `risk_score`, `review_context`, and `health`. `search_crg_graph` reads the CRG-compatible file/symbol projection and FTS index directly from `EngineSource.db`; it requires no export build or second database. `risk_score` and `review_context` continue to read the EngineSource `crg_*` projection/cache.
`impact_radius` defaults to `call|type|inheritance`. For virtual method edits, call `find_overrides` with a qualified symbol such as `UActorComponent::BeginPlay`, or explicitly pass `edge_kinds=call|type|inheritance|override` when override traversal should be mixed into the broader blast radius; unqualified method names can match several same-name class methods and are useful only when that broad fan-out is intentional.
`find_overrides`, `impact_radius`, `risk_score`, `review_context`, and `review_hotspots kind=override` use the `source_override_edges` cache when `source.health` shows `source_override_edges_version=2`; otherwise they fall back to query-time signature matching. If only the override cache/version is stale, run `source repair_crg_cache --scope=override_edges --execute`; use full `source repair_crg_cache --execute` for stale `crg_nodes`, `crg_edges`, or `crg_node_metrics` parity.
Offline, `Plugins\Monolith\Scripts\check_index_freshness.ps1` runs the whole health -> repair -> re-verify chain for both `EngineSource.db` and `ProjectIndex.db` (`-Execute` runs only validated health-indicated repairs; contract: `Docs\specs\SPEC_MonolithAgentOpsScripts.md`).
Use `review_hotspots kind=override` to find high-fanout virtual/override methods before broad API changes. Run `source repair_fts --target=graph_nodes --execute` only when `source health` reports stale graph-node FTS parity.

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

## Reflection Intelligence (structural view of your own C++)

`source_query` reads symbol-level engine + project SOURCE. The Reflection Intelligence (RI) namespaces add a higher-level STRUCTURAL view of the reflected surface, mined from UHT artefacts (`*.gen.cpp`) — use these when you want the as-declared UCLASS/UPROPERTY/UFUNCTION shape rather than the raw source text. Scope: project game module + project plugins (default); marketplace plugins gated (`bIndexMarketplacePluginReflection`, off); Epic engine built-ins excluded.

**`cppreflect_query` (6 actions)** — C++ reflection structure:

| Action | Purpose |
|--------|---------|
| `get_uclass` | Parent class, specifiers, source path for a UCLASS |
| `list_uproperties` | UPROPERTY surface of a UCLASS (paginated) |
| `list_ufunctions` | UFUNCTION surface of a UCLASS (paginated) |
| `find_interface_impls` | Every UCLASS that implements a UINTERFACE (C++ only — not BP) |
| `find_class_specifier` | Classes carrying a specifier; token-forgiving (alias map `Blueprintable`->`IsBlueprintBase`, case-insensitive) |
| `list_class_specifiers` | DISTINCT queryable token vocabulary + per-token counts (no params) |

Call `list_class_specifiers` first to learn what `find_class_specifier` can match — the `flags` column stores UHT metadata keys (`IsBlueprintBase`, `BlueprintType`, `Abstract`), NOT raw C++ specifiers.

**`network_query` (4 actions)** — replication/RPC structure of your C++ (covers project plugins): `list_replicated_classes`, `list_rpc_functions` (specifier-based — `FUNC_NetServer`/`Client`/`Multicast`), `list_onrep_handlers`, `audit_unbalanced_onreps` (catch `ReplicatedUsing=OnRep_X` with no `OnRep_X` handler).

**`reflect_query("rebuild_reflection_index")`** — project-only force-rebuild of the RI `reflect_*` tables. Call it after changing C++ reflection structure (new/renamed UCLASS/UPROPERTY/UFUNCTION) when lazy bootstrap + Live-Coding refresh haven't fired. It does NOT touch `source_query`'s engine source index.

## Rules

- **Never guess** `#include` paths or signatures -- always verify with `source_query`
- Search action is `search_source` (not `search`)
- Source index: engine Runtime/Editor/Developer + plugins + shaders (1M+ symbols)
- Use `find_callers` for idiomatic usage, `get_symbol_context` for quick definition lookup
- `cppreflect_query` for the structural reflected view; `source_query` for symbol-level source
- `cppreflect` `source_line` is `0` (UHT drops it) — round-trip through `source_query("search_source")` for real line numbers
- Use `config_query("explain_setting")` before changing unfamiliar CVars
- Non-existent actions: `get_function_signature`, `get_deprecation_warnings`
