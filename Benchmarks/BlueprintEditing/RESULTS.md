# BlueprintEditing Benchmark Results

## v5 status (2026-06-17): live scored run PENDING

The v5 upgrade (295 tasks: read-back-verified `edit_execute`, executed `workflow_execute`,
input-specific `error_path`, first-call-gated `duplicate_reject`, expanded schema/read coverage,
fixture-name/path `type_discovery`, and fixture lifecycle preflight) changes what the score
*means*: the previous v4 `blueprint_editing_score = 1.000` was reachable by a server that
returned well-formed success envelopes without truly performing/confirming edits — exactly the
gap v5 closes. **The v4 result below is therefore not comparable to v5; a fresh v5 baseline must
be established.**

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
