# Enhanced Input Action Port Verification

**Date:** 2026-07-28
**Review hardening verified:** 2026-07-30
**Scope:** `input` namespace in `MonolithGAS`
**Base:** `ee1dae25f9a90a45ae768abbfcb0d9356810b0c4`
**Verified code source:** `eecd99dca45a99e70ed1f589f39b5cdf846f2bd6`
**Engines:** Unreal Engine 5.7 and 5.8

---

## 1. Goal

Verify that the independent Enhanced Input port adds exactly 10 guarded asset actions, resolves every actionable PR review finding from both review rounds, compiles from the same committed source on UE 5.7 and UE 5.8, passes focused automation including GC-separated creation Undo/Redo and deterministic save-failure coverage, works through the live MCP surface, and leaves no test assets on disk.

---

## 2. Surface and Contracts

| Surface | Result | Evidence |
|---------|--------|----------|
| Read actions | 5 | `list_input_actions`, `get_input_action`, `list_input_mapping_contexts`, `get_input_mapping_context`, `validate_input_mappings` |
| Write actions | 5 | `create_input_action`, `set_input_action_properties`, `create_input_mapping_context`, `add_input_mapping`, `remove_input_mapping` |
| Catalog delta | PASS | Base 1561 → head 1571; all 10 additions are in `input` |
| Write gate | PASS | Each writer requires `dry_run=true` or `confirm=true`; `save=false` is the default |
| Strict paths/types | PASS | Required strings reject non-string JSON; explicit object names must match package names; malformed search roots fail with controlled invalid-params errors |
| Default list scope | PASS | Omitted `path` always scans recursively under `/Game`; focused tests require every returned package to remain inside `/Game` and include project fixtures |
| Dry-run | PASS | Returns `preview_state="proposed"` and proposed values without package/asset or modifier/trigger UObject construction, mutation, dirtying, save, or class-package load |
| Creation transaction | PASS | Unsaved Input Action and Mapping Context creation Undo removes the asset path/registry entry; GC between Undo and Redo cannot collect the transaction-owned UObject; Redo restores the same authored state |
| Selection semantics | PASS | Omitted `context_paths` selects by optional root; explicit `[]` checks zero contexts without a global fallback |
| No-op behavior | PASS | Existing-equivalent edits and absent removal do not dirty or save |
| Clone behavior | PASS | Mapping modifier/trigger cloning and explicit class-array replacement/clear are deterministic |
| Post-mutation save failure | PASS | All five writers return structured error data with `mutation_committed=true`, `partial_mutation=true`, `retry_safe=false`, the save error, and retry guidance |
| Settings independence | PASS | `input` registers before the `bEnableGAS` guard |

The catalog was generated from the C++ registrations with:

```powershell
python D:\P4\speed\Plugins\Monolith\Tools\MonolithQuery\generate_monolith_catalog_snapshot.py `
    --root D:\P4\speed\Saved\GitWorktrees\Monolith-fork-enhanced-input `
    --out "$env:TEMP\monolith-input-catalog.json"
```

---

## 3. Verification Environment

| Engine | Resolved root | Host project | Plugin source |
|--------|---------------|--------------|---------------|
| UE 5.7 | `D:\Engine\UE_5.7` | `D:\P4\MonolithInputUE57Host\MonolithInputUE57Host.uproject` | Main worktree at verified source SHA |
| UE 5.8 | `D:\Engine\UE_5.8` | `D:\P4\MonolithInputUE58Host\MonolithInputUE58Host.uproject` | Detached worktree at the same verified source SHA |

Each root was resolved from its host `.uproject` `EngineAssociation`. The UE 5.8 host used an independent detached checkout at `eecd99dca45a99e70ed1f589f39b5cdf846f2bd6`, so both builds referenced the exact same committed code instead of copying uncommitted files.

---

## 4. Commands

