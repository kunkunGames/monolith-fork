# Localization Action Port Verification

| | |
|---|---|
| Date | 2026-07-28; review hardening reverified 2026-07-31 |
| Branch | `agent/localization-string-tables` |
| Base | `kunkunGames/monolith-fork@ee1dae25f9a90a45ae768abbfcb0d9356810b0c4` |
| Verified source commit | `70de45ec4646ee5f6f775abbc48518ca900004ff` |
| Scope | Ten guarded `localization` actions plus review hardening for exact complex types, aggregate output bounds, object/package identity, lossless metadata/CSV fidelity, spreadsheet-formula safety, atomic save-failure rollback, numeric bounds, and physical path containment |
| Excluded | Security, benchmark, invocation-log/metadata features, reinforcement learning |

---

## 1. Verification frame

| Item | Contract |
|------|----------|
| Goal | Port the standalone localization action surface without depending on other pending upstream feature PRs |
| Engine matrix | Compile and run the same source commit on UE 5.7 and UE 5.8 |
| Mutation safety | Require `dry_run=true` or `confirm=true`; keep package save opt-in; reject outside-project file paths |
| Type safety | Reject scalar coercion, fractional integer limits, null optionals, malformed arrays/metadata, metadata identity ambiguity, and JSON-string recovery for schema-marked exact complex parameters before mutation |
| File safety | Require lexical project containment and reject every symlink/junction component below the project directory |
| Output safety | Bound `list_string_tables` entries across the full returned table set and cap validation issue rows without losing total counts |
| Done criteria | 10 catalog actions, two successful editor links, 6/6 focused tests per engine, live UE 5.8 MCP contract proof, zero unexpected accepted-run warnings/errors, zero residual test assets/CSV files |

The host projects are disposable verification fixtures outside the Monolith checkout. Each host's `Plugins\Monolith` junction resolves to the exact source commit listed above.

---

## 2. Build

The engine root was resolved from each host `.uproject` `EngineAssociation` using the Speed resolver, then the matching engine's `Build.bat` built `UnrealEditor` with `MONOLITH_RELEASE_BUILD=1`.

```powershell
$engineRoot = (& "D:\P4\speed\Build\BatchFiles\Script\ResolveUnrealEngine.ps1" `
    -Project "<host>.uproject" -Output Root) -join ""
