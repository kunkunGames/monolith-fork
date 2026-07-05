# P0-1 MCP Availability — Post-Watchdog Fresh Measurement + Root-Cause Split

| Field | Value |
| --- | --- |
| Date | 2026-07-05 |
| Project | `D:\P4\speed\Speed.uproject` |
| Scope | Re-measure client-observed MCP availability after the 2026-07-03 watchdog landing + PR A0 re-arm; decompose residual transport failures; state honest attribution limits |
| Tool | `Analyzer\analyze_session_transcripts.py` (structured tool-result payloads only) |
| Out | `Saved\Monolith\SessionAnalysis\post-watchdog-20260704\` |

---

## 1. Measurement

Command:

```powershell
python Analyzer\analyze_session_transcripts.py `
  --codex-root $HOME\.codex\sessions `
  --claude-root $HOME\.claude\projects `
  --since 20260703 `
  --out Saved\Monolith\SessionAnalysis\post-watchdog-20260704 --include-samples
```

| Metric | Post-watchdog (since 20260703) | Baseline (roi-20260703, plan §3.1) |
| --- | ---: | ---: |
| Session files scanned | 2,588 | — |
| Monolith tool results | 266 across 44 sessions | — |
| Errors (rate) | 36 (0.135) | — |
| **Transport / server-blind** | **1 (0.028 of errors)** | **246 across 48 sessions (≈87% of client errors)** |
| Server-captured (already in `action.jsonl`) | 16 | — |
| Other | 19 | — |

By source:

| Source | Calls | Errors | Transport | Server-captured | Other |
|---|---:|---:|---:|---:|---:|
| claude (proxy-routed) | 218 | 35 | **0** | 16 | 19 |
| codex (direct 9316) | 48 | 1 | 1 | 0 | 0 |

## 2. Root-cause split (the five failure classes from plan §3.1 / DoD 7.1)

| Class | Status | Evidence |
|---|---|---|
| editor process dead | **covered** | Watchdog scheduled task `Monolith MCP Watchdog - Speed` is `Running` with **both** an `AtLogOn` trigger and the PR A0 30-min re-arm `MSFT_TaskTimeTrigger` (verified live 2026-07-05). Single-instance process tree (`monolith_watchdog.exe` 22512 → child 61708 → `powershell.exe` 59828 running `watch_mcp.ps1`); `MultipleInstances IgnoreNew` held (no duplicate instance). 43 recovery-class UBT rebuilds under `Saved\Monolith\Watchdog\UBT-*.log` across 07-03..05 (7 / 35 / 8 by day; the 07-04/05 counts include operator-triggered builds during this task, so treat as an upper bound, not a pure watchdog-recovery count). |
| headless alive but `/health` unhealthy | **covered, but over-broad — NEW FINDING** | The recover path kills a `-NullRHI` editor that leaves `9316` unhealthy. During this task it also killed a **legitimate `-NullRHI` automation/commandlet run** (`Automation RunTests`, no MCP server, distinct `AbsLog`): the run ended mid-init with no `Engine exit requested` / `ConsoleCtrl` shutdown marker — an external kill. So the heuristic cannot distinguish "unhealthy MCP editor" from "a non-MCP headless editor that was never meant to serve 9316." Recorded as a `Docs\TODO.md` follow-up; workaround is to stop the watchdog around commandlet/automation runs. |
| non-headless editor alive, `9316` flicker | **partially root-caused, open** | The ConsoleCtrl down-class was root-caused 2026-07-04 (plan §0.1-1): closing the visible watchdog/console window propagates `ConsoleCtrl RequestExit` to the recovered editor, taking both down. Fix (process-group separation) tracked in TODO. |
| Codex direct HTTP send failure | **open** | Native `monolith_proxy.exe` has not received the send-side retry/backoff that the Python/JS proxy got in CL 722. Codex-direct-9316 is the exact path that produced the baseline 246 transport failures. |
| proxy path failure | **covered** | CL 722 send-side retry self-heals flicker for proxy-routed clients. This window: **0 transport failures across 218 Claude (proxy) Monolith calls** — the strongest positive signal. |

## 3. Honest attribution limits

The transport-failure count dropped from ≈246 (87% of client errors) to 1 (2.8% of errors), but this is **not cleanly attributable to endpoint health alone**:

1. **Codex call-volume collapse.** Codex issued only 48 Monolith calls in this window vs thousands in the baseline. The baseline transport failures were overwhelmingly Codex-direct-9316; far fewer Codex calls structurally yields far fewer Codex-direct transport failures, independent of any endpoint improvement.
2. **ConnectionRefused undercount (analyzer `measurement_caveat`, emitted since PR A).** A client that cannot connect at the MCP handshake produces **no tool result**, so it is invisible to this analyzer. A low transport count in a low-Codex-call window is therefore not proof of endpoint health.
3. **What IS well-supported:** the proxy-routed path (Claude Code, this session's own path) saw 0/218 transport failures, and the watchdog + re-arm trigger is demonstrably live and recovering editor deaths. So the *proxy* availability improvement is real; the *Codex-direct* path improvement is unproven pending native-proxy retry + a window with comparable Codex call volume.

## 4. Verdict and residual work

- **Proxy-routed availability: materially improved** (0/218 this window; CL 722 + watchdog supervision).
- **Codex-direct availability: unproven** — needs native `monolith_proxy.exe` send-retry, then re-measure in a comparable-volume window.
- **New watchdog over-kill finding:** the `-NullRHI` kill heuristic must exempt non-MCP headless editors (match on the `Saved\HeadlessMcp` MCP-editor signature / 9316 ownership, not on `-NullRHI` alone). Filed in `Docs\TODO.md`.
- **DoD 7.1 status:** transport/session ratio dropped (gate 1 met with the volume caveat); failures are decomposed into the five classes (gate 2 met); a client-side recovery path exists (proxy retry — gate 3 met). Remaining: native-proxy retry as a second recovery path for the Codex-direct class.
