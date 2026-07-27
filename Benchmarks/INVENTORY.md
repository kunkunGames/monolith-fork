# Monolith Benchmark Completion Inventory

Snapshot: `activation-settings-compact-api-final-20260726-03`
Catalog contract: `sha256:72abda9f0901c33d` / 61 namespaces / 1863 checked-in actions
Source of truth: manifests and JSONL corpora under `Benchmarks`, plus `Benchmarks/inventory_status.json` for accepted-run evidence.
Validation modes are explicit: `--portable-check` rederives tracked accepted bundles in a clean checkout and uses recorded DB attestation only when the DB is absent; `--check` additionally requires every live DB and pending Saved diagnostic and rejects mtime/content drift.

## Done Contract

For every row: `pass + expected skip + fail + unverified + unwritten = items`.
Benchmark rows are done only when every suite is `accepted` and `fail = 0`, `unverified = 0`, and `unwritten = 0`; pending suites cannot claim numeric result credit. An expected skip counts only when the raw test row and prerequisite state prove that environment-dependent outcome.
For SchemaCompleteness full-catalog rows, `pass` means every applicable schema dimension passed (`schema_score = 1.0`); a reusable fetched row with an incomplete schema is `fail`, not merely completed coverage.
Overall Done additionally requires every fixed execution gate (`GATE-NOLINK`, `GATE-FINAL-LINK`, `GATE-CRG`, `GATE-ANIMATION`, `GATE-PRECOMMIT`) to be `passed` with evidence.
A diagnostic subset or an interrupted prefix is evidence, but never reduces the accepted `unverified` count.

## Fixed Totals

| Items | Pass | Expected skip | Fail | Unverified | Unwritten | Rows classified | Suites accepted | Benchmark rows done | Gates passed | Overall Done |
| ---: | ---: | ---: | ---: | ---: | ---: | :---: | ---: | :---: | ---: | :---: |
| 4557 | 311 | 6 | 0 | 4240 | 0 | NO | 1/8 | NO | 4/5 | NO |

## Suite Summary

| Suite | Namespace rows | Items | Pass | Expected skip | Fail | Unverified | Unwritten | State | Gap | Evidence / diagnostic |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | --- | --- | --- |
| OfflineParity | 5 | 317 | 311 | 6 | 0 | 0 | 0 | accepted |  | Benchmarks/OfflineParity/accepted/activation-settings-compact-api-final-20260726-03/summary.json; Compact UMonolithSettings activation API, project-default-aware cache, and shared parity fixture against the final single-writer EngineSource baseline: 311 MATCH, 0 DIFF, 0 ERROR, 6 explicit decision_id-dependent SKIP. The accepted bundle fingerprints the verified 3.83 GB EngineSource.db content SHA-256. |
| ActionGuidance | 61 | 569 | 0 | 0 | 0 | 569 | 0 | pending | GAP-ACTION-001 | Saved/Monolith/Benchmarks/ActionGuidance/diagnostic-legacy-migrations/run-02/summary.json; 16/16 retired-action migration subset passed; explicit subset is non-canonical and does not reduce the 454-row full-run gap. |
| SourceIndex | 1 | 374 | 0 | 0 | 0 | 374 | 0 | pending | GAP-SOURCE-001 | Saved/Monolith/Benchmarks/SourceIndex/audit-20260717-full01/partial_summary.json; 284 valid prefix rows followed by 3 transport failures; interrupted results are non-comparable, so all 374 rows remain unverified. |
| SchemaCompleteness probe contract | 51 | 329 | 0 | 0 | 0 | 329 | 0 | pending | GAP-SCHEMA-PROBE-001 | The previously reported 329-probe pre-link artifact is not retained locally, so no positive result is claimed. All probe rows remain unverified until the final-linked exact catalog identity is captured and the canonical probe run is retained. |
| SchemaCompleteness live full catalog | 61 | 1863 | 0 | 0 | 0 | 1863 | 0 | pending | GAP-SCHEMA-FULL-001 | Saved/Monolith/Benchmarks/SchemaCompleteness/diagnostic-live-prebuild-20260718-01/summary.json; 20/20 bounded diagnostic rows passed, but max-actions output is non-comparable; all 1,857 checked-in pre-link catalog-contract rows remain unverified. |
| ProjectIndex | 1 | 314 | 0 | 0 | 0 | 314 | 0 | pending | GAP-PROJECT-001 | Saved/Monolith/Benchmarks/ProjectIndex/baseline-20260711e/summary.json; The retained legacy run contains 314 rows but predates the current fail-closed identity and fixture-provenance contracts; it receives no completion credit and fixtures must be regenerated before a current full run. |
| AICapability | 1 | 212 | 0 | 0 | 0 | 212 | 0 | pending | GAP-AI-001 | Saved/Monolith/Benchmarks/AICapability/baseline-20260711d/summary.json; The older run covered 74 rows; the current canonical corpus has 212, so no row is credited to current full-run completion. |
| AssetEditing | 3 | 579 | 0 | 0 | 0 | 579 | 0 | pending | GAP-ASSET-001 | Saved/Monolith/Benchmarks/AssetEditing/baseline-20260711-p1/summary.json; The older run covered 578 rows and predates BEB-429 lifecycle correction; the current canonical corpus has 579. |

