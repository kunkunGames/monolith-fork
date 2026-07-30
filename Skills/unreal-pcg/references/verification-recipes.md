# UE 5.7/5.8 PCG verification recipes

Use the smallest recipe that proves the requested outcome. Every recipe starts from live catalog/schema discovery; the checked-in 28-action table is a compatibility expectation, not permission to call a missing action. These recipes compose existing typed actions and do not add per-node execution tracing or profiling capabilities.

## Verification layers

| Layer | What it can prove | What it cannot prove |
|---|---|---|
| Catalog and schema | The action and parameter contract exposed by the running editor. | That an asset, actor, node type, property, or pin exists. |
| Structural graph validation | Ownership, settings presence, edge endpoint/direction/type/capacity, duplicate/cycle, isolation, and requested output-connection invariants. | That the algorithm produces the intended procedural result. |
| Settings/topology read-back | The graph contains the exact committed nodes, properties, pins, and edges returned within complete bounds. | That the nodes execute successfully in the target world. |
| Component lifecycle/output | The exact component scheduled, reached idle, and exposed bounded result/resource evidence. | Per-node timing, the first node that removed data, or unobserved runtime/streaming behavior. |
| Persistence/reload | Saved graph/level state survives a real load boundary. | Runtime behavior not exercised after reload. |
| Visual/gameplay proof | The requested presentation is visible and usable in the tested scenario. | General structural correctness outside the captured scenario. |

A completion claim must name the layers actually exercised. Do not turn an offline asset index, static build, or structural validator into proof of live generation.

## Recipe A: read-only graph review

Use for inspection, design review, or impact analysis without mutation.

1. Discover `pcg` and the schemas for `get_pcg_graph_info` and `validate_pcg_graph`.
2. Resolve the exact graph asset path through a read action; do not use a screenshot label as identity.
3. Read topology with bounds sized to the graph. If any node, edge, pin, settings, or shared-response budget is truncated, raise only that bound and repeat once.
4. Validate with:
   - `require_output_connection=true` when graph output is part of the declared contract;
   - `require_no_isolated_nodes=true` when every element node must participate in execution.
5. Separate findings into structural failures, semantic design concerns, evidence truncation, and unsupported observability.

Done evidence: exact asset path, complete topology counts, validator error/warning totals and truncation state, plus a concise semantic judgment tied to the written graph contract.

## Recipe B: rerunnable graph authoring

Use for a new graph or a topology/settings change.

### Plan

1. Read `authoring-dataflow.md` and write the source-to-output contract.
2. Discover all planned authoring action schemas.
3. For every unfamiliar stage, query live node types and editable fields. Resolve ambiguity with the exact concrete class path.
4. Capture a bounded baseline with `get_pcg_graph_info` when changing an existing graph.

### Apply

1. Create/reuse the graph only under the exact `existing_policy` contract.
2. Give nodes stable authored titles when rerunnable automation needs exact matching.
3. Add one settings node at a time and read back its actual `node_id` and pins.
4. Dry-run every nontrivial settings property tree. Apply only after the dry-run report matches the intended field/value contract.
5. When the graph needs user parameters, call `set_pcg_graph_user_parameters` with its default dry-run before component override work. Require explicit defaults, inspect every add/update/remove row and before/after count, commit atomically, then verify after reload through the assigned component's parameter read-back.
6. For a static subgraph node, call dedicated `set_pcg_subgraph` with the exact graph/interface path, dry-run before commit, and require exact assigned-interface/concrete-graph read-back. Do not write `SubgraphInstance` or bypass recursion/filter rejection through generic reflection.
7. Connect exact returned node IDs and pin labels. Insert explicit discovered conversion/filter nodes when direct compatibility is unavailable.
8. Use `save=true` at intentional persistence checkpoints. If intermediate actions use `save=false` to reduce save churn, remember that earlier successful edits remain dirty when a later independent action fails; either continue to a validated final save or deliberately revert them.

### Prove

1. Read back topology and load-bearing settings with complete bounds.
2. Run strict structural validation appropriate to the graph contract.
3. Require explicit save evidence for persistent work.
4. If persistence across editor/load boundaries is required, perform a real load-away/reload through the owning live workflow, resolve the asset again, and repeat read-back/validation. Do not claim reload proof from an in-memory save result.

Done evidence: exact graph path; node/edge counts; exact load-bearing settings; validation totals; response-completeness fields; save evidence; and reload evidence when required.

### Whole-graph donor promotion variant

When the goal is to replace an existing canonical graph from a fully authored donor while retaining the target identity, use `replace_pcg_graph_contents` instead of replaying individual authoring calls. Require distinct exact clean `UPCGGraph` assets, reject recursive donors or a donor that contains the target anywhere in its static-subgraph hierarchy, and set explicit node/edge bounds large enough for both complete graphs. Run the default dry-run and verify source counts, target-before counts, `target_identity_preserved=true`, and `persistent_properties_verified=true`; then commit only with `dry_run=false`, `confirm=true`, and the intended save value. Read back the same target path with complete bounds, validate, perform a real package reload, and repeat. A second identical commit must return `unchanged` with `saved=false`. Treat any `rollback_status` other than `verified` on failure as a hard stop, leaving the target dirty for operator recovery.

## Recipe C: editor-world component generation

Use for component creation, configuration, graph assignment, generation, result inspection, cleanup, and repeatability.

### Persistent setup

For a reusable Actor Blueprint host class, use `set_blueprint_component_graph` instead of the live-instance sequence below. Resolve the canonical project-owned Blueprint asset path and exact SCS variable name, run the default dry-run first, then commit only with `dry_run=false` plus `confirm=true`. Require a successful compile, reconstructed-template re-resolution, exact graph read-back, and save evidence when persistence was requested. Do not substitute `set_component_graph`, which intentionally targets an exact live editor-world instance.

