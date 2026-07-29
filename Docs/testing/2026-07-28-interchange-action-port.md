# Monolith Interchange Action Port Verification

**Date:** 2026-07-29 (KST)
**Branch:** `agent/interchange-import-pipeline`
**Pinned implementation source:** `ea6bc58f8d259715d16cc859fd6f7809edaaf7c9`
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
| Batch preview | Dry-run reserves each successful row's prospective package, so later same-name sources see the same `fail` conflict or unique `rename` suffix as a confirmed sequence. |
| Preflight normalization | `can_import` applies the same destination-folder whitespace/trailing-slash normalization as import. |
| Exact source index | A present `source_file_index` must be integer JSON; a string such as `"1"` cannot fall back to `INDEX_NONE`. |
| Plugin load contract | `Monolith.uplugin` enables the engine's `Interchange` plugin because `MonolithInterchange` hard-links `InterchangeEngine`. |
| Workflow truthfulness | The skill uses `describe_query(action_schema)` and treats `[w]` as external side effects rather than promising universal editor Undo. |

No security benchmark, metadata-analysis feature, reinforcement-learning feature, runtime gameplay feature, or asset-presentation change is part of this work.

---

## 2. Action Contract

| Group | Actions | Expected behavior |
|-------|---------|-------------------|
| Discovery | `get_supported_formats`, `can_import`, `can_reimport`, `get_import_data` | Read-only format, backend, reimport-handler, path, and source-data inspection. |
| Import | `import_asset`, `import_assets`, `import_scene`, `import_mesh`, `import_skeletal_mesh`, `import_texture`, `import_audio` | Validate a concrete backend, typed compatibility, link-safe source path, `/Game` destination, collision policy, confirmation, and dry-run gates before mutation. Batch preview includes intra-batch prospective conflicts. |
| Reimport/export | `update_reimport_path`, `reimport_asset`, `reimport_assets`, `export_asset` | Validate handler availability, every relevant source, output path, exporter, confirmation, and post-update readback before reporting success. |

Live `monolith_discover` returned exactly 15 actions and did not advertise `import_with_options`.

---

## 3. Build Verification

Both engine roots were resolved from the applicable disposable host `.uproject` `EngineAssociation` through `Build\BatchFiles\Script\ResolveUnrealEngine.ps1`; repository code and scripts do not hard-code Unreal Engine paths. Each engine used a unique host target whose `Plugins\Monolith` junction pointed at the exact source under test.

| Gate | Result | Evidence | Linked artifact |
|------|--------|----------|-----------------|
| UE 5.7 host-project UBT | Passed — final target invocation at `ea6bc58f`, `Result: Succeeded` | `D:\P4\speed\Saved\ValidationHosts\MonolithPR1UE57\Saved\Logs\PR1-Interchange-Review2-Build-UE57-20260730-013558.out.log` | 295,424 bytes, SHA-256 `8F20F6BB58E91CD318CFD271B1C747D7385E5FAD7BF6DAF332A765ACB28BD77D` |
| UE 5.8 host-project UBT | Passed — final target invocation at `ea6bc58f`, `Result: Succeeded` | `D:\P4\speed\Saved\ValidationHosts\MonolithPR1UE58\Saved\Logs\PR1-Interchange-Review2-Build-UE58-20260730-013018.out.log` | 276,992 bytes, SHA-256 `859E381E22EE6C7BD699197929BF600D482B8432446EFE80E8022EFC51DE58D8` |
| Native proxy | Passed — Visual Studio located through `vswhere`, optimized x64 executable linked | `Tools\MonolithProxy\build_proxy.bat` | 559,104 bytes, SHA-256 `CA5625EB2843FF8917B2E14C85F5BFC3361047CF011BB96123C5905AB807B200` |
| Python proxy syntax | Passed | `python -m py_compile Scripts\monolith_proxy.py` | N/A |

An earlier UE 5.7 direct `UnrealEditor -Plugin=...` attempt was explicitly rejected as evidence because its log contained UE 5.8 host PCH/source paths. The accepted UE 5.7 result comes from the unique `MonolithPR1UE57HostEditor` target and a detached worktree pinned to the implementation source above. The final UE 5.8 run uses the separate UE 5.8 host and the main PR worktree at that same source.

One intermediate UE 5.8 invocation at `558b3092` exposed a real cross-version compile error: UE 5.8 stores `FJsonObject::Values` under a shared-string key, so direct `Values.Find(FString)` is not portable even though it compiles on UE 5.7. The final source uses the public `FJsonObject::TryGetField` API; both accepted builds above are from that corrected `ea6bc58f` source.

