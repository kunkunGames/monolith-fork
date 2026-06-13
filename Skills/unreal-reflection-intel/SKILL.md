---
name: unreal-reflection-intel
description: Use when auditing your project's reflected C++ surface and change-risk via Monolith MCP (cppreflect, network, decision, risk) — UCLASS/UPROPERTY/UFUNCTION metadata and specifiers, interface implementers, replication/RPC/OnRep audits, architecture decision records, git-derived risk hotspots, co-change, churn, and conditional macro gates. For plain C++ symbol/text search, signatures, or class hierarchy use unreal-cpp; to map an asset/Blueprint node to its backing C++ symbol use unreal-bridge; for FTS asset/reference search use unreal-project-search; to actually edit a Blueprint's replicated variables or RepNotify use unreal-blueprints. Triggers on reflection, reflection intelligence, cppreflect, UCLASS, UPROPERTY, UFUNCTION, class specifier, Blueprintable, interface implementers, network, replicated class, RPC, OnRep, RepNotify, unbalanced OnRep, replication audit, decision, ADR, stale decision, supersession, risk, hotspot, cochange, churn, conditional gate, WITH_EDITOR gate.
---

# Unreal Reflection Intelligence Skill

Read-heavy intelligence over your project's own reflected C++ surface and change-risk, driving four Monolith namespaces: **`cppreflect`** (reflection metadata), **`network`** (replication/RPC), **`decision`** (architecture decision records), and **`risk`** (git-derived hotspots/co-change/gates). All four are deterministic, index-backed, read-only evidence surfaces — they do not edit source or assets.

## Discovery

```
monolith_discover({ namespace: "cppreflect" })   // also: network, decision, risk
monolith_discover({ namespace: "cppreflect", action: "get_uclass", mode: "schema" })  // exact params
```

When the editor/MCP server is up, prefer live queries and confirm params with `mode: "schema"`. When MCP is down but `Binaries/monolith_query.exe` and `Saved/EngineSource.db` are present, the same actions run offline read-only (see Offline Examples). Index scope: project game module + project plugins (marketplace plugins gated via `bIndexMarketplacePluginReflection`, off by default; Epic engine built-ins excluded).

## When to use / Use a different skill for

- **This skill (cppreflect / network / decision / risk):** the as-declared reflected shape (UCLASS/UPROPERTY/UFUNCTION specifiers, interface implementers), replication/RPC/OnRep audits, indexed architecture decision records, and git-derived risk/hotspot/co-change/gate evidence.
- **unreal-cpp** — plain C++ symbol/text search, signature verification, includes, class hierarchy, callers/callees, and code-review risk over raw source rather than reflection metadata.
- **unreal-bridge** — cross from a specific asset/Blueprint node to its backing C++ symbol, rather than reflection-wide UCLASS/UPROPERTY audits.
- **unreal-project-search** — FTS asset/reference search across the project, rather than reflection-index queries over reflected types.
- **unreal-blueprints** — actually EDIT a Blueprint's replicated variables or RepNotify; this skill only AUDITS replication/OnRep through reflection.

## Action Reference

Param notation: `name*` required, `name?` optional, `name=val` default, `a/b/c` allowed values, `[w]` mutates (transaction-wrapped). Signatures are a snapshot of the live catalog — for the exact full schema call `monolith_discover` with `mode: "schema"` (the discover-first block above stays the authority).

### `cppreflect` — C++ reflection structure (6)

| Action | Signature | Purpose |
|--------|-----------|---------|
| `get_uclass` | `class_name*` `module_name?` | Parent chain, specifiers, all UPROPERTY/UFUNCTION rows for one UCLASS |
| `list_uproperties` | `class_name?` `blueprint_visible_only?=false` `limit?=50` `cursor?` | UPROPERTY rows (filter by class — engine scope hits thousands) |
| `list_ufunctions` | `class_name?` `blueprint_callable_only?=false` `limit?=50` `cursor?` | UFUNCTION rows of a UCLASS (paginated) |
| `find_interface_impls` | `interface_name*` | Every UCLASS implementing a U-prefixed UINTERFACE (C++ only — not BP) |
| `find_class_specifier` | `specifier_name*` `limit?=50` `cursor?` | Classes whose `flags` carry a token; alias `Blueprintable`->`IsBlueprintBase`, case-insensitive |
| `list_class_specifiers` | _(none)_ | DISTINCT `flags` token vocabulary + per-token counts |

