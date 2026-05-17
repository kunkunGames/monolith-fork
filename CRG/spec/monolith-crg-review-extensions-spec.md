---
doc_type: "spec_source"
schema_version: "2"
status: "draft"
stage: "implementation-ready"
topic_slug: "monolith-crg-review-extensions"
linked_prd: "./monolith-crg-index-navigation-prd.md"
traceability_mode: "req-task-test"
generated_by: "code-survey (CRG <-> Monolith both-sides audit, 2026-05-17)"
---

# SPEC SOURCE: Monolith CRG Review Extensions (next high-ROI patterns)

## Goal

Add the remaining **high-ROI** code-review-graph (CRG) patterns that PR #447
did NOT yet port, on top of the existing Monolith indexes and the CRG
projection cache. No new source-of-truth DB, no Python runtime, no schema
migration of the native tables.

This spec is the result of a both-sides audit on 2026-05-17:

- CRG reference: `D:\P4\game\Plugins\Monolith\CRG\code_review_graph`
  (`changes.py`, `tools/review.py`, `tools/context.py`, `refactor.py`,
  `graph_diff.py`, `hints.py`, `prompts.py`, `flows.py`).
- Monolith target: PR #447 branch `feat/crg-index-navigation-p0` (`c6c3981`),
  `Source/MonolithIndex`, `Source/MonolithSource`, `Tools/MonolithQuery`,
  and the live `Saved/ProjectIndex.db` / `Saved/EngineSource.db`.

## 쉬운 설명

PR #447은 "에셋/심볼 하나를 지정하면" 영향권·위험도·리뷰 컨텍스트를 주는
seed 기반 액션을 넣었다. 하지만 리뷰어가 실제로 시작하는 지점은 "내가 바꾼
체인지리스트(diff)" 다. 이 스펙은 그 진입점(diff -> 무엇을 리뷰할지)과,
오프라인에서도 캐시를 읽게 하는 저비용 항목 등 **아직 적용 안 된 ROI 높은
패턴**만 추려 계약으로 고정한다.

## Already Shipped (do not re-implement)

| Capability | Where | Status |
|---|---|---|
| seed `impact_radius`, `health`, `repair_fts`, `risk_score`, `review_context` (project + source) | `FMonolithIndexReview`, `FMonolithSourceReview`, `FMonolithSourceDatabase` | PR #447 done |
| CRG projection cache `crg_nodes/crg_edges/crg_node_metrics/crg_meta` + `repair_crg_cache` | both DBs | PR #447 done (cache_version=1, scoring_version=3) |
| editor cached risk (`cache.status=hit`, scoring_version=3 after rebuild) | `TryCachedAssetRisk`, `GetCachedRiskForSymbol` | PR #447 done |
| offline parity for seed/review actions | `Tools/MonolithQuery/monolith_query.cpp` | PR #447 done (read-only) |
| sensitivity risk factor + `review_hotspots` | editor + offline source/project review paths | PR #447 done |
| derived risk/metric table (nav `TSK-P1-001`) | superseded by `crg_node_metrics` | effectively delivered (editor only) |

## Verified Current State (2026-05-17 measurements)

- `ProjectIndex.db`: assets=385, dependencies=743, crg parity exact, 0 orphan
  CRG edges. `assets` has `package_path` (UNIQUE), `saved_hash`,
  `last_modified`. ProjectIndex also has a `cpp_symbols` table (1990 rows:
  `file_path,symbol_name,symbol_type,signature,line_number,parent_symbol`).
- `EngineSource.db`: symbols=1,065,390, references=3,125,270,
  inheritance=36,529, crg parity exact. `symbols` has
  `line_start,line_end,file_id->files.path,kind,is_ue_macro,parent_symbol_id`.
  81,252 native `"references"` rows have `from_symbol_id=0` (pre-existing
  dangling refs) and the CRG projection faithfully mirrors them — already
  surfaced by `integrity:orphan_references` and `crg:orphan_edges` as a
  non-fatal warning. Snapshot/diff must treat this as baseline noise, not a
  regression.
- Offline `monolith_query.exe`: seed/review actions now include CRG cache read
  parity, scoring v3 sensitivity, `detect_changes`, `find_unused`, and
  `review_hotspots`. Offline remains read-only by default; only explicit
  `repair_fts --execute`, `repair_crg_cache --execute`, and `snapshot
  --execute` mutate derived/offline-maintenance tables.

## Prioritized High-ROI Backlog (ROI = reviewer value / (cost x regression risk))

