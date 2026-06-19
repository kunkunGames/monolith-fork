# AICapability Metrics

Scoring rules for each category, the composite formula, and the **verified** `ai` action / param /
error-shape evidence the scorers depend on. The single source of truth for the weights is the
`WEIGHTS` dict in `Scripts/ai_capability_benchmark.py`; this document and `manifest.json` are
derived from it and must not drift.

## Composite score

```
ai_capability_score =
    0.34 * edit_execute_rate
  + 0.16 * error_path_rate
  + 0.12 * compile_gate_rate
  + 0.10 * duplicate_reject_rate
  + 0.10 * edit_schema_rate
  + 0.10 * discovery_rate
  + 0.08 * read_schema_rate
```

Weights sum to 1.0 (asserted at import). Each `*_rate` is the fraction of that category's tasks with
`direct_success == True`. A weighted category with zero tasks is a build-time error
(`build_static_tasks`) and a run-time warning (`aggregate`), so a dropped category can never silently
cap the composite while the sum-to-1.0 assert still passes.

## Task inventory (generated)

| Category | Tasks | Weight |
| --- | ---: | ---: |
| `edit_execute` | 9 | 0.34 |
| `error_path` | 8 | 0.16 |
| `compile_gate` | 2 | 0.12 |
| `duplicate_reject` | 4 | 0.10 |
| `edit_schema` | 28 | 0.10 |
| `discovery` | 6 | 0.10 |
| `read_schema` | 16 | 0.08 |
| **Total** | **73** | **1.00** |

`edit_schema` subsystems: blackboard 8, behavior_tree 12, state_tree 1, eqs 7.

> **StateTree surface — compiled out (`WITH_STATETREE=0`).** The `ai` namespace registers the
> StateTree actions `create_st_from_template`, `lint_state_tree`, `runtime_get_st_active_states`,
> `runtime_send_st_event`, `get_crowd_lane_state`, `set_crowd_lane_state` in the catalog (verified
> against `Saved/Monolith/LogAnalysis/_ai_catalog.txt`, 182 ai actions), but on **this build
> StateTree is compiled out (`WITH_STATETREE=0`)**, so `create_st_from_template` and
> `lint_state_tree` are **runtime stubs** that return `isError` "StateTree module not available
> (WITH_STATETREE=0)". No StateTree EXECUTE or lint task can pass on this build. There is also no
> granular StateTree authoring action (`create_state_tree` / `add_st_state` / `compile_state_tree` /
> `add_st_task` / `add_st_transition` / `*_st_state` / `get_state_tree` / `list_state_trees` /
> `validate_state_tree`). StateTree is therefore covered **only** by schema-presence tasks — one
> `edit_schema` (`create_st_from_template`) and one `read_schema` (`lint_state_tree`), which assert
> the action is registered in the catalog (true even when stubbed). It drives **no** `edit_execute`,
> `error_path`, or `compile_gate` task; the falsifiable gate and the BT error_path use Behavior Tree
> validate actions, which DO execute.

## Per-category scoring

### `edit_execute` (read-back verified) — weight 0.34

Each task runs a real `ai` edit (often delete-first so the read-back proves THIS run made the edit),
capturing returned ids into `${label}`, then a **follow-up read must observe the end state**:

- `add_bb_key` / `batch_add_bb_keys` → `get_blackboard` must `contain` the new key name(s).
- `rename_bb_key` → `get_blackboard` must `contain` the new name and have the old name `absent`.
- `remove_bb_key` (add→remove round-trip) → `get_blackboard` must have the name `absent`.
- `add_bt_node` → `get_bt_graph` must `contain` the captured `node_id` (proves THIS node exists; not
  satisfiable by a same-typed leftover). Composite+task variant captures the parent and reads both.
- `set_bt_blackboard` → `get_behavior_tree` must `contain` the linked Blackboard name.
- `add_eqs_generator` → `get_eqs_query` must `contain` the generator class.

A non-error envelope is **necessary but not sufficient** — the read-back gates correctness, so a
silent no-op edit scores 0. (`integrity` enforces every `edit_execute` verify carries a real
assertion verb, never an empty `contains:[]`.)