## Namespace Inventory

### OfflineParity

| Namespace | Items | Pass | Expected skip | Fail | Unverified | Unwritten |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| cppreflect | 96 | 96 | 0 | 0 | 0 | 0 |
| decision | 50 | 44 | 6 | 0 | 0 | 0 |
| network | 46 | 46 | 0 | 0 | 0 | 0 |
| risk | 62 | 62 | 0 | 0 | 0 | 0 |
| source | 63 | 63 | 0 | 0 | 0 | 0 |

### ActionGuidance

| Namespace | Items | Pass | Expected skip | Fail | Unverified | Unwritten |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| ai | 9 | 0 | 0 | 0 | 9 | 0 |
| animation | 23 | 0 | 0 | 0 | 23 | 0 |
| artifact | 7 | 0 | 0 | 0 | 7 | 0 |
| asset | 16 | 0 | 0 | 0 | 16 | 0 |
| audio | 19 | 0 | 0 | 0 | 19 | 0 |
| blueprint | 33 | 0 | 0 | 0 | 33 | 0 |
| bridge | 6 | 0 | 0 | 0 | 6 | 0 |
| build | 5 | 0 | 0 | 0 | 5 | 0 |
| bulk_fill | 2 | 0 | 0 | 0 | 2 | 0 |
| chaos_fracture | 4 | 0 | 0 | 0 | 4 | 0 |
| chooser | 16 | 0 | 0 | 0 | 16 | 0 |
| cloth | 4 | 0 | 0 | 0 | 4 | 0 |
| collection | 9 | 0 | 0 | 0 | 9 | 0 |
| config | 18 | 0 | 0 | 0 | 18 | 0 |
| cppreflect | 9 | 0 | 0 | 0 | 9 | 0 |
| dataflow | 3 | 0 | 0 | 0 | 3 | 0 |
| decision | 9 | 0 | 0 | 0 | 9 | 0 |
| describe | 3 | 0 | 0 | 0 | 3 | 0 |
| editor | 12 | 0 | 0 | 0 | 12 | 0 |
| gamefeatures | 2 | 0 | 0 | 0 | 2 | 0 |
| gameplay_message | 6 | 0 | 0 | 0 | 6 | 0 |
| gas | 19 | 0 | 0 | 0 | 19 | 0 |
| hlod | 7 | 0 | 0 | 0 | 7 | 0 |
| imagegen | 5 | 0 | 0 | 0 | 5 | 0 |
| input | 9 | 0 | 0 | 0 | 9 | 0 |
| interchange | 8 | 0 | 0 | 0 | 8 | 0 |
| level_instance | 6 | 0 | 0 | 0 | 6 | 0 |
| level_sequence | 6 | 0 | 0 | 0 | 6 | 0 |
| leveldesign | 9 | 0 | 0 | 0 | 9 | 0 |
| loading | 6 | 0 | 0 | 0 | 6 | 0 |
| localization | 16 | 0 | 0 | 0 | 16 | 0 |
| lyra | 8 | 0 | 0 | 0 | 8 | 0 |
| material | 25 | 0 | 0 | 0 | 25 | 0 |
| mesh | 11 | 0 | 0 | 0 | 11 | 0 |
| metahuman | 4 | 0 | 0 | 0 | 4 | 0 |
| modelgen | 6 | 0 | 0 | 0 | 6 | 0 |
| modular | 6 | 0 | 0 | 0 | 6 | 0 |
| monolith | 5 | 0 | 0 | 0 | 5 | 0 |
| movie_render | 3 | 0 | 0 | 0 | 3 | 0 |
| ndisplay | 3 | 0 | 0 | 0 | 3 | 0 |
| network | 5 | 0 | 0 | 0 | 5 | 0 |
| niagara | 23 | 0 | 0 | 0 | 23 | 0 |
| notify | 4 | 0 | 0 | 0 | 4 | 0 |
| online | 5 | 0 | 0 | 0 | 5 | 0 |
| paper2d | 8 | 0 | 0 | 0 | 8 | 0 |
| pcg | 6 | 0 | 0 | 0 | 6 | 0 |
| pipeline | 2 | 0 | 0 | 0 | 2 | 0 |
| project | 18 | 0 | 0 | 0 | 18 | 0 |
| reflect | 2 | 0 | 0 | 0 | 2 | 0 |
| risk | 11 | 0 | 0 | 0 | 11 | 0 |
| scene | 18 | 0 | 0 | 0 | 18 | 0 |
| settings | 6 | 0 | 0 | 0 | 6 | 0 |
| slate | 2 | 0 | 0 | 0 | 2 | 0 |
| source | 33 | 0 | 0 | 0 | 33 | 0 |
| source_control | 7 | 0 | 0 | 0 | 7 | 0 |
| sprite | 8 | 0 | 0 | 0 | 8 | 0 |
| ui | 16 | 0 | 0 | 0 | 16 | 0 |
| water | 3 | 0 | 0 | 0 | 3 | 0 |
| workflow | 2 | 0 | 0 | 0 | 2 | 0 |
| world_conditions | 6 | 0 | 0 | 0 | 6 | 0 |
| worldgen | 7 | 0 | 0 | 0 | 7 | 0 |

