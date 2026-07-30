# Monolith — MonolithConfig Module

**Parent:** [SPEC_CORE.md](../SPEC_CORE.md)
**Engine:** Unreal Engine 5.7+
**Version:** 0.21.3 (Beta)

---

## MonolithConfig

**Dependencies:** Core, CoreUObject, Engine, MonolithCore, UnrealEd, AssetTools, Json, JsonUtilities, Projects, DeveloperSettings; Localization public headers and runtime-loaded `ILocalizationModule` interface only

### Classes

| Class | Responsibility |
|-------|---------------|
| `FMonolithConfigModule` | Registers 11 config actions and 12 localization actions |
| `FMonolithConfigActions` | Static handlers. Helpers: ResolveConfigFilePath, GetConfigHierarchy (5 layers: Base -> Default -> Project -> User -> Saved). Uses GConfig API for reliable resolution |
| `FMonolithCVarActions` | Read-only live `IConsoleManager` CVar lookup and bounded deterministic search |
| `FMonolithLocalizationActions` | Culture discovery; strict guarded StringTable inspect/validate/create/edit/CSV handlers; canonical Localization Dashboard target configuration; and a guarded asynchronous localization-target gather/compile pipeline |

### Actions (8 — namespace: "config")

| Action | Description |
|--------|-------------|
| `resolve_setting` | Get effective value via `GConfig->GetString`. Params: `file` (category), `section`, `key` |
| `explain_setting` | Show where value comes from across Base->Default->User layers. Auto-searches Engine/Game/Input/Editor if only `setting` given |
| `diff_from_default` | Compare config layers using GConfig API. Supports 5 INI layers (Base, Default, Project, User, Saved). Reports modified + added. Optional `section` filter |
| `search_config` | Full-text search across all config files. Max 100 results. Optional `file` filter |
| `get_section` | Read entire config section from a file |
| `get_config_files` | List all .ini files with hierarchy level and sizes. Optional `category` filter |
| `get_cvar` | Get one live console variable by exact name, including value, help, flags, read-only/cheat state, and set-by source |
| `find_cvars` | Find live console variables by `prefix` or `contains`; validates a strict mode enum, sorts before limiting, and caps output at 200 rows |

### Actions (12 — namespace: "localization")

| Action | Description |
|--------|-------------|
| `list_cultures` | List available cultures known to Unreal internationalization. |
| `list_string_tables` | List StringTable assets under a project content path. |
| `get_string_table` | Inspect a StringTable asset and return capped entries. |
| `set_target_text_search_directories` | Set one project target's Gather Text From Source directories through the runtime-loaded Localization model, persist `DefaultEditor.ini`, and patch only the matching `GatherTextFromSource` directory rows in the existing gather config. Both modes require `source_control_policy=require_checked_out` plus a positive exact `target_changelist`; `dry_run=true` ForceUpdates provider state and reports structured readiness/blockers without mutation, while `confirm=true` requires every write file to remain a current, non-conflicted existing edit owned by this client in that numbered changelist. |
| `run_target_pipeline` | Preflight (`dry_run=true`) or asynchronously run (`confirm=true`) the canonical `Config/Localization/<Target>_{Gather,Compile}.ini` pipeline. Returns a pollable job, per-step logs, and generated-artifact change audit. |
| `validate_string_table` | Validate a StringTable asset for empty keys, empty strings, duplicate-looking keys, and large output warnings. |
| `create_string_table` | Create a `UStringTable` asset under `/Game`. Requires `dry_run=true` or `confirm=true`; optional `namespace`, `save`. |
| `set_string_entry` | Add or replace one StringTable entry and optional metadata. Requires `asset_path`, `key`, `source_string`, and `dry_run=true` or `confirm=true`. |
| `remove_string_entry` | Remove one StringTable entry by key. Requires `asset_path`, `key`, and `dry_run=true` or `confirm=true`. |
| `set_string_metadata` | Add, replace, or remove metadata on one entry. Requires `asset_path`, `key`, `metadata_key`, and `dry_run=true` or `confirm=true`. |
| `import_string_table_csv` | Import CSV rows into a StringTable from an in-project file path. Requires `dry_run=true` or `confirm=true`; supports `replace_existing`, but refuses `replace_existing=true` when the CSV has zero accepted rows. |
| `export_string_table_csv` | Export StringTable rows to CSV under the project directory. Requires `dry_run=true` or `confirm=true`. |

### Localization contracts

