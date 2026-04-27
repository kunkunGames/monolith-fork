# Monolith — MonolithUI Module

**Parent:** [SPEC_CORE.md](../SPEC_CORE.md)
**Engine:** Unreal Engine 5.7+
**Version:** 0.14.7 (Beta)

---

## MonolithUI

**Dependencies:** Core, CoreUObject, Engine, MonolithCore, UnrealEd, UMGEditor, UMG, Slate, SlateCore, Json, JsonUtilities, KismetCompiler, MovieScene, MovieSceneTracks, CommonUI (optional — `#if WITH_COMMONUI`)
**Total actions in `ui::` namespace:** 96 (42 UMG baseline owned by this module + 50 CommonUI owned by this module + 4 GAS UI binding aliases owned by `MonolithGAS` and registered into `ui::`)
**Settings toggle:** `bEnableUI` (default: True)
**MCP tool:** `ui_query`
**Namespace:** `ui`

### Classes

| Class | Responsibility |
|-------|---------------|
| `FMonolithUIModule` | Registers 92 actions owned by this module (42 UMG + 50 CommonUI when `WITH_COMMONUI`). Logs live `ui` namespace action count at startup |
| `FMonolithUIActions` | Widget blueprint CRUD: create, inspect, add/remove widgets, property writes, compile |
| `FMonolithUISlotActions` | Layout slot operations: slot properties, anchor presets, widget movement |
| `FMonolithUITemplateActions` | High-level HUD/menu/panel scaffold templates (8 templates) |
| `FMonolithUIStylingActions` | Visual styling: brush, font, color scheme, text, image, batch style |
| `FMonolithUIAnimationActions` | UMG widget animation CRUD: list, inspect, create, add/remove keyframes |
| `FMonolithUIBindingActions` | Event/property binding inspection, list view setup, widget binding queries |
| `FMonolithUISettingsActions` | Settings/save/audio/input remapping subsystem scaffolding (5 scaffolds) |
| `FMonolithUIAccessibilityActions` | Accessibility subsystem scaffold, audit, colorblind mode, text scale |

---

## Actions — UMG Baseline (42 — namespace: "ui")

**Widget CRUD (7)**
| Action | Params | Description |
|--------|--------|-------------|
| `create_widget_blueprint` | `save_path`, `parent_class` | Create a new Widget Blueprint asset |
| `get_widget_tree` | `asset_path` | Get the full widget hierarchy tree |
| `add_widget` | `asset_path`, `widget_class`, `parent_slot` | Add a widget to the widget tree |
| `remove_widget` | `asset_path`, `widget_name` | Remove a widget from the widget tree |
| `set_widget_property` | `asset_path`, `widget_name`, `property_name`, `value` | Set a property on a widget via reflection |
| `compile_widget` | `asset_path` | Compile the Widget Blueprint and return errors/warnings |
| `list_widget_types` | none | List all available widget classes that can be instantiated |

**Slot Operations (3)**
| Action | Params | Description |
|--------|--------|-------------|
| `set_slot_property` | `asset_path`, `widget_name`, `property_name`, `value` | Set a layout slot property (padding, alignment, size, etc.) |
| `set_anchor_preset` | `asset_path`, `widget_name`, `preset` | Apply an anchor preset to a Canvas Panel slot |
| `move_widget` | `asset_path`, `widget_name`, `new_parent`, `slot_index` | Move a widget to a different parent slot |

**Templates (8)**
| Action | Params | Description |
|--------|--------|-------------|
| `create_hud_element` | `save_path`, `element_type` | Scaffold a common HUD element (health bar, crosshair, ammo counter, etc.) |
| `create_menu` | `save_path`, `menu_type` | Scaffold a menu Widget Blueprint (main menu, pause menu, etc.) |
| `create_settings_panel` | `save_path` | Scaffold a settings panel with common option categories |
| `create_dialog` | `save_path`, `dialog_type` | Scaffold a dialog Widget Blueprint (confirmation, info, input prompt) |
| `create_notification_toast` | `save_path` | Scaffold a notification/toast Widget Blueprint |
| `create_loading_screen` | `save_path` | Scaffold a loading screen Widget Blueprint with progress bar |
| `create_inventory_grid` | `save_path`, `columns`, `rows` | Scaffold a grid-based inventory Widget Blueprint |
| `create_save_slot_list` | `save_path` | Scaffold a save slot list Widget Blueprint |

