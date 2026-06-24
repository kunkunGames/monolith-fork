# Monolith Agent Coordination and Conventions

This repository relies on several scheduled Jules tasks (agents) to maintain and optimize the codebase. To prevent collisions and ensure a clean history, all agents MUST follow these coordination rules:

## 1. Branch and PR Naming Conventions
Agents must use a strict, predictable branch and PR naming convention to make active work easily discoverable:
- **Branch Format:** `jules/<agent>/<module-or-area>/<short-behavior>` (Note: `<module-or-area>` and `<short-behavior>` are placeholders that must be replaced with descriptive text).
- **PR Title Format:** `<Emoji> <Agent>: <short description>` (e.g., `⚡ Bolt: [description]`, `🛡️ Sentinel: [description]`).
- **Avoid:** Non-standard branch prefixes like `bolt-...`, `perf-...`, `sentinel-...`, or raw `jules-<id>-...` branches, generic PR titles without agent prefixes, template-echo PR titles (e.g. '<Agent>: concise <domain> improvement.'), using literal strings like `short-topic` or `module-or-area` for placeholders, branch name evasion (such as pluralizing terms like `-counts` vs `-count`, appending `-2` / `-v2`), and appending any random numbers, large numeric task IDs, UUIDs, or timestamps to branch names (which defeats duplicate detection).

## 2. Duplicate / Collision Guard
Before making any changes, agents must perform a thorough duplicate and collision check. Because agents may run in concurrent VMs, you must avoid race conditions:
- Always run `git fetch origin --prune` immediately before checking branches.
- Use `git branch -r` and identify any existing `jules/<agent>/...` branches. You MUST also check for legacy or non-standard branch prefixes (e.g., `bolt-*`, `perf-*`, `sentinel-*`, `fix-*`) to catch overlapping older work. **When checking branches, you must evaluate prefix matches (e.g., `jules/agent/module/topic`), not exact matches.** Agents sometimes append random suffixes despite rules forbidding it, so an exact match check is insufficient.
- **Cross-Agent Module Check:** Agents must check if ANY other agent's branch targets their intended module to prevent cross-agent collisions where different tracks attempt to modify the same action handlers or specs. Because many domain agents (e.g., `anim-weaver`, `mesh-cartographer`) use 3-segment branch names (`jules/<agent>/<short-topic>`), a strict glob check like `jules/*/your-module/*` will silently fail to catch them. Agents must scan the full branch list for semantic substring matches of their target area.
- Use `gh pr list` and `gh pr diff <PR_NUMBER> --name-only` (when the `gh` CLI is available) to inspect actual file overlaps across all related open PRs. Do not rely solely on branch names or PR titles.
- **Queue Visibility Fallback:** If `gh` CLI or other PR visibility tools are unavailable (e.g., due to authentication failures), restrict work to isolated files that are extremely unlikely to collide, or stop without PR.
- Stop without PR if a branch with the same prefix exists, if an open PR has the same WorkFingerprint, or touches the same intended files. If an open branch or PR addresses the intended changes, or if the collision is ambiguous, **stop without PR**. No-op is a perfectly acceptable and expected outcome when the queue is healthy or work overlaps. If you decide to no-op, **do not create a branch or an empty PR to report it.**
- PR descriptions must include a 'Duplicate check' section detailing inspected PRs/branches and the reason the work is non-overlapping.
- **Staggered Schedules:** If multiple agents of the same type are scheduled, their runs should be staggered to allow PR visibility to propagate.

## 3. WorkFingerprint Requirement
Every agent PR description must include a `WorkFingerprint` containing at least:
- `agent`: (e.g., Marshal, Bolt, Sentinel)
- `category`: (e.g., performance, test, orchestration)
- `module`: (e.g., Monolith)
- `component/action/helper`: (e.g., MonolithCore, MonolithMesh)
- `intended files`: (e.g., comma-separated list)
- `risk type`: (e.g., collision, regression)
- `public API impact`: (e.g., yes, no)
- `docs/spec impact`: (e.g., yes, no)