Call `list_class_specifiers` first to learn what `find_class_specifier` matches — the `flags` column stores UHT metadata keys (`IsBlueprintBase`, `BlueprintType`, `Abstract`), NOT raw C++ specifiers (`MinimalAPI`, `NotBlueprintable` are UHT-dropped and return a not-captured note). `cppreflect` `source_line` is `0` (UHT drops it) — round-trip through `unreal-cpp`'s `search_source` for real line numbers.

### `network` — replication/RPC structure (4)

| Action | Signature | Purpose |
|--------|-----------|---------|
| `list_replicated_classes` | `limit?=50` `cursor?` | Classes with ≥1 replicated UPROPERTY (+ `replicated_property_count`) |
| `list_rpc_functions` | `class_name?` `rpc_kind?` (`Server`/`Client`/`Multicast`) `limit?=50` `cursor?` | RPC UFUNCTIONs by net specifier |
| `list_onrep_handlers` | `class_name?` `limit?=50` `cursor?` | UFUNCTIONs matching the `OnRep_*` name pattern |
| `[w] audit_unbalanced_onreps` | `limit?=50` `cursor?` | Catch `ReplicatedUsing=OnRep_X` with no `OnRep_X` handler (read-only result; catalog marks it transaction-wrapped) |

### `decision` — architecture decision records (5)

| Action | Signature | Purpose |
|--------|-----------|---------|
| `list_decisions` | `path_filter?` `min_confidence?=0.6` `status?` (`open`/`accepted`/`superseded`/`deprecated`/`draft`) `limit?=50` `cursor?` | Decisions mined from the markdown corpus |
| `get_decision` | `decision_id*` | Read one decision by stable id (e.g. `Docs/specs/SPEC_X.md#anchor`) |
| `list_stale` | `max_age_days*` `path_filter?` `limit?=50` `cursor?` | Decisions whose source markdown is older than N days |
| `find_supersession_chain` | `decision_id*` `depth?=10` | Walk supersedes edges this decision supersedes (transitive) |
| `find_referent_decisions` | `decision_id*` | Inverse — decisions that supersede the given id |

### `risk` — file risk and co-change (5)

| Action | Signature | Purpose |
|--------|-----------|---------|
| `get_hotspot_score` | `file_path*` | Composite churn×complexity hotspot score for one file |
| `get_cochange_pairs` | `file_path*` `limit?=50` `cursor?` | Files that historically co-change with a file |
| `get_file_churn` | `file_path*` `repo_tag?` (e.g. `Monolith`/`Resonance`) | Commit-touch count + last-touched timestamp |
| `get_release_window_hotspots` | `since_unix?` (default 30d ago) `limit?=50` `cursor?` | Top hotspot files touched since a Unix timestamp |
| `list_conditional_gates` | `macro_filter?` (e.g. `WITH_GBA`) `path_filter?` `limit?=50` `cursor?` | `#if WITH_*` sites and `bHas*` Build.cs probes |

`file_path`/`path_filter` are project-relative (forward slashes recommended).

### `reflect` — reflection index maintenance (1)

| Action | Signature | Purpose |
|--------|-----------|---------|
| `[w] rebuild_reflection_index` | _(none)_ | Force clear+rewrite of project-only `reflect_*` tables (cppreflect + network sets) |

`reflect_query("rebuild_reflection_index")` force-rebuilds the project-only RI `reflect_*` tables after you add/rename a UCLASS/UPROPERTY/UFUNCTION and lazy bootstrap + Live-Coding refresh have not fired. Requires the project built with UBT at least once (Live Coding patches do not emit gen.cpp). It does NOT touch the `source` engine index. This action needs a live editor; offline `monolith_query.exe` cannot run it.

## Common Workflows

