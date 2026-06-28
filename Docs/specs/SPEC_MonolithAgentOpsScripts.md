# Monolith — Agent Ops Scripts (MCP Recovery, Index Freshness)

**Parent:** [../SPEC_CORE.md](../SPEC_CORE.md)
**Status:** Implemented and verified 2026-06-11
**Scope:** `Scripts/recover_mcp.ps1`, `Scripts/check_index_freshness.ps1`; the offline invocation-log reader contract lives in [SPEC_MonolithToolInvocationLogs.md](SPEC_MonolithToolInvocationLogs.md)
**Created:** 2026-06-11

---

## 1. Purpose

Two recurring agent workflows were documented only as prose (CLAUDE.md sections 12 and 14, `Skills/monolith-mcp/SKILL.md`): recovering the editor-backed MCP endpoint, and walking the index health -> repair -> re-verify chain. Both are fragile, order-dependent shell sequences — the case where the Agent Skills guidance prefers a deterministic script over re-derived instructions. These scripts give every agent the same floor: one invocation, line-oriented `RESULT=` output, and documented exit codes.

They intentionally do not wrap live MCP namespace actions. Agents call MCP actions through their MCP client; the runtime catalog stays the only source of truth for action names and schemas.

Both scripts are Windows-host helpers (they drive `RunHeadlessEditor.bat` and `Binaries/monolith_query.exe`, which are Windows surfaces in this checkout). The invocation-log reader is cross-platform Python.

## 2. Scripts

| Script | Purpose | Writes anything? |
|---|---|---|
| `Scripts/recover_mcp.ps1` | Probe MCP `/health`; when down, launch the host project's headless editor wrapper and wait for the endpoint | No repo/DB writes; launches an editor process |
| `Scripts/check_index_freshness.ps1` | `source`/`project` health -> stale detection -> repair recommendation; `-Execute` runs warning-indicated repairs and re-verifies | `-Execute` writes `Saved/EngineSource.db` / `Saved/ProjectIndex.db` through `monolith_query.exe` |
| `Scripts/prune_invocation_logs.ps1` | Retention pruning for `Logs/yyyyMMdd` folders (age and/or total-size rules); dry-run by default | `-Execute` deletes pruned date folders |
| `Analyzer/analyze_invocation_logs.py` | Offline reader for daily invocation logs (contract: [SPEC_MonolithInvocationLogAnalyzer.md](SPEC_MonolithInvocationLogAnalyzer.md)) | Writes reports under `Saved/Monolith/LogAnalysis/` only |

## 3. `recover_mcp.ps1` Contract

Sequence: GET `<McpUrl with /health>` (3s timeout, 200 = up) -> resolve host checkout root (`-ProjectRoot`, else walk up from the script until a `*.uproject` is found) -> require `Build/BatchFiles/RunHeadlessEditor.bat` -> launch unless a real editor instance already exists -> poll `/health` until 200 or `-TimeoutSec`.

| Parameter | Default | Notes |
|---|---|---|
| `-McpUrl` | `MONOLITH_URL` env var, else `http://localhost:9316/mcp` | Same endpoint resolution as `Scripts/monolith_proxy.py` |
| `-TimeoutSec` | 600 | Editor boot budget after the launch step |
| `-PollIntervalSec` | 5 | `/health` probe interval |
| `-ProbeOnly` | off | Report up/down only; never launches |
| `-ForceLaunch` | off | Launch even when editor-server candidate processes exist |
| `-ProjectRoot` | upward search | Explicit host checkout root |

Behavior notes:

- The duplicate-launch guard inspects process command lines: `UnrealEditor.exe` / `UnrealEditor-Cmd.exe` instances running with `-game` or `-server` can never bind the MCP port and are not counted as a booting editor. Process existence comes from `Get-Process` (CIM alone can transiently report a live process as missing); CIM only classifies command lines, and a process whose command line cannot be read stays a candidate.
- The wrapper is started through `Start-Process` with its own hidden console and the wrapper process is awaited with .NET `WaitForExit` (60s bound). Piping the wrapper would block on the stdio handles the backgrounded editor inherits; sharing this script's console group would forward a later Ctrl/kill of the script's tree to the editor as `ConsoleCtrl`; and Windows PowerShell 5.1's `Start-Process -Wait` waits on the whole descendant tree — including the backgrounded editor — which also exposes the editor to the caller's process-tree kill.
- While polling, the loop watches the editor-server candidate process set; two consecutive empty samples (debounced against transient CIM misses) stop the script early with `RESULT=EDITOR_EXITED` (exit 6) and the newest editor log path instead of waiting out the full timeout.
- When the wrapper is missing, the script reports `RESULT=BLOCKED` and stops. It never substitutes a direct `UnrealEditor.exe` launch; `Build\BatchFiles\RunHeadlessEditor.bat` owns engine resolution and launch arguments.
- On timeout it prints the newest `Saved/HeadlessMcp/Logs/HeadlessEditor-*.log` path for inspection.
- MCP client configuration is untouched; the existing Monolith proxy detects the server transition (`/health` poll) and refreshes its tool list. Re-run `monolith_status()` before namespace actions.

| Exit code | Meaning |
|---|---|
| 0 | Endpoint up (already up, or came up after launch) |
| 2 | Down and `-ProbeOnly` was requested |
| 3 | Blocked: host root or `RunHeadlessEditor.bat` not found |
| 4 | Blocked: wrapper exited non-zero |
| 5 | Timeout waiting for `/health` |
| 6 | Editor process exited before `/health` answered (crash/shutdown; the newest editor log path is printed) |

## 4. `check_index_freshness.ps1` Contract

Sequence per namespace (`-Target source|project|all`, default all): `monolith_query.exe <ns> health --include-counts=true` -> parse `status` / `warnings` -> extract the repair hint each warning itself carries (`... -> repair_crg_cache` format) -> print exact offline and live repair commands. With `-Execute`: run the deduplicated warning-indicated repairs (`repair_crg_cache`, `repair_fts` only) through the offline CLI, then re-run health and print `VERIFY db=<ns> before=<status> after=<status>`.

| Parameter | Default | Notes |
|---|---|---|
| `-Target` | `all` | `source`, `project`, or `all` |
| `-Execute` | off | Run warning-indicated repairs instead of only reporting |
| `-AllowLiveEditor` | off | Permit `-Execute` while the MCP `/health` endpoint answers |
| `-QueryExe` | `<plugin>/Binaries/monolith_query.exe` | Override for copied binaries |
| `-McpUrl` | `MONOLITH_URL` env var, else `http://localhost:9316/mcp` | Used only to detect a live editor |

Behavior notes:

- Only the repair the health warning itself names is ever auto-run. Health problems without a `repair_*` hint (missing DBs, schema errors) are reported and left to indexing/bootstrap paths — the script does not mask missing data with substitutes.
- `-Execute` against a live editor endpoint is refused (`RESULT=REFUSED`, exit 6) unless `-AllowLiveEditor` is passed; the printed live alternative (`<ns>_query("repair_crg_cache", { "execute": true })`) is the preferred path while the editor is up.
- `repair_fts` maps to `project repair_fts --target=all --execute` for project and `source repair_fts --execute` for source.

| Exit code | Meaning |
|---|---|
| 0 | All checked indexes ok (or repaired back to ok) |
| 2 | Warnings or health errors found (report mode, or no runnable repair) |
| 3 | Blocked: `monolith_query.exe` missing |
| 4 | `-Execute` ran but warnings remain |
| 6 | `-Execute` refused: MCP endpoint up without `-AllowLiveEditor` |

## 5. `prune_invocation_logs.ps1` Contract

Retention for the daily JSONL logs stays manual (SPEC_MonolithToolInvocationLogs.md section 11); this script is the manual entry point. It considers only folders named exactly `yyyyMMdd` under the log root — loose files and other folders are never touched — and removes the oldest folders outside the retention rules.

| Parameter | Default | Notes |
|---|---|---|
| `-LogRoot` | `<plugin>/Logs` | Log root containing date folders |
| `-KeepDays` | 30 | Keep folders newer than N days; 0 disables the age rule |
| `-MaxTotalMB` | 0 (off) | After the age rule, prune oldest until total size fits |
| `-Execute` | off | Without it, dry run: `PRUNE ... dry_run` lines + `RESULT=DRY_RUN`, exit 2 |

