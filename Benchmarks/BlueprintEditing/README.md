# BlueprintEditing Benchmark

Measures Monolith MCP blueprint editing capability across 7 Blueprint types: Actor, Character, Widget, AnimInstance, GameplayAbility, ActorComponent, and Interface.

## Files

| File | Purpose |
|------|---------|
| `tasks.jsonl` | 295 benchmark tasks across 9 categories |
| `manifest.json` | Benchmark metadata, score formula, weights, category counts |
| `METRICS.md` | Metric definitions, weights, scoring policy, anti-gaming notes |
| `RESULTS.md` | Benchmark results (stub until first v5 run) |
| `test_blueprints.md` | Spec for 7 fixture Blueprints under `/Game/Benchmarks/` |
| `Scripts/blueprint_editing_benchmark.py` | Runner script |

## Task Categories

> **v5 (2026-06-17):** `edit_execute` is now **read-back verified** (the mutation must be
> observable via a follow-up read, and node-wiring is executed for real by threading
> `add_node` ids into `connect_pins`); compile steps assert a **clean compile**
> (`error_count == 0`) from the payload, not just a non-error envelope. The existence-only
> `workflow_completeness` category was replaced by **`workflow_execute`** (executed
> build→wire→compile-clean→read-back chains). The fixture-lifecycle patch expands the suite
> by exactly 100 tasks (195→295), adds `preflight`, and separates transport failures from
> fixture-missing / fixture-contract failures before `setup_fixtures` and `run`.

| Category | Count | Tool | Pass Criterion |
|----------|------:|------|----------------|
| `type_discovery` | 21 | `project_query` | Valid non-error response with ≥1 result; required-result queries target fixture names and exact fixture paths |
| `graph_read` | 35 | `blueprint_query` | No transport error AND no isError AND content shape / expected-token checks |
| `variable_read` | 28 | `blueprint_query` | No transport error AND no isError AND fixture-variable / setup-created function checks |
| `read_schema` | 23 | `monolith_discover` | Schema has `planning_signals` list and `skill` string (lenient) |
| `edit_schema` | 46 | `monolith_discover` | Schema has `planning_signals` + `skill` AND no isError (strict) |
| `workflow_execute` | 11 | `blueprint_query` | Executed multi-step chains run, compile clean, and read back their end state |
| `edit_execute` | 104 | `blueprint_query` | Edit call succeeds AND its mutation is observable via read-back; includes UMG, AnimBP, GAS, ActorComponent, Interface, component/property edits, and node-wiring chains |
| `error_path` | 16 | `blueprint_query` | Server returns a structured `isError` whose message references the offending input (transport crash = fail; generic error = fail) |
| `duplicate_reject` | 11 | `blueprint_query` | First call creates the entity AND a second identical `add_*` call returns a duplicate-specific `isError` (not a silent suffix/no-op) |

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

## Primary Score (v5)

The weights live in the single `WEIGHTS` dict in `Scripts/blueprint_editing_benchmark.py` and
are the only source of truth (consumed by the scorer, the manifest formula, the comparison
report, and the module docstring — they cannot drift apart).

```
blueprint_editing_score = 0.26 * edit_execute_rate      (read-back verified)
                        + 0.14 * edit_schema_rate
                        + 0.12 * graph_read_rate
                        + 0.12 * variable_read_rate
                        + 0.10 * error_path_rate         (input-specific)
                        + 0.10 * workflow_execute_rate   (executed end-to-end)
                        + 0.08 * duplicate_reject_rate   (first-call gated)
                        + 0.05 * read_schema_rate
                        + 0.03 * type_discovery_rate     (fixture-based)
```

## Score Dimensions

| Metric | Weight | Direction | Scoring Policy |
|--------|--------|-----------|----------------|
| `edit_execute_rate` | 0.26 | higher is better | Strict + read-back. The mutation must be observable via a follow-up read; node-wiring chains execute `connect_pins` and confirm the connection; compile slots require `error_count == 0`. A non-editing stub scores 0. |
| `edit_schema_rate` | 0.14 | higher is better | Strict. isError on schema fetch fails the task. |
| `graph_read_rate` | 0.12 | higher is better | Strict. isError from server fails (asset-not-found is a real signal). |
| `variable_read_rate` | 0.12 | higher is better | Strict. Same policy as graph_read; Interface fixture asserts its function stubs. |
| `error_path_rate` | 0.10 | higher is better | Inverted + input-specific. The isError message must reference the offending input; a generic "always error" server fails. |
| `workflow_execute_rate` | 0.10 | higher is better | Strict. An executed multi-step chain must run, compile clean, and read back its end state. Replaces existence-only `workflow_completeness`. |
| `duplicate_reject_rate` | 0.08 | higher is better | Inverted. First call must create the entity; the second identical call must return a duplicate-specific isError. Catches silent-suffix/no-op handling that `edit_execute` cannot see. |
| `read_schema_rate` | 0.05 | higher is better | Lenient. Only checks `planning_signals` and `skill` metadata fields. |
| `type_discovery_rate` | 0.03 | higher is better | Valid non-error project search with ≥1 result; required-result queries target the benchmark's own fixtures (portable across projects). |

## Workflows Executed (`workflow_execute`)

Each workflow is actually **run** step-by-step against a fixture (not just checked for catalog
presence), with `add_node` ids threaded into `connect_pins` and a final read-back.

| Workflow | Executed Steps | Read-back |
|----------|----------------|-----------|
| `build_function` | `add_function` → two `add_node` (PrintString) in the function graph → `connect_pins` (then→execute) → `compile_blueprint` | function present in `get_functions` AND the connection exists |
| `implement_interface` | `implement_interface` (BPI_TestInterface) → `compile_blueprint` | interface present in `get_interfaces` |
| `component_assembly` | `add_component` (SphereComponent) → `set_component_property` (SphereRadius) → `compile_blueprint` | component present in `get_components` |
| `widget_style_refresh` | `add_function` → `set_function_params` → `add_node` (FormatText) → `compile_blueprint` | signature contains style input |
| `widget_comment_layout` | `add_comment_node` → `auto_layout` → `compile_blueprint` | graph contains comment text |
| `anim_threadsafe_graph` | `add_function` → `set_function_thread_safe` → `add_node` (Speed get) → `compile_blueprint` | function present |
| `gas_activate_override` | `override_parent_function` (K2_ActivateAbility) → `add_node` → `compile_blueprint` | override present |
| `actorcomponent_toggle_contract` | `add_function` → `set_function_params` → `add_node` (bIsActive set) → `compile_blueprint` | signature contains input |
| `interface_signature_contract` | `add_function` → `set_function_params` → `compile_blueprint` | interface signature contains output |
| `actor_component_property_assembly` | `add_component` (PointLightComponent) → `set_component_property` (Intensity) → `compile_blueprint` | component present |
| `duplicate_function_graph` | `add_function` → `duplicate_graph` → `compile_blueprint` | duplicated graph present |

## Fixture Lifecycle

```powershell
python Scripts\blueprint_editing_benchmark.py preflight `
  --mcp-url http://localhost:9316/mcp

python Scripts\blueprint_editing_benchmark.py setup_fixtures `
  --mcp-url http://localhost:9316/mcp
```

`preflight` first checks `monolith_status`. If the live MCP endpoint is absent, the result is
`failure_kind=transport_error`. If the endpoint is alive but fixture assets or setup-created
variables/functions are missing, the result is `fixture_missing_or_invalid` or
`fixture_contract_missing`. `run` performs this readiness preflight by default; use
`--skip-preflight` only for compatibility testing of old behavior.

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
