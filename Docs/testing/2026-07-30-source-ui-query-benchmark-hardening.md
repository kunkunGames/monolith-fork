# Monolith Source, UI Registry, and Query Benchmark Hardening Verification

**Date:** 2026-07-30  
**Changelist:** 1348  
**Scope:** Source-index availability and CRG integrity, reflection-backed UI type discovery, AssetEditing benchmark coverage, and immutable offline Query bundle evidence  
**Result:** AssetEditing generation and unit contracts pass on current bytes; the isolated 2,073-action snapshot is restored; coordinated protected build, accepted OfflineParity/index refresh, one proxy-locked catalog line-ending normalization, inventory rerun, and final Perforce audit remain

---

## 1. Contract

| Surface | Required behavior |
|---|---|
| Source database availability | Every source action reports the same structured unavailable/index-request error, preserves the concrete database/index failure, and directs the caller to the valid health or indexing action instead of returning a generic empty result. |
| Source indexing lifecycle | Open, schema verification, project pruning, transaction rollback, and CRG refresh failures terminate the indexing run with the owning database error. A failed partial run is never reported as complete. |
| CRG integrity | Deep health detects orphan `crg_node_metrics` rows. CRG repair removes orphan metrics and verifies node, edge, metric, and override-cache parity before success. |
| UI widget-type discovery | `ui.list_widget_types` enumerates the initialized reflection-backed registry, accepts only the declared category enum, includes optional-plugin types, and fails closed on unavailable or stale registry state. |
| AssetEditing benchmark | PCG mutation/replacement/parameter/subgraph/surface cases and UI animation/CommonUI/registry/UISpec cases are represented by canonical tasks, test cases, shards, and inventory metadata. |
| Perforce-safe generation | Canonical generation rewrites only semantically changed UTF-8 outputs, emits the platform checkout line ending (CRLF on Windows), and deletes only obsolete JSON. Expected read-only files in a normal Perforce workspace remain untouched. |
| Offline Query identity | The benchmark validates `Binaries/monolith_query.current.json`, then executes the manifest-selected immutable executable and catalog. Missing, malformed, path-traversing, or hash-mismatched bundle members fail before benchmarking. |
| Accepted evidence | Accepted JSON/JSONL text hashes are newline-portable while executable and database inputs remain byte-exact. The accepted bundle attests every input and is rejected on inventory drift. |
| Catalog isolation | The tracked catalog snapshot is generated from CL 1348 source plus depot head only. Unsubmitted actions owned by other changelists must not leak into the submitted snapshot. |

---

## 2. Immutable Query Bundle

The authoritative manifest resolves this exact bundle:

| Item | Identity |
|---|---|
| Query executable | `Binaries/monolith_query-a2c470a0c741ff18.exe` |
| Query SHA-256 | `5b9918ad3b74b6b6f7fd46620953a46bb4d104ad4e2b8469767f964ec1707406` |
| Catalog | `Binaries/monolith_catalog-66be52a0dfce7a21809f7bb1cb9efa74c9d0641d8acba5fea7757cb5de0780f0.json` |
| Catalog source SHA-256 | `66be52a0dfce7a21809f7bb1cb9efa74c9d0641d8acba5fea7757cb5de0780f0` |
| Catalog content SHA-256 | `649a82ca470e8a5075d5807e64aa3fb6a93f13832f05f848fd9a8708992bc541` |
| Isolated action count | 2,073 |

The isolated catalog was built under
`Saved\Temp\CatalogCL1348_20260730_0430` after restoring every source file
owned by another pending changelist to depot head in that temporary tree. The
normal workspace contained 2,077 actions because other open changelists were
present. The tracked snapshot intentionally contains only the 2,073 actions
available from depot head plus CL 1348.

The fixed compatibility alias was locked by a running process during
publication. That alias is not authoritative: the manifest-selected immutable
executable and catalog above both passed strict content-hash validation.

The source-controlled snapshot was rechecked on 2026-07-30 after a normal
multi-CL workspace regeneration had contaminated it with five actions owned by
other pending changelists (`2,078` total). Regeneration from the preserved
isolated tree restored the declared `2,073` actions and semantic source hash
`66be52a0dfce7a21809f7bb1cb9efa74c9d0641d8acba5fea7757cb5de0780f0`.
Do not regenerate this snapshot from the aggregate pending workspace before
CL 1348 is submitted; use the isolated CL tree or regenerate only after the
submission order has deliberately advanced the depot baseline.

---

## 3. Accepted Offline Parity

The accepted snapshot is:

```text
Benchmarks\OfflineParity\accepted\open-cl-review-20260730-05
```

| Metric | Result |
|---|---:|
| Match | 311 |
| Diff | 0 |
| Error | 0 |
| Skip | 6 |
| Total | 317 |
| Comparable score | 1.0 |
| Input fingerprint | `c0fb73b5f4fa5fc1480f6b1efe5d09d8a3c98fff7582ed03b8e52d0f86ea7d45` |
| Summary SHA-256 | `fe82bba465097e180b0ce3cae658d4c944f6b4facb6930e076047b942c499d21` |
| EngineSource.db SHA-256 | `d6c98c5af9482ef67c306a361908a42546b33280c7bed975ab098d0ea023bfb2` |

The six skips are declared decision-chain cases. Five source missing-input
cases matched their expected structured errors; there were no real errors and
no environment-blocked rows. Strict re-verification produced `134` matches,
`0` differences, `0` errors, and `3` declared skips:

```text
Saved\Logs\Codex\20260730_OpenCLReview\CL1348-verify-offline-parity-05.log
```

