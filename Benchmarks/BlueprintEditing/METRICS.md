# BlueprintEditing Benchmark Metrics

## Primary Score Formula

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

All rates are in `[0.0, 1.0]`. The composite score is in `[0.0, 1.0]`.
Weights sum to exactly 1.0.

> **v4 formula change (2026-06-16):** added the `duplicate_reject` dimension (weight 0.08),
> reducing `edit_execute` 0.25→0.22, `edit_schema` 0.20→0.18, `graph_read`/`variable_read`
> 0.15→0.14 each, and `type_discovery` 0.05→0.04. Scores are not directly comparable to v3
> baselines that lacked `duplicate_reject`. Rationale: `edit_execute` passes on any non-error
> envelope and therefore cannot distinguish an effective duplicate-name guard from a silent
> suffix/no-op — a real defect class it was structurally blind to.

## Metric Definitions

| Metric | Type | Range | Definition |
|--------|------|-------|------------|
| `blueprint_editing_score` | composite | 0.0 – 1.0 | Weighted sum of the eight dimension rates |
| `edit_execute_rate` | rate | 0.0 – 1.0 | Real edit actions against fixture assets — strict: isError or transport error = fail |
| `edit_schema_rate` | rate | 0.0 – 1.0 | Schema fetch for 38 edit actions — strict: isError = fail |
| `graph_read_rate` | rate | 0.0 – 1.0 | Graph reads — strict: isError = fail + content shape check for known fixtures |
| `variable_read_rate` | rate | 0.0 – 1.0 | Variable reads — strict: isError = fail + fixture variable name check |
| `error_path_rate` | rate | 0.0 – 1.0 | **Inverted**: pass = server returns structured `isError` for invalid input; transport crash = fail |
| `duplicate_reject_rate` | rate | 0.0 – 1.0 | **Inverted**: calls each guarded `add_*` action twice; pass = the second identical call returns structured `isError`. A silent suffix/no-op fails. |
| `read_schema_rate` | rate | 0.0 – 1.0 | Schema fetch for 15 read actions — lenient: only checks `planning_signals` + `skill` |
| `type_discovery_rate` | rate | 0.0 – 1.0 | project.search for BP type prefixes — requires ≥1 result (not just non-error envelope) |
| `workflow_completeness_rate` | rate | 0.0 – 1.0 | All required actions found in catalog `actions[].action` (not `.name`) |
| `error_count` | count | 0 – N | Transport errors + isError responses across all tasks (diagnostic only) |

## Score Weights Rationale

