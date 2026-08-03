# Monolith Query CLI Help First Slice

**Parent:** [../SPEC_CORE.md](../SPEC_CORE.md)
**Engine:** Unreal Engine 5.7+
**Status:** Implemented first slice
**Owner module:** `Tools/MonolithQuery`
**Scope:** Add DB-free namespace/action help for `monolith_query.exe`, fix the CLI option parsing drift exposed by that work, and define a companion help baseline for Monolith proxy binaries/launchers.
**Non-goals:** Live MCP `monolith_discover` changes, domain action output shape changes, unrelated DB schema changes, editor-only namespace help, replacing `monolith guide`, or changing MCP proxy protocol behavior.

---

## 1. Purpose

Agents use `Binaries\monolith_query.exe` when the MCP server or Unreal Editor is unavailable. The current CLI is useful but hard to self-orient from the command line: only top-level usage prints, namespace help fails, action-level help is absent, and several common space-separated options silently fall back to defaults.

This first slice makes the offline tool self-service:

- `monolith_query.exe --help` explains the offline surface.
- `monolith_query.exe source --help`, `project --help`, `bridge --help`, `console --help`, and the RI namespaces print DB-free namespace help.
- `monolith_query.exe source impact_radius --help` and equivalent action requests print action syntax, defaults, examples, output keys, and DB behavior.
- Space-separated options accepted by the advertised syntax behave the same as `--key=value`.

The help catalog must describe the offline executable, not the live editor registry. If an action is not in the executable dispatch table, it must not appear as executable offline help.

