# Verification — Reliability Backlog High-ROI Items (2026-06-15)

**Scope:** Verification record for the high-ROI open items from
[`SPEC_MonolithToolCallReliabilityBacklog.md`](../specs/SPEC_MonolithToolCallReliabilityBacklog.md)
landed 2026-06-15 (uncommitted working tree).
**Engine:** Unreal Engine 5.7 (`D:\Engine\UE_5.7`, resolved from `GO.uproject`).
**Surfaces touched:** `Analyzer/`, `Scripts/`, `.github/`, `Source/MonolithCore`, `Source/MonolithSource`, `Tools/MonolithQuery`.

---

## 1. Items and Files

| Item | Files | Spec |
|---|---|---|
| 6B analyzer recency + no-data dimension | `Analyzer/analyze_invocation_logs.py` | `SPEC_MonolithInvocationLogAnalyzer.md` §5/§7.6/§8.3 |
| Optional-module discover audit | `Source/MonolithCore/Private/MonolithCoreTools.cpp` (invariant comment) | `SPEC_MonolithToolCallReliabilityBacklog.md` §5.3 |
| 6A binary-lag CI gate | `Scripts/ci_static_checks.py`, `.github/monolith-static-ci.json` | `SPEC_MonolithQueryHelp.md` |
| 5.2 `coverage_miss` read hint | `Source/MonolithSource/Private/MonolithSourceActions.cpp`, `Tools/MonolithQuery/monolith_query.cpp` | `SPEC_MonolithSource.md` |
| 5.1 CRG cooldown gate | `Tools/MonolithQuery/monolith_query.cpp`, `Tools/MonolithQuery/monolith_query_help.h` | `SPEC_MonolithSource.md`, `CLAUDE.md` §13 |

---

## 2. Verification Results

| Gate | Command | Result |
|---|---|---|
| Analyzer compiles | `python -m py_compile Analyzer/analyze_invocation_logs.py` | PASS |
| Analyzer recency (live window) | `analyze_invocation_logs.py --log-root Logs --fix-boundary 20260608` | PASS — `recent_dates` populated; CRG loop `still_open` (recent_calls=178, recent_err=0); `describe.action_schema` `newly_quiet`; `imagegen` `regressed` under the 0608 boundary; `no_recent_data` distinct from zero-error |
| Analyzer default (legacy order) | `analyze_invocation_logs.py --log-root Logs` | PASS — recent window = last 3 present days `[20260613,14,15]`; recency sections render; default `rank`/`score` unchanged |
| Offline-exe CI gate — stale blocks | edit `monolith_query.cpp` then `ci_static_checks.py … check` | PASS — exactly **1** `[offline-exe-fresh]` blocker with rebuild guidance |
| Offline-exe CI gate — fresh passes | `build.bat` rebuild then `ci_static_checks.py … check` | PASS — `Blocking findings: 0` |
| CI selftest unchanged | `ci_static_checks.py … selftest` | PASS — `selftest passed` |
| Offline exe builds | `Tools/MonolithQuery/build.bat` | PASS — `Built: Plugins\Monolith\Binaries\monolith_query.exe`; `--catalog-self-check` success |
| Editor C++ builds | `UnrealBuildTool GoGameEditor Win64 Development -Project=GO.uproject` | PASS — `Result: Succeeded` (compiled `MonolithCoreTools.cpp` + `MonolithSourceActions.cpp`, linked both DLLs, 10.98 s incremental) |
| CRG cooldown skip | `build_crg_graph --execute --graph-db=<copy>` with recent `last_build_epoch` + churned signature | PASS — `skipped=true, skip_reason=cooldown, seconds_since_last_build=2, replaced=false`, **0.84 s** |
| CRG cooldown disable | same copy with `--cooldown-seconds=0` | PASS — `skipped=false, replaced=true`; fresh `last_build_epoch` written |
| `coverage_miss` (offline) | `monolith_query.exe source read_source <absent>` / `read_file <absent>` | PASS — `[error_class=coverage_miss]` hint + reindex `next_actions` |
| `coverage_miss` (editor) | UBT compile of `MonolithSourceActions.cpp` | PASS — compiles; runtime path requires a live editor (deferred) |

---

## 3. Notes and Deferred Work