| Concern | Required behavior |
|---------|-------------------|
| Asset boundary | Every StringTable path resolves under `/Game`; an explicit object name must match the package leaf; invalid, missing, or non-StringTable assets fail explicitly |
| External-file boundary | CSV paths resolve beneath the current project directory; traversal, outside-project absolute paths, and symlink/junction components below the project root are rejected. The path must also carry the `.csv` extension, so a confirmed export cannot overwrite `Config/DefaultEngine.ini`, the `.uproject`, or any other project file |
| Write authorization | Every mutating call must set `dry_run=true` or `confirm=true`; dry-run reports intent without changing assets or files |
| Persistence | Package mutation and package saving are separate. `save` defaults to `false`; callers opt in explicitly |
| JSON typing | Supplied booleans, numbers, arrays, objects, and strings must have the declared `EJson` type. `culture_names` and `metadata` opt out of registry complex-string recovery, so JSON-encoded strings, numeric strings, fractional integers, null optionals, and malformed members are rejected instead of coerced |
| Error timing | Handler-level malformed parameters return JSON-RPC `-32602` before asset load, package mutation, or file writes |
| Output bounds | Table/entry limits are integral and clamped as doubles to `1..1000` before integer conversion. Entry rows are sorted before truncation; `list_string_tables` shares one entry budget across returned tables and reports available/returned/truncated entry counts. Validation returns at most 200 issue rows while retaining full totals |
| Deterministic bounds | `validate_string_table` snapshots and sorts entries by key before evaluating issues, so the 200-row cap selects the same issues across loads and map rehashes. `list_string_tables` with `include_entries=false` counts entries directly instead of snapshotting and sorting a table whose rows are then discarded |
| Metadata identity | Metadata keys must be non-empty, contain no leading/trailing whitespace, avoid reserved structural names, and remain unique under case-insensitive `FName` identity |
| CSV integrity | Import rejects duplicate, unnamed, and edge-whitespace headers, and rejects repeated entry keys rather than silently merging them. Export coalesces metadata columns by `FName` identity, matching the importer, so an export always round-trips. Cells beginning with `=`, `+`, `-`, `@`, tab, or CR receive the versioned `'__monolith_formula_guard_v1__:` marker so spreadsheet applications do not evaluate localization content as a formula. Existing marker prefixes are doubled, and import removes only a validated versioned marker, preserving legitimate values such as `'=literal`. When metadata exists, export emits `__monolith_metadata_presence_v1`; import validates its per-row key list and uses presence-aware comparison so a present empty string remains distinct from an absent field. Legacy CSV without this column retains non-empty-cell semantics |
| Import atomicity | `import_string_table_csv` compares the parsed rows against the existing entries and reports `changed=false` without calling `Modify` when nothing differs, so re-importing an unchanged export causes no source-control churn. When a change is applied and `save=true` fails, the previous entries, metadata, UE 5.8 developer notes, and the package's original dirty flag are restored, so a failed import is a genuine no-op instead of a dirty rebuilt table |
| Registry collisions | `create_string_table` treats a live `FStringTableRegistry` entry under the target asset id as a collision and returns `-32602`. It never unregisters a table that was not proven stale, which previously destroyed the registration permanently when factory creation then failed |
| Engine compatibility | UE 5.7 uses the two-argument `FStringTable::SetSourceString`; UE 5.8 Editor uses the three-argument overload while preserving developer notes through ordinary updates and replace imports |

### Data flow

| Stage | Owner | Result |
|-------|-------|--------|
| Registration | `FMonolithConfigModule` | Registers `config` and `localization` handlers in the central registry and reports live namespace counts |
| Parse and validate | `FMonolithLocalizationActions` | Verifies exact JSON types, object/package identity, link-safe project/content boundaries, metadata identity, CSV structure, limits, and write authorization |
| Resolve | `FMonolithAssetUtils`, Asset Registry, `UStringTable` | Normalizes `/Game` paths and rejects missing or wrong-class assets |
| Mutate | `FStringTable`, `UStringTable` package | Applies entry/metadata changes only after every guard has passed |
| Persist/export | `UPackage::SavePackage`, `FFileHelper` | Saves only when requested and writes CSV only beneath the project directory |
| Verify | Focused automation | Exercises registration, write gates, dry-run, malformed input, strict typing, and full in-memory CSV lifecycle without leaving artifacts |

### Verification gates

