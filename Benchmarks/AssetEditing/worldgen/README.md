# AssetEditing: worldgen

Generated AssetType slice for the AssetEditing benchmark.

## Supported Operations

| Operation | Tasks |
|---|---:|
| `creation_or_import` | 9 |
| `edit` | 9 |
| `save` | 9 |
| `readback_verify` | 9 |

## Lifecycle

| Lifecycle phase | Tasks |
|---|---:|
| `create_save` | 9 |

## Test Cases

| Case | Tasks | Module | Case file |
|---|---:|---|---|
| `balcony_static_mesh` | 1 | `asset_authoring.worldgen.balcony_static_mesh` | `testcases\balcony_static_mesh.json` |
| `blockout_volume_blueprint` | 1 | `asset_authoring.worldgen.blockout_volume_blueprint` | `testcases\blockout_volume_blueprint.json` |
| `building_static_mesh` | 1 | `asset_authoring.worldgen.building_static_mesh` | `testcases\building_static_mesh.json` |
| `fire_escape_static_mesh` | 1 | `asset_authoring.worldgen.fire_escape_static_mesh` | `testcases\fire_escape_static_mesh.json` |
| `porch_static_mesh` | 1 | `asset_authoring.worldgen.porch_static_mesh` | `testcases\porch_static_mesh.json` |
| `railing_path_static_mesh` | 1 | `asset_authoring.worldgen.railing_path_static_mesh` | `testcases\railing_path_static_mesh.json` |
| `ramp_connector_static_mesh` | 1 | `asset_authoring.worldgen.ramp_connector_static_mesh` | `testcases\ramp_connector_static_mesh.json` |
| `roof_static_mesh` | 1 | `asset_authoring.worldgen.roof_static_mesh` | `testcases\roof_static_mesh.json` |
| `street_static_mesh` | 1 | `asset_authoring.worldgen.street_static_mesh` | `testcases\street_static_mesh.json` |

## Common Commands

```powershell
python Scripts\asset_editing_benchmark.py select --testset-module asset_authoring.worldgen
```

`tasks.jsonl` contains the full task payloads for this AssetType. `testcases\*.json` files contain one edit-domain slice each.
