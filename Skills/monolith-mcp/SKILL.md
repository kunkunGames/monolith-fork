---
name: monolith-mcp
description: Use to discover and route Monolith MCP itself, and to manage the server — find the right namespace/action for a task, browse the live catalog and schemas, check health/version/action-count, trigger reindex, and manage tool profiles, execution-guard audit, readiness, onboarding, and notifications. Start here when you do not know which Monolith tool to call. Triggers on monolith, discover, find action, namespace, catalog, schema, reindex, status, health, action count, tool profile, enable namespace, disable action, execution guard, action audit, readiness, onboarding, MCP server status.
---

# monolith-mcp (discovery + server management)

Monolith exposes Unreal Engine editor capability to agents as ~1,600 actions across ~40 namespaces. The action/namespace catalog is **runtime-discovered** — never hand-maintain a full per-action list; query it.

## Core tools (always available)

| Tool | Use |
|------|-----|
| `monolith_find(query, namespace?)` | Find candidate namespaces/actions from a task description. **Call this first when the action is unclear.** |
| `monolith_discover(namespace?)` | Live catalog + schemas. Supports `mode=summary\|actions\|schema`. No arg → compact namespace summary. |
| `monolith_status()` | Health check: version, uptime, port, registered action count, module status. |
| `monolith_reindex()` | Re-index the project DB. Incremental (delta) by default; `force=true` for full wipe+rebuild. |

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

Beyond the four core tools, the `monolith` namespace carries server-management actions. Discover exact names/params with `monolith_discover({ namespace: "monolith" })`; current surface includes:

- **Catalog / domains:** `discover`, `find`, `describe_domain`, `list_domains`, `load_domain`, `get_loaded_domains`, `get_effective_discovery`, `get_mcp_discovery_state`
- **Tool profiles** (scope the action surface): `list_tool_profiles`, `get_tool_profile`, `create_tool_profile`, `update_tool_profile`, `delete_tool_profile`, `validate_tool_profile`, `set_active_tool_profile`, `set_namespace_enabled`, `set_action_enabled`, `set_action_description_override`, `set_action_execution_policy`
- **Execution guard / audit:** `get_execution_guard_status`, `list_recent_action_audit`, `get_last_rollback`, `list_tool_call_records`, `get_tool_call_record`, `analyze_tool_call_records`
- **Server / session:** `get_mcp_server_status`, `list_mcp_sessions`, `terminate_mcp_session`, `set_mcp_compatibility_options`
- **Readiness / onboarding:** `get_readiness_status`, `get_readiness_help`, `get_onboarding_state`, `set_onboarding_state`
- **Notifications:** `get_notification_settings`, `set_notification_settings`, `test_notification`
- **Maintenance:** `status`, `reindex`, `update`

## Go checkout MCP recovery

For Go checkout work that needs editor-backed Monolith actions, use the configured MCP client connection to `http://localhost:9316/mcp` and confirm it with `monolith_status()` or the active MCP client's health check before calling editor actions.

If the endpoint is unreachable or the MCP transport fails, treat it as an editor/server availability issue and start the project wrapper from the checkout root:

```powershell
.\BatchFiles\RunHeadlessEditor.bat
```

Keep the MCP client configuration on the existing Monolith proxy command; do not point MCP config at this wrapper. The wrapper resolves `UnrealEditor.exe` from `GO.uproject`, launches the full editor with rendering disabled by default (`-NullRHI`) plus unattended args, and leaves source control enabled. Script contract: `Docs\specs\Script\RunHeadlessEditor_SPEC.md`.

After launching the wrapper, wait for `localhost:9316` to listen, reconnect the existing Monolith proxy/client to `http://localhost:9316/mcp`, then re-run `monolith_status()` before using `monolith_find`, `monolith_discover`, or namespace actions. If the endpoint still cannot connect, inspect `Saved\HeadlessMcp\Logs\HeadlessEditor-*.log` plus the Monolith proxy/editor invocation logs, report the concrete blocker, and limit fallback work to read-only `Plugins\Monolith\Binaries\monolith_query.exe` source/project/bridge queries while editor-only actions remain blocked.

## Invocation diagnostics

When the checkout includes Monolith invocation logs, treat them as local diagnostics only. They do not replace the tool return shown to the agent, and their absence in an older checkout is not by itself a tool failure.

- Proxy calls append JSONL records under `Plugins\Monolith\Logs\<yyyyMMdd>\proxy.jsonl`.
- Offline query calls append JSONL records under `Plugins\Monolith\Logs\<yyyyMMdd>\query.jsonl`.
- Editor action dispatch appends JSONL records under `Plugins\Monolith\Logs\<yyyyMMdd>\action.jsonl` when `UMonolithSettings::bEnableDailyLog=true`; the Go checkout opts in through `Config\DefaultMonolith.ini`.
- Proxy/query logging is enabled by default. Unset or `MONOLITH_TOOL_LOG_ENABLED=1` enables it; `MONOLITH_TOOL_LOG_ENABLED=0` disables it before launching the proxy/query process.
- For proxy/query smoke tests or temporary diagnostics, set `MONOLITH_TOOL_LOG_DIR` before launching the process to isolate logs outside `Plugins\Monolith\Logs`.
- Use the logs to aggregate repeated missing-action, schema-confusing, retry, large-result, editor-unavailable, and escape-hatch patterns before changing namespace placement or action contracts.
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
