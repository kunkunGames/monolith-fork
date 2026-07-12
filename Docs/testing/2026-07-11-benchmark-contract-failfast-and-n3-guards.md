# Benchmark Contract Fail-Fast + N3 Cross-Action Drift Guards

- Date: 2026-07-11
- Scope: SchemaCompleteness enumeration and transport fail-fast repair,
  live-catalog corpus migration, ActionGuidance full-catalog generation,
  SourceControl Perforce batching, ProjectIndex fixture portability,
  AICapability composite-scaffold guard, and fresh benchmark verification.
- Change: Speed P4 CL 1093

---

## 1. What changed

| Area | Change |
| --- | --- |
| `Scripts/schema_completeness_benchmark.py` | Catalog enumeration rebuilt for the compact discover contract: summary rows carry only `action_count`; action names are enumerated per namespace via paginated `monolith_discover(mode="actions")`. Strict JSON, canonical action IDs, duplicate expected dimensions, required-probe presence, status preflight, and catalog-version recheck all fail closed. Schema fetch returns a structured outcome so transport, protocol, missing-schema, and runner errors cannot be flattened into a schema score. Invalid runs write `run_failure.json` plus diagnostic `partial_summary.json`, never `summary.json`. |
| `Scripts/benchmark_common.py` + `Scripts/tests/test_benchmark_common_transport_gate.py` | Shared `TransportFailureTracker` used by SchemaCompleteness and ActionGuidance. Three consecutive transport failures abort from request one; cumulative failures above 5% abort after 20 requests; `finalize()` also checks shorter completed populations. Ten boundary tests cover reset, exact threshold, empty raw text, small populations, status identity, and invalid configuration. |
| `Scripts/tests/test_schema_completeness_enumeration.py` (new) | 42 offline tests cover compact discovery pagination, transport and catalog drift, strict probe preflight, scan/probe invalid artifacts, status failure, consecutive/fraction gates, runner exceptions, stale-output removal, and successful completion. |
| `Scripts/ai_capability_benchmark.py` + regenerated `Benchmarks/AICapability/tasks.jsonl`, `manifest.json` | New `edit_execute` chain task (73→74): `create_bt_from_template(patrol)` against `/Game/Benchmarks/AI/BT_BenchTemplateScratch` with delete-first resets (`delete_behavior_tree`, `delete_blackboard`), read back via `get_behavior_tree` `contains BB_BenchTemplateScratch`. Guards the 2026-07-10 handoff N3 class: a composite scaffold whose internal `set_bt_blackboard` dispatch drifts fails the chain. The final contract deletes both generated packages and independently requires both public reads to report exact-path `not found`; any create-based edit row without cleanup and cleanup assertions fails corpus integrity. |
| `Benchmarks/SchemaCompleteness/probe_set.jsonl` + `manifest.json` (388→330) | Removed 122 stale action IDs and added 64 current-contract probes. The tracked migration classifies every source probe and records direct, partial, composite, and no-equivalent mappings. Current availability contract is 315 required, 4 optional, and 11 feature-gated probes. |
| `Source/MonolithConfig/Private/MonolithConfigActions.cpp` | `config.resolve_setting` `file` param Required→Optional. When omitted, section/key is searched across Engine, Game, Input, Editor, Scalability, GameUserSettings; response reports matched `category` + `searched_categories`. Present-but-non-string `file` stays `ErrInvalidParams`. Explicit-`file` behavior unchanged. |
| `Source/MonolithConfig/Private/Tests/MonolithConfigSetterTest.cpp` | 4 new automation tests `Monolith.Config.ResolveSetting.*`: NoFileCategorySearch (seeded probe key found via Engine branch), ExplicitFile (legacy path, no `searched_categories`), MalformedFile (`ErrInvalidParams`), NoFileNotFound (`found=false` + all six categories searched). Probe key is seeded into the in-memory Engine config branch and removed afterwards (project-agnostic). |
| Docs | Monolith module specs, `Plugins/Monolith/Docs/API_REFERENCE.md`, benchmark README/METRICS/RESULTS, and this verification record are synchronized in CL 1093. Project-level `Docs/API_REFERENCE.md` remains file-granularly open in unrelated CL 1082, and `Plugins/Monolith/Docs/SPEC_CORE.md` remains open in unrelated CL 1097; neither file was moved or overwritten. |

## 2. Root-cause evidence (2026-07-10 handoff §0a rev.2)

