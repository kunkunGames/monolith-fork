# Offline Parity Benchmark Metrics

This benchmark scores the exe-vs-py parity quality of the two Monolith offline
tools: `Binaries/monolith_query.exe` (C++) and `Scripts/monolith_offline.py`
(Python reference implementation).

## Primary Score

```text
offline_parity_score =
  0.50 * match_rate           (fraction of non-skipped actions with status=MATCH)
+ 0.20 * (1 - diff_rate)      (1 - fraction of non-skipped actions with status=DIFF)
+ 0.20 * (1 - error_rate)     (1 - fraction of non-skipped actions with status=ERROR)
+ 0.10 * version_parity_score (1.0 if exe parity_spec_rev == py parity_spec_rev, else 0.0)
```

Note: `match_rate + diff_rate + error_rate = 1.0` for non-skipped actions.

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
| `skip_rate` | Lower is better | Fraction of all actions that were skipped due to missing tool, missing corpus input, or other non-runnable condition. |
| `version_parity_score` | Higher is better | 1.0 if `parity_spec_rev` matches between exe and py; 0.0 otherwise. |
| `total_actions` | Informational | Total number of actions in the benchmark table (110+ normally). |
| `comparable_actions` | Informational | Non-skipped actions actually compared (total - skip). |
| `action_count` | Higher is better | Alias for `comparable_actions`; tracks coverage — higher means more namespace/input combinations were exercised. |
| `mean_diffs_per_diff_action` | Lower is better | Average number of diverging fields among actions with status=DIFF. |
| `category_breakdown` | Informational | Per-namespace breakdown of match/diff/error/skip counts and `action_count` (non-skipped). Keys are namespace prefixes (`cppreflect`, `network`, `decision`, `risk`, `source`). |

## Action Statuses

| Status | Meaning |
| --- | --- |
| `MATCH` | Both tools returned exit 0 and their outputs deep-equal (within cursor/time tolerances). |
| `DIFF` | Both tools returned exit 0 but at least one field diverges. |
| `ERROR` | At least one tool returned non-zero exit code or unparseable output. |
| `SKIP` | Action was not run (missing tool executable, no valid corpus input for chained args). |

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
warrants investigation of individual DIFF and ERROR rows in `per_action.jsonl`.
A score drop of more than **0.05** between consecutive runs is a regression signal.

The benchmark does not replace `verify_offline_parity.py` as the CI hard-gate.
Use the benchmark score to detect regressions early and to quantify partial
improvements before a full PASS is achieved.
