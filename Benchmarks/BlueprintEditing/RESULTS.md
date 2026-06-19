# BlueprintEditing Benchmark Results

## v5.1 (2026-06-18) — adversarial hardening + practical expansion

The v5.1 changeset hardened the scoring engine against gaming, roughly doubled executed
coverage, re-weighted away from schema-fetch, and fixed three benchmark defects that a live
baseline run surfaced. See `METRICS.md` → v5.1 note for the full list. Task count 295→305,
10 categories (new `negative_compile`).

### Live baseline (pre-hardening), v5 task set

| Field | Value |
|-------|-------|
| Label | `baseline-v5-pre` |
| Run (UTC) | 2026-06-17T23:5x (local 2026-06-18 08:5x KST) |
| MCP version | `0.20.2` |
| Engine | `++UE5+Release-5.7-CL-51494982` |
| Project | `GO` |
| Tasks run | 295 |
| `blueprint_editing_score` | **0.967** |

Per-dimension: `edit_schema`/`graph_read`/`error_path`/`read_schema`/`type_discovery`/
`duplicate_reject` = 1.000; `edit_execute` 0.942; `variable_read` 0.929; `workflow_execute`
0.909. **All 9 failures were benchmark/contract defects, not server-capability gaps:**

| Failure | Root cause | Fixed in v5.1 by |
|---------|-----------|------------------|
| `get_variables`/`set_variable_defaults` on `bComponentActive` (×2) | `bIsActive` fixture var collided with `UActorComponent`'s native `bIsActive` (silently not created) | renamed fixture var `bComponentActive` |
| `get_interface_functions` (×1) | required param `interface_class` not supplied | pass the interface `/Game` path + resolver fix |
| `implement_interface` workflow (×1) | short name `BPI_TestInterface` unresolvable | full `/Game` path + interface-resolver fix |
| `validate_blueprint` (×4) | lint-report shape (`node_errors`…) text-scanned as a failed compile | `_compile_is_clean` reads the hard-error lists |
| `add_node` FormatText (×1) | read-back token `"Widget {Label}"` not in `get_graph_data` | read back the returned node id (`${self}`) |

A near-1.0 v5 score with these defects shows the **server** is healthy; the score was depressed
by benchmark measurement bugs. v5.1 removes those, then raises the bar so the score reflects
genuine mutation rather than envelope health (NOT comparable to v5 — establish a fresh baseline).

### v5.1 live scored run

| Field | Value |
|-------|-------|
| Label | `v5.1` |
| Run (UTC) | 2026-06-18 |
| MCP version | `0.20.2` (MonolithBlueprint DLL rebuilt with the interface-resolver fix) |
| Engine | `++UE5+Release-5.7-CL-51494982` |
| Project | `GO` |
| Tasks run | 305 |
| Transport errors | 0 |
| `blueprint_editing_score` | **1.000** |

All ten dimensions scored **1.000**: `edit_execute`, `edit_schema`, `graph_read`, `variable_read`,
`error_path`, `workflow_execute`, `duplicate_reject`, `negative_compile`, `read_schema`,
`type_discovery`. A clean run of the hardened suite on the benchmark's own fixtures: the live editor
performs and read-back-confirms real edits (delete-first), wires exec and data pins, compiles clean,
rejects invalid input by the offending identifier, guards duplicate names, and reports a real compile
error on the deliberately-broken `negative_compile` scratch blueprint. Because the v5.1 hardening
drops the no-edit stub ceiling to ≈0.22, a 1.000 here *means* genuine Blueprint-authoring capability,
not envelope health.

**Comparison vs `baseline-v5-pre` (`compare/comparison.md`):** `blueprint_editing_score`
0.967 → 1.000 (**+0.033**); `edit_execute` +0.058, `variable_read` +0.071, `workflow_execute`
+0.091; `negative_compile` is new in v5.1. The score *rose* despite the suite getting strictly
harder, because the benchmark/contract defects that depressed the baseline (the `bComponentActive`
fixture collision, the `validate_blueprint` shape mis-score, and unresolvable interface identifiers)
are fixed — the last backed by the Monolith `ResolveInterfaceClass` handler fix.

