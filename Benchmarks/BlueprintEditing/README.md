# BlueprintEditing Benchmark

Measures Monolith MCP blueprint editing capability across 7 Blueprint types: Actor, Character, Widget, AnimInstance, GameplayAbility, ActorComponent, and Interface.

## Files

| File | Purpose |
|------|---------|
| `tasks.jsonl` | 191 benchmark tasks across 9 categories |
| `manifest.json` | Benchmark metadata, score formula, category counts |
| `METRICS.md` | Metric definitions, weights, scoring policy, anti-gaming notes |
| `RESULTS.md` | Benchmark results (stub until first run) |
| `test_blueprints.md` | Spec for 7 fixture Blueprints under `/Game/Benchmarks/` |
| `Scripts/blueprint_editing_benchmark.py` | Runner script |

## Task Categories

| Category | Count | Tool | Pass Criterion |
|----------|------:|------|----------------|
| `type_discovery` | 14 | `project_query` | Valid non-error JSON response with ≥1 result (prefix queries) |
| `graph_read` | 21 | `blueprint_query` | No transport error AND no isError AND content shape check |
| `variable_read` | 14 | `blueprint_query` | No transport error AND no isError AND fixture-variable check |
| `read_schema` | 15 | `monolith_discover` | Schema has `planning_signals` list and `skill` string (lenient) |
| `edit_schema` | 38 | `monolith_discover` | Schema has `planning_signals` + `skill` AND no isError (strict) |
| `workflow_completeness` | 5 | `monolith_discover` | All required action names present in catalog `actions[].action` |
| `edit_execute` | 70 | `blueprint_query` | Actual edit call succeeds — no transport error, no isError (10 tasks × 7 BP types; requires fixture assets) |
| `error_path` | 8 | `blueprint_query` | Server returns structured `isError: true` for invalid input (transport crash = fail) |
| `duplicate_reject` | 6 | `blueprint_query` | A second identical `add_*` call returns structured `isError` (an effective duplicate-name guard, not a silent suffix) |

## Blueprint Types Tested

| Type | Domain | Benchmark Path |
|------|--------|----------------|
| Actor | gameplay | `/Game/Benchmarks/BPB_TestActor` |
| Character | gameplay | `/Game/Benchmarks/BPB_TestCharacter` |
| Widget | ui | `/Game/Benchmarks/WBP_TestWidget` |
| AnimInstance | animation | `/Game/Benchmarks/ABP_TestAnim` |
| GameplayAbility | ability | `/Game/Benchmarks/GA_TestAbility` |
| ActorComponent | component | `/Game/Benchmarks/BC_TestComponent` |
| Interface | interface | `/Game/Benchmarks/BPI_TestInterface` |

## Primary Score

```
blueprint_editing_score = 0.22 * edit_execute_rate
                        + 0.18 * edit_schema_rate
                        + 0.14 * graph_read_rate
                        + 0.14 * variable_read_rate
                        + 0.10 * error_path_rate
                        + 0.08 * duplicate_reject_rate
                        + 0.07 * read_schema_rate
                        + 0.04 * type_discovery_rate
                        + 0.03 * workflow_completeness_rate
```

## Score Dimensions

| Metric | Weight | Direction | Scoring Policy |
|--------|--------|-----------|----------------|
| `edit_execute_rate` | 0.22 | higher is better | Strict. Real edit actions on fixture assets. Stub servers score 0. |
| `edit_schema_rate` | 0.18 | higher is better | Strict. isError on schema fetch fails the task. |
| `graph_read_rate` | 0.14 | higher is better | Strict. isError from server fails (asset-not-found is a real signal). |
| `variable_read_rate` | 0.14 | higher is better | Strict. Same policy as graph_read. |
| `error_path_rate` | 0.10 | higher is better | Inverted. Server must return isError for invalid inputs; transport crash fails. |
| `duplicate_reject_rate` | 0.08 | higher is better | Inverted. Calls each guarded `add_*` action twice; the second identical call must return isError. Catches silent-suffix/no-op duplicate handling that `edit_execute` cannot see. |
| `read_schema_rate` | 0.07 | higher is better | Lenient. Only checks `planning_signals` and `skill` metadata fields. |
| `type_discovery_rate` | 0.04 | higher is better | Valid non-error project search response with ≥1 result. |
| `workflow_completeness_rate` | 0.03 | higher is better | All required steps in catalog `actions[].action` list. |

## Workflows Tested

| Workflow | Required Actions |
|----------|-----------------|
| `create_function` | `add_function`, `add_node`, `connect_pins`, `compile` |
| `add_variable` | `add_variable`, `set_variable_type`, `set_variable_default` |
| `add_component` | `add_component`, `set_component_property`, `reparent_component`, `compile` |
| `implement_interface` | `add_interface`, `add_function`, `compile` |
| `event_dispatcher_setup` | `add_event_dispatcher`, `add_event_dispatcher_binding`, `compile` |

## Generate

```powershell
python Scripts\blueprint_editing_benchmark.py generate `
  --tasks Benchmarks\BlueprintEditing\tasks.jsonl `
  --manifest Benchmarks\BlueprintEditing\manifest.json
```

## Run

```powershell
python Scripts\blueprint_editing_benchmark.py run `
  --mcp-url http://localhost:9316/mcp `
  --tasks Benchmarks\BlueprintEditing\tasks.jsonl `
  --label current `
  --output-dir Saved\Monolith\Benchmarks\BlueprintEditing\current
```

## Compare

```powershell
python Scripts\blueprint_editing_benchmark.py compare `
  --baseline Saved\Monolith\Benchmarks\BlueprintEditing\baseline\summary.json `
  --current  Saved\Monolith\Benchmarks\BlueprintEditing\current\summary.json `
  --output-dir Saved\Monolith\Benchmarks\BlueprintEditing\compare
```

## See Also

- [Benchmarks README](../README.md)
