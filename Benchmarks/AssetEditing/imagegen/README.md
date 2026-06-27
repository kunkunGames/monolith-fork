# AssetEditing: imagegen

Generated AssetType slice for the AssetEditing benchmark.

## Supported Operations

| Operation | Tasks |
|---|---:|
| `creation_or_import` | 5 |
| `edit` | 5 |
| `save` | 5 |
| `readback_verify` | 5 |

## Lifecycle

| Lifecycle phase | Tasks |
|---|---:|
| `create_save` | 5 |

## Test Cases

| Case | Tasks | Module | Case file |
|---|---:|---|---|
| `generated_image_import` | 1 | `asset_authoring.imagegen.generated_image_import` | `testcases\generated_image_import.json` |
| `generated_texture` | 1 | `asset_authoring.imagegen.generated_texture` | `testcases\generated_texture.json` |
| `generated_world_tile_texture` | 1 | `asset_authoring.imagegen.generated_world_tile_texture` | `testcases\generated_world_tile_texture.json` |
| `msdf_texture` | 1 | `asset_authoring.imagegen.msdf_texture` | `testcases\msdf_texture.json` |
| `svg_source_boundary` | 1 | `asset_authoring.imagegen.svg_source_boundary` | `testcases\svg_source_boundary.json` |

## Common Commands

```powershell
python Scripts\asset_editing_benchmark.py select --testset-module asset_authoring.imagegen
```

`tasks.jsonl` contains the full task payloads for this AssetType. `testcases\*.json` files contain one edit-domain slice each.
