# Monolith Offline Parity Benchmark

This benchmark elevates `Scripts/verify_offline_parity.py` from a CI pass/fail
gate into a scored benchmark that tracks exe-vs-py parity quality over time.

## Relationship to verify_offline_parity.py

| | verify_offline_parity.py | offline_parity_benchmark.py |
| --- | --- | --- |
| Role | CI hard-gate | Trend tracking |
| Output | Exit code 0/1 | `summary.json`, `per_action.jsonl`, `comparison.md` |
| Pass criterion | Zero diffs, zero errors, version match | Scored `offline_parity_score` (0.0–1.0) |
| Used in | CI pipeline | Regression detection across builds |

The verify script is the authoritative CI signal. This benchmark adds a numeric
score so regressions can be detected by magnitude before they become hard
failures: a score drop shows *where* parity is degrading even while individual
actions might still pass.

Current coverage:

- `verify_offline_parity.py`: 137 hard-gate actions. CI still requires zero
  diffs, zero real errors, and matching `parity_spec_rev`.
- `offline_parity_benchmark.py`: 317 scored actions: 96 `cppreflect`, 46
  `network`, 50 `decision`, 62 `risk`, and 63 `source` cases. The benchmark
  includes 100 benchmark-only cases beyond the prior 217-action table, plus
  5 expected-error negative source cases that are tracked separately from real
  tool failures. The table is externalized to `actions.jsonl` (one action per
  line) and validated against `manifest.json` `action_count` by hosted static CI.
- Of the 63 `source` rows, the 58 non-probe symbol/signature/usage lookups are
  tagged `offline_unsupported`: their offline surface (the EngineSource DB
  symbol/signature data) may legitimately be absent, so if BOTH offline tools
  agree on the failure the row is a parity `MATCH` (`expected_offline`) rather
  than a `real` `ERROR` that would mask the real exe-vs-py signal. Only a genuine
  disagreement (exactly one tool fails) counts against the score
  (`offline_parity_break`). See `METRICS.md` for the full bucket table.
- Before any scored action, the benchmark runs a read-only Query preflight. If
  Query emits its exact fail-closed active rollback-journal error, the **entire
  run** is invalidated as `environment_blocked`: every row is skipped,
  `comparable_actions` is zero, and `offline_parity_score` is forced to `0.0`.
  The Python reader is not opened in this state. A journal refusal that appears
  after a successful preflight is a real per-action `ERROR`, never a `SKIP`, so
  a one-sided failure cannot shrink the denominator or inflate the score.
- Input fingerprinting follows the executed dependency boundary. All declared
  `cppreflect`, `network`, `decision`, `risk`, and `source` rows read
  `Saved/EngineSource.db`, so that is the only database in the run identity.
  `ProjectIndex.db` and legacy analysis DBs are excluded because no
  OfflineParity row reads them; unrelated live index updates must not stale an
  otherwise comparable parity result.
  The same identity includes `actions.jsonl`, `manifest.json`, the benchmark
  runner, the native Query executable, and the Python fallback, so changes to
  either the corpus or scoring logic cannot reuse an older baseline.

### Scope: what this benchmark cannot see

This benchmark only compares the two OFFLINE tools (`monolith_query.exe` vs
`monolith_offline.py`). Editor-only surfaces such as the `blueprint` write
namespace are never exe-vs-py comparable here — neither offline tool implements
them — so a green score here does not imply those live surfaces are healthy.
Live-capability coverage for editor surfaces lives in the AssetEditing,
ProjectIndex, and AI capability benchmarks, not here.

For CI thresholding, keep the hard-gate as the binary pass/fail check. Use the
benchmark as a trend gate: healthy runs should stay at or above `0.95`, with
`real_error_count == 0` and `expected_error_problem_count == 0`.

## Files

| File | Purpose |
| --- | --- |
| `README.md` | This file. Overview, usage, and relationship to the CI gate. |
| `METRICS.md` | Metric definitions, score formula, action buckets, and interpretation guidance. |
| `RESULTS.md` | Latest checked-in benchmark result summary and evidence paths. |
| `manifest.json` | Static seed manifest: `action_count`, `action_file`, category/bucket counts, scoring summary. CI validates `action_count` against the `actions.jsonl` line count. |
| `actions.jsonl` | The externalized parity action table (one action per line). Reviewable as data; CI line-count-validated like the other benchmarks. |
| `accepted/<snapshot>/` | Tracked minimum accepted-evidence bundle: aggregate summary, raw per-action rows, and a pinned bundle manifest. The multi-gigabyte database is attested, not copied here. |
| `Scripts/offline_parity_benchmark.py` | Standalone benchmark runner (run / compare / report). Loads the table from `actions.jsonl`. |
| `Scripts/test_offline_parity_benchmark.py` | Offline unit tests for the loader, `offline_unsupported` scoring, run-level active-writer blocking, and one-sided journal failures (no live editor). |

