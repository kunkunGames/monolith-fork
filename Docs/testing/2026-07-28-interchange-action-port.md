# Monolith Interchange Action Port Verification

**Date:** 2026-07-29; final review verification 2026-07-31 (KST)
**Branch:** `agent/interchange-import-pipeline`
**Pinned implementation source:** `41dbbe4c778684299f8d7b35058d4968b7317dd0`
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
| Optional source path | A present `source_file` must be an exact non-empty JSON string; null, scalar coercion, and whitespace-only replacement paths fail before handler mutation. |
| Replacement compatibility | A replacement source must match the target asset kind and have a real Interchange translator or legacy factory; untyped assets may only retain an equivalent stored extension. |
| Multi-output imports | Scene/mesh imports accept one source, `conflict_policy=fail`, and a dedicated destination proven empty by complete bounded Asset Registry, loaded-object, and filesystem inspection. |
| Export file shape | Export rejects a directory whose name merely ends in a supported extension instead of treating it as a writable output file. |
| Plugin load contract | `Monolith.uplugin` enables the engine's `Interchange` plugin because `MonolithInterchange` hard-links `InterchangeEngine`. |
| Workflow truthfulness | The skill uses `describe_query(action_schema)` and treats `[w]` as external side effects rather than promising universal editor Undo. |

No security benchmark, metadata-analysis feature, reinforcement-learning feature, runtime gameplay feature, or asset-presentation change is part of this work.

---

## 2. Action Contract

| Group | Actions | Expected behavior |
|-------|---------|-------------------|
| Discovery | `get_supported_formats`, `can_import`, `can_reimport`, `get_import_data` | Read-only format, backend, reimport-handler, path, and source-data inspection. |
| Import | `import_asset`, `import_assets`, `import_scene`, `import_mesh`, `import_skeletal_mesh`, `import_texture`, `import_audio` | Validate a concrete backend, typed compatibility, link-safe source path, `/Game` destination, collision policy, confirmation, and dry-run gates before mutation. Batch preview includes intra-batch prospective conflicts. Scene/mesh formats additionally require one source, `fail`, and a destination proven empty through complete bounded inspection. |
| Reimport/export | `update_reimport_path`, `reimport_asset`, `reimport_assets`, `export_asset` | Validate handler availability, every relevant source, replacement-source type/backend compatibility, output file shape, exporter, confirmation, and post-update readback before reporting success. |

Live `monolith_discover` returned exactly 15 actions and did not advertise `import_with_options`.

---

## 3. Build Verification

Both engine roots were resolved from the applicable disposable host `.uproject` `EngineAssociation` through `Build\BatchFiles\Script\ResolveUnrealEngine.ps1`; repository code and scripts do not hard-code Unreal Engine paths. Each engine used a unique host target whose `Plugins\Monolith` junction pointed at the exact source under test.

| Gate | Result | Evidence | Linked artifact |
|------|--------|----------|-----------------|
| UE 5.7 host-project UBT | Passed — final source-content invocation at `41dbbe4c`, `Result: Succeeded` | `D:\P4\speed\Saved\ValidationHosts\MonolithPR1UE57\Saved\Logs\PR1-Interchange-PathFix-Build-UE57-20260731-020444.log` | 349,184 bytes, SHA-256 `90FE50D15DC14B6583312474132BFB1064AA3F79E2DE43FD7AECE5194096A541` |
| UE 5.8 host-project UBT | Passed — final source-content invocation at `41dbbe4c`, `Result: Succeeded` | `D:\P4\MonolithInterchangeUE58Host\Saved\Logs\InterchangeActionPort-PathFix-Build-UE58-20260731-020350.log` | 329,216 bytes, SHA-256 `560AD59BD0627AADFECFB4900C0AFF11BA9107A1B6D97F763E72348D88768F69` |
| Native proxy | Passed — Visual Studio located through `vswhere`, optimized x64 executable linked | `Tools\MonolithProxy\build_proxy.bat` | 559,104 bytes, SHA-256 `CA5625EB2843FF8917B2E14C85F5BFC3361047CF011BB96123C5905AB807B200` |
| Python proxy syntax | Passed | `python -m py_compile Scripts\monolith_proxy.py` | N/A |

