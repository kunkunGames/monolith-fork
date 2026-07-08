# CommonUI Action Reference (full parameter signatures)

Detailed per-action parameter signatures for the CommonUI-conditional actions of the Monolith **ui** namespace, called via `ui_query()`.

Param notation: `name*` required, `name?` optional, `name=val` default, `a/b/c` allowed values, `[w]` mutates (transaction-wrapped). Accepted aliases noted inline as `name*` (alias `other`). Signatures are a snapshot of the live catalog — for the exact full schema call `monolith_discover({ namespace: "ui", action: "<action>", mode: "schema" })`. The discover-first block in `../SKILL.md` is the authority; do not call from this snapshot alone if param names, aliases, or ranges are load-bearing.

## CommonUI Actions (50, v0.14.0, conditional)

Require the CommonUI engine plugin. Stock in UE 5.7 at `Engine/Plugins/Runtime/CommonUI/`. Build.cs detects via 3-location scan; missing plugin → actions silently unregister.

Filter the listing: `monolith_discover({ namespace: "ui", category: "CommonUI" })`. Runtime-phase actions marked `[RUNTIME]` require a PIE session.

### A: Activatable Lifecycle (8)

Param notation as in the Action Reference above: `name*` required, `name?` optional, `name=val` default, `a/b/c` allowed, `[w]` mutates. `wbp_path` accepts alias `asset_path` where noted in the schema.

| Action | Params | Purpose |
|---|---|---|
| `[w] create_activatable_widget` | `save_path*`, `root_widget=CanvasPanel` | New WBP subclass of UCommonActivatableWidget |
| `[w] create_activatable_stack` | `wbp_path*` (alias `asset_path`), `widget_name*`, `parent_widget?` | Add UCommonActivatableWidgetStack into existing tree |
| `[w] create_activatable_switcher` | `wbp_path*` (alias `asset_path`), `widget_name*`, `parent_widget?` | Add UCommonActivatableWidgetSwitcher |
| `[w] configure_activatable` | `wbp_path*` (alias `asset_path`), `bAutoActivate?`, `bIsModal?`, `bIsBackHandler?`, `activated_visibility?` (ESlateVisibility), `deactivated_visibility?`, `input_mapping?`, `input_mapping_priority?` | Stamp CDO flags |
| `[w] push_to_activatable_stack` `[RUNTIME]` | `container_name*`, `widget_class*` | Push widget class onto named container |
| `[w] pop_activatable_stack` `[RUNTIME]` | `container_name*`, `mode=top` (top/all) | Pop top or clear |
| `get_activatable_stack_state` `[RUNTIME]` | `container_name*` | Depth + top widget |
| `[w] set_activatable_transition` | `wbp_path*` (alias `asset_path`), `widget_name*`, `transition_type?` (ECommonSwitcherTransition), `transition_duration?`, `transition_curve_type?` (ETransitionCurve) | Tune transition (stack/container only) |

### B: Buttons + Styling (9)

| Action | Params | Purpose |
|---|---|---|
| `[w] convert_button_to_common` | `wbp_path*`, `widget_name*`, `target_class?` (UCommonButtonBase subclass; omit for transient) | Replace UButton with UCommonButtonBase (child NOT auto-transferred — see Limitations) |
| `[w] configure_common_button` | `wbp_path*`, `widget_name*`, `is_toggleable?`, `requires_hold?`, `min_width?`, `min_height?`, `max_width?`, `max_height?`, `click_method?` (DownAndUp/MouseDown/MouseUp/PreciseClick), `disabled_reason?` | Set button behavior |
| `[w] create_common_button_style` | `package_path*`, `asset_name*`, `properties?` | Create UCommonButtonStyle asset (returns _C class path) |
| `[w] create_common_text_style` | `package_path*`, `asset_name*`, `properties?` | Create UCommonTextStyle asset |
| `[w] create_common_border_style` | `package_path*`, `asset_name*`, `properties?` | Create UCommonBorderStyle asset |
| `[w] apply_style_to_widget` | `wbp_path*`, `widget_name*`, `style_asset*` (style _C class path) | Assign style class to button/text/border |
| `[w] batch_retheme` | `folder_path*`, `old_style*`, `new_style*` | Swap style class refs across WBPs |
| `[w] configure_common_text` | `wbp_path*`, `widget_name*`, `wrap_text_width?`, `line_height_percentage?`, `mobile_font_size_multiplier?`, `scrolling_enabled?`, `text_case?` (None/ToUpper/ToLower) | UCommonTextBlock props |
| `[w] configure_common_border` | `wbp_path*`, `widget_name*`, `reduce_padding_by_safezone?`, `minimum_padding?` (FMargin text) | UCommonBorder props |
| `[w] convert_textblock_to_common` | `wbp_path*`, `widget_name*` | Replace UTextBlock with UCommonTextBlock |
| `[w] convert_border_to_common` | `wbp_path*` (alias `asset_path`), `widget_name*` | Replace UBorder with UCommonBorder |
| `[w] reparent_widget_root` | `wbp_path*` (alias `asset_path`), `new_class*` (UPanelWidget subclass) | Swap WBP root panel, migrate children |
| `[w] apply_token_binding` | `wbp_path*` (alias `asset_path`), `widget_name*`, `target_property*`, `token_key*` | Bind property to Tokenforge token (validation/probe only; returns not_implemented on commit) |

