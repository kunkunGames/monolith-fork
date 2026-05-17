# Monolith Editor Pre-Merge Check Actions Verification

Metadata

| Field | Value |
|---|---|
| Date | 2026-05-18 |
| Scope | `project.pre_merge_check`, `source.pre_merge_check` |
| Branch | `feat/editor-pre-merge-check-actions` |
| Engine | `D:\Engine\UE_5.7` |

---

## 1. Coverage

| Area | Result |
|---|---|
| Spec-first contract | `Docs/specs/SPEC_MonolithIndex.md`, `Docs/specs/SPEC_MonolithSource.md`, and `Docs/API_REFERENCE.md` define the live action inputs and output contract |
| Project action | `project.pre_merge_check` composes `health`, `detect_changes`, and optional `find_unused` into `decision`, `checks[]`, `findings[]`, counts, risk, truncation, and next actions |
| Source action | `source.pre_merge_check` composes `health`, `detect_changes`, and optional `find_unused`, including source heuristic test-gap findings |
| Safety | Both actions are read-only, VCS-agnostic, and never shell out to P4/git |

---

## 2. Verification Gates

| Gate | Command | Status | Notes |
|---|---|---|---|
| Whitespace | `git diff --check` | PASS | No whitespace errors |
| UBT build | `D:\Engine\UE_5.7\Engine\Binaries\DotNET\UnrealBuildTool\UnrealBuildTool.exe GoGameEditor Win64 Development -Project="D:\P4\game\GO.uproject" -WaitMutex -NoHotReloadFromIDE` | PASS | `Result: Succeeded`; P4 password warnings were non-blocking checkout noise |
| Automation | `D:\Engine\UE_5.7\Engine\Binaries\Win64\UnrealEditor-Cmd.exe D:\P4\game\GO.uproject -NullRHI -NoSound -Unattended -NoSplash -NoP4 -ExecCmds="Automation RunTests Monolith.IndexGuard; Quit" -TestExit="Automation Test Queue Empty" -ReportExportPath="D:\P4\game\Saved\AutomationReports\pr495-pre-merge-check-rebased"` | PASS | `index.json`: `succeeded=35`, `succeededWithWarnings=0`, `failed=0`; includes #494 wildcard regressions plus source/project pre_merge_check tests |
| Live MCP smoke | Full editor `-NullRHI` + HTTP `tools/call` to `source_query.pre_merge_check` and `project_query.pre_merge_check` | PASS | Server ready; `source.pre_merge_check("Actor.cpp")` returned `status=warning`, `decision=warn`, `changed_entity_count=5`, `test_gap_count=5`; `project.pre_merge_check("Content/Maps/Interactable/BP_Wave.uasset")` returned `status=warning`, `decision=warn`, `changed_entity_count=1`, `impacted_count=0` |
| Static CI | `uv run python Scripts\ci_static_checks.py --config .github\monolith-static-ci.json --github check` in clean detached worktree | PASS | Blocking findings `0`; advisory only for external `.claude/agents` directory |

---

## 3. Follow-Up

| Item | Status |
|---|---|
| Live MCP smoke test against editor DBs | Complete |
| Static CI clean-worktree pass | Complete |
| Remote GitHub Actions | Pending; known account billing/spending-limit blocker may prevent jobs from starting |
