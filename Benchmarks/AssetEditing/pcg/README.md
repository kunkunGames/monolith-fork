# AssetEditing: pcg

Generated AssetType slice for the AssetEditing benchmark.

## Supported Operations

| Operation | Tasks |
|---|---:|
| `creation_or_import` | 1 |
| `edit` | 1 |
| `save` | 1 |
| `readback_verify` | 1 |

## Lifecycle

| Lifecycle phase | Tasks |
|---|---:|
| `create_save` | 1 |

## Test Cases

| Case | Tasks | Module | Case file |
|---|---:|---|---|
| `graph_authoring` | 1 | `asset_authoring.pcg.graph_authoring` | `testcases\graph_authoring.json` |

## Common Commands

```powershell
python Scripts\asset_editing_benchmark.py select --testset-module asset_authoring.pcg
```

`tasks.jsonl` contains the full task payloads for this AssetType. `testcases\*.json` files contain one edit-domain slice each.
