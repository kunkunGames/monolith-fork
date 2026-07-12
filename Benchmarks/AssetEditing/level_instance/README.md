# AssetEditing: level_instance

Generated AssetType slice for the AssetEditing benchmark.

## Supported Operations

| Operation | Tasks |
|---|---:|
| `creation_or_import` | 3 |
| `edit` | 3 |
| `save` | 3 |
| `readback_verify` | 3 |

## Lifecycle

| Lifecycle phase | Tasks |
|---|---:|
| `create_save` | 3 |

## Test Cases

| Case | Tasks | Module | Case file |
|---|---:|---|---|
| `blueprint_prefab` | 1 | `asset_authoring.level_instance.blueprint_prefab` | `testcases\blueprint_prefab.json` |
| `blueprint_prefab_guard` | 1 | `asset_authoring.level_instance.blueprint_prefab_guard` | `testcases\blueprint_prefab_guard.json` |
| `prefab_placement` | 1 | `asset_authoring.level_instance.prefab_placement` | `testcases\prefab_placement.json` |

## Common Commands

```powershell
python Scripts\asset_editing_benchmark.py select --testset-module asset_authoring.level_instance
```

`tasks.jsonl` contains the full task payloads for this AssetType. `testcases\*.json` files contain one edit-domain slice each.