| Dimension | Weight | Rationale |
|-----------|--------|-----------|
| `edit_execute_rate` | 0.22 | Highest weight: calls real edit actions on fixture assets. A stub server with no editor connection cannot pass these. |
| `edit_schema_rate` | 0.18 | Schema correctness for all 38 edit actions; unknown action names return isError and fail the task. |
| `graph_read_rate` | 0.14 | Most common inspection call; strict + content shape check means empty envelopes don't pass. |
| `variable_read_rate` | 0.14 | Variable inspection underlies most edit workflows; same strict policy + fixture variable check. |
| `error_path_rate` | 0.10 | Graceful rejection of invalid inputs. A server that crashes instead of returning isError scores 0. |
| `duplicate_reject_rate` | 0.08 | Duplicate-name guard consistency across `add_*` actions. Catches silent suffix/no-op handling (the class of bug that `edit_execute`'s "any non-error = pass" rule is blind to). |
| `read_schema_rate` | 0.07 | Lenient introspection signal; rarely fails on healthy endpoints with correct action names. |
| `type_discovery_rate` | 0.04 | Discovery is a prerequisite but converges quickly. Requires ≥1 result (not just non-error). |
| `workflow_completeness_rate` | 0.03 | Gold-standard completeness; low weight because high variance and already covered by other dimensions. |

## Task Category Counts

| Category | Task Count | Safety Level |
|----------|------------|--------------|
| `type_discovery` | 14 | read_only |
| `graph_read` | 21 | read_only |
| `variable_read` | 14 | read_only |
| `read_schema` | 15 | read_only_discovery |
| `edit_schema` | 38 | read_only_discovery |
| `workflow_completeness` | 5 | read_only_discovery |
| `edit_execute` | 70 | mutating_fixture |
| `error_path` | 8 | read_only_invalid |
| `duplicate_reject` | 6 | mutating_idempotency |
| **Total** | **191** | |

`edit_execute` expanded from 9 Actor-only tasks to 10 tasks × 7 BP types = 70 tasks.
"already exists" responses count as success (idempotent across repeated benchmark runs).

## Read Action Names (15, verified v0.20.2)

All names verified against the live `blueprint` namespace catalog via `monolith_discover(namespace="blueprint", mode="actions")`.

| Action | Replaced |
|--------|---------|
| `list_graphs` | — |
| `get_graph_data` | — |
| `get_graph_summary` | — |
| `get_execution_flow` | replaces `get_event_graph` (did not exist) |
| `get_functions` | replaces `list_functions` (did not exist) |
| `get_variables` | — |
| `get_dependencies` | replaces `get_class_hierarchy` (did not exist) |
| `find_variable_references` | replaces `find_references` (did not exist) |
| `get_components` | replaces `list_components` (did not exist) |
| `get_blueprint_info` | — |
| `validate_blueprint` | replaces `get_compile_status` (did not exist) |
| `describe_cdo_schema` | replaces `get_compile_errors` (did not exist) |
| `get_interfaces` | — |
| `get_event_dispatchers` | — |
| `get_parent_class` | — |

## Edit Action Domain Breakdown (38, verified v0.20.2)

| Domain | Count | Actions |
|--------|------:|---------|
| `node` | 7 | `add_node`, `remove_node`, `connect_pins`, `disconnect_pins`, `set_node_position`, `set_pin_default`, `copy_nodes` |
| `function` | 8 | `add_function`, `remove_function`, `add_event_node`, `rename_function`, `set_function_params`, `set_function_thread_safe`, `add_macro`, `add_local_variable` |
| `variable` | 7 | `add_variable`, `remove_variable`, `rename_variable`, `set_variable_type`, `set_variable_defaults`, `add_replicated_variable`, `promote_pin_to_variable` |
| `class` | 4 | `reparent_blueprint`, `implement_interface`, `remove_interface`, `set_cdo_properties` |
| `component` | 5 | `add_component`, `remove_component`, `rename_component`, `set_component_property`, `reparent_component` |
| `compilation` | 2 | `compile_blueprint`, `save_asset` |
| `state` | 2 | `auto_layout`, `duplicate_graph` |
| `event` | 3 | `add_event_dispatcher`, `remove_event_dispatcher`, `set_event_dispatcher_params` |

Note: `undo`/`redo` do not exist in the blueprint catalog; replaced with `auto_layout`/`duplicate_graph`.

## Scoring Policy Details

- **`edit_execute_rate` is strict:** requires `blueprint_query` to succeed (no transport error, no `isError`). Fixture assets at `/Game/Benchmarks/` must exist. If fixtures are absent, tasks return `isError` and score 0 — this is intentional (exposes missing fixture setup).
- **`edit_schema_rate` is strict:** `monolith_discover` must return `planning_signals` + `skill` AND must not return `isError`. An unknown action name returns `isError` and fails the task, so all 38 names must exist in the live catalog.
- **`graph_read_rate`** is strict: no transport error AND no `isError` AND content check passes. For `list_graphs`, checks that the fixture's expected graph name (`EventGraph` or `AnimGraph`) appears in the response. For `get_graph_data`/`get_graph_summary`, only the no-error check applies.
- **`variable_read_rate`** is strict: no transport error AND no `isError`. For `get_variables` tasks where fixture variables are known, checks that at least one fixture variable name appears in the response text. Interface type's slot 2 uses `get_functions` (Interfaces have no variables).
- **`error_path_rate` is inverted:** the task sends invalid inputs (non-existent asset, invalid node IDs, non-existent variable/function/component names). Pass = server returns structured `isError: true`. Transport crash, timeout, or silent success on invalid input all score 0. All action names use real catalog names so the test exercises graceful error handling, not "unknown action" rejection.
- **`read_schema_rate` is lenient:** checks `planning_signals` (non-empty list) and `skill` (non-empty string). `isError` not checked for read actions.
- **`type_discovery_rate`** requires ≥1 result in the response (`results_count > 0`). A non-error empty response scores 0.
- **`workflow_completeness_rate`** parses `actions[].action` from the `monolith_discover` catalog (the live catalog uses `"action"` key, not `"name"`). Substring fallback is used only when `actions` is absent from the response (flagged in evidence).
- **`duplicate_reject_rate` is inverted and two-call:** for each guarded `add_*` action the runner issues the create call **twice** (an optional `setup_arguments` runs first to create a host entity, e.g. the function a local variable lives in). Pass = the **second** identical call returns a structured `isError` (`server_handled AND isError`). A silent suffix (`BenchMeshComp` → `BenchMeshComp1`), a silent no-op success, or a transport crash all score 0. Covered actions: `add_variable`, `add_function`, `add_component`, `add_event_node`, `add_event_dispatcher`, `add_local_variable`. The fixed names are idempotent across runs, so the first call may itself return "already exists" on a repeat run — only the second call is scored.

## Anti-Gaming Properties

| Category | Can stub server pass? | Why / Mitigation |
|----------|----------------------|-----------------|
| `edit_execute` | Partial | Requires live editor + fixture assets; "already exists" is accepted (idempotent). Stub that always returns non-error passes; stub that always returns isError fails. |
| `error_path` | No | Stub always returns non-error → `server_is_error=False` → inverted check fails all 8 tasks |
| `edit_schema` | Only if action names are known | Stub must return `planning_signals`+`skill` without `isError` for 38 specific action names |
| `graph_read` | Partially | Content check catches empty envelopes; stub returning `{"nodes":[]}` still passes get_graph_data |
| `variable_read` | Partially | Fixture variable name check catches empty variable lists for non-Interface types |
| `workflow_completeness` | Only if catalog structure is mimicked | Must return `actions[].action` list with all required action names |
| `duplicate_reject` | No | A stub that always returns non-error fails all 6 (second call must be a structured `isError`). A stub that always returns isError fails the *first* create in `edit_execute` instead. Requires real, state-dependent duplicate detection. |

Maximum achievable composite score by a stub server with correct action-name knowledge (no live editor):
`0.22*1 + 0.18*1 + 0.14*0.5 + 0.14*0.5 + 0.10*0 + 0.08*0 + 0.07*1 + 0.04*1 + 0.03*1 ≈ 0.68`
A stub server without fixture assets cannot pass `error_path_rate` (0.10) or `duplicate_reject_rate` (0.08) — both require real state-dependent `isError` behavior — and gets partial credit only on content-checked `graph_read`/`variable_read`. Adding `duplicate_reject` lowered the stub ceiling from ≈0.80 to ≈0.68.

## Catalog Version

Action names verified against Monolith MCP blueprint namespace v0.20.2 (138 actions total).
Re-verify after major server upgrades by running `monolith_discover(namespace="blueprint", mode="actions")` and comparing against `BLUEPRINT_READ_ACTIONS` and `BLUEPRINT_EDIT_ACTIONS` in `Scripts/blueprint_editing_benchmark.py`.

## Baseline Results

| Label | Score | edit_execute | duplicate_reject | Tasks | Notes |
|-------|------:|-------------:|-----------------:|------:|-------|
| baseline-v2 | 0.425 | 0.000 | — | 124 | Fixtures missing; 9 Actor-only edit_execute tasks |
| expanded-v3 | 1.000 | 1.000 | — | 185 | 70 edit_execute (10×7 types); fixtures created; all parameters verified |
| current-v4 | 1.000 | 1.000 | 1.000 | 191 | Added `duplicate_reject` (6) + rebalanced weights. Run against the **unfixed** server, `duplicate_reject_rate` = 0.667 (add_component & add_local_variable silently suffixed); both handlers fixed in MonolithBlueprint so all 6 now pass. |
