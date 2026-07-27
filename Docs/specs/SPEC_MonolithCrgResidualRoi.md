# Monolith CRG Residual ROI Backlog

**Parent:** [../SPEC_CORE.md](../SPEC_CORE.md), [SPEC_MonolithSource.md](SPEC_MonolithSource.md), [SPEC_MonolithIndex.md](SPEC_MonolithIndex.md), [SPEC_MonolithToolCallReliabilityBacklog.md](SPEC_MonolithToolCallReliabilityBacklog.md)
**Engine:** Unreal Engine 5.7+
**Status:** Trust slice implemented 2026-06-15; scoped projection refresh verified 2026-06-16; graph export retirement and EngineSource-backed node search implemented 2026-07-21
**Created:** 2026-06-15
**Owner modules:** `MonolithSource`, `MonolithIndex`, `Tools/MonolithQuery`, `Scripts/`, `Analyzer/`
**Scope:** Residual high-ROI work after the 2026-06-15 adversarial CRG audit. Covers the source/project CRG-inspired review surfaces, derived `crg_*` projection caches, EngineSource-backed graph-node search, scoring correctness, freshness repair, maintenance SLOs, and regression fixtures.
**Non-goals:** Removing the retained `source` or `project` CRG-inspired review surfaces, replacing Monolith's native indexes with the external CRG runtime, adding community/betweenness/flow algorithms, renaming `source.search_crg_graph`, or keeping stale/duplicate data hidden behind silent fallbacks.

---

## 1. Decision

Do **not** remove CRG wholesale.

Keep the high-value query surfaces:

| Surface | Keep? | Reason |
|---|---|---|
| `source.impact_radius`, `source.risk_score`, `source.review_hotspots`, `source.review_context`, `source.find_overrides` | Yes | They package source review context over existing `symbols`, `"references"`, `inheritance`, and override edges. |
| `project.impact_radius`, `project.risk_score`, `project.review_hotspots`, `project.review_context` | Yes | They package asset dependency and risk context over existing `assets` and `dependencies`. |
| Derived `crg_*` projection tables | Yes | They are disposable caches that accelerate risk/review surfaces and can be rebuilt from authoritative DB tables. |
| Separate graph export artifact | No | Caller/log census did not justify a duplicated DB and builder lifecycle. `source.search_crg_graph` is retained over the canonical EngineSource File/Symbol VIEW and FTS index. |

The residual work is not about proving that "CRG" exists. It is about making the retained surfaces trustworthy enough that agents can act on them without second-guessing duplicate rows, stale caches, or false-positive risk labels. The practical value is a compact review package over Monolith's existing source/project indexes; custom CRG ownership is justified only where it improves `risk_score`, `impact_radius`, `review_context`, `review_hotspots`, `pre_merge_check`, or the retained EngineSource graph-node search.

CRG must remain a derived risk/impact cache, not a second source of truth and not a custom graph that humans maintain by hand. If source symbols, source references, asset rows, or asset dependencies disagree with CRG rows, the native source/project tables win and the CRG projection is repaired or rebuilt. If a CRG component cannot be kept disposable, scoped, health-gated, and measurably useful to review decisions, it should be removed or reduced rather than carried as custom maintenance debt.

## 2. Evidence Baseline

The 2026-06-15 read-only audit established the baseline below. These values are evidence for scheduling, not a live invariant; rerun the commands before closing the work.

| Finding | Evidence | Interpretation |
|---|---|---|
| Source CRG projection parity is healthy | `source health --include-counts=true`: `symbols=536436`, `crg_nodes=536436`, `crg_node_metrics=536436`, `source_override_edges_version=2`; `source crg_graph_health`: `nodes=618521`, `edges=403226`, `fts_rows=618521`, schema version 9 | Source projection and graph export are structurally valid. The problem is result quality and maintenance cost, not total source cache corruption. |
| Project CRG projection is stale | `project health --include-counts=true`: `assets=16177`, `crg_nodes=10000`, `crg_node_metrics=10000`, warning recommends `project.repair_crg_cache` | Project risk/review actions can read stale cached metrics until repair or freshness enforcement runs. |
| Project repair is cheap on a copy | `project repair_crg_cache --execute --project-db=<copy>` fixed `crg_nodes` and metrics `10000 -> 16177` in about 1.2 s | Repair should be safe to trigger when health proves parity drift, but live concurrency and transaction behavior still need tests. |
| Source result quality is polluted | `AProjectMonster` appears many times in source/graph DB samples; many rows have blank file paths and huge `file_id` values. `review_context AProjectMonster` produced repeated `<unknown>` impact rows. | Duplicate/blank-path rows are a trust problem for `search_source`, `search_crg_graph`, `review_context`, and hotspot ranking. |
| Project sensitivity scoring has false positives | `/Game/Design/DataAsset/WorldTable/DA_World_002_Lobby` received `sensitivity: crypto/signing/hash surface` because `Design` contains `sign`. | Substring matching is too broad. Scoring must use token boundaries and expose which token matched. |
| Query surfaces are cheap; maintenance was expensive | Recent log aggregation showed `repair_crg_cache`/`build_crg_graph` dominating wall time while `risk_score`, `impact_radius`, and `review_context` were comparatively cheap. | Optimize and gate maintenance/export paths before considering query-surface removal. |

