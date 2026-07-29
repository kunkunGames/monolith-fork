# Monolith Interchange Action Port Verification

**Date:** 2026-07-29 (KST)
**Branch:** `agent/interchange-import-pipeline`
**Pinned implementation source:** `2edb668cf96e6efa0c7a1f32330c43ae9a7090f6`
**Engine floor:** Unreal Engine 5.7
**Engine ceiling tested:** Unreal Engine 5.8
**Status:** Passed

---

## 1. Scope

This verification covers the `MonolithInterchange` editor module, its 15-action `interchange` namespace, the native and Python proxy fallback surfaces, and the matching `unreal-interchange` skill.

The review hardening replaces advertised-but-unimplemented behavior with concrete Unreal contracts:

| Concern | Verified contract |
|---------|-------------------|
| Import capability | `can_import` requires a real Interchange translator or loaded legacy factory, not module presence alone. |
| Typed import | Scene, static-mesh, skeletal-mesh, texture, and audio entry points select compatible backends and validate returned object types. |
| Conflicts | `fail` rejects an existing package, `overwrite` enables replacement, and `rename` uses `IAssetTools::CreateUniqueAssetName` plus `UAssetImportTask::DestinationName`. |
| Reimport | `FReimportManager::CanReimport` is authoritative; stored and replacement sources pass existence/root/link checks; confirmed path updates require handler readback equality. |
| Export | A matching `UExporter` must exist before either dry-run or confirmed export reports success. |
| File roots | Lexical containment is insufficient; every path component below an allowed root is checked for symlink/junction traversal. |
| API truthfulness | The inert `import_with_options` action was removed instead of echoing options that Unreal never applied. |

No security benchmark, metadata-analysis feature, reinforcement-learning feature, runtime gameplay feature, or asset-presentation change is part of this work.

---

## 2. Action Contract

| Group | Actions | Expected behavior |
|-------|---------|-------------------|
| Discovery | `get_supported_formats`, `can_import`, `can_reimport`, `get_import_data` | Read-only format, backend, reimport-handler, path, and source-data inspection. |
| Import | `import_asset`, `import_assets`, `import_scene`, `import_mesh`, `import_skeletal_mesh`, `import_texture`, `import_audio` | Validate a concrete backend, typed compatibility, link-safe source path, `/Game` destination, collision policy, confirmation, and dry-run gates before mutation. |
| Reimport/export | `update_reimport_path`, `reimport_asset`, `reimport_assets`, `export_asset` | Validate handler availability, every relevant source, output path, exporter, confirmation, and post-update readback before reporting success. |

Live `monolith_discover` returned exactly 15 actions and did not advertise `import_with_options`.

---

## 3. Build Verification

Both engine roots were resolved from the applicable disposable host `.uproject` `EngineAssociation` through `Build\BatchFiles\Script\ResolveUnrealEngine.ps1`; repository code and scripts do not hard-code Unreal Engine paths. Each engine used a unique host target whose `Plugins\Monolith` junction pointed at the exact source under test.

| Gate | Result | Evidence | Linked artifact |
|------|--------|----------|-----------------|
| UE 5.7 host-project UBT | Passed — 445 actions, `Result: Succeeded`, no UE 5.8/primary-worktree path matches | `Saved\ReviewBuilds\PR1-UE57-Host-20260729-231212.ubt.log` | 275,968 bytes, SHA-256 `C0656A239E5C3A31ACF1EA41C8C9321F6E5087F5D37808CC57288D6C6BD56056` |
| UE 5.8 host-project UBT | Passed — 438 actions, `Result: Succeeded` | `Saved\ReviewBuilds\PR1-UE58-PostTest-20260729-230906.ubt.log` | 258,560 bytes, SHA-256 `32834CD3A4E4129F4C54E8EC10D24425B7FC3E2D7076BD3D867D57C9BB8A3B3B` |
| Native proxy | Passed — Visual Studio located through `vswhere`, optimized x64 executable linked | `Tools\MonolithProxy\build_proxy.bat` | 559,104 bytes, SHA-256 `CA5625EB2843FF8917B2E14C85F5BFC3361047CF011BB96123C5905AB807B200` |
| Python proxy syntax | Passed | `python -m py_compile Scripts\monolith_proxy.py` | N/A |

An earlier UE 5.7 direct `UnrealEditor -Plugin=...` attempt was explicitly rejected as evidence because its log contained UE 5.8 host PCH/source paths. The accepted UE 5.7 result comes from the unique `MonolithPR1UE57HostEditor` target and a detached worktree pinned to the implementation source above.

---

## 4. Automation and Live MCP Verification

