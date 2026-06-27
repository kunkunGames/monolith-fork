# AssetEditing: input

Generated AssetType slice for the AssetEditing benchmark.

## Supported Operations

| Operation | Tasks |
|---|---:|
| `creation_or_import` | 5 |
| `edit` | 5 |
| `save` | 5 |
| `readback_verify` | 5 |

## Lifecycle

| Lifecycle phase | Tasks |
|---|---:|
| `create_save` | 5 |

## Test Cases

| Edit domain | Tasks | Module | Case file |
|---|---:|---|---|
| `axis2d_movement_mapping` | 1 | `asset_authoring.input.axis2d_movement_mapping` | `testcases\axis2d_movement_mapping.json` |
| `axis3d_mapping` | 1 | `asset_authoring.input.axis3d_mapping` | `testcases\axis3d_mapping.json` |
| `duplicate_key_conflict` | 1 | `asset_authoring.input.duplicate_key_conflict` | `testcases\duplicate_key_conflict.json` |
| `enhanced_input` | 1 | `asset_authoring.input.enhanced_input` | `testcases\enhanced_input.json` |
| `enhanced_input_mapping_removal` | 1 | `asset_authoring.input.enhanced_input_mapping_removal` | `testcases\enhanced_input_mapping_removal.json` |

## Common Commands

```powershell
python Scripts\asset_editing_benchmark.py select --testset-module asset_authoring.input
```

`tasks.jsonl` contains the full task payloads for this AssetType. `testcases\*.json` files contain one edit-domain slice each.