There is **no StateTree `edit_execute`**: `create_st_from_template` and its `lint_state_tree`
read-back are stubbed on `WITH_STATETREE=0` (return `isError` "StateTree module not available"), so
no StateTree mutation can be read back on this build. StateTree is covered by schema tasks only.

### `error_path` (inverted + input-specific) — weight 0.16

Every task sends a **real action with real param names** against an existing fixture but an invalid
identifier. Pass = a structured `isError` whose message references the **offending identifier**
(`specific_tokens`). A reject-everything server whose canned message contains the generic words
(`error_tokens`) but never echoes the bad identifier is recorded as `generic_only` and **fails**. A
silent success on invalid input also fails. There is **no StateTree error_path** — `lint_state_tree`
is stubbed on `WITH_STATETREE=0`; the StateTree-missing probe is replaced by a
`validate_behavior_tree`-on-missing-asset probe, which DOES execute and echoes the path. The 8
offending identifiers, with the verified handler wording each is expected to echo (`validate_` and
`get_behavior_tree` share the `NONEXISTENT_BT_ZZZZ` asset token but are distinct actions):

| Action | Offending identifier | Verified error wording (file:line) |
| --- | --- | --- |
| `get_blackboard` | `NONEXISTENT_BB_ZZZZ` (asset) | `Asset not found: %s` (`MonolithAIBlackboardActions.cpp:618`) |
| `remove_bb_key` | `NONEXISTENT_BBKEY_ZZZZ` | `Key '%s' not found in blackboard ...` (`...BlackboardActions.cpp:839`) |
| `rename_bb_key` | `NONEXISTENT_RENAMEKEY_ZZZZ` | `Key '%s' not found` (`...BlackboardActions.cpp:883`) |
| `validate_behavior_tree` | `NONEXISTENT_BT_ZZZZ` (asset) | `BehaviorTree not found: %s` (`MonolithAIInternal.cpp:65`) |
| `add_bt_node` | `NONEXISTENT_BTNodeClass_ZZZZ` | `BT node class not found: %s` (`...BehaviorTreeActions.cpp:2283`) |
| `remove_bt_node` | `NONEXISTENT_BTNODE_ZZZZ` | node-not-found (BT node lookup; `...BehaviorTreeActions.cpp`) |
| `get_behavior_tree` | `NONEXISTENT_BT_ZZZZ` (asset) | `Behavior Tree not found: %s` (`...BehaviorTreeActions.cpp:2088`) |
| `add_eqs_generator` | `NONEXISTENT_EQS_ZZZZ` (asset) | EQS-query-not-found (`MonolithAIEQSActions.cpp`) |

Each fixture-asset error path echoes the asset path, which contains the unique `NONEXISTENT_*_ZZZZ`
token, so the offending identifier is present in the verified wording.

### `compile_gate` (falsifiable validate gate) — weight 0.12

StateTree lint is unavailable on `WITH_STATETREE=0` (the `lint_state_tree` action is a stub that
returns `isError`), so this gate is built entirely from the real `validate_behavior_tree` quality
action. Two complementary probes, each on its own isolated scratch Behavior Tree:

- **negative — `validate_invalid`:** create `BT_BenchEmptyScratch` via `create_behavior_tree` with
  **no nodes**. Its root has no children, so `validate_behavior_tree` emits an `error`-severity issue
  "Root has no children — empty Behavior Tree" and sets `valid == false`
  (`MonolithAIBehaviorTreeActions.cpp:4097`, severity gate at `:4149`). VERIFIED LIVE: an empty BT
  returns `{valid:false, issue_count:1, issues:[{severity:"error", ...}]}`. The gate passes only if
  `valid == False` **and** an `error`-severity issue is present — requiring the error issue (not just
  the false verdict) is the anti-reject-everything guard. A stub that always reports `valid == true`
  **fails**.
- **positive — `validate_valid`:** build `BT_BenchValidateScratch` with a `BTComposite_Selector` root
  child and a linked Blackboard. `validate_behavior_tree` sets `valid == false` only when an
  `error`-severity issue exists (`MonolithAIBehaviorTreeActions.cpp:4149`); a Selector-rooted BT with
  a Blackboard has none. VERIFIED LIVE: a clean Selector-rooted BT returns
  `{valid:true, issue_count:2 (warnings)}`. The gate passes only if `valid == True`. A reject-
  everything stub that always reports `valid == false` **fails**.

