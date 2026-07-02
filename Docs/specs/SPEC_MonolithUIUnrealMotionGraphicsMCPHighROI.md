# Monolith UI - UnrealMotionGraphicsMCP High-ROI Port Spec

**Status:** Draft
**Date:** 2026-07-02
**Scope:** Monolith-native UMG/UI improvements inspired by `D:\P4\UnrealMotionGraphicsMCP` source, docs, action prompts, and `Unreal_UMG_Widget_Guide_EN.md`.

## 1. Purpose

This spec records the high-ROI features worth porting or adapting from UnrealMotionGraphicsMCP into Monolith UI work. The goal is not feature parity with that plugin. The goal is to improve how agents complete Unreal UMG work through Monolith's existing action registry, typed schemas, dry-run/confirm guards, workflow proof objects, and editor/offline verification contracts.

Monolith must remain:

- **Monolith-native:** use existing `ui`, `editor`, `material`, `blueprint`, `workflow`, `bulk_fill`, `describe`, and skill surfaces instead of adding an external Python MCP bridge.
- **Monolith-only:** no dependency on `UmgMcp`, Gemini wrappers, prompt-manager web apps, `uv`, or an extra sidecar runtime.
- **Explicit by default:** mutating actions must keep explicit `asset_path` / target fields. Hidden "active target" fallback is useful as a read-only hint, but it must not silently redirect writes.
- **Proof-first:** new authoring conveniences must feed compile, layout, visual artifact, round-trip, and optional runtime proof rather than return weak success envelopes.

## 2. Survey Evidence

### 2.1 UnrealMotionGraphicsMCP Action Surface

The external repo exposes these UMG-relevant tool groups:

| Area | Evidence | Relevant actions/patterns |
| --- | --- | --- |
| Context and attention | `D:\P4\UnrealMotionGraphicsMCP\Resources\prompts.json`; `Source\Readme.md`; `Source\UmgMcp\Private\FileManage\UmgMcpAttentionCommands.cpp` | `get_target_umg_asset`, `set_target_umg_asset`, `get_target_widget`, `set_target_widget`, last/recent target history. |
| Widget sensing | `Resources\Python\UmgMcpServer.py`; `Source\UmgMcp\Private\Widget\UmgMcpWidgetCommands.cpp`; `Source\UmgMcp\Private\Widget\UmgGetSubsystem.cpp` | `get_widget_tree`, `query_widget_properties`, `get_creatable_widget_types`, `get_widget_schema`, `get_layout_data`, `check_widget_overlap`. |
| Widget mutation | `Source\UmgMcp\Private\Widget\UmgSetSubsystem.cpp` | `create_widget`, `set_widget_properties`, `delete_widget`, `reparent_widget`, `save_asset`; implicit parent from active widget. |
| JSON / markup transformation | `Readme.md`; `Source\UmgMcp\Private\FileManage\UmgMcpFileTransformationCommands.cpp`; `Resources\Python\Bridge\UMGHTMLParser.py` | `export_umg_to_json`, `apply_json_to_umg`, `apply_layout`, HTML/XML-like UMG tree to JSON conversion. |
| UMG animation | `Readme.md`; `Resources\Python\Animation\UMGSequencer.py`; `Source\UmgMcp\Private\Animation\UmgMcpSequencerCommands.cpp` | `animation_overview`, `animation_widget_properties`, `animation_time_properties`, append widget tracks, append time slices, scoped key deletes. |
| Widget Blueprint graph editing | `Readme.md`; `Resources\prompts.json`; `Source\UmgMcp\Private\Blueprint\UmgBlueprintFunctionSubsystem.cpp` | `set_edit_function`, `add_step`, `prepare_value`, `connect_data_to_pin`, cursor-node editing, widget event syntax such as `Button.OnClicked`. |
| UMG material/HLSL loop | `Readme.md`; `Resources\prompts.json`; `Source\UmgMcp\Private\Material\UmgMcpMaterialCommands.cpp` | UI-domain material target, single Custom node HLSL workflow, structured parameters, compile diagnostics. |
| Tool profile / prompt compression | `Readme.md`; `Resources\prompts.json`; `Resources\Python\PromptManager\server.py` | Tool enable/disable profiles and prompt templates to reduce action confusion. |

Inventory caveats found during the survey:

- `prompts.json` and README claim some surfaces that are stale or mismatched with actual Python/C++ routing.
- `set_active_widget` appears in prompt/catalog material while the public Python surface exposes `set_target_widget`.
- `set_animation_data` appears in docs/catalog and helper code but is not a reliable routed public action.
- `material_get_graph` is exposed by the Python/catalog layer but lacks a complete C++ command path in the surveyed source.
- `reparent_widget` and `move_widget` have contract/routing mismatches in the external plugin.
- `check_widget_overlap` is disabled/weak and returns too little evidence for Monolith proof gates.

### 2.2 UMG Guide Rules Worth Encoding

`D:\P4\UnrealMotionGraphicsMCP\Unreal_UMG_Widget_Guide_EN.md` is most useful as a rule source for audits and workflow guidance. High-value rules:

- Do not treat Canvas Panel as universal layout.
- Build automatic layout first; use absolute positioning last.
- Break complex UI into reusable UserWidgets.
- Do not use Canvas for one-child wrappers.
- Use ListView/TileView for large dynamic lists.
- Use render transforms for temporary animation; use layout properties for stable structure.
- Screen-edge UI should use anchors and SafeZone.
- Avoid Tick and property binding for regular UI updates; prefer event-driven/ViewModel updates.
- Decorative layers should not block mouse/touch input.
- Style button/focus/hover/disabled states explicitly.
- Use UI materials for repeated visual effects and UMG animation for state transitions.
- Test with target resolution, DPI, input device, and platform safe-zone conditions.

### 2.3 Source Docs Inventory

The repo docs divide into:

- `Readme.md`: action/status overview, prompt/profile concept, JSON source-of-truth workflow, UMG/Blueprint/Sequencer/Material/HLSL sections.
- `Source\Readme.md`: architecture and "current focus object" grammar for terse UI authoring.
- `Resources\Python\readme.md`: Python server/tool category overview.
- `Unreal_UMG_Widget_Guide_EN.md`: the canonical reusable UMG design guidance.
- `Readme_zh.md`, `CONTRIBUTING.md`, `Resources\Python\CONTRIBUTING.md`, `gemini.md`: process/localization material; not canonical Monolith product guidance.

### 2.4 UE 5.8 Engine Comparison Anchors

`D:\P4\speed\Speed.uproject` declares `EngineAssociation` `5.8`; therefore this spec treats `D:\Engine\UE_5.8` as the primary engine contract. UE 5.7 notes are useful only when a feature is absent or renamed in UE 5.8.

| Area | UE 5.8 anchor | Spec consequence |
| --- | --- | --- |
| Widget tree mutation | `UWidgetTree`, `UPanelWidget`, `UCanvasPanelSlot`, `FWidgetBlueprintOperationUtils` | Prefer engine editor-operation utilities for add/move/remove/rename/replace. Direct tree mutation is acceptable only when it preserves the same transaction, variable-map, slot, named-slot, cycle, and compile semantics. |
| Layout proof | `FWidgetRenderer`, `SVirtualWindow`, `FGeometry`, `SWidget::GetTickSpaceGeometry`, `USafeZone`/`SSafeZone`, `EVisibility`, `FHittestGrid`, `UListView` | Cached designer/editor geometry is not proof. Measurement must own a profile render/prepass path or return `unavailable`. |
| Animation | `UWidgetAnimation`, `FWidgetAnimationBinding`, `UMovieScene`, `UMovieScene2DTransformTrack`, `UWidgetAnimationDelegateBinding` | Animation identity and runtime lookup come from MovieScene bindings plus `AnimationBindings`, not possessable names alone. Delegate bindings are generated-class dynamic bindings, not property tracks. |
| UI materials/perf | `UImage::SetBrushFromMaterial`, `FSlateBrush::ResourceObject`, `UMaterialInterface::IsUIMaterial`, `URetainerBox`, `SRetainerWidget`, `UMaterialExpressionCustom` | UMG accepts material brushes, but Monolith must lint/prove `MD_UI`, Retainer texture-parameter correctness, Custom-node risks, and visual/perf evidence. |

## 3. Current Monolith Overlap

Monolith already covers much of the basic action inventory:

| External idea | Current Monolith surface | Gap |
| --- | --- | --- |
| Widget CRUD | `ui.create_widget_blueprint`, `ui.add_widget`, `ui.remove_widget`, `ui.rename_widget`, `ui.move_widget`, `ui.set_widget_property`, `ui.set_slot_property` | Keep Monolith's explicit schema; do not clone implicit stateful CRUD. |
| Active target context | No first-class `ui` work context action; most actions require explicit `asset_path` | Add explicit, session/request-scoped context helpers for ergonomics, but never as hidden mutating fallback. |
| Widget schema/property lookup | `ui.list_widget_types`, `ui.list_widget_properties`, `ui.dump_property_allowlist`, `ui.list_widget_property_enums` | Missing one read-only, type-level "what can I safely set and why?" schema with tooltips, enum values, allowlist status, aliases, and slot paths. |
| Editor-safe widget mutation | `ui.add_widget`, `ui.remove_widget`, `ui.rename_widget`, `ui.move_widget`, spec build/patch candidates | UE 5.8 exposes editor operation utilities that preserve slot/named-slot/transaction/variable semantics. Monolith should use them or prove equivalent behavior before expanding patch workflows. |
| JSON source of truth | `ui.build_ui_from_spec`, `ui.dump_ui_spec`, `ui.dump_ui_spec_schema` | Good canonical base. Missing spec diff/patch ergonomics and markup-to-spec conversion. |
| Layout audit | `ui.audit_widget_layout` | Structural audit exists. Authored bounds/overlap/safe-zone evidence now lives in `ui.measure_widget_layout`; full render/prepass geometry remains pending. |
| Visual proof | `editor.capture_scene_preview(asset_type="widget")`, `ui.verify_widget_visual_artifacts`, `ui.measure_widget_layout`, `workflow.ui_shipping_widget_blueprint(proof_profile="visual")` | Good base. Visual/runtime proof now composes authored layout-bounds evidence; future render-geometry measurement remains pending. |
| UMG animation creation | `ui.create_animation_v2`, `ui.add_bezier_eased_segment`, `ui.bake_spring_animation`, `ui.add_animation_event_track`, `ui.bind_animation_to_event` | Missing compact animation overview/timeline/time-slice read actions and delta editing for existing animations. |
| Blueprint event graph editing | Generic Blueprint graph/source actions exist outside `ui` | Missing UI-specific event binding workflow that respects the ViewModel boundary and proves compile/read-back. |
| UI materials | `material` namespace + UI styling/effect actions | Missing a UI-domain material/Retainer workflow that proves `MD_UI`, brush/effect binding, Custom-node diagnostics, and visual/perf evidence. |
| Tool profile compression | Monolith tool profiles, skills, discovery metadata, invocation logs | Do not port the prompt web UI. Add UI task profiles/guidance only if logs show schema confusion. |

### 3.1 Anti-Duplication Contract

Every implementation slice must classify an external UnrealMotionGraphicsMCP API into exactly one Monolith treatment before adding code:

| Treatment | Rule |
| --- | --- |
| Reuse existing primitive | No new action. Improve docs, examples, search metadata, schema hints, or tests around the existing action. |
| Enhance existing primitive | Keep the current Monolith action name and extend its implementation/result shape without breaking callers. |
| Add read-only guidance/proof action | Allowed only when no current action answers the question and the action reduces unsafe writes or proof gaps. |
| Add workflow | Allowed only when it composes at least two existing domain primitives and adds UI-specific policy, proof, or ViewModel/runtime boundaries. |
| Reject | Do not add an alias, wrapper, or clone when the external API is stale, stateful, weaker than Monolith, or belongs to another namespace. |

Implementation requirements:

- Do not add external-name compatibility aliases such as `create_widget`, `apply_json_to_umg`, `hlsl_set`, or `material_add_node` unless a future migration plan proves they reduce real invocation-log confusion without hiding weaker semantics.
- New workflow actions must expose the underlying Monolith primitive actions they plan or executed.
- New primitive actions must include a `duplicate_audit` note in the implementing PR/spec update that lists the existing Monolith action names checked.
- If an existing action already owns the operation but lacks proof, upgrade proof/read-back on that action or add a workflow proof wrapper; do not create a second primitive with the same effect.
- If a feature is useful only as a shorthand over an explicit action, prefer skills, action search metadata, examples, and tool profiles over new registry actions.

### 3.2 External API Deduplication Map

This table maps the full external API inventory to Monolith-safe implementation choices.

| External API group | Use as-is? | Monolith-safe treatment |
| --- | --- | --- |
| `get_target_umg_asset`, `get_target_widget`, last/recent target reads | Partial | Add explicit `ui.get_widget_context` style helpers only. They are convenience/read context, not mutation routing. |
| `set_target_umg_asset`, `set_target_widget`, `animation_target`, `widget_target` | No as mutating fallback | Add explicit `ui.set_widget_context`, but never auto-create assets/animations or silently target writes from context. Use `ui.create_widget_blueprint`, `ui.create_animation_v2`, and explicit action params for real mutation. |
| `get_widget_tree` | Already covered | Reuse existing `ui.get_widget_tree`; improve token-efficient examples/search metadata if needed. |
| `query_widget_properties` | Mostly covered | Reuse `ui.list_widget_properties`, `ui.dump_ui_spec`, and `ui.describe_widget_type_schema` for safe property discovery. Do not add a generic raw property query clone. |
| `get_creatable_widget_types` | Already covered | Reuse `ui.list_widget_types`; enhance metadata rather than add a synonym. |
| `get_widget_schema` | Enhance via new read action | Add `ui.describe_widget_type_schema` only because it merges allowlist, reflection, slots, enum values, setter hints, and examples beyond existing lists. |
| `get_layout_data`, `check_widget_overlap` | Replace semantics | Add `ui.measure_widget_layout`; do not port cached-geometry overlap because UE cached geometry is not proof. |
| `create_widget`, `delete_widget`, `set_widget_properties`, `reparent_widget`, `save_asset` | No | Reuse/upgrade `ui.add_widget`, `ui.remove_widget`, `ui.set_widget_property`, `ui.set_slot_property`, `ui.move_widget`, and `asset.save_asset`. Do not add active-widget union-write variants. |
| `export_umg_to_json` | Already covered under stronger model | Reuse `ui.dump_ui_spec`; keep `FUISpecDocument` as the canonical round-trip contract. |
| `apply_json_to_umg`, `apply_layout` | No direct port | Add `ui.convert_markup_to_ui_spec`, `ui.diff_ui_spec`, and confirm-gated `ui.apply_ui_spec_patch`; mutation must go through spec validation and editor-safe widget mutation. |
| `set_edit_function`, `set_cursor_node`, `add_step`, `prepare_value`, `connect_data_to_pin`, `delete_node`, `add_variable`, `delete_variable`, `compile_blueprint` | No UI duplicates | Use existing `blueprint` namespace primitives where available. Add only `workflow.ui_bind_widget_event` to compose Blueprint primitives with UI/ViewModel policy and compile/read-back proof. |
| `animation_overview`, `animation_widget_properties`, `animation_time_properties` | Adapt | Add `ui.get_animation_overview`, `ui.get_animation_timeline`, `ui.get_animation_time_slice` as read-only UMG/MovieScene views; do not duplicate `ui.create_animation_v2`. |
| `animation_append_widget_tracks`, `animation_append_time_slice`, `animation_delete_widget_keys`, `set_property_keys` | Adapt | Add `ui.apply_animation_delta` as the single safe aggregate for existing-animation edits. Keep create paths in `ui.create_animation_v2`; deletes require `confirm_delete`. |
| `material_set_target`, `material_define_variable`, `material_add_node`, `material_connect_*`, `material_set_*`, `material_delete`, `material_get_*`, `material_compile_asset` | No UI duplicates | Use existing `material` primitives such as `create_material`, `build_material_graph`, `create_custom_hlsl_node`, `update_custom_hlsl_node`, `connect_expressions`, `set_expression_property`, `recompile_material`, `delete_expression(s)`, and graph/pin introspection actions. |
| `hlsl_set_target`, `hlsl_get`, `hlsl_set`, `hlsl_compile` | No new `hlsl` namespace | Use `material.create_custom_hlsl_node` / `material.update_custom_hlsl_node` plus `material.recompile_material` / stats. Add only UI workflow proof for `MD_UI`, brush binding, Retainer parameter, and visual evidence. |
| `set_widget_style`, `apply_global_theme`, `style_create_asset` | Partial, mostly existing | Reuse CommonUI style actions (`create_common_button_style`, `create_common_text_style`, `create_common_border_style`, `apply_style_to_widget`, `batch_retheme`). Add native UMG style support only for concrete gaps such as `FButtonStyle` read/write schema that CommonUI does not cover. |

## 4. ROI Backlog

| Priority | Candidate | ROI | Why it matters |
| --- | --- | --- | --- |
| P0 | Type-level UMG property/schema guidance | Very high | Reduces schema confusion and raw-mode misuse before any write. |
| P0 | Explicit UI work context | Very high | Keeps repeated UMG edits concise without unsafe global hidden defaults. |
| P0 | Layout geometry and overlap verifier | Very high | Turns "looks okay" into measurable bounds/overlap proof across target resolutions. |
| P0 | Guide-derived production lint expansion | Very high | Encodes common UMG mistakes agents repeatedly make: Canvas overuse, SafeZone misses, hit-test blockers, dynamic-list misuse. |
| P0 | UE 5.8 editor-safe widget mutation layer | Very high | Prevents direct tree edits from losing slot data, named-slot semantics, Blueprint variables, transactions, or compile consistency. |
| P1 | Markup/compact layout to `FUISpecDocument` converter | High | Lets agents author dense UI layout in a human-readable format while preserving Monolith's transactional `build_ui_from_spec`. |
| P1 | UMG animation overview and delta editing | High | Existing authoring is strong, but safe modification of existing animations needs read/diff/delete proofs. |
| P1 | UI event-binding workflow | High | Interaction work fails when graph editing is ad hoc; a UI-specific workflow can compose Blueprint graph primitives with ViewModel policy. |
| P1 | UI-domain material/Retainer proof workflow | Medium-high | Advanced UI effects are common enough to justify a workflow, but it must compose `material` + `ui` rather than duplicate low-level material actions. |
| P2 | UI task tool-profile presets | Medium | Helpful only if invocation logs show repeated action confusion after schema/guidance improvements. |

