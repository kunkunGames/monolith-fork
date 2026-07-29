# Source Control Action Port Verification

**Date:** 2026-07-29
**Scope:** `source_control` namespace and shared source-control preparation utility
**Base:** `ee1dae25f9a90a45ae768abbfcb0d9356810b0c4`
**Verified patch ID:** `22bfa1121f61fb9bf00bd3f60049788052bbbe5c`
**Engines:** Unreal Engine 5.7 and 5.8

---

## 1. Goal

Verify that the independent source-control port adds exactly 11 practical actions, keeps provider mutations and Perforce inspection bounded and explicit, compiles from the same implementation patch on UE 5.7 and UE 5.8, and passes the focused automation suite on both engines.

---

## 2. Surface and Contracts

| Surface | Result | Evidence |
|---------|--------|----------|
| Provider query actions | 2 | `get_capabilities`, `get_status` |
| Provider mutation actions | 7 | `checkout`, `add`, `checkout_or_add`, `delete`, `mark_for_delete`, `revert`, `revert_unchanged` |
| Perforce read actions | 2 | `list_opened`, `map_depot_paths` |
| Catalog delta | PASS | Base 1561 → head 1572; all 11 additions are in the new `source_control` namespace and no action was removed |
| Strict JSON parameters | PASS | Optional booleans accept JSON booleans only; `limit` must be a finite integer in `[1, 5000]`; changelists accept only decimal values or `default` |
| Destructive-operation gate | PASS | Delete and revert variants require `confirm=true` unless `dry_run=true` |
| Bounded Perforce work | PASS | At most 5000 inputs/unique paths, 128 paths per command, 24,000 command characters, and 40 `p4 where` processes |
| Bounded opened window | PASS | `p4 opened` requests `limit + 1`, returns at most `limit`, and exposes sentinel/lower-bound semantics instead of issuing an unbounded count query |
| Partial mapping behavior | PASS | Per-path failures remain row-local; successful mappings preserve input identity and order |
| Excluded feature classes | PASS | The changed source/spec/skill contains no security, benchmark, reinforcement-learning, invocation-log, or action-search-metadata implementation |

The catalog was regenerated from the feature worktree with:

```powershell
python D:\P4\MonolithPortAudit\Tools\MonolithQuery\generate_monolith_catalog_snapshot.py `
    --root D:\P4\MonolithForkSourceControl `
    --out D:\P4\MonolithForkSourceControlActionCatalog.json
```

The generated catalog contained 1572 actions across 25 namespaces. Comparison against the 1561-action, 24-namespace base catalog found exactly 11 additions and zero removals.

---

## 3. Verification Environment

| Engine | Resolved root | Host project | Plugin source |
|--------|---------------|--------------|---------------|
| UE 5.7 | `D:\Engine\UE_5.7` | `D:\P4\MonolithSourceControlUE57Host\MonolithSourceControlUE57Host.uproject` | `D:\P4\MonolithForkSourceControl` |
| UE 5.8 | `D:\Engine\UE_5.8` | `D:\P4\MonolithSourceControlUE58Host\MonolithSourceControlUE58Host.uproject` | `D:\P4\MonolithForkSourceControlUE58` |

Each engine root was resolved from the isolated host project's `.uproject` `EngineAssociation`. The UE 5.8 host used an independent detached worktree. Before either build, `git patch-id --stable` reported `22bfa1121f61fb9bf00bd3f60049788052bbbe5c` for both worktrees.

---

## 4. Commands

The isolated editor targets were built through each resolved engine's UnrealBuildTool:

```powershell
& D:\Engine\UE_5.7\Engine\Binaries\DotNET\UnrealBuildTool\UnrealBuildTool.exe `
    UnrealEditor Win64 Development `
    "-Project=D:\P4\MonolithSourceControlUE57Host\MonolithSourceControlUE57Host.uproject" `
    -WaitMutex -NoHotReloadFromIDE

& D:\Engine\UE_5.8\Engine\Binaries\DotNET\UnrealBuildTool\UnrealBuildTool.exe `
    UnrealEditor Win64 Development `
    "-Project=D:\P4\MonolithSourceControlUE58Host\MonolithSourceControlUE58Host.uproject" `
    -WaitMutex -NoHotReloadFromIDE
```