## Accepted completion evidence

An OfflineParity run receives completion credit only after its minimum complete
bundle is promoted to `Benchmarks/OfflineParity/accepted/<snapshot>/`:

- `summary.json` retains aggregate metrics, run validity, exact benchmark input
  signatures, and the input fingerprint.
- `per_action.jsonl` retains every raw result row so the inventory can rederive
  namespace pass/fail/expected-skip counts instead of trusting the aggregate.
- `bundle_manifest.json` is SHA-pinned by `inventory_status.json` and in turn
  pins both evidence files, raw row count, input fingerprint, and the complete
  `Saved/EngineSource.db` attestation.

The database itself is deliberately not copied into the source-controlled
bundle. `benchmark_inventory.py --portable-check` accepts the recorded database
attestation only when `Saved/EngineSource.db` is absent, as in a clean hosted-CI
checkout. When the database is present, portable mode still verifies its exact
size, mtime, and content SHA-256 and rejects drift. The full local
`benchmark_inventory.py --check` always requires the database and every pending
`Saved/...` diagnostic to exist. Neither mode grants completion credit from a
missing pending diagnostic.

## Run

```powershell
python Plugins\Monolith\Scripts\offline_parity_benchmark.py run `
  --label current `
  --output-dir Saved\Monolith\Benchmarks\OfflineParity\current
```

With lenient cursor comparison (mirrors `--ignore-cursor-bytes` from the CI gate):

```powershell
python Plugins\Monolith\Scripts\offline_parity_benchmark.py run `
  --label current `
  --output-dir Saved\Monolith\Benchmarks\OfflineParity\current `
  --ignore-cursor-bytes
```

Override tool paths if needed:

```powershell
python Plugins\Monolith\Scripts\offline_parity_benchmark.py run `
  --label v1.2.3 `
  --exe-path Binaries\monolith_query.exe `
  --py-path Scripts\monolith_offline.py `
  --output-dir Saved\Monolith\Benchmarks\OfflineParity\v1.2.3
```

Each run writes:
- `summary.json` -- aggregate metrics including `offline_parity_score`
- `per_action.jsonl` -- one line per action with label, status, diff_count, warning_count, error, expected_error, error_kind, exe_exit_code, py_exit_code
- `partial_summary.json` -- updated every 5 actions for progress visibility

## Compare

```powershell
python Plugins\Monolith\Scripts\offline_parity_benchmark.py compare `
  --baseline Saved\Monolith\Benchmarks\OfflineParity\baseline\summary.json `
  --current  Saved\Monolith\Benchmarks\OfflineParity\current\summary.json `
  --output-dir Saved\Monolith\Benchmarks\OfflineParity\comparison
```

Writes `comparison.json` and `comparison.md` (Markdown delta table).

## Report

```powershell
python Plugins\Monolith\Scripts\offline_parity_benchmark.py report `
  --summary Saved\Monolith\Benchmarks\OfflineParity\current\summary.json
```

Prints a human-readable summary of a single run to stdout.

## Missing exe handling

If `Binaries/monolith_query.exe` does not exist, all actions are reported as
SKIP with a `tool not found: exe` reason. No actions are comparable, so the
primary `offline_parity_score` is forced to `0.0` instead of awarding partial
credit from absent diff/error rates. This allows the benchmark to run in
environments where only the Python reference tool is present, while making the
missing exe visibly absent from the score trend.

An active editor-owned rollback journal is handled more narrowly than a
missing executable but at run scope, not row scope. The benchmark invokes Query
with global `--readonly` and uses one DB-backed preflight before chain discovery.
Only Query's exact fail-closed journal message invalidates the run; all rows are
then `SKIP(environment_blocked)` and the score is `0.0`. The benchmark does not
launch the Python reader while blocked, avoiding a second access to an unstable
corpus. Broad strings such as `database is locked` do not receive this exemption.

Once preflight succeeds, every action remains scoreable. If the exact journal
marker appears later because the environment changed mid-run, that row is a
real `ERROR` even when the Python side succeeds. This deliberately makes the
race visible instead of removing the failed row from `comparable_actions`.