## 5. P0 Specs

### 5.1 `ui.describe_widget_type_schema`

Add a read-only action that merges reflection, type registry, property allowlist, slot support, enum values, aliases, and examples for one widget type or one live widget instance.

Proposed params:

```json
{
  "widget_class": "Button|TextBlock|/Script/UMG.Button",
  "asset_path": "/Game/UI/WBP_Menu",
  "widget_name": "StartButton",
  "include_inherited": false,
  "include_unsafe": false,
  "include_examples": true
}
```

Proposed result:

```json
{
  "ok": true,
  "schema_version": "ui_widget_type_schema.v1",
  "widget_class": "/Script/UMG.Button",
  "resolved_from": "widget_class|live_widget",
  "engine_path": "/Script/UMG.Button",
  "live_slot_class": "/Script/UMG.CanvasPanelSlot",
  "properties": [
    {
      "path": "ColorAndOpacity",
      "json_path": "ColorAndOpacity",
      "cpp_type": "FLinearColor",
      "settable": true,
      "allowlist_status": "allowed|blocked|requires_raw_mode|slot_only",
      "enum_values": [],
      "aliases": ["color_and_opacity"],
      "setter": "SetColorAndOpacity",
      "deprecated_direct_access": false,
      "tooltip": "...",
      "example_value": [1, 1, 1, 1]
    }
  ],
  "slot_properties": [
    {
      "path": "Slot.Anchors",
      "slot_class": "/Script/UMG.CanvasPanelSlot",
      "settable": true,
      "slot_context": "only when parent produces CanvasPanelSlot"
    }
  ],
  "warnings": [],
  "next_actions": [
    {"tool": "ui.set_widget_property", "available": true},
    {"tool": "ui.dump_property_allowlist", "available": true}
  ]
}
```

Implementation notes:

- Use `MonolithUIRegistrySubsystem`, `UIPropertyAllowlist`, and existing reflection helpers.
- Do not loosen `set_widget_property`; this is guidance, not a bypass.
- Include exact reason when a property is blocked.
- Include enum values currently surfaced by `ui.list_widget_property_enums`.
- Include the actual live `slot_class` only when `asset_path` + `widget_name` are supplied and the widget currently has a parent slot.
- Mark slot paths as contextual. `Slot.*` writes are valid only when the live parent creates that slot class; root widgets have no slot, and Canvas/Box/Grid slot schemas are not interchangeable.
- Surface setter/deprecated-direct-access hints from reflected metadata so agents prefer engine-supported setters over raw field writes.

Acceptance:

- Automation covers `Button`, `TextBlock`, `Image`, `CanvasPanelSlot`, and a live fixture WBP.
- `raw_mode=false` blocked paths are accurately marked.
- Slot acceptance covers root-widget-no-slot, Canvas-vs-Box mismatch, single-child parent capacity, and a valid `UCanvasPanelSlot` setter path.
- Result is stable enough for `describe_query("action_schema")` style agent consumption.

Implementation status (2026-07-02):

- Landed initial Monolith-native `ui.describe_widget_type_schema` in `MonolithUIRegistryActions.cpp`.
- The action reuses `UMonolithUIRegistrySubsystem`, `FUITypeRegistry`, curated property mappings, enum reflection, live widget slot context, and owner-action `next_actions`.
- It does not loosen `ui.set_widget_property` and does not add external compatibility aliases.
- Current test coverage proves TextBlock allowlist schema and Button enum surfacing. Remaining acceptance gaps: Image fixture, live Canvas slot fixture, root-no-slot warning fixture, and stricter `include_inherited` behavior.

### 5.2 `ui.set_widget_context`, `ui.get_widget_context`, `ui.clear_widget_context`

Add explicit UI work-context actions inspired by UnrealMotionGraphicsMCP's attention subsystem. The goal is a concise authoring loop, not implicit write routing.

Proposed `set` params:

```json
{
  "asset_path": "/Game/UI/WBP_Menu",
  "widget_name": "StartButton",
  "animation_name": "Intro",
  "scope": "session|request",
  "ttl_seconds": 900
}
```

Proposed `get` result:

```json
{
  "ok": true,
  "schema_version": "ui_widget_context.v1",
  "context": {
    "asset_path": "/Game/UI/WBP_Menu",
    "widget_name": "StartButton",
    "animation_name": "Intro",
    "scope": "session",
    "source": "explicit_set",
    "expires_at": "..."
  },
  "resolved": {
    "asset_exists": true,
    "widget_exists": true,
    "animation_exists": true
  },
  "usable_as_default_for_mutation": false
}
```

Rules:

- Mutating actions must continue to require explicit targets in their public schema.
- A workflow may read this context to prefill a plan, but the plan must echo the resolved asset/widget/animation and require confirmation before write.
- No auto-created default assets, no editor-focus-based silent switch, no cross-agent global singleton.
- Context is diagnostic state, not source of truth; every result must include `resolved_from` and freshness.

Acceptance:

- Context set/get/clear works in isolated sessions without leaking across concurrent requests.
- If a target asset or widget is deleted, `get` reports stale context and no mutation uses it silently.
- Workflow dry-runs can include `suggested_defaults` from context while still showing explicit action params.

### 5.3 `ui.measure_widget_layout`

Add a read-only geometry action that renders or constructs the widget at one or more target profiles and reports screen-space bounds plus overlap/safe-zone findings.

Implementation status:

- Initial v1 implementation landed 2026-07-02 in `MonolithUISpecActions.cpp` as a read-only authored layout model action.
- v1 reuses `FUISpecSerializer` output, reports `measurement_model="authored_spec_layout_model"`, detects sibling overlap and explicit safe-zone violations across requested profiles, and never mutates assets.
- v1 deliberately sets `render_geometry_proof=false` and `render_bounds_available=false`; it does not use cached designer geometry. Full virtual-window render/prepass geometry remains a future upgrade and should compose with `editor.capture_scene_preview` / `ui.verify_widget_visual_artifacts` for visual proof.
- `workflow.ui_shipping_widget_blueprint(proof_profile="visual"|"runtime")` now composes `ui.measure_widget_layout` by default through `run_layout_measure=true`, using the first `visual_profiles[]` row or the explicit `preview_resolution` as authored layout evidence. The workflow exposes this under `validation.layout_measure`, `proof.layout_measure`, `proof.ui_evidence.layout_measure_status`, and `next_actions[]`.

Proposed params:

```json
{
  "asset_path": "/Game/UI/WBP_Menu",
  "profiles": [
    {
      "name": "desktop",
      "resolution": [1920, 1080],
      "dpi_scale": 1.0,
      "rect_space": "both",
      "visibility_filter": ["Visible", "HitTestInvisible", "SelfHitTestInvisible"],
      "list_sampling": {"mode": "visible_only"},
      "retainer_policy": "request_render",
      "settle_frames": 1
    },
    {
      "name": "mobile",
      "resolution": [1280, 720],
      "dpi_source": "project_rule",
      "safe_zone": {"left": 48, "top": 24, "right": 48, "bottom": 24},
      "rect_space": "both"
    }
  ],
  "check_overlap": true,
  "check_safe_zone": true,
  "max_allowed_overlap_ratio": 0.0
}
```

Proposed result:

```json
{
  "ok": true,
  "schema_version": "ui_layout_measure.v1",
  "asset_path": "/Game/UI/WBP_Menu",
  "profiles": [
    {
      "name": "desktop",
      "resolution": [1920, 1080],
      "dpi_scale": 1.0,
      "measurement_source": "owned_virtual_window_render",
      "widgets": [
        {
          "widget_name": "StartButton",
          "class": "Button",
          "visibility": "Visible",
          "hit_test_visible": true,
          "layout_bounds": {"x": 760, "y": 420, "w": 400, "h": 72},
          "render_bounds": {"x": 760, "y": 420, "w": 400, "h": 72},
          "layout_render_divergence": 0.0
        }
      ],
      "overlaps": [],
      "safe_zone_violations": [],
      "status": "pass"
    }
  ],
  "checks": []
}
```

Implementation notes:

- Use a Monolith-owned offscreen widget measurement path, such as a virtual window / `FWidgetRenderer`-style prepass and paint, and read geometry from that same pass.
- Do not treat cached designer/editor `GetCachedGeometry()` or stale `GetTickSpaceGeometry()` as layout proof. If Monolith cannot own the profile measurement pass, return `status="unavailable"` with an `unavailable_reason`.
- Report layout bounds and render bounds separately. Layout rectangles prove layout-space intersection; render rectangles capture render-transform divergence.
- Visibility and hit-test status are separate. `Hidden` still occupies layout, `Collapsed` does not, and `HitTestInvisible` / `SelfHitTestInvisible` affect input without necessarily changing geometry.
- List virtualization is limited by the materialized entries in the measurement pass. Default to `visible_only`; require explicit scroll offsets or item sampling before claiming coverage for virtualized `ListView`/`TileView` rows.
- Safe-zone proof must use explicit safe-zone input or a declared source. Do not infer platform-safe margins from a desktop capture.
- Retainer proof must record whether retained content was requested/forced to render for the profile and whether the measurement saw fresh content.
- Compose with `workflow.ui_shipping_widget_blueprint(proof_profile="visual"|"runtime")` as optional-but-default layout evidence via `run_layout_measure=true`; do not create external `get_layout_data` / `check_widget_overlap` compatibility actions.
- Keep `ui.audit_widget_layout` as structural static lint; `ui.measure_widget_layout` is geometry evidence.
- This action cannot prove every runtime state, localized text expansion, focus/navigation route, click routing result, or semantic wrongness of an overlap. Those remain visual artifact and runtime interaction proof responsibilities.