## 4. Verification Claims
Do not claim Unreal Engine (UE) verification or release packaging was successful unless the tools were actually executed in the current VM. If the UE Editor is unavailable, explicitly note `[blocked: UE 5.7 editor unavailable in Jules VM]` in a dedicated 'Blocked verification' section in the PR description. If build, packaging, or other UE command-line tools are unavailable while the editor itself is not the blocker, use the matching concrete blocker text, for example `[blocked: UE 5.7 build tools unavailable in Jules VM]`.

When a verification succeeds, the `Verification:` section of the PR must clearly state the exact tools and checks that were run, such as `Ran git diff --check, verified file edits, and ran static CI checks which returned 0 blocking findings.` Do not use vague claims like "verified" or "tests passed" without specifying what was executed.

## 5. Journal Hygiene
When updating `.jules/` journal files, only document durable coordination rules and learnings. Do not use them as routine work logs or task journals.

## 6. Temporary Workflow Artifacts
Agents often create temporary files (such as `pr_body.txt`, helper Python scripts, or JSON dumps) during their workflows. To maintain repository hygiene, you must ensure all temporary workflow artifacts are completely deleted before staging and committing your final changes.

## 7. Single Responsibility
Agents must keep PRs tightly scoped. Do not mix unrelated security, test, spec, performance, release, refactor, and prompt-governance work in one PR. Refer to `.jules/agent-coordination.md` for explicit domain boundaries. If a task requires modifying files outside your primary domain, or if the only remaining useful change requires bundling unrelated concerns, stop without PR instead.

## 8. Unauthorized PR Operations
Agents must not close, merge, or delete pull requests or branches unless explicitly authorized to do so by the user. If you encounter a superseded, redundant, or conflicting PR/branch, you should stop without PR (no-op) instead of taking destructive actions to "clean up" the queue.

## 9. Public Action Contracts
Agents performing routine refactoring, performance optimization (Bolt), or hygiene (Curator) tasks must not modify public action contracts or JSON parameter schemas without explicit justification. If a behavior change is not the primary goal, do not alter expected inputs/outputs just to simplify code.

## 10. Minimum PR Value Threshold
Passing static CI is not enough to make a scheduled PR worth merging; a PR must also be non-overlapping, current after rebase, and clearly more valuable than a no-op. Stop without PR if the only available change is a micro-edit against shared coordination docs, action-count docs, or release docs where multiple agents often race.

## 10a. Forbid Style-Only Prompt Changes
Agents must avoid creating PRs that only contain style, formatting, or trivial wording changes to prompt and coordination files like `AGENTS.md` and files in `.jules/`. If the intended modification does not fundamentally change an actionable rule or behavior, **stop without PR**.

## 10b. PR Body Hygiene and Sensitive Information
Agents must keep PR descriptions focused and professional. Do not dump raw task execution logs, internal agent reasoning traces, or sensitive project findings into PR bodies or commit messages. Only include the required structured fields (like WorkFingerprint and Duplicate check) and a concise summary of the change.

## 11. External CI Limits
If a GitHub Actions CI check fails with a billing-related error (e.g., "recent account payments have failed" or "spending limit needs to be increased"), recognize that this is an external repository limit, not a code defect. Do not attempt to fix it via code changes; simply inform the user.

## 12. Index Freshness and CRG Cache
Project and source indexing must keep their CRG projection/cache data in sync. Successful ProjectIndex and EngineSource indexing completion rebuilds the matching CRG projection/cache automatically; if `project.health` or `source.health` reports stale CRG parity, run the matching `repair_crg_cache` action with execute enabled. Do not chain `repair_crg_cache` + `build_crg_graph` as a routine post-build pair: reindex completion already rebuilds the CRG cache, `repair_crg_cache` is health-gated repair only (it fast no-ops when parity is fresh, but the pair still cost ~9,000s/week in invocation logs), and `build_crg_graph` matters only when the `search_crg_graph` / `graph.db` export surface is actually used.

