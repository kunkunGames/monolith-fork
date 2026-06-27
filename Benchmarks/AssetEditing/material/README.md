# AssetEditing: material

Generated AssetType slice for the AssetEditing benchmark.

## Supported Operations

| Operation | Tasks |
|---|---:|
| `creation_or_import` | 21 |
| `edit` | 21 |
| `save` | 21 |
| `readback_verify` | 21 |

## Lifecycle

| Lifecycle phase | Tasks |
|---|---:|
| `create_save` | 21 |

## Test Cases

| Case | Tasks | Module | Case file |
|---|---:|---|---|
| `decal_domain` | 1 | `asset_authoring.material.decal_domain` | `testcases\decal_domain.json` |
| `base` | 1 | `asset_authoring.material.base` | `testcases\base.json` |
| `batch_layout_replace` | 1 | `asset_authoring.material.batch_layout_replace` | `testcases\batch_layout_replace.json` |
| `custom_hlsl` | 1 | `asset_authoring.material.custom_hlsl` | `testcases\custom_hlsl.json` |
| `expression_crud` | 1 | `asset_authoring.material.expression_crud` | `testcases\expression_crud.json` |
| `function` | 1 | `asset_authoring.material.function` | `testcases\function.json` |
| `function_instance` | 1 | `asset_authoring.material.function_instance` | `testcases\function_instance.json` |
| `function_maintenance` | 1 | `asset_authoring.material.function_maintenance` | `testcases\function_maintenance.json` |
| `function_parameter_group_rename` | 1 | `asset_authoring.material.function_parameter_group_rename` | `testcases\function_parameter_group_rename.json` |
| `graph` | 1 | `asset_authoring.material.graph` | `testcases\graph.json` |
| `graph_clear_import` | 1 | `asset_authoring.material.graph_clear_import` | `testcases\graph_clear_import.json` |
| `instance_parameters` | 1 | `asset_authoring.material.instance_parameters` | `testcases\instance_parameters.json` |
| `instance_reparent` | 1 | `asset_authoring.material.instance_reparent` | `testcases\instance_reparent.json` |
| `instance_single_param_delete_batch` | 1 | `asset_authoring.material.instance_single_param_delete_batch` | `testcases\instance_single_param_delete_batch.json` |
| `layer_assets` | 1 | `asset_authoring.material.layer_assets` | `testcases\layer_assets.json` |
| `texture_import_settings` | 1 | `asset_authoring.material.texture_import_settings` | `testcases\texture_import_settings.json` |
| `parameter_inventory` | 1 | `asset_authoring.material.parameter_inventory` | `testcases\parameter_inventory.json` |
| `pbr_material_from_disk` | 1 | `asset_authoring.material.pbr_material_from_disk` | `testcases\pbr_material_from_disk.json` |
| `postprocess_emissive` | 1 | `asset_authoring.material.postprocess_emissive` | `testcases\postprocess_emissive.json` |
| `preview_render` | 1 | `asset_authoring.material.preview_render` | `testcases\preview_render.json` |
| `texture_preview_tiling` | 1 | `asset_authoring.material.texture_preview_tiling` | `testcases\texture_preview_tiling.json` |

## Common Commands

```powershell
python Scripts\asset_editing_benchmark.py select --testset-module asset_authoring.material
```

`tasks.jsonl` contains the full task payloads for this AssetType. `testcases\*.json` files contain one edit-domain slice each.