**Process notes (reproducibility):** the headless `-nullrhi` editor crashes intermittently under
heavy mutation; the scored run is gated on **zero transport errors** (a crash mid-run is rejected and
retried on a fresh boot). `setup_fixtures` now `save_asset`s each fixture so new entities survive a
reboot. Running the suite twice on the SAME editor instance accumulates in-memory test entities; run
once per boot (or reset fixtures) for a clean number — see `Docs/TODO.md` (fixture-growth item).

---

## v5 status (2026-06-17): superseded by v5.1

The v5 upgrade (295 tasks: read-back-verified `edit_execute`, executed `workflow_execute`,
input-specific `error_path`, first-call-gated `duplicate_reject`, expanded schema/read coverage,
fixture-name/path `type_discovery`, and fixture lifecycle preflight) changed what the score
*means*: the previous v4 `blueprint_editing_score = 1.000` was reachable by a server that
returned well-formed success envelopes without truly performing/confirming edits — exactly the
gap v5 closes. **The v4 result below is therefore not comparable to v5/v5.1.**

The v5 scoring logic was **validated offline** against mock servers (no live editor required):

| Mock server | Result |
|-------------|--------|
| Faithful editor (mutations take effect, reads reflect them, invalid input rejected, duplicate guard present) | Passes every sampled category (`edit_execute`, `workflow_execute`, `error_path`, `duplicate_reject`, …) |
| No-op stub (returns success envelopes but never mutates; reads come back empty) | **Blocked on every read-back-dependent task** — fails `edit_execute`, the wiring `op_chain`, `workflow_execute`, `error_path`, and `duplicate_reject` |

The live scored run is still pending a healthy editor/MCP endpoint. Use `preflight` first:
`transport_error` means there is no usable live MCP response; `fixture_missing_or_invalid` means
the endpoint is alive but a fixture asset is absent/invalid; `fixture_contract_missing` means a
fixture exists but setup-created variables/functions are missing. All response-shape dependencies
the read-back logic relies on are recorded in `METRICS.md` → Catalog Version. **Re-run once
preflight passes** (see Run Command below).

## v4 result (2026-06-16, superseded — not comparable to v5)

| Field | Value |
|-------|-------|
| Label | `current` (v4) |
| Run timestamp (UTC) | 2026-06-16T19:05:13Z |
| MCP version | `0.20.2` |
| Engine | `++UE5+Release-5.7-CL-51494982` |
| Project | `GO` |
| Tasks run | 191 |
| `blueprint_editing_score` | 1.000 (v4 weights; reachable without true read-back — the v5 defect class) |

## Run Command (v5)

```powershell
# 1. Bring the editor / MCP endpoint up first if it is down:
#    D:\P4\game\BatchFiles\RunHeadlessEditor.bat   (wait for localhost:9316 to listen)
# 2. Check endpoint + fixture readiness. This fails fast with transport_error,
#    fixture_missing_or_invalid, or fixture_contract_missing:
python Scripts\blueprint_editing_benchmark.py preflight --mcp-url http://localhost:9316/mcp
# 3. Create / verify fixtures (now creates typed vars and setup function stubs for all 7 fixtures):
python Scripts\blueprint_editing_benchmark.py setup_fixtures --mcp-url http://localhost:9316/mcp
# 4. Run the v5 benchmark. run performs readiness preflight by default:
python Scripts\blueprint_editing_benchmark.py run `
  --mcp-url http://localhost:9316/mcp `
  --tasks Benchmarks\BlueprintEditing\tasks.jsonl `
  --label v5 `
  --output-dir Saved\Monolith\Benchmarks\BlueprintEditing\v5
```

Expected on a healthy live editor: 295 tasks loaded; `edit_execute_rate`,
`workflow_execute_rate`, `error_path_rate`, and `duplicate_reject_rate` at or near 1.0 (the
benchmark's own fixtures and the live `blueprint` handlers satisfy every read-back). A near-1.0
here now *means* the server actually performs, wires, compiles-clean, rejects bad input, and
guards duplicates — not merely that it replied without error.
