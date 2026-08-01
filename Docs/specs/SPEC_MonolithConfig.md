# Monolith — MonolithConfig Module

**Parent:** [SPEC_CORE.md](../SPEC_CORE.md)
**Engine:** Unreal Engine 5.7+
**Version:** 0.22.0 (Beta)

---

## MonolithConfig

**Dependencies:** Core, CoreUObject, Engine, MonolithCore, UnrealEd, AssetTools, Json, JsonUtilities

### Classes

| Class | Responsibility |
|-------|---------------|
| `FMonolithConfigModule` | Registers 8 `config` actions and 10 `localization` actions; unregisters both namespaces on shutdown |
| `FMonolithConfigActions` | Static handlers. Helpers: ResolveConfigFilePath, GetConfigHierarchy (5 layers: Base -> Default -> Project -> User -> Saved). Uses GConfig API for reliable resolution |
| `FMonolithCVarActions` | Read-only live `IConsoleManager` CVar lookup and bounded deterministic search |
| `FMonolithLocalizationActions` | Culture discovery plus strict, guarded StringTable inspect/validate/create/edit/CSV handlers |

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

### Actions (10 — namespace: "localization")

| Action | Kind | Contract |
|--------|------|----------|
| `list_cultures` | Read | Resolve requested culture names, optionally including derived cultures; with no list, return configured/default culture context |
| `list_string_tables` | Read | Scan a `/Game` content path, optionally returning metadata and entries under one aggregate output budget |
| `get_string_table` | Read | Load one `/Game` StringTable and return at most `limit` entries |
| `validate_string_table` | Read | Report empty keys/source strings, duplicate-looking keys, and oversized output concerns with bounded issue rows and complete totals |
| `create_string_table` | Write | Create a new `/Game` StringTable; namespace defaults to the asset name and an explicit object name must match the package leaf |
| `set_string_entry` | Write | Add or replace one entry and optional string metadata with unambiguous metadata keys |
| `remove_string_entry` | Write | Remove one existing entry by key |
| `set_string_metadata` | Write | Add, replace, or remove one metadata field on an existing entry |
| `import_string_table_csv` | Write | Import project-scoped CSV rows, optionally replacing existing entries while preserving lossless metadata-presence information |
| `export_string_table_csv` | External write | Export a StringTable to a project-scoped CSV path with structural metadata-presence information |

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
| Catalog | Generated catalog contains exactly 10 `localization` actions |
| Compile | `MonolithConfig` links in both UE 5.7 and UE 5.8 |
| Focused automation | `Monolith.ParamGuard.MonolithConfig.Localization` passes all focused tests with zero test warnings/errors on both engines |
| Lifecycle | Create two in-memory tables; prove the shared list-entry budget and 200-row validation cap; round-trip present-empty versus absent metadata through CSV; reject reserved/whitespace metadata and mismatched object names; preserve UE 5.8 notes; then remove, inspect, and clean up |
| Side effects | Focused tests leave no `.uasset` and no CSV under their test paths |
| Visual/Discord | N/A: the action surface has no visual or presentation behavior |

---
