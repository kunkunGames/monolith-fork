---
name: unreal-ui
description: Use when authoring runtime Unreal UMG UI via Monolith MCP (ui namespace) — Widget Blueprints, HUDs, menus, settings/save panels, widget tree/slots/anchors, styling, animations, data binding, CommonUI, accessibility, and ViewModel-boundary UI. For actor/component Blueprint graphs use unreal-blueprints; for editor Slate / Editor Utility Widgets use unreal-slate; the GAS attribute feeding a health bar is authored in unreal-gas then bound here; for string-table/FText UI text use unreal-localization; for font/Texture2D ingest use unreal-asset. Triggers on UI, UMG, widget, Widget Blueprint, WBP, HUD, health bar, make a health bar, menu, pause menu, settings panel, save game, button, list view, anchor, brush, font, dialog, loading screen, inventory grid, CommonUI, accessibility, ViewModel.
---

# Unreal UI Workflows

Drives the **`ui`** namespace via `ui_query()` to author runtime UMG: Widget Blueprints, HUDs, menus, settings/save scaffolds, widget tree/slots/anchors, styling, animations, data binding, CommonUI, and accessibility. The tables below are a curated snapshot, not the full live catalog.

## Discovery

Always confirm the live action set and an action's exact parameter schema before calling — the registry is the source of truth, not these tables:

```
monolith_discover({ namespace: "ui" })                                  // list live actions
monolith_discover({ namespace: "ui", category: "CommonUI" })            // filter to CommonUI-conditional actions
monolith_discover({ namespace: "ui", category: "CommonFramework" })     // Lyra Common plugin diagnostics
monolith_discover({ namespace: "ui", action: "add_widget", mode: "schema" })  // exact params for one action
monolith_find("make a health bar HUD")                                  // jump straight to the right action
```

The `ui` live catalog is large and build-flag dependent; confirm the exact count with `monolith_discover`. Always-on UMG authoring plus CommonUI-plugin-conditional actions plus GAS attribute-binding aliases are available in the typical Speed editor build (post Phase A-L MonolithUI architecture expansion; texture/font ingest moved to `asset`). Always-on surface includes Widget CRUD, explicit work context (`set_widget_context`, `get_widget_context`, `clear_widget_context`), UIExtension point setup, CommonFramework diagnostics/authoring (`get_common_framework_status`, `add_primary_game_layout_layer`, `describe_common_widget_blueprint`, `describe_common_messaging_flow`, `validate_common_dialog_contract`, `validate_common_layer_push_contract`, `validate_frontend_menu_flow`), Slot, Templates, Styling, UI post-copy repair, Animation v1/v2 plus read-only overview/timeline/time-slice inspection, Bindings, Settings scaffolds, Accessibility, Hoisted Design Import effects, EffectSurface, Spec Builder (`build_ui_from_spec`, `convert_markup_to_ui_spec`, `diff_ui_spec`, `apply_ui_spec_patch`, `measure_widget_layout`, `build_menu_from_spec`, `apply_common_menu_transform_spec`), and Type Registry diagnostics (`describe_widget_type_schema`, `dump_property_allowlist`, enum/property helpers).

**CommonUI actions require the CommonUI engine plugin** (stock UE 5.7, `Engine/Plugins/Runtime/CommonUI/`). When absent, the `WITH_COMMONUI` action pack unregisters; detect the live surface via `monolith_discover`.

## When to use / Use a different skill for

Use **unreal-ui** for runtime UMG widgets — Widget Blueprints, HUDs, menus, settings/save panels, widget tree CRUD, slots/anchors, styling, animations, animation read/delta evidence, data binding, CommonUI, and accessibility on the UI side.

- **unreal-blueprints** — the graph is an actor/component Blueprint or generic Blueprint variables/functions, not a Widget Blueprint / UMG layout.
- **unreal-slate** — the UI is editor Slate / an Editor Utility Widget or editor-side tooling, not a runtime UMG widget/HUD/menu.
- **unreal-gas** — author the GameplayAbility/AttributeSet/GameplayEffect that feeds a HUD in GAS; bind the resulting attribute (health/mana) to the widget here.
- **unreal-localization** — the work is string-table/FText localization of UI text, versus widget layout, styling, or binding here.
- **unreal-asset** — ingesting a font (TTF/OTF) or Texture2D used by UI (moved out of the `ui` namespace), versus constructing the widget that consumes it.

## Key Parameters

- `asset_path` -- Widget Blueprint path (e.g. `/Game/UI/WBP_MyWidget`)
- `save_path` -- destination for new WBP assets
- `widget_name` / `widget_class` -- widget name in tree / type (`TextBlock`, `Image`, `Button`, etc.)
- `parent_name` -- parent panel (omit for root)
- `anchor_preset` -- `center`, `top_left`, `stretch_fill`, etc.

## Project UI Architecture Rules

When creating or modifying UMG widgets, Widget Blueprints, HUDs, menus, UI binding code, or UI-facing Blueprint/C++ APIs, keep UI code behind a ViewModel boundary.

- Widgets must not directly reach into gameplay `Actor`, `Pawn`, `Controller`, component, subsystem, or domain/model objects to pull mutable gameplay state.
- Expose UI-facing state through an explicit ViewModel object or ViewModel-like adapter owned by the UI flow; bind widget text, visibility, enabled state, lists, and progress values to that ViewModel.
- Route user intent from widgets to the ViewModel first. The ViewModel may translate it into gameplay commands, service calls, or events; widgets should not call gameplay actors/models directly.
- Gameplay/domain changes should notify the ViewModel through delegates, events, or observer-style subscriptions. Widgets subscribe to ViewModel-facing notifications, not to arbitrary Actor/model delegates.
- If an existing widget currently binds directly to an Actor/model, new work should move the touched path toward ViewModel mediation instead of expanding the direct dependency.
- Blueprint and C++ APIs exposed for UI must preserve this boundary: public UI entry points take ViewModels, interfaces, or UI DTOs rather than raw gameplay Actors/models unless the asset's purpose is actor picking/inspection tooling.