$build = Join-Path $engineRoot "Engine\Build\BatchFiles\Build.bat"
$env:MONOLITH_RELEASE_BUILD = "1"
& $build UnrealEditor Win64 Development "-Project=<host>.uproject" -WaitMutex -NoHotReloadFromIDE
```

| Engine | Host project | Result | Linked DLL | SHA256 |
|--------|--------------|--------|------------|--------|
| UE 5.7 | `D:\P4\MonolithLocalizationFinalUE57Host\MonolithLocalizationFinalUE57Host.uproject` | PASS, clean-host full editor link, 434/434 actions (`LocalizationActionPort-FinalClean-Build-UE57-20260731-014726.log`) | 613376 bytes | `248C08E174536AEAA78A24C36B656BE01F1656CF4AB1F19B34A895527B7EF74A` |
| UE 5.8 | `D:\P4\MonolithLocalizationUE58Host\MonolithLocalizationUE58Host.uproject` | PASS, final localization action/test compile and editor link, 4/4 actions (`LocalizationActionPort-FinalHead-Build-UE58-20260731-014510.log`) | 589824 bytes | `735EB7281966FEF1DB1FFD56A2DAC82B109EE45F12E672EB9C2CAFF2ED5D2524` |

The disposable hosts contain no project `Source/*Target.cs`, so the accepted builds use the shared `UnrealEditor` target. An earlier UE 5.7 attempt with the nonexistent `MonolithLocalizationUE57HostEditor` target failed in Rules assembly before compiling Monolith and is excluded from the evidence set (`LocalizationActionPort-Review2-Build-UE57-20260730-022624.out.log`). Each accepted UBT invocation used a unique `-Log=...` path to avoid the process-global default `Log.txt` lock.

---

## 3. Focused automation

```powershell
& "<engine>\Engine\Binaries\Win64\UnrealEditor-Cmd.exe" "<host>.uproject" `
    -unattended -nop4 -nosplash -NullRHI `
    '-ExecCmds=Automation RunTests Monolith.ParamGuard.MonolithConfig.Localization; Quit' `
    '-ini:Monolith:[/Script/MonolithCore.MonolithSettings]:bMcpServerEnabled=False' `
    "-ReportExportPath=<report-directory>" "-abslog=<log-path>"
```

| Engine | Report | Succeeded | With warnings | Failed | Not run | Test warnings | Test errors |
|--------|--------|-----------|---------------|--------|---------|---------------|-------------|
| UE 5.7 | `D:\P4\MonolithLocalizationFinalUE57Host\Saved\Automation\LocalizationActionPort-FinalClean-UE57-20260731-015014\index.json` | 6 | 0 | 0 | 0 | 0 | 0 |
| UE 5.8 | `D:\P4\MonolithLocalizationUE58Host\Saved\Automation\LocalizationActionPort-FinalHead-UE58-20260731-014601\index.json` | 6 | 0 | 0 | 0 | 0 | 0 |

Accepted automation logs are `D:\P4\MonolithLocalizationFinalUE57Host\Saved\Logs\LocalizationActionPort-FinalClean-Automation-UE57-20260731-015014.log` and `D:\P4\MonolithLocalizationUE58Host\Saved\Logs\LocalizationActionPort-FinalHead-Automation-UE58-20260731-014601.log`. The command-line INI override keeps the automation-only hosts from binding the default MCP port `9316`, which belongs to the active Speed editor. The save-failure case acquires the package file exclusively, declares the resulting engine diagnostics as expected, and proves that `SAVE_NoError` returns a structured action error rather than terminating the commandlet.

| Test | Verified contract |
|------|-------------------|
| `LocalizationStringTableActionsRegister` | All 10 actions register under `localization` |
| `LocalizationStringTableWriteGate` | Mutation without `dry_run=true` or `confirm=true` fails before creation |
| `LocalizationStringTableCreateDryRun` | Dry-run reports intent and creates no asset |
| `LocalizationStringTableRejectsMalformedParams` | Missing/malformed handler inputs fail explicitly |
| `LocalizationStrictJsonTypes` | Wrong JSON types, numeric strings, fractional integers, nulls, encoded complex strings, huge integral limits, mismatched object/package names, and whitespace metadata keys are rejected without coercion or overflow |
| `LocalizationStringTableLifecycle` | Two in-memory tables prove aggregate list-entry bounds and 205→200+5 issue truncation; present-empty versus absent metadata and formula-looking/literal-apostrophe values survive CSV export/replace-import; an exclusive package handle proves save failure returns explicitly and restores rows, metadata, notes, and the original dirty flag; reserved/whitespace metadata is rejected; cleanup succeeds |

---

## 4. Compatibility defects resolved

| Failure | Root cause | Resolution | Proof |
|---------|------------|------------|-------|
| UE 5.7 compile rejected newer JSON helper usage | The source branch used helper APIs not available across the full UE 5.7/5.8 matrix | Read `FJsonValue::Type` and exact typed values directly; no scalar coercion | Both engine builds plus `LocalizationStrictJsonTypes` |
| StringTable entry access compiled against an incomplete type | Entry operations require the concrete StringTable core declarations | Include `Internationalization/StringTableCore.h` wherever entry access is compiled | Both engine builds |
| `SetSourceString` arity differed | UE 5.7 exposes a two-argument call; UE 5.8 Editor requires the development-notes argument | Added one version-gated compatibility helper; UE 5.8 preserves existing notes through `GetDevNotes()` | Both engine builds plus lifecycle test |
| Lifecycle CSV path was rejected | The test reused an engine-relative `ProjectSavedDir()` string as an external user path, violating the project-relative input contract | Pass `Saved\...` to the action and compute a separate absolute path only for test cleanup | 6/6 on both engines; no CSV residue |
| Test registration emitted replacement warnings | Each focused test registered the same namespace unconditionally | Register localization actions only when the namespace is absent | Zero test warnings on both engines |
| Encoded arrays/objects bypassed handler type checks | Registry-level complex-value recovery rewrote JSON strings before localization handlers could enforce their contract | Added per-parameter `allow_string_encoded_complex=false` with `RequiredExactType` / `OptionalExactType`; localization arrays and metadata opt out explicitly | Live encoded-array rejection plus `LocalizationStrictJsonTypes` |
| Capped output depended on StringTable iteration order | Entries were truncated before sorting | Snapshot and sort all keys before applying `limit` | Live `limit=1` returns `A_KEY` after inserting `Z_KEY` first |
| Empty metadata on an absent key was reported unchanged | Value-only comparison could not distinguish absence from a present empty value | Test metadata-key presence separately, then apply the empty value as a real mutation | Live response reports `changed=true` and returns `EmptyMarker:""` |
| Reserved metadata could corrupt the CSV identity columns | Case-insensitive metadata names such as `KEY` could collide with `key` and `source_string` | Reject reserved metadata headers on export and duplicate headers on import | Live `KEY` export is rejected; focused CSV coverage passes on both engines |
| Replace import lost UE 5.8 development notes | `replace_existing` cleared entries and recreated source strings without preserving the third StringTable field | Snapshot UE 5.8 development notes by key and restore them when replacing matching entries | UE 5.8 lifecycle assertions pass |
| Large JSON numbers could overflow before clamping | Numeric `limit` values were cast to `int32` before the action clamp | Clamp the finite integral double to `[1, 1000]` before conversion | Live `limit=1e30` returns safely without an error or overflow |
| Lexical containment allowed reparse-point escape | A path under the project text prefix could traverse a junction to an external directory | Walk each existing path component below the project root and reject symlinks/junctions | Live import and export through `Saved\MonolithTests\LocalizationLink-*` both reject the path; external target is unchanged |
| Empty CSV cells erased present-empty metadata | A plain CSV cell cannot distinguish an absent field from a present field whose value is `""` | Export and consume the validated structural `__monolith_metadata_presence_v1` per-row key list; preserve legacy non-empty-cell behavior when absent | Focused lifecycle plus live export/import show `empty_metadata_present=true` and `absent_metadata_preserved=true` |
| `list_string_tables` multiplied the requested entry cap per table | Each table summary received the full `limit` independently | Share one decreasing entry-row budget across returned table summaries and report available/returned/truncated counts | Focused two-table fixture and live 207-entry probe return only 2 entry rows |
| Validation could serialize every issue | `issues` grew with every invalid entry | Count all issues but serialize at most 200 rows; expose full/returned/truncated counts and derive `valid` from the full total | Focused and live probes report 205 total, 200 returned, 5 truncated |
| Explicit object names could disagree with package names during creation | Asset-path splitting trusted the object segment independently of the package leaf | Reject mismatched object/package names before asset creation | Focused guard and live `/Game/.../Foo.Bar` rejection |
| Metadata headers could hide identity changes in whitespace or case | Trimming silently changed CSV headers and StringTable metadata uses case-insensitive `FName` identity | Reject edge whitespace, reserved names, and normalized collisions before mutation/export/import | Focused header/set/export guards plus live whitespace rejection |
| Present-empty metadata was still missed by unchanged-import detection | `GetMetaData` returns `""` for both an absent field and a present empty field | Use presence-aware enumeration before comparing each incoming metadata value | Lifecycle removes the field after export, then proves replace import reports `changed=true` and recreates it |
| Formula guard stripped legitimate leading apostrophes | A bare `'` prefix could not distinguish an emitted guard from user content such as `'=literal` | Emit the versioned `'__monolith_formula_guard_v1__:` marker, double a literal marker prefix, and decode only validated guards | Lifecycle round-trips formula-looking source/metadata cells and literal apostrophe content exactly |
| Save-failure rollback left an initially clean package dirty and could terminate a commandlet | Rows and notes were restored after `UPackage::SavePackage` failed, but the pre-mutation dirty flag was not; default save flags could route an ordinary file-access error through the fatal output device | Snapshot `UPackage::IsDirty()` before `Modify`, restore it after rebuilding the rollback snapshot, and set `SAVE_NoError` so the action can return its structured error | Lifecycle exclusively locks the target package file, verifies explicit non-fatal save failure, exact row restoration, and `IsDirty()==false` on UE 5.7 and UE 5.8 |

---

## 5. Live UE 5.8 MCP contract

The disposable UE 5.8 host listened on `127.0.0.1:9433`; the active Speed editor on the default `9316` endpoint was left untouched. The accepted log is `D:\P4\MonolithLocalizationUE58Host\Saved\Logs\LocalizationActionPort-Review2-LiveMCP-UE58-20260730-023539.log`. Exact schemas for every called localization action and `editor.run_console_command` were read before execution.

| Probe | Result |
|-------|--------|
| Health | HTTP 200, Monolith `0.21.3`, 1275 registered internal tools |
| Discovery | `monolith_discover(namespace="localization", detail=true)` returned all 10 actions |
| Exact schema | `describe_query(action="action_schema", ...)` was read for create, set entry, set metadata, list, validate, export, import, get, and shutdown before each action family |
| Object/package identity | `/Game/Tests/Monolith/Localization/LiveReview2_20260730023811/Foo.Bar` was rejected because object `Bar` does not match package leaf `Foo` |
| Aggregate output | Two returned tables contained 207 available entries; `entry_budget=2`, `returned_entry_count=2`, and `truncated_entry_count=205` |
| Validation cap | `issue_count=205`, `returned_issue_count=200`, `truncated_issue_count=5` |
| Metadata identity | A metadata key with edge whitespace was rejected explicitly |
| Lossless CSV | Export header was `key,source_string,__monolith_metadata_presence_v1,EmptyAllowed,Owner`; replace import retained the present empty value while leaving the same field absent on the other row |
| Cleanup | CSV deleted; residual `*LiveReview2*` `.uasset` files: 0 |
| Shutdown | `editor.run_console_command("QUIT_EDITOR")` stopped PID 30228 and released port 9433 |

---

## 6. Catalog and side-effect audit

The catalog snapshot generator reported 1571 actions, compared with 1561 on the fork base. Exactly 10 entries use the `localization` namespace:

`create_string_table`, `export_string_table_csv`, `get_string_table`, `import_string_table_csv`, `list_cultures`, `list_string_tables`, `remove_string_entry`, `set_string_entry`, `set_string_metadata`, `validate_string_table`.

| Audit | UE 5.7 | UE 5.8 |
|-------|--------|--------|
| Residual localization test `.uasset` files | 0 | 0 |
| Residual `Saved\MonolithTests\Localization` CSV files | 0 | 0 |
| Screenshot verification | N/A | N/A |
| Discord screenshot upload | N/A — no visual, gameplay, UI, or presentation behavior changed | N/A — no visual, gameplay, UI, or presentation behavior changed |

No screenshots were captured and `Build\BatchFiles\Script\UploadScreenshotTestsToDiscord.bat` was not invoked because this is a headless data/action-surface change.
