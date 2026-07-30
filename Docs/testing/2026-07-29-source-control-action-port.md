# Source Control Action Port Verification

**Date:** 2026-07-30; final review verification 2026-07-31
**Scope:** `source_control` namespace and shared source-control preparation utility
**Base:** `ee1dae25f9a90a45ae768abbfcb0d9356810b0c4`
**Verified code commit:** `7c6144602c9ceaed8a6e494f82ddd8c6b198d22d`
**Verified stable patch ID:** `69387eaf536b422bb5c01cc37da9420cea0516d0`
**Engines:** Unreal Engine 5.7 and 5.8

---

## 1. Goal

Verify that the independent source-control port adds exactly 11 practical actions, keeps provider mutations and Perforce inspection bounded and explicit, resolves every actionable review finding, compiles from the same implementation commit on UE 5.7 and UE 5.8, and passes the focused automation suite plus live MCP readback.

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
| Mounted-package discrimination | PASS | Only registered Unreal mount points are interpreted as package paths; `/home/...` remains a POSIX absolute filesystem path through normalization and response mapping |
| Preparation safety | PASS | Benign skips remain editable; other-user checkout, conflict, stale revision, and unknown/unsupported provider states set `blocking=true`, `safe_to_proceed=false`, and abort all checkout/add mutations. A failed checkout suppresses the add phase. |
| Bounded Perforce work | PASS | At most 5000 inputs/unique paths, 128 paths per command, 24,000 command characters, 40 `p4 where` processes, and 30 seconds per child process |
| Perforce timeout behavior | PASS | Deadline expiry terminates the process tree, drains captured output, diagnoses the current and unstarted batches, and launches no later batch |
| Cross-platform arguments | PASS | Windows CRT quoting and Unreal Unix double-quoted argv behavior have focused round-trip coverage; unsupported Unix embedded double quotes fail validation |
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
| UE 5.8 | `D:\Engine\UE_5.8` | `D:\P4\MonolithSourceControlUE58ReviewHost\MonolithSourceControlUE58ReviewHost.uproject` | `D:\P4\MonolithForkSourceControlUE58Review` |

Each engine root was resolved from the isolated host project's `.uproject` `EngineAssociation`. The UE 5.8 host used an independent detached worktree at the exact verified code commit. `git diff <base> <verified-code-commit> | git patch-id --stable` reported `69387eaf536b422bb5c01cc37da9420cea0516d0`.

---

## 4. Commands

The isolated editor targets were built through each `.uproject`-resolved engine root. The same resolver pattern was used for both hosts:

```powershell
$project = "<isolated-host>.uproject"
$engine = & D:\P4\speed\Build\BatchFiles\Script\ResolveUnrealEngine.ps1 -Project $project
if (-not $?) { throw "ResolveUnrealEngine failed" }

& (Join-Path $engine "Engine\Build\BatchFiles\Build.bat") `
    UnrealEditor Win64 Development "-Project=$project" `
    -WaitMutex -NoHotReloadFromIDE "-Log=<unique-UBT-log>"
```

Focused automation used the same test prefix on both engines:

```text
Automation RunTests Monolith.SourceControl
```

The ten tests were:

```text
Monolith.SourceControl.P4WhereBatch.OpenedBounds
Monolith.SourceControl.P4WhereBatch.PartialFailure
Monolith.SourceControl.P4WhereBatch.ProcessDeadline
Monolith.SourceControl.P4WhereBatch.RowLocalStderr
Monolith.SourceControl.P4WhereBatch.Scale
Monolith.SourceControl.P4WhereBatch.ViewOrder
Monolith.SourceControl.P4WhereBatch.WindowsCommandLine
Monolith.SourceControl.ParamValidation.DocumentedInput
Monolith.SourceControl.ParamValidation.TypedParams
Monolith.SourceControl.PrepareDecision.BlockingStates
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
| Full editor target build | PASS, 11 final-head actions | PASS, 11 final-head actions |
| Final UBT result | `Result: Succeeded`, 13.62 seconds | `Result: Succeeded`, 17.16 seconds |
| UBT log | `D:\P4\MonolithSourceControlUE57Host\Saved\Logs\SourceControl-FinalReview-UBT-UE57-20260731-021437.log` | `D:\P4\MonolithSourceControlUE58ReviewHost\Saved\Logs\SourceControl-FinalReview-UBT-UE58-20260731-021437.log` |
| Focused automation | PASS, 10/10 | PASS, 10/10 |
| Automation warnings / errors | 0 / 0 | 0 / 0 |
| Automation report | `D:\P4\MonolithSourceControlUE57Host\Saved\Automation\SourceControl-FinalReview-UE57-20260731-021543\index.json` | `D:\P4\MonolithSourceControlUE58ReviewHost\Saved\Automation\SourceControl-FinalReview-UE58-20260731-021543\index.json` |
| `UnrealEditor-MonolithCore.dll` bytes | 1,118,208 | 1,063,424 |
| `MonolithCore` DLL SHA256 | `ED661310577D9EEFE2315C72FE5F1B4BA79DFFD8094791D4A3ABD2BED3013887` | `A855A24A26E216E30CDE88E9FA9A2A762A2459C4A807A26E468000DBFBDC195C` |
| `UnrealEditor-MonolithSourceControl.dll` bytes | 338,944 | 325,632 |
| `MonolithSourceControl` DLL SHA256 | `7D54DFE275458F9C4B06CBABA08C883D610EAD26F4786D467928F0B5FE8EE0A4` | `032EC7626F84D83377BD4D78E9475FC78E1410F1F7797AA9F17796154878F3F3` |

Additional gates:

| Gate | Result |
|------|--------|
| Hosted static-check equivalent | PASS on the original action port: 0 blocking findings; review hardening did not change action registration or module descriptors |
| `git diff --check` | PASS on the final review hardening diff |
| Feature-category exclusion scan | PASS: no forbidden feature implementation found |
| Worktree identity before build | PASS: main UE 5.7 worktree source at `7c614460`; UE 5.8 detached worktree at exact code commit `7c6144602c9ceaed8a6e494f82ddd8c6b198d22d` |

The final review pass closes four remaining fail-open paths found by current-line review: stale/conflicted facts now precede the already-open shortcut, an invalid provider state cannot authorize add, checkout failure suppresses the add phase, and `list_opened(resolve_packages=true)` propagates `p4 where` launch/timeout failure. The focused suite proves all ten tests succeed with zero test warnings or errors on both engines.

---

## 6. Live MCP Readback

The earlier `3b0ce866` UE 5.8 binary was started on isolated port `9436` with source control and indexing disabled. Schema discovery ran before action calls. This evidence is retained for live transport/schema/readback coverage; exact-head proof for `7c614460` is the cross-version build and focused automation evidence in Section 5.

| Gate | Result | Evidence |
|------|--------|----------|
| MCP initialize | PASS | Server `monolith` version `0.21.3` |
| Exact schemas | PASS, 3/3 | `get_capabilities`, `map_depot_paths`, `checkout_or_add` |
| Mounted package | PASS | `/Game/SourceControlTest/SC_TestAsset.SC_TestAsset` returned `is_package=true` and `/Game/SourceControlTest/SC_TestAsset` |
| POSIX absolute path | PASS | `/home/monolith/Project/Content/Foo.uasset` retained the same local identity, `is_package=false`, and an empty `package_path` |
| Real `p4 where` child | PASS | One bounded command launched; the intentionally non-client depot path returned one row-local diagnostic |
| Provider-disabled preparation | PASS | `checkout_or_add(dry_run=true)` returned `ok=false`, `available=false`, `skipped=true`, and performed no mutation |
| Shutdown | PASS | `QUIT_EDITOR` closed PID `33460`, port `9436` closed, and no residual validation editor remained |
| Log scan | PASS | 0 fatal/assert/ensure/automation-error matches |

Live log:

```text
D:\P4\MonolithSourceControlUE58ReviewHost\Saved\Logs\SourceControl-Review3-LiveMCP-UE58-20260730-045057.log
```

The log records `bEnableIndex=false`, MCP listen on `9436`, and graceful listener shutdown.

---

## 7. Review Regression Evidence

The first UE 5.7 review run deliberately included the new POSIX contract and failed `DocumentedInput` because the action's later filename-to-package conversion still accepted the foreign root. The accepted fix revalidates the converted package against registered mount points. The final UE 5.7 and UE 5.8 reports both pass 10/10, so the review issue has a red-to-green regression proof rather than source inspection alone.

---

## 8. Visual and Delivery Scope

| Gate | Result | Reason |
|------|--------|--------|
| 1920x1080 screenshot | N/A | The change adds headless editor source-control handlers, schemas, batching logic, and automation; it changes no runtime or editor visual presentation. |
| Discord screenshot upload | N/A | No screenshot artifact was required, so `Build\BatchFiles\Script\UploadScreenshotTestsToDiscord.bat` was not run. |

---

## 9. Conclusion

PASS. The verified implementation adds exactly 11 `source_control` actions, uses strict parameter contracts, recognizes only mounted Unreal package paths, fails closed on blocking preparation states, bounds both Perforce batches and child-process lifetime, preserves row-local mapping diagnostics, builds on UE 5.7 and UE 5.8, and passes 10/10 focused automation tests with no test warnings or errors on either engine. Earlier-head live schema/action/readback evidence is reported separately and is not presented as exact-head runtime proof.
