# Localization Action Port Verification

| | |
|---|---|
| Date | 2026-07-28 |
| Branch | `agent/localization-string-tables` |
| Base | `kunkunGames/monolith-fork@ee1dae25f9a90a45ae768abbfcb0d9356810b0c4` |
| Verified source commit | `1eb26db4f7c8e0df598f88339ecb7facbb07a31f` |
| Scope | Ten guarded `localization` actions for culture discovery and StringTable read/validate/create/edit/CSV workflows |
| Excluded | Security, benchmark, invocation-log/metadata features, reinforcement learning |

---

## 1. Verification frame

| Item | Contract |
|------|----------|
| Goal | Port the standalone localization action surface without depending on other pending upstream feature PRs |
| Engine matrix | Compile and run the same source commit on UE 5.7 and UE 5.8 |
| Mutation safety | Require `dry_run=true` or `confirm=true`; keep package save opt-in; reject outside-project file paths |
| Type safety | Reject scalar coercion, fractional integer limits, null optionals, and malformed arrays/metadata before mutation |
| Done criteria | 10 catalog actions, two successful editor links, 6/6 focused tests per engine, zero test warnings/errors, zero residual test assets/CSV files |

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
| UE 5.7 | `D:\P4\MonolithLocalizationUE57Host\MonolithLocalizationUE57Host.uproject` | PASS (`Result: Succeeded`) | 448000 bytes | `C40AF2069740DF610DE6D71E02738A4F79E0A72B58DF051216E0BDA82F808343` |
| UE 5.8 | `D:\P4\MonolithLocalizationUE58Host\MonolithLocalizationUE58Host.uproject` | PASS (`Result: Succeeded`) | 428544 bytes | `872BB74F2D30E1AC0A29E8F1B6F397407A39A007F52749046764D89FA0EA2820` |

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
| UE 5.7 | `D:\P4\MonolithLocalizationUE57Host\Saved\Automation\LocalizationActionPort-UE57-CrossVersionFinal\index.json` | 6 | 0 | 0 | 0 | 0 | 0 |
| UE 5.8 | `D:\P4\MonolithLocalizationUE58Host\Saved\Automation\LocalizationActionPort-UE58-Final\index.json` | 6 | 0 | 0 | 0 | 0 | 0 |

| Test | Verified contract |
|------|-------------------|
| `LocalizationStringTableActionsRegister` | All 10 actions register under `localization` |
| `LocalizationStringTableWriteGate` | Mutation without `dry_run=true` or `confirm=true` fails before creation |
| `LocalizationStringTableCreateDryRun` | Dry-run reports intent and creates no asset |
| `LocalizationStringTableRejectsMalformedParams` | Missing/malformed handler inputs fail explicitly |
| `LocalizationStrictJsonTypes` | Wrong JSON types, numeric strings, fractional integers, nulls, and malformed metadata/arrays are rejected |
| `LocalizationStringTableLifecycle` | In-memory create → set → metadata → validate → export → remove → import → inspect → cleanup round trip succeeds |

---

## 4. Compatibility defects resolved

| Failure | Root cause | Resolution | Proof |
|---------|------------|------------|-------|
| UE 5.7 compile rejected newer JSON helper usage | The source branch used helper APIs not available across the full UE 5.7/5.8 matrix | Read `FJsonValue::Type` and exact typed values directly; no scalar coercion | Both engine builds plus `LocalizationStrictJsonTypes` |
| StringTable entry access compiled against an incomplete type | Entry operations require the concrete StringTable core declarations | Include `Internationalization/StringTableCore.h` wherever entry access is compiled | Both engine builds |
| `SetSourceString` arity differed | UE 5.7 exposes a two-argument call; UE 5.8 Editor requires the development-notes argument | Added one version-gated compatibility helper; UE 5.8 preserves existing notes through `GetDevNotes()` | Both engine builds plus lifecycle test |
| Lifecycle CSV path was rejected | The test reused an engine-relative `ProjectSavedDir()` string as an external user path, violating the project-relative input contract | Pass `Saved\...` to the action and compute a separate absolute path only for test cleanup | 6/6 on both engines; no CSV residue |
| Test registration emitted replacement warnings | Each focused test registered the same namespace unconditionally | Register localization actions only when the namespace is absent | Zero test warnings on both engines |

---

## 5. Catalog and side-effect audit

The catalog snapshot generator reported 1571 actions, compared with 1561 on the fork base. Exactly 10 entries use the `localization` namespace:

`create_string_table`, `export_string_table_csv`, `get_string_table`, `import_string_table_csv`, `list_cultures`, `list_string_tables`, `remove_string_entry`, `set_string_entry`, `set_string_metadata`, `validate_string_table`.

| Audit | UE 5.7 | UE 5.8 |
|-------|--------|--------|
| Residual localization test `.uasset` files | 0 | 0 |
| Residual `Saved\MonolithTests\Localization` CSV files | 0 | 0 |
| Screenshot verification | N/A | N/A |
| Discord screenshot upload | N/A — no visual, gameplay, UI, or presentation behavior changed | N/A — no visual, gameplay, UI, or presentation behavior changed |

No screenshots were captured and `Build\BatchFiles\Script\UploadScreenshotTestsToDiscord.bat` was not invoked because this is a headless data/action-surface change.
