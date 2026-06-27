# AssetEditing: interchange

Generated AssetType slice for the AssetEditing benchmark.

## Supported Operations

| Operation | Tasks |
|---|---:|
| `creation_or_import` | 8 |
| `edit` | 8 |
| `save` | 8 |
| `readback_verify` | 8 |

## Lifecycle

| Lifecycle phase | Tasks |
|---|---:|
| `create_save` | 8 |

## Test Cases

| Edit domain | Tasks | Module | Case file |
|---|---:|---|---|
| `interchange_audio_import` | 1 | `asset_authoring.interchange.audio_import` | `testcases\audio_import.json` |
| `interchange_batch_import` | 1 | `asset_authoring.interchange.batch_import` | `testcases\batch_import.json` |
| `interchange_batch_reimport` | 1 | `asset_authoring.interchange.batch_reimport` | `testcases\batch_reimport.json` |
| `interchange_generic_import` | 1 | `asset_authoring.interchange.generic_import` | `testcases\generic_import.json` |
| `interchange_generic_options_import` | 1 | `asset_authoring.interchange.generic_options_import` | `testcases\generic_options_import.json` |
| `interchange_scene_import` | 1 | `asset_authoring.interchange.scene_import` | `testcases\scene_import.json` |
| `interchange_static_mesh_reimport_export` | 1 | `asset_authoring.interchange.static_mesh_reimport_export` | `testcases\static_mesh_reimport_export.json` |
| `interchange_texture_import` | 1 | `asset_authoring.interchange.texture_import` | `testcases\texture_import.json` |

## Common Commands

```powershell
python Scripts\asset_editing_benchmark.py select --testset-module asset_authoring.interchange
```

`tasks.jsonl` contains the full task payloads for this AssetType. `testcases\*.json` files contain one edit-domain slice each.