**Styling (6)**
| Action | Params | Description |
|--------|--------|-------------|
| `set_brush` | `asset_path`, `widget_name`, `brush_property`, `texture_path` | Set a brush/image property on a widget |
| `set_font` | `asset_path`, `widget_name`, `font_asset`, `size` | Set the font and size on a text widget |
| `set_color_scheme` | `asset_path`, `color_map` | Apply a color scheme (name->LinearColor map) across the widget |
| `batch_style` | `asset_path`, `style_operations` | Apply multiple styling operations in a single transaction |
| `set_text` | `asset_path`, `widget_name`, `text` | Set display text on a text widget |
| `set_image` | `asset_path`, `widget_name`, `texture_path` | Set the texture on an image widget |

**Animation (5)**
| Action | Params | Description |
|--------|--------|-------------|
| `list_animations` | `asset_path` | List all UMG animations on a Widget Blueprint |
| `get_animation_details` | `asset_path`, `animation_name` | Get tracks and keyframes for a named animation |
| `create_animation` | `asset_path`, `animation_name` | Create a new UMG widget animation |
| `add_animation_keyframe` | `asset_path`, `animation_name`, `widget_name`, `property`, `time`, `value` | Add a keyframe to a widget animation track |
| `remove_animation` | `asset_path`, `animation_name` | Remove a UMG widget animation |

**Bindings (4)**
| Action | Params | Description |
|--------|--------|-------------|
| `list_widget_events` | `asset_path` | List all bindable events on a Widget Blueprint |
| `list_widget_properties` | `asset_path`, `widget_name` | List all bindable properties on a widget |
| `setup_list_view` | `asset_path`, `list_view_name`, `entry_widget_path` | Configure a List View widget with an entry widget class |
| `get_widget_bindings` | `asset_path` | Get all active property and event bindings on a Widget Blueprint |

**Settings Scaffolding (5)**
| Action | Params | Description |
|--------|--------|-------------|
| `scaffold_game_user_settings` | `save_path`, `class_name` | Scaffold a UGameUserSettings subclass with common settings properties |
| `scaffold_save_game` | `save_path`, `class_name` | Scaffold a USaveGame subclass with save slot infrastructure |
| `scaffold_save_subsystem` | `save_path`, `class_name` | Scaffold a save game subsystem (UGameInstanceSubsystem) |
| `scaffold_audio_settings` | `save_path`, `class_name` | Scaffold an audio settings manager with volume/mix controls |
| `scaffold_input_remapping` | `save_path`, `class_name` | Scaffold an input remapping system backed by Enhanced Input |

**Accessibility (4)**
| Action | Params | Description |
|--------|--------|-------------|
| `scaffold_accessibility_subsystem` | `save_path`, `class_name` | Scaffold a UGameInstanceSubsystem implementing accessibility features |
| `audit_accessibility` | `asset_path` | Audit a Widget Blueprint for common accessibility issues (missing tooltips, low contrast, small text) |
| `set_colorblind_mode` | `asset_path`, `mode` | Apply a colorblind-safe palette mode (deuteranopia, protanopia, tritanopia) |
| `set_text_scale` | `asset_path`, `scale` | Apply a global text scale factor to all text widgets in the blueprint |

---

## Actions — CommonUI (50 — namespace: "ui", conditional on `WITH_COMMONUI`)

Shipped M0.5, v0.14.0 (2026-04-19). Tested M0.5.1 (2026-04-25): 50/50 editor-time actions PASS, 8 bugs found and fixed. 11 actions marked [RUNTIME] need PIE testing.

### Conditional Compilation

- **Build.cs detection:** 3-location probe (project `Plugins/`, engine `Plugins/Marketplace/`, engine `Plugins/Runtime/`) for the CommonUI plugin
- **Compile guard:** `#if WITH_COMMONUI` — wraps all 50 CommonUI actions
- **Release escape hatch:** `MONOLITH_RELEASE_BUILD=1` forces `WITH_COMMONUI=0`, stripping CommonUI dependency from the released DLL

### Style Pattern

Class-as-data: style creators (`create_common_button_style`, `create_common_text_style`, `create_common_border_style`) produce Blueprint subclasses via `FKismetEditorUtilities::CreateBlueprint`. These return `_C` class paths. Consumed by `TSubclassOf<UStyle>` properties on CommonUI widgets via `apply_style_to_widget` and `batch_retheme`.

### Default Button Class

`convert_button_to_common` and other button-targeting actions auto-create a persistent Blueprint at `/Game/Monolith/CommonUI/MonolithDefaultCommonButton` (session-cached, saveable). This avoids requiring callers to pre-create a button subclass for simple operations.

### Category A: Activatable Lifecycle (8 actions)

