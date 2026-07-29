# Enhanced Input Action Port Verification

**Date:** 2026-07-28
**Review hardening verified:** 2026-07-30
**Scope:** `input` namespace in `MonolithGAS`
**Base:** `ee1dae25f9a90a45ae768abbfcb0d9356810b0c4`
**Verified code source:** `3539e208785d6291631c420e24bab80b34f67a35`
**Engines:** Unreal Engine 5.7 and 5.8

---

## 1. Goal

Verify that the independent Enhanced Input port adds exactly 10 guarded asset actions, resolves every actionable PR review finding, compiles from the same committed source on UE 5.7 and UE 5.8, passes focused automation including real creation Undo/Redo, works through the live MCP surface, and leaves no test assets on disk.

---

## 2. Surface and Contracts

| Surface | Result | Evidence |
|---------|--------|----------|
| Read actions | 5 | `list_input_actions`, `get_input_action`, `list_input_mapping_contexts`, `get_input_mapping_context`, `validate_input_mappings` |
| Write actions | 5 | `create_input_action`, `set_input_action_properties`, `create_input_mapping_context`, `add_input_mapping`, `remove_input_mapping` |
| Catalog delta | PASS | Base 1561 → head 1571; all 10 additions are in `input` |
| Write gate | PASS | Each writer requires `dry_run=true` or `confirm=true`; `save=false` is the default |
| Strict paths/types | PASS | Required strings reject non-string JSON; explicit object names must match package names; malformed search roots fail with controlled invalid-params errors |
| Dry-run | PASS | Returns `preview_state="proposed"` and proposed values without package/asset or modifier/trigger UObject construction, mutation, dirtying, or save |
| Creation transaction | PASS | Unsaved Input Action and Mapping Context creation Undo removes the asset path/registry entry; Redo restores the same authored state |
| Selection semantics | PASS | Omitted `context_paths` selects by optional root; explicit `[]` checks zero contexts without a global fallback |
| No-op behavior | PASS | Existing-equivalent edits and absent removal do not dirty or save |
| Clone behavior | PASS | Mapping modifier/trigger cloning and explicit class-array replacement/clear are deterministic |
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

Each root was resolved from its host `.uproject` `EngineAssociation`. The UE 5.8 host used an independent detached checkout at `3539e208785d6291631c420e24bab80b34f67a35`, so both builds referenced the exact same committed code instead of copying uncommitted files.

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
$ubtLog = "D:\P4\MonolithInputUE57Host\Saved\Logs\EnhancedInputActionPort-Review-UBT-UE57-20260730-010356.log"
& (Join-Path $engineRoot "Engine\Build\BatchFiles\Build.bat") `
    UnrealEditor Win64 Development "-Project=$project" `
    -WaitMutex -NoHotReloadFromIDE "-Log=$ubtLog"

$project = "D:\P4\MonolithInputUE58Host\MonolithInputUE58Host.uproject"
$engineRoot = & D:\P4\speed\Build\BatchFiles\Script\ResolveUnrealEngine.ps1 `
    -Project $project -Output Root
if (-not $? -or [string]::IsNullOrWhiteSpace([string]$engineRoot)) {
    throw "Failed to resolve the UE 5.8 engine root"
}
$ubtLog = "D:\P4\MonolithInputUE58Host\Saved\Logs\EnhancedInputActionPort-Review-UBT-UE58-20260730-010512.log"
& (Join-Path $engineRoot "Engine\Build\BatchFiles\Build.bat") `
    UnrealEditor Win64 Development "-Project=$project" `
    -WaitMutex -NoHotReloadFromIDE "-Log=$ubtLog"
```

Focused automation used the same prefix on both engines:

```powershell
Automation RunTests Monolith.ParamGuard.GAS.InputAssets
```

The seven tests were `Registration`, `WriteGate`, `DryRun`, `StrictParams`, `CreationUndo`, `LifecycleAndClone`, and `IdempotencyConflictNoOp`. `CreationUndo` executes both Undo and Redo for a newly created `UInputAction` and `UInputMappingContext`, verifies the package path disappears on Undo, and verifies the authored description survives Redo.

---

## 5. Results

