# Offline Parity Benchmark Metrics

This benchmark scores the exe-vs-py parity quality of the two Monolith offline
tools: `Binaries/monolith_query.exe` (C++) and `Scripts/monolith_offline.py`
(Python reference implementation).

## Definition source (actions.jsonl + manifest.json)

The parity action table is externalized to
`Benchmarks/OfflineParity/actions.jsonl` (one JSON object per line) and described
by `Benchmarks/OfflineParity/manifest.json`. This mirrors the other five
benchmarks so the same hosted-static-CI line-count validation
(`.github/monolith-static-ci.json` -> `benchmark_definitions`) covers it:
`manifest.json` `action_count` must equal the non-empty line count of
`actions.jsonl`. `Scripts/offline_parity_benchmark.py` loads the table at runtime
via `load_action_specs()` / `build_actions()`; the `DEFAULT_ACTIONS` and
`DEFAULT_MANIFEST` module paths are CI-validated against the config.

Each `actions.jsonl` row has `label`, `namespace`, `action`, `args`, and optional
`compare` (`text`), `expected_error`, `offline_unsupported`, and `requires`
(`decision_id`). The `args` list may contain placeholder tokens — `{{uclass}}`,
`{{decision_id}}`, `{{risk_path}}` — substituted from chain discovery at load
time; a row whose `requires` chain input is missing is reported as `SKIP`.

## Primary Score

```text
offline_parity_score =
  0.50 * match_rate           (fraction of non-skipped actions with status=MATCH)
+ 0.20 * (1 - diff_rate)      (1 - fraction of non-skipped actions with status=DIFF)
+ 0.20 * (1 - error_rate)     (1 - fraction of non-skipped actions with status=ERROR)
+ 0.10 * version_parity_score (1.0 if exe parity_spec_rev == py parity_spec_rev, else 0.0)
```

Note: `match_rate + diff_rate + error_rate = 1.0` for non-skipped actions.
If there are no comparable actions (`comparable_actions == 0`), the score is
forced to **0.0**. Missing tooling should not receive partial credit from the
absence of diffs or errors.

The formula therefore simplifies to:

```text
offline_parity_score = 0.70 * match_rate + 0.20 * (1 - error_rate) + 0.10 * version_parity_score
```

A perfect run (all actions match, both tools at the same parity revision) scores
**1.0**. A run where the exe is missing scores **0.0** (all actions skipped,
no comparable actions).

## Metrics

| Metric | Direction | Definition |
| --- | --- | --- |
| `offline_parity_score` | Higher is better | Composite parity quality score (0.0–1.0). Primary regression signal. |
| `match_rate` | Higher is better | Fraction of non-skipped actions where exe and py outputs deep-equal (status=MATCH). |
| `diff_rate` | Lower is better | Fraction of non-skipped actions where exe and py outputs diverge (status=DIFF). |
| `error_rate` | Lower is better | Fraction of non-skipped actions where either tool returned a non-zero exit code or unparseable output (status=ERROR). |
| `skip_rate` | Lower is better | Fraction of all actions that were skipped due to missing tool, missing corpus input, or a run-level active-writer environment block. |
| `expected_error_rate` | Informational | Fraction of comparable actions that intentionally exercise an expected negative path and matched as expected. These rows keep `status=MATCH` with `error_kind=expected` (bad-input probe) or `error_kind=expected_offline` (offline-unsupported surface where both tools agree on the failure). |
| `real_error_rate` | Lower is better | Fraction of comparable actions with unexpected tool/process/parse failures (`error_kind=real`). This is the first diagnostic to inspect when `error_rate` moves. |
| `version_parity_score` | Higher is better | 1.0 if `parity_spec_rev` matches between exe and py; 0.0 otherwise. |
| `total_actions` | Informational | Total number of actions in the benchmark table. Current table: 317 actions. |
| `comparable_actions` | Informational | Non-skipped actions actually compared (total - skip). |
| `action_count` | Higher is better | Alias for `comparable_actions`; tracks coverage — higher means more namespace/input combinations were exercised. |
| `expected_error_count` | Informational | Count of expected-match negative-path actions where both tools agreed on the failure (`error_kind` in `expected`, `expected_offline`). |
| `expected_error_problem_count` | Lower is better | Count of negative/offline-unsupported actions where the tools disagreed (`error_kind` in `expected_missing`, `expected_mismatch`, `offline_parity_break`): one tool succeeded, both tools succeeded, or only one tool failed. |
| `real_error_count` | Lower is better | Count of unexpected tool failures. CI trend gates should require this to be 0. |
| `environment_blocked_count` | Informational | Number of rows invalidated after the run-level preflight detected Query's exact fail-closed active rollback-journal error. A blocked run has `environment_blocked_count == total_actions`, zero comparable actions, and score 0.0. |
| `mean_diffs_per_diff_action` | Lower is better | Average number of diverging fields among actions with status=DIFF. |
| `category_breakdown` | Informational | Per-namespace breakdown of match/diff/error/skip counts, expected-error diagnostics, real-error diagnostics, `environment_blocked`, and `action_count` (non-skipped). Keys are namespace prefixes (`cppreflect`, `network`, `decision`, `risk`, `source`). |

