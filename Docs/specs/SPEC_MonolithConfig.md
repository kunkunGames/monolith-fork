# Monolith — MonolithConfig Module

**Parent:** [SPEC_CORE.md](../SPEC_CORE.md)
**Engine:** Unreal Engine 5.7+
**Version:** 0.21.3 (Beta)

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
| `list_string_tables` | Read | Scan a `/Game` content path, optionally returning capped entries and metadata |
| `get_string_table` | Read | Load one `/Game` StringTable and return at most `limit` entries |
| `validate_string_table` | Read | Report empty keys/source strings, duplicate-looking keys, and oversized output concerns |
| `create_string_table` | Write | Create a new `/Game` StringTable; namespace defaults to the asset name |
| `set_string_entry` | Write | Add or replace one entry and optional string metadata |
| `remove_string_entry` | Write | Remove one existing entry by key |
| `set_string_metadata` | Write | Add, replace, or remove one metadata field on an existing entry |
| `import_string_table_csv` | Write | Import project-scoped CSV rows, optionally replacing existing entries |
| `export_string_table_csv` | External write | Export a StringTable to a project-scoped CSV path |

### Localization contracts

| Concern | Required behavior |
|---------|-------------------|
| Asset boundary | Every StringTable path resolves under `/Game`; invalid, missing, or non-StringTable assets fail explicitly |
| External-file boundary | CSV paths resolve beneath the current project directory; traversal and outside-project absolute paths are rejected |
| Write authorization | Every mutating call must set `dry_run=true` or `confirm=true`; dry-run reports intent without changing assets or files |
| Persistence | Package mutation and package saving are separate. `save` defaults to `false`; callers opt in explicitly |
| JSON typing | Supplied booleans, numbers, arrays, objects, and strings must have the declared `EJson` type. Numeric strings, fractional integers, null optionals, and malformed array/object members are rejected instead of coerced |
| Error timing | Handler-level malformed parameters return JSON-RPC `-32602` before asset load, package mutation, or file writes |
| Output bounds | Table/entry limits are integral and clamped to `1..1000`; list results are sorted before truncation where deterministic ordering matters |
| Engine compatibility | UE 5.7 uses the two-argument `FStringTable::SetSourceString`; UE 5.8 Editor uses the three-argument overload while preserving existing developer notes through `FStringTableEntry::GetDevNotes()` |

### Data flow

| Stage | Owner | Result |
|-------|-------|--------|
| Registration | `FMonolithConfigModule` | Registers `config` and `localization` handlers in the central registry and reports live namespace counts |
| Parse and validate | `FMonolithLocalizationActions` | Verifies exact JSON types, project/content boundaries, limits, and write authorization |
| Resolve | `FMonolithAssetUtils`, Asset Registry, `UStringTable` | Normalizes `/Game` paths and rejects missing or wrong-class assets |
| Mutate | `FStringTable`, `UStringTable` package | Applies entry/metadata changes only after every guard has passed |
| Persist/export | `UPackage::SavePackage`, `FFileHelper` | Saves only when requested and writes CSV only beneath the project directory |
| Verify | Focused automation | Exercises registration, write gates, dry-run, malformed input, strict typing, and full in-memory CSV lifecycle without leaving artifacts |

### Verification gates

| Gate | Requirement |
|------|-------------|
| Catalog | Generated catalog contains exactly 10 `localization` actions |
| Compile | `MonolithConfig` links in both UE 5.7 and UE 5.8 |
| Focused automation | `Monolith.ParamGuard.MonolithConfig.Localization` passes all 6 tests with zero test warnings/errors on both engines |
| Lifecycle | Create, set, metadata, validate, export, remove, import, inspect, and cleanup execute in one deterministic in-memory test |
| Side effects | Focused tests leave no `.uasset` and no CSV under their test paths |
| Visual/Discord | N/A: the action surface has no visual or presentation behavior |

---