| ID | Pattern | CRG origin | Monolith feasibility | Tier |
|---|---|---|---|---|
| RX-1 | `detect_changes` (diff/changelist -> mapped entities -> risk + impact + test-gap + priorities) | `changes.py`, `tools/review.py` | `symbols.line_start/end`+`files.path`; `assets.saved_hash`/`package_path`; reuses existing risk + impact helpers | **P0** |
| RX-2 | Offline CRG cache parity | editor already does cached risk + repair; cache-spec `REQ-006` now allows execute-gated offline rebuild | mirror `GetCachedRiskForSymbol`/`TryCachedAssetRisk` + `crg:*` health into `monolith_query.cpp`, plus `repair_crg_cache --execute` over derived `crg_*` rows | **P0** |
| RX-3 | `find_unused` (dead asset / dead symbol, advisory) | `refactor.py:find_dead_code` | 0-fan-in over `dependencies`/`"references"` + entry/macro/reflection exclusions | **P1** |
| RX-4 | `snapshot` + `diff_snapshots` (index drift) | `graph_diff.py` | over existing `crg_nodes`/`crg_edges` + `crg_meta.built_at`; nav `TSK-P1-002` | **P1** |
| RX-5 | `pre_merge_check` composed GO/NO-GO | `prompts.py:pre_merge_check` | pure composition of RX-1 + RX-3 + `health` | **P2** (after RX-1, RX-3) |
| RX-6 | Cross-DB asset<->symbol bridge | nav `TSK-P2-002`; CRG cross-community analog | `cpp_symbols` scaffold + cross-sqlite join; high value, high cost/risk | **P2** (documented, deferred) |
| RX-7 | UE-domain **sensitivity** risk factor | `changes.py:compute_risk_score` + `constants.py:SECURITY_KEYWORDS` (+0.20) | #447 `MonolithIndexReview.cpp:90` comment explicitly "replaces CRG's security/flow factors" with class-weight only — re-add one decomposed factor+reason in the existing `RiskScore` SQL/helpers; ~zero traversal cost | **P1** |
| RX-8 | `review_hotspots` (hub/fan-in + large symbol + advisory question candidates) | `analysis.py:find_hub_nodes/find_knowledge_gaps/generate_suggested_questions`, `tools/query.py:find_large_functions` | `crg_node_metrics` already has fan-in/out/descendants/risk; `symbols.line_start/end` + `files.line_count` already support LOC hotspots; project side partly overlaps no-seed `project.risk_score`, source side has no global top-risk action | **P2** (cheap, but less changelist-critical) |

