# Monolith Agent Tool-Call & MCP Reliability Backlog

**Parent:** [SPEC_MonolithInvocationLogAnalyzer.md](SPEC_MonolithInvocationLogAnalyzer.md)
**Engine:** Unreal Engine 5.7+
**Status:** Accepted backlog (log-grounded, 2026-06-14)
**Created:** 2026-06-14
**Owner modules (open work):** MonolithCore, MonolithSource, MonolithLogicDriver, `Analyzer/`, `Tools/MonolithQuery`. Section 5.6 large-result work also touches MonolithBlueprint, MonolithAudio, MonolithPaper2D; Section 4 references MonolithImageGen, MonolithEditor, MonolithSourceControl, MonolithBlueprint for already-closed items.
**Scope:** A ROI-ranked, source-grounded work list for making agents call Monolith MCP actions and the offline `monolith_query.exe` more successfully. Derived from a full read-only sweep of `Logs/` (2026-05-20 → 2026-06-14) with the invocation-log analyzer, then ground-truthed against current source and re-checked against the latest log days so already-closed issues are not re-opened.
**Non-goals:** New domain namespaces, action output-payload redesign, changing public action contracts beyond additive alias/coercion, telemetry, or anything the analyzer spec already covers as detector design.

---

## 1. Purpose

`SPEC_MonolithInvocationLogAnalyzer.md` defines the detectors that turn daily invocation logs into findings. This spec is the **curated, verified output** of running those detectors over the current log window and confirming each candidate against live source and the most recent log days, so the team works on what agents are *still* failing on rather than what they failed on weeks ago.

The dominant methodological result (Section 3) is that **most loud findings are already fixed; their log evidence is historical, trapped in the window before the fix reached the running editor/offline binary.** Two independent examples — `describe.action_schema` (fixed in source 2026-05-29, effective in the editor ~06-08) and `imagegen.generate_image_via_ima2` (rate-limit classifier committed 06-13, after a one-day 06-10 error burst that had already self-subsided) — were each mis-ranked as "open and worsening" until the per-item fix date was checked. This spec ranks only the bleed confirmed on the latest log days.

## 2. Evidence Base

Numbers below are a **dated analyzer snapshot**; `Logs/` is append-only, so live re-counts grow. Treat the snapshot as the reproducible baseline (Section 8), not a live total.

| Field | Value |
|---|---|
| Analyzer | `python Analyzer/analyze_invocation_logs.py --log-root Logs --out Saved/Monolith/LogAnalysis/goal-20260614 --top 60` |
| Snapshot | `goal-20260614`, generated `2026-06-14T18:09:04+09:00` |
| Window | `20260520` → `20260614` (26 date folders) |
| Records | 43,205 across 50 JSONL files, 0 parse warnings (snapshot; live re-count grows as the current day appends — e.g. ~43,219 shortly after) |
| Surfaces | action 36,422 (1,530 error) · query 6,742 (495 error) · proxy 41 (2 error) |
| Recency check | Per-item re-bucket on the latest days. **No-data caveat:** of the latest 3 days only `20260612` carries real action-surface invocations; `20260613/action.jsonl` is ~716 rows of 100% `monolith.discover` enumeration (0 real domain calls except the logicdriver errors in 5.3) and `20260614` has no `action.jsonl` at all. So "0 errors on 06-13/06-14" for an editor action is *no-data*, not a fix proven to hold; query-surface days are complete. |
| Report | `Saved/Monolith/LogAnalysis/goal-20260614/summary.md` + `findings.json` (157 findings) |

Heartbeat (`monolith.status`, 25,003 calls, 0 errors) dominates raw counts and is correctly excluded from ROI ranking by the analyzer's `heartbeat` class.

**Coverage scope and blind spot (now measured — §6C).** This backlog is grounded in Monolith's **server-side** invocation logs (`action`/`query`/`proxy.jsonl`), which record every call that *reaches* Monolith regardless of client. A second reader, `Analyzer/analyze_session_transcripts.py`, now also mines the **client-side agent session transcripts** (Codex `~/.codex/sessions/`, Claude Code `~/.claude/projects/`), parsing only structured tool-result payloads. Result over the same window (1,180 session files): **Codex is the primary Monolith MCP client** (376 Monolith tool results across 26 sessions; Claude Code ≈0 `mcp__monolith__*`), and of **137 client-observed Monolith errors, 136 (99.3%) are transport/availability failures** ("Transport send error" to `localhost:9316`) across 20 of 26 Monolith-using sessions, with **0 server-captured** errors. This overturns the earlier crude grep estimate (which substring-matched "error"/"failed"/"9316" inside *successful* payloads and wrongly suggested schema errors dominated): the real dominant client-side failure is **MCP endpoint availability**, and it is **100% invisible to `action.jsonl`** because the call dies before the editor logs it (`CLAUDE.md` §14). Caveat: only parsed tool-result payloads are trustworthy — raw `grep` over transcripts is contaminated because the injected `CLAUDE.md` text itself contains `9316`/`recover_mcp`/`ECONNREFUSED`.

## 3. Method — Solved vs Open Temporal Split

Each candidate's error rows were bucketed against the source-fix date(s) that bracket the window, then re-checked on the latest days. Buckets: `pre` (`<0529`), `gap` (`0529–0607`), `post` (`>=0608`); the "Latest" column is the status that actually decides open vs closed (last real-call day noted where it is not the window end).