Acceptance:

- Changing requested resolution changes measured bounds for an anchored/responsive fixture.
- Generated WBP fixture with intentional overlap reports a deterministic finding.
- Layout-vs-render-transform fixture reports non-zero divergence.
- Visibility fixture distinguishes `Visible`, `Hidden`, `Collapsed`, `HitTestInvisible`, and `SelfHitTestInvisible`.
- Safe-zone fixture reports edge violations at mobile profile and passes after wrapping the edge group in SafeZone.
- `ScrollBox` fixture reports all arranged children; `ListView` fixture reports only materialized entries unless sampling is requested.
- Null-RHI/server paths return `unavailable` or `blocked`, not fake success.
- Visual/runtime workflow proof exposes `layout_measure.status` through `validation.layout_measure` and `proof.ui_evidence.layout_measure_status`.

### 5.4 Expand `ui.audit_widget_layout` With Production Lints

Add guide-derived rule IDs to the existing structural audit.

New rule IDs:

| Rule ID | Severity | Finding |
| --- | --- | --- |
| `CanvasOveruse` | warning | Deep or repeated Canvas Panels where an auto-layout panel is more appropriate. |
| `OneChildCanvasWrapper` | warning | Canvas Panel with one child and no anchor/Z-order need. |
| `EdgeUiMissingSafeZone` | warning/error by profile | Top/bottom/edge HUD group not wrapped by SafeZone or equivalent. |
| `DecorativeHitTestBlocker` | error | Image/Border/Overlay layer above interactive widget is hit-test visible. |
| `LargeStaticListWithoutListView` | warning | Large repeated child set under ScrollBox/VerticalBox where ListView/TileView is likely required. |
| `LayoutAnimatedForStableState` | warning | Animation targets layout/slot properties for repeated visual effects instead of render transform/material parameters. |
| `UnstyledInteractiveState` | warning | Button-like widget lacks explicit normal/hover/pressed/disabled/focus style evidence where inspectable. |
| `EventDrivenUpdateMissing` | advisory | Widget has Tick/property-binding patterns for frequently updated UI where ViewModel/event update should be used. |
| `RetainerMisuse` | warning/error by profile | RetainerBox wraps fast-changing or large/fullscreen content without freshness/perf proof. |
| `InvalidationMisuse` | warning | InvalidationBox wraps continuously changing content, or volatile widgets are not marked appropriately. |
| `HiddenInteractiveSpace` | warning | Responsive removal uses `Hidden` where occupied layout space is likely unintended; prefer `Collapsed` when removing from layout. |
| `DpiSafeZoneProfileMissing` | error for proof profiles | Mobile/console proof profile omits explicit DPI and safe-zone inputs. |
| `MaterialDomainMismatch` | error | UMG-bound brush/effect material is not `MD_UI`. |
| `RetainerEffectParameterMismatch` | error | RetainerBox texture parameter does not exist on the effect material or does not match the widget property. |

Implementation notes:

- Keep the action read-only.
- Add `rule_profile`: `strict`, `shipping`, `advisory`.
- Let `workflow.ui_shipping_widget_blueprint` include these lints under `proof.checks[]`.
- Do not hard fail on every guide recommendation by default; warnings must be actionable and suppressible by rule ID.
- Retainer and Invalidation findings are heuristics unless paired with runtime/profile evidence. They should trigger proof requirements before recommending the optimization.

Acceptance:

- Fixture tests for one-child Canvas, decorative hit-test blocker, and SafeZone miss.
- Fixture tests for RetainerBox parameter mismatch, non-UI material binding, Hidden-vs-Collapsed layout risk, and missing mobile DPI/safe-zone profile.
- Existing `audit_widget_layout` tests continue to pass.
- Result includes `suggested_fix` using existing Monolith action names.

Implementation status (2026-07-02):

- Initial `ui.audit_widget_layout` production-lint slice landed without adding a duplicate public action. Findings now include stable `rule_id` and `rule_profile` fields and support `rule_profile=advisory|shipping|strict` plus `suppress_rule_ids[]`.
- Implemented low-risk static rules using only canonical `FUISpecSerializer` data: `OneChildCanvasWrapper`, `CanvasOveruse`, `EdgeUiMissingSafeZone` (warning by default, error in `strict`), `DecorativeHitTestBlocker`, `HiddenInteractiveSpace`, `UnstyledInteractiveState`, `MaterialDomainMismatch`, and `LargeStaticListWithoutListView`.
- `UnstyledInteractiveState` deliberately stays conservative: it only warns for button-like controls (`Button`/CommonButton-style names and `CheckBox`) that lack serialized `styleRef` or CommonUI `StyleRefs` evidence. Native per-state `FButtonStyle` inspection is recorded as `not_serialized` rather than guessed.
- `MaterialDomainMismatch` now uses serialized `content.brushPath` evidence: if the brush resource resolves as `UMaterialInterface` and its base material domain is not `MD_UI`, `ui.audit_widget_layout` emits an error with `material_path`, `material_domain`, and `expected_domain`.
- Focused fixtures cover one-child Canvas, decorative hit-test blocker, hidden interactive layout space, button-like controls without serialized style evidence, Surface-domain Image brush material mismatch, and static SafeZone ancestry miss. Existing Canvas anchor and dynamic-text tests remain the regression baseline.
- Deferred rules remain intentionally evidence-gated: Retainer/Invalidation rules need freshness/volatility/runtime profile evidence. `DpiSafeZoneProfileMissing` now belongs to `workflow.ui_shipping_widget_blueprint` profile validation rather than the WBP tree-only lint pass.

Workflow-profile follow-up status (2026-07-02):

- `workflow.ui_shipping_widget_blueprint` now forwards `layout_rule_profile` and `suppress_layout_rule_ids` to the existing `ui.audit_widget_layout` owner action, preserving the single structural-lint surface.
- Mobile/console/handheld/TV `visual_profiles[]` rows now require explicit `dpi_scale` and `safe_zone`; missing fields produce `validation.visual_profile.findings[].rule_id="DpiSafeZoneProfileMissing"` and block the visual/runtime proof envelope.

### 5.5 UE 5.8 Editor-Safe Widget Mutation Layer

Before expanding spec patch workflows, upgrade mutating widget operations to match UE 5.8 editor semantics. `FWidgetBlueprintOperationUtils` is the preferred implementation path for add, move, remove, rename, replace, wrap, named-slot replacement, and event-property binding where available.

Rules:

- Prefer UE 5.8 operation utilities for widget add/move/remove/rename/replace. If a Monolith action keeps direct `UWidgetTree` or `UPanelWidget` edits, it must document why and prove equivalent behavior.
- Preserve slot properties, named-slot bindings, parent single-child constraints, widget variable maps, inherited/BindWidget constraints, transaction state, `RF_Transactional`, `Modify()`, structural blueprint modification, compile, and read-back.
- `ui.move_widget` is a high-risk action until it proves slot-preserving move behavior. Moving through remove/add is not acceptable unless slot properties are captured and restored exactly and named-slot semantics are preserved.
- `ui.apply_ui_spec_patch` must use this mutation layer for add/move/remove/replace operations; broad raw JSON-to-UMG writes remain non-candidates.
- Mutating operations must return `operation_source` (`ue_operation_utils|monolith_equivalent`), `slot_preservation`, compile result, warnings, and post-write tree/spec read-back.

Acceptance:

- Move fixture preserves `UCanvasPanelSlot` anchors, offsets, alignment, Z-order, and parent index.
- Named-slot fixture preserves or rejects named-slot moves explicitly.
- Single-child parent fixture fails before mutation when capacity is exhausted.
- Rename/remove fixtures keep Blueprint variables and animation bindings consistent after compile.
- Spec patch fixture proves add, move, property update, and remove through read-back, not by trusting the input patch.

Implementation status (2026-07-02):

- Initial `ui.move_widget` safe-move slice landed in `MonolithUISlotActions.cpp` without adding external `move_widget` / `reparent_widget` action aliases.
- The action now rejects root/no-slot moves and exhausted single-child parents before mutation, returns `operation_source="monolith_equivalent"`, and reports `slot_preservation`.
- Compatible slot preservation currently covers `UCanvasPanelSlot` layout/autosize/Z-order plus `UVerticalBoxSlot`, `UHorizontalBoxSlot`, and `UOverlaySlot` padding/alignment/size where applicable.
- Focused automation covers CanvasPanel-to-CanvasPanel slot preservation, full single-child parent rejection, and initial `ui.apply_ui_spec_patch` routing through existing owner actions for add/remove/move/slot/property/text changes. The patch planner now preflights `set_slot_property` / `set_anchor_preset` steps against the live WBP slot class before execution. Remaining P0.5 gaps: named-slot fixture, variable-map/animation-binding consistency for rename/remove, and richer replace routing.