`bridge` is not a separate executable in the current checkout; it is the `monolith_query.exe bridge ...` namespace. The shipped command-line control plane under `Binaries\` is the immutable `monolith_query-<source-hash>.exe` plus `monolith_catalog-<semantic-hash>.json` selected together by `monolith_query.current.json`, and the immutable `monolith_proxy-<source-hash>.exe` selected by `monolith_proxy.current.json` (plus the watchdog wrapper where included). The historical fixed Query and Proxy names are compatibility-only and are not the authoritative release-selected images. This spec therefore covers both:

- Query help for offline DB-backed namespaces, including `bridge`.
- Proxy help for the manifest-selected immutable native proxy and the script proxy launchers that agents may configure in MCP clients.
- Compatibility guidance for `Scripts\monolith_offline.py`, which remains documented as a developer fallback but currently lags the native executable.

## 2. Pre-Implementation Code Facts

The source survey for this spec was based on `Tools/MonolithQuery/monolith_query.cpp` and current `Binaries\monolith_query.exe` behavior.

| Area | Current fact | Impact |
|------|--------------|--------|
| File shape | `monolith_query.cpp` is a single 9284-line standalone CLI with parser, dispatch, DB helpers, output logging, and action handlers. | Help metadata should be local and lightweight; avoid a separate parser stack. |
| Top-level help | Usage text is hardcoded in `parse_args` and only prints through the `argc < 3` path. Current `monolith_query.exe --help` prints usage but exits nonzero. | Help is treated like an error and cannot be cleanly invoked by automation. |
| Namespace help | `monolith_query.exe source --help` is parsed as namespace `source`, action `--help`; it opens `EngineSource.db`, then fails as an unknown source action. | Help depends on DB availability and fails for the exact syntax agents expect. |
| Action dispatch | Current offline dispatch includes `source` entries including `repair_crg_graph` alias, `project`, `bridge`, `console`, `monolith`, and RI namespaces `cppreflect`, `network`, `decision`, `risk`. | Help must be generated or checked against dispatch tables to avoid drift. |
| Namespace list | `offline_namespaces()` must return every executable offline namespace, including `bridge` and `console`. | Unknown namespace errors must report the full offline surface. |
| Action errors | Unknown namespace/action handling should suggest nearby valid names before any DB open. | Agent recovery is inconsistent without the already-available fuzzy helper pattern. |
| Option arity | `value_options` covers only part of the options read later by `opt`, `opt_int`, and `opt_bool`. | `--max-depth 1 --max-results 1` currently runs as defaults `max_depth=2`, `max_results=200`; inline `--max-depth=1 --max-results=1` works. |
| Shipped executables | `Binaries\` contains a manifest-selected immutable Query/catalog pair and a manifest-selected immutable Proxy. The fixed `monolith_query.exe` is a best-effort documentation compatibility copy; release staging writes it from the selected immutable bytes, while a local build may leave an older locked compatibility image in place. The fixed proxy name is compatibility-only and is not released. | The spec should not imply a separate bridge executable exists or treat either mutable fixed name as runtime authority. |
| Proxy help | The manifest-selected native proxy must make `--help` inert; script proxies also document usage in comments and expose their own help path. | Operators and agents must be able to inspect configuration without entering the MCP stdio loop. |
| Proxy call-log env drift | `Tools\MonolithProxy\README.md` and `Docs/SPEC_CORE.md` describe `Saved/Logs/MonolithCalls.jsonl` for both proxies, but `MONOLITH_CALL_LOG` / `MONOLITH_PROJECT_ROOT` are implemented in `monolith_proxy.cpp`; Python/Node script proxies expose daily `MONOLITH_TOOL_LOG_*` logging and tool-cache paths instead. | Proxy help must report runtime-specific support instead of implying every launcher supports every env var. |
| Python offline fallback | `Scripts\monolith_offline.py` uses `argparse` and exposes some help, but its surface is stale relative to `monolith_query.exe`: top-level `--help` can fail on Windows CP949 because the docstring contains non-ASCII punctuation, `bridge` is unknown, and modern source/project review actions such as `source impact_radius` are missing. | Existing docs call it a byte-identical fallback, but current code does not satisfy that claim. |
| Agent scripts | Maintenance scripts such as `check_offline_exe_fresh.py`, `verify_offline_parity.py`, `ci_static_checks.py`, `index_project.py`, `tag_path_params.py`, and `make_release.ps1` already have argparse/PowerShell help or narrowly scoped usage comments. | They are useful verification tools, but they are not Monolith runtime binaries and should not be mixed into the binary help contract. |
| Existing docs | `SPEC_MonolithSource.md`, `SPEC_MonolithIndex.md`, `AGENTS.md`, and the default DB verification doc already define DB locations and preferred offline commands. | Help should reuse these contracts, not invent new behavior. |

The table above is retained as pre-implementation evidence. Its `repair_crg_graph` alias and graph-specific DB observations describe the retired export implementation; they are not current help or dispatch requirements. The current catalog routes `source.search_crg_graph` to `EngineSource.db` and exposes no graph build/rebuild/health alias or `--graph-db` option.

## 3. User Stories

| Story | Required result |
|-------|-----------------|
| Agent has no editor/MCP connection and needs source triage syntax. | `monolith_query.exe source --help` prints high-value source commands and exits 0 without touching a DB. |
| Agent wants the exact options for a review action. | `monolith_query.exe source review_context --help` prints required seed, optional params, defaults, examples, and output keys. |
| Agent uses PowerShell-style separated args. | `--max-results 10`, `--detail-level standard`, and `--include-counts false` parse exactly like their `--key=value` forms. |
| Agent mistypes a namespace or action. | The error prints nearest valid names and points to the matching `--help` command before any DB open. |
| Agent works from copied DBs. | Help explains default DB resolution and the `--db`, `--source-db`, and `--project-db` override rules. `source.search_crg_graph` follows the EngineSource selection and has no graph-specific override. |

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

- Offline namespaces: `source`, `project`, `bridge`, `console`, `monolith`, `cppreflect`, `network`, `decision`, `risk`.
- One-line summary per namespace.
- Common DB rules:
  - default DBs resolve from the executable location;
  - `source`, `console`, and RI namespaces read `Saved\EngineSource.db`;
  - `project` reads `Saved\ProjectIndex.db`;
  - `bridge` reads both;
  - every `source` action, including `search_crg_graph`, reads `Saved\EngineSource.db`;
  - `--db`, `--source-db`, and `--project-db` override copied or non-standard DBs; `--graph-db` is not a supported option.
- Logging controls: `MONOLITH_TOOL_LOG_ENABLED=0` disables query logs; `MONOLITH_TOOL_LOG_DIR` redirects them.
- Query trace/log context variables: `MONOLITH_TOOL_LOG_MAX_FIELD_BYTES`, `MONOLITH_TRACE_ID`, `MONOLITH_PARENT_SPAN_ID`, and `MONOLITH_VERSION`.
- Examples for the highest ROI workflows: health, source search, EngineSource graph-node search, review context, project search, and bridge asset-symbol search.

Namespace help must include:

- Namespace description.
- Backing DBs and whether help itself is DB-free.
- Action list grouped by workflow, not just alphabetical order.
- A short "start here" section with 3 to 6 examples.
- Mutating/offline-write warnings for execute-gated actions: `repair_fts`, `repair_crg_cache`, and `snapshot --execute`.

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
edge_kinds, end, execute, file_path, include_content,
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

- Unknown namespace output includes the full offline namespace list including `bridge` and `console`.
- Unknown action output includes sibling action suggestions.
- The message includes the relevant help command, for example `monolith_query.exe source --help`.
- `source`, `project`, `bridge`, and `console` get suggestion behavior comparable to the RI namespaces.
- Exit behavior should be consistent. Prefer nonzero for unknown namespace/action errors unless a compatibility audit finds a caller depending on the RI exit-0 shape.

### 4.6 Compatibility And Side Effects

- Help requests must not open SQLite DBs, recover rollback journals, create graph lock files, or execute write paths.
- Existing successful action JSON output must not change.
- Existing `--version` JSON output must remain unchanged.
- Existing inline option forms must remain compatible.
- Daily query logging may record help invocations, but help logging must preserve stdout/stderr/exit semantics.
- Do not add live-only namespaces to offline help unless `monolith_query.exe` can execute them.
- `source|project repair_crg_cache --execute` must begin one immediate transaction before creating CRG tables or indexes, prove main-database write access with a no-op base-table write, and keep schema creation plus cache replacement inside that transaction. If UnrealEditor, an indexer, or another process denies write sharing, the action returns structured `write_preflight.status="unavailable"` remediation and leaves no partial CRG schema or cache changes.

### 4.7 Companion Binary Help Baseline

All Monolith command-line entrypoints shipped or documented for agents must have a safe explicit help path. For this checkout, that means:

| Entrypoint | Required help forms |
|------------|---------------------|
| `Binaries\monolith_query.exe` | Covered by sections 4.1 through 4.6. |
| Manifest-selected `Binaries\monolith_proxy-<source-hash>.exe` | `--help`, `-h`, and `help`. |
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
| `source` | Every current source dispatch entry, including EngineSource-backed `search_crg_graph` and the C++ authoring-ergonomics actions `get_include_path`, `get_signature`, `check_deprecations`, `verify_symbols`, `find_example_usage`, `lint_header`, and `generate_class_stub` (plain-text output, byte-parity-gated against `Scripts/monolith_offline.py`). The catalog must not expose removed `build_crg_graph`, `rebuild_crg_graph`, `repair_crg_graph`, or `crg_graph_health` entries. |
| `project` | All 17 dispatch entries. Do not include live-only `list_gameplay_tags` or `search_gameplay_tags` unless offline dispatch is added separately. |
| `bridge` | `search_asset_symbols`. |
| `console` | Snapshot-backed `search_objects`, `get_object`, and `health`; `refresh_snapshot` and `execute` are documented live-only guidance actions. |
| `monolith` | `guide`, including section names. |
| `cppreflect` | 6 RI actions. |
| `network` | 4 RI actions. |
| `decision` | 5 RI actions. |
| `risk` | 5 RI actions. |

The descriptor catalog should mark live-editor-only Monolith namespaces as out of scope rather than listing them as offline commands.

P0 also covers shipped/documented Monolith binary help:

| Binary or launcher | P0 help coverage |
|--------------------|------------------|
| `monolith_proxy-<source-hash>.exe` selected by `monolith_proxy.current.json` | Native stdio-to-HTTP proxy usage, env vars, no-loop help behavior, version. |
| `monolith_proxy.py` | Same proxy usage and env vars, no-loop help behavior, version. |
| `monolith_proxy.js` | Same proxy usage and env vars, no-loop help behavior, version. |
| `monolith_proxy.bat` / `monolith_proxy.sh` | Thin launcher help that identifies runtime selection and points at native/script proxy usage. |
| `monolith_offline.py` | Explicit parity-or-limited-fallback help decision and Windows-safe top-level help. |

## 6. High-ROI Additions From Survey

| Priority | Addition | Reason |
|----------|----------|--------|
| P0 | Complete option arity parsing from descriptors. | Prevents silent defaulting in common agent commands such as `--max-results 10` and `--detail-level standard`. |
| P0 | DB-free unknown action validation. | Avoids "DB not found" masking a simple typo or help request. |
| P0 | Include `bridge` and `console` in offline namespace list and top-level help. | Unknown namespace diagnostics must expose every executable namespace. |
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
| Top-level help success | `Binaries\monolith_query.exe --help` exits 0 and prints all offline namespaces including `bridge` and `console`. |
| Namespace help success | `source --help`, `project --help`, `bridge --help`, `console --help`, `monolith --help`, `cppreflect --help`, `network --help`, `decision --help`, and `risk --help` all exit 0. |
| Action help success | At minimum `source impact_radius --help`, `source review_context --help`, `project search --help`, `project pre_merge_check --help`, `bridge search_asset_symbols --help`, `console search_objects --help`, and `cppreflect get_uclass --help` exit 0. |
| Help is DB-free | `source --help --db X:\definitely\missing`, `source impact_radius --help --db X:\definitely\missing`, `console --help --db X:\missing`, and `bridge --help --source-db X:\missing --project-db X:\missing` still exit 0. |
| Parser parity | `source impact_radius UObject --max-depth 1 --max-results 1` produces the same `input.max_depth=1` and `input.max_results=1` as the inline `--max-depth=1 --max-results=1` form. |
| Boolean value parity | `source health --include-counts false` matches `source health --include-counts=false`; `project pre_merge_check X --include-unused false` matches inline false. |
| Unknown namespace recovery | `monolith_query.exe sorce health` fails before DB open, suggests `source`, and prints `monolith_query.exe --help` or `source --help` guidance. |
| Unknown action recovery | `monolith_query.exe source review_contxt` fails before DB open, suggests `review_context`, and prints `monolith_query.exe source --help`. |
| No output drift | Representative non-help commands keep their JSON shape: `source health --include-counts=false`, `project health --include-counts=false`, and `bridge search_asset_symbols --symbol=UObject --limit=1`. |
| Proxy help success | The executable selected by `Binaries\monolith_proxy.current.json --help`, `python Scripts\monolith_proxy.py --help`, `node Scripts\monolith_proxy.js --help`, `Scripts\monolith_proxy.bat --help`, and `Scripts\monolith_proxy.sh --help` exit 0 where the runtime exists. |
| Proxy help is inert | Proxy help commands do not contact `MONOLITH_URL`, start health polling, enter the stdio MCP loop, or create proxy/query log files. |
| Proxy version success | The manifest-selected immutable proxy `--version`, `python Scripts\monolith_proxy.py --version`, and `node Scripts\monolith_proxy.js --version` exit 0 and print tool/version identity; the native identity matches manifest tool/runtime/version/source-hash fields. |
| Python fallback truthfulness | `python Scripts\monolith_offline.py --help` exits 0 on Windows. If kept as parity, it supports `bridge --help` and current source/project review actions; if kept as limited fallback, help/docs clearly say what it does not support. |
| Docs sync | `Docs/MONOLITH_GUIDE.md` and `Docs/API_REFERENCE.md` no longer claim `monolith_offline.py` is byte-identical unless parity verification proves it. |
| Freshness/parity guard | `python Scripts\check_offline_exe_fresh.py` passes after query rebuild; `python Scripts\verify_offline_parity.py` passes when the Python fallback is kept as parity, or is updated to reflect limited-fallback scope. |
| Build | `Tools\MonolithQuery\build.bat` first hard-gates the generated catalog against current action registrations, then atomically publishes `Binaries\monolith_query-<source-hash>.exe`, `monolith_catalog-<semantic-hash>.json`, and the exact-field `monolith_query.current.json` manifest after executable/version/catalog/SHA validation. Updating fixed `monolith_query.exe` is best-effort compatibility only. `Tools\MonolithProxy\build.bat` independently publishes `Binaries\monolith_proxy-<source-hash>.exe` plus `monolith_proxy.current.json`; updating fixed `monolith_proxy.exe` is also best-effort compatibility only. |
| Static checks | Hosted CI first generates the ignored source snapshot at `Tools/MonolithQuery/Generated/monolith_catalog_snapshot.json`, then `python Scripts/ci_static_checks.py --config .github/monolith-static-ci.json --github check` runs the official generator in `--check` mode and blocks when that snapshot drifts from source registrations. The same pass rejects case-insensitive duplicate `.uplugin` plugin references and requires `Monolith.uplugin` `VersionName` to match the newest CHANGELOG release plus the version metadata in `Docs/SPEC_CORE.md`, `Docs/API_REFERENCE.md`, and README, so merge-only dependency duplication and action-count-only release downgrades cannot pass silently. Workflow-order validation prevents the checker from running before its generated input; immutable release artifacts remain outside source control. |
| Whitespace/status | `git diff --check` passes and `git status --short` shows only intended files plus any pre-existing user changes. |

## 8a. Implementation Evidence

Implemented in the first slice:

- `Binaries\monolith_query.current.json`: exact schema `schema_version`, `tool`, `runtime`, `file`, `plugin_version`, `parity_spec_rev`, `source_hash`, `sha256`, `catalog_file`, `catalog_source_hash`, and `catalog_sha256`. `file` is exactly `monolith_query-<16-char source_hash>.exe`; `catalog_file` is exactly `monolith_catalog-<64-char catalog_source_hash>.json`.
- `Tools/MonolithQuery/build.bat` and `publish_query_bundle.py`: `/MT /O2 /Brepro` makes one source generation byte-reproducible; publication verifies Query `--version` identity (`runtime=native-cpp`), recomputes the catalog semantic hash, validates both content SHA-256 values, refuses semantic immutable-name collisions, and atomically advances the current manifest only after the pair is complete. Nonsemantic catalog regeneration (`generated_at` or source-line provenance only) reuses the already-published immutable catalog bytes.
- `.github/workflows/ci.yml`, `.github/monolith-static-ci.json`, `Scripts/ci_static_checks.py`, and `Scripts/test_action_guidance_benchmark.py`: hosted static CI bootstraps the ignored source snapshot with `generate_monolith_catalog_snapshot.py`, then the source-contract tests and `offline_catalog_snapshot` gate consume and recheck that exact generated input. The generic required-token ordering guard blocks workflows that omit or reorder this prerequisite. Registry drift is caught before Query build/release without tracking generated source snapshots; release-version extraction also cross-checks the descriptor, CHANGELOG, core spec, API reference, and README. Immutable catalog leaves and `monolith_query.current.json` remain owned exclusively by the validated `publish_query_bundle.py` publication flow.
- `Scripts/source_generation_hash.py`: Query generation identity hashes the build-contract bytes plus the complete ordered text-input set after normalizing CRLF and lone CR to LF. This makes the same Git/P4 source content produce one generation across clean Git and P4 workspaces. Canonicalization applies only to source-generation identity; executable, catalog, and manifest SHA-256 values always cover the exact raw artifact bytes.
- Direct `Binaries\monolith_query-<source-hash>.exe` and fixed compatibility Query catalog actions validate the strict current manifest, selected immutable executable SHA, catalog leaf/SHA/semantic identity, and compiled source/version identity before serving the in-memory validated catalog. Invalid or mismatched bundles fail explicitly; an explicit trusted `--snapshot` remains the proxy/developer override. Source-tree developer builds outside `Binaries` retain the generated-snapshot default.
- `Scripts/make_release.ps1` independently recomputes the Query source hash through the same shared canonical helper contract, runs parity against the selected immutable image (not the fixed alias), and revalidates the same expected hash after allowlist staging.

- `Tools/MonolithQuery/monolith_query_help.h` plus `Tools/MonolithQuery/monolith_query.cpp`: descriptor-backed help catalog, DB-free help paths, descriptor-derived option arity, catalog self-check, and DB-free unknown namespace/action suggestions.
- `Tools/MonolithProxy/monolith_proxy_help.h`, `Tools/MonolithProxy/monolith_proxy.cpp`, and `Scripts/monolith_proxy.*`: safe proxy `--help` / `--version` paths that return before stdio loop, health polling, and proxy logging.
- `Scripts/monolith_offline.py`: limited legacy fallback help contract, Windows-safe help output, and native-exe routing for unsupported current namespaces such as `bridge`.
- `Docs/MONOLITH_GUIDE.md` and `Docs/API_REFERENCE.md`: public commands retain `Binaries/monolith_query.exe` as a compatibility path while production selection uses the validated immutable Query/catalog manifest; `monolith_offline.py` remains the limited legacy fallback.
- `Scripts/verify_offline_parity.py`: RI parity verifier now skips decision-id-dependent actions when the active DB corpus has no decision records instead of failing with empty args.

Verified on 2026-06-03:

- `Tools\MonolithQuery\build.bat`
- `Tools\MonolithProxy\build.bat`
- `python Scripts\check_offline_exe_fresh.py` verifies the full ordered Query input set through `Scripts/source_generation_hash.py`, including the shared LF/CRLF/lone-CR canonicalization contract.
  - Wired into `Scripts\ci_static_checks.py` as the config-gated `offline_exe_freshness` check (`.github/monolith-static-ci.json`) so a stale shipped `monolith_query.exe` is a **blocker** (6A binary-lag gate, `SPEC_MonolithToolCallReliabilityBacklog.md` §6A). The check reuses the build's exact contract and source list, and gracefully **advisory-skips** when the exe is absent (local/release artifact) or cannot be executed on the CI host (e.g. a Windows PE binary on a Linux runner); only a present, runnable, hash-mismatched exe blocks.
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