Pass-2 audit (2026-05-17) net-new vs v1 of this spec: **RX-7** added (the
single cheapest sharpening — extends the most-used action with the one risk
signal #447 intentionally dropped); **RX-1 test-gap** downgraded to explicit
heuristic (see contract) because `EngineSource.db` has **no `TESTED_BY`
edge** and #447 has **no test-symbol concept** (grep-confirmed); CRG
`incremental.py` supports git/svn only (**no Perforce**) — reinforces the
`changed_paths[]`-primary contract.

Pass-3 audit (2026-05-17) net-new: **RX-8** added after the explicit
auxiliary-module sweep. The important distinction is that CRG's full
community/betweenness/surprise stack is still too expensive and weakly mapped
for UE, but the cheap subset — hub/fan-in ranking plus large-symbol ranking —
is already present in Monolith's native/cache columns and gives reviewers a
useful global "where should I be careful?" view.

## Implementation Status (2026-05-17, PR #447)

- **RX-2 — DONE (offline).** `Tools/MonolithQuery/monolith_query.cpp`:
  `try_cached_risk` + `append_crg_health_checks`; `risk_score`
  (source+project) reads `crg_node_metrics` (`scoring_version=3` after
  rebuild, `cache.status=hit`) with query-time fallback; `health` emits `crg:*`
  checks. `repair_crg_cache --execute` rebuilds only derived `crg_*` rows from
  existing indexed source/project tables; dry-run reports the plan/counts and
  stays read-only. Built + live-verified.
- **RX-1 — DONE (offline).** `source.detect_changes` /
  `project.detect_changes` in `monolith_query.cpp`: changelist → symbols/
  assets, reuses RX-2 cached risk + fallback, bounded depth-1 impact,
  advisory heuristic test-gaps (source), risk-ordered priorities,
  minimal/standard. Built + live-verified.
- **Editor parity for RX-1 — DEFERRED.** Editor `*.detect_changes` needs a
  by-file-path symbol query on `FMonolithSourceDatabase`, which currently
  carries an unrelated uncommitted local change; adding to it now would
  entangle the commit. Project-editor variant is feasible via the public
  `FMonolithIndexReview` + raw DB and is the next step.
- **RX-3 — DONE (offline).** `source.find_unused` / `project.find_unused`
  in `monolith_query.cpp`: advisory dead-symbol / orphan-asset detection
  (0 inbound refs; UE reflection/automation/entry exclusions + inheritance-
  parent guard for source; root-class exclusion for project), confidence
  graded by name-ambiguity (source) / indirect-reference class (project),
  recall-first default. Built + live-verified.
- **RX-7 — DONE (editor + offline).** Project/source risk scoring and CRG
  cache rebuild SQL now include a bounded UE-domain sensitivity factor with
  `raw_counts.sensitivity`, explicit reasons, and scoring_version=3.
- **RX-8 — DONE (editor + offline).** `source.review_hotspots` /
  `project.review_hotspots` rank capped fan-in/fan-out/risk/large hotspots
  with optional advisory questions and no community/betweenness dependency.
- **RX-4 — DONE (editor + offline).** `source.snapshot` / `project.snapshot`
  store derived CRG projection manifests with explicit `execute=true`;
  `source.diff_snapshots` / `project.diff_snapshots` compare stored/current
  manifests read-only with capped new/removed node/edge samples. The offline
  CLI mirrors this so closed-editor index drift checks use the same derived
  `crg_snapshots` contract.
- **RX-5 — DONE (editor + offline).**
  `source.pre_merge_check` / `project.pre_merge_check` compose `health`,
  `detect_changes`, and optional `find_unused` into an advisory `decision`,
  `checks[]`, and `findings[]`. The offline CLI exposes the same read-only
  contract so closed-editor review can run the same gate.
- **RX-6 — DONE (editor + offline).** `context.bridge_asset_symbols` provides
  the read-only asset<->source heuristic bridge. Offline `monolith_query
  context bridge_asset_symbols` uses ProjectIndex + EngineSource DBs read-only
  and preserves the same confidence/reasons contract without mutation.
- Verification record: `Docs/testing/2026-05-17-crg-review-extensions.md`.

## Non-Goals (correctly excluded — do not spec as work)

- CRG `flows.py` execution-flow detection / criticality. CRG's own README
  states flow detection is weak outside Python; UE reflection / delegates /
  Blueprint dispatch make it unreliable. Stays nav `P2`.
- Community detection (Leiden), architecture overview, embeddings, wiki/D3
  visualization, daemon, multi-repo registry — intentionally out of scope
  per nav PRD non-goals.
- Full CRG bridge-node betweenness and surprise scoring are excluded from
  RX-8. They depend on community IDs, cross-language assumptions, and an
  expensive NetworkX-style pass. RX-8 keeps only the cheap, cache/native
  signals that Monolith already stores.
- Edge confidence (nav `TSK-P1-003`): nav spec already judges Monolith hard
  references "already extracted, low need". Not in this backlog.
- `hints.py` session-state intent inference: Monolith already emits static
  `next_actions` on every action; marginal gain does not justify in-plugin
  session state.
- Merging `ProjectIndex.db` and `EngineSource.db`; making CRG tables
  authoritative; any native-schema migration.

## Implementation Contract

- All new actions are additive in the existing `project` / `source`
  namespaces (index-navigation nav `REQ-009`); existing direct/seed action
  output shapes are unchanged.
- Reuse, do not duplicate, the existing helpers: project risk via
  `FMonolithIndexReview::RiskScore`, source risk via
  `FMonolithSourceReview::RiskScore`, impact via the existing
  `ImpactRadius`, cached risk via `TryCachedAssetRisk` /
  `FMonolithSourceDatabase::GetCachedRiskForSymbol`.
- Common output contract stays: `status`, `summary`, `input`, `limits`,
  `truncated`, (`items`|`impact`|domain array), `next_actions`.
- Diff parsing is read-only and VCS-pluggable. The game project is Perforce;
  the Monolith plugin repo is git. Provide both an explicit
  `changed_paths[]` input (VCS-agnostic, primary contract) and an optional
  git-diff convenience like CRG `parse_git_diff_ranges`. P4 changelist
  enumeration is the caller's responsibility in P0 (pass `changed_paths`).
- `detect_changes` line->entity mapping must use the CRG overlap rule:
  `node.line_start <= range.end AND node.line_end >= range.start`
  (`changes.py:map_changes_to_nodes`).
- `find_unused` and `detect_changes` test-gap output are **advisory**: every
  item carries a `confidence` and `reasons[]`; reflection/UFUNCTION/
  Blueprint-callable/override/entry symbols and `is_ue_macro` are excluded or
  down-weighted, never hard-deleted. Recall-first like CRG, false positives
  acknowledged.
- Snapshot storage is a derived CRG artifact (new `crg_snapshots` table in
  each DB, same droppable-cache rules as the projection); never a native
  source-of-truth.

## Action Contracts

### `source.detect_changes` / `project.detect_changes` (RX-1, P0)

- params: `changed_paths` (string[]; required unless `git_base` given),
  `git_base` (string, optional; git-only convenience),
  `max_depth` (default 2), `max_results` (default 200),
  `detail_level=minimal|standard` (default `minimal`), `include_source=false`.
- source mapping: `changed_paths` -> `files.path` (suffix match) ->
  overlapping `symbols` by line range.
- project mapping: `changed_paths` -> `assets.package_path` (and `.uasset`/
  `.umap` path normalization); `git_base` unsupported for project (assets are
  binary) -> require `changed_paths`.
- output: `status`, `summary`, `input`, `limits`, `risk_score` (max of item
  risks), `changed_entities[]` (each: id/name/path + reused risk
  `score/tier/reasons[]/raw_counts{}` + `cache`), `impact` (bounded radius of
  changed set), `test_gaps[]` (advisory), `review_priorities[]` (top-N by
  risk), `truncated`, `next_actions[]`.
- `detail_level=minimal` returns only summary, risk_score,
  changed_entity_count, test_gap_count, top-3 priorities (names only) —
  mirrors `detect_changes_func` minimal.
- **test-gap is heuristic, not edge-backed.** Unlike CRG (which has a
  `TESTED_BY` edge + `get_transitive_tests`), `EngineSource.db` has no test
  edge and #447 has no test-symbol concept (grep-confirmed). Define a
  test-symbol heuristic: `files.path` under a `Tests/` segment OR
  name/signature matching UE automation macros (`IMPLEMENT_*_AUTOMATION_TEST`,
  `*Spec`, `DEFINE_SPEC`, `TEST_*`). Coverage = changed symbol is in the
  callee-closure (bounded) of any test symbol. Every `test_gaps[]` item
  carries `confidence` and a reason naming the heuristic; the action output
  states this is recall-first and may yield false "gaps" for
  reflection/Blueprint-driven tests. A real `TESTED_BY` projection edge is a
  future item, not P0.
- optional `group_by_file=true` returns a per-file rollup (port of CRG
  `query_graph` `file_summary`): file -> changed symbols + max risk; cheap,
  pairs naturally with the changelist input.
- risk reuses cached metrics when present (same hit/miss/fallback as
  `risk_score`); cache miss -> query-time, never an error.

### Offline cache read parity (RX-2, P0)

- `monolith_query.exe <source|project> risk_score`: when `crg_node_metrics`
  + `crg_meta` exist and are consistent, read them (rebuilt caches use
  scoring_version=3, `cache.status=hit`); else fall back to query-time
  scoring v3 (`cache.status=miss`/`unavailable`) — behavior identical to the
  editor helper, read-only, never writes, never runs `repair_crg_cache`.
- `monolith_query.exe <source|project> health`: include the same `crg:*`
  checks the editor `ComputeHealth`/`Health` emit (table/index presence,
  node/edge/metric parity, orphan CRG edges, `crg_meta.cache_version`,
  `crg_meta.scoring_version`). Missing cache = informational, not error.
- offline `repair_crg_cache` writer parity is now accepted as the next
  implemented execute-gated follow-up: `monolith_query.exe <source|project>
  repair_crg_cache --execute` may rebuild the derived `crg_*` cache without
  an editor, while dry-run stays read-only and reports the plan/counts.

### `source.find_unused` / `project.find_unused` (RX-3, P1)

- params: `kind` (project: optional asset_class filter; source: `function|
  class|struct|all`), `limit` (default 100), `min_confidence` (default
  **`low`** — recall-first: an advisory dead-code action must surface all
  candidates by default; raising to `medium`/`high` is an explicit
  narrowing. The earlier `medium` default was corrected during
  implementation because it hid every genuine orphan whose class is
  commonly referenced indirectly), `include_reasons=true`.
- project: assets that are never a `dependencies.target_asset_id` and are not
  roots (exclude `World`/`Level`/`PrimaryAssetLabel`/explicit root config).
- source: `symbols` with 0 inbound `"references"`, `is_ue_macro=0`, not an
  inheritance parent, name/signature not matching UE reflection/automation
  /entry patterns; `confidence` lowered when ambiguity (overload, bare-name)
  is detected (port of `_is_plausible_caller` intent).
- output: `status`, `summary`, `items[{id,name,path,kind,confidence,reasons[]}]`,
  `truncated`, `next_actions[]`. Never mutates.

### `source.snapshot` / `project.snapshot` + `*.diff_snapshots` (RX-4, P1)

- storage: `crg_snapshots(id INTEGER PK, label TEXT, taken_at INTEGER,
  node_count INTEGER, edge_count INTEGER, manifest_json TEXT)` per DB
  (derived cache; created on first use; droppable/rebuildable).
- `snapshot` params: `label` (optional), `execute=false` default
  (dry-run reports what would be captured; execute writes one snapshot row).
  Rejected while the owning subsystem is indexing.
- `diff_snapshots` params: `before` (label/id), `after` (label/id; default =
  current live projection). Read-only.
- Offline parity mirrors the editor contract in `monolith_query.exe`:
  `snapshot --execute` is the only new write path and must open the selected
  DB read/write; dry-run `snapshot` and all `diff_snapshots` calls stay
  read-only. The action must not rebuild the CRG projection and must not
  shell out to P4/git.
- diff output (port of `graph_diff.diff_snapshots`): `new_nodes[]`,
  `removed_nodes[]`, `new_edges[]`, `removed_edges[]`, capped lists +
  `summary{nodes_added,nodes_removed,edges_added,edges_removed,
  before_total,after_total}`. The 81,252 known dangling-ref edges must be
  classified as baseline, not `removed/added`, when unchanged.

### `*.pre_merge_check` (RX-5, P2)

- Composition only: run `health`, `detect_changes` (RX-1), and optional
  `find_unused` (RX-3). Emit a single advisory gate: `status`, `decision`
  (`pass` / `warn` / `fail`), `summary`, `risk_score`, `checks[]`,
  `findings[]`, `changed_entity_count`, `impacted_count`, optional source
  `test_gap_count`, `unused_count`, `truncated`, and `next_actions[]`.
- `detail_level=minimal` returns the compact gate plus counts. `standard`
  also embeds `health`, `change_analysis`, and `unused` when requested.
- Thresholds mirror the editor implementation: any component `error` fails;
  no changed match, high risk (`risk_score >= 0.66`), broad direct impact
  (`impacted_count > 50`), source test gaps, or sampled unused candidates
  warn. The action is read-only, accepts caller-supplied changed paths, and
  never shells out to P4/git.

### `source.review_hotspots` / `project.review_hotspots` (RX-8, P2)

- params: `kind=fan_in|fan_out|risk|large|all` (default `all`),
  `limit` (default 50), `min_lines` (source only, default 100),
  `include_questions=true`.
- source: read-only query over `crg_node_metrics` joined to `crg_nodes` and
  `symbols`/`files`; `large` uses `symbols.line_start/end` and `files.path`.
  The live `EngineSource.db` already has 16,399 symbols with >=100 indexed
  lines, so this must be thresholded and capped to avoid noise.
- project: read-only query over `crg_node_metrics` joined to `crg_nodes` and
  `assets`; note that no-seed `project.risk_score` already provides a
  top-risk asset list, so `project.review_hotspots` mainly adds consistent
  fan-in/fan-out and question-style output.
- output: `status`, `summary`, `input`, `limits`,
  `hotspots[{id,name,path,kind,score,tier,signals{fan_in,fan_out,
  descendants,line_count,risk_score},reasons[]}]`, optional
  `questions[{priority,target,question,reason}]`, `truncated`,
  `next_actions[]`.
- Do not port full CRG `find_bridge_nodes` betweenness or
  `find_surprising_connections` in this action. Without community IDs and
  TESTED_BY edges, Monolith should surface cache-backed hotspot facts and
  advisory questions, not pretend it has CRG's Python/community semantics.

### Cross-DB bridge (RX-6, P2 — editor + offline, read-only)

- Add `context.bridge_asset_symbols` as the editor RX-6 bridge because the
  `context` namespace already owns both ProjectIndex and EngineSource access.
  The first implementation is heuristic/read-only rather than a persisted
  cross-sqlite join: no ProjectIndex, EngineSource, CRG cache, snapshot, P4, or
  git mutation.
- Params: one of `asset_path` or `symbol` is required; optional `limit`
  (default 20) and `detail_level` (`minimal|standard`, default `minimal`).
- Asset-seeded flow: load ProjectIndex asset details, derive candidate native
  symbol names from package path, asset name, asset class, and common UE
  prefixes/suffixes (`BP_`, `WBP_`, `DA_`, `GA_`, `_C`), then query
  EngineSource exact name and FTS results.
- Symbol-seeded flow: query EngineSource exact name/FTS first, then search
  ProjectIndex by exact and normalized symbol tokens.
- Output contract: `status`, `input`, `limits`, capped `links[]`, `warnings[]`,
  `truncated`, and `next_actions`. Each link contains `confidence`
  (`high|medium|low`), `reasons[]`, an `asset` object when available, and a
  `symbol` object when available. The action must make uncertainty explicit;
  a lexical fallback cannot masquerade as a guaranteed parent-class relation.
- Offline parity: `monolith_query context bridge_asset_symbols` mirrors the
  editor contract over `ProjectIndex.db` + `EngineSource.db` from `--db` (or
  explicit `--project-db` / `--source-db`), opens both read-only, never writes
  CRG cache/snapshots/FTS tables, and never shells out. Asset-seeded and
  symbol-seeded flows must return the same `input`, `limits`, `links[]`,
  `warnings[]`, `count`, `truncated`, `lexical_only=true`, and `next_actions`
  shape as the editor action.

### UE-domain sensitivity risk factor (RX-7, P1)

- Port the *pattern* of CRG `compute_risk_score`'s security factor
  (`changes.py:260`, `constants.py:SECURITY_KEYWORDS`, +0.20 cap), NOT the
  keyword list. #447 deliberately dropped it (`MonolithIndexReview.cpp:90`:
  "replaces CRG's security/flow factors").