## Action Reference

Param notation: `name*` required, `name?` optional, `name=val` default, `a/b/c` allowed values, `[w]` mutates (transaction-wrapped). Accepted aliases noted inline as `name*` (alias `other`). Signatures are a snapshot of the live catalog — for the exact full schema call `monolith_discover({ namespace: "ui", action: "<action>", mode: "schema" })`. The discover-first block above remains the authority.

| Action | Key Params | Purpose |
|--------|-----------|---------|
| **Widget CRUD (7)** | | |
| `[w] create_widget_blueprint` | `save_path*`, `parent_class=UserWidget`, `root_widget=CanvasPanel`, `skip_save=false` | Create new WBP |
| `get_widget_tree` | `asset_path*` | Full hierarchy as JSON |
| `[w] add_widget` | `asset_path*`, `widget_class*`, `widget_name?`, `parent_name?`, `anchor_preset?`, `position?`, `size?`, `padding?`, `h_align?` (Left/Center/Right/Fill), `v_align?` (Top/Center/Bottom/Fill), `auto_size=false`, `compile=true` | Add widget to panel |
| `[w] remove_widget` | `asset_path*`, `widget_name*`, `compile=true` | Remove from tree |
| `[w] set_widget_property` | `asset_path*`, `widget_name*`, `property_name*`, `value*` (alias `property_value`), `compile=false`, `raw_mode=false` | Set any UPROPERTY (allowlist-gated unless `raw_mode`) |
| `[w] compile_widget` | `asset_path*` | Compile WBP (returns errors[]/warnings[]) |
| `list_widget_types` | `filter?` (panel/leaf/input/display/layout) | Available widget classes |
| `[w] rename_widget` | `wbp_path*` (alias `asset_path`), `old_name*`, `new_name*` | Rename widget FName (must be unique) |
| `[w] dump_blueprint_compile_log` | `asset_path*` | Fresh compile + errors[]/warnings[]/notes[] |
| **Context (3, diagnostic only)** | | |
| `set_widget_context` | `asset_path?`, `widget_name?`, `animation_name?`, `scope=session/request`, `ttl_seconds=900` | Remember explicit UMG focus for this session/request; never auto-creates assets and never becomes a hidden write default |
| `get_widget_context` | `scope=auto/session/request`, `include_recent=true`, `recent_limit=5` | Read context with freshness and asset/widget/animation existence; result includes `usable_as_default_for_mutation=false` |
| `clear_widget_context` | `scope=all/session/request` | Clear current session/request UI context without touching assets |
| `[w] add_widget_variable` | `wbp_path*` (alias `asset_path`), `var_name*`, `var_type*` (token grammar — see action desc), `default_value?`, `var_category?` | Add member variable to WBP |
| `[w] set_widget_is_variable` | `wbp_path*` (alias `asset_path`), `widget_name*`, `is_variable*` | Toggle widget's bIsVariable flag |
| `list_widget_property_enums` | `wbp_path?`, `widget_class?`, `property_name?` | Enum-typed props + valid values |
| `[w] dump_property_allowlist` | `widget_type*` | Allowed property paths for a widget type |
| **UIExtension Points (1)** | | |
| `[w] add_extension_point_widget` | `asset_path*`, `widget_name*`, `extension_point_tag*`, `widget_class?=/Script/UIExtension.UIExtensionPointWidget`, layout params, `compile=true`, `save=false` | Add/update a `UUIExtensionPointWidget`-compatible widget, assign a registered GameplayTag, and keep Canvas/box/overlay slot layout idempotent without hard-linking UIExtension. |
| **CommonFramework (7)** | | |
| `get_common_framework_status` | `include_properties?=false`, `include_functions?=false`, `property_limit?=40`, `function_limit?=80` | Reflected availability/status for CommonUI, CommonGame, UIExtension, CommonUser, CommonLoadingScreen, GameSettings, GameplayMessageRouter, ModularGameplayActors, and GameSubtitles. |
| `[w] add_primary_game_layout_layer` | `asset_path*`, `layer_tag*`, `widget_name?`, `widget_class?=/Script/CommonUI.CommonActivatableWidgetStack`, layout params, `compile=true`, `save=false` | Add/update a CommonActivatable layer container widget inside a PrimaryGameLayout WBP. Returns the `RegisterLayer` tag/widget pair; it does not modify CommonGame runtime code. |
| `describe_common_widget_blueprint` | `asset_path*`, `include_extension_points?=true`, `include_layer_candidates?=true`, `include_widget_tree?=false` | Inspect PrimaryGameLayout parentage, UIExtension point tags, and CommonActivatable layer candidates. |
| `describe_common_messaging_flow` | `messaging_class?`, `config_section?`, `modal_layer_tag?=UI.Layer.Modal`, `include_subclasses?=true`, `subclass_limit?=40` | Describe CommonGame messaging wiring by reflection/config only: selected CommonMessagingSubsystem subclass, dialog classes, modal layer tag, DefaultUIPolicyClass, and loaded subclasses. |
| `validate_common_dialog_contract` | `messaging_class?`, `config_section?`, `confirmation_dialog_class?`, `error_dialog_class?` | Validate configured or explicit confirmation/error dialog classes are loadable concrete CommonGameDialog subclasses. |
| `validate_common_layer_push_contract` | `layout_asset_path?`, `layer_tag?=UI.Layer.Modal`, `layer_widget_name?`, `dialog_class?`, `require_layout_asset?=false` | Validate modal-layer push readiness without editing PrimaryGameLayout: tag registration, optional layout WBP layer candidates, dialog class compatibility, and proof limits for RegisterLayer graph wiring. |
| `validate_frontend_menu_flow` | `layout_asset_path?`, `required_layers?`, `screens?`, `modal_layer_tag?=UI.Layer.Modal`, `dialog_class?`, `require_layout_asset?=false`, `require_dialog?=false`, `include_graph_scan?=true` | Validate a Lyra/CommonUI frontend menu flow contract read-only: optional PrimaryGameLayout layer candidates, dialog class compatibility, per-screen CommonActivatable parentage, required/forbidden widgets, expected widget classes/variables, desired focus, and graph text needles. |
| **Slot & Layout (3)** | | |
| `[w] set_slot_property` | `asset_path*`, `widget_name*`, `anchors?`, `offsets?`, `position?`, `size?`, `alignment?`, `z_order?`, `auto_size?`, `h_align?`, `v_align?`, `padding?`, `compile=false` | Any slot property |
| `[w] set_anchor_preset` | `asset_path*`, `widget_name*`, `preset*` (top_left/top_center/top_right/center_left/center/center_right/bottom_left/bottom_center/bottom_right/stretch_horizontal/stretch_vertical/stretch_fill/stretch_top/stretch_bottom/stretch_left/stretch_right), `compile=false` | Named anchor preset |
| `[w] move_widget` | `asset_path*`, `widget_name*`, `new_parent_name*`, `compile=true` | Reparent widget |
| **Templates (8)** | | |
| `[w] create_hud_element` | `asset_path*`, `element_type*` (crosshair/health_bar/ammo_counter/stamina_bar/interaction_prompt/damage_indicator/compass/subtitles/flashlight_battery), `widget_name_prefix?`, `compile=true` | Pre-built HUD element |
| `[w] create_menu` | `save_path*`, `menu_type*` (main_menu/pause_menu/death_screen/credits), `buttons?` (array of label strings) | Menu WBP |
| `[w] create_settings_panel` | `save_path*`, `tabs?` (graphics/audio/controls/gameplay/accessibility — default all) | Tabbed settings panel |
| `[w] create_dialog` | `save_path*`, `title=Confirm`, `body="Are you sure?"`, `confirm_text=Yes`, `cancel_text=No` | Confirmation dialog |
| `[w] create_notification_toast` | `save_path*`, `position=top_right` (top_right/bottom_right/top_left/bottom_left/top_center) | Toast widget |
| `[w] create_loading_screen` | `save_path*`, `show_progress=true`, `show_tips=true`, `show_spinner=true` | Loading screen |
| `[w] create_inventory_grid` | `save_path*`, `columns=5`, `rows=4`, `slot_size=64` | Inventory grid |
| `[w] create_save_slot_list` | `save_path*`, `max_slots=3` | Save slot selector |
| **Styling (7)** | | |
| `[w] set_brush` | `asset_path*`, `widget_name*`, `property_name*`, `draw_type=Image` (Image/Box/Border/RoundedBox/NoDrawType), `tint_color?`, `image_size?`, `margin?`, `corner_radius?`, `outline_color?`, `outline_width?`, `texture_path?`, `material_path?`, `compile=false` | Configure brush |
| `[w] set_font` | `asset_path*`, `widget_name*`, `font_size?`, `font_family?`, `typeface=Regular` (Regular/Bold/Italic/Light), `letter_spacing?`, `outline_size?`, `outline_color?`, `compile=false` | Font on text widgets |
| `[w] set_color_scheme` | `colors*` (map e.g. {"User1":"#0A0A14",...}) | EStyleColor User1-16 palette |
| `[w] batch_style` | `asset_path*`, `widget_class*`, `property_name*`, `value*`, `compile=false` | Apply to all widgets of class |
| `[w] set_text` | `asset_path*`, `widget_name*`, `text?`, `text_color?`, `font_size?`, `justification?` (Left/Center/Right), `compile=false` | Convenience text setter |
| `[w] set_image` | `asset_path*`, `widget_name*`, `texture_path?`, `material_path?`, `tint_color?`, `size?`, `compile=false` | Convenience image setter |
| `[w] set_retainer_effect_material` | `asset_path*`, `widget_name*`, `material_path*`, `texture_parameter=Texture`, `request_render=false`, `compile=false` | RetainerBox effect material binding with exact texture-parameter/UI-domain proof |
| `[w] set_rounded_corners` | `asset_path*`, `widget_name*`, `corner_radii?` [TL,TR,BR,BL], `outline_color?`, `outline_width?`, `fill_color?`, `compile=true` | Reflection writer for corner/outline/fill (≥1 optional required) |
| **Spec Builder + Menu Transform (10)** | | |
| `[w] build_ui_from_spec` | `asset_path*`, `spec*`, `overwrite=true`, `dry_run=false`, `treat_warnings_as_errors=false`, `raw_mode=false`, `request_id?` | Transactional canonical `FUISpecDocument` to Widget Blueprint builder. |
| `dump_ui_spec_schema` | none | Read the canonical `FUISpecDocument` schema plus live allowlist projection. |
| `convert_markup_to_ui_spec` | `markup*`, `dialect=umg_xml_v1`, `strict=true`, `root_save_path?`, `spec_name?`, `parent_class?`, `source_name?`, `treat_warnings_as_errors=false`, `request_id?` | Read-only XML/HTML-like UMG markup to canonical `FUISpecDocument` JSON. Does not create or modify assets; feed returned `spec` into `build_ui_from_spec`. |
| `diff_ui_spec` | `asset_path*`, `desired_spec*`, `compare_mode=structural/properties/full`, `request_id?` | Read-only live-WBP vs desired `FUISpecDocument` diff. Emits stable-name patch candidates, reports existing `FDelegateEditorBinding` preservation/at-risk evidence, and reports unsupported fields instead of falling back to raw writes. |
| `[w] apply_ui_spec_patch` | `asset_path*`, `patch*`, `dry_run=true`, `confirm=false`, `compile=true`, `save=false`, `read_back=true`, `continue_on_error=false`, `request_id?` | Confirm-gated existing-WBP patch workflow. Routes add/remove/move/explicit replace/slot/property/text/image/Border-brush/common-style/type-specific-style/CommonUI-style/EffectSurface changes through existing owner actions, decomposes `replace_widget` to `remove_widget` + `add_widget` plus content/style/effect owner-action steps, maps text `fontColor` via `set_text`, maps Image `brushPath` via `set_image`, maps Border `brushPath` via `set_brush(Background)`, decomposes `set_style` opacity/visibility plus SizeBox size constraints, Border brush color/padding, and ProgressBar fill color to allowlist-gated `set_widget_property`, routes `apply_style_to_widget` through the existing CommonUI owner action, decomposes canonical `FUISpecEffect` fields to existing `set_effect_surface_*` actions, decomposes `slot.anchorPreset` to `set_anchor_preset`, preflights slot-class compatibility, compiles once, optionally saves, and returns round-trip dump proof. |
| `dump_ui_spec` | `asset_path*`, `emit_defaults=false`, `request_id?` | Read an existing Widget Blueprint back into canonical `FUISpecDocument` JSON for round-trip/diff workflows. |
| `audit_widget_layout` | `asset_paths?`, `path_prefix=/Game/UI`, `allowed_canvas_slots?`, `include_tests=false`, `rule_profile=shipping`, `suppress_rule_ids?`, `treat_warnings_as_errors=false` | Read-only structural layout lint over dumped UISpec data, including Canvas overuse, one-child Canvas wrappers, SafeZone ancestry, decorative hit-test blockers, hidden interactive space, and large static list heuristics. |
| `measure_widget_layout` | `asset_path*`, `profiles?`, `check_overlap=true`, `check_safe_zone=true`, `max_allowed_overlap_ratio=0.0` | Read-only authored layout evidence from canonical UISpec slot/style data. Reports bounds, sibling overlaps, and explicit safe-zone violations; render bounds are marked unavailable until paired with visual capture proof. |
| `[w] apply_common_menu_transform_spec` | `spec?`, `screens?`, `layout_asset_path?`, `layout_layers?`, `layers?`, `extension_points?`, `widget_properties?`, `remove_widgets?`, `variable_defaults?`, `focus_table?`, `initial_focus?`, `desired_focus?`, `nav_overrides?`, `navigation_bulk?`, `widget_subtrees?`, `blueprint_graphs?`, `font_repairs?`, `frontend_validation?`, shared remaps, `dry_run=true`, `confirm=false`, `compile=true`, `save=false`, `continue_on_error=false` | Apply the menu-level transform counterpart to `build_menu_from_spec`: dry-run plan by default, then orchestrate existing layer, UIExtension, widget property/removal, Blueprint variable default, initial focus, navigation bulk, subtree copy, graph clone, font repair, and frontend validation actions. Writes require `dry_run=false` and `confirm=true`. |
| `[w] build_menu_from_spec` | `screens*`, `layers?`, `focus_table?`, `nav_overrides?`, `overwrite=true`, `dry_run=false`, `treat_warnings_as_errors=false`, `raw_mode=false`, `request_id?` | Multi-screen menu document builder; full per-screen specs route through `build_ui_from_spec`, cross-screen aggregation is echoed for follow-up transform workflows. |
| **Post-Copy Repair (3)** | | |
| `[w] copy_widget_subtree_with_class_remap` | `source_asset_path*`, `destination_asset_path*`, `source_widget_name?`, `source_widget_names?`, `destination_widget_name?`, `destination_parent_name?`, `class_remaps?`, `object_remaps?`, `root_remaps?`, `source_root?`, `dest_root?`, `existing_policy=fail/replace/skip`, `insert_policy=source_index/append`, `require_remapped_classes=false`, `compile=true`, `dry_run=true`, `confirm=false`, `save=false` | Copy one or more WBP widget subtrees into another WBP while remapping widget classes plus hard/soft object references. Dry-run plans by default; writes require `dry_run=false` and `confirm=true`; collisions default to fail. |
| `[w] clone_composite_font_with_remapped_faces` | `source_font_path*`, `destination_font_path*`, `root_remaps?`, `source_root?`, `dest_root?`, `font_face_remaps?`, `dry_run=true`, `confirm=false`, `save=false` | Clone a composite `UFont` to a new destination asset while remapping `UFontFace` references. Fails on destination collision or missing remapped faces; writes require `dry_run=false` and `confirm=true`. |
| `[w] repair_slate_font_references` | `asset_path*`, `root_remaps?`, `source_root?`, `dest_root?`, `font_asset_remaps?`, `include_unchanged=false`, `dry_run=true`, `confirm=false`, `save=false` | Scan a copied UI asset package for serialized `FSlateFontInfo` values and remap their `FontObject` `UFont` references. Dry-run preflights by default; writes require `dry_run=false` and `confirm=true`. |
| **Animation v1 (5)** | | |
| `list_animations` | `asset_path*` | List UWidgetAnimations |
| `get_animation_details` | `asset_path*`, `animation_name*` | Tracks, sections, keyframes |
| `[w] create_animation` `[DEPRECATED]` | `asset_path*`, `animation_name*`, `duration*`, `tracks?` | DEPRECATED — use `create_animation_v2` |
| `[w] add_animation_keyframe` `[DEPRECATED]` | `asset_path*`, `animation_name*`, `widget_name*`, `property*` (opacity/transform/color), `time*`, `value*`, `component?` (tx/ty/angle/sx/sy or r/g/b/a) | DEPRECATED — use `create_animation_v2` |
| `[w] remove_animation` | `asset_path*`, `animation_name*` | Remove animation |
| **Animation v2 (5, preferred)** | | |
| `[w] create_animation_v2` | `asset_path*`, `animation_name*`, `duration_sec*`, `tracks*` (array of {widget_name,property,keys:[{time,value,interp?:cubic/linear/constant,arrive_tangent?,leave_tangent?,arrive_weight?,leave_weight?}]}), `compile_once=true` | Multi-track/multi-key with cubic easing |
| `[w] add_bezier_eased_segment` | `asset_path*`, `animation_name*`, `widget_name*`, `property*`, `from_value*`, `to_value*`, `start_time*`, `end_time*`, `bezier*` ([x1,y1,x2,y2] CSS cubic-bezier) | Insert 2-key eased segment |
| `[w] bake_spring_animation` | `asset_path*`, `animation_name*`, `widget_name*`, `property*`, `from_value*`, `to_value*`, `stiffness=100`, `damping=10`, `mass=1`, `fps=60`, `duration=2.0`, `compile_once=true` | Bake damped spring into keys |
| `[w] add_animation_event_track` | `asset_path*`, `animation_name*`, `events*` (array of {time,event_name}) | Add event track to animation |
| `[w] bind_animation_to_event` | `asset_path*`, `animation_name*`, `widget_event*` (OnHovered/OnUnhovered/OnPressed/OnReleased/OnFocusReceived/OnFocusLost), `animation_event=Started` (Started/Finished) | Wire widget event to animation |
| **Animation read inspection (3)** | | |
| `get_animation_overview` | `asset_path*`, `animation_name?`, `include_all=false` | Read-only compact inventory for UWidgetAnimation timing, MovieScene bindings/tracks, key counts, event summaries, and delegate binding rows. External MCP names such as `animation_overview` are search aliases only, not registered actions. |
| `get_animation_timeline` | `asset_path*`, `animation_name*`, `widget_name?`, `property_path?`, `include_events=true`, `max_rows=1000` | Read-only sorted property-key and event-key rows for animation QA and round-trip evidence. |
| `get_animation_time_slice` | `asset_path*`, `animation_name*`, `time?`, `times?`, `widget_name?`, `property_path?`, `event_tolerance_frames=0` | Read-only sampled float-track values at one or more times, plus exact-frame event matches. |
| **Animation delta (1)** | | |
| `[w] apply_animation_delta` | `asset_path*`, `animation_name*`, `operations*` (array of {op:upsert_float_key/delete_float_key, widget_name? or binding_guid?, property_path/property, time, value?}), `dry_run=true`, `confirm=false`, `confirm_delete=false`, `compile=true`, `read_back=true` | Confirm-gated scalar float-key merge/upsert/delete on existing animations. Use `create_animation_v2` for new/full animation authoring; external Sequencer write names are search aliases only. |
| **Data Binding (4)** | | |
| `list_widget_events` | `asset_path*`, `widget_name?` | Bindable events |
| `list_widget_properties` | `asset_path*`, `widget_name*` | Settable properties with types |
| `[w] setup_list_view` | `asset_path*`, `list_widget_name*`, `entry_widget_class*` (entry WBP path — **required**, was previously documented optional), `entry_height=50`, `entry_width=100` | Configure ListView/TileView |
| `get_widget_bindings` | `asset_path*` | All property bindings |
| **Settings & Save Scaffolding (5)** | | |
| `[w] scaffold_game_user_settings` | `class_name*`, `module_name*`, `features?` (audio_volumes/mouse_sensitivity/accessibility_flags/keybinding_support) | UGameUserSettings subclass C++ |
| `[w] scaffold_save_game` | `class_name*`, `module_name*`, `properties?` (array of {name,type,default_value}) | ULocalPlayerSaveGame subclass C++ |
| `[w] scaffold_save_subsystem` | `class_name*`, `module_name*`, `save_game_class*` | Save management subsystem C++ |
| `[w] scaffold_audio_settings` | `categories?` (default Master/Music/SFX/Voice/Ambient) | Audio settings wiring info |
| `[w] scaffold_input_remapping` | `actions?` (input action names) | Keybinding remapping setup |
| `[w] scaffold_main_menu` | `save_path*`, `button_names?`, `parent_class?`, `action_button_class?`, `action_table?`, `default_style_palette?` | One-shot main-menu WBP with buttons + bound action bar |
| `[w] scaffold_settings_panel_with_tabs` | `save_path*`, `tab_names?`, `parent_class?`, `action_table?`, `action_button_class?` | One-shot settings-panel WBP with tab list + switcher + action bar |
| `[w] scaffold_pause_menu` | `save_path*`, `action_table*`, `button_names?`, `parent_class?`, `action_button_class?` | One-shot pause-menu WBP with buttons + action bar |
| **Accessibility (4)** | | |
| `[w] scaffold_accessibility_subsystem` | `class_name*`, `module_name*` | Accessibility settings subsystem C++ |
| `[w] audit_accessibility` | `asset_path*` | Audit font size, focus, navigation, tooltips |
| `[w] set_colorblind_mode` | `mode*` (Normal/Deuteranope/Protanope/Tritanope), `severity=5` (0-10), `correct=true` | Colorblind correction (runtime) |
| `[w] set_text_scale` | `scale*` (0.75-1.5) | UI text scale (runtime) |