The editor targets were built with each host's resolved engine root and a per-run UBT log, avoiding the shared `%LOCALAPPDATA%\UnrealBuildTool\Log.txt`:

```powershell
$project = "D:\P4\MonolithInputUE57Host\MonolithInputUE57Host.uproject"
$engineRoot = & D:\P4\speed\Build\BatchFiles\Script\ResolveUnrealEngine.ps1 `
    -Project $project -Output Root
if (-not $? -or [string]::IsNullOrWhiteSpace([string]$engineRoot)) {
    throw "Failed to resolve the UE 5.7 engine root"
}
$ubtLog = "D:\P4\MonolithInputUE57Host\Saved\Logs\EnhancedInput-Review4-Accepted-UBT-UE57-20260730-035832.log"
& (Join-Path $engineRoot "Engine\Build\BatchFiles\Build.bat") `
    UnrealEditor Win64 Development "-Project=$project" `
    -WaitMutex -NoHotReloadFromIDE "-Log=$ubtLog"

$project = "D:\P4\MonolithInputUE58Host\MonolithInputUE58Host.uproject"
$engineRoot = & D:\P4\speed\Build\BatchFiles\Script\ResolveUnrealEngine.ps1 `
    -Project $project -Output Root
if (-not $? -or [string]::IsNullOrWhiteSpace([string]$engineRoot)) {
    throw "Failed to resolve the UE 5.8 engine root"
}
$ubtLog = "D:\P4\MonolithInputUE58Host\Saved\Logs\EnhancedInput-Review4-Accepted-UBT-UE58-20260730-040021.log"
& (Join-Path $engineRoot "Engine\Build\BatchFiles\Build.bat") `
    UnrealEditor Win64 Development "-Project=$project" `
    -WaitMutex -NoHotReloadFromIDE "-Log=$ubtLog"
```

Focused automation used the same prefix on both engines. The verification editor disabled the MCP listener and the automatic index run through command-line config overrides, and used `-abslog=<absolute-path>` plus an engine-appropriate report export path:

```powershell
Automation RunTests Monolith.ParamGuard.GAS.InputAssets
```

The eight tests were `Registration`, `WriteGate`, `DryRun`, `StrictParams`, `CreationUndo`, `LifecycleAndClone`, `IdempotencyConflictNoOp`, and `SaveFailureReporting`. `CreationUndo` executes Undo, garbage collection, and Redo for a newly created `UInputAction` and `UInputMappingContext`, verifies the package path disappears on Undo, and verifies the authored description survives GC and Redo. `SaveFailureReporting` holds each target `.uasset` open through `IFileManager::CreateFileWriter`, forcing a real Windows move failure and checking all five writers' structured post-mutation error contract.

---

## 5. Results

| Gate | UE 5.7 | UE 5.8 |
|------|--------|--------|
| Full editor target build | PASS | PASS |
| Focused automation | PASS, 8/8 | PASS, 8/8 |
| Automation warnings / errors | 0 / 0 | 0 / 0 |
| Build log | `D:\P4\MonolithInputUE57Host\Saved\Logs\EnhancedInput-FinalReview-Build-UE57-20260730-233529.out.log` | `D:\P4\MonolithInputUE58Host\Saved\Logs\EnhancedInput-FinalReview-Build-UE58-20260730-233612.out.log` |
| UBT log | `D:\P4\MonolithInputUE57Host\Saved\Logs\EnhancedInput-FinalReview-UBT-UE57-20260730-233529.log` | `D:\P4\MonolithInputUE58Host\Saved\Logs\EnhancedInput-FinalReview-UBT-UE58-20260730-233612.log` |
| Report | `D:\P4\MonolithInputUE57Host\Saved\Automation\EnhancedInput-FinalReview-TestExit-UE57-20260730-233837\index.json` | `D:\P4\MonolithInputUE58Host\Saved\Automation\EnhancedInput-FinalReview-TestExit-UE58-20260730-233929\index.json` |
| `UnrealEditor-MonolithGAS.dll` bytes | 2,521,088 | 2,417,152 |
| DLL SHA256 | `0D5D521331E7A2F1FD5927973AA94A7DDE251C39816E6304243C01D046348557` | `3DFAC53B2E45111CA677F0F8B37AD3479A390ADAC837A7AC1059687B2E372F59` |
| Residual `Content\Tests\Monolith\Input` files | 0 | 0 |

