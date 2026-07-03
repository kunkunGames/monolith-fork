# MCP Recover Probe Diagnostics

---

## Metadata

| Field | Value |
|---|---|
| Date | 2026-07-04 |
| Area | Monolith Agent Ops |
| Change | `recover_mcp.ps1 -ProbeOnly` reports actionable down-state diagnostics |
| Source | `Saved\Monolith\SessionAnalysis\roi-20260703\summary.md` and `Saved\Monolith\LogAnalysis\roi-20260703\summary.md` |

---

## 1. Purpose

The 2026-07-03 session analysis showed transport/availability failures as the largest client-observed Monolith MCP failure class. Before this change, `recover_mcp.ps1 -ProbeOnly` collapsed every down-state into a bare `RESULT=MCP_DOWN`, leaving no local evidence about health failure type, MCP-port ownership, editor boot candidates, or whether the watchdog/recover path was the right next action.

---

## 2. Verification

| Step | Command | Result |
|---|---|---|
| PowerShell parse | `[System.Management.Automation.Language.Parser]::ParseFile('Plugins\Monolith\Scripts\recover_mcp.ps1', ...)` | Passed; no parser errors. |
| Probe-only diagnostics | `powershell -NoProfile -ExecutionPolicy Bypass -File Plugins\Monolith\Scripts\recover_mcp.ps1 -ProbeOnly; Write-Output ("LASTEXITCODE={0}" -f $LASTEXITCODE)` | Passed; output included `RESULT=MCP_DOWN`, `reason=timeout`, `listener_port=9316`, `listener_count=0`, `editor_candidate_count=0`, `headless_candidate_count=0`, `next_action=run_watch_mcp_or_recover_mcp`, and `LASTEXITCODE=2`. |
| Screenshot / Discord upload | N/A | Script diagnostics only; no visual, gameplay, UI presentation, VFX, animation, material, or asset-presentation change required screenshot capture or Discord upload. |

---

## 3. Result

Passed. The ProbeOnly down-path still exits 2 and never launches, builds, recovers, or runs index maintenance, but the result line now contains the local evidence needed to choose the next recovery action.
