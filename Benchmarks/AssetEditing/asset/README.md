# AssetEditing: asset

Generated AssetType slice for the AssetEditing benchmark.

## Supported Operations

| Operation | Tasks |
|---|---:|
| `creation_or_import` | 6 |
| `edit` | 6 |
| `save` | 6 |
| `readback_verify` | 6 |

## Lifecycle

| Lifecycle phase | Tasks |
|---|---:|
| `create_save` | 6 |

## Test Cases

| Edit domain | Tasks | Module | Case file |
|---|---:|---|---|
| `batch_delete` | 1 | `asset_authoring.asset.batch_delete` | `testcases\batch_delete.json` |
| `font` | 1 | `asset_authoring.asset.font` | `testcases\font.json` |
| `hygiene` | 1 | `asset_authoring.asset.hygiene` | `testcases\hygiene.json` |
| `texture` | 1 | `asset_authoring.asset.texture` | `testcases\texture.json` |
| `texture_file_import` | 1 | `asset_authoring.asset.texture_file_import` | `testcases\texture_file_import.json` |
| `texture_role_presets` | 1 | `asset_authoring.asset.texture_role_presets` | `testcases\texture_role_presets.json` |

## Common Commands

```powershell
python Scripts\asset_editing_benchmark.py select --testset-module asset_authoring.asset
```

`tasks.jsonl` contains the full task payloads for this AssetType. `testcases\*.json` files contain one edit-domain slice each.