| Action | Total err | pre | gap | post `>=0608` | Latest | Verdict |
|---|---:|---:|---:|---:|---|---|
| `describe.action_schema` | 109 | 3 | 106 | 0 | 0 | Closed (alias build caught up ~06-08) |
| `editor.run_python` | 15 | 15 | 0 | 0 | 0 | Closed |
| `blueprint.get_component_details` | 61 | 55 | 6 | 0 | no calls after 06-07 | Closed (06-08 source fix; post-0608 is no-data, fix date is the evidence) |
| `source_control.*` (strict type / `files`→`paths`) | ~19 | all | 0 | 0 | 0 (last 05-27) | No recurrence in window; fixed 2026-07-04 with explicit additive tolerance for `files`, scalar path strings, and known boolean string literals. |
| offline `--help` (`<none>`/`project`/`source`) | ~70 | 52 | 18 | 0 | 0 (last 06-03) | Closed (Query Help first slice; no post-fix success row — smoke-check recommended, §8) |
| `imagegen.generate_image_via_ima2` | 196 | 80 | 7 | 109 | 0 | Closed for classification (committed 06-13; the 109 post-0608 were a 06-10 burst that subsided before the fix) — see 5.4 for the remaining blind-retry gap |
| `source.read_file` param-confusion (`path`→`file_path`) | ~73 | 59 | 14 | 0 (last 06-05) | 0 | Closed (alias `:337` functioning) |
| `source.read_source` param-confusion (no `symbol`) | ~150 | 97 | 53 | 0 (last 06-09) | 0 | Mostly closed; reroute cleanup in 5.5 |
| `source.read_source`/`read_file` **index miss** ("No symbol/file found") | ~28 + tail | — | — | persists | **all 4 latest-day read errors** | **Open — EngineSource.db coverage gap (5.2)** |
| `logicdriver` discover unknown-namespace | 11 | 0 | 0 | 11 | **11 on 06-13 (latest action day)** | **Open — routing gap (5.3)** |
| `source.build_crg_graph` + `repair_crg_cache` | ~18 | low | low | low | **23–25/day each** | **Open — cost, not errors (5.1)** |

Closure rule: a row that is `0` on the latest *real-call* day is closed; a post-bucket that is loud but zero on the latest day (imagegen) is closed for that failure mode. Always confirm against source AND the last real-call day before scheduling.

## 4. Already-Closed Issues — Do Not Re-Open