- **5.1 trigger fix is N/A in-repo.** The `repair_crg_cache` + `build_crg_graph` chaining is agent/schedule behavior (`CLAUDE.md` §12), not a committed script — `Scripts/check_index_freshness.ps1` only issues health-gated `repair_crg_cache`. The cooldown gate enforces the cap regardless of caller.
- **5.2 remaining (steps 1–2).** Quantifying scope-vs-stale coverage and widening the indexer / making `index_health` detect symbol/file misses is editor-side and remains open; only the `coverage_miss` read hint (step 3) landed here.
- **Optional-module audit conclusion.** `gas`/`combograph`/`logicdriver` are the only namespaces whose entire action set can be empty; every other optional namespace registers an always-on `get_status`/list action, so the `GetKnownOptionalModules()` list is complete. The invariant is documented in-code so future module authors keep it complete.
- **CRG cooldown skip cost.** The skip still computes `source_counts` before short-circuiting, so the headline cost saved is the ~15 s rebuild (and the avoided `graph.db` write), not the COUNT(*) probes; measured skip wall-time was 0.84 s on the live 9.4 GB `EngineSource.db`.
- **No live DB mutated.** CRG cooldown tests ran against a temporary `graph.db` copy, removed after the run.

---

## 4. Adversarial Review and Fixes (2026-06-15)

A multi-agent adversarial review (one reviewer per change-area, each finding self-verified) ran over the changes and confirmed 8 real defects (coverage-miss was clean). All were fixed:

| # | Sev | Area | Defect | Fix | Verified |
|---|---|---|---|---|---|
| 1 | high | analyzer | Per-date recency tallies counted heartbeat/synthetic rows the findings exclude, so synthetic-only recent traffic could flip `no_recent_data` → false `newly_quiet`/closed | Gated `count_by_action_date`/`error_by_action_date` behind the same `_exclude_from_problem_findings` check as the finding counters (window `all_date_keys` stays noise-inclusive) | Controlled fixture: synthetic-only recent day → `still_open=None`, `status=no_recent_data`, `recent_calls=0` |
| 2 | low | analyzer | `recency_score` = filtered base score × weight from unfiltered population | Same fix as #1 aligns both populations | — |
| 3 | low | ci-gate | `check_offline_exe_fresh.py` `--version` subprocess had no timeout; a hung exe wedges CI, and `TimeoutExpired` escapes the `(ValueError, OSError)` catch | Added `timeout=30` and converted `TimeoutExpired` → `ValueError` in `read_exe_source_hash` | py_compile + selftest pass; freshness FRESH |
| 4 | medium | crg-cooldown | Live MCP `build_crg_graph` did not forward `cooldown_seconds`, so the spec's tunability was unreachable via MCP | Cooldown DOES protect the live path (child runs default 1800; `force` bypasses); the tuning override is offline-CLI only — spec corrected to state this accurately | `SPEC_MonolithSource.md` row |
| 5 | low | crg-cooldown | `cooldown_seconds` undocumented in build/rebuild/repair_crg_graph `--help` | Added to syntax/flags/defaults in `monolith_query_help.h`; rebuilt | `--help` shows it; `--cooldown-seconds 600` space-form parses |
| 6-8 | high/low | optional-audit | The in-code audit comment falsely claimed `audio`/`ai`/`level_sequence` are always-on; a large **bEnable\*-setting-gated** class (~18 namespaces) also returns 0 actions when a user disables a default-true toggle | Ground-truthed every module `StartupModule`; rewrote the comment to split **WITH_\*-plugin-gated** (gas/combograph/logicdriver — complete) from the **setting-gated** class (deferred, user-initiated; preferred remedy: always-on `get_status`). `chooser`/`animation`/`niagara` confirmed always-on. TODO + backlog spec corrected | Comment-only (compiles); grep of all `*Module.cpp` |

Post-fix gates: `ci_static_checks … check` → **0 blocking findings**; `… selftest` pass; offline exe rebuilt FRESH; `--catalog-self-check` success; analyzer py_compile + live-window + controlled-fixture runs pass.

## 5. Out of Scope (still open in the backlog)

- **MCP endpoint availability root cause** (P1 top agent item) — editor-side `9316` flicker + Codex-direct client path; requires a running editor and out-of-repo Codex config. Partial mitigation (proxy transient-retry) already landed CL 722.
- **P3 input-tolerance / cost** — imagegen server-side cooldown/dedup (5.4), `source_control` coercion (5.5), large-result projection (5.6): explicitly low-priority/latent, not high-ROI.