### SourceIndex

| Namespace | Items | Pass | Expected skip | Fail | Unverified | Unwritten |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| source | 374 | 0 | 0 | 0 | 374 | 0 |

### SchemaCompleteness probe contract

| Namespace | Items | Pass | Expected skip | Fail | Unverified | Unwritten |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| ai | 17 | 0 | 0 | 0 | 17 | 0 |
| animation | 22 | 0 | 0 | 0 | 22 | 0 |
| asset | 6 | 0 | 0 | 0 | 6 | 0 |
| audio | 12 | 0 | 0 | 0 | 12 | 0 |
| blueprint | 24 | 0 | 0 | 0 | 24 | 0 |
| bridge | 3 | 0 | 0 | 0 | 3 | 0 |
| bulk_fill | 1 | 0 | 0 | 0 | 1 | 0 |
| chaos_fracture | 1 | 0 | 0 | 0 | 1 | 0 |
| chooser | 4 | 0 | 0 | 0 | 4 | 0 |
| cloth | 1 | 0 | 0 | 0 | 1 | 0 |
| collection | 6 | 0 | 0 | 0 | 6 | 0 |
| combograph | 2 | 0 | 0 | 0 | 2 | 0 |
| config | 5 | 0 | 0 | 0 | 5 | 0 |
| cppreflect | 4 | 0 | 0 | 0 | 4 | 0 |
| dataflow | 2 | 0 | 0 | 0 | 2 | 0 |
| decision | 2 | 0 | 0 | 0 | 2 | 0 |
| describe | 2 | 0 | 0 | 0 | 2 | 0 |
| editor | 4 | 0 | 0 | 0 | 4 | 0 |
| gamefeatures | 1 | 0 | 0 | 0 | 1 | 0 |
| gas | 21 | 0 | 0 | 0 | 21 | 0 |
| hlod | 3 | 0 | 0 | 0 | 3 | 0 |
| imagegen | 4 | 0 | 0 | 0 | 4 | 0 |
| input | 5 | 0 | 0 | 0 | 5 | 0 |
| interchange | 7 | 0 | 0 | 0 | 7 | 0 |
| level_instance | 4 | 0 | 0 | 0 | 4 | 0 |
| level_sequence | 3 | 0 | 0 | 0 | 3 | 0 |
| leveldesign | 3 | 0 | 0 | 0 | 3 | 0 |
| localization | 5 | 0 | 0 | 0 | 5 | 0 |
| logicdriver | 2 | 0 | 0 | 0 | 2 | 0 |
| material | 17 | 0 | 0 | 0 | 17 | 0 |
| mesh | 12 | 0 | 0 | 0 | 12 | 0 |
| metahuman | 1 | 0 | 0 | 0 | 1 | 0 |
| modelgen | 3 | 0 | 0 | 0 | 3 | 0 |
| monolith | 8 | 0 | 0 | 0 | 8 | 0 |
| movie_render | 2 | 0 | 0 | 0 | 2 | 0 |
| ndisplay | 1 | 0 | 0 | 0 | 1 | 0 |
| network | 2 | 0 | 0 | 0 | 2 | 0 |
| niagara | 18 | 0 | 0 | 0 | 18 | 0 |
| paper2d | 3 | 0 | 0 | 0 | 3 | 0 |
| pcg | 2 | 0 | 0 | 0 | 2 | 0 |
| project | 10 | 0 | 0 | 0 | 10 | 0 |
| risk | 2 | 0 | 0 | 0 | 2 | 0 |
| scene | 17 | 0 | 0 | 0 | 17 | 0 |
| slate | 1 | 0 | 0 | 0 | 1 | 0 |
| source | 26 | 0 | 0 | 0 | 26 | 0 |
| source_control | 3 | 0 | 0 | 0 | 3 | 0 |
| sprite | 1 | 0 | 0 | 0 | 1 | 0 |
| ui | 14 | 0 | 0 | 0 | 14 | 0 |
| water | 1 | 0 | 0 | 0 | 1 | 0 |
| world_conditions | 3 | 0 | 0 | 0 | 3 | 0 |
| worldgen | 6 | 0 | 0 | 0 | 6 | 0 |

