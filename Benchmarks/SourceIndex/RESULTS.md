# SourceIndex Benchmark Results

Latest verified run against the live `source` namespace (MCP `http://localhost:9316/mcp`, engine `++UE5+Release-5.8`).

## 2026-07-11 run-20260711-final — post-reindex + CRG-repair final

The canonical run used the fully rebuilt Source DB followed by the health-directed CRG cache repair.
Deep health proved symbol/FTS parity, no orphan symbols or references, and exact CRG parity:
1,325,706 source symbols/nodes/metrics and 90,832 valid native/projected edges. The run then completed
all 363 tasks with zero transport failures and identical start/end endpoint, catalog, project, and
engine identity.

| Metric | Value |
| --- | ---: |
| `source_index_score` | **0.910407** |
| `symbol_hit_rate` | 0.948454 |
| `field_completeness_rate` | 0.773585 |
| `schema_adherence_rate` | 1.000000 |
| `stale_rate` | 0.000000 |
| `ergonomics_success_rate` | 1.000000 |
| `negative_recovery_rate` | 0.807692 |
| `transport_failure_count` | 0 |

Canonical task SHA-256 is
`5D4EA0D9E72C05154463F65C3CD1C55ED04C8B45F92C65452C188EF46782E540`;
manifest SHA-256 is
`25259F20FFF5AC9E51F6B378FA90233B77CA2B84EB3F156A37B4687D032D0A44`;
input fingerprint is
`acdaf84d264c1f7c9da3ec5aa0539b70889a39a71017099e4cce5b3253552931`.
Output: `Saved\Monolith\Benchmarks\SourceIndex\run-20260711-final`.

## 2026-07-11 baseline-20260711 — first baseline on the 6-term formula

The 2026-06-17 score (0.93253) used the older 5-term formula and is not
comparable. This is the first live baseline after the loophole-closing 6-term
formula change (Speed CL 1093 suite-wide refresh,
`Docs/testing/2026-07-11-benchmark-contract-failfast-and-n3-guards.md`).

| Metric | Value |
| --- | ---: |
| `source_index_score` | **0.8750** |
| `symbol_hit_rate` | 0.9072 |
| `field_completeness_rate` | 0.7736 |
| `schema_adherence_rate` | 1.0000 |
| `stale_rate` | 0.0000 |
| `ergonomics_success_rate` | 1.0000 |
| `negative_recovery_rate` | 0.6538 |

363 tasks, all executed (no transport failures). Weak dimensions to improve:
`negative_recovery_rate` (invalid-input responses that fail to name the
offending identifier) and `field_completeness_rate`.
Output: `Saved\Monolith\Benchmarks\SourceIndex\baseline-20260711`.
Editor 0.20.3, engine `++UE5+Release-5.8`.

## How to populate

```powershell
python Plugins\Monolith\Scripts\source_index_benchmark.py run `
  --mcp-url http://localhost:9316/mcp `
  --tasks Plugins\Monolith\Benchmarks\SourceIndex\tasks.jsonl `
  --label initial `
  --output-dir Saved\Monolith\Benchmarks\SourceIndex\initial `
  --request-timeout-s 12
```

Then copy the key metrics here from `Saved\Monolith\Benchmarks\SourceIndex\initial\summary.json`.

## Results table scaffold

| Run label | Date | Formula | source_index_score | symbol_hit_rate | field_completeness_rate | schema_adherence_rate | stale_rate | negative_recovery_rate |
| --- | --- | --- | ---: | ---: | ---: | ---: | ---: | ---: |
| `run-20260711-final` | 2026-07-11 | 6-term | 0.910407 | 0.948454 | 0.773585 | 1.0 | 0.0 | 0.807692 |
| `baseline-20260711` | 2026-07-11 | 6-term | 0.8750 | 0.9072 | 0.7736 | 1.0 | 0.0 | 0.6538 |
| `fix-final-confirm` | 2026-06-17 | 5-term (pre-loophole-fix) | 0.93253 | 0.807229 | 1.0 | 1.0 | 0.0 | n/a |

Notes:

- **Score formula changed (2026-06-18).** The composite is now the 6-term formula in `METRICS.md`/`manifest.json` (`0.30 symbol_hit + 0.20 field_completeness + 0.15 schema_adherence + 0.10 (1-stale) + 0.10 ergonomics + 0.15 negative_recovery`). The `0.93253` row above was produced under the previous 5-term formula and before the empty-response loophole and `negative_recovery` category existed; it is retained for history only and is **not comparable** to the two 6-term 2026-07-11 runs.
- Two structural loopholes are now closed: (1) `require_results` lookups (28 curated symbols KNOWN to have a definition / callers / callees) score 0 on an empty or sentinel response, and (2) `field_completeness_rate` is averaged only over those expected-nonempty lookups, so an all-empty run can no longer earn a vacuous `1.0`. The earlier `symbol_hit_rate=0.807` was inflated by empty `find_callers`/`find_callees` returns counting as hits; the new score is expected to drop until the index actually serves call edges for those symbols.
- The new `negative_recovery` category (13 tasks) measures self-correcting behaviour on bad input — `get_include_path`/`get_signature`/`verify_symbols` etc. must return a structured error that names the offending identifier with a did-you-mean / qualified-symbol hint, not a crash or silent empty.
- `health_check` tasks pass when `status` is healthy; a symbol count is only required for `include_counts=true` requests because the `source health` action only emits `row_counts.symbols` in that mode.