- Add ONE additional decomposed factor inside the existing scoring path
  (`FMonolithIndexReview::RiskScore`, `FMonolithSourceReview::RiskScore`, the
  `crg_node_metrics` rebuild SQL, and the offline `monolith_query.cpp` risk
  path) — no new traversal, no schema change.
- UE/game-adapted sensitivity keyword set (configurable, documented): save/
  serialize/archive, network/replication/RPC/`UFUNCTION.*Server|Client|
  NetMulticast`, auth/login/account/session, purchase/IAP/store/entitlement,
  anticheat, crypt/encrypt/decrypt/sign/hash, exec/eval/command, file/
  registry/process. Match on `name`/`qualified_name`/`signature` (source) and
  `asset_class`/path (project, e.g. `Network`, `Save`, `Store`).
- Output: one extra `reasons[]` entry (e.g. `"sensitivity: replication/RPC
  surface"`) and a `raw_counts.sensitivity` flag/score; bounded contribution
  (cap ~0.15-0.20) so it tunes, never dominates. Decomposed like every other
  factor (nav non-functional req: no single magic number).
- **Ripple**: this changes scoring output, so it bumps
  `crg_meta.scoring_version` (2 -> 3) and requires a `repair_crg_cache`
  rebuild for cached parity; `risk_score`/`review_context`/`detect_changes`
  all inherit it automatically because they share the helper. Coordinate the
  version bump + cache rebuild in the same change (that ripple is exactly why
  this is P1, not P0, despite ~zero traversal cost).

