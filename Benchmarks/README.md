# Monolith Benchmarks

| Benchmark | Primary Score | Dimensions | Script |
| --- | --- | --- | --- |
| [ActionGuidance](ActionGuidance/README.md) | effectiveness_score | Discovery planning, error recovery, param correction | Scripts/action_guidance_benchmark.py |
| [SourceIndex](SourceIndex/README.md) | source_index_score | Symbol recall, field completeness, schema adherence | Scripts/source_index_benchmark.py |
| [SchemaCompleteness](SchemaCompleteness/README.md) | schema_completeness_score | Full catalog param types, required flags, planning signals | Scripts/schema_completeness_benchmark.py |
| [OfflineParity](OfflineParity/README.md) | offline_parity_score | exe-vs-py result matching, version parity | Scripts/offline_parity_benchmark.py |
| [ProjectIndex](ProjectIndex/README.md) | project_index_score | Asset search recall, gameplay tag lookup, schema adherence | Scripts/project_index_benchmark.py |
| [BlueprintEditing](BlueprintEditing/README.md) | blueprint_editing_score | Edit action schemas, graph reads, variable reads, type discovery, workflow completeness | Scripts/blueprint_editing_benchmark.py |

## Score Interpretation

All primary scores are in [0.0, 1.0]. Higher is better for all primary scores.

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

# BlueprintEditing
python Scripts\blueprint_editing_benchmark.py run `
  --mcp-url http://localhost:9316/mcp `
  --tasks Benchmarks\BlueprintEditing\tasks.jsonl `
  --label current `
  --output-dir Saved\Monolith\Benchmarks\BlueprintEditing\current
```
