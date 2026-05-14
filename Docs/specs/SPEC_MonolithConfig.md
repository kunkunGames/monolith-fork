# Monolith — MonolithConfig Module

**Parent:** [SPEC_CORE.md](../SPEC_CORE.md)
**Engine:** Unreal Engine 5.7+
**Version:** 0.14.9 (Beta)

---

## MonolithConfig

**Dependencies:** Core, CoreUObject, Engine, MonolithCore, UnrealEd, Json, JsonUtilities

### Classes

| Class | Responsibility |
|-------|---------------|
| `FMonolithConfigModule` | Registers 10 config actions and 4 localization actions |
| `FMonolithConfigActions` | Static handlers. Helpers: ResolveConfigFilePath, GetConfigHierarchy (5 layers: Base -> Default -> Project -> User -> Saved). Uses GConfig API for reliable resolution |
| `FMonolithLocalizationActions` | Static handlers for localization and StringTable operations. |

### Actions (10 — namespace: "config")

| Action | Description |
|--------|-------------|
| `resolve_setting` | Get effective value via `GConfig->GetString`. Params: `file` (category), `section`, `key` |
| `explain_setting` | Show where value comes from across Base->Default->User layers. Auto-searches Engine/Game/Input/Editor if only `setting` given |
| `diff_from_default` | Compare config layers using GConfig API. Supports 5 INI layers (Base, Default, Project, User, Saved). Reports modified + added. Optional `section` filter |
| `search_config` | Full-text search across all config files. Max 100 results. Optional `file` filter |
| `get_section` | Read entire config section from a file |
| `get_config_files` | List all .ini files with hierarchy level and sizes. Optional `category` filter |
| `list_plugins` | List discovered plugins with enabled state and descriptor metadata. Read-only. |
| `get_plugin` | Get descriptor metadata for one discovered plugin. Read-only. |
| `get_cvar` | Get one console variable value and flags. Read-only. |
| `find_cvars` | Find console variables by prefix or substring. Read-only. |

### Actions (4 — namespace: "localization")

| Action | Description |
|--------|-------------|
| `list_cultures` | List available cultures known to Unreal internationalization. |
| `list_string_tables` | List StringTable assets under a project content path. |
| `get_string_table` | Inspect a StringTable asset and return capped entries. |
| `validate_string_table` | Validate a StringTable asset for empty keys, empty strings, duplicate-looking keys, and large output warnings. |

---
