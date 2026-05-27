# Monolith — MonolithEditor Module

**Parent:** [SPEC_CORE.md](../SPEC_CORE.md)
**Engine:** Unreal Engine 5.7+
**Version:** 0.16.0 (Beta)

---

## MonolithEditor

**Dependencies:** Core, CoreUObject, Engine, MonolithCore, UnrealEd, Json, JsonUtilities, MessageLog, LiveCoding (Win64 only)

### Classes

| Class | Responsibility |
|-------|---------------|
| `FMonolithEditorModule` | Creates FMonolithLogCapture, attaches to GLog, registers 59 editor actions across build/log capture, crash reporting, context selection, viewport capture, automation, scripting, PIE, map, module-status, asset-context, crash-report, metadata, and v0.16.0 preview/inspection toolsets, and owns editor PIE transaction-buffer cleanup |
| `FMonolithLogCapture` | FOutputDevice subclass. Ring buffer (10,000 entries max). Thread-safe. Tracks counts by verbosity |
| `FMonolithEditorActions` | Static handlers for build and log operations. Hooks into `ILiveCodingModule::GetOnPatchCompleteDelegate()` to capture compile results and timestamps |
| `FMonolithSettingsCustomization` | IDetailCustomization for UMonolithSettings. Adds re-index buttons for project and source databases in Project Settings UI |

### Actions (59 — namespace: "editor")

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
| `get_live_coding_diagnostics` | Read-only Live Coding diagnostics summary. Returns availability/enabled/started flags, normalized compile result, diagnostic freshness, bounded compile log excerpts, error/warning counts, and an explicit empty `ubt_diagnostics` array because UBT artifact scraping is not part of this editor-session action. |
| `get_crash_context` | CrashContext.runtime-xml + Ensures.log + 20 recent errors. Truncated at 4096 chars |
| `capture_scene_preview` | Capture screenshot of Niagara or material asset in preview scene. Params: `asset_path`, `asset_type`, `seek_time`, `camera`, `resolution`, `output_path` |
| `capture_sequence_frames` | Multi-frame temporal capture at specified timestamps. Returns array of frame PNGs. Params: `asset_path`, `timestamps[]` (Max: 1000), `camera`, `resolution` |
| `stitch_flipbook` | Stitch multiple texture assets into a flipbook atlas. Params: `frames[]`, `columns`, `save_path` |
| `get_viewport_info` | Get active editor viewport camera location, rotation, FOV, resolution, realtime state |
| `list_open_viewports` | List level editor viewport capture sources and report visual-capture availability for asset-editor, widget-designer, and thumbnail paths. |
| `capture_level_viewport` | Capture a named level editor viewport to a PNG. Errors when the requested viewport is unavailable instead of silently falling back. |
| `capture_asset_thumbnail` | Capture an asset thumbnail to PNG only when `thumbnail_fallback=true`; returns `source="asset_thumbnail"` and never claims an asset-editor viewport capture. |
| `capture_asset_editor_viewport` | Reports structured `unavailable` until asset-editor viewport source discovery is safe. No level-viewport fallback. |
| `capture_widget_designer` | Reports structured `unavailable` until widget-designer viewport source discovery is safe. No level-viewport fallback. |
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
| `run_automation_tests` | Run automation tests by prefix in the running editor (no PIE, no separate process). `max_tests` limit defaults to 200 (hard max 1000). Returns run id, state, progress, success/passed/failed counts, per-test errors, and the applied `max_tests`. Records compact history for later inspection. |
| `get_automation_run_status` | Return the current Monolith automation run state when active, last run summary when idle, history count/capacity, and explicit `can_stop=false` / `stop_status="unsupported_cancel"` because this runner executes synchronously. |
| `stop_automation_tests` | Explicit no-op cancellation endpoint for parity with UnrealMCP clients. Returns `stopped=false`, `can_stop=false`, and `stop_status="unsupported_cancel"` instead of pretending synchronous `StartTestByName + StopTest` runs can be interrupted. |
| `list_automation_history` | List compact recent Monolith-triggered automation runs newest-first. `max_results` defaults to 20 and is capped to the in-memory history capacity. |
| `run_python` | Execute a Python command, statement, or file via IPythonScriptPlugin::ExecPythonCommandEx. Returns success, stdout/stderr captured by Python, and (for evaluate_statement mode) the evaluated result. |
| `load_level` | Close the current persistent level (without saving) and load the specified level by /Game/... asset path. Wraps ULevelEditorSubsystem::LoadLevel. |
| `start_pie` | Begin a PIE session pinned to in-viewport mode (`EPlaySessionWorldType::PlayInEditor` + first active level viewport via `FLevelEditorModule::GetFirstActiveViewport`). Independent of the user's `LastExecutedPlayModeType` toolbar choice. Returns `started: true, mode: 'in_viewport'`. Refuses to queue duplicates when PIE is already running. |
| `stop_pie` | End the active PIE session via `GUnrealEd->RequestEndPlayMap()`. No-op (returns `stopped: false`) if PIE not active. |
| `run_console_command` | Execute a console command. Routes to the first PIE PlayerController found (multi-client PIE not disambiguated); falls back to `GEngine->Exec` (with null-guard) when no PIE session is active. |

