# Monolith Query CLI Help First Slice

**Parent:** [../SPEC_CORE.md](../SPEC_CORE.md)
**Engine:** Unreal Engine 5.7+
**Status:** Implemented first slice
**Owner module:** `Tools/MonolithQuery`
**Scope:** Add DB-free namespace/action help for `monolith_query.exe`, fix the CLI option parsing drift exposed by that work, and define a companion help baseline for Monolith proxy binaries/launchers.
**Non-goals:** Live MCP `monolith_discover` changes, domain action output shape changes, DB schema changes, editor-only namespace help, replacing `monolith guide`, adding new offline query actions, or changing MCP proxy protocol behavior.

---

## 1. Purpose

Agents use `Binaries\monolith_query.exe` when the MCP server or Unreal Editor is unavailable. The current CLI is useful but hard to self-orient from the command line: only top-level usage prints, namespace help fails, action-level help is absent, and several common space-separated options silently fall back to defaults.

This first slice makes the offline tool self-service:

- `monolith_query.exe --help` explains the offline surface.
- `monolith_query.exe source --help`, `project --help`, `bridge --help`, and the RI namespaces print DB-free namespace help.
- `monolith_query.exe source impact_radius --help` and equivalent action requests print action syntax, defaults, examples, output keys, and DB behavior.
- Space-separated options accepted by the advertised syntax behave the same as `--key=value`.

The help catalog must describe the offline executable, not the live editor registry. If an action is not in the executable dispatch table, it must not appear as executable offline help.