## Action Statuses

| Status | Meaning |
| --- | --- |
| `MATCH` | Both tools returned exit 0 and their outputs deep-equal (within cursor/time tolerances). |
| `DIFF` | Both tools returned exit 0 but at least one field diverges. |
| `ERROR` | At least one tool returned non-zero exit code or unparseable output. |
| `SKIP` | Action was not comparable (missing tool executable, no valid corpus input for chained args, or the complete run was invalidated by the active-writer preflight). |

Expected-error cases are deliberate negative-path probes. If both tools exit
non-zero, the row is a parity match with `status=MATCH` and
`error_kind=expected`. If the negative path is not observed consistently, the
row becomes `status=DIFF` with `error_kind=expected_missing` or
`error_kind=expected_mismatch`. Unexpected non-zero exits on a plain row remain
`status=ERROR` with `error_kind=real`.

### Active-writer environment classification

`monolith_query.exe` intentionally refuses to recover or open a hot rollback
journal when an Unreal Editor may still own `EngineSource.db`. The benchmark
always invokes Query with global `--readonly`, then runs a DB-backed preflight
before chain discovery or scored actions. If preflight stderr contains the exact
fail-closed marker
`Rollback journal exists for database and could not be recovered safely:`, the
benchmark does not launch the Python reader and invalidates the **entire run**.
Every action becomes `SKIP(environment_blocked)`, `comparable_actions` becomes
zero, and the score is forced to 0.0. This preserves safety without selecting
only Query's failed rows out of the denominator.

This classification does not match broad strings such as `database is locked`
and does not turn generic process/tool failures into skips. After a successful
preflight, the marker has no row-level skip privilege: if it appears because the
environment changed mid-run, that action is `ERROR(real)` and stays comparable.
In particular, Query failure plus Python success is an error and cannot inflate
the score.

### Action buckets and offline-unsupported reclassification

Each row in `actions.jsonl` falls into one of three buckets (see `manifest.json`
`buckets`):

| Bucket | Flag in `actions.jsonl` | Both tools fail | Exactly one tool fails | Both succeed |
| --- | --- | --- | --- | --- |
| `normal` | (none) | `ERROR` / `real` | `ERROR` / `real` | compare outputs |
| `expected_error` | `expected_error:true` | `MATCH` / `expected` | `DIFF` / `expected_mismatch` | `DIFF` / `expected_missing` |
| `offline_unsupported` | `offline_unsupported:true` | `MATCH` / `expected_offline` | `DIFF` / `offline_parity_break` | compare outputs |

The `offline_unsupported` bucket exists because this benchmark measures parity
(exe-vs-py agreement), not live capability. When an offline surface is
legitimately absent, both offline tools fail the same way; that is still parity,
so the row is a `MATCH` (`expected_offline`) instead of a `real` `ERROR` that
masks the real signal. Only a genuine exe-vs-py disagreement (exactly one tool
fails) is a `DIFF` (`offline_parity_break`) that counts against the score. If the
surface is present on both tools the outputs are compared normally.

`expected` and `expected_offline` are the expected-match kinds (they keep
`status=MATCH`); `expected_missing`, `expected_mismatch`, and
`offline_parity_break` are the expected-problem kinds (`status=DIFF`, counted in
`expected_error_problem_count`).

## Score Formula Weight Rationale

| Component | Weight | Rationale |
| --- | --- | --- |
| `match_rate` | 0.50 | The dominant signal: actions that fully agree need no investigation. |
| `1 - diff_rate` | 0.20 | Partial credit for actions that run clean but disagree on content. |
| `1 - error_rate` | 0.20 | Tool errors prevent any comparison; they are as bad as diffs for trend purposes. |
| `version_parity_score` | 0.10 | A version mismatch is a leading indicator of parity debt even when actions still match. |

## Cursor and Time-Field Tolerances

These tolerances match those in `verify_offline_parity.py` and are intentionally
not reflected in the score (they are below the DIFF threshold):

- **Time-tolerant fields** (`cutoff_unix`, `since_unix`): deltas within 5 s are
  recorded as warnings, not diffs.
- **`--ignore-cursor-bytes`**: decodes opaque base64 cursors and compares only
  `{p, tc}`; `qh` mismatches are recorded as warnings, not diffs.
- **`risk.get_release_window_hotspots`**: cursor `qh` is always wall-clock-derived
  and treated as a warning regardless of `--ignore-cursor-bytes`.

## Interpretation

A score above **0.95** is the healthy baseline target. A score below **0.85**
warrants investigation of individual DIFF and real ERROR rows in
`per_action.jsonl`. A score drop of more than **0.05** between consecutive runs
is a regression signal.

The benchmark does not replace `verify_offline_parity.py` as the CI hard-gate.
Use the benchmark score to detect regressions early and to quantify partial
improvements before a full PASS is achieved.

CI threshold guidance:

- Hard gate: `verify_offline_parity.py` remains the binary CI signal.
- Trend gate: require `offline_parity_score >= 0.95`,
  `real_error_count == 0`, and `expected_error_problem_count == 0`.
- Investigation gate: treat `offline_parity_score < 0.85` or a drop greater
  than `0.05` from the previous checked run as a regression.