## 3. Ranked Work

| Rank | Item | ROI | Primary files |
|---:|---|---|---|
| 1 | Source symbol/file de-duplication and path provenance | Restores trust in `search_source`, `review_context`, `review_hotspots`, and `search_crg_graph` | `MonolithSourceDatabase.*`, `MonolithSourceIndexer.*`, `MonolithCppParser.*`, `MonolithSourceReview.*`, `monolith_query.cpp` |
| 2 | Project sensitivity tokenization and provenance | Removes high-visibility false risk labels with a small scoring change | `MonolithIndexReview.cpp`, `monolith_query_crg.h`, `MonolithIndexQueryTests.cpp` |
| 3 | Project CRG cache freshness and transactional repair enforcement | Prevents stale cached risk/review data and makes health actionable | `MonolithIndexReview.*`, `monolith_query_crg.h`, `check_index_freshness.ps1` |
| 4 | Regression fixtures for observed failures | Locks in the audit findings and prevents silent drift | `MonolithSourceQueryTests.cpp`, `MonolithIndexQueryTests.cpp`, CLI fixture tests |
| 5 | EngineSource node-search SLO and retired-action visibility | Removes duplicated storage/maintenance while preserving search semantics and historical cost evidence | `monolith_query.cpp`, `Analyzer/analyze_invocation_logs.py`, `SPEC_MonolithSource.md` |
| 6 | Regression gates for removed graph-export pieces | Prevents runtime/help/watchdog reintroduction without losing useful CRG review actions | Tests, analyzer reports, source/project specs |

## 3.1 Implementation Update - 2026-06-15 to 2026-06-16

The retained high-ROI slice is implemented and verified as a trust/correctness pass, not as a broader graph-engine expansion.

| Area | Status | Result |
|---|---|---|
| P0 fixtures | Done for P1-P3 trust failures | Added automation fixtures for known-path source priority, project sensitivity false positives/positive controls, and stale project CRG parity. Maintenance/export SLO measurement remains under P4. |
| P1 source provenance | Implemented | `search_source`, `symbols_by_name`, `source.review_context`, and offline `source.search_crg_graph` prefer known file paths, expose `path_status`, and de-duplicate missing-path duplicates without collapsing real symbol identity. |
| P2 sensitivity scoring | Implemented | Source/project editor paths and offline query paths use token-boundary/camel-case aware sensitivity matching. `Design` and `Assignment` are negative controls; `Signature` and `CryptoHash` remain positive controls. |
| P3 project freshness | Implemented for copied-DB fixture | `project.health --include-counts=true` detects stale CRG count parity and `project.repair_crg_cache --execute` repairs the fixture back to healthy parity. |
| P3.1 scoped project refresh | Implemented and verified | Asset startup/live indexing completion now refreshes only changed project paths plus one-hop dependency/referencer neighbors in `crg_*` when projection tables exist, including deleted-path neighbor recovery from old CRG edges, and falls back to full repair only for missing tables. `Monolith.IndexGuard.Project.RefreshCrgCacheForAssetsScoped` passed in `Saved\Automation\MonolithCrgScopedRefresh_20260616_R11\index.json`. |
| P3.2 duplicate-free project source reindex + scoped source CRG refresh | Implemented and verified | Project-only source indexing prunes existing project `Source`/project-plugin `Source` rows, orphan symbols, and dependent source/CRG projection rows before reinserting current project symbols. `InsertModule`/`InsertFile` re-select canonical ids after `INSERT OR IGNORE`, preventing stale `last_insert_rowid()` from attaching new symbols to nonexistent files. `FMonolithSourceDatabase::RefreshCrgCacheForFiles` refreshes changed-file symbols plus one-hop/pending-prune neighbors without a full CRG rebuild when projection tables exist. `Monolith.IndexGuard.Source.PruneIndexedFilesUnderRootsRemovesProjectSlice` passed in `Saved\Automation\MonolithCrgScopedRefresh_20260616_R11\index.json`; project-only reindex `Saved\Logs\MonolithReindexScopedRefresh_20260616_R10.log` completed in 31.9 s commandlet time / 39.3 s wall time, with scoped source CRG refresh for 1934 file(s), 12494 affected symbol(s), reference edges 0.910 s, inheritance edges 0.151 s, metrics 4.034 s. Final `source health --include-counts=true --include-deep-checks=true` was `status=ok`, including `integrity:orphan_symbols`, `integrity:orphan_references`, CRG node/edge/metric parity, and `source_override_edges_version=2`. |
| P4 maintenance/export SLO | Implemented and measured 2026-07-21 | `search_crg_graph` reads EngineSource directly; routine/restart/daily paths have no graph-export build step. A broad `UObject --limit=5` query on the 4.66 GB real DB dropped from more than three minutes with the unbounded VIEW join to 372 ms with a 4,096-row proven candidate boundary and no full fallback. Historical graph actions are analyzer-only `retired_action` evidence. |
| P5 graph export keep/deprecate | Implemented 2026-07-21 | The export builder/health/actions were removed while `search_crg_graph`, `risk_score`, `review_context`, `impact_radius`, `review_hotspots`, `find_overrides`, and `repair_crg_cache` remain. |