Verified fixed in source (with proof) and quiet on the latest log days. Recorded so future log reads (and the analyzer's own ROI table) do not re-prioritize them. This catalog lists loud-then-quiet findings whose closure is **source-verifiable**; other lifetime-loud, recently-zero high-severity findings in `findings.json` without a single source fix (`describe.schema` 16/11, `paper2d.get_asset` 12/10 — wrong-name/usage errors) are deferred to the analyzer "newly-quiet" view (§6B item 3), not asserted closed here.

| Finding | Source proof | Why closed |
|---|---|---|
| `imagegen.generate_image_via_ima2` rate-limit error **classification** (107/109 of the post-0608 errors were a single 06-10 burst) | `MonolithImageGenActions.cpp` classifies HTTP 429 / "rate limit" as `error_class="provider_rate_limited"` with `retry_after_seconds` + advisory hint; committed 2026-06-13 (`f5c0873b`) | The 06-10 burst self-subsided before the fix (06-12 = 7 calls / 0 errors), so the quiet was the provider limit clearing; the in-tree classifier now prevents *future* opaque `-32603` storms. **Classification only** — no server-side backoff/dedup (5.4) |
| `describe.action_schema` wants `target_namespace`/`target_action`, agents send `namespace`/`action` | `MonolithBulkFillActions.cpp` `BuildDescribeActionSchemaSchema()` registers aliases `{namespace,domain}`→`target_namespace`, `{action}`→`target_action` (dated 2026-05-29) | K2 `ApplyAliases` gate (`MonolithToolRegistry.cpp` ExecuteAction) rewrites before required-param check; 0 errors post-0608 |
| `editor.run_python` wants `command`, agents send `code`/`script` | `MonolithEditorActions.cpp:946` registers `{code,script}`→`command` | 0 errors post-0608 |
| `blueprint.get_component_details` hard `-32603` on a nonexistent/mistyped `component_name` (58 of 61 window errors "Component not found") | `MonolithBlueprintActions.cpp` now returns `Success(BuildComponentNotFoundResult(...))` with `match_status=component_not_found` + `scs_components`/`inherited_native_components`/`candidate_components`/`next_actions`; commit `e2db9d81` 2026-06-08; `SPEC_MonolithBlueprint.md` updated same commit | Component-not-found is now a structured recoverable Success listing available/candidate components; no calls after 06-07 so post-0608 zero is no-data, the fix date is the evidence |
| `blueprint.resolve_node` CustomEvent dry-runs with `reliable:"true"` fail strict bool schema validation (`schema_fix:action:blueprint.resolve_node:7fc7ff6f02294432` in the 2026-07-03 ROI snapshot) | `MonolithBlueprintNodeActions.cpp` registers only `resolve_node.reliable` as `bool|string` and the handler normalizes boolean plus `true/false/1/0/yes/no` string literals while rejecting arbitrary strings. `MonolithBlueprintParamGuardTests.cpp` covers both acceptance and rejection. | Closed 2026-07-03 by an additive dry-run-only compatibility path. Mutating Blueprint actions and other bool params remain strict. Verification: `Monolith.Blueprint.ResolveNode.AcceptsReliableStringLiteral`, `Monolith.ParamGuard.Blueprint.NodeActionsRejectMalformedTopLevelParams`, and live MCP success/error calls. |
| `source.read_file` wants `file_path`, agents send `path` | `MonolithSourceActions.cpp:337` `RequiredDiskPath("file_path", …, {"path"})` | Alias functioning; last param-confusion error 06-05, 0 since (latest read_file errors are index misses, 5.2 — not this) |
| Offline `monolith_query.exe --help` / `<ns> --help` fails | `SPEC_MonolithQueryHelp.md` (Implemented first slice) | 0 `--help` errors post-0608 (last 06-03). Closure rests on no-data — add the §8 smoke check |
| `source.trigger_project_reindex` returned `unknown_action` offline (loud through 06-05, ~54% lifetime err) | `Tools/MonolithQuery/monolith_query.cpp` offline handler returns `status=live_only`, `offline_supported=false`, exit 0 | Opaque "Unknown source action" → actionable `live_only` guidance; 0 errors from 06-07 on |

`source_control` strict-type errors (`dry_run="true"` string, scalar `paths`, key `files`) stopped recurring after 05-27 and are now source-fixed as an additive-tolerance item: `files` aliases to `paths`, scalar path strings wrap to a one-item path list, and known boolean string literals parse only inside source-control handlers. Numeric booleans and arbitrary bool strings still fail.

The alias capability is first-class and proven: `FParamSchemaBuilder::Required(name,type,desc,{aliases})` and the K2 `ApplyAliases` dispatch gate (`SPEC_MonolithActionAuthoring.md` §5–6). New alias/coercion work is therefore low-risk and additive — it accepts more input without changing any output, satisfying the public-contract-stability rule in `CLAUDE.md` §9.

## 5. Open High-ROI Backlog (ranked, latest-day verified)

Ranking = bleed confirmed on the latest real-call days × cost × confidence, with implementation risk as a tie-breaker toward low-risk/high-confidence fixes. **This mixes machine-cost and agent-experience ROI — see §5.0 for the by-axis re-read, which matters if the goal is specifically "agents route/operate better."**

| Rank | Item | Latest-day evidence | Cost driver | Risk |
|---:|---|---|---|---|
| 1 | source CRG maintenance loop (5.1) | 23–25 `build_crg_graph` + 23–25 `repair_crg_cache` **per day**, 06-12/13/14 | ~14k s/week recent; ~127k s cumulative (≈90% historical pre-0608) | medium |
| 2 | EngineSource.db symbol/file index-coverage gap (5.2) | all 4 latest-day read errors are "No symbol/file found" on correctly-passed inputs | repeated failed reads → retries/fallbacks; misleads triage | medium |
| 3 | `logicdriver` discover unknown-namespace routing gap (5.3) | 11 identical `-32602` on 06-13 (latest action day) | retry loop; agent cannot discover an enabled module | low |
| 4 | imagegen blind-retry / provider cost (5.4) | latent — classifier shipped 06-13, but no server-side backoff/dedup | duplicate expensive calls during a provider storm | low |
| 5 | input-tolerance cleanups: read param-routing (5.5 partial remains) | param-routing dried up post-06-09; source_control coercion is closed | wasted round-trips on natural-but-wrong input | low |
| 6 | large-result context pressure (5.6) | 4 actions ≥ 209 KB (window-wide) | context bloat → extra follow-up calls | medium |

#### 5.0 Two value axes — agent-experience vs machine-cost vs measurement

The table above ranks by *bleed × cost*, which mixes two different ROI axes. If the objective is specifically **"agents route and operate better"**, re-read the backlog by axis — they are not the same work:

| Item | Axis | Does an interactive agent's tool-call succeed where it failed? |
|---|---|---|
| 5.2 index-coverage | 🧭 agent routing/operation | Yes — direct: real reads stop returning "not found" |
| 5.3 logicdriver discover | 🧭 agent routing/operation | Yes — direct: ends a `-32602` discover retry loop |
| 5.5 input tolerance · 5.4 imagegen retryable · 5.6 large-result | 🧭 agent routing/operation | Yes, but low current volume / latent |
| (Section 4 alias + schema fixes) | 🧭 agent routing/operation | **Already shipped** — these were the biggest agent-routing wins |
| 5.1 CRG maintenance loop | 💰 machine cost / infra | **No** — automation-driven, offline-CLI surface, does not contend with the live editor; saves ~14k s/week of machine time but changes no agent's success rate |
| 6A build/binary lag | 🔭 delivery multiplier | Indirect — makes every other fix reach agents in hours not days |
| 6B analyzer recency | 🔭 our triage tooling | No direct agent impact |
| MCP endpoint availability (measured via 6C) | 🧭 agent routing/operation | **Yes — the largest measured agent-facing failure.** A downed endpoint = the agent does nothing. 136 transport failures across 20 sessions (§2). The 6C reader that surfaces it is 🔭 tooling; the availability fix itself is 🧭 |
| 6C session-transcript reader | 🔭 measurement | Implemented (`Analyzer/analyze_session_transcripts.py`). Quantified the availability gap above |

**Honest magnitude (updated by §6C measurement).** The largest agent-routing *schema/alias* improvements already landed (Section 4); the open *server-visible* agent-facing residue is modest (index ~4 errors/day, logicdriver 11/day). But the §6C measurement changed the picture: the **single largest agent-facing failure class is MCP endpoint availability — 136 transport failures (99.3% of client-observed Monolith errors), invisible to the server logs.** It dwarfs every server-visible open item and is the true top agent-operation priority.

**Agent-experience-first ordering** (use this if the goal is agents routing/operating better, not machine cost): **MCP availability fix (endpoint uptime / reconnect hardening, now measured as #1) → 5.2 index-coverage → 5.3 discover → 5.4/5.5/5.6**. The cost/tooling track (5.1 CRG, 6A, 6B, the 6C reader) runs in parallel and is justified on its own axis. 6C measurement is done; the open work it points to is the availability fix.

### 5.1 source CRG maintenance loop — enforce a cooldown gate (Rank 1 by cost, not by agent impact)

**Evidence.** `query:source.repair_crg_cache` and `query:source.build_crg_graph` are the only candidates **still running on every recent day** — 25/23/25 paired calls on 06-12/13/14, error count ~0. This is a wasted-wall-time problem, not a correctness one. Sampled records: `--force` **never present**, `routing_context.namespace_source = "offline_cli"`, `decision_source = "direct"`.

**Cost (recency-honest).** On the latest 3 days `build_crg_graph` runs ~13–17 s/call (06-12 13.4 s / 06-13 17.3 s / 06-14 15.0 s) and `repair_crg_cache` ~58–78 s/call (58.5 / 77.7 / 68.1 s) — about ~14k s/week combined right now. The headline ~127k s cumulative (`repair` ~462 calls/~76.9k s, `build` ~443/~50.1k s) is ~90% pre-0608 heavy rebuilds; do not quote the ~114 s whole-window per-call average as the current rate. At ~15 s/build + ~68 s/repair these are still real incremental rebuilds (`build_mode=atomic_temp_replace`, `replaced=true`, source signature churns every call), not a sub-second skip — so the `CLAUDE.md` §12 skip-when-fresh path cannot engage and the right remedy is a cooldown, not the existing freshness no-op. (The maintenance class scopes to the `source` namespace; ~10 `project.repair_crg_cache` fast no-ops are excluded, so a raw grep returns more.)

**Root cause (confirm in step 1).** A direct offline caller (script or schedule) chains `repair_crg_cache --execute` + `build_crg_graph --execute` as a routine pair — the anti-pattern `CLAUDE.md` §12 warns against. Documented guidance alone has not stopped it; it must become an enforced gate. Either skip-when-fresh does not engage on the offline path, or the source signature is invalidated between calls by incremental builds.

**Fix.**
1. Find the caller: grep `BatchFiles/`, `Scripts/`, and `.jules/` schedules for a `repair_crg_cache` + `build_crg_graph` sequence; identify the offline_cli trigger.
2. Add a **cooldown-per-signature-change** gate on the offline path in `Tools/MonolithQuery/monolith_query.cpp` (and the shared builder): rebuild at most once per source-signature change; when parity is fresh and `--force` is absent, return `status=skipped, reason=parity_fresh` in well under a second with a `return_summary` field the analyzer can read.
3. Remove the routine chaining at the trigger; keep `repair_crg_cache` health-gated and `build_crg_graph` on-demand per `CLAUDE.md` §12.

**Verification.** Force the cold state (call 1 with `--force` or after touching the signature), assert call 1 did real rebuild work (> ~10 s), then assert a second call with no intervening source change returns `skipped/parity_fresh` in < 1 s — so the rebuild→skip *transition* is tested, not a vacuous already-fresh pass. Analyzer `maintenance_loop` cumulative-seconds for the next window drops.

### 5.2 EngineSource.db symbol/file index-coverage gap (Rank 2)

**Evidence.** Every read error on the latest days is an index miss on **correctly-formed input**, not a param mistake: `source read_source FTabManager::RequestSavePersistentLayout` → "No symbol found…"; `source read_file Engine/Source/Runtime/Slate/Private/Framework/Docking/TabManager.cpp --start=1000 --end=1110` → "No file found…" (emitted at `MonolithSourceActions.cpp` read handlers). Window-wide, `no_symbol_indexed` + `no_file_indexed` dominate the non-param read errors and span **both Engine source (Slate) and project source (`Source/GoGame/...`)**. `environment.index_health` reports `ok` despite these misses, so the gap is invisible to the freshness check.

**Root cause.** `EngineSource.db` does not cover the symbols/files agents actually request — either the index scope omits paths/symbols that exist, or it is stale relative to the tree, while `index_health=ok` masks it.

**Fix.**
1. Quantify coverage: take the distinct missed symbols/paths from `query.jsonl`/`action.jsonl`, confirm they exist on disk, and check whether they are absent from `EngineSource.db` (scope gap) vs present-but-stale.
2. If scope: widen the indexer to cover the missed roots (tie to `CLAUDE.md` §12 post-build reindex / `source.trigger_project_reindex`). If staleness: make `index_health` actually detect symbol/file misses instead of reporting `ok`.
3. Until covered, the read handlers should return a `coverage_miss` hint (reindex suggestion) rather than a bare "not found", so agents stop retrying or falling back to `editor.run_python`.

**Verification.** A read for a symbol/file proven to exist on disk but missing from the DB returns a `coverage_miss` hint; after reindex it returns the body. Analyzer read-error `no_symbol_indexed`/`no_file_indexed` counts drop. Update `SPEC_MonolithSource.md` / `SPEC_MonolithIndex.md` in the same change.

### 5.3 logicdriver discover unknown-namespace routing gap (Rank 3)

**Evidence.** `monolith.discover {namespace:"logicdriver"}` → `-32602 "Unknown namespace: logicdriver"`, 11× identical `retry_signature` on 06-13 (the latest action day). `logicdriver` is a structurally normal optional Fab-plugin module (`Source/MonolithLogicDriver/`, gated `WITH_LOGICDRIVER`, `bEnableLogicDriver=true` default in `MonolithSettings.h`, with `Skills/unreal-logicdriver/SKILL.md` routing agents to it).

**Root cause (grounded).** `GetKnownOptionalModules()` (`MonolithCoreTools.cpp:32-49`) lists only `gas` and `combograph`, so the graceful `disabled`/`not_installed` + install-hint branch (`MonolithCoreTools.cpp:1130-1182`) never fires for `logicdriver`; it falls through to the bare `-32602` at `:1178`. The inconsistency is self-evident: `bulk_fill`/`describe` already advertise `logicdriver` as a valid adapter namespace (`MonolithBulkFillActions.cpp:23`), but `discover` does not know it.

**Fix.** Add a `{logicdriver, bEnableLogicDriver, logicdriver_query, install hint}` entry to `GetKnownOptionalModules()` so `discover` returns `Success{status:not_installed|disabled, hint}` instead of `-32602`, stopping the retry loop. Audit the list against all `WITH_*`-gated optional modules to prevent the next instance. Update `SPEC_MonolithSource.md` / the discover contract in the same change.

**Verification.** `monolith.discover {namespace:"logicdriver"}` on a build without the plugin returns a `Success` with `status` + hint and exit 0 (no `-32602`); with the plugin it lists actions.

### 5.4 imagegen blind-retry / provider cost (Rank 4)

**Evidence.** The 06-13 classifier (`MonolithImageGenActions.cpp`) emits `provider_rate_limited` + `retry_after_seconds` + an advisory hint, but there is **no** server-side backoff/dedup/cooldown (grep of `Source/MonolithImageGen/` for `throttle|backoff|cooldown|dedup` = none) and it does not consume the existing `retry_signature`. During the 06-10 storm agents re-issued the same expensive request; the analyzer's `duplicate_retry` detector already flags this (`duplicate_retry:action:imagegen.generate_image_via_ima2` at 25/22/19/19). The fix path was never exercised after shipping (06-12 had 7 successes / 0 rate-limit events), so this is latent risk, not a current bleed.

**Fix (no fallback — `CLAUDE.md` core rule).** Add a server-side cooldown or dedupe keyed on the existing `retry_signature`, and/or a documented "honor `retry_after_seconds`" contract for clients. No provider substitution or placeholder image. No new detector needed — `duplicate_retry` already covers it.

**Verification.** Inject a simulated 429; assert a second identical request inside `retry_after_seconds` short-circuits without a provider call.

### 5.5 Input-tolerance cleanups (Rank 5, low priority)

Two additive tolerances for natural-but-wrong agent input. Both are historical/latent, not latest-day bleed, so low priority — but cheap and aligned with the proven alias pattern.

- **read_source path-vs-symbol reroute.** `read_source` already reroutes to `read_file` when the `symbol` *value* looks like a path (`MonolithSourceActions.cpp:1137-1150`, `LooksLikeSourceFilePath` at `:77`), but the empty-`symbol` guard at `:1133-1136` fires first, so a path supplied under the `file_path`/`path` key (no `symbol`) errors before the reroute. Param-confusion of this class last appeared 06-09; fixing it is correctness cleanup, not a current-bleed fix. Fix: before the empty-`symbol` error, if `symbol` is absent but `file_path`/`path` is present, delegate to `HandleReadFile` with a routing `warnings[]` note.
- **source_control type coercion (closed 2026-07-04).** `MonolithSourceControlActions.cpp` now declares `paths` as `array|string` with `files` alias, accepts scalar path strings in handlers, and parses known boolean string literals for `dry_run`, `confirm`, and `resolve_packages` while rejecting numeric booleans and arbitrary strings. Coverage: `FMonolithSourceControlTypedParamsTest` and `FMonolithSourceControlInputToleranceTest`.

### 5.6 Large-result context pressure (Rank 6)

**Evidence (byte values reproduce exactly).** `query:source.find_overrides` 282,723 B (offline `Tools/MonolithQuery`); `action:blueprint.search_functions` 265,751 B; `action:audio.list_available_metasound_nodes` 213,195 B; `action:paper2d.list_assets` 209,196 B (editor handlers). Large payloads inflate context and trigger follow-up calls.

**Fix.** Add default result caps + field projection + `next_cursor` to these list/search actions (per the result-quality detectors and the `structuredContent` surface in `SPEC_MonolithStructuredToolResults.md`). The offline `find_overrides` path and the three editor handlers are separate code paths — do it path-by-path, not as one cross-cutting change. Lower urgency than 5.1–5.3 because it degrades efficiency rather than failing the call.

## 6. Systemic Root Causes (highest leverage)

### 6A. Build / binary distribution lag

The `describe.action_schema` timeline (fix 2026-05-29 → effective ~2026-06-08, ~106 avoidable failures in the gap) is the clearest case. Action-contract fixes only help agents once the **running** editor and the shipped `monolith_query.exe` carry them.

**Fix.** Make "fix reached the running surface" verifiable and fast:
1. After any action-schema/alias change, require a UBT editor rebuild **and** an offline-exe refresh in the same changelist; `Scripts/check_offline_exe_fresh.py` already detects staleness — wire it into `Scripts/ci_static_checks.py` so a stale `monolith_query.exe` fails CI.
2. Confirm the post-build reindex/reload hook (`CLAUDE.md` §12) actually fires; if not, document `source.trigger_project_reindex` as the manual catch-up. (Note this also feeds the 5.2 index-coverage gap.)
3. Treat the analyzer's pre/post split (Section 3) as the regression check: re-run after a deploy and confirm the fixed action's latest-day bucket is zero.

### 6B. Analyzer ROI ranking is recency-blind

The analyzer's High-ROI table still ranks `describe.action_schema` (score 1740) and `imagegen.generate_image_via_ima2` (score 3175) high although both are at 0 errors on the latest days. Whole-window counts hide whether a problem is current — and a single global fix-boundary is also insufficient, because different actions are fixed on different dates (imagegen ~06-13 vs the alias batch ~05-29/06-08). This recency blindness mis-ranked two separate items during the authoring of this spec.

**Fix (in `Analyzer/analyze_invocation_logs.py`).**
1. Add a recency dimension to ROI scoring: weight evidence by age (decay older rows) or compute a `recent_error_rate` over the last K days alongside the lifetime rate.
2. Emit a `still_open` flag per candidate from a pre/post split, defaulting the boundary to "last K days vs the rest" so per-item fix dates are respected without manual configuration; allow `--fix-boundary` to override.
3. Surface a "regressions" view (recent rate ≥ historical rate) and a "newly quiet" view (lifetime-loud, recently-zero) so reviewers see both directions — and so loud-then-quiet items (`describe.schema`, `paper2d.get_asset`) land somewhere accountable instead of being silently dropped.
4. Distinguish *no-data* from *zero-error* on a given day (an editor action with 0 calls is not "passing"). Keep changes additive and behind flags so existing report consumers are unaffected (`CLAUDE.md` §9 / analyzer non-goals).

### 6C. Client-side MCP availability is invisible to server logs — measured; now the top agent-facing item

Every detector built on `Logs/` reads records that only exist *after* a call reaches Monolith. Calls that die at the transport — endpoint `localhost:9316` unreachable, `Session not found`, connection closed — never produce an `action.jsonl` row, so the server-side dataset structurally cannot see them.

**Reader: implemented.** `Analyzer/analyze_session_transcripts.py` (read-only sibling of `analyze_invocation_logs.py`) parses Codex `~/.codex/sessions/` and Claude `~/.claude/projects/` rollouts, classifying **structured tool-result payloads only** (`function_call_output` / `tool_result`) into `transport_availability` vs `server_captured` vs `other`, and emits a `mcp_availability` finding. It deliberately ignores prompt/instruction records, so it is immune to the injected-`CLAUDE.md` contamination (`9316`/`recover_mcp`/`ECONNREFUSED`).

**Measured result (window `--since 20260520`, 2026-06-14 run; record: `Docs/testing/2026-06-14-mcp-availability-session-transcripts.md`).** 1,180 session files; 376 Monolith tool results across 26 sessions (Codex 375, Claude 1). **137 errors (36.4%); 136 are `transport_availability` ("Transport send error", 99.3% of errors) across 20 of 26 sessions; 0 `server_captured`.** Bursts on 20260606 (42), 20260522 (33), 20260521 (29), 20260607 (11). This is the **largest agent-facing Monolith failure class measured anywhere and is 100% absent from `action.jsonl`** — it overturns the earlier crude-grep guess that schema errors dominated.

**Partial mitigation landed (2026-06-14, CL 722).** The failures are *transient flicker* — every one of the 8 failing sessions interleaves successes and failures (`SFFSFFFSSSS…`), 0 are sustained-down. So a bounded retry self-heals most of them. `Scripts/monolith_proxy.py` and `monolith_proxy.js` now retry **send-side connection failures only** (`ECONNREFUSED`/reset/etc. — the request never reached the server, so no mutation can double-execute) with backoff; read timeouts are never retried. Verified by unit tests (`Docs/testing/2026-06-14-proxy-retry-and-schema-routing.md`). **Scope limit:** this helps proxy-routed clients (Claude Code via `.mcp.json`); the measured pain is **Codex**, which uses its own rmcp streamable-HTTP client directly to `9316` and bypasses the proxy, so it is unaffected by the proxy retry.

**Open work (root cause, the bigger fix).** Treat MCP endpoint availability as the #1 agent-operation item: find why the editor `9316` endpoint flickers (correlate transport bursts with editor GC/build/restart windows) and harden the editor-side MCP HTTP server so it does not refuse connections under load; and/or get the same connection-retry into the Codex client path (its config, outside this repo) and the native C++ `monolith_proxy.exe`. Reader follow-up: capture post-failure recovery behavior (reconnect vs retry vs `editor.run_python` fallback).

**2026-07-03 follow-up and mitigation.** A fresh session transcript run over 3,355 Codex/Claude files (`Saved/Monolith/SessionAnalysis/roi-20260703`) found 2,432 Monolith tool results and 282 errors; 246 were transport/availability failures across 48 sessions, all from Codex direct calls. The immediate local mitigation is `Scripts/watch_mcp.ps1`: keep `/health` supervised during long agent work, and when the endpoint is down because the editor process is gone or a headless editor is alive but unhealthy, run the host editor UBT build, restart-triggered source/graph DB maintenance, restart through `recover_mcp.ps1`, and run post-health asset DB maintenance before returning to the normal loop. `recover_mcp.ps1` also forces headless-safe asset-editor settings to avoid stale docked-toolkit modal loops. This closes the "dead editor stays dead" and "headless PID alive but MCP blocked" operational gaps, but it does not close the root-cause work for in-process 9316 flicker while a non-headless editor is still alive, nor the Codex client retry path.

**2026-07-04 follow-up.** `Scripts/recover_mcp.ps1 -ProbeOnly` now preserves the down-path exit code 2 while reporting actionable local diagnostics in the same `RESULT=MCP_DOWN` line: health failure reason/detail, MCP-port listener count/PIDs, listener owners, editor-server candidate count/PIDs, headless candidate count/PIDs, and a deterministic `next_action`. This does not reduce transport failures by itself, but it removes the blind "MCP_DOWN only" branch from the recovery path and gives the next agent enough evidence to distinguish "nothing is listening", "a non-editor owns 9316", "editor boot is still in progress", and "headless editor likely needs watchdog recovery".

## 7. Sequencing

| Phase | Work | Gate before next |
|---|---|---|
| ✅ done | 6C session-transcript reader (`analyze_session_transcripts.py`) | Reader splits transport vs server-captured; measured 136 transport failures (§6C) |
| ✅ done (CL 722) | MCP availability — **proxy transient-retry** (`monolith_proxy.py`/`.js`), partial mitigation for proxy-routed clients | Unit tests pass (`Docs/testing/2026-06-14-proxy-retry-and-schema-routing.md`) |
| ✅ done (CL 723) | 5.3 logicdriver discover entry | Build `Result: Succeeded`; `discover {logicdriver}` returns Success+hint via existing branch |
| ✅ done (CL 723) | 5.5 read_source path reroute | Build `Result: Succeeded`; `SPEC_MonolithSource.md` synced |
| ✅ done (2026-06-15) | 6B recency + no-data dimension in the analyzer | `--recent-days`/`--fix-boundary`/`--rank-by-recency`, per-finding `still_open`/`recency_status`/`recency_score`, no-data vs zero-error, recency views. Verified on the live window (CRG loop `still_open`; `describe.action_schema` `newly_quiet`) |
| ✅ done (2026-06-15) | optional-module audit (apply the §5.3 pattern to every `WITH_*` module) | Audited: `gas`/`combograph`/`logicdriver` are the only **WITH_\*-plugin-gated** namespaces (vanish when the plugin is absent — the logicdriver case), all listed → that class complete. A separate **bEnable\*-setting-gated** class (`audio`/`ai`/`material`/`blueprint`/`mesh`/`scene`/`ui`/`asset`/`config`/`level_sequence`/… ) returns `-32602` only when a user disables a default-true toggle — documented as a deferred, lower-priority gap (preferred remedy: always-on `get_status`). Both classes documented in `GetKnownOptionalModules()` |
| ✅ done (2026-06-15) | 5.2 EngineSource.db coverage gap — **`coverage_miss` read hint** (step 3) | `read_source`/`read_file` return `error_class=coverage_miss` + hint + `next_actions` on the editor (UBT Succeeded) and offline (runtime-verified) surfaces. **Still open:** indexer-widening / honest `index_health` (steps 1–2, editor-side) |
| ✅ done (2026-06-15) | 5.1 maintenance-loop cooldown gate (💰 cost track) | `--cooldown_seconds` (1800) + `last_build_epoch` in `build_crg_graph`; `skip_reason=cooldown`. `build.bat` build; cooldown-skip (0.84 s) + disable + write verified. Trigger fix N/A (chaining is agent/schedule behavior, not in-repo) |
| ✅ done (2026-06-15) | 6A binary-lag CI gate | `offline_exe_freshness` check in `ci_static_checks.py`; stale exe → 1 blocker, rebuild → 0 (verified end-to-end) |
| **P1 (top agent item, PARTIAL/OPEN)** | **MCP endpoint availability root cause** — proxy retry landed; `Scripts/watch_mcp.ps1` now covers dead-editor rebuild/restart, unhealthy-headless restart, modal-safe recover settings, restart-triggered DB maintenance, and `recover_mcp.ps1 -ProbeOnly` now reports actionable down-state diagnostics; editor-side `9316` flicker while non-headless editor is alive + Codex-direct retry/native proxy path remain open | Transport-failure count in a fresh session-transcript run drops |
| ✅ done (2026-07-03) | Offline `project search` / `source search_source` `--query` alias tolerance | `Tools\MonolithQuery\monolith_query.cpp` accepts positional, `--query`, and `--q` search text for both actions; `monolith_query_help.h` documents the forms. Built with `Tools\MonolithQuery\build.bat`; verified inline/space-separated options, positional compatibility, missing-query guidance, and catalog self-check (`Docs\testing\2026-07-03-monolith-query-search-query-alias.md`). |
| ✅ done (2026-07-03) | Offline `project.list_gameplay_tags` / `project.search_gameplay_tags` missing-action parity | `Tools\MonolithQuery\monolith_query.cpp` now serves the live ProjectIndex gameplay-tag read contracts from `tags`, `tag_references`, and `assets`; `monolith_query_help.h` documents both actions and keeps help catalog self-check green. Built with `Tools\MonolithQuery\build.bat`; verified `get_stats` sees `tags=335`, `list_gameplay_tags --limit=3`, `search_gameplay_tags Game --limit=3`, and catalog self-check (`Docs\testing\2026-07-03-monolith-query-gameplay-tags.md`). |
| ✅ done (2026-07-04) | `source.find_callers` / `source.find_callees` `query` alias tolerance | Live MCP schemas now declare `query` as a validated alias for `symbol`, closing the `missing=[symbol] provided=[query]` ROI finding; offline `monolith_query.exe` accepts positional, `--query`, and `--q` and uses the same qualified `Class::Method` trailing-method lookup as the live handlers. Built with `Tools\MonolithQuery\build.bat`; verified catalog self-check, freshness, `source find_callers --query UObject::GetName`, `source find_callees --q UObject::GetName`, missing-query negative cases, and registry alias automation coverage (`Docs\testing\2026-07-04-source-callgraph-query-alias.md`). |
| ✅ done (2026-07-04) | `source_control` additive input tolerance | Live schemas and handlers now accept `files` as a validated alias for `paths`, single path strings as one-item path lists, and known boolean string literals for `dry_run`, `confirm`, and `resolve_packages`; malformed bool strings and numeric booleans remain errors. Coverage: `FMonolithSourceControlTypedParamsTest`, `FMonolithSourceControlInputToleranceTest`, and `Docs\testing\2026-07-04-source-control-input-tolerance.md`. |
| ✅ done (2026-07-04) | `source.health` deep-check routing guard | Live and offline `source.health` now return `maintenance_recommendation` with explicit maintenance booleans and reason codes. Healthy shallow health no longer places expensive deep health in required `next_actions`; it is exposed only as an optional diagnostic action. Coverage: `FSourceHealthHealthyTest`, offline `monolith_query.exe source health --no-log`, and `Docs\testing\2026-07-04-source-health-maintenance-recommendation.md`. |
| ✅ done (2026-07-04) | 5.4 imagegen blind-retry cooldown/dedup | `imagegen.generate_image_via_ima2` now records a process-local rate-limit cooldown keyed to the redacted retry signature when the bridge returns HTTP 429 / `RATE_LIMITED` / relayed rate-limit text. Identical requests inside the window short-circuit before provider I/O with `provider_call_skipped=true`, preserving `error_class=provider_rate_limited` and `retry_after_seconds`. Coverage: `Monolith.ParamGuard.MonolithImageGen.GenerateImageViaIma2RateLimitCooldown` and `Docs\testing\2026-07-04-imagegen-rate-limit-cooldown.md`. |
| P3 (OPEN) | 5.6 large-result projection | Per-item verification below |

Each phase is a single-responsibility change set (`CLAUDE.md`/Monolith §7); do not bundle maintenance, index, discover, and analyzer work into one PR. Landed 2026-06-14: CL 722 (proxy retry), CL 723 (logicdriver discover + read_source reroute), CL 715 (this spec + analyzers + records). Landed 2026-06-15 (uncommitted working tree): 6B analyzer recency, optional-module audit, 5.2 `coverage_miss` hint, 5.1 CRG cooldown gate, 6A binary-lag CI gate.

## 8. Verification & Acceptance

| Area | Criteria |
|---|---|
| Evidence reproducible | Re-running the Section 2 command over a **frozen window** (`--until 20260613`) reproduces the record/surface counts and 0 parse warnings; the Section 3 per-action error buckets reproduce on the live tree even though aggregate totals grow. |
| maintenance loop | Cold→fresh transition: call 1 (forced/cold) does real rebuild (> ~10 s); call 2 with no source change returns `skipped/parity_fresh` in < 1 s; latest-day call counts drop. |
| index coverage | A read for an on-disk symbol/file missing from `EngineSource.db` returns a `coverage_miss` reindex hint; after reindex it returns the body; `index_health` reflects the miss. |
| logicdriver discover | `monolith.discover {namespace:"logicdriver"}` returns `Success` + status/hint and exit 0 (no `-32602`). |
| imagegen | Simulated 429: a second identical request within `retry_after_seconds` short-circuits without a provider call. |
| analyzer recency | Default run flags `still_open` per candidate, distinguishes no-data from zero-error, and exposes regressions / newly-quiet views; legacy report fields unchanged. |
| client-side availability (6C) | ✅ Met — `analyze_session_transcripts.py` parses Codex/Claude rollouts, reports a `mcp_availability` finding split into transport vs server-captured (structured payloads only), and measured 136/137 transport failures (`Docs/testing/2026-06-14-mcp-availability-session-transcripts.md`). Open follow-up: the MCP-availability fix itself (§7 P1). |
| binary lag | A stale `monolith_query.exe` fails `python Scripts/ci_static_checks.py --config .github/monolith-static-ci.json --github check`. |
| offline help | `Binaries\monolith_query.exe --help`, `<ns> --help`, and `<ns> <action> --help` each exit 0 (closes the no-data gap in Section 4). |
| offline search query options | `Binaries\monolith_query.exe project search` and `source search_source` accept positional search text, `--query=TEXT`, `--query TEXT`, `--q=TEXT`, and `--q TEXT`; missing search text exits non-zero with a message naming the positional/`--query` contract. |
| offline project gameplay tags | `Binaries\monolith_query.exe project list_gameplay_tags` and `project search_gameplay_tags` exit 0 against a healthy `ProjectIndex.db`, return paginated `tags[]`, and return explicit `schema_missing` JSON instead of pretending missing tag tables are empty. |
| source call graph query alias | Live `source.find_callers` / `source.find_callees` registered schemas rewrite `query` to `symbol`; offline `monolith_query.exe source find_callers/find_callees` accept positional, `--query`, and `--q`, while missing search text still exits non-zero with an explicit positional/`--query` contract. |
| Static checks | `python -m py_compile Analyzer/analyze_invocation_logs.py` passes; repo static checks pass or report only external blockers. |
| Docs sync | Touched module specs (`SPEC_MonolithSource.md`, `SPEC_MonolithIndex.md`, `SPEC_MonolithInvocationLogAnalyzer.md`) updated in the same changelist. |

## 9. Out of Scope

- The closed issues in Section 4 (no new code; reference only) — including the imagegen rate-limit *classification* already in `MonolithImageGenActions.cpp` (the remaining blind-retry gap is in-scope as 5.4).
- New namespaces or output-payload redesigns.
- Heartbeat (`monolith.status`) volume — healthy, zero-cost, correctly classified as noise.
- The 06-13 `monolith.discover` enumeration spike (~716 calls, ~0.46 s total, in-memory catalog read) — single-day, analyzer-flagged as low-scoring `duplicate_retry`; its only real signal (the 11 logicdriver errors) is raised as 5.3.
- Synthetic-test actions (`gas.scaffold_custom_ability_task` 45/45, `asset.import_texture_from_bytes`) — excluded by the `synthetic_test` class; not real agent failures.
