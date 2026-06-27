# AssetEditing: asset

Generated AssetType slice for the AssetEditing benchmark.

## Supported Operations

| Operation | Tasks |
|---|---:|
| `creation_or_import` | 11 |
| `edit` | 11 |
| `save` | 11 |
| `readback_verify` | 11 |

## Lifecycle

| Lifecycle phase | Tasks |
|---|---:|
| `create_save` | 11 |

## Test Cases

| Case | Tasks | Module | Case file |
|---|---:|---|---|
| `batch_delete` | 1 | `asset_authoring.asset.batch_delete` | `testcases\batch_delete.json` |
| `batch_rename_dry_run` | 1 | `asset_authoring.asset.batch_rename_dry_run` | `testcases\batch_rename_dry_run.json` |
| `delete_assets_guard` | 1 | `asset_authoring.asset.delete_assets_guard` | `testcases\delete_assets_guard.json` |
| `font` | 1 | `asset_authoring.asset.font` | `testcases\font.json` |
| `hygiene` | 1 | `asset_authoring.asset.hygiene` | `testcases\hygiene.json` |
| `texture` | 1 | `asset_authoring.asset.texture` | `testcases\texture.json` |
| `texture_file_conflict_policy` | 1 | `asset_authoring.asset.texture_file_conflict_policy` | `testcases\texture_file_conflict_policy.json` |
| `texture_file_import` | 1 | `asset_authoring.asset.texture_file_import` | `testcases\texture_file_import.json` |
| `texture_postprocess_png` | 1 | `asset_authoring.asset.texture_postprocess_png` | `testcases\texture_postprocess_png.json` |
| `texture_role_presets` | 1 | `asset_authoring.asset.texture_role_presets` | `testcases\texture_role_presets.json` |
| `typed_validation` | 1 | `asset_authoring.asset.typed_validation` | `testcases\typed_validation.json` |

## Common Commands

```powershell
python Scripts\asset_editing_benchmark.py select --testset-module asset_authoring.asset
```

`tasks.jsonl` contains the full task payloads for this AssetType. `testcases\*.json` files contain one edit-domain slice each.
