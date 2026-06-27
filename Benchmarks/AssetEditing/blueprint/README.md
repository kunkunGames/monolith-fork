# AssetEditing: blueprint

Generated AssetType slice for the AssetEditing benchmark.

## Supported Operations

| Operation | Tasks |
|---|---:|
| `creation_or_import` | 4 |
| `edit` | 4 |
| `save` | 4 |
| `readback_verify` | 4 |

## Lifecycle

| Lifecycle phase | Tasks |
|---|---:|
| `create_save` | 4 |

## Test Cases

| Case | Tasks | Module | Case file |
|---|---:|---|---|
| `asset_types` | 1 | `asset_authoring.blueprint.asset_types` | `testcases\asset_types.json` |
| `duplicate_reparent` | 1 | `asset_authoring.blueprint.duplicate_reparent` | `testcases\duplicate_reparent.json` |
| `spec_builder` | 1 | `asset_authoring.blueprint.spec_builder` | `testcases\spec_builder.json` |
| `timeline_persistence` | 1 | `asset_authoring.blueprint.timeline_persistence` | `testcases\timeline_persistence.json` |

## Common Commands

```powershell
python Scripts\asset_editing_benchmark.py select --testset-module asset_authoring.blueprint
```

`tasks.jsonl` contains the full task payloads for this AssetType. `testcases\*.json` files contain one edit-domain slice each.
