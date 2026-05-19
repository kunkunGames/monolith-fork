# Slate Inspector Read-only Verification

| | |
|---|---|
| Date | 2026-05-19 |
| Branch | `codex/slate-readonly` |
| Spec | `Docs/specs/SPEC_MonolithSlate.md` |
| Scope | Native `slate` namespace for UE 5.7 live editor Slate UI inspection |

---

## 1. Spec And Code Sync

| Contract | Evidence |
|---|---|
| Separate module and namespace | `MonolithSlate::FSlateInspectorActions` registers actions under `slate`, not `ui`, from the dedicated `MonolithSlate` module. |
| Disabled default | `UMonolithSettings::bEnableSlateInspectorActions=false`; only `get_inspector_status` is always registered by default. |
| Read-only slice | Implemented actions list/read/snapshot/capture/wait only; no click, key, hover, text input, or widget mutation action is registered. |
| Slate capture source | `capture_widget` uses `FSlateApplication::TakeScreenshot` and reports `viewport_fallback_used=false`. |
| Ref safety | Widget refs are generation-scoped, TTL-bound, opaque strings and do not expose raw memory addresses. |

## 2. UE 5.7 Plugin Build

Command:

```powershell
$engineRoot = powershell -NoProfile -ExecutionPolicy Bypass -File "D:\P4\game\BatchFiles\Script\ResolveUnrealEngine.ps1" -Project "D:\P4\game\GO.uproject" -Output Root
& "$engineRoot\Engine\Binaries\DotNET\UnrealBuildTool\UnrealBuildTool.exe" UnrealEditor Win64 Development -Plugin="<worktree>\Monolith.uplugin" -WaitMutex -NoHotReloadFromIDE -NoUBTMakefiles
```

Result: PASS.

Notes:

- Final UBT result was `Succeeded`.
- `-NoUBTMakefiles` is required in this project layout because multiple same-name Monolith worktrees exist under local worktrees; cached makefiles can resolve a sibling Monolith checkout until the cache is bypassed.
- Non-blocking warning remained: `MassEntity` is deprecated.

## 3. Static Checks

| Check | Command | Result |
|---|---|---|
| Patch whitespace | `git diff --check` | PASS |
| Hosted static CI parity | `uv run python Scripts\ci_static_checks.py --config .github\monolith-static-ci.json --github check` | PASS: blocking findings 0; advisory `.claude/agents` external prerequisite only |

## 4. Deferred Runtime Test

No live editor MCP smoke was run in this pass. The required follow-up is:

1. Enable `bEnableSlateInspectorActions=true`.
2. Restart the editor.
3. Call `slate.get_inspector_status`, `slate.list_windows`, `slate.snapshot_widgets`, `slate.describe_widget`, `slate.capture_widget`, and `slate.wait_for_widget` against a visible editor window.
4. Confirm `capture_widget.viewport_fallback_used=false`, stale refs reject cleanly, and default limits clamp large snapshots.