**Preview & Inspection (4 — v0.16.0)**

| Action | Description |
|--------|-------------|
| `capture_material_grid` | Render N material instances side-by-side under shared HDRI/floor/lighting in a single `FAdvancedPreviewScene` + `USceneCaptureComponent2D` capture pass. Auto-grid layout via `ceil(sqrt(N))` with optional `columns` override. Params: `material_paths[]`, `output_path`, `resolution`, `columns`, `preview_mesh`, `camera`. Failed loads logged + skipped; `material_count` reflects successes only. |
| `capture_with_overlay` | Single-asset capture under one of 5 engine debug show-flags: `wireframe`, `normals`, `uv_density`, `lightmap_density`, `shader_complexity`. Toggles the matching `FEngineShowFlags` setter before `CaptureScene`. Params: `asset_path`, `mode`, `output_path`, `resolution`, `camera`. UE 5.7 has no public normal-visualiser setter — `normals` mode falls back to `SetMeshEdges`; external name preserved for future engine versions. |
| `inspect_material_pbr` | Reflective walk of a material's texture parameter list, classifying each by PBR slot (basecolor/normal/roughness/metallic) and detecting ORM / ARM / MRA channel-packing conventions by substring. Pure JSON, no rendering. Returns scalar/vector/texture parameter lists per slot plus packed-channel flags. `material_class` distinguishes Material / MaterialInstance / MaterialInstanceConstant / MaterialInstanceDynamic. |
| `inspect_texture_channels` | `LockMipReadOnly` on `UTexture2D` source mip 0; walks pixel buffer per channel for min/max/mean statistics. Returns width, height, runtime pixel format, sRGB flag, has_alpha flag. Optional `emit_splits=true` writes 4 grayscale-replicated BGRA8 PNGs to `output_dir` for per-channel visual diff. Non-BGRA8 source returns a clean warning payload rather than mis-decoded bytes. |

Plus `capture_scene_preview` (in Base section above) was **extended** in v0.16.0: `asset_type` enum now also accepts `static_mesh`, `skeletal_mesh` (with optional `animation_path` + `seek_time` for posed-frame capture), and `widget` (UMG via `FWidgetRenderer` with `scale` DPI multiplier). No new action — schema widening only.

---

### Visual Capture Fallback Contract

`capture_asset_thumbnail` is the implemented fallback for PRD 34 visual verification when an actual asset-editor viewport cannot be identified. The caller must pass `thumbnail_fallback=true`; otherwise the action errors so clients do not mistake thumbnail output for viewport output. Supported `output_path` extensions (`png`, `jpg`/`jpeg`, `bmp`, `exr`, `tga`, `hdr`) select the image encoder; unknown or missing extensions are normalized to `.png`, and the response `output_path` and `format` fields report the normalized file path and actual encoder used. Asset-editor and widget-designer viewport captures remain explicit `unavailable` responses until Monolith can name the captured viewport source.

Generic asset lifecycle operations are owned by `MonolithAsset`: use `asset.import_texture_from_file`, `asset.save_asset`, and `asset.delete_assets` instead of editor-owned generic asset mutation verbs.

---

### PIE Transaction Buffer Cleanup

`FMonolithEditorModule` registers a private PIE transaction-buffer guard when the editor module starts and unregisters it on module shutdown. The guard calls `GEditor->ResetTransaction()` before PIE starts, after PIE ends, and before map loads while a PIE session is active. This keeps stale undo-buffer references from retaining old PIE worlds during garbage collection after Blueprint or asset mutation workflows.