## Requirement Registry

- [REQ-001] Add `project.detect_changes` + `source.detect_changes`
  (changelist/diff -> mapped entities -> reused risk + bounded impact +
  advisory test-gaps + review priorities; minimal default).
- [REQ-002] Offline `monolith_query.exe` `risk_score` reads
  `crg_node_metrics` (scoring_version=3 after rebuild/`cache.status=hit`) with safe
  query-time fallback; offline `health` emits the editor `crg:*` checks.
  `risk_score` and `health` stay read-only; offline `repair_crg_cache` is an
  explicit `--execute` writer for derived `crg_*` rows only.
- [REQ-003] Add `project.find_unused` + `source.find_unused` advisory
  dead-asset / dead-symbol detection with `confidence` + `reasons[]` and
  UE reflection/automation/entry exclusions.
- [REQ-004] Add `*.snapshot` + `*.diff_snapshots` over the existing CRG
  projection via a derived `crg_snapshots` table; treat known dangling-ref
  baseline as non-regression.
- [REQ-005] Add `*.pre_merge_check` as a pure composition of REQ-001 +
  REQ-003 + `health` producing a GO/REVIEW/NO-GO verdict.
- [REQ-006] Preserve all existing direct/seed action output shapes and
  offline behavior (additive only).
- [REQ-007] Add a read-only `context.bridge_asset_symbols`
  cross-DB asset<->symbol bridge with explicit confidence/reasons, graceful
  unavailable/indexing warnings, and no index/cache/VCS mutation. Offline CLI
  parity uses existing DB files only and remains read-only.