- **N5**: `schema_completeness_benchmark.py scan` enumerated 0 actions after the compact discover
  change and recorded `score 0.0` with exit 0 — reproduced against the live 0.20.3 endpoint before
  the fix (old enumeration read `namespaces[].actions`, which summary rows no longer inline).
- **N3 (config)**: live call `config.resolve_setting {section, key}` without `file` →
  `Missing required param(s): [file]` (reproduced 2026-07-11 02:5x KST, editor 0.20.3, 1831 actions).
- **N3 (ai scaffold)**: `ai.create_bt_from_template` internal `set_bt_blackboard` dispatch already
  uses `asset_path`/`blackboard_path` in this checkout (MonolithAIScaffoldActions.cpp L54/L1896) and
  the live call succeeds (7-node patrol BT, Blackboard linked and read back). The new benchmark task
  pins this contract so a future drift is caught by the suite instead of production logs.

## 3. Offline verification (no editor required)

| Check | Result |
| --- | --- |
| `python -m unittest Scripts.tests.test_benchmark_common_transport_gate` | 10/10 OK |
| `python -m unittest Scripts.tests.test_schema_completeness_enumeration` | 42/42 OK |
| `python Scripts/tests/test_schema_completeness_value_domain.py` | 23/23 OK (regression) |
| `python -m unittest Scripts.tests.test_action_guidance_routing_weight` | 27/27 OK (regression) |
| `python Scripts/ai_capability_benchmark.py generate` | 74 tasks, manifest task_count=74, weights sum 1.0 |
| `python Scripts/test_ai_capability_benchmark.py` | 39/39 PASS |
| `python Scripts/test_source_index_benchmark.py` | 35/35 PASS |
| `python Scripts/test_project_index_benchmark.py` | 23/23 PASS |
| `python Scripts/offline_parity_benchmark.py` contract self-test | 12/12 PASS |
| Shared pagination / task-corpus / CI-inventory suites | 5/5, 6/6, 3/3 PASS |
| Configured `benchmark_contract_tests` inventory | **13/13 scripts exited 0** after the final AICapability cleanup addition |
| `python Scripts/ci_static_checks.py ... check` | No benchmark-count/py findings from this change (remaining blockers are pre-existing workspace `Binaries/*` repo-hygiene and CRLF findings unrelated to this CL) |

## 4. Live verification (editor 0.20.3)

- Enumeration parity: fixed scan discovered **1831 actions** across 61 namespaces — exactly the
  live catalog count (previously 0).
- `ai.create_bt_from_template(save_path=/Game/Benchmarks/AI/BT_BenchTemplateScratch, template=patrol)`
  → success, `blackboard=/Game/Benchmarks/AI/BB_BenchTemplateScratch.BB_BenchTemplateScratch`,
  `get_behavior_tree` read-back contains `BB_BenchTemplateScratch` (the new task's verify contract,
  verified end-to-end by hand before generation).
- Editor-instability handling: a mid-scan editor crash burst (18/100 `schema_not_returned` in the
  blueprint namespace during a smoke scan) now trips the `--max-failed-fraction` gate instead of
  recording a depressed baseline.
- `config.resolve_setting` optional-`file` C++ change: compiled via the 08:49/09:00 watchdog UBT
  cycles and verified live — `config.resolve_setting {section, key}` without `file` now resolves
  (`category:"Engine"` + `searched_categories` reported). Automation tests
  `Monolith.Config.ResolveSetting.*` via `editor.run_automation_tests`: **4/4 passed**
  (NoFileCategorySearch, ExplicitFile, MalformedFile, NoFileNotFound). The first run caught that
  `FJsonValue::TryGetString` silently coerces JSON numbers; the handler now uses the strict
  `MonolithParamUtils::GetOptionalStringParam` (IsJsonString) policy.
- `monolith.guide` wire schema post-rebuild: `section` carries `required:false` plus the 6-value
  enum; catalog fingerprint rotated (`sha256:546e23a1c2d3f137`).
- Current catalog audit: **1840 actions / 61 namespaces** at fingerprint
  `sha256:ef52979d7e00a249`. The migrated 330-probe corpus has 315 live required
  actions and 15 explicitly absent optional/feature-gated actions; required
  stale count is zero. Live strict preflight identified three default-off config
  gates that the original corpus incorrectly treated as required:
  `gamefeatures.list_plugins` (`bEnableGameFeatureActions`),
  `slate.describe_widget` (`bEnableSlateInspectorActions`), and
  `worldgen.get_building_archetype` (`bEnableProceduralTownGen`).
