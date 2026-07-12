# ProjectIndex Benchmark Results

## 2026-07-11 schema-v2 canonical fixture refresh

The project-local fixture contract was hardened after the first refresh was
shown to depend on five mutable `/Game/Benchmarks/AI` assets from another
benchmark/default changelist. The canonical live refresh now admits only
on-disk, non-empty, current, submitted source-controlled packages outside
mutable benchmark roots, and then re-verifies the exact asset identity through
the same content-inclusive `project.search` shape used by the benchmark.

| Contract | Result |
| --- | --- |
| Live catalog | `sha256:ef52979d7e00a249`, 24 `project` schema actions |
| Stable known answers | 30 |
| Mutable `/Game/Benchmarks` known answers | 0 |
| Regenerated tasks | 314 total, 30 known-answer, sequential IDs |
| Fixture SHA-256 | `2DA88DAC548B2F94575F7D573303FEDA30E00F2973DA416856522487D0AEF46C` |
| Task SHA-256 | `901F10BD119A9EE6B5E6919F49DC169C686123C6EB657C01EFE3A21BACB45C97` |

The tracked fixture is byte-identical to the 228.8-second live refresh output.
A first strict refresh found only 26 stable rows and failed without creating an
output or changing the tracked fixture; balanced selection was then corrected
to continue rounds until the requested count is reached or all prefix pools are
exhausted. `build_static_tasks` itself now requires a validated schema-v2
fixture, so direct Python callers cannot silently fall back to the removed
2026-06 cross-project schema/known-answer constants. Offline regressions,
missing/invalid fixture no-overwrite checks, provider-unavailable no-overwrite,
deterministic reverse-order selection, and independent task regeneration all
passed.

The live runner now uses the same publishability contract as
SchemaCompleteness and ActionGuidance. It validates the status envelope before
task zero, rejects task-level parse/protocol errors, aborts after three
consecutive transport failures or an over-5% transport fraction after 20
samples, and checks shorter runs at finalize. Invalid runs write
`run_failure.json` plus diagnostic `partial_summary.json`; they do not write
normal `summary.json` or final `per_task.json`. ProjectIndex runner regressions
are 22/22 passing, including stale-output cleanup and triggering-row
preservation. A new valid live score is recorded below only after the hardened
runner completes the 314-task corpus.

## 2026-07-11 baseline-20260711 — first live run; exposed testset portability rot

First-ever live run (Speed CL 1093 suite-wide refresh). Score **0.7149** — but
the misses are benchmark rot, not index regressions:

| Metric | Value | Diagnosis |
| --- | ---: | --- |
| `project_index_score` | 0.7149 | depressed by the two rot rows below |
| `search_hit_rate` | 1.0000 | healthy |
| `known_answer_hit_rate` | 0.0000 | all 30 fixtures reference GO-project assets (`DA_Monster_*` …) that do not exist in Speed; additionally the scorer matched only `match_object_path`, which in the live contract is the match-site field path (e.g. `MapID`), not the asset package path |
| `schema_adherence_rate` | 0.4750 | 21/40 schema tasks reference actions absent from the live `project` namespace (20260618 catalog snapshot rot) |
| `field_completeness_rate` | 1.0000 | healthy |
| `error_free_rate` | 0.9364 | the 21 unknown-action rows |

Repairs shipped in the same CL: `known_answer_hit` now matches asset identity
via `asset_path` (legacy `match_object_path` identity rows still accepted), and
a new `refresh_live_fixtures` subcommand derives the schema-action list
(paginated live discover) plus verified known-answer pairs from the CURRENT
project into `Benchmarks/ProjectIndex/live_fixtures.json`, which `generate`
consumes in place of the 20260618 cross-project snapshots (Speed derivation:
24 live schema actions + 30 verified known answers, corpus 330→314 tasks).

Reruns on the refreshed corpus (`baseline-20260711b/c/e`) each spanned an
endpoint churn window and were correctly failed by the new
`--max-transport-failed-fraction` gate (37%/8.6%/37% transport failures)
instead of recording poisoned numbers. The reachable subsets validate the
repairs decisively — final attempt (`baseline-20260711e`, 117/314 transport
failures): **`known_answer_hit_rate` 1.0** (every reachable Speed-derived
fixture hit; was 0.0 on the rotten corpus), **`schema_adherence_rate` 1.0**
(was 0.475), `field_completeness_rate` 1.0; the depressed
`search_hit`/`stale`/`error_free` dimensions on that run are all
transport-outage rows. Record the full clean baseline by re-running on a
stable endpoint:

```powershell
python Plugins\Monolith\Scripts\project_index_benchmark.py run `
  --mcp-url http://localhost:9316/mcp `
  --tasks Plugins\Monolith\Benchmarks\ProjectIndex\tasks.jsonl `
  --label <label> --output-dir Saved\Monolith\Benchmarks\ProjectIndex\<label>
```

Output: `Saved\Monolith\Benchmarks\ProjectIndex\baseline-20260711`.

## How to populate

```powershell
python Plugins\Monolith\Scripts\project_index_benchmark.py run `
  --mcp-url http://localhost:9316/mcp `
  --tasks Plugins\Monolith\Benchmarks\ProjectIndex\tasks.jsonl `
  --label initial `
  --output-dir Saved\Monolith\Benchmarks\ProjectIndex\initial `
  --request-timeout-s 12
```

Then copy the key metrics here from `Saved\Monolith\Benchmarks\ProjectIndex\initial\summary.json`.

## Results table scaffold

| Run label | Date | project_index_score | search_hit_rate | known_answer_hit_rate | field_completeness_rate | schema_adherence_rate | stale_rate | error_free_rate |
| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| _(no complete runs yet)_ | -- | -- | -- | -- | -- | -- | -- | -- |

## Offline scoring self-test (no live editor)

`python Plugins\Monolith\Scripts\test_project_index_benchmark.py` validates the new
scoring branches without an MCP endpoint. Latest local result:

| Scenario | project_index_score | all_empty | Note |
| --- | ---: | --- | --- |
| Empty / broken index (all searches return 0 results) | 0.30 (capped) | true | Was `1.000` before the empty-results loophole fix |
| Healthy index (known-answer hits + field-complete rows) | 1.00 | false | Ground-truth recall + completeness both satisfied |
