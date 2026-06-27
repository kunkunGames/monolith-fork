# AssetEditing: logicdriver

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

| Edit domain | Tasks | Module | Case file |
|---|---:|---|---|
| `state_machine_graph` | 1 | `asset_authoring.logicdriver.state_machine_graph` | `testcases\state_machine_graph.json` |

## Common Commands

```powershell
python Scripts\asset_editing_benchmark.py select --testset-module asset_authoring.logicdriver
```

`tasks.jsonl` contains the full task payloads for this AssetType. `testcases\*.json` files contain one edit-domain slice each.
