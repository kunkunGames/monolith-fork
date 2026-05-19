# Dataflow Dedicated Graph Inspection Verification

| | |
|---|---|
| Date | 2026-05-19 |
| Branch | `codex/dataflow-asset-inspection` |
| Spec | `Docs/specs/SPEC_MonolithDataflow.md` |
| Scope | Move the next high-ROI Dataflow inspection slice into `MonolithDataflow` and keep release builds dependency-safe. |

---

## 1. Expected Contracts

| Contract | Result |
|---|---|
| Dataflow graph inspection is registered from the `dataflow` namespace, not `mesh`. | PASS |
| `get_status` and `list_assets` remain always-on and AssetRegistry/module-status-only. | PASS |
| Graph inspection actions compile only when `WITH_MONOLITH_DATAFLOW=1`. | PASS |
| `MONOLITH_RELEASE_BUILD=1` forces optional Dataflow runtime dependencies off. | PASS |
| No Dataflow action mutates, evaluates, regenerates, saves, or dirties packages. | PASS |
| `get_dataflow_graph` returns only connections whose endpoint nodes are in the returned node slice and caps rows with `connection_limit`. | PASS |

## 2. Validation Commands

Command:

```powershell
git diff --check origin/master...HEAD
```

Result: PASS.

Command:

```powershell
uv run python Scripts\ci_static_checks.py --config .github\monolith-static-ci.json --github check
```

Result: PASS. Blocking findings: 0. Advisory finding: `.claude/agents` external-prerequisite warning.

Command:

```powershell
$engineRoot = powershell -NoProfile -ExecutionPolicy Bypass -File "D:\P4\game\BatchFiles\Script\ResolveUnrealEngine.ps1" -Project "D:\P4\game\GO.uproject" -Output Root
& "$engineRoot\Engine\Binaries\DotNET\UnrealBuildTool\UnrealBuildTool.exe" UnrealEditor Win64 Development -Plugin="D:\P4\_codex_merge\monolith-pr-work\Monolith.uplugin" -Module=MonolithDataflow -WaitMutex -NoHotReloadFromIDE -NoUBTMakefiles
```

Result: PASS.

Command:

```powershell
$env:MONOLITH_RELEASE_BUILD='1'
$engineRoot = powershell -NoProfile -ExecutionPolicy Bypass -File "D:\P4\game\BatchFiles\Script\ResolveUnrealEngine.ps1" -Project "D:\P4\game\GO.uproject" -Output Root
& "$engineRoot\Engine\Binaries\DotNET\UnrealBuildTool\UnrealBuildTool.exe" UnrealEditor Win64 Development -Plugin="D:\P4\_codex_merge\monolith-pr-work\Monolith.uplugin" -Module=MonolithDataflow -WaitMutex -NoHotReloadFromIDE -NoUBTMakefiles
Remove-Item Env:MONOLITH_RELEASE_BUILD -ErrorAction SilentlyContinue
```

Result: PASS.

## 3. Deferred Runtime Fixture

No editor automation fixture with a real `UDataflow` asset was run in this slice. `Docs/TODO.md` tracks a follow-up fixture that should cover nodes, variables, comments, and connections once a stable sample asset is available.
