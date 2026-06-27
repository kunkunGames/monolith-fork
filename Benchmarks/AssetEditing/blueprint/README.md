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

| Edit domain | Tasks | Module | Case file |
|---|---:|---|---|
| `blueprint_asset_types` | 1 | `asset_authoring.blueprint.asset_types` | `testcases\asset_types.json` |
| `blueprint_duplicate_reparent` | 1 | `asset_authoring.blueprint.duplicate_reparent` | `testcases\duplicate_reparent.json` |
| `blueprint_spec_builder` | 1 | `asset_authoring.blueprint.spec_builder` | `testcases\spec_builder.json` |
| `blueprint_timeline_persistence` | 1 | `asset_authoring.blueprint.timeline_persistence` | `testcases\timeline_persistence.json` |

## Common Commands

```powershell
python Scripts\asset_editing_benchmark.py select --testset-module asset_authoring.blueprint
```

`tasks.jsonl` contains the full task payloads for this AssetType. `testcases\*.json` files contain one edit-domain slice each.