- `probe-20260711-valid3` proved the new in-run transport contract against an
  actual endpoint loss. The run aborted at 21/315 after 2 transport failures
  (9.52%, over the 5% budget), wrote `run_failure.json` and
  `partial_summary.json`, and did not write `summary.json`. The preceding runner
  reached 200/315 with 79 failures before manual termination; the recurrence is
  now bounded.
- `probe-20260711-valid4` then completed the same migrated contract on a stable
  endpoint in 809 seconds: 315/315 fetches, zero fetch/transport failures, zero
  stale required probes, 15 explicit gate skips, 1462/1462 expected dimension
  checks, and probe/critical/high pass rates of 1.0. The valid schema score is
  0.9460 (`value_domain_rate=0.7300`; every other dimension 1.0). Success left
  `summary.json` and removed both invalid/partial artifacts.

## 5. Baseline runs (2026-07-11, sequenced on the shared editor)

Results recorded in each suite's `RESULTS.md`; output dirs
`Saved/Monolith/Benchmarks/<Suite>/baseline-20260711*`.

| Suite | Score | Coverage | Notes |
| --- | ---: | --- | --- |
| SchemaCompleteness (scan) | **0.9395** | 1831/1831 actions, 61 ns, 0 failed | First full-catalog baseline on the fixed enumeration; `value_domain` 0.6979 is the weak dimension; `monolith.guide` was the single missing required-flag action (fixed, §5a) |
| SchemaCompleteness (probe) | run failed by design | 388 probes, 136 fetch failures (35.1%) | Editor crash-loop window; the new `--max-failed-fraction` gate exited 1 instead of recording a depressed baseline — first live catch |
| SchemaCompleteness (migrated probe) | **0.948667** | 315 scored + 15 explicit skips, 0 stale/fetch/transport failures | Final `probe-20260711-final`; all required probes scored, critical/high pass rates 1.0, `value_domain_rate=0.743333` |
| OfflineParity | **1.0** | 317 total, 311/311 comparable MATCH, 6 decision-chain skips | Final `run-20260711-final`; 0 diff, 0 real error, 5/5 expected-error matches, identical C++/Python parity revision |
| SourceIndex | **0.910407** | 363/363 tasks, 0 transport failures | Final post-reindex/CRG-repair run; `symbol_hit=0.948454`, `negative_recovery=0.807692`, stable start/end identity |
| ProjectIndex | 0.7149 | 330 tasks | Score depressed by testset rot, not index regressions (§5b) |
| ActionGuidance | **0.873396** | 289/289 tasks, 0 transport failures | Final `run-20260711-final`; stable start/end identity at catalog `sha256:4af1375172e8818a` |
| AICapability | **1.0** | 74/74 tasks, 0 transport failures | Final isolated `run-20260711-final-isolated`; all seven dimensions 1.0, stable identity, seven scratch packages absent from disk and P4 |
| ProjectIndex (rerun, Speed fixtures) | **1.0** | 314/314 tasks, 0 errors, 0 transport failures | Final `run-20260711-final`; stable identity, 30 submitted known answers, canonical task SHA-256 `901F10BD...` |

### 5a. monolith.guide schema repair (found by the scan)

`required_params_marked` 0.9994 = exactly one action missing a boolean
`required` flag: `monolith.guide` (registered with a raw FJsonObject schema
instead of FParamSchemaBuilder). Rebuilt the registration with
`FParamSchemaBuilder().Optional(...).Enum("section", {onboarding, recipes,
decisions, errors, skills_map, gotchas})` (`MonolithGuideTool.cpp`); verified
live post-rebuild: wire schema now carries `required:false` + the 6-value
enum, catalog fingerprint rotated.

### 5b. ProjectIndex testset portability rot (found by the first live run)

- All 30 `known_answer` fixtures referenced GO-project assets
  (`DA_Monster_*`, …) absent from Speed → `known_answer_hit_rate` 0.0.
- 21/40 `schema_field_presence` tasks referenced actions absent from the live
  `project` namespace (20260618 catalog snapshot) → `schema_adherence_rate`
  0.475 and all 21 `error_free_rate` misses.
