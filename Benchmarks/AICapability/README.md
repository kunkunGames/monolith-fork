# AICapability Benchmark

Measures the Monolith MCP server's capability to support engine-native **AI authoring** over the
`ai` namespace. The canonical corpus is bound to the complete live 182-action catalog: 44 curated
read/edit schema probes plus 138 generated discovery-only probes give **182/182 schema coverage**.
The weighted score still targets the highest-value subsystems and retains its adversarial categories
so complete schema coverage cannot turn the benchmark into a green-on-broken grader.

- **Runner:** `Scripts/ai_capability_benchmark.py`
- **Offline self-test:** `Scripts/test_ai_capability_benchmark.py`
- **MCP tool:** `ai_query` (all `ai.*` actions dispatch through this single tool)
- **Primary score:** `ai_capability_score` (weighted sum of the seven established category rates below; weights sum to 1.0)
- **Catalog diagnostic:** `catalog_schema_rate` (unweighted success rate over all 182 live action schemas)
- **Subsystems tested:** Behavior Tree, Blackboard, EQS (granular edits); StateTree (schema only — see note)

## Why this benchmark exists (and why it leads with adversarial categories)

The benchmark-ROI report (`Saved/Monolith/LogAnalysis/_benchmark_roi_report.md`, PART B) ranked the
`ai` namespace as the one greenfield surface worth a new benchmark — but with an explicit caveat:
there is **no real-usage failure corpus** for `ai` (66 logged calls ever, all synthetic), so a
clean-fixture happy-path suite would "inherit the exact AssetEditing blind spot" (a perfect
score over actions that fail in production). The verdict: *build only if the adversarial categories
ship on day one.*

So this runner concentrates weight on falsifiable, read-back-verified work and keeps clean-fixture
happy-path reads to a minimum:

| Category | Weight | What it proves | Adversarial? |
| --- | ---: | --- | :---: |
| `edit_execute` | 0.34 | A real `ai` edit (add BB key / add BT node / set BT blackboard / add EQS generator) is **read back** from `get_blackboard`/`get_behavior_tree`/`get_bt_graph`/`get_eqs_query` — a silent no-op cannot pass | yes |
| `error_path` | 0.16 | An invalid input returns a structured `isError` that **names the offending identifier** (a reject-everything canned message fails) | yes |
| `compile_gate` | 0.12 | An empty Behavior Tree must make **`validate_behavior_tree` report `valid==false`** (with an error-severity issue), and a clean Behavior Tree must make it report **`valid==true`** — the falsifiable validate gate (built only from real BT validate actions; StateTree lint is unavailable on `WITH_STATETREE=0`) | yes |
| `duplicate_reject` | 0.10 | A **second** identical `create_*` / `add_bb_key` must be refused with a duplicate-specific `isError` (no silent suffix/no-op) | yes |
| `edit_schema` | 0.10 | `monolith_discover` resolves the schema for 28 edit actions (strict: `isError` fails) | tripwire |
| `discovery` | 0.10 | `list_*` / `search_ai_assets` find the **seeded fixtures** (require ≥1 result) | tripwire |
| `read_schema` | 0.08 | `monolith_discover` resolves the schema for 16 read actions (lenient) | tripwire |
| `catalog_schema` | unweighted | `monolith_discover` resolves the remaining 138 live action schemas; combined with `read_schema` and `edit_schema`, this proves 182/182 catalog coverage | diagnostic |

The four adversarial categories carry **0.72** of the score. A server that schema-resolves every
`ai` action but cannot actually edit an asset, name a bad identifier, reject a duplicate, or
validate a tree correctly scores **at most 0.28**.

`catalog_schema` is intentionally excluded from `WEIGHTS`: adding full-catalog discovery must not
change the established `read_schema_rate` / `edit_schema_rate` denominators or invalidate historical
weighted comparisons. Its failures are still visible per task and in `catalog_schema_rate`.

Every create-based row owns a dedicated scratch lifecycle. The template scaffold, duplicate, and
compile-gate rows reset only benchmark-owned paths, reject every reset failure except an explicit
already-absent response, and cannot pass until follow-up public reads prove that cleanup removed each
key or made every generated package unresolvable. Persistent `*_BenchDup` or compile-gate assets from
another changelist are never reused.

> **StateTree coverage note — compiled out (`WITH_STATETREE=0`).** On this build StateTree is
> compiled out, so `create_st_from_template` and `lint_state_tree` are **runtime stubs** that return
> `isError` "StateTree module not available (WITH_STATETREE=0)". No StateTree EXECUTE or lint task can
> pass here, so StateTree is covered **only by schema-presence tasks**. The curated prefix retains
> one `edit_schema` (`create_st_from_template`) and one `read_schema` (`lint_state_tree`); runtime/
> crowd StateTree-related actions from the live catalog are generated only as read-only
> `catalog_schema` rows. The `ai` namespace also exposes no granular
> StateTree authoring (`create_state_tree` / `add_st_state` / `compile_state_tree` / `*_st_state`
> etc. do **not** exist). The falsifiable gate and the formerly-StateTree error_path are built from
> Behavior Tree `validate_behavior_tree` instead, which DOES execute. Behavior Tree / Blackboard /
> EQS keep full granular-edit coverage.