## 6. P1 Specs

### 6.1 `ui.convert_markup_to_ui_spec`

Port the useful part of external `UMGHTMLParser.py`, but make it read-only and canonicalize into Monolith `FUISpecDocument`. Do not mutate assets directly.

Proposed params:

```json
{
  "markup": "<VerticalBox Name=\"Root\"><Button Name=\"Play\"><TextBlock Name=\"Label\" Text=\"Play\"/></Button></VerticalBox>",
  "dialect": "umg_xml_v1",
  "root_save_path": "/Game/UI/WBP_Menu",
  "strict": true,
  "spec_name": "WBP_Menu",
  "parent_class": "UserWidget",
  "source_name": "designer-export.xml",
  "treat_warnings_as_errors": false,
  "request_id": "optional-correlation-token"
}
```

Proposed result:

```json
{
  "ok": true,
  "schema_version": "ui_markup_to_spec.v1",
  "would_create_asset": false,
  "spec": {},
  "validation": {
    "is_valid": true,
    "llm_report": "..."
  },
  "warnings": [],
  "next_actions": [
    {"tool": "ui.build_ui_from_spec", "available": true}
  ]
}
```

Rules:

- Supported tags map only to registered `UITypeRegistry` types.
- Slot attributes use explicit `slot.*` namespace and validate against parent slot type.
- Unknown tags fail in `strict=true`, warn in `strict=false`.
- The action must never call `build_ui_from_spec` internally.

Acceptance:

- Round-trip fixture: markup -> spec -> build -> dump spec has expected tree.
- Invalid slot attribute returns structured validation error.

Implementation status (2026-07-02):

- Landed initial read-only `ui.convert_markup_to_ui_spec` in `MonolithUISpecActions.cpp`.
- Supported dialects are `umg_xml_v1`, `umg_html_v1`, and `html`, parsed as strict XML-style markup through UE `XmlParser`; loose browser HTML mutation is intentionally not supported.
- Tags map to `UITypeRegistry` tokens; unknown tags are errors in `strict=true` and warnings in `strict=false`.
- Supported attributes include `Name`/`id`, content fields, `style.*`, `slot.*`, `common.*`, and `effect.*`. Unsupported CSS/event/raw-property attributes are rejected or warned based on `strict`.
- The action returns canonical `FUISpecDocument` JSON through the same `DocumentToJson` path used by `dump_ui_spec`, never calls `build_ui_from_spec`, and reports `would_create_asset=false`.
- Current test coverage proves basic tree conversion, strict unknown-tag failure, `root_save_path` read-only name derivation, parent-slot contextual rejection for incompatible `slot.*` attributes, and an action-level `convert_markup_to_ui_spec -> build_ui_from_spec -> dump_ui_spec` round-trip fixture that builds `/Game/Tests/Monolith/UI/WBP_MarkupRoundtrip`, dumps it back to `FUISpecDocument`, and verifies the expected Canvas/TextBlock/Button tree.

### 6.2 `ui.diff_ui_spec` and `ui.apply_ui_spec_patch`

UnrealMotionGraphicsMCP's JSON source-of-truth idea is already better represented by Monolith's `dump_ui_spec` / `build_ui_from_spec`. The missing ROI is ergonomic deltas for existing WBPs.

Proposed `ui.diff_ui_spec`:

```json
{
  "asset_path": "/Game/UI/WBP_Menu",
  "desired_spec": {},
  "compare_mode": "structural|properties|full"
}
```

Proposed `ui.apply_ui_spec_patch`:

```json
{
  "asset_path": "/Game/UI/WBP_Menu",
  "patch": [],
  "dry_run": true,
  "confirm": false,
  "compile": true,
  "save": false
}
```

Rules:

- Patch uses stable widget names and property paths, not tree-index-only addressing.
- Mutating patch requires `dry_run=false` and `confirm=true`.
- Add/move/remove/replace operations must route through the UE 5.8 editor-safe mutation layer or an explicitly equivalent Monolith implementation.
- Slot changes must be contextual: a `Slot.*` patch is accepted only when the live parent creates the matching slot class.
- Raw property writes are not a fallback for unsupported fields. Unsupported fields must be reported, preserved, or rejected according to `compare_mode`.
- Response must include changed widgets, unsupported fields, compile result, save status, and round-trip proof when possible.

Acceptance:

- Patch can add a child, update a property, and remove a widget in fixture WBP.
- Unsupported non-spec graph bindings are reported, not deleted.

Status: initial Monolith-native P1.2 diff/patch slice landed 2026-07-02 in `MonolithUISpecActions.cpp`.

- `ui.diff_ui_spec` dumps the current WBP through `FUISpecSerializer`, validates `desired_spec`, compares stable widget names in `structural|properties|full` modes, and emits patch candidates for add/remove/move/slot/text/font-color/Image-brush/common-style/type-specific style/EffectSurface deltas.
- `ui.apply_ui_spec_patch` registers only the canonical owner action and routes explicit patch ops through existing owner actions: `ui.add_widget`, `ui.remove_widget`, `ui.move_widget`, `ui.rename_widget`, `ui.set_slot_property`, `ui.set_widget_property`, `ui.set_text`, `ui.set_image`, existing `ui.set_effect_surface_*` actions, `ui.compile_widget`, and optionally `asset.save_asset`. Explicit `replace_widget` patch ops are confirm-gated and decompose to `ui.remove_widget` + `ui.add_widget` plus the same content/style/effect owner-action routing; `preserve_children=true` decomposes to temporary `ui.add_widget`, explicit child `ui.move_widget`, old `ui.remove_widget`, and `ui.rename_widget` to restore the stable widget name. No duplicate public replace action is registered.
- Writes are dry-run by default and require `dry_run=false` plus `confirm=true`; unsupported ops are reported under `unsupported_fields` and are not converted to broad raw writes.
- `slot.anchorPreset` patch fields decompose to the existing `ui.set_anchor_preset` owner action; slot patch steps are preflighted against the live slot class so Canvas-only fields cannot be silently ignored on box/overlay slots.
- Text patching includes `fontColor` through existing `ui.set_text`; explicit empty text values are now treated as intentional writes. Image `content.brushPath` patching resolves the asset as `UTexture2D` or `UMaterialInterface` and then routes through existing `ui.set_image`. `Border.content.brushPath` uses the same resource validation and routes to existing `ui.set_brush` with `property_name=Background`; `build_ui_from_spec` / `dump_ui_spec` now round-trip the same Border brush resource. Unresolved/empty brush paths and broader native style-struct brush targets remain unsupported instead of becoming raw writes.
- `FUISpecStyle` patching now handles common `RenderOpacity` / `Visibility`, `SizeBox` width/height/min/max desired size constraints, `Border` brush color/padding, and `ProgressBar` fill color. `diff_ui_spec` emits `set_style` patch candidates for those fields, and `apply_ui_spec_patch` decomposes them to allowlist-gated `ui.set_widget_property` steps. Clearing SizeBox override flags, Border outline-only fields, and unrelated widget style structs remain unsupported evidence instead of broad raw writes.
- CommonUI `commonUI.styleRefs[0]` deltas now emit `apply_style_to_widget` patch candidates and `apply_ui_spec_patch` routes them to the existing `ui.apply_style_to_widget` owner action. Clearing CommonUI styles, changing input layer/mode metadata, and multi-style ref lists remain unsupported evidence instead of hidden graph/style writes.
- Type mismatches in `diff_ui_spec` now emit explicit `replace_widget` patch candidates with `replace_decomposition` evidence. Mutating replacement requires `confirm_replace=true`, requires the caller to supply the replacement parent, preserves children only when `preserve_children=true` and an explicit `child_widget_names[]` list is present, and expands only to existing owner actions. Diff candidates automatically offer `preserve_children=true` when current and desired direct child IDs match and the replacement class can host those children.
- `diff_ui_spec` now includes a `graph_binding_preservation` report for existing `FDelegateEditorBinding` rows. It marks property/graph bindings as `preserved_by_default` when the target widget remains in the desired spec, reports at-risk bindings when the target widget is absent, and does not synthesize hidden Blueprint graph patch ops.
- EffectSurface `effect` patching decomposes canonical `FUISpecEffect` fields to existing owner actions: `set_effect_surface_corners`, `set_effect_surface_fill` (solid), `set_effect_surface_backdropBlur`, `set_effect_surface_dropShadow`, and `set_effect_surface_innerShadow`. Default/no-op effect bags remain unsupported rather than implicitly enabling feature flags; actual provider execution keeps the existing `-32010` optional-provider contract.
- Focused automation covers dry-run planning plus a real fixture lifecycle that builds a WBP from markup/spec, adds a child, updates text/font/color content, removes a child, compiles, and verifies the resulting `dump_ui_spec` round-trip. Provider-independent dry-run tests verify EffectSurface patch decomposition, common/type-specific style decomposition, Border brush routing through `ui.set_brush`, CommonUI style owner-action routing, explicit replace decomposition, preserve-children replacement through existing owner actions, and a read-only diff fixture verifies graph-binding preservation reporting.
- Remaining P1.2 gaps: broader non-Image brush/style struct routing beyond Border and named-slot replacement fixtures.

