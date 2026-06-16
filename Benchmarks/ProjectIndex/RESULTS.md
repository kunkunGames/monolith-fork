# ProjectIndex Benchmark Results

No results yet. Run the benchmark to populate.

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

| Run label | Date | project_index_score | search_hit_rate | field_completeness_rate | schema_adherence_rate | stale_rate |
| --- | --- | ---: | ---: | ---: | ---: | ---: |
| _(no runs yet)_ | -- | -- | -- | -- | -- | -- |