Verified shape for both: `{asset_path, valid:bool, issue_count:int, issues:[...]}`
(`MonolithAIBehaviorTreeActions.cpp:4164`). `_validate_is_valid` returns `None` (not pass) when the
gate call errors at the transport / parse / `isError` level — the asset failed to **load**, not to
produce a verdict — which is the anti-reject-everything guard: a server that simply errors on
everything cannot pass either probe. The scratch assets are isolated and harmless, so no cleanup is
required.

### `duplicate_reject` — weight 0.10

The create action is called **twice**. The first must be a clean create (a leading `setup_arguments`
delete resets a prior run's leftover, so a reject-everything server fails `first_ok`). The **second**
identical call must return a duplicate-specific `isError` (message contains one of
`already / exist / duplicate / in use / taken`). Verified duplicate wording:

| Action | Verified duplicate error (file:line) |
| --- | --- |
| `add_bb_key` | `Key '%s' already exists in this blackboard` (`MonolithAIBlackboardActions.cpp:767`) |
| `create_blackboard` / `create_behavior_tree` / `create_eqs_query` | `Asset already exists at '%s'. Delete it first or use a different path.` (`MonolithAIInternal.cpp:155`, via `EnsureAssetPathFree`) |

There is no `create_state_tree` duplicate row: the `ai` namespace has no `create_state_tree` action
(StateTree creation goes through `create_st_from_template`, which does not surface the
`EnsureAssetPathFree` duplicate wording), so StateTree is intentionally omitted from this category.

### `edit_schema` (strict) / `read_schema` (lenient) — weights 0.10 / 0.08

Both call `monolith_discover({action, mode:"schema", namespace:"ai"})`. Pass = the schema carries
`planning_signals` (non-empty list) and `skill` metadata. `edit_schema` additionally **fails on
`isError`** (e.g. an unknown/renamed action), making it a coverage tripwire: all 28 edit names and
16 read names must still resolve — and because every one of them is verified to exist in
`_ai_catalog.txt`, an `isError` here means a real regression, never an invented action. These are the
only non-executed categories and carry the least weight.

### `discovery` — weight 0.10

`list_blackboards` / `list_behavior_trees` / `list_eqs_queries` / `search_ai_assets` /
`list_bt_node_classes` must return ≥1 result **and** contain the expected token (the seeded fixture
name, or `Selector` for BT composites). Because they target the benchmark's OWN seeded fixtures, this
is portable — a capable server scores 1.0 in any project, not only where AI assets happen to exist.
`get_ai_overview` accepts 0 results (broad recall). There is no `list_state_trees` discovery probe —
the `ai` namespace exposes no such action.

## Anti-gaming properties (mirrors BlueprintEditing v5.1)

- **No green-on-empty:** discovery requires ≥1 result for the fixture-targeted queries.
- **No green-on-broken:** the four adversarial categories (0.72 weight) require observed mutations,
  the offending identifier, duplicate rejection, and a real empty-BT invalid verdict (with an error
  issue) + a real clean-BT valid verdict — none satisfiable by a schema-only or reject-everything
  server.
- **No silent category drop:** an empty weighted category is a build-time error and run-time warning.
- **Idempotent:** edits are delete-first or self-cleaning; the validate gate uses isolated scratch
  Behavior Trees created with already-exists tolerance, so re-runs stay bounded.

## Offline validation

```powershell
python Scripts/ai_capability_benchmark.py generate          # rebuilds tasks.jsonl + manifest.json
python Scripts/test_ai_capability_benchmark.py              # 14 scoring-branch asserts, no editor
```

The self-test fabricates MCP envelopes
(`{"result":{"content":[{"type":"text","text":<json>}],"isError":<bool>}}`) and confirms each NEW
failure mode scores LOW while a healthy response scores high (stub-always-valid empty-BT gate → fail,
empty-BT `valid==false` with no error issue → fail, isError validate → fail, stub-always-invalid
validate → fail, silent-no-op edit → fail, generic reject-all error → fail, silent second create →
fail; and the healthy counterparts, including the empty-BT `valid==false` + error issue → pass).