Verification record:

```powershell
Saved\Automation\MonolithCrgResidualRoi_20260615_R2\index.json
Saved\Automation\MonolithCrgScopedRefresh_20260616_R11\index.json
Saved\Logs\MonolithReindexScopedRefresh_20260616_R10.log
Plugins\Monolith\Binaries\monolith_query.exe --version  # source_hash=ecfaa97ca26b8192
Saved\Automation\MonolithGraphRetirementFixed\index.json  # 5/5 Source graph-retirement automation tests passed
Saved\Monolith\LogAnalysis\graph-retirement-20260721-gated\summary.md  # 741120 records; 574 retired graph calls; 7726657.635 ms historical export cost
Plugins\Monolith\Binaries\monolith_query.exe source search_crg_graph UObject --limit=5  # 372 ms, candidate_limit=4096, full fallback=false
Plugins\Monolith\Binaries\monolith_query.exe source health --include-counts=true  # 6.82 s, 1418683 graph-node docs and all CRG/FTS parity clean
```

## 4. Requirements

### P0 - Fixture and Measurement Harness

Add regression fixtures before broad behavior changes.

| Requirement | Contract |
|---|---|
| P0.1 source duplicate fixture | A source fixture must reproduce a repeated symbol name with distinguishable overload/class/file identity and a blank-path duplicate candidate. |
| P0.2 review-context pollution fixture | A `source.review_context` fixture must fail before the fix when the top impact list is dominated by repeated `<unknown>` or blank-path rows. |
| P0.3 sensitivity fixture | Project scoring tests must cover `Design`, `Assignment`, `Signal`, `Signature`, `Crypto`, and `Hash`. |
| P0.4 stale project cache fixture | A copied ProjectIndex DB with `assets != crg_nodes` must make `project.health --include-counts=true` report stale parity and must be repairable. |
| P0.5 maintenance SLO baseline | Analyzer output must preserve retired graph-action counts/durations separately from current query/maintenance latency, so the removal benefit remains measurable without ranking old calls as current demand. |

### P1 - Source De-duplication and Path Provenance

The source index and CRG search path must preserve real distinct symbols while suppressing or explaining duplicate blank-path rows.

| Requirement | Contract |
|---|---|
| P1.1 stable identity | De-duplication must use stable identity, not name-only matching. Qualified name, kind, parent, signature, file path, and line range must be considered before rows are collapsed. |
| P1.2 no silent data loss | If two rows have the same display name but represent real distinct declarations, both remain visible with enough provenance to distinguish them. |
| P1.3 blank-path handling | Blank or unresolved file paths must not dominate ranked search/review results. They must either be suppressed behind a warning bucket or returned with an explicit `path_status=missing`/equivalent provenance field. |
| P1.4 review-context ranking | `source.review_context` must rank known-path symbols above blank-path duplicates unless the blank-path row has stronger, explicit evidence. |
| P1.5 graph-node search parity | `source.search_crg_graph` must not reintroduce blank-path duplicate storms from its EngineSource VIEW/FTS; results expose `path_status` and apply known-path-first de-duplication. |

Acceptance:

```powershell
Plugins\Monolith\Binaries\monolith_query.exe source search_source AProjectMonster --limit=10
Plugins\Monolith\Binaries\monolith_query.exe source review_context AProjectMonster --depth=2 --limit=20
Plugins\Monolith\Binaries\monolith_query.exe source search_crg_graph AProjectMonster --limit=10
```

The returned rows must include the real `Source/<ProjectGameModule>/Public/NPC/ProjectMonster.h` result and must not be dominated by repeated blank-path or `<unknown>` entries.

### P2 - Project Sensitivity Tokenization and Provenance

Risk sensitivity must match semantic tokens, not arbitrary substrings.

| Requirement | Contract |
|---|---|
| P2.1 token boundaries | Sensitivity matching must treat separators, path boundaries, camel-case transitions, and underscores as token boundaries. |
| P2.2 no substring false positive | `Design` and `Assignment` must not match the `sign` token. |
| P2.3 positive controls | `Sign`, `Signature`, `Crypto`, `Hash`, `Auth`, `Token`, `Session`, `Network`, `Save`, and `Exec` remain recognized when they are standalone tokens or accepted compound tokens. |
| P2.4 provenance | Risk reasons must include the matched sensitivity token or category so reviewers can understand why a row is sensitive. Cached SQL rebuilds may report the token-boundary category when per-token extraction is unavailable. |
| P2.5 shared parity | Editor/project scoring and offline `monolith_query` scoring must use the same tokenization rules. |

Acceptance:

```powershell
Plugins\Monolith\Binaries\monolith_query.exe project risk_score /Game/Design/DataAsset/WorldTable/DA_World_002_Lobby --limit=5 --min-tier=low
```

The Lobby data asset must not report signing/crypto/hash sensitivity merely because the path contains `Design`. Dedicated positive-control fixtures containing `Signature`, `Crypto`, and `Hash` must still report sensitivity.

### P3 - Project CRG Freshness and Repair Enforcement

Project CRG projection rows are disposable, but stale parity must be visible and cheap to repair.

| Requirement | Contract |
|---|---|
| P3.1 health is authoritative | `project.health --include-counts=true` must report warning/fail when `assets`, `crg_nodes`, and `crg_node_metrics` counts diverge. |
| P3.2 repair is transactional | `project.repair_crg_cache --execute` must rebuild projection rows inside a transaction and leave either the old valid projection or the new valid projection after interruption. |
| P3.3 live-index gate | Repair must refuse or defer while `UMonolithIndexSubsystem::IsIndexing()` is true. |
| P3.4 offline/live parity | Offline CLI and live editor repair paths must return the same summary fields: old counts, new counts, repaired tables, skipped/fresh state, duration, and next actions. |
| P3.5 freshness gating | Read actions may fall back to query-time scoring on cache miss, but they must not make a stale projection look healthy. |

Acceptance:

```powershell
Plugins\Monolith\Binaries\monolith_query.exe project health --include-counts=true
Plugins\Monolith\Binaries\monolith_query.exe project repair_crg_cache --execute --project-db=<copied_ProjectIndex.db>
Plugins\Monolith\Binaries\monolith_query.exe project health --include-counts=true --project-db=<copied_ProjectIndex.db>
```

The copied DB must move from stale parity to healthy parity. A simulated in-progress indexing state or DB write contention must return an explicit refusal/busy result, not a partial silent repair.

### P4 - EngineSource Node-Search and Maintenance SLO

Graph-node search is an EngineSource-owned read surface. It must not require a second database, export builder, lock, cooldown, or scheduled maintenance step.

| Requirement | Contract |
|---|---|
| P4.1 no export lifecycle | Runtime/catalog/help/proxy/watchdog paths must not open, create, build, poll, or accept an override for a separate graph database. |
| P4.2 direct search | `source.search_crg_graph` must query `source_graph_nodes_fts` and `source_graph_nodes` in EngineSource with the frozen FTS/LIKE/result contract. |
| P4.3 bounded maintenance | Incremental files/symbols triggers keep the graph-node FTS current; only migration and explicit `source.repair_fts --target=graph_nodes --execute` may perform a full rebuild. |
| P4.4 source health owner | `source.health` reports schema, trigger, version, bounded MATCH readiness, and optional deep row parity. |
| P4.5 analyzer continuity | Historical graph build/rebuild/health rows remain visible as `retired_action`, excluded from current maintenance loops and missing-action demand. |
| P4.6 broad-query latency without semantic drift | On the 4.66 GB reference EngineSource DB, `UObject --limit=5` must complete within 2 seconds without the full-rank fallback. The implementation uses a bounded FTS5 rank pool, strict omitted-rank boundary proof, and geometric expansion; equal-rank and sparse-kind fixtures must still match the frozen legacy ordering exactly. |