`bridge` is not a separate executable in the current checkout; it is the `monolith_query.exe bridge ...` namespace. The only shipped Monolith executables under `Binaries\` are `monolith_query.exe` and `monolith_proxy.exe`. This spec therefore covers both:

- Query help for offline DB-backed namespaces, including `bridge`.
- Proxy help for `monolith_proxy.exe` and the script proxy launchers that agents may configure in MCP clients.
- Compatibility guidance for `Scripts\monolith_offline.py`, which remains documented as a developer fallback but currently lags the native executable.

## 2. Pre-Implementation Code Facts

The source survey for this spec was based on `Tools/MonolithQuery/monolith_query.cpp` and current `Binaries\monolith_query.exe` behavior.

| Area | Current fact | Impact |
|------|--------------|--------|
| File shape | `monolith_query.cpp` is a single 9284-line standalone CLI with parser, dispatch, DB helpers, output logging, and action handlers. | Help metadata should be local and lightweight; avoid a separate parser stack. |
| Top-level help | Usage text is hardcoded in `parse_args` and only prints through the `argc < 3` path. Current `monolith_query.exe --help` prints usage but exits nonzero. | Help is treated like an error and cannot be cleanly invoked by automation. |
| Namespace help | `monolith_query.exe source --help` is parsed as namespace `source`, action `--help`; it opens `EngineSource.db`, then fails as an unknown source action. | Help depends on DB availability and fails for the exact syntax agents expect. |
| Action dispatch | Current offline dispatch includes `source` 27 entries including `repair_crg_graph` alias, `project` 17 entries, `bridge` 1, `monolith` 1, and RI namespaces `cppreflect` 6, `network` 4, `decision` 5, `risk` 5. | Help must be generated or checked against dispatch tables to avoid drift. |
| Namespace list | `offline_namespaces()` returns `source`, `project`, `monolith`, `cppreflect`, `network`, `decision`, `risk`, but omits `bridge` even though bridge dispatch exists. | Unknown namespace errors currently under-report the offline surface. |
| Action errors | RI unknown action returns JSON with suggestions and exit 0; `source`, `project`, and `bridge` unknown actions throw plain fatal errors without suggestions. | Agent recovery is inconsistent and misses the already-available fuzzy helper pattern. |
| Option arity | `value_options` covers only part of the options read later by `opt`, `opt_int`, and `opt_bool`. | `--max-depth 1 --max-results 1` currently runs as defaults `max_depth=2`, `max_results=200`; inline `--max-depth=1 --max-results=1` works. |
| Shipped executables | `Binaries\` contains `monolith_query.exe` and `monolith_proxy.exe`. | The spec should not imply a separate bridge executable exists. |
| Proxy help | `Binaries\monolith_proxy.exe --help` currently starts the stdio proxy and logs startup to stderr instead of printing usage. Script proxies also document usage in comments but do not expose a first-class help path. | Operators and agents cannot safely inspect proxy configuration without accidentally entering the MCP stdio loop. |
| Proxy call-log env drift | `Tools\MonolithProxy\README.md` and `Docs/SPEC_CORE.md` describe `Saved/Logs/MonolithCalls.jsonl` for both proxies, but `MONOLITH_CALL_LOG` / `MONOLITH_PROJECT_ROOT` are implemented in `monolith_proxy.cpp`; Python/Node script proxies expose daily `MONOLITH_TOOL_LOG_*` logging and tool-cache paths instead. | Proxy help must report runtime-specific support instead of implying every launcher supports every env var. |
| Python offline fallback | `Scripts\monolith_offline.py` uses `argparse` and exposes some help, but its surface is stale relative to `monolith_query.exe`: top-level `--help` can fail on Windows CP949 because the docstring contains non-ASCII punctuation, `bridge` is unknown, and modern source/project review actions such as `source impact_radius` are missing. | Existing docs call it a byte-identical fallback, but current code does not satisfy that claim. |
| Agent scripts | Maintenance scripts such as `check_offline_exe_fresh.py`, `verify_offline_parity.py`, `ci_static_checks.py`, `index_project.py`, `tag_path_params.py`, and `make_release.ps1` already have argparse/PowerShell help or narrowly scoped usage comments. | They are useful verification tools, but they are not Monolith runtime binaries and should not be mixed into the binary help contract. |
| Existing docs | `SPEC_MonolithSource.md`, `SPEC_MonolithIndex.md`, `AGENTS.md`, and the default DB verification doc already define DB locations and preferred offline commands. | Help should reuse these contracts, not invent new behavior. |

## 3. User Stories

| Story | Required result |
|-------|-----------------|
| Agent has no editor/MCP connection and needs source triage syntax. | `monolith_query.exe source --help` prints high-value source commands and exits 0 without touching a DB. |
| Agent wants the exact options for a review action. | `monolith_query.exe source review_context --help` prints required seed, optional params, defaults, examples, and output keys. |
| Agent uses PowerShell-style separated args. | `--max-results 10`, `--detail-level standard`, and `--include-counts false` parse exactly like their `--key=value` forms. |
| Agent mistypes a namespace or action. | The error prints nearest valid names and points to the matching `--help` command before any DB open. |
| Agent works from copied DBs. | Help explains default DB resolution and `--db`, `--source-db`, `--project-db`, and `--graph-db` override rules. |

## 4. Requirements

### 4.1 Help Entry Points

The CLI must support these DB-free forms:

| Command | Meaning | Exit |
|---------|---------|------|
| `monolith_query.exe --help` | Top-level help index | 0 |
| `monolith_query.exe -h` | Top-level help index | 0 |
| `monolith_query.exe help` | Top-level help index | 0 |
| `monolith_query.exe help <namespace>` | Namespace help | 0 when namespace is known |
| `monolith_query.exe <namespace> --help` | Namespace help | 0 when namespace is known |
| `monolith_query.exe <namespace> -h` | Namespace help | 0 when namespace is known |
| `monolith_query.exe <namespace> help` | Namespace help | 0 when namespace is known |
| `monolith_query.exe help <namespace> <action>` | Action help | 0 when namespace/action is known |
| `monolith_query.exe <namespace> <action> --help` | Action help | 0 when namespace/action is known |
| `monolith_query.exe <namespace> <action> -h` | Action help | 0 when namespace/action is known |
| `monolith_query.exe <namespace> help <action>` | Action help | 0 when namespace/action is known |

`argc == 1` may keep the current compact usage/error behavior, but explicit help flags must be success paths.

### 4.2 Help Content Contract

Top-level help must include:

- Offline namespaces: `source`, `project`, `bridge`, `monolith`, `cppreflect`, `network`, `decision`, `risk`.
- One-line summary per namespace.
- Common DB rules:
  - default DBs resolve from the executable location;
  - `source` and RI namespaces read `Saved\EngineSource.db`;
  - `project` reads `Saved\ProjectIndex.db`;
  - `bridge` reads both;
  - source CRG graph actions read or write `Saved\graph.db`;
  - `--db`, `--source-db`, `--project-db`, and `--graph-db` override copied or non-standard DBs.
- Logging controls: `MONOLITH_TOOL_LOG_ENABLED=0` disables query logs; `MONOLITH_TOOL_LOG_DIR` redirects them.
- Query trace/log context variables: `MONOLITH_TOOL_LOG_MAX_FIELD_BYTES`, `MONOLITH_TRACE_ID`, `MONOLITH_PARENT_SPAN_ID`, and `MONOLITH_VERSION`.
- Examples for the highest ROI workflows: health, source search, review context, project search, bridge asset-symbol search, and CRG graph health.

Namespace help must include:

- Namespace description.
- Backing DBs and whether help itself is DB-free.
- Action list grouped by workflow, not just alphabetical order.
- A short "start here" section with 3 to 6 examples.
- Mutating/offline-write warnings for execute-gated actions: `repair_fts`, `repair_crg_cache`, `build_crg_graph --execute`, `snapshot --execute`.

Action help must include:

- Full command syntax.
- Required positionals and accepted option aliases.
- Defaults and enum values for options.
- Whether the action is read-only, dry-run by default, or execute-gated.
- Primary output keys.
- One or more copy-ready examples.
- Related follow-up commands when already known in the action result contract.

### 4.3 Help Catalog As Source Of Truth

The implementation must not add another unsynchronized hardcoded usage blob. Add an explicit local help/action descriptor catalog and use it for:

1. Top-level help rendering.
2. Namespace help rendering.
3. Action help rendering.
4. Known action checks before opening DBs.
5. Option arity parsing.
6. Unknown namespace/action suggestions.

The existing dispatch maps may remain, but the first slice must add a static verification path that detects catalog/dispatch drift for every offline namespace. A lightweight in-process self-check or script is acceptable.

### 4.4 Option Parsing Contract

The parser must support all advertised option spelling forms:

```powershell
--max-results=10
--max-results 10
--max_results=10
--max_results 10
```

For boolean options, the parser must distinguish flags from explicit boolean values:

| Form | Result |
|------|--------|
| `--execute` | `execute=true` |
| `--execute=true` | `execute=true` |
| `--execute false` | `execute=false` |
| `--include-counts=false` | `include_counts=false` |
| `--include-counts false` | `include_counts=false` |

Boolean flags must not accidentally consume a positional argument. A following token is a boolean value only when it is one of `true`, `false`, `1`, `0`, `yes`, or `no` case-insensitively.

At minimum, P0 must correctly type all options already read by `Args::opt`, `Args::opt_int`, `Args::opt_double`, and `Args::opt_bool`, including:

```text
asset_path, before, blueprint_callable_only, blueprint_visible_only,
changed_paths, class_name, context_lines, cursor, decision_id,
dependency_type, depth, detail_level, diff_file, diff_stdin, direction,
edge_kinds, end, execute, file_path, force, graph_db, include_content,
include_counts, include_questions, include_unused, interface_name, kind,
label, limit, macro_filter, max_age_days, max_depth, max_lines, max_results,
min_confidence, min_lines, min_tier, module, module_name, no_header,
path_filter, project_db, ranges, ref_kind, repo_tag, rpc_kind, scope,
section, seed, since_unix, source_db, specifier_name, start, status, symbol,
target, unused_limit
```

### 4.5 Unknown Name Recovery

Unknown namespace and unknown action handling must be DB-free when the failure can be determined from the descriptor catalog.

Required behavior:

- Unknown namespace output includes the full offline namespace list including `bridge`.
- Unknown action output includes sibling action suggestions.
- The message includes the relevant help command, for example `monolith_query.exe source --help`.
- `source`, `project`, and `bridge` get suggestion behavior comparable to the RI namespaces.
- Exit behavior should be consistent. Prefer nonzero for unknown namespace/action errors unless a compatibility audit finds a caller depending on the RI exit-0 shape.

### 4.6 Compatibility And Side Effects

- Help requests must not open SQLite DBs, recover rollback journals, create graph lock files, or execute write paths.
- Existing successful action JSON output must not change.
- Existing `--version` JSON output must remain unchanged.
- Existing inline option forms must remain compatible.
- Daily query logging may record help invocations, but help logging must preserve stdout/stderr/exit semantics.
- Do not add live-only namespaces to offline help unless `monolith_query.exe` can execute them.

### 4.7 Companion Binary Help Baseline

All Monolith command-line entrypoints shipped or documented for agents must have a safe explicit help path. For this checkout, that means:

| Entrypoint | Required help forms |
|------------|---------------------|
| `Binaries\monolith_query.exe` | Covered by sections 4.1 through 4.6. |
| `Binaries\monolith_proxy.exe` | `--help`, `-h`, and `help`. |
| `Scripts\monolith_proxy.py` | `--help`, `-h`, and `help`. |
| `Scripts\monolith_proxy.js` | `--help`, `-h`, and `help`. |
| `Scripts\monolith_proxy.bat` | `--help`, `-h`, and `help`, forwarding to the same text without probing runtimes unnecessarily when possible. |
| `Scripts\monolith_proxy.sh` | `--help`, `-h`, and `help`, forwarding to the same text without probing runtimes unnecessarily when possible. |
| `Scripts\monolith_offline.py` | Either parity help with `monolith_query.exe` or an explicit deprecation/limited-fallback help message. |

Proxy help must:

- Exit 0.
- Print to stdout, with no startup banner on stderr except unavoidable launcher diagnostics.
- Not start the stdio JSON-RPC read loop.
- Not start background health polling.
- Not contact `MONOLITH_URL` or `http://localhost:9316/mcp`.
- Not open `Saved/Logs/MonolithCalls.jsonl`, `Logs/yyyyMMdd/proxy.jsonl`, or proxy tool-cache files.
- Explain the proxy's role: stdio-to-HTTP MCP bridge for the editor-hosted Monolith server.
- Show default `MONOLITH_URL=http://localhost:9316/mcp`.
- List relevant environment variables with per-runtime support notes:
  - `MONOLITH_URL`
  - `MONOLITH_SPLIT_EDITOR_QUERY`
  - `MONOLITH_EDITOR_ACTION_ALLOWLIST`
  - `MONOLITH_EDITOR_ACTION_DENYLIST`
  - `MONOLITH_TOOL_LOG_ENABLED`
  - `MONOLITH_TOOL_LOG_DIR`
  - `MONOLITH_TOOL_LOG_MAX_FIELD_BYTES`
  - `MONOLITH_CALL_LOG` (native C++ proxy call log)
  - `MONOLITH_PROJECT_ROOT` (native C++ proxy call-log path root)
  - `LOCALAPPDATA` / temp directory fallback for script-proxy tool cache paths