| Gate | UE 5.7 | UE 5.8 |
|------|--------|--------|
| Full editor target build | PASS | PASS |
| Focused automation | PASS, 7/7 | PASS, 7/7 |
| Automation warnings / errors | 0 / 0 | 0 / 0 |
| Build log | `D:\P4\MonolithInputUE57Host\Saved\Logs\EnhancedInputActionPort-Review-Build-UE57-20260730-010356.out.log` | `D:\P4\MonolithInputUE58Host\Saved\Logs\EnhancedInputActionPort-Review-Build-UE58-20260730-010512.out.log` |
| UBT log | `D:\P4\MonolithInputUE57Host\Saved\Logs\EnhancedInputActionPort-Review-UBT-UE57-20260730-010356.log` | `D:\P4\MonolithInputUE58Host\Saved\Logs\EnhancedInputActionPort-Review-UBT-UE58-20260730-010512.log` |
| Report | `D:\P4\MonolithInputUE57Host\Saved\Automation\EnhancedInputActionPort-Review-UE57-20260730-010418\index.json` | `D:\P4\MonolithInputUE58Host\Saved\Automation\EnhancedInputActionPort-Review-UE58-20260730-010541\index.json` |
| `UnrealEditor-MonolithGAS.dll` bytes | 2,483,712 | 2,381,824 |
| DLL SHA256 | `2E15B91A52D184639E7A4D18238776EAC83227B73FABECCF3D5F9DEDB61B6939` | `F09AAFA45497473AA6DDC425EAD3CD87919AACD45353A3D613228A89503C677D` |
| Residual `Content\Tests\Monolith\Input` files | 0 | 0 |

Both accepted build logs contain `Result: Succeeded`. Both automation reports contain seven successes, zero successes-with-warnings, zero failures, and zero not-run tests.

---

## 6. Live MCP Review Matrix

The final UE 5.8 DLL ran in the disposable host on `127.0.0.1:9435` (PID 69216). Schemas were obtained through `describe_query(action_schema)` before mutation calls.

| Review concern | Live result |
|----------------|-------------|
| Skill schema discovery | The workflow obtained exact action schemas through `describe_query(action_schema)` before every mutation family instead of relying on terse discovery output |
| Required string type | Numeric `asset_path` rejected: `Malformed parameter: asset_path must be a string` |
| Explicit object-name mismatch | `/IA_Mismatch.Other` rejected because `Other` does not match `IA_Mismatch` |
| Malformed list root | `/Game//Input` rejected by Unreal's LongPackageName validation |
| Explicit empty selection | `context_paths: []` returned `contexts_checked: 0` even with a malformed unused `path` |
| Proposed dry-run response | New action preview returned `Axis2D`, requested description/flags, `Cumulative`, `preview_state: proposed`, and `would_create: true` |
| Creation Undo/Redo | Undo made `get_input_action` return `Asset not found`; Redo restored the asset and `Live transaction round trip` description |
| Mapping dry-run | Source clone predicted `after_count: 2`, one modifier, and one trigger; immediate context read remained at `mapping_count: 1` |
| Mapping commit/readback | Confirmed clone produced `mapping_count: 2` with `InputModifierNegate` and `InputTriggerHold`; validation returned `valid: true` |
| Persistence side effects | Every live write used `save=false`; after graceful `QUIT_EDITOR`, port 9435 was released and no test `.uasset` existed |

Live editor log: `D:\P4\MonolithInputUE58Host\Saved\Logs\MonolithInputUE58Host.log`.

---

## 7. Review Fix Rationale

Simply moving `FScopedTransaction` before `NewObject` was not sufficient: an initial live test showed Undo restoring constructor defaults while leaving the new object at its package path. The final implementation records a package-stable `FCommandChange` before construction, suppresses incidental constructor/Asset Registry serialization inside that transaction, and lets the custom change move the asset to/from the transient package while keeping the Asset Registry synchronized. This is why the final gate requires both path disappearance on Undo and exact state restoration on Redo rather than checking `RF_Transactional` alone.

---

## 8. Visual and Delivery Scope

| Gate | Result | Reason |
|------|--------|--------|
| 1920x1080 screenshot | N/A | The change adds editor-side MCP schemas, handlers, and headless asset automation; it changes no runtime or editor visual presentation. |
| Discord screenshot upload | N/A | No screenshot artifact was required, so `Build\BatchFiles\Script\UploadScreenshotTestsToDiscord.bat` was not run. |

---

## 9. Conclusion

PASS. The verified code adds exactly 10 `input` actions, resolves all eight actionable review findings, preserves strict mutation/no-op/preview contracts, builds on the supported UE 5.7 floor and UE 5.8, passes 7/7 warning-free focused tests on both engines, completes live creation Undo/Redo and mapping clone/readback through MCP, and leaves no generated test assets behind.
