# Localization Action Port Verification

| | |
|---|---|
| Date | 2026-07-28; review hardening reverified 2026-07-30 |
| Branch | `agent/localization-string-tables` |
| Base | `kunkunGames/monolith-fork@ee1dae25f9a90a45ae768abbfcb0d9356810b0c4` |
| Verified source commit | `b1d244e986dbfc72b0d4244ccfbb5b8e3053d988` |
| Scope | Ten guarded `localization` actions plus review hardening for exact complex types, deterministic caps, metadata/CSV fidelity, numeric bounds, and physical path containment |
| Excluded | Security, benchmark, invocation-log/metadata features, reinforcement learning |

---

## 1. Verification frame

| Item | Contract |
|------|----------|
| Goal | Port the standalone localization action surface without depending on other pending upstream feature PRs |
| Engine matrix | Compile and run the same source commit on UE 5.7 and UE 5.8 |
| Mutation safety | Require `dry_run=true` or `confirm=true`; keep package save opt-in; reject outside-project file paths |
| Type safety | Reject scalar coercion, fractional integer limits, null optionals, malformed arrays/metadata, and JSON-string recovery for schema-marked exact complex parameters before mutation |
| File safety | Require lexical project containment and reject every symlink/junction component below the project directory |
| Done criteria | 10 catalog actions, two successful editor links, 6/6 focused tests per engine, live UE 5.8 MCP contract proof, zero test warnings/errors, zero residual test assets/CSV files |

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
| UE 5.7 | `D:\P4\MonolithLocalizationUE57Host\MonolithLocalizationUE57Host.uproject` | PASS, 185 actions (`LocalizationActionPort-Review-Build-UE57-Accepted-20260730-000929.log`) | 481792 bytes | `06E070D0BA0818CCF2E348B56DE1D5C9DE5604BAD57D985BA56223E445354B5F` |
| UE 5.8 | `D:\P4\MonolithLocalizationUE58Host\MonolithLocalizationUE58Host.uproject` | PASS, 185 actions (`LocalizationActionPort-Review-Build-UE58-20260730-001232.out.log`) | 458240 bytes | `5F2DEEFDE94737D394611A3145F55F6045B6258EB73EE00DC9D4DCFB003FBA8B` |

---

## 3. Focused automation

```powershell
& "<engine>\Engine\Binaries\Win64\UnrealEditor-Cmd.exe" "<host>.uproject" `
    -unattended -nop4 -nosplash -NullRHI `
    '-ExecCmds=Automation RunTests Monolith.ParamGuard.MonolithConfig.Localization; Quit' `
    "-ReportExportPath=<report-directory>" "-abslog=<log-path>"
```

| Engine | Report | Succeeded | With warnings | Failed | Not run | Test warnings | Test errors |
|--------|--------|-----------|---------------|--------|---------|---------------|-------------|
| UE 5.7 | `D:\P4\MonolithLocalizationUE57Host\Saved\Automation\LocalizationActionPort-Review-UE57-20260730-001129\index.json` | 6 | 0 | 0 | 0 | 0 | 0 |
| UE 5.8 | `D:\P4\MonolithLocalizationUE58Host\Saved\Automation\LocalizationActionPort-Review-UE58-20260730-001423\index.json` | 6 | 0 | 0 | 0 | 0 | 0 |

| Test | Verified contract |
|------|-------------------|
| `LocalizationStringTableActionsRegister` | All 10 actions register under `localization` |
| `LocalizationStringTableWriteGate` | Mutation without `dry_run=true` or `confirm=true` fails before creation |
| `LocalizationStringTableCreateDryRun` | Dry-run reports intent and creates no asset |
| `LocalizationStringTableRejectsMalformedParams` | Missing/malformed handler inputs fail explicitly |
| `LocalizationStrictJsonTypes` | Wrong JSON types, numeric strings, fractional integers, nulls, encoded complex strings, huge integral limits, and malformed metadata/arrays are handled without coercion or overflow |
| `LocalizationStringTableLifecycle` | In-memory create → set → empty metadata → deterministic capped inspect → guarded export → replace import with dev-note preservation → cleanup succeeds |

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

---

## 5. Live UE 5.8 MCP contract

The disposable UE 5.8 host listened on `127.0.0.1:9433`; the active Speed editor on the default `9316` endpoint was left untouched. The accepted log is `D:\P4\MonolithLocalizationUE58Host\Saved\Logs\LocalizationActionPort-LiveMCP-UE58-20260730-0020.log`.

| Probe | Result |
|-------|--------|
| Health | HTTP 200, Monolith `0.21.3`, 1275 registered internal tools |
| Discovery | `monolith_discover(namespace="localization", detail=true)` returned all 10 actions |
| Exact schema | `describe_query(action="action_schema", ...)` returned `metadata.type=object` and `allow_string_encoded_complex=false` |
| Encoded complex type | `culture_names="[\"en\"]"` failed with `culture_names must be an array` |
| Numeric bound | `limit=1e30` completed safely after pre-conversion clamping |
| Metadata and ordering | Setting an absent empty metadata value reported `changed=true`; capped inspection returned `A_KEY` before `Z_KEY` |
| CSV guard | Case-insensitive reserved metadata header `KEY` blocked export; normal export and replace import accepted two rows |
| Physical containment | Import and export through a project-local junction to an external directory were both rejected; no external output was created |
| Shutdown | `editor.run_console_command("QUIT_EDITOR")` stopped PID 60812 and released port 9433 |

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
