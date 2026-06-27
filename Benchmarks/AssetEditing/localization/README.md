# AssetEditing: localization

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
| `stringtable` | 1 | `asset_authoring.localization.stringtable` | `testcases\stringtable.json` |
| `stringtable_blueprint_bulk` | 1 | `asset_authoring.localization.stringtable_blueprint_bulk` | `testcases\stringtable_blueprint_bulk.json` |
| `stringtable_csv` | 1 | `asset_authoring.localization.stringtable_csv` | `testcases\stringtable_csv.json` |
| `stringtable_export_remove` | 1 | `asset_authoring.localization.stringtable_export_remove` | `testcases\stringtable_export_remove.json` |
| `stringtable_metadata_remove` | 1 | `asset_authoring.localization.stringtable_metadata_remove` | `testcases\stringtable_metadata_remove.json` |

## Common Commands

```powershell
python Scripts\asset_editing_benchmark.py select --testset-module asset_authoring.localization
```

`tasks.jsonl` contains the full task payloads for this AssetType. `testcases\*.json` files contain one edit-domain slice each.
