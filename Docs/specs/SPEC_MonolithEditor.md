# Monolith — MonolithEditor Module

**Parent:** [SPEC_CORE.md](../SPEC_CORE.md)
**Engine:** Unreal Engine 5.7+
**Version:** 0.14.10 (Beta)

---

## MonolithEditor

**Dependencies:** Core, CoreUObject, Engine, MonolithCore, UnrealEd, Json, JsonUtilities, MessageLog, LiveCoding (Win64 only)

### Classes

| Class | Responsibility |
|-------|---------------|
| `FMonolithEditorModule` | Creates FMonolithLogCapture, attaches to GLog, registers 36 actions (20 base + capture GIF + 4 crash breadcrumb actions + 3 selection actions + 2 Phase J F8 map actions + 2 v0.14.8 PR #48 automation + 2 v0.14.9 Issue #50 scripting + 3 v0.14.10 PR #54 PIE/console) |
| `FMonolithLogCapture` | FOutputDevice subclass. Ring buffer (10,000 entries max). Thread-safe. Tracks counts by verbosity |
| `FMonolithEditorActions` | Static handlers for build and log operations. Hooks into `ILiveCodingModule::GetOnPatchCompleteDelegate()` to capture compile results and timestamps |
| `FMonolithSettingsCustomization` | IDetailCustomization for UMonolithSettings. Adds re-index buttons for project and source databases in Project Settings UI |

### Actions (36 — namespace: "editor")

**Base (22 — v0.14.7 baseline + Phase J F8)**

| Action | Description |
|--------|-------------|
| `trigger_build` | Live Coding compile. `wait` param for synchronous. Windows-only. Auto-enables Live Coding |
| `live_compile` | Trigger Live Coding hot-reload compile. Alternative to trigger_build |
| `get_build_errors` | Build errors/warnings from log capture. Max 500 entries |
| `get_build_status` | Live Coding availability, started, enabled, compiling status |
| `get_build_summary` | Total error/warning counts + compile status |
| `search_build_output` | Search build log by `pattern`. Default limit 100, max 1000 |
| `get_recent_logs` | Recent log entries. Default 100, max 1000 |
| `search_logs` | Search by `pattern`, `category`, `verbosity`, `limit` (max 2000) |
| `tail_log` | Last N lines formatted `[category][verbosity] message`. Default 50, max 500 |
| `get_log_categories` | List all active log categories seen in ring buffer |
| `get_log_stats` | Log stats: total, fatal, error, warning, log, verbose counts |
| `get_compile_output` | Structured compile report: result, time, log lines from compile categories (LogLiveCoding, LogCompile, LogLinker), error/warning counts, patch status. Time-windowed to last compile |
| `get_crash_context` | CrashContext.runtime-xml + Ensures.log + 20 recent errors. Truncated at 4096 chars |
| `capture_scene_preview` | Capture screenshot of Niagara or material asset in preview scene. Params: `asset_path`, `asset_type`, `seek_time`, `camera`, `resolution`, `output_path` |
| `capture_sequence_frames` | Multi-frame temporal capture at specified timestamps. Returns array of frame PNGs. Params: `asset_path`, `timestamps[]`, `camera`, `resolution` |
| `import_texture` | Import external image (PNG/TGA/EXR/HDR) as UTexture2D with settings (compression, sRGB, tiling, LOD group). Params: `source_path`, `destination`, `settings` |
| `stitch_flipbook` | Stitch multiple texture assets into a flipbook atlas. Params: `frames[]`, `columns`, `save_path` |
| `delete_assets` | Delete one or more assets by path. Params: `asset_paths[]` (Max: 200), `force` |
| `get_viewport_info` | Get active editor viewport camera location, rotation, FOV, resolution, realtime state |
| `create_empty_map` | **Phase J F8.** Create a fully blank UWorld asset at `path` and save the package. v1 supports `map_template="blank"` only. Errors cleanly on path collision, malformed package path, factory/save failure |
| `get_module_status` | **Phase J F8.** Report `{ module_name, plugin_name, enabled, loaded, is_runtime, version? }` for the named modules (or all Monolith modules if `module_names` is omitted). Unknown modules return `enabled=false / loaded=false / plugin_name=""` without error |
| `dev_trigger_ensure` | DEV ONLY: Fires ensure(false) inside the breadcrumb scope to exercise the CrashRecovery capture pipeline. Editor stays alive. |
| `get_last_crash_reason` | Return the most recent Monolith crash breadcrumb (tool, action, params, timestamp). Returns {found:false} if no crash has been recorded. |
| `list_recent_crashes` | List recent Monolith crash breadcrumbs newest-first. Optional filters: limit (default 20, max 1000), since (ISO8601), tool (substring). |
| `get_crash_stats` | Aggregate Monolith crash counts grouped by tool, action, or tool_action. Optional 'since' filter (ISO8601). Useful for spotting recurrent crash sources. |
| `get_selected_actors` | Get stable metadata for actors selected in the Level Viewport or World Outliner |
| `get_selected_assets` | Get FAssetData-derived metadata for assets selected in the Content Browser |
| `get_active_asset_editor` | Get the active or unambiguous open asset editor with explicit fallback source |
| `capture_system_gif` | Capture a Niagara system as a sequence of PNG frames with optional GIF encoding via ffmpeg or python |
| `list_automation_tests` | List all registered automation tests, optionally filtered by prefix |
| `run_automation_tests` | Run automation tests by prefix in the running editor (no PIE, no separate process). `max_tests` limit defaults to 200 (hard max 1000). Returns success/passed/failed counts, per-test errors, and the applied `max_tests`. |
| `run_python` | Execute a Python command, statement, or file via IPythonScriptPlugin::ExecPythonCommandEx. Returns success, stdout/stderr captured by Python, and (for evaluate_statement mode) the evaluated result. |
| `load_level` | Close the current persistent level (without saving) and load the specified level by /Game/... asset path. Wraps ULevelEditorSubsystem::LoadLevel. |
| `start_pie` | Begin a PIE session pinned to in-viewport mode (`EPlaySessionWorldType::PlayInEditor` + first active level viewport via `FLevelEditorModule::GetFirstActiveViewport`). Independent of the user's `LastExecutedPlayModeType` toolbar choice. Returns `started: true, mode: 'in_viewport'`. Refuses to queue duplicates when PIE is already running. |
| `stop_pie` | End the active PIE session via `GUnrealEd->RequestEndPlayMap()`. No-op (returns `stopped: false`) if PIE not active. |
| `run_console_command` | Execute a console command. Routes to the first PIE PlayerController found (multi-client PIE not disambiguated); falls back to `GEngine->Exec` (with null-guard) when no PIE session is active. |

---
