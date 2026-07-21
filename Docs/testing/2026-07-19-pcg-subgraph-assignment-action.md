# PCG Static Subgraph Assignment Action Verification

**Date:** 2026-07-19
**Status:** Source, focused tests, spec, and skill implemented; Strict NonUnity compile-only pass and downstream canonical graph verification complete; linked build and exact Subgraph automation pending
**Changelist:** 1198

---

## 1. Scope

Add `pcg.set_pcg_subgraph` as the typed authoring path for assigning one exact project-owned `UPCGGraph` or `UPCGGraphInstance` to an existing `UPCGSubgraphSettings` node. The action exists because `SubgraphInstance` is not a generic editable settings field in UE 5.8; the supported mutation path is `UPCGBaseSubgraphSettings::SetSubgraph`. This change does not mutate ProjectMGH, SpeedCore, SpeedBox, SpeedSwitching, SpeedTerritory, or any other content asset.

---

## 2. Static verification

| Gate | Result | Evidence |
|------|--------|----------|
| Engine contract | PASS | The project resolver selects `D:\Engine\UE_5.8`. Engine `PCGSubgraph.h` exposes public `SetSubgraph`, `UPCGSubgraphSettings::SubgraphInstance`, `SubgraphOverride`, and `SubgraphAssetFilter`; `UPCGGraphInstance::CanGraphInterfaceBeSet` and `UPCGGraph::Contains(const UPCGGraph*)` provide the graph-instance and static-subgraph recursion preflights. |
| Exact path and type | PASS | Parent and child paths normalize to top-level project-owned mounted packages whose object name matches the package leaf. Parent loading requires `UPCGGraph`; the requested child requires a loadable `UPCGGraphInterface` with a concrete graph. The child read-back must equal the requested case-sensitive canonical object path, so redirectors, aliases, case-only variants, subobjects, donor paths, and type mismatches fail closed. |
| Node and override guard | PASS | `node_id` resolves by exact object identity or unique authored title and must own concrete `UPCGSubgraphSettings` with a valid `SubgraphInstance`. An active `SubgraphOverride` is rejected instead of changing a hidden default assignment. |
| Recursion and customization | PASS | The handler rejects graph-instance recursion through `CanGraphInterfaceBeSet`, direct self-reference, and any requested graph that recursively contains the parent. Parent graph customization is evaluated through exact AssetRegistry metadata; metadata absence under an active filter and filter rejection both fail closed. |
| Dry-run and no-op | PASS | `dry_run=true` performs every path/type/override/recursion/filter preflight without calling `SetSubgraph`, dirtying, or saving. An exact repeated assignment returns `unchanged`, explicit assigned-interface/concrete-graph read-back, and `saved=false`. |
| Mutation and rollback | PASS | Commit snapshots the previous exact interface, all incident edges, and package dirty state; applies through `SetSubgraph`; verifies exact interface and concrete graph; and requires incident topology to retain the same endpoint identities and pin labels before running the existing bounded structural validator. An assignment that would silently drop or replace an edge is rolled back and directs the caller to disconnect/reconnect explicitly. Read-back, topology, validation, or save failure restores the previous interface, incident topology, and dirty state before returning an error. |
| Persistence evidence | PASS | `save=true` uses the existing `UEditorAssetSubsystem::SaveLoadedAsset` boundary and requires a non-empty package file. The result exposes requested, previous, and currently assigned interface/concrete-graph paths, node/settings identity, bounded pins, mutation state, and save evidence. |
| Focused automation | ADDED, NOT RUN | `Monolith.PCG.GraphAuthoring.Subgraph.AssignDryRunSaveReload` covers side-effect-free dry-run, exact apply/save/reload, assigned identity, clean repeated no-op, and dirty-state preservation. `Monolith.PCG.GraphAuthoring.Subgraph.RecursionGuard` covers allowed parent-to-child assignment plus rejected child-to-parent and direct-self hierarchies. Registration, transaction policy, and pre-save-policy arrays include subgraph assignment and `replace_pcg_graph_contents` in the current 27-action surface; all eight graph mutators remain in the pre-save policy. |
| Spec and skill | PASS | `SPEC_MonolithPCG.md`, `Skills\unreal-pcg\SKILL.md`, and all three focused PCG references define the current 27-action surface and require dedicated subgraph assignment/whole-graph replacement actions instead of generic reflected writes. |
| Public API reference | READY (DEPENDENT CL 1200) | The CL1200 owner synchronized `Plugins\Monolith\Docs\API_REFERENCE.md` to the 27-action surface, including `replace_pcg_graph_contents`, its safe defaults, full-replacement contract, target-owned `LastEditedDocuments` preservation, source-control/rollback/save/no-op behavior, and recursion guards. Submit implementation CL1198 before API aggregate CL1200 so the public contract never precedes the registered implementation. |
| Migration content use | PASS (DOWNSTREAM CONTENT CL) | The canonical `/SpeedCore/ProjectMGH/Authoring/PCG_ProjectMGHLayout` integration is no longer queued. Live bounded read-back reports 492 element nodes, 2 special nodes, and 696 edges; strict validation reports valid with 0 invalid edges, no cycle, 0 isolated nodes, 0 errors, and 0 warnings. The single `GetActorData_13` `Floor` source fans out to 19 consumers, `legacy_roots=0`, and the repeat graph-replacement dry-run is unchanged. This is downstream content evidence, not a claim that CL1198 owns the asset mutation. |

---

## 3. Protected and live verification

| Gate | Result | Evidence |
|------|--------|----------|
| Supplemental strict compile | PASS | With `P4_BUILD_CHANGELIST=1198` and `ALLOW_RUNNING_EDITOR=1`, the protected `Build\BatchFiles\BuildGameEditorStrictNonUnity.bat -NoLink -Module=MonolithPCG` path completed all `11/11` compile actions. `-NoLink` skipped binary preparation, link, cleanup, revert, and reconcile, so this proves current source/UHT compatibility only. The running editor was not stopped and no proxy/recovery path was invoked. The final linked project build remains pending. |
| Focused Subgraph automation execution | PENDING | Run the two exact `Monolith.PCG.GraphAuthoring.Subgraph.*` tests through a live route. The completed GraphContents run below is related replacement coverage but does not satisfy these two assignment-specific tests. |
| Related GraphContents automation | PASS (4/4) | Async run `automation-20260718T235316Z-5EC0DADB` completed the four exact `Monolith.PCG.GraphAuthoring.GraphContents.*` tests with 4 passed, 0 failed, 0 skipped, and no test errors. |
| Monolith static CI | HISTORICAL BLOCKER; RE-RUN PENDING | The earlier offline run reported one catalog-drift blocker and 241 advisories while the route was unavailable and the generated catalog snapshot belonged to excluded CL1100. A subsequent live discovery verified the current 27-action PCG surface; re-run static CI after the publication owner resolves the excluded snapshot rather than treating the older offline result as current pass evidence. |
| Content mutation | N/A IN CL1198 / DOWNSTREAM VERIFIED | CL1198 changes source, tests, spec, skill, and this record only. The later ProjectMGH `.uasset`/`.umap` integration is verified in its owning content changelist and is not moved into CL1198. |
| Screenshot / Discord | N/A | No visual, gameplay, level, or asset-presentation state changed. PC 1920x1080 capture and Discord upload belong to the later live content integration, not this source capability change. |