- Show minimal MCP config examples for native proxy and script launcher.
- Explain that `monolith_query.exe` is the offline DB query tool when the editor or MCP server is unavailable.

Proxy `--version` is a P0 companion requirement for `monolith_proxy.exe`, `monolith_proxy.py`, and `monolith_proxy.js`. It must exit 0 and print a compact version string or JSON containing at least `tool` and `version`. Launcher scripts may forward `--version` to the selected runtime implementation.

### 4.8 Python Offline Fallback Contract

`Scripts\monolith_offline.py` must not remain ambiguously documented as byte-identical unless it actually is. Choose one of these outcomes in the implementation slice:

| Outcome | Required behavior |
|---------|-------------------|
| Keep as fallback parity | Mirror `monolith_query.exe` help entrypoints, namespaces, actions, option spellings, and `--version` identity closely enough for `Scripts\verify_offline_parity.py` to verify the shared surface. |
| Keep as limited legacy fallback | Top-level help and docs must say it is a limited developer fallback; list the exact supported namespaces/actions; route agents to `Binaries\monolith_query.exe` for current `bridge`, source review, project review, CRG, and reflection-intelligence parity. |
| Remove from public agent guidance | Update docs to stop recommending it as an agent fallback; keep only test/parity tooling references if still needed internally. |