## Anchor Presets

`top_left`(0,0,0,0) `top_center`(0.5,0,0.5,0) `top_right`(1,0,1,0) `center_left`(0,0.5,0,0.5) `center`(0.5,0.5,0.5,0.5) `center_right`(1,0.5,1,0.5) `bottom_left`(0,1,0,1) `bottom_center`(0.5,1,0.5,1) `bottom_right`(1,1,1,1) `stretch_fill`(0,0,1,1) `stretch_horizontal`(0,0.5,1,0.5) `stretch_vertical`(0.5,0,0.5,1)

## Common Workflows

### 1. Build a HUD through the ViewModel boundary

Create the WBP, add the widget tree and slots, expose state as ViewModel-facing member variables (do NOT have the widget reach into gameplay Actors/components for mutable state — see Project UI Architecture Rules), style, then animate. Compile at the end.

```
1.  ui_query("create_widget_blueprint", {"save_path": "/Game/UI/WBP_GameHUD", "root_widget": "CanvasPanel"})
2.  ui_query("add_widget", {"asset_path": "/Game/UI/WBP_GameHUD", "widget_class": "Image", "widget_name": "HealthFill", "anchor_preset": "top_left"})
3.  ui_query("add_widget", {"asset_path": "/Game/UI/WBP_GameHUD", "widget_class": "TextBlock", "widget_name": "AmmoText", "anchor_preset": "bottom_right"})
4.  ui_query("set_anchor_preset", {"asset_path": "/Game/UI/WBP_GameHUD", "widget_name": "HealthFill", "preset": "top_left"})
5.  ui_query("add_widget_variable", {"wbp_path": "/Game/UI/WBP_GameHUD", "var_name": "ViewModel", "var_type": "<token — see action desc via monolith_discover>"})
6.  ui_query("list_widget_properties", {"asset_path": "/Game/UI/WBP_GameHUD", "widget_name": "HealthFill"})
7.  ui_query("get_widget_bindings", {"asset_path": "/Game/UI/WBP_GameHUD"})
8.  ui_query("set_text", {"asset_path": "/Game/UI/WBP_GameHUD", "widget_name": "AmmoText", "text": "30", "font_size": 28})
9.  ui_query("set_color_scheme", {"colors": {"User1": "#0A0A14", "User2": "#E03030"}})
10. ui_query("create_animation_v2", {"asset_path": "/Game/UI/WBP_GameHUD", "animation_name": "LowHealthPulse", "duration_sec": 1.0, "tracks": [{"widget_name": "HealthFill", "property": "opacity", "keys": [{"time": 0.0, "value": 1.0}, {"time": 0.5, "value": 0.4, "interp": "cubic"}, {"time": 1.0, "value": 1.0}]}]})
11. ui_query("bind_animation_to_event", {"asset_path": "/Game/UI/WBP_GameHUD", "animation_name": "LowHealthPulse", "widget_event": "OnHovered", "animation_event": "Started"})
12. ui_query("compile_widget", {"asset_path": "/Game/UI/WBP_GameHUD"})
```

