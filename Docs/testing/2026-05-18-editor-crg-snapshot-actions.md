# Monolith Editor CRG Snapshot Actions Verification

Metadata

| Field | Value |
|---|---|
| Date | 2026-05-18 |
| Scope | `project.snapshot`, `project.diff_snapshots`, `source.snapshot`, `source.diff_snapshots` |
| Branch | `feat/editor-crg-snapshot-actions` |
| Engine | `D:\Engine\UE_5.7` |

---

## 1. Coverage

| Area | Result |
|---|---|
| Spec-first contract | `Docs/specs/SPEC_MonolithIndex.md`, `Docs/specs/SPEC_MonolithSource.md`, and `Docs/API_REFERENCE.md` define the live action inputs and output contract |
| Project action | `project.snapshot` stores a derived CRG manifest only with `execute=true`; `project.diff_snapshots` compares stored/current manifests read-only |
| Source action | `source.snapshot` stores a derived CRG manifest only with `execute=true`; `source.diff_snapshots` compares stored/current manifests read-only |
| Safety | Snapshot writes only touch `crg_snapshots`; diffs never rebuild caches or shell out to P4/git |

---

## 2. Verification Gates

| Gate | Command | Status | Notes |
|---|---|---|---|
| Whitespace | `git diff --check` | PASS | No whitespace errors |
| UBT build | `D:\Engine\UE_5.7\Engine\Binaries\DotNET\UnrealBuildTool\UnrealBuildTool.exe GoGameEditor Win64 Development -Project="D:\P4\game\GO.uproject" -WaitMutex -NoHotReloadFromIDE` | PASS | `Result: Succeeded`; P4 password warnings were non-blocking checkout noise |
| Automation | `D:\Engine\UE_5.7\Engine\Binaries\Win64\UnrealEditor-Cmd.exe D:\P4\game\GO.uproject -NullRHI -NoSound -Unattended -NoSplash -NoP4 -ExecCmds="Automation RunTests Monolith.IndexGuard; Quit" -TestExit="Automation Test Queue Empty" -ReportExportPath="D:\P4\game\Saved\AutomationReports\pr497-snapshot-rebased"` | PASS | `index.json`: `succeeded=37`, `succeededWithWarnings=0`, `failed=0`; includes `Project.SnapshotDiff` and `Source.SnapshotDiff` |
| Live MCP smoke | Full editor `-NullRHI` + HTTP `tools/call` to `source_query.snapshot`, `source_query.diff_snapshots`, `project_query.snapshot`, and `project_query.diff_snapshots` | PASS | `source.snapshot execute=true` created `codex-rx4-source-20260518-020111` with `node_count=1065390`, `edge_count=2238254`; `source.diff_snapshots` against `current` returned `status=ok`, zero deltas. `project.snapshot execute=true` created `codex-rx4-project-20260518-020241` with `node_count=1`, `edge_count=0`; `project.diff_snapshots` against `current` returned `status=ok`, zero deltas |
| Static CI | Clean detached worktree `uv run python Scripts\ci_static_checks.py --config .github\monolith-static-ci.json --github check` | PASS | Blocking findings `0`; advisory only for external `.claude/agents` directory |

---

## 3. Follow-Up

| Item | Status |
|---|---|
| Live MCP smoke test against editor DBs | Complete |
| Static CI clean-worktree pass | Complete |
| Remote GitHub Actions | Pending; known account billing/spending-limit blocker may prevent jobs from starting |