Regardless of outcome:

- `python Scripts\monolith_offline.py --help` must exit 0 on Windows PowerShell with the default console encoding.
- Help text must avoid non-ASCII punctuation that fails under CP949, or explicitly force UTF-8-safe output before printing.
- `--version` must continue to be machine-readable enough for `Scripts\verify_offline_parity.py`.
- If the script remains listed in `Docs/MONOLITH_GUIDE.md` or `Docs/API_REFERENCE.md`, those docs must match the chosen outcome.

## 5. First-Slice Catalog Scope

P0 covers all namespaces currently executable by `monolith_query.exe`:

| Namespace | P0 help coverage |
|-----------|------------------|
| `source` | All 27 dispatch entries, including alias note for `repair_crg_graph -> rebuild_crg_graph`. |
| `project` | All 17 dispatch entries. Do not include live-only `list_gameplay_tags` or `search_gameplay_tags` unless offline dispatch is added separately. |
| `bridge` | `search_asset_symbols`. |
| `monolith` | `guide`, including section names. |
| `cppreflect` | 6 RI actions. |
| `network` | 4 RI actions. |
| `decision` | 5 RI actions. |
| `risk` | 5 RI actions. |

The descriptor catalog should mark live-editor-only Monolith namespaces as out of scope rather than listing them as offline commands.

