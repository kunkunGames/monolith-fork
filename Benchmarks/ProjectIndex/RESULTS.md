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