Step 5's `ViewModel` member variable is the binding seam: bind widget text/visibility/progress to ViewModel-facing state (steps 6-7 inspect the settable properties and existing bindings), and route user intent from widgets to the ViewModel rather than to gameplay Actors. The GAS attribute (e.g. health/mana) that feeds this HUD is authored in **unreal-gas**, then surfaced through the ViewModel and bound here. Confirm `var_type` token grammar and exact params with `monolith_discover({ namespace: "ui", action: "<action>", mode: "schema" })` before calling.

For widget event intent, use the workflow wrapper instead of inventing UI-local Blueprint graph actions. Start with dry-run; apply only after the returned `actions[]` shows the expected `blueprint.resolve_node`, `blueprint.add_node`, `blueprint.connect_pins`, and compile/read-back steps.

```
1. workflow_query("ui_bind_widget_event", {
     "asset_path": "/Game/UI/WBP_MainMenu",
     "widget_name": "StartButton",
     "event": "Clicked",
     "intent": {
       "kind": "viewmodel_command",
       "viewmodel_variable": "ViewModel",
       "viewmodel_class": "MainMenuViewModel",
       "command": "StartGame"
     },
     "dry_run": true
   })
2. workflow_query("ui_bind_widget_event", {
     "asset_path": "/Game/UI/WBP_MainMenu",
     "widget_name": "StartButton",
     "event": "Clicked",
     "intent": {
       "kind": "viewmodel_command",
       "viewmodel_variable": "ViewModel",
       "viewmodel_class": "MainMenuViewModel",
       "command": "StartGame"
     },
     "dry_run": false,
     "confirm": true,
     "compile": true,
     "run_read_back": true
   })
3. workflow_query("ui_shipping_widget_blueprint", {"widget_asset_path": "/Game/UI/WBP_MainMenu", "proof_profile": "visual"})
```

