# Monolith Benchmarks

| Benchmark | Primary Score | Dimensions | Script |
| --- | --- | --- | --- |
| [ActionGuidance](ActionGuidance/README.md) | effectiveness_score | Discovery planning, error recovery, param correction | Scripts/action_guidance_benchmark.py |
| [SourceIndex](SourceIndex/README.md) | source_index_score | Symbol recall, field completeness, schema adherence | Scripts/source_index_benchmark.py |
| [SchemaCompleteness](SchemaCompleteness/README.md) | schema_completeness_score | Full catalog param types, required flags, planning signals | Scripts/schema_completeness_benchmark.py |
| [OfflineParity](OfflineParity/README.md) | offline_parity_score | exe-vs-py result matching, version parity | Scripts/offline_parity_benchmark.py |
| [ProjectIndex](ProjectIndex/README.md) | project_index_score | Asset search recall, gameplay tag lookup, schema adherence | Scripts/project_index_benchmark.py |
| [AssetEditing](AssetEditing/README.md) | asset_editing_score | Blueprint and cross-domain asset edit actions, graph reads, variable reads, type discovery, workflow completeness | Scripts/asset_editing_benchmark.py |

## Score Interpretation

All primary scores are in [0.0, 1.0]. Higher is better for all primary scores.

## Input Fingerprints

Benchmark `summary.json` and `partial_summary.json` files include two top-level fields:

- `benchmark_inputs`: task/probe file sha256 and line counts, manifest signature, available local DB file signatures, and compact MCP/catalog metadata.
- `input_fingerprint`: a stable sha256 digest of `benchmark_inputs` for stale-baseline detection.

Runner defaults are resolved relative to the Monolith plugin root, so `Benchmarks\...\tasks.jsonl` means `Plugins\Monolith\Benchmarks\...\tasks.jsonl` regardless of the current working directory. Hosted static CI validates that configured task/probe JSONL non-empty line counts match each benchmark manifest count and that runner default paths resolve to the configured files without needing a live MCP server.

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