- [REQ-008] Add one decomposed UE-domain sensitivity factor to the shared
  risk helpers (project + source + `crg_node_metrics` rebuild + offline),
  bump `crg_meta.scoring_version` 2->3, rebuild cache for parity; bounded,
  tunable, surfaced as a `reasons[]` entry and `raw_counts.sensitivity`.
- [REQ-009] Add `source.review_hotspots` + `project.review_hotspots` as
  read-only global review triage over existing cache/native columns: fan-in,
  fan-out, cached risk, and source LOC hotspots; no community/betweenness
  dependency.

## Task Registry

- [TSK-P0-001] Implement a read-only VCS-agnostic diff/changelist mapper:
  `changed_paths[]` (+ optional git base) -> `files.path`/`assets.
  package_path` -> overlapping `symbols`/assets (CRG overlap rule).
- [TSK-P0-002] Implement `source.detect_changes` / `project.detect_changes`
  reusing existing risk + impact helpers; minimal/standard; register in
  both namespaces and in `monolith_query.cpp`.
- [TSK-P0-003] Wire offline `risk_score` + `health` to read the CRG
  projection cache (mirror editor helpers; read-only).
- [TSK-P0-004] Wire offline `repair_crg_cache --execute` to rebuild derived
  source/project `crg_*` projection rows without editor startup.
- [TSK-P1-001] Implement `*.find_unused` advisory detection with confidence
  and UE exclusion rules.
- [TSK-P1-002] Implement `crg_snapshots` derived table + `*.snapshot` /
  `*.diff_snapshots`; baseline-noise classification.
- [TSK-P1-003] Add the sensitivity factor + UE keyword set to both
  `RiskScore` helpers, the `crg_node_metrics` rebuild SQL, and the offline
  risk path; bump scoring_version 2->3; update cache/health version checks.
- [TSK-P2-001] Implement `*.pre_merge_check` composition + verdict
  thresholds.
- [TSK-P2-002] Implement `context.bridge_asset_symbols` on the existing
  `FMonolithSourceContextActions` surface; use bounded ProjectIndex search and
  EngineSource symbol search; expose heuristic confidence.
- [TSK-P2-003] Implement `*.review_hotspots` with capped fan/risk/LOC
  queries and advisory question output; register editor + offline actions.
- [TSK-P2-004] Implement offline `monolith_query context bridge_asset_symbols`
  over `ProjectIndex.db` + `EngineSource.db` with the editor RX-6 output shape.
