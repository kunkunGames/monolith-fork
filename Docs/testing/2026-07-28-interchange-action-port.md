# Monolith Interchange Action Port Verification

**Date:** 2026-07-28 (KST)
**Branch:** `agent/interchange-import-pipeline`
**Tested source:** `31feb8f6`
**Engine floor:** Unreal Engine 5.7
**Engine ceiling tested:** Unreal Engine 5.8
**Status:** Passed

---

## 1. Scope

This verification covers the new `MonolithInterchange` editor module and its 16-action `interchange` namespace. The port includes guarded source/destination validation, single and batch import, typed import entry points, reimport-source management, reimport, export, module/spec/API registration, and the matching `unreal-interchange` skill.

No security feature, benchmark, invocation-log feature, metadata-analysis feature, or reinforcement-learning feature is part of this change.

---

## 2. Action Contract

| Group | Actions | Expected behavior |
|-------|---------|-------------------|
| Discovery | `get_supported_formats`, `can_import`, `can_reimport`, `get_import_data` | Read-only format, path, and source-data inspection. |
| Import | `import_asset`, `import_assets`, `import_scene`, `import_mesh`, `import_skeletal_mesh`, `import_texture`, `import_audio`, `import_with_options` | Validate source, destination, conflict policy, confirmation, and dry-run gates before mutation. |
| Reimport/export | `update_reimport_path`, `reimport_asset`, `reimport_assets`, `export_asset` | Validate asset/source/output paths and confirmation before writes. |

---

## 3. Build Verification

Both engine roots were resolved from a host `.uproject` `EngineAssociation` through `Build\BatchFiles\Script\ResolveUnrealEngine.ps1`; no engine path was hard-coded into repository code or scripts.

| Gate | Command shape | Result | Linked artifact |
|------|---------------|--------|-----------------|
| UE 5.7 full plugin UBT | `UnrealBuildTool UnrealEditor Win64 Development -Plugin=<Monolith.uplugin> -WaitMutex -NoHotReloadFromIDE` with `MONOLITH_RELEASE_BUILD=1` | Passed — `Result: Succeeded` | `Binaries\Win64\UnrealEditor-MonolithInterchange.dll`, 238,080 bytes, SHA-256 `B5F02E36C00D91AF30489C8165E9E4D4181C7EE5A42E470243E57F8B0B7DC1CC` |
| UE 5.8 host-project UBT | `UnrealBuildTool MonolithForkValidationHostEditor Win64 Development -Project=<host.uproject> -WaitMutex -NoHotReloadFromIDE` with `MONOLITH_RELEASE_BUILD=1` | Passed — `Result: Succeeded` | `Binaries\Win64\UnrealEditor-MonolithInterchange.dll`, 223,232 bytes, SHA-256 `5CBED8F443B2B5EC271D40C4F7790A0ED34E3791CDABCC932686F8AC1DE670EB` |

The UE 5.8 build used an isolated minimal host because the 5.8 UBT does not reliably resolve external plugin module rules from the direct `UnrealEditor -Plugin=...` form. The host linked `Plugins\Monolith` to the exact detached tested source and did not modify the Speed checkout or another PR worktree.

---

## 4. Catalog and Automation Verification

| Gate | Evidence | Result |
|------|----------|--------|
| Descriptor parsing | `Monolith.uplugin` parsed as JSON and contained exactly one `MonolithInterchange` module entry. | Passed |
| Catalog regeneration | The generated catalog increased from 1,561 to 1,577 actions. Exactly 16 generated entries used namespace `interchange`. | Passed |
| Focused automation | `Monolith.ParamGuard.MonolithInterchange.ImportRejectsMalformedParams` | Passed: 1 succeeded, 0 warnings, 0 errors |
| Complete registration | The focused test enumerated and asserted all 16 expected action names. | Passed |
| Mutation guard | Missing `source_file` failed schema validation; a nonexistent source with `dry_run=true` returned a structured guarded-failure row without creating an asset. | Passed |
| Registry noise | Final automation log contained 0 `Overwriting existing action: interchange.*` warnings. | Passed |
| Patch hygiene | `git diff --check` and excluded-scope path scan | Passed |

Automation evidence:

- `D:\P4\MonolithInterchangeUE58Host\Saved\Automation\InterchangeActionPort-UE58-Final\index.json`
- `D:\P4\MonolithInterchangeUE58Host\Saved\Logs\InterchangeActionPort-Automation-UE58-Final.log`

---

## 5. Side-Effect Review

| Scenario | Expected side effect | Verified behavior |
|----------|----------------------|-------------------|
| Read-only discovery | No package or file mutation | Registry and discovery handlers remain read-only. |
| Missing source | No import attempt | Rejected before `AssetTools` import. |
| Invalid `/Game` destination | No package creation | Destination validation runs before mutation. |
| Import/reimport/export without confirmation | No write | Write actions require `confirm=true` unless explicitly dry-running. |
| Batch row failure | Other rows remain inspectable | Batch handlers return one structured row per input. |

---

## 6. Visual and Discord Evidence

Screenshot verification was not applicable. The change adds editor-side import pipeline actions and has no visual, gameplay, UMG, VFX, animation-presentation, material-presentation, or level-presentation output. Therefore `Build\BatchFiles\Script\UploadScreenshotTestsToDiscord.bat` was not run.