`workflow.ui_bind_widget_event` accepts runtime UI intent only through `intent.kind="viewmodel_command"` and rejects direct Actor/Pawn/Controller/component targets. If pin inference is ambiguous, supply `intent.event_exec_pin`, `intent.command_exec_pin`, `intent.viewmodel_value_pin`, and `intent.command_target_pin` rather than bypassing the workflow.

For UI material effects, keep material graph work in the `material` owner namespace and use the workflow wrapper for proof and binding. Do not call or invent `hlsl_*` / external `material_*` compatibility APIs.

```
1. workflow_query("ui_material_hlsl_effect", {
     "material_path": "/Game/UI/Materials/M_ButtonGlow",
     "hlsl": "return float4(Glow, Glow, Glow, Glow);",
     "parameters": [
       {"name": "Glow", "type": "scalar", "default": 1.0}
     ],
     "create_material": true,
     "connect_opacity": true,
     "bind_to": {
       "asset_path": "/Game/UI/WBP_MainMenu",
       "widget_name": "GlowImage",
       "binding_action": "set_image"
     },
     "dry_run": true
   })
2. workflow_query("ui_material_hlsl_effect", {
     "material_path": "/Game/UI/Materials/M_ButtonGlow",
     "hlsl": "return float4(Glow, Glow, Glow, Glow);",
     "parameters": [
       {"name": "Glow", "type": "scalar", "default": 1.0}
     ],
     "connect_opacity": true,
     "bind_to": {
       "asset_path": "/Game/UI/WBP_MainMenu",
       "widget_name": "GlowImage",
       "binding_action": "set_image"
     },
     "dry_run": false,
     "confirm": true,
     "compile": true
   })
3. workflow_query("ui_shipping_widget_blueprint", {"widget_asset_path": "/Game/UI/WBP_MainMenu", "proof_profile": "visual"})
```