- The scorer matched identity only against `match_object_path`, which in the
  live contract is the match-site field path (e.g. `MapID`), not the asset
  package path (`asset_path`).

Repairs: `known_answer_hit` now matches `asset_path` (legacy
`match_object_path` identity rows still accepted); new
`refresh_live_fixtures` subcommand derives the schema-action list (paginated
live discover) plus re-verified known-answer pairs from the CURRENT project
into `Benchmarks/ProjectIndex/live_fixtures.json` (fail-fast on thin
derivations), consumed by `generate` in place of the 20260618 snapshots.
Offline tests: 3 new branches in `Scripts/test_project_index_benchmark.py`
(8/8 total).

### 5c. AICapability text-vs-structuredContent drift (found by the fixture gate)

The live server returns a terse `content[0].text` ("OK; see
structuredContent.") with the real JSON in `structuredContent`. AICapability's
token checks (`_response_text_contains_all` for preflight/discovery,
`_verify_readback` contains/not_contains for every edit_execute read-back)
searched the text only, so the blackboard fixture preflight reported
`fixture_contract_missing` although `get_blackboard` returned all five keys —
and every edit_execute read-back (0.34 of the score) would have false-failed
the same way. Fixed with a shared `searchable_text` helper (text + structured
payload JSON); offline self-test remains 14/14.

### 5d-0. value_domain Range sweep (first slice, driven by the scan baseline)

The scan's dominant `value_domain` failure is bounded integer params whose
handlers clamp internally but never declare `minimum`/`maximum` on the wire
schema (`FParamSchemaBuilder::Range` exists and was simply unused). Declared
ranges matching each handler's actual clamp were added for 16 actions / 17
params: `chaos_fracture.list_geometry_collection_{assets,components}` (1..500),
`water.list_bodies` (1..500), `network.{list_replicated_classes,
list_rpc_functions,list_onrep_handlers,audit_unbalanced_onreps}` (1..200),
`monolith.execute_plan.max_result_bytes_per_step` (1024..262144),
`paper2d.list_assets` (1..500) + `paper2d.get_asset.tag_limit` (0..200),
`ndisplay.list_config_assets` (1..500), `dataflow.list_assets` (1..500) +
type-rows limit (1..1000), `metahuman.list_character_assets` (1..500),
`cloth.list_clothing_assets` (1..500),
`artifact.package_build_outputs` (1..50000) /
`mirror_screenshot_evidence` (1..10000). `pcg`/`imagegen` equivalents are held
back: their files are open in other active changelists (CL 1082 / default).
Delta measurement: next SchemaCompleteness scan (tracked in `Docs/TODO.md`
value_domain sweep item).

### 5d-1. Namespace-score renormalization (benchmark composition fix)

Per-namespace scores folded N/A param-gated dimensions in as 0.0, capping
all-param-less namespaces (`slate`, `reflect`) at 0.35 despite passing every
applicable dimension. `build_namespace_breakdown` now renormalizes weights
over applicable dimensions and reports `param_gated_applicable`; the top-level
aggregate is unchanged. Offline tests: 13/13
(`test_schema_completeness_value_domain.py`).

### 5d. ActionGuidance / SourceIndex discover pagination fix

Both runners called `monolith_discover(mode="actions")` without a limit
(default 50-row page): ActionGuidance's error-recovery scorer could not see
actions past page 1 for namespaces like `ai` (182 actions), silently failing
recovery through no fault of the server; SourceIndex's generate top-up saw a
truncated action list. Both now use the shared
`benchmark_common.paginate_discover_action_names` pager (5 offline tests in
`Scripts/tests/test_benchmark_common_pagination.py`).

### 5e. Hosted static CI benchmark inventory closure

The checkout contains seven benchmark manifests, but
`.github/monolith-static-ci.json` previously defined only six: AICapability was
not subject to manifest/task/default-path validation. The contract-test list
also ran only four of ten lightweight Python benchmark tests, omitting
AICapability, OfflineParity, the shared discovery pager, the ActionGuidance
routing-weight tests, and both SchemaCompleteness suites.

Repairs: AICapability is now a first-class `benchmark_definitions` entry; all
eleven lightweight Python benchmark tests plus the inventory guard
are in `benchmark_contract_tests`; and
`Scripts/tests/test_benchmark_ci_inventory.py` closes the recurrence path by
requiring exact one-to-one coverage of every `Benchmarks/*/manifest.json` and
every `Scripts/test_*.py` / `Scripts/tests/test_*.py` file. The benchmark index
now lists AICapability as the seventh suite. Offline verification:
`test_benchmark_ci_inventory.py` 3/3 passed. The final twelve-script configured
contract run is recorded in the verification summary after all runner changes.

### 5f. SourceControl Perforce batch mapping

`source_control.list_opened(resolve_packages=true)` previously paid one
`p4 where` process launch per opened row, Windows quoting could split a depot
path containing a quote-adjacent backslash, and `list_opened` issued an
unbounded exact-count query before returning a bounded page. The SourceControl
module now owns a CRT-compatible Windows argv encoder and a shared bounded
`p4 -ztag where <encoded paths...>` batch primitive. It rejects control
characters before dispatch, accepts only decimal/default changelists, limits
both raw and unique path counts to 2,000, and caps each batch at 128 paths,
24,000 actual encoded characters, and 16 commands. Deterministic deduplication,
row-local failure attribution, view-order preservation, and aggregate counters
remain shared by `map_depot_paths` and `list_opened`.

`list_opened` now performs only `p4 -ztag opened -m (limit + 1)` (maximum
backend window 2,001). It removes the one sentinel from returned rows and
reports whether the bounded observation is exact or a lower bound instead of
claiming an unbounded total.

Verification:

- `Monolith.SourceControl.P4WhereBatch.*`: 6/6 automation tests passed
  (`WindowsCommandLine`, `OpenedBounds`, `Scale`, `PartialFailure`, `ViewOrder`,
  `RowLocalStderr`; run `automation-20260711T092100Z-F555C76F`).
- `Monolith.ParamValidation.MonolithSourceControl.*`: 2/2 automation tests
  passed (run `automation-20260711T092105Z-A5010264`). The schema and handler
  guards reject zero/out-of-range/fractional/string-coerced limits,
  numeric/non-decimal/control changelists, 2,001 duplicate raw paths, and path
  controls.
- A live malicious mapping input, `//bad/a\" //speed/Speed.uproject`, remained
  one argv element and one failed mapping row. The response preserved the full
  original input/error and reported raw/requested/unique/command counts of one;
  no injected second mapping was executed.
- Live `source_control.list_opened(changelist="1093", resolve_packages=true,
  limit=1)` returned and resolved one row with one `p4 where` command after
  observing two backend records: `backend_record_limit=2`,
  `sentinel_record_count=1`, `count_semantics="lower_bound"`, `has_more=true`,
  and `truncated=true`.
- Live `source_control.list_opened(changelist="1093-other")` failed with
  `-32602` before Perforce execution and required decimal digits or `default`.
- The resolver-driven primary `SpeedEditor Win64 Development` UBT completed
  successfully after compiling and linking the updated SourceControl module and
  automation tests.

### 5g. Full-catalog ActionGuidance generation

ActionGuidance generation now enumerates all paginated live actions and builds
a compact, deterministic corpus instead of depending on a hand-maintained
subset. At catalog fingerprint `sha256:ef52979d7e00a249` it covered 61
namespaces / 1840 actions with 289 weighted tasks, including explicit
SourceControl read-only policy cases. The runner uses the shared transport
tracker and refuses to publish a normal summary for an invalid endpoint run.
The corpus contract and routing suites passed after regeneration. The final
`run-20260711-final` live execution completed all 289 tasks with zero transport
failures, stable start/end identity, and `effectiveness_score=0.873396` at catalog
`sha256:4af1375172e8818a`. Canonical tasks SHA-256 is
`DD25758F2C6A356D28AA0EDD3D3D8F0F502F7E951C2F6B3F137F07CCD0EC8190`.

### 5h. ProjectIndex submitted-fixture contract

The first project-local fixture refresh still admitted transient
`/Game/Benchmarks/AI` assets because exact ProjectIndex identity search proves
index consistency, not clean-checkout availability. Five known answers depended
on default/CL 1095 adds or an AICapability-created scratch asset. The corrected
schema-v2 fixture contract rejects mutable benchmark roots and requires every
candidate to pass all of the following before it can be written:

1. `project.get_saved_asset_state` reports an on-disk package with non-zero
   file size.
2. `source_control.get_status` reports an available provider and one known,
   current, source-controlled state that is not added, deleted, ignored, or
   conflicted.
3. The benchmark-shaped content-inclusive `project.search` returns the exact
   asset identity path.

Generation is fail closed at both the CLI and `build_static_tasks` boundary;
missing/invalid schema-v2 fixtures no longer select a 2026-06 legacy snapshot
and do not overwrite tasks or manifest. Candidate pools are sorted and consumed
prefix-round-robin until the target is filled or every pool is exhausted.

Live validation against catalog `sha256:ef52979d7e00a249` completed in 228.8
seconds with 24 `project` schema actions and 30 submitted known answers. The
tracked fixture is byte-identical to the live output (SHA-256
`2DA88DAC548B2F94575F7D573303FEDA30E00F2973DA416856522487D0AEF46C`).
The regenerated corpus has 314 sequential tasks, 30 known-answer tasks, zero
mutable-root rows, and deterministic task SHA-256
`901F10BD119A9EE6B5E6919F49DC169C686123C6EB657C01EFE3A21BACB45C97`.
The offline regression suite and an independent generate hash check passed.
ProjectIndex's 314-task runner was also moved from a completed-run-only 5%
check to the shared in-run tracker. Status and task protocol failures now abort
without a normal summary; three consecutive failures, the 20-sample 5% gate,
and short-run finalize share the same artifact contract as SchemaCompleteness.
The ProjectIndex runner suite is 23/23 passing. The final
`run-20260711-final` execution completed all 314 tasks with zero errors and zero
transport failures, stable start/end identity, and `project_index_score=1.0`;
every dimension is 1.0 except the intentionally inverted `stale_rate=0.0`.

### 5i. Source DB rebuild, health-directed CRG repair, and final SourceIndex run

The full resolver-selected UE 5.8 `MonolithReindex` commandlet completed 1,376
engine modules plus 19 project modules in 27 minutes. Although the commandlet
reported intermittent module-insert `disk I/O error` warnings from concurrent
DB access, the required post-run deep health check—not the commandlet exit code—
was authoritative: schema/triggers, orphan checks, and symbol/FTS parity were
clean at 1,325,706 symbols and 2,301,768 source-FTS rows. Health then identified
the CRG projection as stale. The explicit `source repair_crg_cache --scope=all
--execute=true` maintenance path rebuilt 1,325,706 nodes, 1,325,706 metrics,
90,832 valid edges, and 133,186 override edges. A second deep health returned
`status=ok`, no warnings, and `maintenance_required=false`.

The final post-build/post-AICap deep health remained authoritative and clean at
1,325,794 symbols, 2,301,954 source-FTS rows, 1,325,794 CRG nodes/metrics,
90,833 valid CRG edges, and 5,591,693 references. It again returned no warnings
and no maintenance requirement.

The canonical `run-20260711-final` SourceIndex run then completed 363/363 tasks
with zero transport failures and stable start/end identity. Score improved from
the first 6-term baseline 0.8750 to **0.910407** (`symbol_hit=0.948454`,
`field_completeness=0.773585`, `schema_adherence=1.0`, `stale=0.0`,
`ergonomics=1.0`, `negative_recovery=0.807692`). Task SHA-256 is
`5D4EA0D9E72C05154463F65C3CD1C55ED04C8B45F92C65452C188EF46782E540`.

### 5j. AI scratch lifecycle, verified deletion, and unrelated dirty-asset GC

The first hardened duplicate run exposed that `delete_blackboard`,
`delete_behavior_tree`, and `delete_eqs_query` trusted
`ObjectTools::ForceDeleteObjects`' object count even when a `.uasset` and
Perforce add remained. All three now delegate to the single
`MonolithAsset.delete_assets(force=true)` lifecycle and return its
package/registry/file/source-control verification. Behavior Tree node-creation
actions also fail and destroy the new graph node when schema edge creation is
rejected instead of returning an orphan-node success.

The runner now gives duplicate and compile-gate rows isolated scratch packages,
strict reset/create semantics, cleanup, and exact public-read absence checks.
The BT template edit row separately owns both its generated BT and derived
Blackboard, deletes both after the linkage readback, and requires two exact-path
`not found` reads. Corpus integrity rejects a create-based edit row without that
cleanup contract. The offline AICapability suite is 39/39 passing and the tracked
manifest enumerates all seven task-owned scratch paths.

A concurrent four-job AssetEditing run then provided a live adversarial case:
between `add_bb_key` success and `get_blackboard`, another request entered
`asset.delete_assets`, whose production path used `CollectGarbage(RF_NoFlags)`.
That policy can collect an unrelated unsaved `RF_Standalone` asset; the readback
correctly failed instead of hiding the lost edit. The delete lifecycle now uses
Unreal's editor-standard `GARBAGE_COLLECTION_KEEPFLAGS` for every target-eviction
collection, and `MonolithAsset.DeleteAssets.PreservesUnrelatedStandaloneDirtyAsset`
pins the independent dirty-asset survival contract.

After a resolver-selected full `SpeedEditor Win64 Development` build and fresh
headless boot, `editor.run_automation_tests(prefix="MonolithAsset.DeleteAssets.")`
completed **9/9 passed**, including the new unrelated-dirty-asset regression.
The isolated canonical AICapability run then completed **74/74** with
`ai_capability_score=1.0`, all seven dimensions at 1.0, zero transport failures,
stable start/end identity, task SHA-256
`AE898D3FDFADAC2FBB07371C3A409EADCCE8FBA855FBD34187B93A6874A446BB`,
manifest SHA-256
`4C0610BE988BF8984852504BBB0595C5D60EFC33909E3428088C03B731AD5CF6`,
and input fingerprint
`b3bfb12bcbdf3f86611f7a4a431d64af556ec356778c656a056c126da0cdc24b`.
All seven owned scratch packages were absent from disk and Perforce afterward;
the default changelist contained no AICapability asset.

### 5k. Final OfflineParity after source-index repair

The explicit `run-20260711-final` run completed all 317 rows with 311/311
comparable actions matching, zero diffs, zero real errors, five expected-error
matches, and six decision-ID-dependent skips. Both implementations reported
parity revision `2026-05-29.1`; the composite remained **1.0** with input
fingerprint
`92f6717c9d99e528792dbc2215e9cf822b2358b40a386b5336f6c8b67708ae3e`.
The output is
`Saved/Monolith/Benchmarks/OfflineParity/run-20260711-final`.

### 5l. Headless Slate modal guard and accurate modal identity

The first post-build editor boot answered `/health` but temporarily rejected an
editor-backed action. The existing `MODAL_OPEN` diagnostic claimed
`This asset editor has no docked tabs`, but engine-source inspection proved that
text came from the active background `SDockingArea`: the callback ignored
`FModalWindowContext::WindowIdentifier` before Slate pushed the actual modal.
The real window on the verified boot was the allowed slow-task progress window
`Running Python start-up scripts...`.

`FMonolithEditorModule` now scopes `GIsRunningUnattendedScript=true` whenever
`FApp::CanEverRender()==false`. This is the exact UE 5.8 flag used by
`FSlateApplication::AddModalWindow` to cancel non-slow-task modals; `-Unattended`
alone does not set it. The callback also harvests the context window directly.
The resolver-selected full build passed, a normal `recover_mcp.ps1` boot logged
`HeadlessUnattendedScriptGuard: CanEverRender=false`, the slow-task window was
reported accurately, the endpoint remained healthy, and the subsequent 9-test
automation run plus 74-task AICap run completed through the public MCP actions.

## 6. Known blockers / follow-ups

- Live Coding change detection on the headless `-NullRHI` editor did not observe source edits
  (tmp+rename and in-place rewrites both undetected; `LogLiveCoding: no code changes detected`).
  Monolith `.cpp` changes on this workstation propagate via the watchdog's UBT rebuild on the next
  editor restart. Follow-up tracked in `Docs/TODO.md` if it persists on an interactive editor.
- The final hosted-static-CI rerun no longer reports an OfflineParity blocker.
  It still reports 151 `repo-hygiene` blockers because the hosted Git policy
  rejects every tracked/generated path under `Plugins/Monolith/Binaries`, while
  the Speed Perforce contract requires touched build artifacts to remain in the
  task changelist. Resolving that repository-distribution policy conflict is a
  separate owner decision; this change does not silently exempt or delete the
  tracked binaries. The same run reports 267 advisories (existing CRLF inventory,
  missing external `.claude/agents`, the broad secret-pattern heuristic, and a
  non-gating `unreal-console` skill-drift result). Direct live discovery confirmed
  that `console` is absent because `UMonolithSettings::bEnableConsole` defaults to
  false and Speed does not override it; enabling that privileged command surface
  is a separate configuration/security decision, not an implicit benchmark fix.