After a successful C++ build, Live Coding, or hot reload, EngineSource.db should refresh through incremental project source indexing. If the post-build reload hook did not fire or the editor was unavailable, run `source.trigger_project_reindex` once the existing EngineSource.db bootstrap is present. `trigger_reindex` / `trigger_project_reindex` are live-editor actions: bring the MCP endpoint up first (`Scripts/recover_mcp.ps1`); offline `monolith_query.exe` cannot run them and answers with `live_only` guidance instead of reindexing.

Offline `monolith_query.exe` calls in the default checkout resolve built-in DB paths from the executable location: `source` resolves `Saved/EngineSource.db`, `project` resolves `Saved/ProjectIndex.db`, `bridge` resolves both, and source CRG graph actions resolve `Saved/graph.db`. `--db`, `--source-db`, `--project-db`, and `--graph-db` remain override options for copied or non-standard databases.

For routine source lookup and code-review triage, prefer `source search_source`, `source risk_score`, `source review_context`, `source impact_radius`, `source review_hotspots`, and `source health`. `source impact_radius` defaults to `call|type|inheritance`; pass `edge_kinds=call|type|inheritance|override` only when override traversal is intentionally part of the blast-radius query. Use `source find_overrides` before changing virtual/override functions, preferably with a qualified symbol such as `UActorComponent::BeginPlay`, and use `source review_hotspots kind=override` to find high-fanout override contracts. `source build_crg_graph --execute` is an explicit CRG-compatible `Saved/graph.db` export/search maintenance action, not routine setup. `source repair_crg_cache --execute` remains the repair path for stale EngineSource `crg_*` projection/cache parity. `Saved/graph.db` is not the source of truth for `risk_score` or `review_context`; current `graph.db` flow/community/risk auxiliary tables are reserved/unimplemented placeholders and zero rows there are not a health failure.

Project search is content-inclusive by default. Live `project.search` and offline `Binaries\monolith_query.exe project search` search `fts_assets`, `fts_nodes`, `fts_variables`, `fts_parameters`, `fts_datatable_rows`, `fts_actors`, and `fts_asset_search_values` unless `include_content=false` / `--include-content=false` is specified. Search results include `match_source`, `match_table`, `match_field`, `match_object_path`, and `match_value`; use these provenance fields before treating a hit as an asset identity match.

`project repair_fts --target=all` covers all seven project FTS tables. Prefer a dry-run first on the live editor DB; use `--execute` only when repair is intended and the DB is writable, or verify write behavior on a copied DB.

`Scripts/check_index_freshness.ps1` runs the offline health -> repair -> re-verify chain for both DBs in one call: report mode prints each warning plus exact repair commands; `-Execute` runs only the warning-indicated `repair_crg_cache`/`repair_fts` repairs and refuses on-disk DB writes while the MCP endpoint is up unless `-AllowLiveEditor` is passed. Contract: `Docs/specs/SPEC_MonolithAgentOpsScripts.md`.

## 13. Offline Source/Bridge Usage

When the Monolith MCP server or Unreal Editor is not running, agents can use `Binaries/monolith_query.exe` directly. The source namespace covers C++ search, references, callers, callees, review context, risk score, and CRG-compatible graph search. The bridge namespace links assets and source symbols through `bridge search_asset_symbols`.

```powershell
Binaries\monolith_query.exe source search_source UObject --limit=5
Binaries\monolith_query.exe source risk_score UObject --limit=5
Binaries\monolith_query.exe source review_context UObject --detail-level=minimal
Binaries\monolith_query.exe source review_hotspots --kind=override --limit=10
Binaries\monolith_query.exe source find_overrides UActorComponent::BeginPlay --direction=in --max-depth=2
Binaries\monolith_query.exe source health --include-counts=true
Binaries\monolith_query.exe bridge search_asset_symbols --asset-path=/Game/Maps/Interactable/BP_Wave --limit=5
Binaries\monolith_query.exe bridge search_asset_symbols --symbol=UObject --limit=5
```