| Action | Params | Description |
|--------|--------|-------------|
| `create_activatable_widget` | `save_path`, `parent_class` | Create a Widget Blueprint parented to `UCommonActivatableWidget` |
| `create_activatable_stack` | `save_path` | Scaffold a WBP containing a `UCommonActivatableWidgetStack` |
| `create_activatable_switcher` | `save_path` | Scaffold a WBP containing a `UCommonActivatableWidgetSwitcher` |
| `configure_activatable` | `asset_path`, `widget_name`, `properties` | Set activatable-specific properties (auto-activate, input config) |
| `push_to_activatable_stack` | `stack_widget`, `widget_class` | [RUNTIME] Push a widget onto an activatable stack |
| `pop_activatable_stack` | `stack_widget` | [RUNTIME] Pop the top widget from an activatable stack |
| `get_activatable_stack_state` | `stack_widget` | [RUNTIME] Query the stack: active widget, depth, transition state |
| `set_activatable_transition` | `asset_path`, `transition_type`, `duration` | Configure push/pop transition animations |

### Category B: Buttons + Styling (9 actions)

| Action | Params | Description |
|--------|--------|-------------|
| `convert_button_to_common` | `asset_path`, `widget_name` | Replace a UButton with UCommonButtonBase (does NOT auto-transfer children) |
| `configure_common_button` | `asset_path`, `widget_name`, `properties` | Set button-specific properties (triggering input action, selection state, etc.) |
| `create_common_button_style` | `save_path`, `style_name`, `style_spec` | Create a Blueprint subclass of `UCommonButtonStyle` |
| `create_common_text_style` | `save_path`, `style_name`, `style_spec` | Create a Blueprint subclass of `UCommonTextStyle` |
| `create_common_border_style` | `save_path`, `style_name`, `style_spec` | Create a Blueprint subclass of `UCommonBorderStyle` |
| `apply_style_to_widget` | `asset_path`, `widget_name`, `style_class` | Apply a style class to a CommonUI widget |
| `batch_retheme` | `asset_path`, `style_map` | Retheme multiple widgets in a single transaction |
| `configure_common_text` | `asset_path`, `widget_name`, `properties` | Set `UCommonTextBlock` properties (style, scroll speed, auto-collapse) |
| `configure_common_border` | `asset_path`, `widget_name`, `properties` | Set `UCommonBorder` properties (style, opacity, etc.) |

### Category C: Input/Actions/Glyphs (7 actions)

| Action | Params | Description |
|--------|--------|-------------|
| `create_input_action_data_table` | `save_path`, `table_name` | Create a DataTable for CommonUI input action definitions |
| `add_input_action_row` | `table_path`, `row_name`, `action_spec` | Add a row to an input action DataTable |
| `bind_common_action_widget` | `asset_path`, `widget_name`, `action_row` | Bind a `UCommonActionWidget` to display a specific input glyph |
| `create_bound_action_bar` | `save_path` | Scaffold a WBP containing a `UCommonBoundActionBar` |
| `get_active_input_type` | none | [RUNTIME] Query the current active input type (gamepad, keyboard, touch) |
| `set_input_type_override` | `input_type` | [RUNTIME] Force a specific input type for glyph display |
| `list_platform_input_tables` | none | List all registered platform input DataTables |

### Category D: Navigation/Focus (5 actions)

| Action | Params | Description |
|--------|--------|-------------|
| `set_widget_navigation` | `asset_path`, `widget_name`, `nav_rules` | Configure explicit navigation rules (up/down/left/right targets) |
| `set_initial_focus_target` | `asset_path`, `target_name` | Set the initial focus target for an activatable widget |
| `force_focus` | `widget_name` | [RUNTIME] Force focus to a specific widget |
| `get_focus_path` | none | [RUNTIME] Query the current focus path (widget chain) |
| `request_refresh_focus` | none | [RUNTIME] Request CommonUI to recalculate focus |

### Category E: Lists/Tabs/Groups (7 actions)

| Action | Params | Description |
|--------|--------|-------------|
| `setup_common_list_view` | `asset_path`, `list_view_name`, `entry_widget_path` | Configure a `UCommonListView` with an entry widget class |
| `create_tab_list_widget` | `save_path` | Scaffold a WBP containing a `UCommonTabListWidgetBase` |
| `register_tab` | `tab_list_widget`, `tab_id`, `tab_widget` | [RUNTIME] Register a tab with a tab list |
| `create_button_group` | `group_name` | [RUNTIME] Create a `UCommonButtonGroupBase` for radio-style selection |
| `configure_animated_switcher` | `asset_path`, `widget_name`, `properties` | Configure `UCommonAnimatedSwitcher` transition settings |
| `create_widget_carousel` | `save_path` | Scaffold a WBP containing a `UCommonWidgetCarousel` |
| `create_hardware_visibility_border` | `save_path`, `visibility_tags` | Scaffold a WBP with `UCommonHardwareVisibilityBorder` (platform-gated visibility) |

