# Monolith Benchmarks

| Benchmark | Primary Score | Dimensions | Script |
| --- | --- | --- | --- |
| [ActionGuidance](ActionGuidance/README.md) | effectiveness_score | Discovery planning, error recovery, param correction | Scripts/action_guidance_benchmark.py |
| [AICapability](AICapability/README.md) | ai_capability_score | AI asset schemas, executable edits, negative gates, fixture-backed discovery | Scripts/ai_capability_benchmark.py |
| [SourceIndex](SourceIndex/README.md) | source_index_score | Symbol recall, field completeness, schema adherence | Scripts/source_index_benchmark.py |
| [SchemaCompleteness](SchemaCompleteness/README.md) | schema_completeness_score | Full catalog param types, required flags, planning signals | Scripts/schema_completeness_benchmark.py |
| [OfflineParity](OfflineParity/README.md) | offline_parity_score | exe-vs-py result matching, version parity | Scripts/offline_parity_benchmark.py |
| [ProjectIndex](ProjectIndex/README.md) | project_index_score | Asset search recall, gameplay tag lookup, schema adherence | Scripts/project_index_benchmark.py |
| [AssetEditing](AssetEditing/README.md) | asset_editing_score | Blueprint and cross-domain asset edit actions, graph reads, variable reads, type discovery, workflow completeness | Scripts/asset_editing_benchmark.py |

## Score Interpretation

All primary scores are in [0.0, 1.0]. Higher is better for all primary scores.

## Completion Inventory

[`INVENTORY.md`](INVENTORY.md) is the fixed namespace-by-namespace completion
ledger. It distinguishes missing test definitions (`unwritten`) from written
rows that do not yet have an accepted current full-run result (`unverified`).
Regenerate and validate it after any corpus, manifest, catalog snapshot, or
accepted-run change:

```powershell
python Scripts\benchmark_inventory.py --write
python Scripts\benchmark_inventory.py --portable-check
python Scripts\benchmark_inventory.py --check
```

`--portable-check` is the hosted-CI/clean-checkout mode. It rederives accepted
results from tracked `summary.json` plus every suite-specific artifact consumed
by the validator (raw JSONL rows and JSON sidecars), verifies their pinned bundle
manifest, and permits a required database to be absent only when the
bundle contains the exact recorded size, mtime, signature, content SHA-256, and
input fingerprint. If that database exists in the checkout, portable mode still
verifies it and rejects drift. Missing `Saved/...` diagnostics for pending suites
remain zero-credit gaps and are reported, rather than making hosted static CI
depend on ignored local artifacts.

`--check` is the full local/operator mode. It additionally requires every
fingerprinted database and referenced pending diagnostic to exist, and rejects
source-input mtime drift as well as size/content drift. `--write` uses this full
contract so the checked-in ledger cannot be regenerated from attestations alone.

The benchmark scope is complete only when the ledger reports zero failed,
unverified, and unwritten items. Diagnostic subsets and interrupted prefixes
remain evidence but do not reduce the accepted completion gap.

## Input Fingerprints

Benchmark `summary.json` and `partial_summary.json` files include two top-level fields:

- `benchmark_inputs`: task/probe file SHA-256 and line counts, manifest signature, suite-specific database content SHA-256 values, and compact MCP/catalog metadata.
- `input_fingerprint`: a stable sha256 digest of `benchmark_inputs` for stale-baseline detection.

Runner defaults are resolved relative to the Monolith plugin root, so `Benchmarks\...\tasks.jsonl` means `Plugins\Monolith\Benchmarks\...\tasks.jsonl` regardless of the current working directory. Hosted static CI validates that configured task/probe JSONL non-empty line counts match each benchmark manifest count and that runner default paths resolve to the configured files without needing a live MCP server.

Database identity is an exact dependency contract, not a scan of every DB that happens to exist locally:

| Suite | Database files in accepted evidence |
| --- | --- |
| ActionGuidance | none; registry-routing scope marker |
| SourceIndex | `Saved/EngineSource.db`, `Saved/graph.db` |
| SchemaCompleteness | none; live schema-registry scope marker |
| OfflineParity | `Saved/EngineSource.db` |
| ProjectIndex | `Saved/ProjectIndex.db` |
| AICapability | none; live editor AI-action scope marker |
| AssetEditing | `Saved/ProjectIndex.db` because type-discovery tasks query the project index |

Every listed database entry carries a full content SHA-256 in addition to size
and mtime. A required database must exist when a run is produced, promoted, or
checked with the full local contract. A tracked accepted bundle lets portable CI
verify that recorded identity when the multi-gigabyte database is intentionally
absent from a clean checkout; it never lets a present but changed database pass.
An unrelated database change does not stale a suite whose answers cannot depend
on it.

Hosted static CI also treats the benchmark inventory as a closed contract. Every
`Benchmarks/*/manifest.json` must have exactly one `benchmark_definitions` entry,
and every lightweight `Scripts/test_*.py` or `Scripts/tests/test_*.py` check must
appear in `benchmark_contract_tests`. `Scripts/tests/test_benchmark_ci_inventory.py`
enforces both directions and runs the inventory's portable check, so adding a
corpus, regression test, or untracked local-only completion dependency cannot
silently bypass CI.

## Release Packaging

Benchmark corpora are source-checkout inputs, not runtime plugin payload. `Scripts\make_release.ps1`
keeps release ZIPs lean by excluding generated AssetEditing JSON/JSONL corpus files while leaving
the benchmark scripts and documentation available in source. Local benchmark use is unchanged
because the tracked corpus remains in the repository.

Use this report before release hygiene reviews:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File Scripts\summarize_benchmark_corpus.ps1
```

If a generated corpus must be shared outside the source checkout, publish it as a separate
benchmark-corpus artifact with the Monolith commit SHA, manifest SHA, and task SHA, then restore it
under `Benchmarks\...` before running the benchmark. Do not add generated corpora to runtime ZIPs.

## Running All Benchmarks

Run each benchmark in sequence from the Monolith plugin root. All scripts require a live MCP endpoint at `http://localhost:9316/mcp` except OfflineParity which can run offline.

```powershell
# ActionGuidance
python Scripts\action_guidance_benchmark.py run `
  --mcp-url http://localhost:9316/mcp `
  --tasks Benchmarks\ActionGuidance\tasks.jsonl `
  --label current `
  --output-dir Saved\Monolith\Benchmarks\ActionGuidance\current

# SourceIndex
python Scripts\source_index_benchmark.py run `
  --mcp-url http://localhost:9316/mcp `
  --tasks Benchmarks\SourceIndex\tasks.jsonl `
  --label current `
  --output-dir Saved\Monolith\Benchmarks\SourceIndex\current

# SchemaCompleteness
python Scripts\schema_completeness_benchmark.py scan `
  --mcp-url http://localhost:9316/mcp `
  --label current `
  --output-dir Saved\Monolith\Benchmarks\SchemaCompleteness\current

# OfflineParity
python Scripts\offline_parity_benchmark.py run `
  --label current `
  --output-dir Saved\Monolith\Benchmarks\OfflineParity\current

# ProjectIndex
python Scripts\project_index_benchmark.py run `
  --mcp-url http://localhost:9316/mcp `
  --tasks Benchmarks\ProjectIndex\tasks.jsonl `
  --label current `
  --output-dir Saved\Monolith\Benchmarks\ProjectIndex\current

# AssetEditing
python Scripts\asset_editing_benchmark.py run `
  --mcp-url http://localhost:9316/mcp `
  --tasks Benchmarks\AssetEditing\tasks.jsonl `
  --label current `
  --output-dir Saved\Monolith\Benchmarks\AssetEditing\current
```