### SchemaCompleteness live full catalog

| Namespace | Items | Pass | Expected skip | Fail | Unverified | Unwritten |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| asset | 20 | 0 | 0 | 0 | 20 | 0 |
| blueprint | 140 | 0 | 0 | 0 | 140 | 0 |
| material | 66 | 0 | 0 | 0 | 66 | 0 |
| paper2d | 3 | 0 | 0 | 0 | 3 | 0 |
| animation | 212 | 0 | 0 | 0 | 212 | 0 |
| cloth | 2 | 0 | 0 | 0 | 2 | 0 |
| metahuman | 2 | 0 | 0 | 0 | 2 | 0 |
| chooser | 16 | 0 | 0 | 0 | 16 | 0 |
| niagara | 129 | 0 | 0 | 0 | 129 | 0 |
| editor | 82 | 0 | 0 | 0 | 82 | 0 |
| scene | 57 | 0 | 0 | 0 | 57 | 0 |
| build | 2 | 0 | 0 | 0 | 2 | 0 |
| artifact | 2 | 0 | 0 | 0 | 2 | 0 |
| notify | 1 | 0 | 0 | 0 | 1 | 0 |
| config | 11 | 0 | 0 | 0 | 11 | 0 |
| localization | 10 | 0 | 0 | 0 | 10 | 0 |
| project | 24 | 0 | 0 | 0 | 24 | 0 |
| collection | 13 | 0 | 0 | 0 | 13 | 0 |
| source | 37 | 0 | 0 | 0 | 37 | 0 |
| bridge | 5 | 0 | 0 | 0 | 5 | 0 |
| source_control | 11 | 0 | 0 | 0 | 11 | 0 |
| ui | 165 | 0 | 0 | 0 | 165 | 0 |
| slate | 1 | 0 | 0 | 0 | 1 | 0 |
| mesh | 71 | 0 | 0 | 0 | 71 | 0 |
| level_instance | 16 | 0 | 0 | 0 | 16 | 0 |
| hlod | 12 | 0 | 0 | 0 | 12 | 0 |
| leveldesign | 43 | 0 | 0 | 0 | 43 | 0 |
| worldgen | 36 | 0 | 0 | 0 | 36 | 0 |
| modelgen | 7 | 0 | 0 | 0 | 7 | 0 |
| imagegen | 10 | 0 | 0 | 0 | 10 | 0 |
| sprite | 9 | 0 | 0 | 0 | 9 | 0 |
| interchange | 16 | 0 | 0 | 0 | 16 | 0 |
| ndisplay | 2 | 0 | 0 | 0 | 2 | 0 |
| dataflow | 2 | 0 | 0 | 0 | 2 | 0 |
| gamefeatures | 9 | 0 | 0 | 0 | 9 | 0 |
| lyra | 21 | 0 | 0 | 0 | 21 | 0 |
| online | 8 | 0 | 0 | 0 | 8 | 0 |
| modular | 4 | 0 | 0 | 0 | 4 | 0 |
| gameplay_message | 5 | 0 | 0 | 0 | 5 | 0 |
| settings | 6 | 0 | 0 | 0 | 6 | 0 |
| loading | 4 | 0 | 0 | 0 | 4 | 0 |
| chaos_fracture | 3 | 0 | 0 | 0 | 3 | 0 |
| pcg | 28 | 0 | 0 | 0 | 28 | 0 |
| water | 2 | 0 | 0 | 0 | 2 | 0 |
| input | 10 | 0 | 0 | 0 | 10 | 0 |
| gas | 142 | 0 | 0 | 0 | 142 | 0 |
| ai | 182 | 0 | 0 | 0 | 182 | 0 |
| audio | 98 | 0 | 0 | 0 | 98 | 0 |
| level_sequence | 13 | 0 | 0 | 0 | 13 | 0 |
| movie_render | 13 | 0 | 0 | 0 | 13 | 0 |
| world_conditions | 4 | 0 | 0 | 0 | 4 | 0 |
| decision | 5 | 0 | 0 | 0 | 5 | 0 |
| risk | 5 | 0 | 0 | 0 | 5 | 0 |
| cppreflect | 6 | 0 | 0 | 0 | 6 | 0 |
| network | 4 | 0 | 0 | 0 | 4 | 0 |
| pipeline | 2 | 0 | 0 | 0 | 2 | 0 |
| reflect | 1 | 0 | 0 | 0 | 1 | 0 |
| monolith | 37 | 0 | 0 | 0 | 37 | 0 |
| workflow | 11 | 0 | 0 | 0 | 11 | 0 |
| bulk_fill | 2 | 0 | 0 | 0 | 2 | 0 |
| describe | 3 | 0 | 0 | 0 | 3 | 0 |

