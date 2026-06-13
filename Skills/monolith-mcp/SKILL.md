---
name: monolith-mcp
description: Use to discover and route Monolith MCP itself, and to manage and recover the server — find the right namespace/action for a task, browse the live catalog and schemas, check health/version/action-count, trigger reindex, and manage tool profiles, execution-guard audit, readiness, onboarding, and notifications. Start here when you do not know which Monolith tool to call. This skill OWNS routing/discovery/server-management; for inspecting writable shapes and running validated bulk_fill writes once you know the action, use monolith-schema. Triggers on monolith, discover, find action, what action, which tool, namespace, list namespaces, catalog, schema, reindex, status, version, health, action count, tool profile, enable namespace, disable action, execution guard, action audit, readiness, onboarding, MCP server status, monolith_find, monolith_discover, tool not found, monolith down, MCP not connected, recover MCP, reconnect MCP, headless editor, 9316.
---

# monolith-mcp (discovery + server management)

Monolith exposes Unreal Engine editor capability to agents as ~1,600 actions across ~40 namespaces. The action/namespace catalog is **runtime-discovered** — never hand-maintain a full per-action list; query it.

## When to use / Use a different skill for

- **Use this skill** to figure out *which* tool/namespace/action to call, browse the live catalog and schemas, check server health/version/action-count, trigger reindex, and recover the MCP endpoint (server down, not connected, reconnect to `9316`, headless editor wrapper).
- **Use monolith-schema** once you already know the action and need to inspect a writable target shape with `describe` or run validated dry-run `bulk_fill` writes. This skill owns routing and server-management; monolith-schema owns writable-shape inspection and strict bulk writes.
- **Use unreal-build** when the task is actually compiling/building or fixing build errors rather than discovering which tool to call.
- For a specific domain action, jump to its per-namespace sibling via the [Namespace map](#namespace-map-skill-per-namespace) below.

## Core tools (always available)

Param notation: `name*` required, `name?` optional, `name=val` default, `a/b/c` allowed values, `[w]` mutates (transaction-wrapped). Signatures below are a snapshot of the live catalog — for the exact, full, current schema of any action call `monolith_discover` with `mode: "schema"`.

| Tool | Params (req* opt? =default) | Use |
|------|------------------------------|-----|
| `monolith_find` | `query*`, `namespace?`, `limit=8` (1..50), `include_schema=false` | Find candidate namespaces/actions from a task description. **Call this first when the action is unclear.** |
| `monolith_discover` | `namespace?`, `action?`, `category?`, `mode=summary` (summary/actions/schema) | Live catalog + schemas. No arg → compact namespace summary. `action` implies `mode=schema`. |
| `monolith_status` | (none) | Health check: version, uptime, port, registered action count, module status. |
| `monolith_reindex` | `[w]` `force=false` | Re-index the project DB. Incremental (delta) by default; `force=true` for full wipe+rebuild. |
| `monolith_guide` | `section?` (onboarding/recipes/decisions/errors/skills_map/gotchas) | Cross-namespace workflow guide: recipes, X-vs-Y decision matrices, error-to-recovery maps, gotchas. Pass `section` to bound context cost; omit for the full index. |

### Standard discovery flow

```
monolith_find("place a spot light and check its coverage")        # → namespaces/actions
monolith_discover({ namespace: "scene" })                          # actions in a namespace
monolith_discover({ namespace: "scene", action: "place_light", mode: "schema" })  # exact params
```

Then call the namespace tool: `scene_query("place_light", { type: "spot", location: [...] })`.

## Namespace map (skill per namespace)

Each namespace has a dedicated skill in this folder. Invoke a namespace with `{namespace}_query(action, params)`.

| Domain | Namespaces → skills |
|--------|---------------------|
| Code / project | `source`→unreal-cpp, `project`→unreal-project-search, `bridge`→unreal-bridge, `editor`→unreal-build / unreal-debugging / unreal-performance |
| Gameplay | `ai`→unreal-ai, `gas`→unreal-gas, `blueprint`→unreal-blueprints, `logicdriver`→unreal-logicdriver, `combograph`→unreal-combograph, `input`→unreal-input, `world_conditions`→unreal-world-conditions, `gamefeatures`→unreal-gamefeatures |
| Spatial / level | `scene`→unreal-scene, `leveldesign`→unreal-leveldesign, `worldgen`→unreal-worldgen, `mesh`→unreal-mesh, `level_instance`→unreal-level-instance, `hlod`→unreal-hlod, `pcg`→unreal-pcg, `water`→unreal-water |
| Content | `material`/`asset`→unreal-materials, `niagara`→unreal-niagara, `animation`→unreal-animation, `metahuman`→unreal-metahuman, `audio`→unreal-audio, `ui`→unreal-ui, `slate`→unreal-slate, `paper2d`→unreal-paper2d, `chaos_fracture`→unreal-chaos-fracture, `cloth`→unreal-cloth, `dataflow`→unreal-dataflow, `chooser`→unreal-chooser, `interchange`→unreal-interchange, `modelgen`→unreal-modelgen, `imagegen`→unreal-imagegen, `ndisplay`→unreal-ndisplay |
| Sequencing | `level_sequence`→unreal-level-sequences |
| Project ops | `config`→unreal-config, `source_control`→unreal-source-control, `collection`→unreal-collection, `localization`→unreal-localization |

## `monolith` admin namespace

Beyond the core tools, the `monolith` namespace carries server-management actions. Invoke as `monolith_query(action, params)` (or via the namespace tool your client exposes). Same notation as above; `[w]` actions wrap a transaction. Signatures are a live-catalog snapshot — confirm with `monolith_discover({ namespace: "monolith", action: "<name>", mode: "schema" })`.

**Catalog / discovery**

| Action | Params (req* opt? =default) | Use |
|--------|------------------------------|-----|
| `discover` | `namespace?`, `action?`, `category?`, `mode=summary` (summary/actions/schema) | Same as `monolith_discover`. |
| `find` | `query*`, `namespace?`, `limit=8` (1..50), `include_schema=false` | Same as `monolith_find`. |
| `guide` | `section?` (onboarding/recipes/decisions/errors/skills_map/gotchas) | Cross-namespace workflow guide. |
| `get_effective_discovery` | `namespace?`, `category?` | Discovery output after the active tool profile is applied. |
| `get_mcp_discovery_state` | (none) | Current live-registry discovery snapshot + refresh semantics. |

**Tool profiles** (scope the action surface)

| Action | Params (req* opt? =default) | Use |
|--------|------------------------------|-----|
| `list_tool_profiles` | (none) | List local tool-surface profiles. |
| `get_tool_profile` | `profile_id*` | Get one profile definition. |
| `create_tool_profile` `[w]` | `profile_id*`, `display_name?`, `description?`, `mode=denylist` (denylist/allowlist), `custom_instructions?`, `enabled_namespaces?` (array), `enabled_actions?` (array), `disabled_namespaces?` (array), `disabled_actions?` (array), `description_overrides?` (object) | Create a profile. |
| `update_tool_profile` `[w]` | `profile_id*`, `display_name?`, `description?`, `mode=denylist`, `custom_instructions?`, `enabled_namespaces?`, `enabled_actions?`, `disabled_namespaces?`, `disabled_actions?`, `description_overrides?` | Replace a profile definition. |
| `delete_tool_profile` `[w]` | `profile_id*` | Delete a non-built-in, inactive profile. |
| `set_active_tool_profile` `[w]` | `profile_id*` | Set the active profile for discovery/execution filtering. |
| `validate_tool_profile` | `profile_id?` (defaults to active) | Validate profile ns/action ids against the registered surface. |
| `set_namespace_enabled` `[w]` | `namespace*`, `profile_id?` (defaults to active), `enabled=true` | Enable/disable one namespace in a profile. |
| `set_action_enabled` `[w]` | `action_id*` (namespace.action), `profile_id?`, `enabled=true` | Enable/disable one action in a profile. |
| `set_action_description_override` `[w]` | `action_id*` (namespace.action), `profile_id?`, `description_override?` (empty clears) | Set/clear a profile-specific action description. |
| `set_action_execution_policy` `[w]` | `action*` (e.g. `blueprint.add_node`), `policy?` (object; omit to reset to read_only) | Dev-only process-local execution-policy override (read_only/track_dirty_packages/transaction_optional/transaction_required/post_edit_validate). |

**Execution guard / audit**

| Action | Params (req* opt? =default) | Use |
|--------|------------------------------|-----|
| `get_execution_guard_status` | (none) | Central guard/audit status (duration + dirty-package deltas; no auto-rollback this milestone). |
| `list_recent_action_audit` | `limit=25` (1..100) | Recent audit rows: action, status, duration, changed-package count, rollback status. Raw payloads never logged. |
| `get_last_rollback` | (none) | Last rollback report when registry-policy rollback is available; reports unavailable otherwise. |

**Server / session**

| Action | Params (req* opt? =default) | Use |
|--------|------------------------------|-----|
| `get_mcp_server_status` | (none) | MCP transport status, CORS/header policy, protocol support, route state, request limits. |
| `list_mcp_sessions` | `limit=100` (1..1000) | Report MCP session-tracking availability (streamable HTTP mode does not persist per-client sessions). |
| `terminate_mcp_session` `[w]` | `session_id*` | Report session-termination availability without inventing session state. |
| `set_mcp_compatibility_options` `[w]` | `options?` (object; `browser_access`=loopback_only/disabled) | Set safe MCP compatibility options. |

**Readiness / onboarding**

| Action | Params (req* opt? =default) | Use |
|--------|------------------------------|-----|
| `get_readiness_status` | (none) | Read-only readiness checks: server, registry, index, optional modules, settings gates. |
| `get_readiness_help` | `component?` | Safe help text for readiness failures (no installers run). |
| `get_onboarding_state` | (none) | Local onboarding progress, skipped steps, next recommended step. |
| `set_onboarding_state` `[w]` | `action=complete` (complete/skip/reopen/reset), `step?` | Update an onboarding step. |

**Notifications**

| Action | Params (req* opt? =default) | Use |
|--------|------------------------------|-----|
| `get_notification_settings` | (none) | Local notification preferences. |
| `set_notification_settings` `[w]` | `settings*` (object of preference booleans) | Persist notification preferences. |
| `test_notification` `[w]` | `message="Monolith notification test"` | Trigger a harmless local notification test when editor toasts are enabled. |

**Maintenance**

| Action | Params (req* opt? =default) | Use |
|--------|------------------------------|-----|
| `status` | (none) | Same as `monolith_status`. |
| `reindex` `[w]` | `force=false` | Same as `monolith_reindex`. |
| `update` `[w]` | `action=check` (check/install) | Check for or install Monolith updates from GitHub Releases. |

> Action-name note (live-catalog snapshot {{2026-06-13}}): the domain-loading actions (`describe_domain`, `list_domains`, `load_domain`, `get_loaded_domains`) and the tool-call-record actions (`list_tool_call_records`, `get_tool_call_record`, `analyze_tool_call_records`) previously listed here are **not present** in the current `monolith` registry. Domain/category scoping now goes through `discover` (`category`), `get_effective_discovery`, and the tool-profile actions; invocation-record analysis lives in the on-disk JSONL logs read by the offline analyzer (see [Invocation diagnostics](#invocation-diagnostics)), not a `monolith` action. Re-confirm against `monolith_discover({ namespace: "monolith" })` before relying on any name here.

## Go checkout MCP recovery

For Go checkout work that needs editor-backed Monolith actions, use the configured MCP client connection to `http://localhost:9316/mcp` and confirm it with `monolith_status()` or the active MCP client's health check before calling editor actions.

If the endpoint is unreachable or the MCP transport fails, treat it as an editor/server availability issue and start the project wrapper from the checkout root:

```powershell
.\BatchFiles\RunHeadlessEditor.bat
```

Keep the MCP client configuration on the existing Monolith proxy command; do not point MCP config at this wrapper. The wrapper resolves `UnrealEditor.exe` from `GO.uproject`, launches the full editor with rendering disabled by default (`-NullRHI`) plus unattended args, and leaves source control enabled. Script contract: `Docs\specs\SPEC_MonolithHeadlessMcpLaunch.md`.

After launching the wrapper, wait for `localhost:9316` to listen, reconnect the existing Monolith proxy/client to `http://localhost:9316/mcp`, then re-run `monolith_status()` before using `monolith_find`, `monolith_discover`, or namespace actions. If the endpoint still cannot connect, inspect `Saved\HeadlessMcp\Logs\HeadlessEditor-*.log` plus the Monolith proxy/editor invocation logs, report the concrete blocker, and limit fallback work to read-only `Plugins\Monolith\Binaries\monolith_query.exe` source/project/bridge queries while editor-only actions remain blocked.

`Scripts\recover_mcp.ps1` runs this whole sequence deterministically — probe `/health`, launch the wrapper only when no editor-server candidate process exists (`-game`/`-server` instances are ignored), wait, and end with a `RESULT=` token plus documented exit code (contract: `Docs\specs\SPEC_MonolithAgentOpsScripts.md`):

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File Plugins\Monolith\Scripts\recover_mcp.ps1             # probe + launch + wait
powershell -NoProfile -ExecutionPolicy Bypass -File Plugins\Monolith\Scripts\recover_mcp.ps1 -ProbeOnly  # diagnose only, never launches
```

## Invocation diagnostics

When the checkout includes Monolith invocation logs, treat them as local diagnostics only. They do not replace the tool return shown to the agent, and their absence in an older checkout is not by itself a tool failure.

- Proxy calls append JSONL records under `Plugins\Monolith\Logs\<yyyyMMdd>\proxy.jsonl`.
- Offline query calls append JSONL records under `Plugins\Monolith\Logs\<yyyyMMdd>\query.jsonl`.
- Editor action dispatch appends JSONL records under `Plugins\Monolith\Logs\<yyyyMMdd>\action.jsonl` when `UMonolithSettings::bEnableDailyLog=true`; the Go checkout opts in through `Config\DefaultMonolith.ini`.
- Proxy/query logging is enabled by default. Unset or `MONOLITH_TOOL_LOG_ENABLED=1` enables it; `MONOLITH_TOOL_LOG_ENABLED=0` disables it before launching the proxy/query process.
- For proxy/query smoke tests or temporary diagnostics, set `MONOLITH_TOOL_LOG_DIR` before launching the process to isolate logs outside `Plugins\Monolith\Logs`.
- Use the logs to aggregate repeated missing-action, schema-confusing, retry, large-result, editor-unavailable, and escape-hatch patterns before changing namespace placement or action contracts.
- `python Plugins\Monolith\Analyzer\analyze_invocation_logs.py --log-root Plugins\Monolith\Logs --since <yyyyMMdd>` is the offline reader for exactly that aggregation: noise-classified findings, error classes, retry/duplicate clusters, slow calls, and per-surface format-version mix, written as markdown/json/csv under `Saved\Monolith\LogAnalysis\` (reader contract: `Docs\specs\SPEC_MonolithInvocationLogAnalyzer.md`).
- Do not commit `Plugins\Monolith\Logs\*`; logs can contain project/source context even after redaction and truncation.

## Offline CLI (no editor / no MCP)

When the editor and MCP server are down, `source` / `project` / `bridge` actions still work against the on-disk DBs via the bundled CLI (reads `Saved\EngineSource.db`, `Saved\ProjectIndex.db`, `Saved\graph.db`):

```
Plugins\Monolith\Binaries\monolith_query.exe source search_source UObject --limit=5
Plugins\Monolith\Binaries\monolith_query.exe project search Health --limit=10 --include-content=true
Plugins\Monolith\Binaries\monolith_query.exe project review_context /Game/Path/Asset --detail-level=minimal
Plugins\Monolith\Binaries\monolith_query.exe source health
Plugins\Monolith\Binaries\monolith_query.exe source find_overrides UActorComponent::BeginPlay --direction=in --max-depth=2
Plugins\Monolith\Binaries\monolith_query.exe source review_hotspots --kind=override --limit=10
```

The CLI is the MCP-free equivalent of `source_query` / `project_query` / `bridge_query` only — other namespaces need the running editor. Offline `project search` matches live `project.search`: `--include-content=true` is the default and searches assets, nodes, variables, parameters, DataTable rows, actors, and supplemental values; use `--include-content=false` for asset/node-only search.

## Source/project index freshness

When a source query fails to show a C++ change that is present on disk, treat the source index as stale before making source-backed conclusions.

1. Discover the current `source` reindex action schema through the live catalog, then call the source reindex action when available.
2. After the reindex reports completion, verify freshness by searching for the touched symbol, filename, or unique changed text through `source_query("search_source", ...)` or `source_query("read_source", ...)`.
3. If MCP/editor source reindex is unavailable or fails in the Go checkout, run the project's primary UBT build command from the checkout root, then verify the same symbol or unique changed text through `source_query` or `Plugins\Monolith\Binaries\monolith_query.exe source search_source ...`. Do not treat the build itself as source-index verification.
4. If the index still cannot see the change, report the concrete blocker and avoid source-index-backed review or API claims until indexing is fixed.

For stale CRG/FTS parity (as opposed to a stale index payload), `Scripts\check_index_freshness.ps1` runs the offline health -> repair -> re-verify chain for both DBs: report mode prints each health warning plus the exact offline and live repair commands; `-Execute` runs only the warning-indicated `repair_crg_cache`/`repair_fts` repairs and refuses on-disk DB writes while the MCP endpoint is up unless `-AllowLiveEditor` is passed (contract: `Docs\specs\SPEC_MonolithAgentOpsScripts.md`).

For project assets, `project.search` is content-inclusive by default and returns provenance fields such as `match_source`, `match_table`, `match_field`, `match_object_path`, and `match_value`; inspect them before treating a hit as an asset identity match. Use `include_content=false` only for identity-sensitive lookup or noisy name/type searches.

## Rules

- Route through the **live catalog** before calling actions; action names can change between versions.
- Prefer `monolith_find` → `monolith_discover(..., mode:"schema")` over guessing parameters.
- Treat the runtime registry as authoritative for sibling/custom actions too; static docs and skills are workflow guidance, not an exhaustive loaded-action roster.
- For high-risk actions, use focused schema discovery because strict validation may reject wrong JSON types, missing required fields, malformed query fragments, or out-of-range values before the handler runs.
- After indexing completes, the matching CRG projection/cache rebuilds automatically; run `project repair_crg_cache --execute` or `source repair_crg_cache --execute` only when health reports stale parity.
- `source repair_crg_cache --execute` rebuilds EngineSource `crg_*` metrics plus the signature-aware `source_override_edges` cache used by `find_overrides`, `impact_radius`, `risk_score`, `review_context`, and `review_hotspots --kind=override`. Use `source repair_crg_cache --scope=override_edges --execute` when only the override edge cache/version is stale.
- When project search looks stale, run `project health` first; `project repair_fts --target=all --execute` rebuilds all seven project FTS tables.
- `Saved\graph.db` is the CRG-compatible source graph export/search artifact, not the source of truth for source risk/review actions. Its `flows`, `communities`, and `risk_index` auxiliary tables are reserved placeholders; zero rows there are not a health failure.