P0 also covers shipped/documented Monolith binary help:

| Binary or launcher | P0 help coverage |
|--------------------|------------------|
| `monolith_proxy.exe` | Native stdio-to-HTTP proxy usage, env vars, no-loop help behavior, version. |
| `monolith_proxy.py` | Same proxy usage and env vars, no-loop help behavior, version. |
| `monolith_proxy.js` | Same proxy usage and env vars, no-loop help behavior, version. |
| `monolith_proxy.bat` / `monolith_proxy.sh` | Thin launcher help that identifies runtime selection and points at native/script proxy usage. |
| `monolith_offline.py` | Explicit parity-or-limited-fallback help decision and Windows-safe top-level help. |

## 6. High-ROI Additions From Survey

| Priority | Addition | Reason |
|----------|----------|--------|
| P0 | Complete option arity parsing from descriptors. | Prevents silent defaulting in common agent commands such as `--max-results 10` and `--detail-level standard`. |
| P0 | DB-free unknown action validation. | Avoids "DB not found" masking a simple typo or help request. |
| P0 | Include `bridge` in offline namespace list and top-level help. | Current unknown namespace diagnostics hide an executable namespace. |
| P0 | Add source/project/bridge suggestions. | RI already has a suggestion pattern; applying it to the high-traffic namespaces improves recovery. |
| P0 | Add safe proxy binary/launcher help and version. | `monolith_proxy.exe --help` currently starts proxy mode; explicit help should be inspection-only. |
| P0 | Resolve `monolith_offline.py` fallback truthfulness. | Current code and docs disagree; agents should not be sent to a stale fallback for `bridge` or CRG/source-review actions. |
| P1 | `--help-json` or `--help --format=json`. | Lets agents and docs tooling consume the same catalog without scraping text. |
| P1 | `monolith_query.exe list_actions --json`. | Useful for skill generation and offline catalog drift checks. |
| P1 | Descriptor-to-doc sync check against `Docs/specs/SPEC_MonolithSource.md` and `SPEC_MonolithIndex.md`. | Catches docs drift without blocking the first help slice. |

## 7. Implementation Plan