### ProjectIndex

| Namespace | Items | Pass | Expected skip | Fail | Unverified | Unwritten |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| project | 314 | 0 | 0 | 0 | 314 | 0 |

### AICapability

| Namespace | Items | Pass | Expected skip | Fail | Unverified | Unwritten |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| ai | 212 | 0 | 0 | 0 | 212 | 0 |

### AssetEditing

| Namespace | Items | Pass | Expected skip | Fail | Unverified | Unwritten |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| blueprint | 289 | 0 | 0 | 0 | 289 | 0 |
| mixed | 269 | 0 | 0 | 0 | 269 | 0 |
| project | 21 | 0 | 0 | 0 | 21 | 0 |

## Remaining Gap List

Only these declared gaps and execution gates may expand the remaining work. New gaps require a concrete failed row or a manifest/catalog identity change.

| ID | Scope | Remaining | Done when | Blocker / sequencing |
| --- | --- | ---: | --- | --- |
| GAP-SCHEMA-FULL-001 | SchemaCompletenessFullCatalog | 1863 | A final-linked current-identity exact-catalog scan publishes comparable=true, zero fetch failures, and zero quality failures for every catalog action. | Run against the final linked live catalog; resumable diagnostics may proceed beforehand. |
| GAP-SCHEMA-PROBE-001 | SchemaCompletenessProbe | 329 | All 329 declared probes finish under the same current catalog identity with no required stale row. | Run after the full live catalog identity is fixed. |
| GAP-SOURCE-001 | SourceIndex | 374 | All 374 canonical rows complete in one valid run with zero transport/protocol failures. | Live HTTP MCP must remain stable for the full sequence. |
| GAP-ACTION-001 | ActionGuidance | 569 | All 454 canonical rows complete with current task and catalog fingerprints. | Regenerate once against the final catalog, then run the canonical corpus. |
| GAP-PROJECT-001 | ProjectIndex | 314 | Live fixtures are regenerated and all 314 rows pass the fail-closed provenance gate. | Fixture refresh must precede the full run. |
| GAP-AI-001 | AICapability | 212 | All 212 canonical AI rows complete with current fixture and catalog identity. | Run after read-only index suites. |
| GAP-ASSET-001 | AssetEditing | 579 | All 579 canonical rows complete with jobs=1 and zero unexpected errors. | Run last because it mutates benchmark assets. |

