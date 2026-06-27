# AssetEditing: hlod

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

| Case | Tasks | Module | Case file |
|---|---:|---|---|
| `layer` | 1 | `asset_authoring.hlod.layer` | `testcases\layer.json` |
| `mesh_setup_layer` | 1 | `asset_authoring.hlod.mesh_setup_layer` | `testcases\mesh_setup_layer.json` |

## Common Commands

```powershell
python Scripts\asset_editing_benchmark.py select --testset-module asset_authoring.hlod
```

`tasks.jsonl` contains the full task payloads for this AssetType. `testcases\*.json` files contain one edit-domain slice each.