1. Edit `Tools/MonolithQuery/monolith_query.cpp` to add small descriptor structs for namespaces, actions, positionals, options, examples, output keys, mutability, and DB scope.
2. Populate descriptors for every P0 offline namespace and action listed in section 5, using the current dispatch maps and action handler `Args::opt*` reads as the ground truth.
3. Add an early help parser in `main` before DB resolution and before action DB open. Handle top-level, namespace-level, and action-level help requests.
4. Replace the hardcoded `argc < 3` usage blob with descriptor-rendered top-level help, preserving compact usage for malformed/no-arg calls.
5. Replace the partial `value_options` set with descriptor-derived option arity, including optional boolean value handling.
6. Add descriptor-driven known namespace/action checks before DB open; unknown names should print suggestions and help pointers.
7. Add a local verification script or self-check that compares descriptor action names with the dispatch tables for `source`, `project`, `bridge`, `monolith`, `cppreflect`, `network`, `decision`, and `risk`.
8. Add safe early `--help` / `--version` handling to `Tools\MonolithProxy\monolith_proxy.cpp`, `Scripts\monolith_proxy.py`, `Scripts\monolith_proxy.js`, and the launcher scripts without entering MCP loop mode.
9. Decide and implement the `Scripts\monolith_offline.py` outcome from section 4.8, then sync `Docs/MONOLITH_GUIDE.md`, `Docs/API_REFERENCE.md`, and parity tooling text if needed.
10. Rebuild the offline executable with `Tools\MonolithQuery\build.bat`.
11. Rebuild the native proxy executable with `Tools\MonolithProxy\build.bat`.
12. Run `python Scripts\check_offline_exe_fresh.py` and `python Scripts\verify_offline_parity.py` when the Python fallback is kept as parity.
13. Run `python Scripts/ci_static_checks.py --config .github/monolith-static-ci.json --github check`.
14. Run verification commands for help, parser parity, proxy help, proxy version, offline fallback help, and unknown-name recovery, then run `git diff --check` and `git status --short`.
15. Complete pre-commit steps to ensure proper testing, verification, review, and reflection are done.

## 8. Verification Gates

| Gate | Required evidence |
|------|-------------------|
| Top-level help success | `Binaries\monolith_query.exe --help` exits 0 and prints all offline namespaces including `bridge`. |
| Namespace help success | `source --help`, `project --help`, `bridge --help`, `monolith --help`, `cppreflect --help`, `network --help`, `decision --help`, and `risk --help` all exit 0. |
| Action help success | At minimum `source impact_radius --help`, `source review_context --help`, `project search --help`, `project pre_merge_check --help`, `bridge search_asset_symbols --help`, and `cppreflect get_uclass --help` exit 0. |
| Help is DB-free | `source --help --db X:\definitely\missing`, `source impact_radius --help --db X:\definitely\missing`, and `bridge --help --source-db X:\missing --project-db X:\missing` still exit 0. |
| Parser parity | `source impact_radius UObject --max-depth 1 --max-results 1` produces the same `input.max_depth=1` and `input.max_results=1` as the inline `--max-depth=1 --max-results=1` form. |
| Boolean value parity | `source health --include-counts false` matches `source health --include-counts=false`; `project pre_merge_check X --include-unused false` matches inline false. |
| Unknown namespace recovery | `monolith_query.exe sorce health` fails before DB open, suggests `source`, and prints `monolith_query.exe --help` or `source --help` guidance. |
| Unknown action recovery | `monolith_query.exe source review_contxt` fails before DB open, suggests `review_context`, and prints `monolith_query.exe source --help`. |
| No output drift | Representative non-help commands keep their JSON shape: `source health --include-counts=false`, `project health --include-counts=false`, and `bridge search_asset_symbols --symbol=UObject --limit=1`. |
| Proxy help success | `Binaries\monolith_proxy.exe --help`, `python Scripts\monolith_proxy.py --help`, `node Scripts\monolith_proxy.js --help`, `Scripts\monolith_proxy.bat --help`, and `Scripts\monolith_proxy.sh --help` exit 0 where the runtime exists. |
| Proxy help is inert | Proxy help commands do not contact `MONOLITH_URL`, start health polling, enter the stdio MCP loop, or create proxy/query log files. |
| Proxy version success | `Binaries\monolith_proxy.exe --version`, `python Scripts\monolith_proxy.py --version`, and `node Scripts\monolith_proxy.js --version` exit 0 and print tool/version identity. |
| Python fallback truthfulness | `python Scripts\monolith_offline.py --help` exits 0 on Windows. If kept as parity, it supports `bridge --help` and current source/project review actions; if kept as limited fallback, help/docs clearly say what it does not support. |
| Docs sync | `Docs/MONOLITH_GUIDE.md` and `Docs/API_REFERENCE.md` no longer claim `monolith_offline.py` is byte-identical unless parity verification proves it. |
| Freshness/parity guard | `python Scripts\check_offline_exe_fresh.py` passes after query rebuild; `python Scripts\verify_offline_parity.py` passes when the Python fallback is kept as parity, or is updated to reflect limited-fallback scope. |
| Build | `Tools\MonolithQuery\build.bat` succeeds and copies the binary to `Binaries\monolith_query.exe`; `Tools\MonolithProxy\build.bat` succeeds and copies the binary to `Binaries\monolith_proxy.exe`. |
| Static checks | `python Scripts/ci_static_checks.py --config .github/monolith-static-ci.json --github check` passes or reports only documented nonblocking advisories. |
| Whitespace/status | `git diff --check` passes and `git status --short` shows only intended files plus any pre-existing user changes. |

