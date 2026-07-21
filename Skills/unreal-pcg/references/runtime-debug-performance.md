# UE 5.8 PCG runtime, determinism, debugging, and cost

Use this reference when a request crosses from graph topology into component execution, partitioning, repeatability, output diagnosis, or performance risk. It describes what the current Monolith PCG surface can prove. It does not replace live Unreal Insights, PCG editor debugging, or a profiler, and it must not turn a static estimate into a measured engine cost.

## Choose execution ownership deliberately

The current component contract accepts `on_load`, `on_demand`, and `at_runtime`. Select one from the requested ownership model, then confirm the exact live schema and normalized read-back.

| Intent | Recommended handling through Monolith |
|---|---|
| Author, test, or regenerate explicitly in the editor | Prefer `on_demand`; schedule with `generate_component`, poll `get_component`, and inspect only when idle. |
| Persist a component configured for load-triggered behavior | Set and read back `on_load`; do not claim load behavior was proven unless a separate load/reload test actually observes it. |
| Configure engine runtime-scheduler ownership | Set and read back `at_runtime`, then treat manual lifecycle ownership as restricted. Do not force editor lifecycle actions through a runtime-owned component when the handler rejects them. |

`generate_component`, `refresh_component`, `cancel_component`, and `cleanup_component` are non-blocking requests. Preserve task IDs as decimal strings and use separate `get_component` calls. Never sleep, spin, manually tick, or hold an MCP action open while the game thread finishes work.

## Partition ownership

Partitioning is a capability and ownership boundary, not an optimization checkbox.

- Set `partitioned=true` only when the exact original component reports a configuration for which `CanPartition()` succeeds.
- Never create a component on `APCGPartitionActor`.
- A component owned by a partition actor is inspection-only, regardless of its local-component flag. Read `original_component_path` and direct mutations to the exact original user-authored component.
- The scheduling-time component package is not necessarily the final package set. PCG completion can introduce partition or external packages that were unknowable before execution; validate the final changelist after the component is idle.
- Do not infer World Partition coverage, cell residency, streaming behavior, or runtime-generation reach from `partitioned=true` alone. The 28-action PCG surface does not expose a complete World Partition execution audit.

## Build a determinism claim from controlled evidence

Determinism must name the controlled inputs and the compared outputs. “It generated twice” is not enough.

### Hold constant

- exact graph asset and topology;
- exact read-back node settings;
- exact component path, graph assignment, activation, trigger, partition setting, and signed 32-bit seed;
- exact graph-instance override names, values, and override identity;
- world/actor inputs that the graph consumes;
- the same output/resource bounds, with no truncation in compared fields.

### Compare

Choose project-specific invariants before the first run. Suitable evidence from the current surface can include:

- output row count and each row's reported data kind/count summary;
- complete sorted tag sets when `tags_truncated=false`;
- managed-resource count and complete managed-object summaries when their bounds are not truncated;
- exact generated class or soft object paths when returned by the resource summary;
- a project-defined spatial or presentation invariant verified by the owning scene/visual workflow.

Do not compare task IDs as output identity. Do not compare partial arrays. Ordering or generated object names are acceptable only when the project contract and repeated evidence establish them as stable; they are not a universal PCG guarantee.

### Repeatability recipe

```text
confirm idle and exact saved configuration
-> generate_component(force=false unless the test explicitly requires force)
-> poll get_component to idle
-> collect complete bounded output evidence
-> cleanup_component(remove_components=true)
-> poll get_component to idle and verify resources are removed as expected
-> generate again with unchanged controlled inputs
-> poll to idle
-> collect the same complete evidence
-> compare only the predeclared invariants
```

If a world input is intentionally dynamic, record it as a varying input and do not label differing output nondeterministic until that input is controlled.

## Diagnose empty or wrong output without guessing

Use the first failing layer below. Change one layer at a time and read it back before moving on.

### Layer 1: response completeness

- Check top-level and nested truncation fields on graph, node, pin, output, tag, resource, managed-object, and user-parameter responses.
- Raise only the bound that prevents the required conclusion.
- Keep `include_managed_resource_count=false` during routine polling; request exact enumeration only when the count is part of the diagnosis.

