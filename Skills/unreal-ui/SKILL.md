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
monolith_discover({ namespace: "ui", action: "add_widget", mode: "schema" })  // exact params for one action
monolith_find("make a health bar HUD")                                  // jump straight to the right action
```

**136 UI actions** in the live catalog (snapshot — confirm via `monolith_discover`): always-on UMG authoring plus CommonUI-plugin-conditional actions plus GAS attribute-binding aliases (post Phase A–L MonolithUI architecture expansion; texture/font ingest moved to `asset`). Always-on surface: Widget CRUD (7), Slot (3), Templates (8), Styling (6), v1 Animation (5 — `create_animation` + `add_animation_keyframe` `[DEPRECATED]` Phase L; prefer `create_animation_v2`), v2 Animation hoisted (5), Bindings (4), Settings scaffolds (5), Accessibility (4), Hoisted Design Import effects (3), EffectSurface (10), **Spec Builder (3 — `build_ui_from_spec`, `dump_ui_spec_schema`, `dump_ui_spec`)**, Type Registry diagnostic (1).

**CommonUI actions require the CommonUI engine plugin** (stock UE 5.7, `Engine/Plugins/Runtime/CommonUI/`). When absent, 51 actions silently unregister (50 in `Source/MonolithUI/Private/CommonUI/*.cpp` + 1 inline `dump_style_cache_stats` lambda). Detect via `monolith_discover`.

## When to use / Use a different skill for

Use **unreal-ui** for runtime UMG widgets — Widget Blueprints, HUDs, menus, settings/save panels, widget tree CRUD, slots/anchors, styling, animations, data binding, CommonUI, and accessibility on the UI side.

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
| `[w] add_widget_variable` | `wbp_path*` (alias `asset_path`), `var_name*`, `var_type*` (token grammar — see action desc), `default_value?`, `var_category?` | Add member variable to WBP |
| `[w] set_widget_is_variable` | `wbp_path*` (alias `asset_path`), `widget_name*`, `is_variable*` | Toggle widget's bIsVariable flag |
| `list_widget_property_enums` | `wbp_path?`, `widget_class?`, `property_name?` | Enum-typed props + valid values |
| `[w] dump_property_allowlist` | `widget_type*` | Allowed property paths for a widget type |
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
| **Styling (6)** | | |
| `[w] set_brush` | `asset_path*`, `widget_name*`, `property_name*`, `draw_type=Image` (Image/Box/Border/RoundedBox/NoDrawType), `tint_color?`, `image_size?`, `margin?`, `corner_radius?`, `outline_color?`, `outline_width?`, `texture_path?`, `material_path?`, `compile=false` | Configure brush |
| `[w] set_font` | `asset_path*`, `widget_name*`, `font_size?`, `font_family?`, `typeface=Regular` (Regular/Bold/Italic/Light), `letter_spacing?`, `outline_size?`, `outline_color?`, `compile=false` | Font on text widgets |
| `[w] set_color_scheme` | `colors*` (map e.g. {"User1":"#0A0A14",...}) | EStyleColor User1-16 palette |
| `[w] batch_style` | `asset_path*`, `widget_class*`, `property_name*`, `value*`, `compile=false` | Apply to all widgets of class |
| `[w] set_text` | `asset_path*`, `widget_name*`, `text?`, `text_color?`, `font_size?`, `justification?` (Left/Center/Right), `compile=false` | Convenience text setter |
| `[w] set_image` | `asset_path*`, `widget_name*`, `texture_path?`, `material_path?`, `tint_color?`, `size?`, `compile=false` | Convenience image setter |
| `[w] set_rounded_corners` | `asset_path*`, `widget_name*`, `corner_radii?` [TL,TR,BR,BL], `outline_color?`, `outline_width?`, `fill_color?`, `compile=true` | Reflection writer for corner/outline/fill (≥1 optional required) |
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

## Capturing UMG Widgets to PNG (editor:: action)

`editor_query("capture_scene_preview", { asset_path: "/Game/UI/WBP_Foo", asset_type: "widget", scale: 1.5 })` renders a Widget Blueprint offscreen via `FWidgetRenderer` and writes a PNG. Optional `scale` is a DPI multiplier. Useful for design reviews, accessibility audits, and verifying menu scaffolds before PIE. See `monolith_guide(section="recipes")` entry "Visual introspection -- going beyond thumbnails".

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

## CommonUI Actions (50, v0.14.0, conditional)

Require the CommonUI engine plugin. Stock in UE 5.7 at `Engine/Plugins/Runtime/CommonUI/`. Build.cs detects via 3-location scan; when the plugin is absent the 50 actions silently unregister. Runtime-phase actions marked `[RUNTIME]` require a PIE session. Filter the listing via `monolith_discover({ namespace: "ui", category: "CommonUI" })`.

Full per-action parameter signatures — grouped into subsections A–I (Activatable Lifecycle, Buttons + Styling, Input/Actions/Glyphs, Navigation/Focus, Lists/Tabs/Groups/Switchers/Carousel/HW Visibility, Content, Dialog/Modal, Audit + Lint, Accessibility Bridge) plus CommonUI Known Limitations — live in [references/commonui-actions.md](references/commonui-actions.md). The discover-first block above stays the authority; do not call from that snapshot alone if param names, aliases, or ranges are load-bearing.