Focused automation used the same test prefix on both engines:

```text
Automation RunTests Monolith.SourceControl
```

The eight tests were:

```text
Monolith.SourceControl.P4WhereBatch.OpenedBounds
Monolith.SourceControl.P4WhereBatch.PartialFailure
Monolith.SourceControl.P4WhereBatch.RowLocalStderr
Monolith.SourceControl.P4WhereBatch.Scale
Monolith.SourceControl.P4WhereBatch.ViewOrder
Monolith.SourceControl.P4WhereBatch.WindowsCommandLine
Monolith.SourceControl.ParamValidation.DocumentedInput
Monolith.SourceControl.ParamValidation.TypedParams
```

The fork base does not contain `Scripts\ci_static_checks.py` or `.github\monolith-static-ci.json`, so the prescribed checkout-local command cannot run there. The upstream checker engine was therefore run against this worktree with a temporary local configuration that enabled module-map, `Build.cs`, action-registration, automation-name, generated-header, repository, workflow, and text checks while disabling out-of-scope benchmark, analyzer/log, proxy, skill-drift, and offline checks:

```powershell
python D:\P4\MonolithPortAudit\Scripts\ci_static_checks.py `
    --config .github\monolith-static-ci.json `
    --github check
```

The temporary configuration was removed immediately after the check and is not part of the change.

---

## 5. Results

| Gate | UE 5.7 | UE 5.8 |
|------|--------|--------|
| Full editor target build | PASS | PASS, 442 build actions |
| Final UBT result | `Result: Succeeded`, exit 0 | `Result: Succeeded`, 192.01 seconds |
| Focused automation | PASS, 8/8 | PASS, 8/8 |
| Automation warnings / errors | 0 / 0 | 0 / 0 |
| Automation report | `D:\P4\MonolithSourceControlUE57Host\Saved\Automation\SourceControl-20260729-222749\index.json` | `D:\P4\MonolithSourceControlUE58Host\Saved\Automation\SourceControl-20260729-223600\index.json` |
| `UnrealEditor-MonolithCore.dll` bytes | 1,106,944 | 1,052,160 |
| `MonolithCore` DLL SHA256 | `40DBC92D19E0C473CA5F1A76629145B03F923B421CAE245735494590947F935D` | `33D79847517EF667806A6327897CF5DD70C2955A072235E4BAB5B03D7B256354` |
| `UnrealEditor-MonolithSourceControl.dll` bytes | 312,320 | 300,544 |
| `MonolithSourceControl` DLL SHA256 | `7C66DC87EF1DF7039DC475FA61319B942AADA9C239D8F4CD3125C3647BEEC57A` | `B0BD3EC14C0B81290D3906EE012FB77AB654E5165DD499B5EF177C1E08FB19A8` |

Additional gates:

| Gate | Result |
|------|--------|
| Hosted static-check equivalent | PASS: 0 blocking findings; 777 repository-wide CRLF/missing-external-agent advisory findings |
| `git diff --check` | PASS |
| Feature-category exclusion scan | PASS: no forbidden feature implementation found |
| Worktree parity before build | PASS: identical stable patch ID in UE 5.7 and UE 5.8 worktrees |

The UE 5.8 full target build emitted pre-existing Unreal 5.8 deprecation warnings in unrelated modules. No changed `MonolithSourceControl` or `MonolithCore` source produced a build error.

---

## 6. Visual and Delivery Scope

| Gate | Result | Reason |
|------|--------|--------|
| 1920x1080 screenshot | N/A | The change adds headless editor source-control handlers, schemas, batching logic, and automation; it changes no runtime or editor visual presentation. |
| Discord screenshot upload | N/A | No screenshot artifact was required, so `Build\BatchFiles\Script\UploadScreenshotTestsToDiscord.bat` was not run. |

---

## 7. Conclusion

PASS. The verified implementation adds exactly 11 `source_control` actions, uses strict parameter contracts, bounds Perforce process and result work, preserves row-local mapping diagnostics, builds on UE 5.7 and UE 5.8, and passes 8/8 focused automation tests with no test warnings or errors on either engine.