`source search_crg_graph` reads `Saved/graph.db` and uses `nodes_fts` before falling back to LIKE. Use `source build_crg_graph --execute` only when that graph-node search/export surface is explicitly needed; the builder uses a graph rebuild lock, skips when source-signature metadata is current (`skip_reason=parity_fresh`) or when the last build is within the `--cooldown-seconds` window (default 1800s, `skip_reason=cooldown`) unless `--force` is passed, and replaces `graph.db` only after a validated temp DB build. The cooldown caps the daily `repair_crg_cache`+`build_crg_graph` maintenance loop because `graph.db` is an export/search cache; do not chain the pair (§12). Live editor/MCP execute calls return `status=started`, `job_id`, and `poll_action=source.crg_graph_health`; offline `monolith_query.exe source build_crg_graph --execute` remains synchronous. `bridge search_asset_symbols` is read-only, opens `Saved/ProjectIndex.db` and `Saved/EngineSource.db`, and returns heuristic links with `confidence`, `reasons`, `asset`, `symbol`, `warnings`, `count`, `truncated`, and `lexical_only`.

## 13a. Offline Project Search Usage

Use offline project search when MCP/editor access is down but `Saved/ProjectIndex.db` is present:

```powershell
Binaries\monolith_query.exe project search Health --limit=10 --include-content=true
Binaries\monolith_query.exe project search Health --limit=10 --include-content=false
Binaries\monolith_query.exe project health --include-counts=true
Binaries\monolith_query.exe project repair_fts --target=all
```

Default `--include-content=true` is the high-recall discovery mode. Use `--include-content=false` for bridge/source context, asset identity matching, or noisy name/type lookup. Do not duplicate `EngineSource.db` source symbols or `graph.db` nodes into `ProjectIndex.db`; use `source`/`bridge` actions for source relationships.

## 14. MCP Connection Recovery in Go Checkout

For Go checkout work that needs editor-backed Monolith actions, use the configured MCP client connection to `http://localhost:9316/mcp` and confirm it with `monolith_status()` or the active MCP client's health check before calling editor actions. If the MCP endpoint is unreachable or the transport fails, start the project headless editor wrapper and reconnect the existing Monolith proxy/client instead of bypassing Monolith:

```powershell
D:\P4\game\BatchFiles\RunHeadlessEditor.bat
```

Wait for `localhost:9316` to listen, reconnect to `http://localhost:9316/mcp`, then re-run `monolith_status()` before using `monolith_find`, `monolith_discover`, or namespace actions. If the endpoint still cannot connect after the editor starts, inspect `D:\P4\game\Saved\HeadlessMcp\Logs\HeadlessEditor-*.log` and the Monolith proxy/editor invocation logs, report the concrete blocker, and limit fallback work to read-only `Binaries\monolith_query.exe` source/project/bridge queries while editor-only actions remain blocked.

`Scripts/recover_mcp.ps1` runs this probe -> launch -> wait -> verify sequence deterministically (`-ProbeOnly` for diagnosis; `-game`/`-server` instances are not counted as a booting editor; documented `RESULT=` tokens and exit codes). Contract: `Docs/specs/SPEC_MonolithAgentOpsScripts.md`.

## 15. Tool Invocation Daily Logs

The daily invocation log contract is documented in `Docs/specs/SPEC_MonolithToolInvocationLogs.md`. When a checkout includes the implementation, the files are local diagnostics only and must not be treated as canonical tool output.