- [TSK-001..n test tasks] see Test Registry; extend existing
  `Monolith.IndexGuard.*` temp-DB fixtures (project + source).

## Test Registry

- [TEST-001] detect_changes (source): temp DB, symbol spanning a changed
  line range is returned with reused risk fields; non-overlapping symbol is
  not; minimal output omits full bodies; cycle/`truncated` respected.
- [TEST-002] detect_changes (project): `changed_paths` of `.uasset` ->
  `package_path` resolves; risk + bounded impact present; unknown path =
  warning not crash.
- [TEST-003] offline parity: against a fixture DB with a rebuilt cache,
  `monolith_query risk_score` reports scoring_version=3 + `cache.status=hit`;
  without cache, query-time scoring stays v3 with `cache.status=miss` or
  `cache.status=unavailable`; `health` lists `crg:*` checks and expects
  `crg_meta.scoring_version=3`.
- [TEST-003b] offline repair parity: copied EngineSource/ProjectIndex DBs
  support dry-run `repair_crg_cache`, execute-gated rebuild, CRG health
  parity, and cached `risk_score` hits without mutating live DBs.
- [TEST-004] find_unused (source): a symbol with 0 inbound refs and no UE
  reflection markers is listed with confidence/reasons; a UFUNCTION/override
  /macro symbol is excluded or low-confidence.
- [TEST-005] find_unused (project): an asset with 0 referencers is listed;
  a `World`/`Level` root is excluded.
- [TEST-006] snapshot/diff: snapshot A, mutate fixture (add 1 node / drop 1
  edge), snapshot B, `diff_snapshots(A,B)` reports exactly the delta; the
  known dangling-ref baseline is not reported as added/removed.
- [TEST-007] pre_merge_check: low-risk change -> `decision=pass`; no indexed
  match, high-risk impact, test gaps, or unused candidates -> `warn`; failed
  component health/change analysis -> `fail`; pure composition (no new
  scoring).
- [TEST-008] regression: existing `Monolith.IndexGuard.*` and the 5 seed
  actions (editor + offline) keep shape and behavior.
- [TEST-009] sensitivity factor: a fixture symbol whose name matches a UE
  sensitivity keyword (e.g. `*_OnRep_*` / `ServerExec*` / `SaveGame*`) gets a
  `sensitivity` reason + bounded score lift vs an identical non-sensitive
  symbol; factor stays decomposed; `scoring_version=3` after rebuild;
  cache-hit and query-time paths agree.
- [TEST-010] review_hotspots: fixture source/project DBs with known fan-in,
  fan-out, cached risk, and source LOC values return the expected capped
  hotspot ordering and advisory questions; full community/betweenness fields
  are absent by design.
- [TEST-011] bridge_asset_symbols: asset-seeded and symbol-seeded smoke tests
  return bounded `links[]` with confidence/reasons; missing index/search
  conditions degrade to `warnings[]` instead of crashing; no write path exists.
- [TEST-011b] offline bridge_asset_symbols: copied/live read-only DB smoke
  covers asset-seeded `/Game/Maps/Interactable/BP_Wave` and symbol-seeded
  `Wave`, with non-empty links and explicit confidence/reasons.

## Traceability

- REQ-001 -> TSK-P0-001, TSK-P0-002 -> TEST-001, TEST-002
- REQ-002 -> TSK-P0-003 -> TEST-003
- REQ-003 -> TSK-P1-001 -> TEST-004, TEST-005
- REQ-004 -> TSK-P1-002 -> TEST-006
- REQ-005 -> TSK-P2-001 -> TEST-007
- REQ-006 -> all TSK -> TEST-008
- REQ-007 -> TSK-P2-002, TSK-P2-004 -> TEST-011, TEST-011b
- REQ-008 -> TSK-P1-003 -> TEST-009
- REQ-009 -> TSK-P2-003 -> TEST-010
- P2 items are outside P0/P1 acceptance until P0/P1 land and latency/value
  is measured.

## Acceptance Gate

- P0 = REQ-001 + REQ-002 (highest ROI, lowest cost, additive). Closing P0
  means a reviewer can go from a Perforce changelist / git base to a
  risk-ranked review list in one action, and offline tooling/CI gets the
  same cached risk + CRG health the editor already has.
- P1 = REQ-003 + REQ-004 + REQ-008. P2 = REQ-005 + REQ-007 +
  REQ-009.
- Each landed item must update `Docs/specs/SPEC_MonolithIndex.md`,
  `Docs/specs/SPEC_MonolithSource.md`, `Docs/API_REFERENCE.md`,
  `Docs/TODO.md`, and a `Docs/testing/<date>-crg-review-extensions.md`
  record (CLAUDE.md Docs Spec Sync), and add regression-safe automation
  tests before merge.

## Appendix: Both-Sides Survey Coverage (2026-05-17)

