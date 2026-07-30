# AssetEditing: pcg

Generated AssetType slice for the AssetEditing benchmark.

## Supported Operations

| Operation | Tasks |
|---|---:|
| `creation_or_import` | 6 |
| `edit` | 6 |
| `save` | 6 |
| `readback_verify` | 6 |

## Lifecycle

| Lifecycle phase | Tasks |
|---|---:|
| `create_save` | 6 |

## Test Cases

| Case | Tasks | Module | Case file |
|---|---:|---|---|
| `graph_authoring` | 1 | `asset_authoring.pcg.graph_authoring` | `testcases\graph_authoring.json` |
| `graph_mutation` | 1 | `asset_authoring.pcg.graph_mutation` | `testcases\graph_mutation.json` |
| `graph_replacement` | 1 | `asset_authoring.pcg.graph_replacement` | `testcases\graph_replacement.json` |
| `graph_user_parameters` | 1 | `asset_authoring.pcg.graph_user_parameters` | `testcases\graph_user_parameters.json` |
| `subgraph_assignment` | 1 | `asset_authoring.pcg.subgraph_assignment` | `testcases\subgraph_assignment.json` |
| `surface_discovery` | 1 | `asset_authoring.pcg.surface_discovery` | `testcases\surface_discovery.json` |

## Common Commands

```powershell
python Scripts\asset_editing_benchmark.py select --testset-module asset_authoring.pcg
```

`tasks.jsonl` contains the full task payloads for this AssetType. `testcases\*.json` files contain one edit-domain slice each.
