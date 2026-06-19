# BlueprintEditing Benchmark

Measures Monolith MCP blueprint editing capability across 7 Blueprint types: Actor, Character, Widget, AnimInstance, GameplayAbility, ActorComponent, and Interface.

## Files

| File | Purpose |
|------|---------|
| `tasks.jsonl` | 309 benchmark tasks across 10 categories |
| `manifest.json` | Benchmark metadata, score formula, weights, category counts |
| `METRICS.md` | Metric definitions, weights, scoring policy, anti-gaming notes |
| `RESULTS.md` | Benchmark results |
| `test_blueprints.md` | Spec for 7 fixture Blueprints under `/Game/Benchmarks/` |
| `Scripts/blueprint_editing_benchmark.py` | Runner script |

## Task Categories

> **v5.1 (2026-06-18) — adversarial hardening + practical-coverage expansion:** closed the
> scoring holes a reject-everything / read-only / no-op server could exploit and roughly doubled
> executed coverage. (1) **`error_path`** now scores on the **offending identifier** (the unique
> `NONEXISTENT_*`/`INVALID_*` token), not generic English words — a canned "could not find variable"
> no longer passes. (2) **`edit_execute` create tasks are delete-first op_chains** so the read-back
> proves THIS run made the edit (leftover state from a prior run can no longer mask a silent no-op);
> `add_node` reads back the exact returned node id; `set_component_property` reads back the VALUE;
> `add_replicated_variable` asserts the replication flag; CDO writes assert the property+value.
> (3) **`duplicate_reject`** requires a CLEAN first create (delete-reset each run). (4) New executed
> categories: **`negative_compile`** (deliberately break a graph and require the compiler to REPORT
> `error_count>0` — makes "compile is clean" falsifiable), plus data-pin wiring, pin literals, and
> delete round-trips. (5) **Re-weighted** away from trivially-passed schema-fetch (0.19→0.10) toward
> executed work. (6) Fixed three benchmark defects the live baseline surfaced: the `bIsActive`
> fixture var collided with `UActorComponent`'s native `bIsActive` (renamed `bComponentActive`),
> `validate_blueprint`'s lint-report shape was mis-scored as a failed compile, and interface tasks
> passed an unresolvable short name (now the full `/Game` path, backed by an interface-resolver fix).
>
> **v5 (2026-06-17):** `edit_execute` became read-back verified; `workflow_completeness` (existence
> only) was replaced by executed `workflow_execute`; suite expanded to 295 tasks with `preflight`.

| Category | Count | Tool | Pass Criterion |
|----------|------:|------|----------------|
| `type_discovery` | 21 | `project_query` | Valid non-error response with ≥1 result; required-result queries target fixture names and exact fixture paths |
| `graph_read` | 35 | `blueprint_query` | No transport error AND no isError AND content shape / expected-token checks |
| `variable_read` | 28 | `blueprint_query` | No transport error AND no isError AND fixture-variable / setup-created function checks |
| `read_schema` | 23 | `monolith_discover` | Schema has `planning_signals` list and `skill` string (lenient) |
| `edit_schema` | 46 | `monolith_discover` | Schema has `planning_signals` + `skill` AND no isError (strict) |
| `workflow_execute` | 11 | `blueprint_query` | Executed multi-step chains run, compile clean, and read back their end state |
| `edit_execute` | 113 | `blueprint_query` | Edit call succeeds AND its mutation is observable via read-back; creates run delete-first so the read-back proves THIS run; includes UMG, AnimBP, GAS, ActorComponent, Interface, component/property value, exec- and data-pin wiring, pin literals, and delete round-trips |
| `error_path` | 20 | `blueprint_query` | Server returns a structured `isError` whose message references the **offending identifier** (transport crash = fail; generic-only error = fail) |
| `duplicate_reject` | 11 | `blueprint_query` | First call CLEANLY creates the entity (delete-reset each run) AND a second identical `add_*` call returns a duplicate-specific `isError` |
| `negative_compile` | 1 | `blueprint_query` | A deliberately broken scratch blueprint must be REPORTED as a real compile failure (`error_count>0`); a transport/isError/clean envelope = fail |

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

## Primary Score (v5.1)

The weights live in the single `WEIGHTS` dict in `Scripts/blueprint_editing_benchmark.py` and
are the only source of truth (consumed by the scorer, the manifest formula, the comparison
report, and the module docstring — they cannot drift apart). See `METRICS.md` for the full
weights rationale and changelog.

```
blueprint_editing_score = 0.31 * edit_execute_rate       (delete-first + read-back verified)
                        + 0.12 * graph_read_rate
                        + 0.12 * variable_read_rate
                        + 0.10 * error_path_rate          (offending-identifier specific)
                        + 0.10 * workflow_execute_rate    (executed end-to-end)
                        + 0.08 * edit_schema_rate
                        + 0.07 * duplicate_reject_rate    (clean-first-call gated)
                        + 0.05 * negative_compile_rate    (compiler-runs falsifiable)
                        + 0.03 * type_discovery_rate
                        + 0.02 * read_schema_rate
```

## Score Dimensions

| Metric | Weight | Direction | Scoring Policy |
|--------|--------|-----------|----------------|
| `edit_execute_rate` | 0.31 | higher is better | Strict + delete-first read-back. Creates run a leading remove so the read-back proves THIS run made the edit; `add_node` reads back the returned node id; `set_component_property` reads back the value; node-wiring chains execute `connect_pins` (exec AND data pins); compile slots require `error_count == 0`. A non-editing stub scores 0. |
| `graph_read_rate` | 0.12 | higher is better | Strict. isError from server fails (asset-not-found is a real signal). |
| `variable_read_rate` | 0.12 | higher is better | Strict. Same policy as graph_read; Interface fixture asserts its function stubs. |
| `error_path_rate` | 0.10 | higher is better | Inverted + input-specific. The isError message must reference the **offending identifier**; a generic always-error server fails. Covers nonexistent inputs AND real precondition/value failures (create-on-existing, bad type/parent/node_type tokens). |
| `workflow_execute_rate` | 0.10 | higher is better | Strict. An executed multi-step chain must run, compile clean, and read back its end state. Replaces existence-only `workflow_completeness`. |
| `edit_schema_rate` | 0.08 | higher is better | Strict. isError on schema fetch fails the task. |
| `duplicate_reject_rate` | 0.07 | higher is better | Inverted. First call must CLEANLY create (delete-reset each run); the second identical call must return a duplicate-specific isError. Catches silent-suffix/no-op handling that `edit_execute` cannot see. |
| `negative_compile_rate` | 0.05 | higher is better | A deliberately broken scratch blueprint must be REPORTED as a real compile failure (`error_count>0`). Makes "compile is clean" falsifiable. |
| `type_discovery_rate` | 0.03 | higher is better | Valid non-error project search with ≥1 result; required-result queries target the benchmark's own fixtures (portable across projects). |
| `read_schema_rate` | 0.02 | higher is better | Lenient. Only checks `planning_signals` and `skill` metadata fields. |

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