Acceptance:

```powershell
Plugins\Monolith\Binaries\monolith_query.exe source search_crg_graph UObject --limit=5
Plugins\Monolith\Binaries\monolith_query.exe source health --include-counts=true
python Plugins\Monolith\Analyzer\analyze_invocation_logs.py --log-root Plugins\Monolith\Logs --since 20260615 --rank-by-recency
```

Search and health must succeed without creating another DB. The reference broad query must meet P4.6 and expose its candidate/expansion/fallback diagnostics. A fresh invocation window must contain no graph build/open/create calls, while old rows remain visible only as retired evidence.

### P5 - Graph Export Removal Criteria (Satisfied)

The caller/log census justified removal of the export lifecycle while retaining the useful public node-search action and all CRG review surfaces.

| Requirement | Contract |
|---|---|
| P5.1 caller census | Completed: runtime callers were confined to the removable builder/health/watchdog/docs/tests; low search demand did not justify duplicated storage. |
| P5.2 retained owner | `MonolithSource` owns the canonical VIEW/FTS and health/repair; `MonolithQuery` owns the single live/offline search implementation. |
| P5.3 removal boundary | Build/rebuild/health actions, offline alias, graph-only DDL/locks/path/env/cooldown, proxy forwarding, and watchdog maintenance are removed together. |
| P5.4 no query-surface regression | `risk_score`, `review_context`, `impact_radius`, `review_hotspots`, `find_overrides`, `repair_crg_cache`, and `search_crg_graph` remain and are regression-tested. |

## 5. Test Matrix

| Area | Test location | Required coverage |
|---|---|---|
| Source de-dup | `Source/MonolithSource/Private/Tests/MonolithSourceQueryTests.cpp` | `Monolith.IndexGuard.Source.KnownPathSymbolPreferred`: duplicate name with missing-path candidate, known path priority, and `review_context` seed provenance. |
| Graph search parity | `Tools/MonolithQuery/test_engine_source_crg_search.py` plus Source schema automation | `search_crg_graph` preserves FTS/LIKE semantics, stable ordering, result fields, `path_status`, and known-path-first duplicate suppression over EngineSource. |
| Project sensitivity | `Source/MonolithIndex/Private/Tests/MonolithIndexQueryTests.cpp` | `Monolith.IndexGuard.Project.RiskScoreSensitivityTokenBoundary`: negative substring cases and positive sensitivity token cases. |
| Project freshness | `MonolithIndexQueryTests.cpp` copied-DB fixture | `Monolith.IndexGuard.Project.HealthWarnsOnStaleCrgCache`: stale count detection and repair-to-healthy parity. |
| Retired maintenance evidence | `Analyzer` fixtures and real-log verification record | Historical build/rebuild/graph-health rows remain visible as `retired_action` evidence, are excluded from current problem findings, and no fresh-window graph export maintenance is required. |

## 6. Documentation Updates

Every implementation change must update docs in the same changelist:

| Change | Docs |
|---|---|
| Source de-dup/path provenance | `SPEC_MonolithSource.md`, `Docs/API_REFERENCE.md` if public fields change |
| Project sensitivity scoring | `SPEC_MonolithIndex.md`, `Docs/API_REFERENCE.md` if public `reasons[]`/raw counts change |
| Project repair/freshness behavior | `SPEC_MonolithIndex.md`, `SPEC_MonolithAgentOpsScripts.md` if script behavior changes |
| Graph export gating/removal | `SPEC_MonolithSource.md`, `SPEC_MonolithToolCallReliabilityBacklog.md`, this spec |
| Analyzer SLO output | `SPEC_MonolithInvocationLogAnalyzer.md` |

## 7. Exit Criteria

The retained trust slice and P4/P5 decision are done when all of these are true:

1. `source.search_source`, `source.search_crg_graph`, and `source.review_context` no longer produce duplicate blank-path storms for the observed `AProjectMonster`/`AProjectCharacter` class of failures.
2. `project.risk_score` does not classify `Design` as signing/crypto/hash sensitivity, while positive-control sensitive names still score.
3. `project.health --include-counts=true` detects stale CRG parity, and `project.repair_crg_cache --execute` repairs a copied stale DB transactionally.
4. Routine source/project review and `search_crg_graph` queries do not create or maintain a separate graph database.
5. The invocation analyzer preserves old graph-action cost as retired evidence and a fresh post-fix window has zero graph build/open/create activity.
6. The graph-export removal leaves the retained source/project CRG review surfaces and EngineSource graph-node search healthy.