## Layout

| File | Purpose |
| --- | --- |
| `manifest.json` | Generated: task counts, weights, score formula, fixture paths, live catalog version/action-set hash, 182/182 coverage proof, run-integrity gates |
| `tasks.jsonl` | Generated: 74 stable curated tasks followed by 138 sorted `catalog_schema` rows (212 total) |
| `README.md` | This file |
| `METRICS.md` | Per-category scoring rules + the verified `ai` action/param/error evidence (file:line) |
| `RESULTS.md` | Run history + the exact live run commands (run requires the shared editor) |
| `test_fixtures.md` | Standing fixtures plus task-owned reset/create/assert/cleanup/readback contracts |

## Usage

```powershell
# 1. (offline) self-test scoring and live-catalog generation contracts
python Scripts/test_ai_capability_benchmark.py

# 2. (live editor) regenerate tasks + manifest against one stable live catalog
python Scripts/ai_capability_benchmark.py generate --mcp-url http://localhost:9316/mcp

# 3. (live editor) seed only the three standing AI fixtures at /Game/Benchmarks/AI/
python Scripts/ai_capability_benchmark.py setup_fixtures --mcp-url http://localhost:9316/mcp

# 4. (live editor) preflight, then score a run
python Scripts/ai_capability_benchmark.py preflight   --mcp-url http://localhost:9316/mcp
python Scripts/ai_capability_benchmark.py run --mcp-url http://localhost:9316/mcp `
    --output-dir Saved/Monolith/Benchmarks/AICapability/<label> --label <label>

# 5. (optional) compare two runs
python Scripts/ai_capability_benchmark.py compare `
    --baseline <a>/summary.json --current <b>/summary.json --output-dir <out>
```

`generate` now requires the live editor just like `run`: it validates `monolith_status`, the summary
AI count, every paginated action name, and a second status under one `catalog_version` before writing
either canonical file. Missing curated actions, duplicate names, page/count mismatch, or catalog
drift leaves the existing files untouched. `setup_fixtures`, `preflight`, `run`, and `compare` retain
the established command surface. **`run` requires the single shared editor** (`ai` is an
editor-only namespace; there is no offline `monolith_query.exe` equivalent), so a scored run is left
to a coordinated session — see `RESULTS.md`.

### Run-integrity contract

`run` owns a mandatory strict `monolith_status` preflight. `--skip-preflight` skips only the
fixture-readiness probes; it can never bypass status validation. Invalid JSON, a non-object JSON
envelope, a top-level JSON-RPC error, a missing MCP result, `isError` status, or a status payload that
does not declare `server_running=true` aborts before the first scored task.

A canonical run also requires the non-empty `manifest.json` `catalog_version` to exactly match the
live status `catalog_version`. A missing or stale manifest identity aborts before fixture probes or
scored task calls and writes only invalid-run artifacts; regenerate the canonical corpus against the
stable live catalog instead of editing the version by hand. Explicit subsets remain non-comparable
diagnostics and do not claim this canonical identity binding.

Every MCP call made by a task is attributed to its row, including setup, edit, duplicate first/second
calls, readback, compile/validate, cleanup, and cleanup-readback calls. The shared transport gate aborts after three
consecutive transport-failed tasks or when the failure fraction exceeds 5% after 20 attempted tasks.
Exactly 5% is allowed. A completed corpus shorter than 20 tasks applies the same fraction budget in
`finalize()`.

| Output | Contract |
| --- | --- |
| `summary.json` | Valid completed run only; contains `run_valid=true`, `metrics_valid=true`, completion status, and transport counters |
| `per_task.json` | Valid completed run only |
| `per_task.jsonl` | Incremental diagnostic rows, including a triggering transport/protocol/runner-error row |
| `partial_summary.json` | In-progress or invalid-run diagnostics; removed after a valid completion |
| `run_failure.json` | Machine-readable invalid-run reason; mutually exclusive with `summary.json` |

Known outputs are removed before input/status validation, so a failed rerun cannot expose a stale
success. Task protocol failures and runner exceptions invalidate the run, write diagnostic artifacts,
and make the CLI exit non-zero. A valid semantic `result.isError=true` remains a scoreable domain
response for the negative and duplicate-rejection tasks; it is not a transport/protocol failure.

## Action-name provenance

Every canonical generation enumerates the live `ai` namespace through paginated
`monolith_discover(mode="actions")`, cross-checks the 182-row result against the summary count, and
records both `catalog_version` and a normalized action-set SHA-256 in `manifest.json`. Existing
curated schema actions must all remain live; every otherwise-uncovered live action receives exactly
one sorted, read-only `catalog_schema` task. No action name is inferred from the source-scanned
offline catalog, which can contain compile-gated actions absent from the current editor profile.

The fixed execute/error/duplicate/compile categories still reference no granular StateTree authoring
action. `create_st_from_template` and `lint_state_tree` remain schema-only on this
`WITH_STATETREE=0` build, while generated catalog coverage is always `monolith_discover` and can
never promote a newly visible action into execution. Verified handler return/error shapes used by
the adversarial scorers remain anchored with file:line evidence in `METRICS.md`.