An earlier UE 5.7 direct `UnrealEditor -Plugin=...` attempt was explicitly rejected as evidence because its log contained UE 5.8 host PCH/source paths. The accepted UE 5.7 result comes from the unique `MonolithPR1UE57HostEditor` target and a detached worktree whose source content matches the implementation source above. The final UE 5.8 run uses the separate UE 5.8 host with the same source content.

One intermediate UE 5.8 invocation at `558b3092` exposed a real cross-version compile error: UE 5.8 stores `FJsonObject::Values` under a shared-string key, so direct `Values.Find(FString)` is not portable even though it compiles on UE 5.7. The final source retains the public `FJsonObject::TryGetField` fix first verified at `ea6bc58f`; the accepted builds above additionally include every later review hardening commit through `41dbbe4c`.

The next review pass at `92a0b3de` found two post-preflight gaps: typed result validation could report a plain error after Unreal had already created wrong-kind objects, and only `rename` assigned `DestinationName`, allowing sanitized `fail`/`overwrite` imports to drift from the predicted package. Code commit `450b30c7` snapshots destination object paths, rolls back new typed-mismatch assets, reports retained results as an explicit partial mutation, and assigns the resolved name for every conflict policy.

---

## 4. Automation and Live MCP Verification

| Gate | UE 5.7 | UE 5.8 |
|------|--------|--------|
| Focused automation | 1 succeeded, 0 warnings, 0 errors | 1 succeeded, 0 warnings, 0 errors |
| Report | `D:\P4\speed\Saved\ValidationHosts\MonolithPR1UE57\Saved\Automation\PR1-Interchange-PathFix-UE57-20260731-020501\index.json` | `D:\P4\MonolithInterchangeUE58Host\Saved\Automation\InterchangeActionPort-PathFix-UE58-20260731-020412\index.json` |
| Test | `Monolith.ParamGuard.MonolithInterchange.ImportRejectsMalformedParams` | `Monolith.ParamGuard.MonolithInterchange.ImportRejectsMalformedParams` |

The focused test verifies complete 15-action registration, absence of `import_with_options`, schema rejection for a missing or wrongly typed source parameter, guarded handling of a missing file, audio-vs-PNG typed mismatch with the exact `typed_import_extension_mismatch` code, dry-run rejection when no exporter supports the requested extension, `can_import` destination normalization, `fail`/`rename` same-name batch preview behavior using a real PNG fixture, and controlled rejection of a nonnumeric source index. Project-generated fixture paths are converted to full paths before dispatch so UE 5.7 and UE 5.8 exercise the same allowed-root contract.

### Review 3 rollback and resolved-name evidence

| Gate | UE 5.7 | UE 5.8 |
|------|--------|--------|
| Build | Succeeded; 315,392-byte DLL, SHA256 `EB96AE51CC84717ACD4184FC5B6C430AFD8242213BB44FE43927BDB37515880F` | Succeeded; 296,448-byte DLL, SHA256 `556A743EB64F9E5B5D9CE9A98F45C06C6F0BA7B59B8A401DBC104D0E406A046B` |
| Build log | `D:\P4\speed\Saved\ValidationHosts\MonolithPR1UE57\Saved\Logs\PR1-Rollback-Build-UE57-20260730-030405.log` | `D:\P4\speed\Saved\ValidationHosts\MonolithPR1UE58\Saved\Logs\PR1-Rollback-Incremental-Build-UE58-20260730-030252.log` |
| Focused automation | 1 succeeded, 0 warnings, 0 errors | 1 succeeded, 0 warnings, 0 errors |
| Report | `D:\P4\speed\Saved\ValidationHosts\MonolithPR1UE57\Saved\Automation\PR1-Interchange-Review3-Final-UE57-20260730-030432\index.json` | `D:\P4\speed\Saved\ValidationHosts\MonolithPR1UE58\Saved\Automation\PR1-Interchange-Review3-Final-UE58-20260730-030308\index.json` |

