# AssetEditing: data

Generated AssetType slice for the AssetEditing benchmark.

## Supported Operations

| Operation | Tasks |
|---|---:|
| `creation_or_import` | 12 |
| `edit` | 12 |
| `save` | 12 |
| `readback_verify` | 12 |

## Lifecycle

| Lifecycle phase | Tasks |
|---|---:|
| `create_save` | 12 |

## Test Cases

| Case | Tasks | Module | Case file |
|---|---:|---|---|
| `curvetable` | 1 | `asset_authoring.data.curvetable` | `testcases\curvetable.json` |
| `curvetable_row_maintenance` | 1 | `asset_authoring.data.curvetable_row_maintenance` | `testcases\curvetable_row_maintenance.json` |
| `asset` | 1 | `asset_authoring.data.asset` | `testcases\asset.json` |
| `asset_bulk_cdo_schema` | 1 | `asset_authoring.data.asset_bulk_cdo_schema` | `testcases\asset_bulk_cdo_schema.json` |
| `asset_property_path` | 1 | `asset_authoring.data.asset_property_path` | `testcases\asset_property_path.json` |
| `datatable` | 1 | `asset_authoring.data.datatable` | `testcases\datatable.json` |
| `datatable_bulk_import_export` | 1 | `asset_authoring.data.datatable_bulk_import_export` | `testcases\datatable_bulk_import_export.json` |
| `datatable_dry_run_strict_apply` | 1 | `asset_authoring.data.datatable_dry_run_strict_apply` | `testcases\datatable_dry_run_strict_apply.json` |
| `datatable_schema_csv_file` | 1 | `asset_authoring.data.datatable_schema_csv_file` | `testcases\datatable_schema_csv_file.json` |
| `datatable_schema_readback` | 1 | `asset_authoring.data.datatable_schema_readback` | `testcases\datatable_schema_readback.json` |
| `datatable_strict_rejection` | 1 | `asset_authoring.data.datatable_strict_rejection` | `testcases\datatable_strict_rejection.json` |
| `seed_data_asset` | 1 | `asset_authoring.data.seed_data_asset` | `testcases\seed_data_asset.json` |

## Common Commands

```powershell
python Scripts\asset_editing_benchmark.py select --testset-module asset_authoring.data
```

`tasks.jsonl` contains the full task payloads for this AssetType. `testcases\*.json` files contain one edit-domain slice each.