For float4 Custom HLSL outputs, `connect_opacity=true` automatically selects the ComponentMask alpha path unless a matching explicit Custom additional output such as `Alpha` is supplied. That mask is composed through `material.build_material_graph(clear_existing=false)`, not through a duplicate UI or HLSL action.

For RetainerBox effect materials, use the Retainer owner action or the workflow wrapper; do not route `EffectMaterial`/`TextureParameter` through raw `set_widget_property`.

```
1. workflow_query("ui_retainer_effect_material", {
     "material_path": "/Game/UI/Materials/M_RetainerBlur",
     "bind_to": {
       "asset_path": "/Game/UI/WBP_MainMenu",
       "retainer_widget_name": "MenuRetainer",
       "texture_parameter": "Texture"
     },
     "dry_run": true
   })
2. workflow_query("ui_retainer_effect_material", {
     "material_path": "/Game/UI/Materials/M_RetainerBlur",
     "bind_to": {
       "asset_path": "/Game/UI/WBP_MainMenu",
       "retainer_widget_name": "MenuRetainer",
       "texture_parameter": "Texture"
     },
     "dry_run": false,
     "confirm": true,
     "request_render": true,
     "compile": true
   })
3. workflow_query("ui_shipping_widget_blueprint", {"widget_asset_path": "/Game/UI/WBP_MainMenu", "proof_profile": "visual"})
```