## Execution Gates

| ID | Status | Contract | Evidence |
| --- | --- | --- | --- |
| GATE-NOLINK | passed | Before the coordinated editor-down window, use only protected -NoLink partial compilation and do not replace the protected build scripts with direct UBT/UAT. | 2026-07-18: the latest CL 1100 protected strict non-unity -NoLink compile of MonolithCore+MonolithAnimation passed 111/111 compile actions. Core/Animation DLL and PDB SHA-256 values were identical before and after; no link, binary deletion, process termination, or binary reconcile ran. |
| GATE-FINAL-LINK | passed | After CL 1198 PCG live revalidation, use one user-coordinated editor-down window for the CL 1100 protected link build. | 2026-07-21: BuildGameEditorAndRun.bat completed the normal SpeedEditor build with exit 0; BuildGameEditorStrictNonUnity.bat then compiled and linked 1,472 actions under Strict, WarningsAsErrors, DisableUnity, and NoUBTMakefiles with exit 0. |
| GATE-CRG | passed | Source health must prove CRG parity, then OfflineParity must be rerun against that exact EngineSource.db and promoted as a schema-v2 tracked accepted bundle. | 2026-07-21: source.repair_crg_cache rebuilt 91,028 edges, 1,328,817 node metrics, and 133,279 override edges; follow-up deep source.health was clean. graph-retirement-20260721-08 then completed 317/317 rows as 311 MATCH, 0 DIFF, 0 ERROR, and 6 exact prerequisite-dependent SKIP rows against that EngineSource.db and the final Query binary. |
| GATE-ANIMATION | pending_after_final_link | After MCP recovery, every discovered Monolith.ParamGuard.Animation test must pass on the newly linked binary (currently 11/11). |  |
| GATE-PRECOMMIT | passed | Run static CI, portable and full inventory checks, final P4/CL/default-CL audit, and review/reflection evidence after the final accepted bundles are fixed. | 2026-07-21: final graph-retirement OfflineParity scored 1.0; hosted static CI completed with 0 blocking findings; benchmark inventory unit tests passed; portable and full inventory checks verified 229 rows; Strict Non-Unity compiled and linked 1,472 actions; git diff --check passed; final scope and accepted-bundle reviews found no blocking regression. |

## Changelist Boundary

- CL 1100 (`bench`): Benchmark corpora, runners, benchmark-generated fixtures/assets, and Monolith defects directly discovered by those benchmark runs (currently Core, Animation, native Proxy, and Query CRG repair).
- CL 1200 (`Monolith`): UI visual-artifact action/API implementation, its focused automation test, and UI/API documentation only.
- Verified overlap: 0 exact files and 0 source modules as of 2026-07-18; CL 1100 touches MonolithCore/MonolithAnimation while CL 1200 touches MonolithUI. Content/Monolith/CommonUI/MonolithDefaultCommonButton.uasset remains in CL 1100 because three CL 1100 benchmark assets hard-reference it: WBP_BenchCommonButton, WBP_BenchCommonContainers, and WBP_BenchCommonPauseMenu.

Regenerate and validate:

```powershell
python Scripts\benchmark_inventory.py --write
python Scripts\benchmark_inventory.py --portable-check
python Scripts\benchmark_inventory.py --check
```
