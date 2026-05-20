---
name: unreal-config
description: Use to read and edit Unreal project/engine config (.ini) settings, sections, and console variables via Monolith MCP. Triggers on config, ini, cvar, console variable, project settings, DefaultEngine, DefaultGame, DefaultInput, setting, set config, get config, config section.
---

# unreal-config

**10 actions** via `config_query(action, params)`. Action names below are the live registry surface; call `monolith_discover` for exact parameter schemas.

## Discovery

```
monolith_discover({ namespace: "config" })                      # all actions in this namespace
monolith_discover({ namespace: "config", action: "<action>", mode: "schema" })  # exact params
```

## Action Reference

### Core (10)

| Action | Purpose |
|--------|---------|
| `diff_from_default` | Show project config overrides vs engine defaults for a category |
| `explain_setting` | Show where a config value comes from across Base->Default->User layers |
| `find_cvars` | Find console variables by prefix or substring. Read-only. |
| `get_config_files` | List all config files with their hierarchy level |
| `get_cvar` | Get one console variable value and flags. Read-only. |
| `get_plugin` | Get descriptor metadata for one discovered plugin. Read-only. |
| `get_section` | Read an entire config section from a specific file |
| `list_plugins` | List discovered plugins with enabled state and descriptor metadata. Read-only. |
| `resolve_setting` | Get effective value of a config key across the full INI hierarchy |
| `search_config` | Full-text search across all config files |

## Notes

- This reference is generated from the live `RegisterAction` surface. If an action is missing or renamed, re-run `monolith_discover({ namespace: "config" })` — the catalog is the source of truth.
- Pass `mode: "schema"` to `monolith_discover` for required/optional params and types before calling an action.