Both final-review full-target invocations compile the two changed translation units, relink `UnrealEditor-MonolithGAS.dll`, and end with `Result: Succeeded` (5/5 affected actions). Both `-TestExit="Automation Test Queue Empty"` automation reports contain eight successes, zero successes-with-warnings, zero failures, and zero not-run tests. The earlier accepted clean-host full builds remain the from-scratch module baseline; the final-review builds prove the exact changed source and test inputs are linked.

---

## 6. Live MCP Review Matrix

The final UE 5.8 DLL ran in the disposable host on `127.0.0.1:9435` (PID 67644). The server reported Monolith `0.21.3`, UE 5.8, 1,275 actions, and the expected disposable project. Ten exact schemas were obtained through `describe_query(action_schema)` before the input and editor calls.

| Review concern | Live result |
|----------------|-------------|
| Schema discovery | Exact schemas were checked for eight `input` actions plus `editor.run_console_command` and `editor.list_dirty_packages` before use |
| Default list | `list_input_actions` with omitted `path` completed successfully; the automation fixture separately proved every returned package remained under `/Game` and that the project fixture was included |
| Proposed creation | `create_input_action` dry-run returned `preview_state: proposed`; a scoped list immediately afterward remained at `count: 0` |
| Deferred class resolution | Mapping dry-run accepted an unloaded Blueprint-generated class path, returned `class_resolution: deferred_until_confirm`, and left the context at `mapping_count: 0` |
| Mapping add/readback | Confirmed add produced one mapping with `InputModifierNegate` and `InputTriggerHold`; validation returned `valid: true` |
| Proposed removal | Removal dry-run returned `preview_state: proposed`; immediate readback remained at `mapping_count: 1` |
| GC-safe creation transaction | A second Input Action was created, Undo reduced the scoped action list to one, `OBJ GC` ran, and Redo restored `Accepted GC live transaction` |
| Persistence side effects | All four live writes returned `saved: false`; exactly three scoped dirty packages were audited before graceful `QUIT_EDITOR`; afterward PID 67644 exited, port 9435 was released, and residual files were 0 |

Live editor log: `D:\P4\MonolithInputUE58Host\Saved\Logs\EnhancedInput-Review4-AcceptedClean-LiveMCP-UE58-20260730-041422.log`. It contains zero Monolith error/assert/ensure lines. The only two Monolith warnings are the paired startup `MODAL_OPEN`/`MODAL_CLOSE` diagnostics for an immediately dismissed empty asset-editor tab.

---

## 7. Review Fix Rationale

Simply moving `FScopedTransaction` before `NewObject` was not sufficient: an initial live test showed Undo restoring constructor defaults while leaving the new object at its package path. The final implementation records a package-stable `FCommandChange` before construction, suppresses incidental constructor/Asset Registry serialization inside that transaction, and lets the custom change move the asset to/from the transient package while keeping the Asset Registry synchronized. The command change retains the transient UObject reference across Revert instead of marking it as garbage, so the final gate requires path disappearance on Undo, an intervening GC, and exact state restoration on Redo rather than checking `RF_Transactional` alone.

Dry-run modifier/trigger validation now separates syntax from loading: `FSoftClassPath` validates the string, `ResolveClass` validates an already-loaded class when available, and `StaticLoadClass` runs only after a confirmed write. This preserves a truthful preview while preventing Blueprint package loads from a dry-run.

