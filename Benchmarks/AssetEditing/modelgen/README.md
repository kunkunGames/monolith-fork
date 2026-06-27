# AssetEditing: modelgen

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
| `generated_model_job_lifecycle` | 1 | `asset_authoring.modelgen.generated_model_job_lifecycle` | `testcases\generated_model_job_lifecycle.json` |
| `generated_static_mesh` | 1 | `asset_authoring.modelgen.generated_static_mesh` | `testcases\generated_static_mesh.json` |

## Common Commands

```powershell
python Scripts\asset_editing_benchmark.py select --testset-module asset_authoring.modelgen
```

`tasks.jsonl` contains the full task payloads for this AssetType. `testcases\*.json` files contain one edit-domain slice each.
