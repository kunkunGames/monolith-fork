# AssetEditing: collection

Generated AssetType slice for the AssetEditing benchmark.

## Supported Operations

| Operation | Tasks |
|---|---:|
| `creation_or_import` | 5 |
| `edit` | 5 |
| `save` | 4 |
| `readback_verify` | 5 |

## Lifecycle

| Lifecycle phase | Tasks |
|---|---:|
| `create` | 1 |
| `create_save` | 4 |

## Test Cases

| Case | Tasks | Module | Case file |
|---|---:|---|---|
| `content_browser_collection` | 1 | `asset_authoring.collection.content_browser_collection` | `testcases\content_browser_collection.json` |
| `content_browser_collection_remove` | 1 | `asset_authoring.collection.content_browser_collection_remove` | `testcases\content_browser_collection_remove.json` |
| `content_browser_dynamic_collection` | 1 | `asset_authoring.collection.content_browser_dynamic_collection` | `testcases\content_browser_dynamic_collection.json` |
| `delete_readback` | 1 | `asset_authoring.collection.delete_readback` | `testcases\delete_readback.json` |
| `unique_name_validation` | 1 | `asset_authoring.collection.unique_name_validation` | `testcases\unique_name_validation.json` |

## Common Commands

```powershell
python Scripts\asset_editing_benchmark.py select --testset-module asset_authoring.collection
```

`tasks.jsonl` contains the full task payloads for this AssetType. `testcases\*.json` files contain one edit-domain slice each.