`workflow.ui_material_hlsl_effect` composes `material.set_material_property`, Custom HLSL node creation/update, material output wiring, optional ComponentMask alpha wiring through `material.build_material_graph`, material compile/stat/domain/connection proof, `ui.set_image`/`ui.set_brush`, and Widget Blueprint compile log proof. `workflow.ui_retainer_effect_material` composes `material.get_material_parameters`, `material.get_material_properties`, `ui.set_retainer_effect_material`, and optional widget visual proof. Neither workflow proves Retainer runtime performance; do not claim Retainer/Invalidation optimization evidence without a measured profile.

Pre-built HUD elements are also available — `create_hud_element` stamps a `health_bar`/`crosshair`/`ammo_counter`/etc. element instead of hand-building the tree:

```
1. ui_query("create_widget_blueprint", {"save_path": "/Game/UI/WBP_GameHUD"})
2. ui_query("create_hud_element", {"asset_path": "/Game/UI/WBP_GameHUD", "element_type": "health_bar"})
3. ui_query("set_font", {"asset_path": "/Game/UI/WBP_GameHUD", "widget_name": "ammo_counter_Current", "font_size": 28, "typeface": "Bold"})
4. ui_query("compile_widget", {"asset_path": "/Game/UI/WBP_GameHUD"})
```

### 2. Settings / save panel

Build the tabbed settings panel and save-slot selector WBPs, then scaffold the backing C++ settings/save classes. Audit accessibility before shipping.

```
1. ui_query("create_settings_panel", {"save_path": "/Game/UI/WBP_Settings", "tabs": ["graphics", "audio", "controls", "accessibility"]})
2. ui_query("create_save_slot_list", {"save_path": "/Game/UI/WBP_SaveSlots", "max_slots": 3})
3. ui_query("scaffold_game_user_settings", {"class_name": "MyGameUserSettings", "module_name": "MyGame", "features": ["audio_volumes", "accessibility_flags"]})
4. ui_query("scaffold_save_game", {"class_name": "MySaveGame", "module_name": "MyGame", "properties": [{"name": "SlotName", "type": "FString", "default_value": ""}]})
5. ui_query("scaffold_save_subsystem", {"class_name": "MySaveSubsystem", "module_name": "MyGame", "save_game_class": "MySaveGame"})
6. ui_query("audit_accessibility", {"asset_path": "/Game/UI/WBP_Settings"})
```

The `scaffold_*` actions emit C++ subclasses (UGameUserSettings / ULocalPlayerSaveGame / subsystem); the panel widgets bind their UI through a ViewModel adapter per the architecture rules, not by reaching into the settings object directly.

### 3. Repair a copied Widget Blueprint subtree

After an asset package copy, use the post-copy repair actions to restore or replace a WBP subtree with explicit remaps. Start with dry-run and only apply after the plan shows the intended class/object rewrites.

```
1. ui_query("copy_widget_subtree_with_class_remap", {
     "source_asset_path": "/Game/UI/Old/WBP_Source",
     "destination_asset_path": "/Game/UI/New/WBP_Destination",
     "source_widget_name": "HostPanel",
     "destination_widget_name": "HostPanel",
     "class_remaps": { "VerticalBox": "HorizontalBox" },
     "root_remaps": { "/Game/UI/Old": "/Game/UI/New" },
     "dry_run": true
   })
2. ui_query("copy_widget_subtree_with_class_remap", {
     "source_asset_path": "/Game/UI/Old/WBP_Source",
     "destination_asset_path": "/Game/UI/New/WBP_Destination",
     "source_widget_name": "HostPanel",
     "destination_widget_name": "HostPanel",
     "class_remaps": { "VerticalBox": "HorizontalBox" },
     "root_remaps": { "/Game/UI/Old": "/Game/UI/New" },
     "existing_policy": "replace",
     "dry_run": false,
     "confirm": true,
     "compile": true
   })
3. ui_query("repair_slate_font_references", {"asset_path": "/Game/UI/New/WBP_Destination", "root_remaps": {"/Game/UI/Old": "/Game/UI/New"}, "dry_run": true})
```

This action owns WBP tree repair only. Actor/component Blueprint graph cloning still belongs to **unreal-blueprints**.

## Capturing UMG Widgets to PNG (editor:: action)

`editor_query("capture_scene_preview", { asset_path: "/Game/UI/WBP_Foo", asset_type: "widget", scale: 1.5 })` renders a Widget Blueprint offscreen via `FWidgetRenderer` and writes a PNG. Optional `scale` is a DPI multiplier. Useful for design reviews, accessibility audits, and verifying menu scaffolds before PIE. See `monolith_guide(section="recipes")` entry "Visual introspection -- going beyond thumbnails".