All five writers share one post-mutation completion path. When `save=true` fails after an in-memory mutation, the action returns an error rather than a success-shaped payload and explicitly tells the caller that the mutation is committed, partial, and unsafe to retry blindly.

### Review round 3 (head `d5a05516`)

Both remaining findings were confirmed and closed.

Every asset-path parameter in the `input` schemas (`asset_path`, `context_path`, `action_path`, `source_context_path`, `source_action_path`, and the list roots `path`) now uses the `RequiredAssetPath` / `OptionalAssetPath` builder sugar instead of the generic `Required` / `Optional` entries. Those generic entries were classified as `EMonolithParamKind::Other`, so the registry's `AssetPath` rewrite never ran and a Windows client supplying backslashes had its path rejected after normalization produced a malformed `/Game/...` value. The namespace now behaves like the repository's other asset APIs.

`Monolith.ParamGuard.GAS.InputAssets.SaveFailureReporting` is gated to Windows. `FScopedSaveBlocker` provokes the failure by holding a write handle open on the destination `.uasset`, which only blocks `SavePackage` where file locking is mandatory. POSIX locking is advisory and does not prevent the temp-file rename, so on macOS/Linux the save would succeed and every structured-failure assertion would fail; the expected log messages are Windows-specific for the same reason. The save-failure contract is platform independent, but this way of provoking it is not, so the test reports an explicit skip rather than a false failure.

### Review round 4 (2026-07-30)

The final three fresh findings were confirmed and fixed at their contract boundaries.

- `NormalizeAndValidateInputAssetPath` now normalizes `\` separators itself. This covers `context_paths` array elements, which cannot use the dispatcher's scalar `AssetPath` rewrite, and also makes direct handler-level validation consistent with dispatched scalar paths.
- The core registry intentionally recovers schema-declared array/object parameters from JSON-encoded strings for MCP-client compatibility. The input API/spec now states that `modifier_classes`, `trigger_classes`, and `context_paths` accept that compatibility form while still rejecting non-string/empty elements. Automation exercises both a recovered context selection and recovered empty class arrays.
- A source action+key selector no longer silently takes the first duplicate mapping. Ambiguous selectors return invalid-params and require `source_mapping_index`; an explicit index must be in range and identify the requested action+key row. Automation creates two distinguishable duplicate rows, proves the unindexed selector fails, and proves index 1 clones the intended empty modifier/trigger state.

The round also re-ran the already-fixed active review contracts: required strings reject non-string JSON, malformed list roots reject invalid long package names, add/remove dry-runs return `preview_state="proposed"`, and the save-failure test is explicitly Windows-gated.

Final round verification:

- UE 5.7 and UE 5.8 full-target invocations: 5/5 affected compile/link actions, `Result: Succeeded`.
- `Monolith.ParamGuard.GAS.InputAssets`: 8/8 on both engines, warning/error 0/0, queue-empty and `TestExit` confirmed.
- Residual `Content\Tests\Monolith\Input` files: 0 on both hosts.

---

## 8. Visual and Delivery Scope

| Gate | Result | Reason |
|------|--------|--------|
| 1920x1080 screenshot | N/A | The change adds editor-side MCP schemas, handlers, and headless asset automation; it changes no runtime or editor visual presentation. |
| Discord screenshot upload | N/A | No screenshot artifact was required, so `Build\BatchFiles\Script\UploadScreenshotTestsToDiscord.bat` was not run. |

---

## 9. Conclusion

PASS. The verified code adds exactly 10 `input` actions, resolves every actionable finding across all review rounds, preserves strict scalar/mutation/no-op/preview/save-failure contracts while documenting intentional encoded-array compatibility, rejects ambiguous source clones, builds on the supported UE 5.7 floor and UE 5.8, passes 8/8 warning-free focused tests on both engines, completes live schema-first dry-run/commit/readback plus GC-separated creation Undo/Redo through MCP, and leaves no generated test assets behind.