| Gate | Requirement |
|------|-------------|
| Write gate | Every mutating localization action must reject calls unless `dry_run=true` or `confirm=true` is supplied. |
| Asset scope | StringTable asset paths must resolve under `/Game`; filesystem CSV paths must remain under the project directory unless the action explicitly documents a broader scope. |
| Return shape | Mutating actions return `dry_run`, `changed`, `saved`, `asset_path`, and a StringTable summary or row-level result object where applicable. |
| Package handling | Successful non-dry-run writes call `Modify()`, mark the package dirty, and save only when `save=true` is supplied. |
| CSV shape | CSV import/export uses `key,source_string,<metadata...>` headers; unknown metadata headers are preserved as per-entry metadata keys. |
| Target settings model | `set_target_text_search_directories` loads `ILocalizationModule` at runtime, resolves only a persisted project target, rejects a stale live model, mutates `FGatherTextFromTextFilesConfiguration::SearchDirectories`, and persists `DefaultEditor.ini` through the target's editor change notification. It does not statically link the optional engine Localization import library. |
| Target source paths | Each requested source directory must use an explicit `%LOCENGINEROOT%` or `%LOCPROJECTROOT%` prefix, resolve to an existing non-root directory, contain no wildcard or parent traversal, and remain unique after slash/case normalization. |
| Target gather config | The action requires one existing `<Target>_Gather.ini`, exact `Content/Localization/<Target>` source/destination scope, unique case-insensitive `GatherTextStepN` section names, and exactly one canonical `GatherTextFromSource` step with contiguous directory rows immediately after `CommandletClass`. It replaces only those rows, preserves all other text and line terminators character-for-character, and rejects missing, malformed, mixed-line-ending, ambiguous, duplicate-section, or stale input. Other generated configs cannot change because this property has no semantic input into them. |
| Target settings atomicity | Dry-run never mutates the dashboard model or generated files. Confirm refuses missing or read-only settings/gather files and any exact-numbered-changelist blocker. After acquiring the single-mutation flag, it performs a fresh pre-write ForceUpdate and returns before snapshots, `PostEditChange`, or file writes if ownership changed. Only then does it snapshot the exact affected bytes, and it restores both the in-memory directories and file bytes if model persistence, targeted gather-config write, exact readback, or post-write source-control ownership revalidation fails. |
| Installed-engine boundary | `MonolithConfig` uses `PrivateIncludePathModuleNames` plus `DynamicallyLoadedModuleNames` for Localization. Persisted `FLocalizationTargetSettings` text is initialized and destroyed through `FStructOnScope` only after the Localization module is loaded; `MonolithConfig` copies only the target name and Gather Text From Source directories into its own snapshot and never directly default-constructs the engine struct. Installed engines that ship `UnrealEditor-Localization.dll` without `UnrealEditor-Localization.lib` therefore remain linkable; a locally generated engine import library must not be required or used as acceptance evidence. |
| Target identity | `run_target_pipeline.target` accepts only 1–64 ASCII letters, digits, underscores, or hyphens. The action resolves only the matching project-local `<Target>_Gather.ini` and `<Target>_Compile.ini` files. |
| Target config scope | Every selected config must keep both `SourcePath` and `DestinationPath` at `Content/Localization/<Target>`. Compile must begin with `GenerateTextLocalizationResource`. |
| Target operations | `operations` is an ordered, duplicate-free subset of `gather` and `compile`; when both are requested, gather must precede compile. |
| Source-control ownership | `set_target_text_search_directories` owns its pre-mutation source-control gate and never auto-checks out: it requires `source_control_policy=require_checked_out`, a positive exact `target_changelist`, enabled/available checkout-and-changelist provider capabilities, ForceUpdated tracked/current/non-added/non-deleted/non-ignored/non-conflicted state, no other-user checkout, current-client checkout, and a valid non-default matching `GetCheckInIdentifier()`. Dry-run returns `ready=false`, provider state, per-file `actual_changelist`, and structured blockers rather than mutating or masking an unprepared provider/file. Confirm performs the initial plan audit, repeats the same ForceUpdate after acquiring its mutation flag but before any snapshot or write, and ForceUpdates again after write before success. Separately, the spawned GatherText commandlet always runs without `-EnableSCC` and with `-nop4`; confirm mode refuses to launch while an existing selected generated output is read-only. |
| Process ownership | One localization target job may run at a time. Target settings cannot change while that job is active. Cancellation and timeout terminate only the child process created by that job; the action never searches for or terminates unrelated editor or commandlet processes. |
| Module lifetime | `MonolithConfig` shutdown closes the launch gate, requests cancellation for its active localization job, and joins the module-owned worker before unregistering actions or unloading code. |
| Async result | Confirm mode returns `job_id`, `poll_action=monolith.get_job`, and `cancel_action=monolith.cancel_job`. Completion reports per-operation exit/log details plus created, updated, and deleted artifact paths under `Content/Localization/<Target>`. |
| Catalog | Generated catalog contains exactly 10 `localization` actions |
| Compile | `MonolithConfig` links in both UE 5.7 and UE 5.8 |
| Focused automation | `Monolith.ParamGuard.MonolithConfig.Localization` passes all focused tests with zero test warnings/errors on both engines |
| Lifecycle | Create two in-memory tables; prove the shared list-entry budget and 200-row validation cap; round-trip present-empty versus absent metadata through CSV; reject reserved/whitespace metadata and mismatched object names; preserve UE 5.8 notes; then remove, inspect, and clean up |
| Side effects | Focused tests leave no `.uasset` and no CSV under their test paths |
| Visual/Discord | N/A: the action surface has no visual or presentation behavior |

---