An empty returned slice with truncation or an inaccessible state is not proof of empty PCG output.

### Layer 2: component state and ownership

- Resolve the component by its exact current path.
- Confirm the exact graph read-back, activation, generation trigger, partition state, seed, and relevant user-parameter overrides.
- Require `generating=false`, `cleaning_up=false`, `refresh_in_progress=false`, and `output_accessible=true` before reading output.
- If graph assignment started a refresh, poll it to idle before changing settings or generating.
- If the target is a partition worker, inspect it and move mutations to `original_component_path`.

### Layer 3: structural graph validity

- Run `validate_pcg_graph` with output and isolation requirements chosen from the graph contract.
- Inspect all returned errors/warnings and their truncation evidence.
- Read topology again and verify every intended edge exists at both endpoints.
- Confirm no branch ends at an unintended isolated node and no expected graph output is disconnected.

### Layer 4: pin and settings semantics

- Inspect exact live pin labels and data types from `get_pcg_graph_info`.
- Treat direct compatibility, required filter, and required conversion as distinct outcomes; insert an explicit suitable node when needed.
- Read back the concrete settings class and only the load-bearing fields.
- Dry-run the suspected property tree before applying a correction.
- Verify exact class/asset references; never substitute a default resource just to make output non-empty.

### Layer 5: source data

- Confirm the source stage receives the actor, spatial, or generated input the design assumes.
- Check coordinate-space, extents, selection rules, metadata/attribute names, and other live-discovered fields that can legally eliminate every item.
- Use the smallest controlled source/input fixture that reproduces the issue. Do not rewrite the production graph into a different algorithm to hide missing source data.

### Layer 6: realization and lifecycle

- Distinguish data output from managed resources. A graph can produce point data without creating actors, or create managed resources under a contract that does not expose the expected graph output row.
- Confirm the realization node's exact class/asset setting and downstream connection.
- Cleanup stale generated state, poll idle, and rerun only after the configuration is read back.
- Use `force=true` only when the test requires forced regeneration; do not use it as a default workaround for an unexplained dirty/up-to-date decision.

If all observable layers pass but output remains semantically wrong, record a PCG semantic/debug-data gap. The current surface does not expose per-node execution data or a trace of the first node that drops all items; do not fabricate that attribution from final output alone.

## Estimate cost without pretending to profile

The current PCG actions expose bounded structure and output summaries, not node timings, peak memory, scheduler occupancy, or GPU eligibility. Separate three kinds of statement:

| Statement | Allowed evidence |
|---|---|
| Structural size | Complete node/edge/pin read-back and validator totals. |
| Output amplification | Controlled input expectation compared with complete output/point/resource/object counts. |
| Runtime performance | A real project profiler or benchmark outside this 28-action surface. Do not infer milliseconds or memory from counts alone. |

For a high-ROI static cost review:

1. Record expected source cardinality or a bounded source fixture.
2. Identify stages that can multiply data, branch it, or realize world objects.
3. Define maximum acceptable output rows, points, resources, and managed objects before execution.
4. Generate once, poll idle, and inspect with small bounds first.
5. If a result is truncated, raise only the relevant limit far enough to prove or reject the budget.
6. Treat generated-actor/component count as a cost signal, not a complete performance measurement.
7. Cleanup and verify the resource state so repeated profiling is not contaminated by prior output.

Prefer fewer deliberate inspections over repeatedly requesting maximum 500-row nested summaries. Avoid `get_component(include_managed_resource_count=true)` in every poll, and request detailed settings only for fields that influence the current decision.

## Recover interrupted work safely

After an editor rebuild, reconnect, undo/redo, or object reconstruction:

1. Re-run `monolith_discover` for the namespace and every action you will call.
2. Re-resolve the graph and actor/component through current read actions.
3. Discard cached UObject identity assumptions and carry the newly returned exact `component_path`.
4. Read lifecycle state before deciding whether to mutate, poll, cancel, clean, or generate.
5. Resume from the first unmet postcondition, using `return_existing` only where its exact-match contract is appropriate.

Do not spawn repeated editors or retry recovery indefinitely. If the live transport cannot be restored through the owning Monolith recovery workflow, record the editor-backed verification as blocked instead of using an offline index to claim live PCG success.