| Gate | UE 5.7 | UE 5.8 |
|------|--------|--------|
| Focused automation | 1 succeeded, 0 warnings, 0 errors | 1 succeeded, 0 warnings, 0 errors |
| Report | `D:\P4\speed\Saved\ValidationHosts\MonolithPR1UE57\Saved\Automation\PR1-InterchangeFinal-20260729-231742\index.json` | `D:\P4\speed\Saved\ValidationHosts\MonolithPR1UE58\Saved\Automation\PR1-InterchangeFinal-20260729-231305\index.json` |
| Test | `Monolith.ParamGuard.MonolithInterchange.ImportRejectsMalformedParams` | `Monolith.ParamGuard.MonolithInterchange.ImportRejectsMalformedParams` |

The focused test verifies complete 15-action registration, absence of `import_with_options`, schema rejection for a missing source parameter, guarded handling of a missing file, audio-vs-PNG typed mismatch with the exact `typed_import_extension_mismatch` code, and dry-run rejection when no exporter supports the requested extension.

Final UE 5.8 live MCP evidence used port `9431`; `/health` reported `status=ok`, version `0.21.3`, and 1,280 registered tools. The editor was closed through `editor.run_console_command("QUIT_EDITOR")` after verification.

| Live scenario | Result |
|---------------|--------|
| Namespace discovery | 15 actions; `can_import` described real translator/factory validation; `import_with_options` absent. |
| Junction escape | `Saved/LinkEscape/linked.png` returned `can_import=false`, `blocked_link_traversal=true`, and `linked_source_blocked`. |
| Direct source | `Saved/Direct/valid.png` returned `can_import=true`, an Interchange translator, and `/Script/UnrealEd.TextureFactory`. |
| Typed mutation | Confirmed `import_texture` returned `status=imported`, `requested_import_kind=texture`, and one matching `Texture2D`. |
| Rename collision | A second dry run detected the loaded `valid` package and resolved it to `valid1`. |
| Reimport | `can_reimport=true` with one handler source; confirmed `update_reimport_path` returned `readback_matches=true`. |
| Export preflight | `.foo` dry run returned `status=error`, `exporter_available=false`, and `exporter_unavailable`. |

The confirmed test mutation existed only in the disposable validation host, remained unsaved, and did not create a repository asset.

---

## 5. Proxy Fallback Verification

Both fallback implementations were tested with an unreachable endpoint and a fresh isolated cache root.

| Surface | Result |
|---------|--------|
| Native `monolith_proxy.exe` | Returned 21 seed tools with exactly one `interchange_query`. |
| Python `Scripts\monolith_proxy.py` | Returned 21 seed tools with exactly one `interchange_query`. |
| Native build bootstrap | Succeeded when `cl.exe` was initially absent from `PATH`; `vswhere.exe` resolved Visual Studio on a non-`C:` installation drive. |

This proves the new namespace remains discoverable when an MCP client starts before the editor, rather than merely proving that the seed string exists in source.

---

## 6. Side-Effect and Review-Thread Closure

| Review concern | Resolution evidence |
|----------------|---------------------|
| Rename did not create a unique name | Live collision resolved `valid` to `valid1`; import task carries `DestinationName`. |
| `can_reimport` inferred capability | Uses and live-verifies `FReimportManager::CanReimport`. |
| Reimport-path update claimed success blindly | Handler/index preflight plus normalized post-update readback; live result matched. |
| Proxy startup omitted Interchange | Native and Python cold-cache fallback each expose one `interchange_query`. |
| `can_import` checked only module presence | Live direct source reports concrete translator/factory; unavailable backends produce an error row. |
| Stored reimport sources were trusted | Every non-replaced stored source now passes existence/root/link validation; batch accepts explicit `allow_external`. |
| Typed handlers shared generic behavior | Five typed handlers select distinct requested kinds, backend rules, and returned-type checks. |
| Export dry run skipped exporter validation | Automation and live MCP both reject an unsupported extension before dry-run success. |
| Lexical root checks allowed junction escape | Live junction fixture was rejected with `linked_source_blocked`. |
| Skill examples pointed outside allowed roots | Skill examples now use project-relative `SourceArt`/`Saved` paths and explain explicit external authorization. |

---

## 7. Visual and Discord Evidence

Screenshot verification was not applicable. The change affects editor-side import pipeline contracts, proxy startup discovery, and build tooling; it has no gameplay, UMG, VFX, animation-presentation, material-presentation, level-presentation, or editor-UI visual output.

Therefore `Build\BatchFiles\Script\UploadScreenshotTestsToDiscord.bat` was not run.