For shippable UI proof, prefer the workflow wrapper over ad hoc capture calls:

```
workflow_query("ui_shipping_widget_blueprint", {
  "widget_asset_path": "/Game/UI/WBP_Foo",
  "proof_profile": "visual",
  "dry_run": false,
  "run_read_only_checks": true,
  "layout_rule_profile": "shipping",
  "suppress_layout_rule_ids": [],
  "visual_profiles": [
    {"name": "desktop", "resolution": [1280, 720], "dpi_scale": 1.0},
    {"name": "mobile", "resolution": [1280, 720], "dpi_scale": 1.0, "safe_zone": {"left": 48, "top": 24, "right": 48, "bottom": 24}}
  ]
})
```

`proof_profile="visual"` composes compile read-back, `editor.capture_scene_preview(asset_type="widget")`, and `ui.verify_widget_visual_artifacts`. The verifier checks PNG existence, byte size, dimensions, hash, transparency, and nonblank pixel evidence, then writes a manifest under `Saved/Monolith`. `layout_rule_profile` and `suppress_layout_rule_ids` are forwarded to `ui.audit_widget_layout`; mobile/console visual profiles must include both `dpi_scale` and `safe_zone` or the workflow reports `DpiSafeZoneProfileMissing`. `proof_profile="runtime"` is intentionally blocked until an async PIE/CommonUI proof chain is supplied; use the returned next actions instead of claiming runtime interaction proof.

## Horror UI + Accessibility Guidelines

**Horror:**
- No health bar -- use vignette + desaturation + heartbeat audio via post-process
- Minimal HUD -- auto-hide when full (stamina, etc.)
- Subtitles ON by default -- caption ALL sounds
- Diegetic where possible -- flashlight battery on prop, not overlay
- No minimap -- navigation uncertainty = horror
- Short interaction trace -- must get close

**Accessibility (critical -- hospice patients):**
- **Atkinson Hyperlegible** font at `Content/UI/Fonts/Atkinson/`
- Minimum: 18pt body, 22pt subtitles
- Always offer: text scale, colorblind mode, reduced motion, hold-vs-toggle
- Focus indicators must be visually distinct (not just color)
- Run `audit_accessibility` on every WBP before shipping

---

## CommonFramework Diagnostics/Authoring (7, always-on)

Use these before editing Lyra/CommonGame layouts or debugging CommonUI policy setup:

- `get_common_framework_status` — reports CommonUI, CommonGame, UIExtension, CommonUser, CommonLoadingScreen, GameSettings, GameplayMessageRouter, ModularGameplayActors, and GameSubtitles plugin/module availability plus bounded reflected class and struct summaries for high-value types such as `GameUIPolicy`, `PrimaryGameLayout`, `UIExtensionPointWidget`, `CommonUserSubsystem`, `LoadingScreenManager`, `GameSettingRegistry`, `GameplayMessageSubsystem`, `ModularCharacter`, `SubtitleDisplaySubsystem`, and `GameplayMessageListenerHandle`.
- `add_primary_game_layout_layer` — adds or updates a `CommonActivatableWidgetContainerBase` layer widget in a `PrimaryGameLayout` WBP and returns the `RegisterLayer` `layer_tag` / `widget_name` pair. It edits only the WBP tree/slot layout; it does not patch CommonGame or generate layout graph code.
- `describe_common_widget_blueprint` — inspects a WBP for `PrimaryGameLayout` parentage, UIExtension point tags, `UIExtensionPointTagMatch` / `DataClasses`, and `CommonActivatableWidgetContainerBase` layer candidates.
- `describe_common_messaging_flow` — reports selected `CommonMessagingSubsystem` subclass/config section, confirmation/error `CommonGameDialog` classes, modal layer tag registration, `DefaultUIPolicyClass`, and loaded messaging subclasses without modifying runtime code.
- `validate_common_dialog_contract` — returns `ok=false` with structured issues when configured confirmation/error classes are missing, unloaded, abstract, deprecated, or not `CommonGameDialog` subclasses.
- `validate_common_layer_push_contract` — checks the modal layer tag, optional `PrimaryGameLayout` WBP layer container candidates, dialog class compatibility, and explicitly reports when RegisterLayer graph wiring is not proven by read-only inspection.
- `validate_frontend_menu_flow` — checks a frontend menu composition spec across optional `PrimaryGameLayout` layer candidates, dialog class compatibility, CommonActivatable screen parents, expected/forbidden widgets, widget classes, variable defaults, desired focus, and graph text needles without editing assets.

These actions use reflection, config reads, and `StaticLoadClass`; they do not hard-link CommonGame, UIExtension, CommonUser, or modify PrimaryGameLayout/CommonGame runtime code.

## CommonUI Actions (62, v0.14.0+, conditional)

Require the CommonUI engine plugin. Stock in UE 5.7 at `Engine/Plugins/Runtime/CommonUI/`. Build.cs detects via 3-location scan; when the plugin is absent the CommonUI action pack silently unregisters. Runtime-phase actions marked `[RUNTIME]` require a PIE session. Filter the listing via `monolith_discover({ namespace: "ui", category: "CommonUI" })`.

Full per-action parameter signatures — grouped into subsections A–I (Activatable Lifecycle, Buttons + Styling, Input/Actions/Glyphs, Navigation/Focus, Lists/Tabs/Groups/Switchers/Carousel/HW Visibility, Content, Dialog/Modal, Audit + Lint, Accessibility Bridge) plus CommonUI Known Limitations — live in [references/commonui-actions.md](references/commonui-actions.md). The discover-first block above stays the authority; do not call from that snapshot alone if param names, aliases, or ranges are load-bearing.