- Proxy calls append JSONL records to `Logs/yyyyMMdd/proxy.jsonl`.
- Offline query calls append JSONL records to `Logs/yyyyMMdd/query.jsonl`.
- Editor action dispatch appends JSONL records to `Logs/yyyyMMdd/action.jsonl` when `UMonolithSettings::bEnableDailyLog=true`; this checkout opts in through `Config\DefaultMonolith.ini`.
- Current records use format v3 (`record_id`, `trace_id`, per-record `span_id`, `routing_context`, `workflow`, `phase_timing`, compact `return_summary`, redaction metadata, no empty optional fields); append-only date folders can still contain older v1/v2 rows. Proxy calls forward trace metadata to editor actions, and action-spawned `monolith_query.exe` calls inherit it.
- Proxy/query logging is enabled by default; unset or `MONOLITH_TOOL_LOG_ENABLED=1` enables it, and `MONOLITH_TOOL_LOG_ENABLED=0` disables it before launching the process.
- For proxy/query smoke tests or temporary diagnostics, set `MONOLITH_TOOL_LOG_DIR` before launching the process to isolate logs outside `Logs/`.
- Use the logs to aggregate repeated missing-action, schema-confusing, retry, large-result, editor-unavailable, and escape-hatch patterns before changing namespace placement or action contracts. The offline reader is `python Analyzer/analyze_invocation_logs.py --log-root Logs --since <yyyyMMdd>` (contract: `Docs/specs/SPEC_MonolithInvocationLogAnalyzer.md`); it writes markdown/json/csv reports under `Saved/Monolith/LogAnalysis/` and never mutates `Logs/`.
- Do not commit `Logs/*`; logs can contain project/source context even after redaction and truncation. Retention is manual: `Scripts/prune_invocation_logs.ps1` (dry-run by default; `-Execute` deletes date folders outside `-KeepDays`/`-MaxTotalMB`).

## 16. Execution Plan Requirements
When creating execution plans, agents must adhere to the following rules to ensure tasks translate directly to safe, verifiable actions:
- **Groundedness and Specificity:** Never guess or assume function names, variable names, or the presence/absence of specific code blocks based solely on `grep` snippets. Always read the exact target file contents to confirm the precise code structure before drafting steps. Steps must be specific, actionable directives (e.g., 'Edit <file> to add <code snippet>'). Avoid vague phrasing like 'Analyze the issue' or 'Implement the change'.
- **Completeness:** Execution plans must explicitly include a step to run the project's static checks (`python Scripts/ci_static_checks.py --config .github/monolith-static-ci.json --github check`) and a verification stage (e.g., `git diff --check` and `git status --short`) before the final pre-commit step.
- **Pre-commit Description:** The final pre-commit step description must be exactly: 'Complete pre-commit steps to ensure proper testing, verification, review, and reflection are done.'

## 17. Monolith Onboarding and Skills
Use `Templates\Onboarding\Onboarding.md` and `Scripts/onboard_monolith.ps1` for global Monolith MCP client setup, project instruction setup, and global skill-link installation. In-repo skills live at `Skills/<skill>/SKILL.md`; do not refresh global skills by copying files. Use `Scripts/install_monolith_skills.ps1` only for direct skill-link repair, and `Scripts/validate_monolith_skills.ps1` to validate repository and installed skill roots.

When changing MCP setup, project instructions, or skill distribution, update `Templates\Onboarding\Onboarding.md` and `Docs/specs/SPEC_MonolithSkillsSymlinkDistribution.md` in the same change.

## 18. Action Count Synchronization
When updating action counts in documentation (e.g., `README.md`, `Docs/API_REFERENCE.md`, `Docs/SPEC_CORE.md`, module specs, and `Monolith.uplugin`), agents must update all counts together in a single PR to prevent fragmentation and drift. Agents whose primary task is not syncing action counts (such as docs-only agents) must avoid count churn unless it is their exact mission. Stop without PR if an active PR from an ActionCountKeeper, SkillDocSmith, or Sentinel spec already touches the same count-bearing files.

## 19. Release Packaging and Exclusions
When maintaining release packaging scripts (e.g., `Scripts/make_release.ps1`), explicitly exclude internal planning, spec, and documentation folders (such as `.jules/`, `CRG/`, `PRD/`, and `Docs/plans/`) from the release ZIP. Even if these folders are listed in `.gitignore`, `git ls-files` will package them if their contents are being tracked in git. Ensure local developer, workspace, and AI tooling directories are explicitly excluded from release archives to prevent data leakage and excessive release sizes.