### 6.3 UMG Animation Read/Delta Surface

Status: initial read-only P1.2 slice landed 2026-07-02 in `FMonolithUIAnimationActions`. P1.3 scalar float-key delta editing landed the same day as canonical `ui.apply_animation_delta`. These slices intentionally register only canonical Monolith owner actions (`ui.get_animation_overview`, `ui.get_animation_timeline`, `ui.get_animation_time_slice`, `ui.apply_animation_delta`). External Sequencer-style names such as `animation_overview`, `animation_widget_properties`, `animation_time_properties`, `animation_append_widget_tracks`, `animation_append_time_slice`, `animation_delete_widget_keys`, and `set_property_keys` are search metadata / response hints only, not duplicate registered actions.

Add read and delta actions inspired by external sequencer commands, but define them against UE 5.8 `UWidgetAnimation`, `UMovieScene`, `FWidgetAnimationBinding`, and generated-class delegate bindings.

| Action | Purpose |
| --- | --- |
| `ui.get_animation_overview` | Read-only compact inventory: timing, display/tick rates, `AnimationBindings`, MovieScene bindings, tracks, property paths, support flags, key counts/times, event summaries, and `UWidgetAnimationDelegateBinding` rows. |
| `ui.get_animation_timeline` | Read-only sorted key/event rows filtered by animation/widget/property. Each row includes frame/time, binding GUID, widget name, track class, property path, value type, channel name, value, interpolation/tangent data where available, and section index. |
| `ui.get_animation_time_slice` | Read-only sampled values at `time` or `times[]`. Continuous property tracks can be evaluated; event rows are reported only for exact frame matches or explicit tolerance. |
| `ui.apply_animation_delta` | Confirm-gated scalar float-key merge/upsert/delete surface for existing animations with dry-run, compile, and post-write read-back. |

Rules:

- Require `asset_path`. Require `animation_name` unless a read action sets `include_all=true`.
- Use `Animation->GetBindings()` and MovieScene binding GUIDs. Possessable names alone are not a valid identity model because widget, root, and slot bindings resolve through `FWidgetAnimationBinding`.
- Default delta behavior is merge/upsert. Never reset a whole section or replace a track unless the operation is explicitly `replace_track` and `confirm_delete=true`.
- Delete animation requires `confirm_delete=true`, variable-map cleanup, old-object rename/transient handling to avoid name collision, compile, dirty, and read-back absence.
- Delete track requires exact `track_id` or exact `(binding_guid/widget_name, property_path, track_class)` plus `confirm_delete=true`. Do not prune `FWidgetAnimationBinding` unless `prune_empty_binding=true`.
- Delete keys by exact frame through MovieScene tick resolution. For color, vector, and transform channels, collect and delete per-channel key handles; do not reuse handles across channels.
- Event tracks are master/event records, not widget property values. Runtime execution proof requires a valid endpoint/function plus successful compile; event write/delete remains deferred until that endpoint validation exists.
- `UWidgetAnimationDelegateBinding` is read back from generated-class dynamic binding data, not from MovieScene property tracks.

Property support matrix:

| Property family | UE track model | Initial Monolith stance |
| --- | --- | --- |
| Scalar float, including `RenderOpacity` | `UMovieSceneFloatTrack` | P1.2 read landed for overview/timeline/time-slice. P1.3 delta write/delete landed for exact-frame scalar float-key upsert/delete with dry-run, `confirm`, `confirm_delete`, compile, and read-back. |
| Native `RenderTransform` | `UMovieScene2DTransformTrack` with seven float channels | Future read/write/delete; do not model as `DoubleVector`. |
| `RenderTransformPivot` and true `FVector2D` properties | `UMovieSceneDoubleVectorTrack` | Future read/write/delete separately from transform. |
| `FLinearColor` | `UMovieSceneColorTrack` | Future RGBA channel read/write/delete; `FSlateColor` writes require reflection gating. |
| Object properties | `UMovieSceneObjectPropertyTrack` | Future read/time-slice first; writes require loadable object paths and matching property class. |
| Events | MovieScene event/master tracks | P1.2 overview/timeline/exact time-slice landed; writes require endpoint/function validation and compile warning. |
| Delegate bindings | `UWidgetAnimationDelegateBinding` dynamic binding rows | P1.2 overview read-back landed; writes only with generated-class binding verification. |
| Audio/material/time-warp/master tracks | Mixed MovieScene tracks | Overview-only until a separate spec owns them. |

Acceptance:

- Existing scalar-float animation keys can be read, modified, read back, and compiled without resetting existing sections or registering external Sequencer action names.
- Overview fixture contains float, native 2D transform, color, vector2D, object, event, and delegate binding data.
- Timeline read-back verifies sorted keys, channel names, interpolation/tangent data, and frame/seconds conversion.
- Time-slice output is deterministic for continuous float/vector/color/transform tracks and exact-only for events.
- Delta merge test proves adding a key does not remove existing keys.
- Replace/delete tests prove `confirm_delete` is required and only targeted scalar float keys disappear. Track/animation deletion remains future work.
- Animation delete test verifies variable-map cleanup, compile, and recreate-with-same-name behavior.
- Round-trip dump preserves unsupported tracks or reports them explicitly; it never silently drops them.

### 6.4 `workflow.ui_bind_widget_event`

Initial ViewModel-command event-binding workflow landed 2026-07-02 as `workflow.ui_bind_widget_event`. This is intentionally a workflow, not a raw `ui` or external-name Blueprint action. It composes existing Blueprint graph owner actions and adds only the UI-specific ViewModel boundary, confirm gate, action plan, compile/read-back envelope, and follow-up proof routing.

Proposed params:

```json
{
  "asset_path": "/Game/UI/WBP_Menu",
  "widget_name": "StartButton",
  "event": "OnClicked",
  "intent": {
    "kind": "viewmodel_command",
    "viewmodel_variable": "ViewModel",
    "command": "StartGame"
  },
  "dry_run": true,
  "confirm": false,
  "compile": true
}
```

Rules:

- `intent.kind` v1 accepts only `viewmodel_command`.
- `intent.viewmodel_variable` and `intent.command` are required.
- Direct gameplay Actor/Pawn/Controller/component targets are rejected for runtime UI instead of being routed into a hidden graph mutation.
- The workflow reports the underlying `blueprint.resolve_node`, `blueprint.add_node`, `blueprint.connect_pins`, `blueprint.compile_blueprint`, and `blueprint.get_graph_summary` steps in `actions[]`.
- `dry_run=false` requires `confirm=true`; otherwise the workflow returns a blocked envelope and does not execute graph mutation.
- Confirmed execution may infer exec/value pins from child action results; ambiguous graphs can supply `intent.event_exec_pin`, `intent.command_exec_pin`, `intent.viewmodel_value_pin`, and `intent.command_target_pin`.
- Follow-up proof remains explicit: `ui.dump_blueprint_compile_log`, `workflow.ui_shipping_widget_blueprint`, and `asset.save_asset` are exposed as `next_actions[]`.

Acceptance:

- Contract fixture verifies registration, dry-run graph plan, ViewModel boundary decision, event-name normalization, child-action visibility, confirm blocking, and direct actor rejection.
- Apply fixture creates/updates the event binding, compiles, and reports read-back proof before this slice is considered fully covered by asset-level automation.
- Negative fixture rejects direct actor access.

## 7. P1/P2 Specs

### 7.1 UI-Domain Material / Retainer Workflows

Do not port external material graph actions into `ui`. Instead add Monolith workflows that use the existing `material` namespace and return UI binding proof.

Initial Image/brush-bound UI material HLSL workflow landed 2026-07-02 as `workflow.ui_material_hlsl_effect`. This is a workflow-only composition layer: low-level graph writes remain in `material`, widget binding remains in `ui`, visual proof remains in `workflow.ui_shipping_widget_blueprint`, and no `hlsl` namespace or external `material_*` aliases are registered.

Proposed workflow:

```json
{
  "workflow": "workflow.ui_material_hlsl_effect",
  "material_path": "/Game/UI/Materials/M_ButtonGlow",
  "hlsl": "return float4(Color.rgb * Glow, Color.a);",
  "parameters": [
    {"name": "Glow", "type": "scalar", "default": 1.0}
  ],
  "bind_to": {
    "asset_path": "/Game/UI/WBP_Menu",
    "widget_name": "GlowImage",
    "property_name": "Brush"
  },
  "dry_run": true,
  "confirm": false
}
```

Retainer effect variant:

```json
{
  "workflow": "workflow.ui_retainer_effect_material",
  "material_path": "/Game/UI/Materials/M_RetainerBlur",
  "bind_to": {
    "asset_path": "/Game/UI/WBP_Menu",
    "retainer_widget_name": "MenuRetainer",
    "texture_parameter": "Texture"
  },
  "dry_run": true,
  "confirm": false
}
```

Rules:

- Material domain must be `MD_UI`.
- Prefer graph nodes. Use a Custom HLSL node only when graph nodes cannot express the requested effect safely.
- UI material outputs must connect through `EmissiveColor` and `Opacity` / `OpacityMask` according to blend mode.
- `UImage`/brush binding may accept a `UMaterialInterface`, but proof must fail or warn when the material is not UI-domain.
- RetainerBox defaults the texture parameter to `Texture`; the workflow must verify the exact parameter name on the effect material instead of assuming `RetainerTexture`.
- Custom-node proof must include input/output names, output type, additional outputs, defines/includes, sampler count, instruction count when available, and warnings for risky tokens such as `clip`, `discard`, `ddx`, `ddy`, invalid names, and texture sampling without enough metadata.
- Workflow success requires all proof slices: `material_proof`, `hlsl_proof`, `binding_proof`, `widget_proof`, and visual/perf proof when the workflow is visual or performance-motivated.
- RetainerBox and InvalidationBox must not be auto-inserted as optimizations without a measured before/after profile. For widget-local parameters, per-widget MIDs are valid and should not be replaced by mesh-oriented CPD guidance.

Landed v1 behavior:

- `workflow.ui_material_hlsl_effect` accepts `material_path`, `hlsl`, `parameters[]`, `expression_name?`, `create_material=false`, UI material defaults (`material_domain=UI`, `blend_mode=Translucent`, `shading_model=Unlit`), and `bind_to`.
- Dry-run reports every child owner action in `actions[]`: `material.create_material` when requested, `material.set_material_property`, `material.create_custom_hlsl_node` or `material.update_custom_hlsl_node`, `material.connect_expressions`, optional `material.build_material_graph(clear_existing=false)` for ComponentMask alpha-to-opacity wiring, `material.recompile_material`, `material.validate_material`, `material.get_compilation_stats`, `material.get_material_properties`, `material.get_full_connection_graph`, `ui.set_image` or `ui.set_brush`, `ui.compile_widget`, `ui.dump_blueprint_compile_log`, `ui.audit_widget_material_lifecycle`, and optional `workflow.ui_shipping_widget_blueprint`.
- HLSL proof reports parameter descriptors, HLSL identifier validity, `Float4` return assumption, and warnings for risky tokens such as `clip`, `discard`, `ddx`, `ddy`, `fwidth`, `SceneTexture`, and texture sampling without enough metadata.
- Confirmed runs require `dry_run=false` and `confirm=true`. Existing material round-trips are the default; new material creation requires `create_material=true` so existing assets do not fail on accidental duplicate creation.
- v2 connects the Custom node to `EmissiveColor` by default. `connect_opacity=true` still uses an explicit compatible `opacity_output_pin` when a matching Custom additional output exists; otherwise the workflow now selects `auto_component_mask_alpha` and composes `material.build_material_graph(clear_existing=false)` to insert a `ComponentMask` node that routes the float4 output alpha channel to `Opacity`. The result includes `validation.opacity_wiring` with `mode=component_mask|direct_custom_output|not_requested`.
- `run_widget_proof=false` leaves `workflow.ui_shipping_widget_blueprint(proof_profile="visual")` as an explicit next action; `run_widget_proof=true` composes it after binding.
- Real fixture coverage landed in `Monolith.UI.MaterialWorkflow.UiMaterialHlslEffectRealFixture`: it creates/reuses a generated WBP with a `UImage`, creates/reuses a UI-domain `UMaterial`, resets the material graph through `material.build_material_graph(clear_existing=true)`, runs `workflow.ui_material_hlsl_effect(dry_run=false, confirm=true, connect_opacity=true)`, and reads back both `material.get_full_connection_graph` and the `UImage` brush resource. The fixture proves that the Image brush references the workflow material and that the material `Opacity` output is driven by `MaterialExpressionComponentMask`.
- Retainer fixture coverage landed in `Monolith.UI.MaterialWorkflow.UiRetainerEffectRealFixture`: it creates/reuses a generated WBP with a `URetainerBox`, creates/reuses a UI-domain effect `UMaterial`, builds an exact `Texture` parameter through `material.build_material_graph`, runs `workflow.ui_retainer_effect_material(dry_run=false, confirm=true)`, reads back the RetainerBox effect material and texture parameter, and verifies that a mismatched texture parameter returns `RetainerEffectParameterMismatch` from `ui.set_retainer_effect_material`.
- `ui.audit_widget_material_lifecycle` landed as the read-only owner lint for Widget Blueprint dynamic-material lifetime. It scans WBP K2 graphs for `UMaterialInstanceDynamic::Create`, `CreateDynamicMaterialInstance`, `GetDynamicMaterial`, and related MID creation calls; reports `DynamicMaterialCreatedInRepeatedLifecycle` when the call is reachable from Tick/Paint/SynchronizeProperties/Prepass paths; and leaves intentional/safe initialization sites as warning/advisory review findings. `workflow.ui_material_hlsl_effect` and `workflow.ui_retainer_effect_material` both compose this owner action instead of adding material/HLSL duplicate APIs.

Deferred:

- `workflow.ui_retainer_effect_material` v1 landed 2026-07-02. It composes `material.get_material_parameters`, `material.get_material_properties`, Retainer-specific `ui.set_retainer_effect_material`, widget compile/log proof, and optional `workflow.ui_shipping_widget_blueprint`; it does not rely on raw `ui.set_widget_property`.
- Retainer/invalidation before-after performance proof remains pending. `workflow.ui_material_hlsl_effect` must not claim visual artifact proof unless `workflow.ui_shipping_widget_blueprint` actually runs, and `workflow.ui_retainer_effect_material` must not claim Retainer runtime/perf proof until measured.

Acceptance:

- Creates/updates a UI-domain material in a fixture, compiles it, binds it to an Image brush, and verifies graph/widget read-back. Landed: real Image/WBP/material fixture proof for `workflow.ui_material_hlsl_effect`, including automatic alpha ComponentMask execution and brush resource read-back. Still pending: visual artifact output verification through `workflow.ui_shipping_widget_blueprint`.
- Binding a Surface-domain material to an Image reports `MaterialDomainMismatch`.
- Retainer workflow contract and real fixture landed; fixture creates a real RetainerBox WBP and UI-domain effect material where the exact texture parameter passes and a mismatch fails.
- Custom HLSL fixture reports sampler/instruction stats and unsafe-token warnings.
- DMI lifecycle lint landed as `ui.audit_widget_material_lifecycle`: it proves no dynamic material instance creation call is statically reachable from repeated Widget Blueprint lifecycle paths such as Tick/paint/SynchronizeProperties/Prepass.
- Retainer/invalidation perf proof is required before the workflow recommends those widgets for optimization.

### 7.2 UI Task Profiles

Borrow the external prompt manager's "enable only relevant tools" idea, but implement it through Monolith's existing profile/skill/guidance surfaces.

Candidate profiles:

- `ui_authoring_minimal`: `ui`, `asset`, `editor.capture_scene_preview`, `workflow.ui_shipping_widget_blueprint`.
- `ui_animation`: `ui` animation actions plus visual proof.
- `ui_common_frontend`: CommonUI/CommonGame validators, menu transform, focus/navigation.
- `ui_material_effect`: material UI workflow + UI proof.

Acceptance:

- Add profile guidance only if invocation logs show repeated UI action confusion.
- No prompt-manager web UI, no editable external prompt JSON.

## 8. Non-Candidates

Do not port:

- `UmgMcp` sidecar socket/MCP bridge, Python server, `uv` environment, Gemini wrappers, debug wrappers, prompt web app.
- Hidden active-target fallback for mutating Monolith actions.
- External-name compatibility aliases that merely duplicate stronger Monolith actions.
- Raw `apply_json_to_umg` direct mutation outside `FUISpecDocument` validation.
- Stale or broken external contracts as-is: `set_active_widget`, `set_animation_data`, `material_get_graph`, external `reparent_widget`, external `move_widget`, and duplicate `add_step` registration.
- A separate `hlsl` namespace for UI material editing; material Custom-node editing already belongs to `material`.
- Generic level actor helpers (`spawn_actor`, `get_actors_in_level`) already covered by Monolith scene/editor namespaces.
- Generic Blueprint/component/material CRUD already covered by Monolith domain namespaces, except where a UI workflow composes them with UMG-specific proof.
- World-building helper scripts under `Resources\Python\helpers`; unrelated to UMG.

## 9. Implementation Order