1. Discover every lifecycle schema used after the current editor connection.
2. Resolve the exact active-world actor path.
3. `create_component` without a graph, carrying its returned exact `component_path` forward.
4. `set_component_settings` with one valid final configuration.
5. `set_component_graph` with the exact graph asset path.
6. Poll separate `get_component` requests until graph-assignment refresh is idle.
7. When changing existing graph-instance scalar overrides, fetch the exact parameter names first, dry-run one complete batch, then commit it. Do not attempt graph user-parameter schema creation through this action.

For an existing component, begin with `get_component`; never replay setup steps blindly. A value-identical `save=true` call is still a guarded mutation and is not a shortcut around generated/active state.

### Execute

1. Declare expected output/resource invariants before generation.
2. Call `generate_component` and record `generation_task_valid` plus the decimal-string task ID.
3. Poll with separate bounded `get_component` calls. Do not request exact managed-resource enumeration on every poll.
4. Continue only when generation, cleanup, and refresh are idle and `output_accessible=true`.
5. Call `get_component_output` with small bounds first. Inspect every top-level and nested count/returned/limit/truncated field.
6. Raise only the bound required to prove each acceptance invariant.

### Clean and repeat

1. Schedule `cleanup_component` with the intended removal mode and record the valid/string task pair.
2. Poll idle. Verify the post-clean state required by the task.
3. Generate again with controlled inputs unchanged when determinism or rerunnability is claimed.
4. Compare only complete, predeclared invariants as defined in `runtime-debug-performance.md`.

Done evidence: exact actor/component/graph paths; normalized settings and seed; save state; task valid/string pairs; final idle state; complete output/tag/resource/object summaries for each asserted field; cleanup result; and second-run comparison when repeatability is claimed.

## Recipe D: partitioned or asynchronous package work

Use when `partitioned=true`, World Partition/external actor packages may be involved, or generation can create packages after scheduling.

1. Mutate only the exact original user-authored component. Treat partition-actor-owned components as read-only and follow `original_component_path`.
2. Inspect `source_control_prepare` from each handler-owned persistent/scheduling mutation. An explicit transient/provider-unavailable skip is evidence of a skip, not checkout success.
3. Poll the PCG work to idle before determining the final file/package set.
4. Run the repository's final changelist/asset validation through its owning live namespace and freshly discovered schema. The PCG scheduling-time package is not sufficient proof of full containment.
5. Report unmapped, invalid, skipped, warning, and truncated results explicitly. If live validation is unavailable, record that as a verification limit; do not substitute an offline index that cannot see pending assets.

Done evidence: original component identity, partition configuration read-back, asynchronous completion, final changelist membership, and live asset-validation totals or a clearly stated limitation.

## Recipe E: visual or gameplay-facing proof

Use only when PCG changes presentation, gameplay, actor placement, VFX, materials, animation, UI, or another visual world result.

1. Complete structural, lifecycle, and output verification first.
2. Use the repository's required editor/PIE/standalone workflow at its required resolution and camera rules.
3. Capture the actual generated state, not an editor thumbnail or graph screenshot.
4. Inspect the image or GIF and state what visible acceptance condition passed or failed.
5. Upload/notify through the repository's explicit artifact path and report the command result.

Follow the consuming repository's required resolution, camera, artifact, and notification contract. A source-only or documentation-only change does not need a fabricated visual artifact; mark visual verification and any upload/notification step `N/A` with the reason.

## Failure classification

Stop at the first unproven required layer and classify it accurately:

| Class | Examples | Next action |
|---|---|---|
| Capability/schema | Action absent, loaded node type absent, field or pin not exposed. | Re-discover once; report or implement the owning Monolith capability rather than guessing. |
| Structural | Invalid edge, cycle, isolated required node, missing output connection. | Correct exact topology, then revalidate. |
| State/ownership | Component active, refresh pending, runtime-scheduler-owned, partition worker target. | Poll or target the exact owner; do not bypass the guard. |
| Semantic output | Structurally valid graph produces empty or wrong data. | Follow the layered diagnosis in `runtime-debug-performance.md`. |
| Evidence completeness | Truncated response, inaccessible output, offline-only asset view. | Obtain bounded complete live evidence or record the limitation. |
| Performance | Counts exceed budget or live benchmark regresses. | Reduce amplification/realization or run the owning profiler; do not invent a timing diagnosis. |

## Efficient completion checklist

- [ ] Live namespace and used action schemas were discovered after the latest rebuild/reconnect.
- [ ] Exact graph, actor, component, asset, class, node, and pin identities came from live read-back.
- [ ] The graph contract and acceptance invariants were written before mutation/execution.
- [ ] Unfamiliar settings were discovered; nontrivial writes were dry-run before commit.
- [ ] Structural validation used the strictness required by the actual graph contract.
- [ ] Every response field used as proof was complete, or its limitation was reported.
- [ ] Persistent mutations have save evidence and required reload proof.
- [ ] Async work was polled to idle without busy-waiting inside an action.
- [ ] Output/resource claims use complete bounded evidence.
- [ ] Determinism claims include cleanup/regeneration and controlled-input comparison.
- [ ] Final changelist/asset containment was checked after asynchronous completion.
- [ ] Required visual and artifact-notification evidence exists, or is correctly marked `N/A`.
- [ ] No graph user-parameter schema capability, per-node trace, or runtime profiler result was claimed beyond the live surface.
