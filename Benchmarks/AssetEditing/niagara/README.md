# AssetEditing: niagara

Generated AssetType slice for the AssetEditing benchmark.

## Supported Operations

| Operation | Tasks |
|---|---:|
| `creation_or_import` | 28 |
| `edit` | 28 |
| `save` | 28 |
| `readback_verify` | 28 |

## Lifecycle

| Lifecycle phase | Tasks |
|---|---:|
| `create_save` | 28 |

## Test Cases

| Edit domain | Tasks | Module | Case file |
|---|---:|---|---|
| `curve_di_key_edit` | 1 | `asset_authoring.niagara.curve_di_key_edit` | `testcases\curve_di_key_edit.json` |
| `custom_hlsl_update` | 1 | `asset_authoring.niagara.custom_hlsl_update` | `testcases\custom_hlsl_update.json` |
| `niagara_clear_emitter_modules` | 1 | `asset_authoring.niagara.clear_emitter_modules` | `testcases\clear_emitter_modules.json` |
| `niagara_data_interface_curve` | 1 | `asset_authoring.niagara.data_interface_curve` | `testcases\data_interface_curve.json` |
| `niagara_duplicate_diff` | 1 | `asset_authoring.niagara.duplicate_diff` | `testcases\duplicate_diff.json` |
| `niagara_dynamic_input` | 1 | `asset_authoring.niagara.dynamic_input` | `testcases\dynamic_input.json` |
| `niagara_effect_type_scalability` | 1 | `asset_authoring.niagara.effect_type_scalability` | `testcases\effect_type_scalability.json` |
| `niagara_emitter_properties` | 1 | `asset_authoring.niagara.emitter_properties` | `testcases\emitter_properties.json` |
| `niagara_emitter_template_spec` | 1 | `asset_authoring.niagara.emitter_template_spec` | `testcases\emitter_template_spec.json` |
| `niagara_emitter_topology` | 1 | `asset_authoring.niagara.emitter_topology` | `testcases\emitter_topology.json` |
| `niagara_event_handler_metadata` | 1 | `asset_authoring.niagara.event_handler_metadata` | `testcases\event_handler_metadata.json` |
| `niagara_hlsl_function` | 1 | `asset_authoring.niagara.hlsl_function` | `testcases\hlsl_function.json` |
| `niagara_import_system_spec` | 1 | `asset_authoring.niagara.import_system_spec` | `testcases\import_system_spec.json` |
| `niagara_mesh_ribbon_renderer` | 1 | `asset_authoring.niagara.mesh_ribbon_renderer` | `testcases\mesh_ribbon_renderer.json` |
| `niagara_module_duplicate_capture` | 1 | `asset_authoring.niagara.module_duplicate_capture` | `testcases\module_duplicate_capture.json` |
| `niagara_module_script` | 1 | `asset_authoring.niagara.module_script` | `testcases\module_script.json` |
| `niagara_module_stack_state` | 1 | `asset_authoring.niagara.module_stack_state` | `testcases\module_stack_state.json` |
| `niagara_renderer_material` | 1 | `asset_authoring.niagara.renderer_material` | `testcases\renderer_material.json` |
| `niagara_renderer_property_binding` | 1 | `asset_authoring.niagara.renderer_property_binding` | `testcases\renderer_property_binding.json` |
| `niagara_sidecar_assets` | 1 | `asset_authoring.niagara.sidecar_assets` | `testcases\sidecar_assets.json` |
| `niagara_sim_stage_lifetime` | 1 | `asset_authoring.niagara.sim_stage_lifetime` | `testcases\sim_stage_lifetime.json` |
| `niagara_sprite_renderer_subuv` | 1 | `asset_authoring.niagara.sprite_renderer_subuv` | `testcases\sprite_renderer_subuv.json` |
| `niagara_system` | 1 | `asset_authoring.niagara.system` | `testcases\system.json` |
| `niagara_user_parameters` | 1 | `asset_authoring.niagara.user_parameters` | `testcases\user_parameters.json` |
| `npc_parameter_remove` | 1 | `asset_authoring.niagara.npc_parameter_remove` | `testcases\npc_parameter_remove.json` |
| `system_effect_type_assignment` | 1 | `asset_authoring.niagara.system_effect_type_assignment` | `testcases\system_effect_type_assignment.json` |
| `system_timing` | 1 | `asset_authoring.niagara.system_timing` | `testcases\system_timing.json` |
| `user_parameter_remove` | 1 | `asset_authoring.niagara.user_parameter_remove` | `testcases\user_parameter_remove.json` |

## Common Commands

```powershell
python Scripts\asset_editing_benchmark.py select --testset-module asset_authoring.niagara
```

`tasks.jsonl` contains the full task payloads for this AssetType. `testcases\*.json` files contain one edit-domain slice each.