The expanded automation creates a new unsaved `UTexture2D`, confirms that rollback identifies and deletes exactly one candidate, then proves an object present in the pre-import snapshot is preserved and classified as a partial-mutation result rather than falsely reported as deleted. Both fixtures are removed before the test exits.

Final live MCP evidence used UE 5.8 port `9431` and fetched `describe.action_schema` before the write. Importing `Saved\Direct\123.png` through `interchange.import_texture` with `conflict_policy=fail` returned `expected_asset_name=Asset_123`, `resolved_asset_name=Asset_123`, and the exact object `/Game/Tests/Monolith/Interchange/NumericNameReview3_20260730_0306/Asset_123.Asset_123`; `matching_kind_count=1`. The editor then exited through the described `editor.run_console_command` schema, port `9431` was released, the copied source fixture was deleted, and no `Asset_123.uasset` remained. Live log: `D:\P4\speed\Saved\ValidationHosts\MonolithPR1UE58\Saved\Logs\PR1-Interchange-Review3-LiveMCP-UE58-20260730-030532.log`.

The latest live MCP follow-up remains pinned to the earlier `ea6bc58f` implementation and used port `9431`; it is retained as transport/schema evidence, not as exact-head runtime proof. `/health` reported `status=ok`, version `0.21.3`, and 1,342 registered tools. Exact schemas were fetched through `describe_query(action_schema)` before domain calls. The editor was closed through `editor.run_console_command("QUIT_EDITOR")`; PID 49724 exited and port 9431 was released. Exact-head proof for `41dbbe4c` is the cross-version build and focused automation evidence above.

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
| Typed mismatch left mutated assets behind a plain error | Destination snapshots distinguish existing objects from new import results. Automation proves complete deletion and pre-existing preservation; incomplete cleanup is `partial_import`, never a plain error. |
| `fail`/`overwrite` could ignore the predicted sanitized name | All policies assign `DestinationName`; live `123.png` import produced the predicted `Asset_123` package exactly. |
| `can_import` contradicted import normalization | Both paths use `NormalizePackageFolder`; live preflight returned the normalized valid path. |
| Nonnumeric source index fell back to all/default sources | Both reimport handlers use exact JSON number/integer validation; live string input returned `invalid_source_file_index`. |
| Interchange engine plugin was not declared | `Monolith.uplugin` enables `Interchange`; UE 5.7 and UE 5.8 host builds both load the hard module dependency. |
| Skill promised transaction-wrapped external writes | `[w]` now means side effects, documents handler/filesystem Undo limits, and uses supported on-demand schema discovery. |
| Optional replacement `source_file` of the wrong JSON type silently used stored metadata | Presence-aware validation returns `invalid_source_file`; the malformed value can no longer turn into a different reimport operation. |
| Scene/mesh imports checked only the primary predicted package | Multi-output imports are restricted to one source, `conflict_policy=fail`, and a destination proven empty by Asset Registry, loaded-object, and bounded filesystem checks. Unknown secondary package names can no longer collide with existing content or sibling batch rows. |
| Replacement reimport source was not checked against the target asset type | Typed assets require format compatibility and a registered backend; unknown asset types require an existing-source extension match. Incompatible replacements return `replacement_source_incompatible`. |
| Export accepted a directory whose name ended in a supported extension | Preflight records `path_is_directory=true` and returns `output_path_is_directory` before dry-run success or filesystem mutation. |

---

## 7. Visual and Discord Evidence

Screenshot verification was not applicable. The change affects editor-side import pipeline contracts, proxy startup discovery, and build tooling; it has no gameplay, UMG, VFX, animation-presentation, material-presentation, level-presentation, or editor-UI visual output.

Therefore `Build\BatchFiles\Script\UploadScreenshotTestsToDiscord.bat` was not run.
