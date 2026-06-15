# MCP Availability from Client Session Transcripts (6C verification)

**Date:** 2026-06-14
**Spec:** [../specs/SPEC_MonolithToolCallReliabilityBacklog.md](../specs/SPEC_MonolithToolCallReliabilityBacklog.md) §6C
**Tool under test:** `Analyzer/analyze_session_transcripts.py` (new, read-only)
**Purpose:** Measure the client-side Monolith MCP failures that the server-side invocation-log analyzer is structurally blind to (calls that die at the transport before reaching the editor).

---

## 1. What was built

A read-only sibling of `analyze_invocation_logs.py` that parses agent client rollouts and classifies **structured tool-result payloads only** (Codex `function_call_output`, Claude `tool_result`) for Monolith MCP tool calls into `transport_availability` vs `server_captured` vs `other`, then emits an `mcp_availability` finding. It never grep-scans raw transcript text, so it is immune to the injected-`CLAUDE.md` contamination (the instruction text contains `9316`, `recover_mcp`, `ECONNREFUSED`).

Supports `--codex-root`, `--claude-root`, `--since/--until` (dated Codex sessions), `--out`, `--include-samples` (bounded + redacted). Writes `summary.md` + `findings.json` under `Saved/Monolith/SessionAnalysis/<stamp>`; never writes under `Logs/`, never opens SQLite.

## 2. Commands

```powershell
python -m py_compile Analyzer/analyze_session_transcripts.py        # OK
python Analyzer/analyze_session_transcripts.py --since 20260520 --include-samples --stamp goal-20260614
```

Output: `Saved/Monolith/SessionAnalysis/goal-20260614/{summary.md,findings.json}`.

## 3. Result

| Metric | Value |
|---|---:|
| Session files scanned | 1,180 |
| Monolith tool results | 376 (Codex 375, Claude 1) |
| Sessions using Monolith | 26 |
| Errors | 137 (36.4%) |
| **`transport_availability` (server-blind)** | **136 (99.3% of errors)** |
| Sessions with a transport failure | 20 of 26 |
| `server_captured` | 0 |
| `other` | 1 |

Transport markers (from tool results only): `transport send error` ×135, `timed out` ×1.

By day (dated Codex sessions): 20260521=29, 20260522=33, 20260527=4, 20260529=9, 20260604=7, 20260605=1, 20260606=42, 20260607=11.

## 4. Findings

1. **Codex is the primary Monolith MCP client**; Claude Code in these checkouts barely calls Monolith MCP (1 result).
2. **The dominant client-side Monolith failure is endpoint availability, not schema** — 99.3% of client-observed errors are `Transport send error` to `localhost:9316`, and **none** appear in server-side `action.jsonl` (the call dies before the editor logs it). This is the largest agent-facing Monolith failure class measured anywhere.
3. **It overturns an earlier crude `grep` estimate** (which substring-matched "error"/"failed"/"9316" inside *successful* `monolith_status` payloads and wrongly suggested ~91% schema errors). Confirms the spec's caveat that only parsed tool-result payloads are trustworthy.

## 5. Correctness checks

- **No dropped server-captured errors:** scanned the 05–06 Codex monolith outputs for server-rejection phrases (`Missing required param`, `Unknown namespace`, `No symbol/file found`, `-3260x`) that lack a tool-failure marker → **0**, so the reader is not silently undercounting server-side errors.
- **No false positives from success payloads:** the reader gates classification on `result_is_error` (Codex `tool call error`/`Transport send error`/`isError:true`), and `TRANSPORT` no longer matches bare `9316` (which occurs in successful `monolith_status` output as `"server_port":9316`). A first draft that matched bare `9316` / loose `error` reported 1 error (under-count) then 368 (over-count); the gated version reports 137, verified against raw structure.
- **Compile:** `python -m py_compile Analyzer/analyze_session_transcripts.py` passes.

## 6. Limitations / follow-ups

- Server-captured errors that return as *normal* results carrying an error field (no tool-failure marker) are intentionally not counted here — they are already visible server-side via `analyze_invocation_logs.py`.
- Not yet captured: post-failure recovery behavior (reconnect vs retry vs `editor.run_python` fallback) and correlation of transport bursts with editor restart/build windows. Tracked in spec §6C.
- The measurement points to the real open work: harden MCP endpoint uptime / reconnect at `localhost:9316` (`CLAUDE.md` §14) — now spec §7 P1 "top agent item".