## 8a. Implementation Evidence

Implemented in the first slice:

- `Tools/MonolithQuery/monolith_query_help.h` plus `Tools/MonolithQuery/monolith_query.cpp`: descriptor-backed help catalog, DB-free help paths, descriptor-derived option arity, catalog self-check, and DB-free unknown namespace/action suggestions.
- `Tools/MonolithProxy/monolith_proxy_help.h`, `Tools/MonolithProxy/monolith_proxy.cpp`, and `Scripts/monolith_proxy.*`: safe proxy `--help` / `--version` paths that return before stdio loop, health polling, and proxy logging.
- `Scripts/monolith_offline.py`: limited legacy fallback help contract, Windows-safe help output, and native-exe routing for unsupported current namespaces such as `bridge`.
- `Docs/MONOLITH_GUIDE.md` and `Docs/API_REFERENCE.md`: offline fallback docs now identify `Binaries/monolith_query.exe` as canonical and `monolith_offline.py` as limited legacy fallback.
- `Scripts/verify_offline_parity.py`: RI parity verifier now skips decision-id-dependent actions when the active DB corpus has no decision records instead of failing with empty args.

Verified on 2026-06-03:

- `Tools\MonolithQuery\build.bat`
- `Tools\MonolithProxy\build.bat`
- `python Scripts\check_offline_exe_fresh.py` verifies the combined query source hash for `monolith_query.cpp` and `monolith_query_help.h`.
  - Wired into `Scripts\ci_static_checks.py` as the config-gated `offline_exe_freshness` check (`.github/monolith-static-ci.json`) so a stale shipped `monolith_query.exe` is a **blocker** (6A binary-lag gate, `SPEC_MonolithToolCallReliabilityBacklog.md` §6A). The check reuses this script's hashed source list, and gracefully **advisory-skips** when the exe is absent (local/release artifact) or cannot be executed on the CI host (e.g. a Windows PE binary on a Linux runner); only a present, runnable, hash-mismatched exe blocks.
- `Binaries\monolith_query.exe --catalog-self-check`
- Query help matrix for top-level, namespace, and action help forms, including DB-missing help paths.
- Parser parity for inline and space-separated numeric/boolean options.
- Proxy native/Python/Node/batch/shell help and version paths with isolated log directory proving no proxy log files were created.
- `python Scripts\check_offline_exe_fresh.py`
- `python Scripts\verify_offline_parity.py` (`17/17 MATCH`, `0 DIFF`, `0 ERROR`, `3 SKIP` because the active DB has no `decision_id` corpus input)
- `python -m py_compile Scripts\monolith_proxy.py Scripts\monolith_offline.py Scripts\verify_offline_parity.py Scripts\check_offline_exe_fresh.py`
- `node --check Scripts\monolith_proxy.js`
- `python Scripts\ci_static_checks.py --config .github\monolith-static-ci.json --github check` (`Blocking findings: 0`; existing advisory CRLF / `.claude/agents` warnings remain)
- `git diff --check`

## 9. Follow-Up Slices

| Follow-up | Reason to defer |
|-----------|-----------------|
| Machine-readable help catalog | Valuable for agents, but text help and parser correctness unblock immediate CLI usage. |
| Docs sync generator | Requires deciding whether specs or the descriptor catalog own examples and output key summaries. |
| Live registry export parity | Needs editor-backed access and must not imply live-only actions work offline. |
| Shell completions | Useful after descriptor metadata stabilizes. |
| Action result schema summaries | Larger effort; current output-key summaries are enough for first-slice orientation. |
