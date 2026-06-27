# AssetEditing: collection

Generated AssetType slice for the AssetEditing benchmark.

## Supported Operations

| Operation | Tasks |
|---|---:|
| `creation_or_import` | 4 |
| `edit` | 4 |
| `save` | 3 |
| `readback_verify` | 4 |

## Lifecycle

| Lifecycle phase | Tasks |
|---|---:|
| `create` | 1 |
| `create_save` | 3 |

## Test Cases

| Edit domain | Tasks | Module | Case file |
|---|---:|---|---|
| `content_browser_collection` | 1 | `asset_authoring.collection.content_browser_collection` | `testcases\content_browser_collection.json` |
| `content_browser_collection_remove` | 1 | `asset_authoring.collection.content_browser_collection_remove` | `testcases\content_browser_collection_remove.json` |
| `content_browser_dynamic_collection` | 1 | `asset_authoring.collection.content_browser_dynamic_collection` | `testcases\content_browser_dynamic_collection.json` |
| `delete_readback` | 1 | `asset_authoring.collection.delete_readback` | `testcases\delete_readback.json` |

## Common Commands

```powershell
python Scripts\asset_editing_benchmark.py select --testset-module asset_authoring.collection
```

`tasks.jsonl` contains the full task payloads for this AssetType. `testcases\*.json` files contain one edit-domain slice each.