```
// Reflected shape of a class
cppreflect_query({ action: "get_uclass", params: { class_name: "UObject" } })
cppreflect_query({ action: "list_uproperties", params: { class_name: "AActor", limit: 20 } })

// Discover specifier vocabulary, then find every Blueprintable UCLASS
cppreflect_query({ action: "list_class_specifiers", params: {} })
cppreflect_query({ action: "find_class_specifier", params: { specifier_name: "Blueprintable" } })

// Replication audit — flag OnRep handlers that were never declared
network_query({ action: "list_replicated_classes", params: { limit: 20 } })
network_query({ action: "audit_unbalanced_onreps", params: { limit: 20 } })

// Architecture decision records and supersession
decision_query({ action: "list_decisions", params: { status: "accepted", limit: 20 } })
decision_query({ action: "find_supersession_chain", params: { decision_id: "<decision-id>" } })

// Change-risk evidence before a review claim
risk_query({ action: "get_hotspot_score", params: { file_path: "Docs/SPEC_CORE.md" } })
risk_query({ action: "list_conditional_gates", params: { macro_filter: "WITH_EDITOR", limit: 20 } })
```

Two end-to-end passes, each walking a single RI concern across its namespace using only the actions in the tables above. Confirm params with `mode: "schema"` before each call.

### Recipe 1 — Replication audit (network namespace)

1. `network_query({ action: "list_replicated_classes", params: { limit: 50 } })` — enumerate classes with ≥1 replicated UPROPERTY (note each `replicated_property_count`) and pick a class to audit.
2. `network_query({ action: "list_onrep_handlers", params: { class_name: "<class>", limit: 50 } })` — list the `OnRep_*` UFUNCTIONs declared on that class so you can compare them against the `ReplicatedUsing` specifiers.
3. `network_query({ action: "list_rpc_functions", params: { class_name: "<class>", rpc_kind: "Server" } })` — review the class's Server RPC surface (repeat with `rpc_kind: "Client"`/`Multicast`) to round out the replication picture.
4. `network_query({ action: "audit_unbalanced_onreps", params: { limit: 50 } })` `[w]` — flag any `ReplicatedUsing=OnRep_X` with no matching `OnRep_X` handler (read-only result; the catalog marks it transaction-wrapped).

### Recipe 2 — Risk-and-decision review pass (risk + decision namespaces)

1. `risk_query({ action: "get_release_window_hotspots", params: { limit: 20 } })` — list top hotspot files touched in the recent release window (defaults to ~30 days ago) to choose a file under review.
2. `risk_query({ action: "get_hotspot_score", params: { file_path: "<file>" } })` then `risk_query({ action: "get_file_churn", params: { file_path: "<file>" } })` — read the composite churn×complexity score and the commit-touch count / last-touched timestamp for that file.
3. `risk_query({ action: "get_cochange_pairs", params: { file_path: "<file>", limit: 50 } })` — find files that historically co-change with it so the review covers the real blast radius.
4. `decision_query({ action: "list_decisions", params: { path_filter: "<dir>", status: "accepted", min_confidence: 0.6, limit: 20 } })` — pull architecture decision records touching that area, then `decision_query({ action: "get_decision", params: { decision_id: "<id>" } })` to read a specific record.
5. `decision_query({ action: "list_stale", params: { max_age_days: 180, path_filter: "<dir>" } })` — surface decisions whose source markdown is older than N days so a stale ADR does not anchor the review.

## Offline Examples

```powershell
Binaries\monolith_query.exe cppreflect get_uclass UObject
Binaries\monolith_query.exe cppreflect list_uproperties AActor --limit=20
Binaries\monolith_query.exe cppreflect list_class_specifiers
Binaries\monolith_query.exe network list_rpc_functions --rpc-kind=Server --limit=20
Binaries\monolith_query.exe network audit_unbalanced_onreps --limit=20
Binaries\monolith_query.exe decision list_decisions --limit=20
Binaries\monolith_query.exe risk get_hotspot_score Docs/SPEC_CORE.md
Binaries\monolith_query.exe risk list_conditional_gates --macro-filter=WITH_EDITOR --limit=20
```

## Gotchas / Rules

- This reference is a snapshot of the live `RegisterAction` surface. If an action is missing or renamed, re-run `monolith_discover({ namespace: "<ns>" })` — the catalog is the source of truth.
- These four namespaces are read-only evidence gathering for architecture, API, replication, and risk review. They do NOT replace source edits, tests, or focused source lookup.
- Treat RI data as indexed evidence. If index health or freshness is suspect, verify before making final claims, and do NOT claim an index was rebuilt unless the rebuild/maintenance action actually ran in the current environment.
- Use qualified class and file names to avoid broad same-name matches.
- For C++ edits, pair RI findings with `unreal-cpp` actions (`review_context`, `impact_radius`, `find_overrides`) where applicable.