1. **P0.1:** `ui.describe_widget_type_schema` (initial implementation landed 2026-07-02; live-slot/unsafe edge fixtures still pending).
2. **P0.2:** Explicit `ui` work context helpers (initial implementation landed 2026-07-02 as `set_widget_context`, `get_widget_context`, `clear_widget_context`; active-editor inference remains intentionally outside `ui` and should compose `editor` owner actions if needed).
3. **P0.3:** `ui.measure_widget_layout`, overlap, safe-zone checks (initial authored-model implementation and `workflow.ui_shipping_widget_blueprint` visual/runtime proof composition landed 2026-07-02; future owned virtual-window render/prepass geometry still pending).
4. **P0.4:** Expand `ui.audit_widget_layout` with guide-derived production lints.
5. **P0.5:** UE 5.8 editor-safe mutation layer for existing widget mutations and future spec patch apply (`ui.move_widget` Canvas/box/overlay slot-preserving slice landed 2026-07-02; `ui.apply_ui_spec_patch` now routes add/remove/move/explicit replace/preserve-children replace/slot/property/text/image/Border brush/common style/type-specific style/CommonUI style/EffectSurface changes through existing owner actions and preflights slot-class compatibility plus preserve-child direct-parent invariants before executing patch steps; named-slot and broader rename/remove consistency remain pending).
6. **P1.1:** `ui.convert_markup_to_ui_spec` read-only parser (initial implementation, build/dump round-trip fixture, and parent-slot contextual validation landed 2026-07-02).
7. **P1.2:** `ui.diff_ui_spec` / `ui.apply_ui_spec_patch` for existing-WBP design-data deltas (initial stable-name diff + confirm-gated add/remove/move/slot/property/text patch slice landed 2026-07-02; slot `anchorPreset` decomposition, live slot-class preflight, image brush routing, Border brush routing via `ui.set_brush`, EffectSurface owner-action decomposition, common `RenderOpacity`/`Visibility` style routing, SizeBox/Border/ProgressBar type-specific style routing, CommonUI style candidates, graph-binding preservation reports, explicit replace decomposition, and preserve-children replacement landed in follow-up slices; broader non-Image brush/style struct routing and named-slot replacement fixtures remain pending).
8. **P1.3:** `ui.get_animation_overview` / timeline / time-slice read actions (initial float/event/delegate overview slice landed 2026-07-02; native transform/color/vector/object read expansion remains pending).
9. **P1.4:** `ui.apply_animation_delta` (initial scalar float-key delta slice landed 2026-07-02; event/transform/color/vector/object deltas remain deferred).
10. **P1.5:** `workflow.ui_bind_widget_event` (initial ViewModel-safe workflow contract landed 2026-07-02; real-WBP apply/read-back fixture remains the next hardening step).
11. **P1.6:** UI-domain material/Retainer proof workflows using existing `material` primitives (`workflow.ui_material_hlsl_effect` v2 landed 2026-07-02 for Custom-HLSL UI material + Image/brush binding proof plus automatic float4 alpha ComponentMask-to-Opacity planning/execution through `material.build_material_graph`; real Image/WBP/material fixture proof landed 2026-07-02; `workflow.ui_retainer_effect_material` v1 landed 2026-07-02 for exact Retainer texture-parameter proof; real RetainerBox/effect-material fixture proof landed 2026-07-02; `ui.audit_widget_material_lifecycle` DMI lifecycle lint landed and is composed by both workflows; Retainer/invalidation perf proof remains pending).
12. **P2.1:** UI task profiles only after log evidence.

## 10. Target Files

Likely implementation files:

- `Source/MonolithUI/Private/MonolithUIActions.cpp` - existing widget CRUD/search metadata and visual verifier integration.
- `Source/MonolithUI/Private/MonolithUISlotActions.cpp` - `ui.move_widget` and slot-preservation upgrade.
- `Source/MonolithUI/Private/Actions/MonolithUIContextActions.cpp` - explicit UI work context helpers.
- `Source/MonolithUI/Private/Actions/MonolithUISpecActions.cpp` - markup/spec conversion, spec diff/patch, layout audit expansion.
- `Source/MonolithUI/Private/Registry/UITypeRegistry.cpp` and `UIPropertyAllowlist.cpp` - type schema and safe property metadata.
- `Source/MonolithUI/Private/MonolithUIAnimationActions.cpp` - animation read inspection actions and legacy v1 animation surface.
- `Source/MonolithUI/Private/Actions/Hoisted/AnimationCoreActions.cpp` - canonical v2 animation writers and future delta helpers when shared with hoisted animation authoring.
- `Source/MonolithUI/Private/CommonUI/MonolithCommonUIButtonActions.cpp` - reuse existing style/theme actions before adding any native UMG style gap filler.
- `Source/MonolithMaterial/Private/MonolithMaterialActions.cpp` - reuse material Custom-node and compile primitives for UI material workflows.
- `Source/MonolithCore/Private/MonolithWorkflowActions.cpp` - `workflow.ui_bind_widget_event`, UI material/Retainer proof workflows, and proof composition.
- `Source/MonolithUI/Private/Tests/*` - deterministic fixtures for schema, layout, lint, markup, animation, and event workflows.
- `Docs/specs/SPEC_MonolithUI.md`, `Skills/unreal-ui/SKILL.md`, `Docs/MONOLITH_GUIDE.md` - public docs and skill examples after implementation.

## 11. Verification Requirements

Every implementation slice must run:

- `git diff --check`
- `python Scripts/ci_static_checks.py --config .github/monolith-static-ci.json --github check`
- UBT build for C++ changes.
- Focused automation tests for the added action/workflow.
- Duplicate audit: inspect current registry/source/docs for an existing owner action before registering any new action, and document the classification from section 3.1.

Additional fixture requirements:

- Generated WBP fixtures must not depend on project sample UI assets.
- Geometry tests should include at least desktop and mobile/safe-zone profiles.
- Runtime/event-binding proof must not claim success unless the relevant compile/read-back or PIE step actually executed.
- Workflow tests must assert that low-level operations are reported as underlying `ui`, `blueprint`, `material`, `editor`, or `asset` actions instead of hidden side effects.
- Suggested benchmark additions: `widget_layout_snapshot_overlap`, `widget_simple_layout_import`, `widget_animation_timeline_edit`, `ui_hlsl_material_brush`.

## 12. Done Criteria

The port is successful when agents can:

1. Ask what properties are safe to set on a widget type before writing.
2. Build or modify a widget through canonical Monolith specs.
3. Prove layout quality structurally and geometrically.
4. Verify visual artifacts from widget captures.
5. Read and safely patch UMG animations.
6. Bind common widget events through a ViewModel-safe workflow.
7. Produce UI material effects only through Monolith material/UI workflows with compile and widget binding proof.

## Appendix A. External Action Inventory Disposition

| External group | External actions | Monolith disposition |
| --- | --- | --- |
| UMG context | `get_target_umg_asset`, `set_target_umg_asset`, `get_target_widget`, `set_target_widget`, `get_last_edited_umg_asset`, `get_recently_edited_umg_assets` | Adapt as explicit `ui` context helpers only. No hidden mutating fallback. |
| Widget read | `get_widget_schema`, `get_creatable_widget_types`, `get_widget_tree`, `query_widget_properties`, `get_layout_data`, `check_widget_overlap` | Adapt schema guidance and geometry measurement. Keep existing tree/type actions. Replace weak overlap action with proof-grade measurement. |
| Widget write | `create_widget`, `set_widget_properties`, `delete_widget`, `reparent_widget`, `save_asset` | Do not port directly. Existing Monolith actions are stronger. Consider a future replace/wrap workflow only with dry-run and slot/child preservation proof. |
| JSON/layout | `export_umg_to_json`, `apply_json_to_umg`, `apply_html_to_umg`, `apply_layout` | Adapt into `dump_ui_spec`, `convert_markup_to_ui_spec`, `diff_ui_spec`, and `apply_ui_spec_patch`. No direct raw JSON mutation. |
| Animation read/write | `get_all_animations`, `get_animation_keyframes`, `get_animated_widgets`, `get_animation_full_data`, `get_widget_animation_data`, `animation_widget_properties`, `animation_time_properties`, `animation_overview`, `create_animation`, `delete_animation`, `set_property_keys`, `remove_property_track`, `remove_keys`, `animation_append_widget_tracks`, `animation_append_time_slice`, `animation_delete_widget_keys` | Keep Monolith v2 animation creation. Add overview/timeline/time-slice reads and confirm-gated delta edits. |
| Blueprint workflow | `compile_blueprint`, `set_edit_function`, `set_cursor_node`, `add_step`, `prepare_value`, `connect_data_to_pin`, `get_function_nodes`, `add_variable`, `delete_variable`, `get_variables`, `delete_node`, `search_function_library` | Do not duplicate generic Blueprint editing in `ui`. Add `workflow.ui_bind_widget_event` that composes Blueprint actions with UI/ViewModel policy. |
| Material/HLSL | `material_set_target`, `material_define_variable`, `material_add_node`, `material_delete`, `material_connect_nodes`, `material_connect_pins`, `material_set_hlsl_node_io`, `material_set_node_properties`, `material_compile_asset`, `material_get_pins`, `material_get_graph`, `hlsl_set_target`, `hlsl_get`, `hlsl_set`, `hlsl_compile` | Use existing Monolith `material` primitives (`create_material`, `build_material_graph`, `create_custom_hlsl_node`, `update_custom_hlsl_node`, `connect_expressions`, `set_expression_property`, `recompile_material`, graph/pin introspection). Do not add a separate `hlsl` namespace. Add UI-domain workflows only where they return material compile and widget binding proof. |
| Style/theme | `set_widget_style`, `apply_global_theme`, `style_create_asset` | Reuse existing CommonUI style/theme actions (`create_common_button_style`, `create_common_text_style`, `create_common_border_style`, `apply_style_to_widget`, `batch_retheme`). Add native UMG style actions only for gaps not covered by CommonUI and only with reflected style-schema proof. |
| Level/system | `refresh_asset_registry`, `get_actors_in_level`, `spawn_actor`, `list_assets` | Do not port into UI. Existing asset/scene/editor/project actions own this surface. |
| Prompt/profile web UI | Prompt Manager and `prompts.json` enable flags | Do not port web UI. Use Monolith skills, action discovery, tool profiles, and invocation-log-driven guidance. |
