---
name: unreal-reflection-intel
description: "Use for Reflection Intelligence through Monolith: UCLASS/UPROPERTY/UFUNCTION lookup, replication audits, decision records, risk hotspots, co-change, conditional gates, and reflection index maintenance. Triggers on cppreflect, replicated class, RPC, OnRep, decision, stale decision, hotspot, cochange, conditional gate."
---

# Unreal Reflection Intelligence Skill

Use this skill for read-heavy architecture, API, replication, decision, and risk analysis backed by Monolith Reflection Intelligence.

## Namespaces

- `cppreflect`: UCLASS, UPROPERTY, UFUNCTION, interface, and specifier lookup.
- `network`: replicated classes, RPCs, OnRep handlers, and replication audits.
- `decision`: indexed architecture decision records and supersession chains.
- `risk`: hotspot scores, co-change pairs, churn, release-window hotspots, and conditional gates.
- `reflect`: reflection index maintenance when available in the live registry.

## Workflow

1. Prefer live MCP queries when the editor-backed server is reachable.
2. If MCP/editor is down but `Binaries/monolith_query.exe` and `Saved/EngineSource.db` are present, use the offline CLI for read-only RI queries.
3. Confirm exact action params with `monolith_discover({ "namespace": "<ri-ns>", "action": "<action>", "mode": "schema" })` when live MCP is available.
4. Use qualified class and file names whenever possible to avoid broad same-name results.
5. Treat RI data as indexed evidence. If index health or freshness is suspect, check the relevant health/rebuild action before making final claims.

## Offline Examples

```powershell
Binaries\monolith_query.exe cppreflect get_uclass UObject
Binaries\monolith_query.exe cppreflect list_uproperties AActor --limit=20
Binaries\monolith_query.exe network list_rpc_functions --limit=20
Binaries\monolith_query.exe network audit_unbalanced_onreps --limit=20
Binaries\monolith_query.exe decision list_decisions --limit=20
Binaries\monolith_query.exe risk get_hotspot_score Docs/SPEC_CORE.md
Binaries\monolith_query.exe risk list_conditional_gates --limit=20
```

## Safety

- These surfaces are for evidence gathering and risk review. They do not replace source edits, tests, or focused source lookup.
- Do not claim an index was rebuilt unless the rebuild or maintenance action actually ran in the current environment.
- For C++ edits, pair RI findings with `source_query("review_context")`, `source_query("impact_radius")`, or `source_query("find_overrides")` when applicable.