| Exit code | Meaning |
|---|---|
| 0 | Nothing to prune, or pruning completed |
| 2 | Dry run found prunable folders (re-run with `-Execute`) |
| 3 | Blocked: log root missing |

## 6. Verification Record (2026-06-11)

| Gate | Result |
|---|---|
| Freshness report mode | Detected the live ProjectIndex CRG parity drift (`project native=10000 crg_nodes=8510`, `valid native edges=260 crg_edges=261`), printed deduplicated offline + live repair commands, exited 2; source reported ok. |
| Freshness execute mode | With the MCP endpoint down, ran `project repair_crg_cache --execute` once (deduplicated from two warnings), re-ran health, reported `VERIFY db=project before=warning after=ok` and `RESULT=REPAIRED`, exited 0. |
| Recovery probe mode | With no MCP server, `-ProbeOnly` reported `RESULT=MCP_DOWN` and exited 2. |
| Recovery guard precision | A running `UnrealEditor.exe <project> -game` instance (which never hosts the editor MCP server) was correctly excluded from the duplicate-launch guard via command-line inspection. |
| Recovery launch + wait | `recover_mcp.ps1` launched `Build/BatchFiles/RunHeadlessEditor.bat` from the resolved host root via detached `Start-Process`, printed the new editor log path, and polled `/health` with 30s `WAITING` heartbeats; `RESULT=MCP_TIMEOUT` (exit 5) and `RESULT=EDITOR_EXITED` (exit 6, detected ~8s after the editor died) were both exercised live. |
| Recovery success path | Verified against a stub `/health` HTTP server: parsed and reported `version`/`tools_registered`/`pid`/`uptime_seconds`, printed the reconnect guidance line, ended with `RESULT=MCP_UP` and exit 0. Headless editor logs from the same session show the real Monolith server binding 9316 at +17s and +7s after wrapper launch, so the live success path is reachable whenever the editor survives boot. |
| Environment blocker (not a script defect) | During verification the Go checkout's headless editor crash-looped ~10s after boot in `FTabManager::SavePersistentLayout` -> `FGenericWindow::GetRestoredDimensions` (`GenericWindow.cpp:113`, engine NullRHI layout-save crash; observed identically across three boots; one earlier boot survived 7 minutes). Until that project/engine issue is fixed, launch-based recovery on that machine state ends in `RESULT=EDITOR_EXITED`. |
| Live-editor refusal path | Not exercised end-to-end in this pass (no pending warnings remained after repair); the gate is enforced by the `RESULT=REFUSED` branch before any repair invocation. |

2026-06-12 follow-up verification:

| Gate | Result |
|---|---|
| Recovery end-to-end with the headless layout-save guard | `recover_mcp.ps1` launched the wrapper; the guarded editor bound MCP at +9s, logged `HeadlessLayoutSaveGuard: CanEverRender=false ...`, and stayed alive past 9 minutes serving actions (pre-guard boots died at 10-20s in the `GenericWindow.cpp:113` layout-save fatal; see SPEC_MonolithEditor.md "Headless layout-save guard"). |
| Recovery success path against the real server | `-ProbeOnly` against the live endpoint reported `version=0.18.1 tools_registered=1722` and ended `RESULT=MCP_UP` with exit 0. A subsequent full launch run (WaitForExit wrapper wait) went launch -> poll -> `RESULT=MCP_UP elapsed_seconds=13` with exit 0 in one invocation, leaving the editor detached from the caller's process tree. |
| Death-detection false positive fixed | A transient empty `Win32_Process` CIM sample had produced `RESULT=EDITOR_EXITED` while the editor was alive and logging; existence now comes from `Get-Process` with a two-sample debounce. |
| Wrapper wait correctness | `Start-Process -Wait` was replaced with .NET `WaitForExit(60s)`: under Windows PowerShell 5.1, `-Wait` waited on the backgrounded editor as a descendant, hanging the script and exposing the editor to the caller's process-tree kill (this, not an engine fault, explained one boot's silent death minutes after launch). |
| Retention dry runs | `prune_invocation_logs.ps1` default (`-KeepDays 30`) kept all 24 date folders and exited 0; `-KeepDays 15` listed 8 prunable folders / 96.25 MB reclaimable, exited 2, and deleted nothing; folder count unchanged afterwards. |