### C: Input / Actions / Glyphs (7)

| Action | Params | Purpose |
|---|---|---|
| `[w] create_input_action_data_table` | `package_path*`, `asset_name*` | UDataTable of FCommonInputActionDataBase |
| `[w] add_input_action_row` | `table_path*`, `row_name*`, `display_name*`, `hold_display_name?`, `nav_bar_priority?`, `keyboard_key?`, `gamepad_key?`, `touch_key?` (FCommonInputTypeInfo text) | Add action row (struct fields as UE text format) |
| `[w] bind_common_action_widget` | `wbp_path*`, `widget_name*`, `table_path*`, `row_name*` | Point UCommonActionWidget at DataTable row |
| `[w] create_bound_action_bar` | `wbp_path*`, `widget_name*`, `parent_widget?`, `action_button_class?` (UCommonButtonBase _C path, default MonolithDefaultCommonButton_C) | Add UCommonBoundActionBar to tree |
| `[w] set_action_bar_button_class` | `wbp_path*`, `widget_name*`, `button_class*` (UCommonButtonBase subclass) | Set ActionButtonClass on an existing bar |
| `get_active_input_type` `[RUNTIME]` | (none) | Current ECommonInputType from LocalPlayer subsystem |
| `[w] set_input_type_override` `[RUNTIME]` | `input_type*` (MouseAndKeyboard/Gamepad/Touch) | Force input type for test |
| `list_platform_input_tables` | (none) | UCommonInputSettings.ControllerData entries |

### D: Navigation / Focus (8)

| Action | Params | Purpose |
|---|---|---|
| `[w] set_widget_navigation` | `wbp_path*`, `widget_name*`, `direction*` (Up/Down/Left/Right/Next/Previous), `rule*` (Escape/Stop/Wrap/Explicit/Custom/CustomBoundary), `explicit_target?` (required when rule=Explicit) | Set per-direction nav rule |
| `[w] set_widget_navigation_bulk` | `wbp_path*` (alias `asset_path`), `entries*` (array of {widget_name,direction,rule,explicit_target?}), `save=false` | Bulk nav writes, compile once |
| `[w] dump_widget_navigation` | `wbp_path*` (alias `asset_path`), `widget_name?` | Read-only dump of per-direction nav rules |
| `[w] set_initial_focus_target` | `wbp_path*`, `target_widget*` | Stamp DesiredFocusTargetName on UCommonActivatableWidget CDO (WBP must expose property) |
| `[w] audit_focus_chain` | `wbp_path*` (alias `asset_path`) | Static nav-graph audit: unreachable/dead_ends/cycles/dangling_explicit |
| `[w] force_focus` `[RUNTIME]` | `widget_name*` | SetFocus on named live widget |
| `get_focus_path` `[RUNTIME]` | (none) | Slate focus chain leaf→root |
| `[w] request_refresh_focus` `[RUNTIME]` | `widget_name*` | Trigger RequestRefreshFocus on activatable |

### E: Lists / Tabs / Groups / Switchers / Carousel / HW Visibility (7)

| Action | Params | Purpose |
|---|---|---|
| `[w] setup_common_list_view` | `wbp_path*`, `widget_name*`, `entry_class*` (UUserWidget class path), `entry_spacing?`, `pool_size?` (0-100) | Configure UCommonListView / UCommonTileView |
| `[w] create_tab_list_widget` | `save_path*` | New WBP subclass of UCommonTabListWidgetBase |
| `[w] register_tab` `[RUNTIME]` | `tab_list_name*`, `tab_id*`, `button_class*`, `tab_index?` (-1=append) | RegisterTab on live instance |
| `[w] create_button_group` `[RUNTIME]` | `button_names*` (array of FNames), `selection_required?` | UCommonButtonGroupBase wrapping PIE widgets |
| `[w] configure_animated_switcher` | `wbp_path*`, `widget_name*`, `transition_type?` (ECommonSwitcherTransition), `transition_duration?`, `transition_curve_type?` (ETransitionCurve) | UCommonAnimatedSwitcher props |
| `[w] create_widget_carousel` | `wbp_path*`, `widget_name*`, `parent_widget?` | Add UCommonWidgetCarousel to tree |
| `[w] create_hardware_visibility_border` | `wbp_path*`, `widget_name*`, `parent_widget?`, `visibility_query?` (FGameplayTagQuery text) | Add UCommonHardwareVisibilityBorder |

