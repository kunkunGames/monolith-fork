# AssetEditing: project

Generated AssetType slice for the AssetEditing benchmark.

## Supported Operations

| Operation | Tasks |
|---|---:|
| `creation_or_import` | 2 |
| `edit` | 2 |
| `save` | 2 |
| `readback_verify` | 2 |

## Lifecycle

| Lifecycle phase | Tasks |
|---|---:|
| `create_save` | 2 |

## Test Cases

| Edit domain | Tasks | Module | Case file |
|---|---:|---|---|
| `index_details_text` | 1 | `asset_authoring.project.index_details_text` | `testcases\index_details_text.json` |
| `saved_asset_state` | 1 | `asset_authoring.project.saved_asset_state` | `testcases\saved_asset_state.json` |

## Common Commands

```powershell
python Scripts\asset_editing_benchmark.py select --testset-module asset_authoring.project
```

`tasks.jsonl` contains the full task payloads for this AssetType. `testcases\*.json` files contain one edit-domain slice each.
