---
name: unreal-config
description: Use to read and edit Unreal project/engine config (.ini) settings, sections, and focused console variables via Monolith MCP - resolve effective values across the INI hierarchy, diff overrides vs defaults, find/get cvars, inspect plugin descriptors. This skill owns generic config read/edit; for the full IConsoleObject registry, console commands, snapshot search, hints/help text, or command execution use unreal-console; for a perf-motivated cvar/INI change with profiling intent use unreal-performance, for Enhanced Input mappings (DefaultInput) use unreal-input, to check out a .ini in P4/Git first use unreal-source-control. Triggers on config, ini, cvar, console variable, project settings, DefaultEngine, DefaultGame, DefaultInput, setting, set config, get config, config section, edit ini, change setting, scalability settings, engine config value, GameUserSettings, config override, platform ini, r. cvar, command line cvar.
---

# unreal-config

**11 actions** via `config_query(action, params)`. Action names below are the live registry surface; call `monolith_discover` for exact parameter schemas.

## Discovery

```
monolith_discover({ namespace: "config" })                      # all actions in this namespace
monolith_discover({ namespace: "config", action: "<action>", mode: "schema" })  # exact params
```

## When to use

Use this skill for generic config read/edit — reading/searching `.ini` values, resolving effective values across the INI hierarchy, diffing overrides vs defaults, and focused `IConsoleVariable` lookup.

Use a different skill for:

- **unreal-performance** — when the cvar/INI change is performance-tuning with profiling intent (scalability, shader stats, draw-call/INI tuning).
- **unreal-console** — when you need the full `IConsoleManager` object registry, console command entries, help/hint text, snapshot-backed search, or guarded command execution.
- **unreal-input** — when editing `DefaultInput` is really Enhanced Input mappings (Input Actions, Input Mapping Contexts, key bindings).
- **unreal-source-control** — when the `.ini` edit must be checked out or marked for add in P4/Git first.

## Action Reference

Param notation: `name*` required, `name?` optional, `name=val` default, `a/b/c` allowed values, `[w]` mutates. Signatures are a snapshot of the live catalog — for the exact, full, current schema of any action call `monolith_discover` with `mode: "schema"`.

### Read (10)

| Action | Purpose | Params (req* opt? =default) |
|--------|---------|------------------------------|
| `resolve_setting` | Get the effective value of a config key. With `file` omitted, search the canonical Engine/Game/Input/Editor/EditorPerProjectUser/GameUserSettings categories and report the resolved category plus searched categories. | `file?` `section*` `key*` |
| `explain_setting` | Show where a config value comes from across Base->Default->User layers | `file?` `section?` `key?` `setting?` (setting = convenience: search key across common categories) |
| `diff_from_default` | Show project config overrides vs engine defaults for a category | `file*` `section?` |
| `search_config` | Full-text search across all config files | `query*` `category?` |
| `get_section` | Read an entire config section from a specific file | `file*` `section*` |
| `get_config_files` | List all config files with their hierarchy level | `category?` |
| `list_plugins` | List discovered plugins with enabled state and descriptor metadata. Read-only. | `name_contains?` `enabled_only=false` `limit=200` |
| `get_plugin` | Get descriptor metadata for one discovered plugin. Read-only. | `name*` |
| `get_cvar` | Get one console variable value and flags. Read-only. Use `console.get_object` for command entries or non-variable console objects. | `name*` |
| `find_cvars` | Find console variables by prefix or substring. Read-only. Use `console.search_objects` for full help-text/command/object search. | `query?` `mode=prefix` (prefix/contains) `limit=100` |

### Write (1)

| Action | Purpose | Params (req* opt? =default) |
|--------|---------|------------------------------|
| `set_developer_setting` `[w]` | DEV-ONLY: set a property on a `UDeveloperSettings` CDO at runtime. Resolves class by short-name or full path, parses `value` via `UProperty::ImportText_Direct`, optionally persists to INI. `#if WITH_EDITOR`-gated. | `class*` `property*` `value*` `save_config=false` |

`file`/`category` take a config category name (e.g. `Engine`, `Game`, `Input`); `section` is the INI section header (e.g. `/Script/Engine.RendererSettings`). `resolve_setting.file` is optional: omit it only when the category is unknown and inspect `category`, `searched_categories`, and per-category diagnostics in the response. For `set_developer_setting`, `class` is a short-name (e.g. `MonolithReflectionIntelSettings`) or full path (`/Script/Module.Class`), `value` is text (`'true'`, `'42'`, `'0.75'`, `'(X=1,Y=2)'`), and `save_config=true` writes back to the persistent INI via `UObject::SaveConfig()`.

## Common Workflows

Numbered recipes use only the actions in the table above. `file`/`category` is a category name (`Engine`/`Game`/`Input`); `section` is the INI header. Run `monolith_discover` with `mode: "schema"` for exact params before each call.

### Recipe 1 — Trace where a config value comes from

1. `config_query("search_config", { query, category })` — full-text find the key across config files when you do not yet know its `file`/`section`; the hits give you the category and section header to use below.
2. `config_query("resolve_setting", { section, key })` — when the category is still unknown, search the canonical categories and inspect the returned `category`/`searched_categories`; pass `{ file, section, key }` when the category is already known.
3. `config_query("explain_setting", { file, section, key })` — show which Base->Default->User layer actually set that value (use the `setting` convenience form to search the key across common categories when the layer is unclear).
4. `config_query("diff_from_default", { file, section })` — confirm whether the resolved value is a project override or the stock engine default, and see the other overrides in the same section.

### Recipe 2 — Change a developer setting and persist it

1. `config_query("resolve_setting", { file, section, key })` — capture the current effective value first so the change is auditable and reversible.
2. `config_query("set_developer_setting", { class, property, value, save_config: true })` `[w]` — set the property on the `UDeveloperSettings` CDO (`class` = short-name or `/Script/Module.Class`, `value` = import text such as `'true'` / `'42'` / `'(X=1,Y=2)'`); `save_config: true` writes back to the persistent INI via `SaveConfig()`. Check out the target `.ini` in P4/Git first via **unreal-source-control**.
3. `config_query("resolve_setting", { file, section, key })` — re-resolve to verify the new value won the hierarchy, and `config_query("diff_from_default", { file, section })` to confirm the override now shows against the engine default.

## Notes

- This reference is generated from the live `RegisterAction` surface. If an action is missing or renamed, re-run `monolith_discover({ namespace: "config" })` — the catalog is the source of truth.
- Pass `mode: "schema"` to `monolith_discover` for required/optional params and types before calling an action.
- All actions are read-only except `set_developer_setting` `[w]`, which wraps a transaction and mutates a settings CDO (and the INI when `save_config=true`). To check out the target `.ini` in P4/Git before a persisted change, use **unreal-source-control**.
