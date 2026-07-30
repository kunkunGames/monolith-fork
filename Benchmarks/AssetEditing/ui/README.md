# AssetEditing: ui

Generated AssetType slice for the AssetEditing benchmark.

## Supported Operations

| Operation | Tasks |
|---|---:|
| `creation_or_import` | 23 |
| `edit` | 23 |
| `save` | 23 |
| `readback_verify` | 23 |

## Lifecycle

| Lifecycle phase | Tasks |
|---|---:|
| `create_save` | 23 |

## Test Cases

| Case | Tasks | Module | Case file |
|---|---:|---|---|
| `animation_inspection_delta_binding` | 1 | `asset_authoring.ui.animation_inspection_delta_binding` | `testcases\animation_inspection_delta_binding.json` |
| `box_shadow_mid` | 1 | `asset_authoring.ui.box_shadow_mid` | `testcases\box_shadow_mid.json` |
| `commonui_action_widget_binding` | 1 | `asset_authoring.ui.commonui_action_widget_binding` | `testcases\commonui_action_widget_binding.json` |
| `commonui_container_navigation` | 1 | `asset_authoring.ui.commonui_container_navigation` | `testcases\commonui_container_navigation.json` |
| `commonui_content_framework` | 1 | `asset_authoring.ui.commonui_content_framework` | `testcases\commonui_content_framework.json` |
| `commonui_input_action_datatable` | 1 | `asset_authoring.ui.commonui_input_action_datatable` | `testcases\commonui_input_action_datatable.json` |
| `commonui_pause_menu_focus` | 1 | `asset_authoring.ui.commonui_pause_menu_focus` | `testcases\commonui_pause_menu_focus.json` |
| `commonui_style_assets` | 1 | `asset_authoring.ui.commonui_style_assets` | `testcases\commonui_style_assets.json` |
| `commonui_styled_button` | 1 | `asset_authoring.ui.commonui_styled_button` | `testcases\commonui_styled_button.json` |
| `commonui_text_block` | 1 | `asset_authoring.ui.commonui_text_block` | `testcases\commonui_text_block.json` |
| `list_view_binding` | 1 | `asset_authoring.ui.list_view_binding` | `testcases\list_view_binding.json` |
| `registry_layout_accessibility` | 1 | `asset_authoring.ui.registry_layout_accessibility` | `testcases\registry_layout_accessibility.json` |
| `settings_notification_templates` | 1 | `asset_authoring.ui.settings_notification_templates` | `testcases\settings_notification_templates.json` |
| `uispec_diff_patch` | 1 | `asset_authoring.ui.uispec_diff_patch` | `testcases\uispec_diff_patch.json` |
| `widget_animation_advanced_events` | 1 | `asset_authoring.ui.widget_animation_advanced_events` | `testcases\widget_animation_advanced_events.json` |
| `widget_animation_remove` | 1 | `asset_authoring.ui.widget_animation_remove` | `testcases\widget_animation_remove.json` |
| `widget_animation_v2` | 1 | `asset_authoring.ui.widget_animation_v2` | `testcases\widget_animation_v2.json` |
| `widget_blueprint` | 1 | `asset_authoring.ui.widget_blueprint` | `testcases\widget_blueprint.json` |
| `widget_slot_property_variable` | 1 | `asset_authoring.ui.widget_slot_property_variable` | `testcases\widget_slot_property_variable.json` |
| `widget_spec_builder` | 1 | `asset_authoring.ui.widget_spec_builder` | `testcases\widget_spec_builder.json` |
| `widget_styling_actions` | 1 | `asset_authoring.ui.widget_styling_actions` | `testcases\widget_styling_actions.json` |
| `widget_templates` | 1 | `asset_authoring.ui.widget_templates` | `testcases\widget_templates.json` |
| `widget_tree_maintenance` | 1 | `asset_authoring.ui.widget_tree_maintenance` | `testcases\widget_tree_maintenance.json` |

## Common Commands

```powershell
python Scripts\asset_editing_benchmark.py select --testset-module asset_authoring.ui
```

`tasks.jsonl` contains the full task payloads for this AssetType. `testcases\*.json` files contain one edit-domain slice each.
