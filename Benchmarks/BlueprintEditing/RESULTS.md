# BlueprintEditing Benchmark Results

Latest run against the live Monolith MCP endpoint.

| Field | Value |
|-------|-------|
| Label | `current` |
| Run timestamp (UTC) | 2026-06-16T19:05:13Z |
| MCP version | `0.20.2` |
| Engine | `++UE5+Release-5.7-CL-51494982` |
| Project | `GO` |
| Tasks run | 191 |
| Tasks passed | 191 |
| `error_count` (diagnostic) | 18 |

`error_count` is diagnostic, not a failure count: it counts every `isError`/transport error
across all tasks. The 18 here are all *expected* structured `isError` responses — 8 `error_path`
invalid-input rejections, 6 `duplicate_reject` second-call rejections, and 4 `edit_execute`
"already exists" idempotent responses on the Interface fixture.

## Score

| Metric | Value |
|--------|------:|
| `blueprint_editing_score` | **1.000** |
| `edit_execute_rate` | 1.000 |
| `edit_schema_rate` | 1.000 |
| `graph_read_rate` | 1.000 |
| `variable_read_rate` | 1.000 |
| `error_path_rate` | 1.000 |
| `duplicate_reject_rate` | 1.000 |
| `read_schema_rate` | 1.000 |
| `type_discovery_rate` | 1.000 |
| `workflow_completeness_rate` | 1.000 |

## What this run established

The previous primary score reached 1.000 **only because the benchmark could not see a real
defect**: `edit_execute` passes on any non-error envelope, so it cannot distinguish an effective
duplicate-name guard from a silent suffix/no-op. Two blueprint actions were silently creating
suffixed duplicates on a repeated call instead of refusing them:

| Action | Pre-fix behavior on duplicate name | Fixed behavior |
|--------|------------------------------------|----------------|
| `add_component` | `success`, created `BenchMeshComp1` (no guard) | `isError: "A component named '<name>' already exists"` |
| `add_local_variable` | `success`, created a second local var (no guard) | `isError: "A local variable named '<name>' already exists in function '<fn>'"` |

`add_event_dispatcher` was *suspected* but cleared: its guard works in a live session; an earlier
cross-process probe only saw a suffix because of an unsaved-asset reload artifact, not a handler bug.

The new `duplicate_reject` dimension (weight 0.08) calls each guarded `add_*` action twice and
requires the second call to return a structured `isError`. It exercises `add_variable`,
`add_function`, `add_component`, `add_event_node`, `add_event_dispatcher`, and `add_local_variable`.
Run against the **unfixed** server, `add_component` and `add_local_variable` score 0, dropping
`duplicate_reject_rate` to ~0.667 — the bug class is now detectable.

## Run Command

```powershell
python Scripts\blueprint_editing_benchmark.py setup_fixtures --mcp-url http://localhost:9316/mcp
python Scripts\blueprint_editing_benchmark.py run `
  --mcp-url http://localhost:9316/mcp `
  --tasks Benchmarks\BlueprintEditing\tasks.jsonl `
  --label current `
  --output-dir Saved\Monolith\Benchmarks\BlueprintEditing\current
```