### Category F: Content Widgets (4 actions)

| Action | Params | Description |
|--------|--------|-------------|
| `configure_numeric_text` | `asset_path`, `widget_name`, `properties` | Configure `UCommonNumericTextBlock` (format, interpolation speed, etc.) |
| `configure_rotator` | `asset_path`, `widget_name`, `properties` | Configure `UCommonRotator` widget properties |
| `create_lazy_image` | `asset_path`, `parent_slot` | Add a `UCommonLazyImage` widget (async texture loading) |
| `create_load_guard` | `asset_path`, `parent_slot` | Add a `UCommonLoadGuard` widget (loading state display) |

### Category G: Dialogs (2 actions)

| Action | Params | Description |
|--------|--------|-------------|
| `configure_modal_overlay` | `asset_path`, `widget_name`, `properties` | Configure modal overlay behavior (dismiss on click, input block) |
| `show_common_message` | `message_spec` | [RUNTIME] Show a CommonUI message dialog (fire-and-forward, no async result binding) |

### Category H: Audit + Lint (4 actions)

| Action | Params | Description |
|--------|--------|-------------|
| `audit_commonui_widget` | `asset_path` | Audit a CommonUI WBP for best-practice violations (missing styles, nav gaps, focus issues) |
| `export_commonui_report` | `asset_paths` | Export an audit report across multiple WBPs |
| `hot_reload_styles` | none | [RUNTIME, EXPERIMENTAL] Force-reload all CommonUI style assets |
| `dump_action_router_state` | none | [RUNTIME, EXPERIMENTAL] Dump the CommonUI action router state (cannot read `CurrentInputLocks` — engine-private) |

### Category I: Accessibility (4 actions)

| Action | Params | Description |
|--------|--------|-------------|
| `enforce_focus_ring` | `asset_path`, `widget_name`, `ring_spec` | Configure a visible focus ring on a CommonUI widget |
| `wrap_with_reduce_motion_gate` | `asset_path`, `widget_name` | Wrap a widget's animations with a reduce-motion accessibility gate |
| `set_text_scale_binding` | `asset_path`, `widget_name`, `binding_spec` | Bind text scale to an accessibility setting |
| `apply_high_contrast_variant` | `asset_path`, `style_map` | Apply high-contrast color overrides to CommonUI styled widgets |

---

## GAS Bridge Aliases (4 — namespace: "ui", source: `MonolithGAS`)

The MonolithGAS module also registers four cross-namespace aliases under `ui::` so that GAS-aware UI authoring tools see them in `ui_query` results without learning a separate namespace. The aliases dispatch to the same handlers as their `gas::` originals — there is no behavioural difference.

| Action (alias) | Canonical | Description |
|---|---|---|
| `ui::bind_widget_to_attribute` | `gas::bind_widget_to_attribute` | Bind a widget's display property to a GAS attribute via `UMonolithGASAttributeBindingClassExtension`. See [SPEC_MonolithGAS.md](SPEC_MonolithGAS.md) for full param schema |
| `ui::unbind_widget_attribute` | `gas::unbind_widget_attribute` | Remove a binding by widget+target_property |
| `ui::list_attribute_bindings` | `gas::list_attribute_bindings` | List all bindings on a Widget Blueprint |
| `ui::clear_widget_attribute_bindings` | `gas::clear_widget_attribute_bindings` | Clear all bindings on a Widget Blueprint |

**Registration site:** `Plugins/Monolith/Source/MonolithGAS/Private/MonolithGASUIBindingActions.cpp:571-577`. Both registrations call the same handler functor — `unbind_widget_attribute` is registered as the no-`s` form in both namespaces.

> **Why this is here, not in MonolithUI source:** the binding logic depends on GAS types (`FGameplayAttribute`, `UAttributeSet`) which would force MonolithUI to take a hard `WITH_GBA` dep. Aliasing keeps MonolithUI buildable in non-GAS projects while still letting `ui_query` callers find the action.

---

## Known Limitations (CommonUI)

1. `convert_button_to_common` does NOT auto-transfer UButton children to UCommonButtonBase — UCommonButtonBase uses an internal widget tree, not AddChild. Callers must rewire manually.
2. `set_initial_focus_target` requires the target WBP to expose a `DesiredFocusTargetName` or `InitialFocusTargetName` FName UPROPERTY and override `NativeGetDesiredFocusTarget`. Action errors out if neither property exists.
3. `show_common_message` is fire-and-forward — async result-binding (Yes/No) requires the dialog WBP to handle internally. No MCP-side delegate routing yet.
4. `dump_action_router_state` cannot read `UCommonUIActionRouterBase::CurrentInputLocks` (private-transient in engine). Engine PR candidate (M0.7).

---