This snapshot remains valid historical evidence for its exact database
fingerprint. It is not the current submission gate: subsequent coordinated
source indexing changed `Saved\EngineSource.db`, and the portable inventory
correctly rejects the present database against the snapshot's byte-exact
attestation. A new accepted snapshot must be produced only after every pending
source CL and the final source index are stable.

---

## 4. Source Index Health

Deep offline health passed against the same EngineSource database used by the
accepted parity bundle:

| Check | Result |
|---|---:|
| Native symbols / symbols FTS / CRG nodes / CRG metrics | `390098 / 390098 / 390098 / 390098` |
| Source graph nodes / graph-node FTS | `411061 / 411061` |
| References | 135,727 |
| Valid native edges / CRG edges | `820 / 820` |
| Inheritance rows | 555 |
| Source FTS rows | 712,471 |
| Orphan symbols, references, edges, or metrics | 0 |
| Maintenance required | `false` |

Evidence:

```text
Saved\Logs\Codex\20260730_OpenCLReview\CL1348-source-health-final-05.json
```

---

## 5. Verification Gates

| Gate | Required result | Current result |
|---|---|---|
| Protected editor build | `P4_BUILD_CHANGELIST=1348`, `SKIP_EDITOR_LAUNCH=1`, `Build\BatchFiles\BuildGameEditorAndRun.bat` | HISTORICAL PASS - `ProtectedBuild_CL1348_after-param-order-fix.stdout.log` records the earlier successful linked build. A current aggregate protected build is pending behind the coordinator-owned CL 1357 root fix. |
| OfflineParity unit tests | `Scripts\test_offline_parity_benchmark.py` | PASS - all 15 test functions passed. |
| AssetEditing corpus and unit tests | `Scripts\test_asset_editing_benchmark.py` | PASS - 588 tasks validated; all corpus checks passed, including read-only expected-output preservation, platform-native generated line endings, and stale-output cleanup. |
| Benchmark CI/inventory tests | `Scripts\tests\test_benchmark_ci_inventory.py` | BLOCKED - 48/49 passed; the remaining portable inventory test correctly reports `accepted OfflineParity database mtime drifted: Saved/EngineSource.db` after later source indexing. |
| Catalog generator tests | `Tools\MonolithQuery\test_generate_monolith_catalog_snapshot.py -v` | PASS - 4/4 passed, including platform-native output and semantic no-rewrite behavior for a read-only current snapshot. |
| Query bundle publication tests | `Tools\MonolithQuery\test_publish_query_bundle.py -v` | PASS - 10/10 passed, including platform-native atomic manifest bytes. |
| Default DB-resolution test | `Tools\MonolithQuery\test_default_db_resolution.py` | PASS - 1 test passed. |
| Inventory | `benchmark_inventory.py --write`, `--check`, and `--portable-check` | PENDING REFRESH - rerun after the final EngineSource database and replacement accepted OfflineParity bundle are stable. |
| Static repository checks | `ci_static_checks.py check` against the submission baseline | PENDING - the prior aggregate-workspace pass used the contaminated 2,078-action snapshot and is not accepted as isolation proof. Rerun after the current 2,073-action isolated snapshot and final CL ordering are stable. |
| Isolated catalog regeneration | Generator against depot head plus CL 1348 source | PASS - 2,073 actions; tracked snapshot was restored from `Saved\Temp\CatalogCL1348_20260730_0430` and has source hash `66be52a0dfce7a21809f7bb1cb9efa74c9d0641d8acba5fea7757cb5de0780f0`. |
| Source line endings | `TestSourceLineEndings.ps1 -ProjectRoot D:\P4\speed -Changelist 1348` | BLOCKED - 101/102 opened text files are CRLF after generator hardening and normalization. The remaining immutable catalog is read-locked by seven active `monolith_proxy-1e8f5d38528046d8.exe` processes; no proxy or editor was terminated. Normalize and rerun after those readers release the file. |
| Source automation | `Monolith.IndexGuard.Source` from the freshly linked editor | PASS - run `automation-20260729T200506Z-D0261036`; `51/51` passed, `0` failed, including invalid-parameter precedence while project source indexing was active. |
| UI registry automation | `Monolith.Registry.UI` from the freshly linked editor | PASS - run `automation-20260729T200521Z-6CF0B8D9`; `5/5` passed, `0` failed. |
| Perforce ownership | Every implementation, test, benchmark, immutable catalog, accepted result, spec, and this record is in CL 1348; unrelated/default files remain outside it. | PENDING - perform final unchanged/missing/dependency audit immediately before submit. |
| Screenshot verification | N/A | The change affects diagnostics, indexing integrity, schema-driven discovery, and benchmark evidence; it does not change runtime or editor visual presentation. |
| Discord screenshot upload | N/A | `UploadScreenshotTestsToDiscord.bat` is not run because screenshot verification is not relevant. |

---

## 6. Acceptance

CL 1348 is submit-ready only when the protected build, benchmark CI/inventory,
inventory refresh, and Perforce ownership rows above all contain exact passing
results for one stable source/index state. Both historical focused automation
prefixes passed through the freshly linked Speed editor; no result from an
unrelated project endpoint was used as CL 1348 evidence.

The first Source run after the port was released,
`automation-20260729T195228Z-A87DBA62`, passed `50/51` tests and exposed one
real ordering defect: `source.find_references` consulted the transient
`source_index_indexing` state before validating `limit` and `ref_kind`, so the
availability error masked `invalid_param`. The handler now validates the full
request before `GetDB()`. The protected rebuild and run
`automation-20260729T200506Z-D0261036` then passed the complete `51/51` prefix
while indexing was active.