---

## 4. Automation and Live MCP Verification

| Gate | UE 5.7 | UE 5.8 |
|------|--------|--------|
| Focused automation | 1 succeeded, 0 warnings, 0 errors | 1 succeeded, 0 warnings, 0 errors |
| Report | `D:\P4\speed\Saved\ValidationHosts\MonolithPR1UE57\Saved\Automation\PR1-Interchange-Review2-UE57-20260730-013731\index.json` | `D:\P4\speed\Saved\ValidationHosts\MonolithPR1UE58\Saved\Automation\PR1-Interchange-Review2-UE58-20260730-013658\index.json` |
| Test | `Monolith.ParamGuard.MonolithInterchange.ImportRejectsMalformedParams` | `Monolith.ParamGuard.MonolithInterchange.ImportRejectsMalformedParams` |

The focused test verifies complete 15-action registration, absence of `import_with_options`, schema rejection for a missing source parameter, guarded handling of a missing file, audio-vs-PNG typed mismatch with the exact `typed_import_extension_mismatch` code, dry-run rejection when no exporter supports the requested extension, `can_import` destination normalization, `fail`/`rename` same-name batch preview behavior, and controlled rejection of a nonnumeric source index.

Final UE 5.8 follow-up live MCP evidence at `ea6bc58f` used port `9431`; `/health` reported `status=ok`, version `0.21.3`, and 1,342 registered tools. Exact schemas were fetched through `describe_query(action_schema)` before domain calls. The editor was closed through `editor.run_console_command("QUIT_EDITOR")`; PID 49724 exited and port 9431 was released.

| Live scenario | Result |
|---------------|--------|
| Namespace discovery | 15 actions; `can_import` described real translator/factory validation; `import_with_options` absent. |
| Junction escape | `Saved/LinkEscape/linked.png` returned `can_import=false`, `blocked_link_traversal=true`, and `linked_source_blocked`. |
| Direct source | `Saved/Direct/valid.png` returned `can_import=true`, an Interchange translator, and `/Script/UnrealEd.TextureFactory`. |
| Typed mutation | Confirmed `import_texture` returned `status=imported`, `requested_import_kind=texture`, and one matching `Texture2D`. |
| Rename collision | A second dry run detected the loaded `valid` package and resolved it to `valid1`. |
| Reimport | `can_reimport=true` with one handler source; confirmed `update_reimport_path` returned `readback_matches=true`. |
| Export preflight | `.foo` dry run returned `status=error`, `exporter_available=false`, and `exporter_unavailable`. |
| Destination normalization | `can_import` accepted surrounding whitespace and a trailing slash, returning the normalized valid `/Game/Tests/Monolith/Interchange/LiveReview2_20260730_0139` path. |
| Same-name batch `fail` preview | First `duplicate.png` returned `would_import`; the second returned `error` with `likely_package_conflict=true`. |
| Same-name batch `rename` preview | Rows resolved to distinct `duplicate` and `duplicate1` packages without creating either asset. |
| Nonnumeric source index | `source_file_index: "1"` returned `status=error` with `invalid_source_file_index`. |

The confirmed test mutation existed only in the disposable validation host and remained unsaved. The follow-up used dry-run only; its two temporary source copies were deleted after the editor exited, and neither host contains a residual `Content\Tests\Monolith\Interchange` asset.

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
| Dry-run ignored intra-batch conflicts | Successful preview rows reserve prospective package names; automation and live MCP prove `fail` and `rename` parity. |
| `can_import` contradicted import normalization | Both paths use `NormalizePackageFolder`; live preflight returned the normalized valid path. |
| Nonnumeric source index fell back to all/default sources | Both reimport handlers use exact JSON number/integer validation; live string input returned `invalid_source_file_index`. |
| Interchange engine plugin was not declared | `Monolith.uplugin` enables `Interchange`; UE 5.7 and UE 5.8 host builds both load the hard module dependency. |
| Skill promised transaction-wrapped external writes | `[w]` now means side effects, documents handler/filesystem Undo limits, and uses supported on-demand schema discovery. |

---

## 7. Visual and Discord Evidence

Screenshot verification was not applicable. The change affects editor-side import pipeline contracts, proxy startup discovery, and build tooling; it has no gameplay, UMG, VFX, animation-presentation, material-presentation, level-presentation, or editor-UI visual output.

Therefore `Build\BatchFiles\Script\UploadScreenshotTestsToDiscord.bat` was not run.
