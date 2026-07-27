# Enhanced Input Action Port Verification

**Date:** 2026-07-28
**Scope:** `input` namespace in `MonolithGAS`
**Base:** `ee1dae25f9a90a45ae768abbfcb0d9356810b0c4`
**Verified source:** `0e561866acd236dec8740683e6059348e9841d7d`
**Engines:** Unreal Engine 5.7 and 5.8

---

## 1. Goal

Verify that the independent Enhanced Input port adds exactly 10 guarded asset actions, compiles from the same source commit on UE 5.7 and UE 5.8, passes the focused automation suite, and leaves no test assets on disk.

---

## 2. Surface and Contracts

| Surface | Result | Evidence |
|---------|--------|----------|
| Read actions | 5 | `list_input_actions`, `get_input_action`, `list_input_mapping_contexts`, `get_input_mapping_context`, `validate_input_mappings` |
| Write actions | 5 | `create_input_action`, `set_input_action_properties`, `create_input_mapping_context`, `add_input_mapping`, `remove_input_mapping` |
| Catalog delta | PASS | Base 1561 → head 1571; all 10 additions are in `input` |
| Write gate | PASS | Each writer requires `dry_run=true` or `confirm=true`; `save=false` is the default |
| Dry-run | PASS | No object/package creation, mutation, dirtying, or save |
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

Each root was resolved from its host `.uproject` `EngineAssociation`. The UE 5.8 host used an independent detached checkout so both builds referenced the exact same committed source instead of copying uncommitted files.

---

## 4. Commands

The editor targets were built with each resolved engine's standard UBT entry point:

```powershell
& D:\Engine\UE_5.7\Engine\Build\BatchFiles\Build.bat `
    UnrealEditor Win64 Development `
    "-Project=D:\P4\MonolithInputUE57Host\MonolithInputUE57Host.uproject" `
    -WaitMutex -NoHotReloadFromIDE

& D:\Engine\UE_5.8\Engine\Build\BatchFiles\Build.bat `
    UnrealEditor Win64 Development `
    "-Project=D:\P4\MonolithInputUE58Host\MonolithInputUE58Host.uproject" `
    -WaitMutex -NoHotReloadFromIDE
```

Focused automation used the same prefix on both engines:

```powershell
Automation RunTests Monolith.ParamGuard.GAS.InputAssets
```

The six tests were `Registration`, `WriteGate`, `DryRun`, `StrictParams`, `LifecycleAndClone`, and `IdempotencyConflictNoOp`.

---

## 5. Results

| Gate | UE 5.7 | UE 5.8 |
|------|--------|--------|
| Full editor target build | PASS | PASS |
| Focused automation | PASS, 6/6 | PASS, 6/6 |
| Automation warnings / errors | 0 / 0 | 0 / 0 |
| Report | `D:\P4\MonolithInputUE57Host\Saved\Automation\InputAssets-20260728-0252\index.json` | `D:\P4\MonolithInputUE58Host\Saved\Automation\InputAssets-20260728-0256\index.json` |
| `UnrealEditor-MonolithGAS.dll` bytes | 2,437,632 | 2,339,328 |
| DLL SHA256 | `675CEB2F041DDE0D460E1125946B393BC670A7898116913122BA8F67D3464B39` | `E8C16359883A5BDFA7263B5CF31AF17E772301EF92C4D9A2A5D3EF75764F883F` |
| Residual `Content\Tests\Monolith\Input` files | 0 | 0 |

UE 5.7 accepted the deprecated `-ReportOutputPath` flag with a warning; UE 5.8 used `-ReportExportPath`. The UE 5.8 full target build emitted only pre-existing deprecation warnings outside the changed `MonolithGAS` source.

---

## 6. Visual and Delivery Scope

| Gate | Result | Reason |
|------|--------|--------|
| 1920x1080 screenshot | N/A | The change adds editor-side MCP schemas, handlers, and headless asset automation; it changes no runtime or editor visual presentation. |
| Discord screenshot upload | N/A | No screenshot artifact was required, so `Build\BatchFiles\Script\UploadScreenshotTestsToDiscord.bat` was not run. |

---

## 7. Conclusion

PASS. The verified source adds exactly 10 `input` actions, preserves strict mutation and no-op contracts, builds on the supported UE 5.7 floor and UE 5.8, passes 6/6 focused tests on both engines, and leaves no generated test assets behind.