Every CRG `code_review_graph` module was triaged against Monolith #447 to
make this a demonstrably full audit, not a sampled one.

| CRG module | Reviewer-ROI pattern | Verdict |
|---|---|---|
| `changes.py` | diff -> nodes -> risk/test-gap/priorities | **RX-1 (P0)** |
| `tools/review.py` | `detect_changes`/`get_review_context`/`get_affected_flows` | RX-1; review_context already in #447; affected_flows excluded (flows) |
| `tools/context.py` | `get_minimal_context` (~100-tok orientation) | review core = RX-1's verdict; rest = communities/flows (excluded) |
| `refactor.py` | `find_dead_code` | **RX-3 (P1)**; `rename_preview`/`apply_refactor` = write/risky, low review-ROI (excluded); `suggest_refactorings` "move" half = community-dependent (excluded) |
| `graph_diff.py` | snapshot/diff | **RX-4 (P1)** |
| `constants.py` + `changes.py:compute_risk_score` | security/sensitivity factor (+0.20) | **RX-7 (P1)** — #447 explicitly dropped it |
| `prompts.py` | `review_changes`/`pre_merge_check` workflows | **RX-5 (P2)** = engine value; prompt templates themselves = agent sugar (skip) |
| `incremental.py` | `get_changed_files` git/svn, `detect_vcs` | feeds RX-1; **no Perforce** -> `changed_paths[]`-primary is the correct boundary |
| `tools/query.py` | `callers_of/callees_of/inheritors_of/children_of/imports_of` | already covered by #447 `find_callers/find_callees/get_class_hierarchy/find_references` |
| `tools/query.py` | `tests_for` (TESTED_BY) / `file_summary` / `find_large_functions` / `traverse_graph` / `list_graph_stats` | `tests_for` -> RX-1 heuristic test-gap (no edge in Monolith); `file_summary` -> RX-1 `group_by_file`; `find_large_functions` -> **RX-8**; traversal already covered by bounded impact/review_context; stats already covered by `health`/`get_stats` |
| `graph.py` | impact radius (recursive CTE), `get_transitive_tests`, risk factors | impact already in #447 bounded BFS; transitive tests = no Monolith test edge (RX-1 heuristic) |
| `flows.py` | entry-point detection + criticality | excluded — CRG README + nav PRD: weak for UE C++/BP/reflection (nav P2) |
| `search.py` | FTS5 hybrid (keyword+vector) | keyword side already in Monolith FTS; vector/embedding side excluded |
| `analysis.py`, `tools/analysis_tools.py` | hubs, bridges, knowledge gaps, surprising connections, suggested questions | cheap hub/hotspot/question subset -> **RX-8**; full bridges/surprise/community-dependent gap logic excluded |
| `communities.py`, `embeddings.py`, `visualization.py`, `wiki.py`, `exports.py`, `daemon.py`, `daemon_cli.py`, `registry.py`, `parser.py`, `migrations.py`, `postprocessing.py`, `skills.py`, `enrich.py`, `memory.py`, `token_benchmark.py` | community/Leiden, vectors, D3/SVG/export, wiki, daemon, multi-repo, tree-sitter parse, migrations/postprocess orchestration, skill defs, hook enrichment, local memory, eval/benchmarking | excluded by nav PRD non-goals / wrong fit for a UE C++ plugin (Monolith already has domain-native indexes) |
| `main.py`, `cli.py`, `tools/build.py`, `tools/docs.py`, `tools/*_tools.py` wrappers | MCP/CLI/prompt wrappers and docs dispatch around the underlying primitives | no separate Monolith feature beyond the underlying rows above |
| `jedi_resolver.py`, `rescript_resolver.py`, `tsconfig_resolver.py`, CRG VSCode extension | Python/Rescript/TypeScript alias/call enrichment and editor UI affordances | language/editor-specific; useful UX precedent only. Review-change UI maps to RX-1; SCM/test decorations rely on TESTED_BY edges Monolith does not have |

Net: P0 = RX-1, RX-2; P1 = RX-3, RX-4, RX-7; P2 = RX-5, RX-6, RX-8. Everything
else in CRG is either already in #447 or correctly out of scope — there is
no further un-triaged high-ROI pattern.

## Implementation Notes

- Build order: TSK-P0-003 (RX-2) first — it is the cheapest, lowest-risk win
  and immediately makes offline agents/CI cache-accurate. Then TSK-P0-001/2
  (RX-1), the highest-value reviewer entry point.
- `detect_changes` must NOT shell out to P4 in P0. Accept `changed_paths`
  explicitly; a git convenience path mirrors `parse_git_diff_ranges` for the
  Monolith plugin repo only.
- Keep all derived artifacts (`crg_*`, `crg_snapshots`) under the existing
  drop-and-rebuild rule; never trigger CRG-table triggers.
- Recall-first, advisory framing for find_unused / test-gaps is mandatory:
  UE C++ has reflection/delegate/Blueprint call edges the symbol graph does
  not capture; output must say so.