### F: Content (4)

| Action | Params | Purpose |
|---|---|---|
| `[w] configure_numeric_text` | `wbp_path*`, `widget_name*`, `numeric_type?` (Number/Percentage/Seconds/Distance), `current_value?`, `formatting_specification?`, `ease_out_exponent?`, `post_interpolation_shrink_duration?` | UCommonNumericTextBlock |
| `[w] configure_rotator` | `wbp_path*`, `widget_name*`, `labels?` (array), `selected_index?` | UCommonRotator labels |
| `[w] create_lazy_image` | `wbp_path*`, `widget_name*`, `parent_widget?` | Add UCommonLazyImage |
| `[w] create_load_guard` | `wbp_path*`, `widget_name*`, `parent_widget?` | Add UCommonLoadGuard |

### G: Dialog / Modal (2)

| Action | Params | Purpose |
|---|---|---|
| `[w] show_common_message` `[RUNTIME]` | `container_name*`, `dialog_class*` | Push dialog WBP onto named container (fire-and-forward — result binding in dialog WBP) |
| `[w] configure_modal_overlay` | `wbp_path*`, `parent_widget*`, `blur_widget_name=ModalBackdropBlur`, `blur_strength?` | Add UBackgroundBlur behind a parent panel |

### H: Audit + Lint (4)

| Action | Params | Purpose |
|---|---|---|
| `[w] audit_commonui_widget` | `wbp_path*` | Per-WBP lint: missing styles, unbound action widgets, missing focus target |
| `[w] export_commonui_report` | `folder_path?` (default /Game) | Project-wide coverage report: activatable count, button styling ratio, action-widget binding ratio |
| `[w] hot_reload_styles` `[RUNTIME, EXPERIMENTAL]` | (none) | Re-apply styles to all PIE buttons |
| `[w] dump_action_router_state` `[RUNTIME, EXPERIMENTAL]` | (none) | Input subsystem + activatable container states |

### I: Accessibility Bridge (4)

| Action | Params | Purpose |
|---|---|---|
| `[w] enforce_focus_ring` | `folder_path*` | Audit: report unstyled UCommonButtonBase widgets |
| `[w] wrap_with_reduce_motion_gate` | `folder_path*` | Stamp bRespectReduceMotion on WBP CDOs (author must expose property + branch animation on subsystem) |
| `[w] set_text_scale_binding` | `folder_path*` | Stamp bHonorAccessibilityTextScale on WBP CDOs |
| `[w] apply_high_contrast_variant` | `folder_path*`, `normal_style*`, `high_contrast_style*` | Swap style class refs to HC variant |

### J: Templates / Scaffolders

| Action | Params | Purpose |
|---|---|---|
| `[w] scaffold_main_menu` | `save_path*`, `button_names?` (array), `parent_class?`, `action_button_class?`, `action_table?`, `default_style_palette?` | One-shot main-menu WBP with buttons + bound action bar |
| `[w] scaffold_settings_panel_with_tabs` | `save_path*`, `tab_names?` (array), `parent_class?`, `action_table?`, `action_button_class?` | One-shot settings-panel WBP with tab list + switcher + action bar |
| `[w] scaffold_pause_menu` | `save_path*`, `action_table*`, `button_names?` (array), `parent_class?`, `action_button_class?` | One-shot pause-menu WBP with buttons + action bar |

### CommonUI Known Limitations (v0.14.0)

- `convert_button_to_common` does NOT auto-transfer UButton children — UCommonButtonBase uses internal widget tree. Rewire manually.
- `set_initial_focus_target` requires the WBP to expose a `DesiredFocusTargetName` FName UPROPERTY.
- `show_common_message` is fire-and-forward — async result-binding belongs in the dialog WBP.
- `dump_action_router_state` cannot read private `CurrentInputLocks` (engine PR candidate).
- 50 CommonUI actions require PIE session for functional testing — M0.5.1 test-coverage pass pending.
