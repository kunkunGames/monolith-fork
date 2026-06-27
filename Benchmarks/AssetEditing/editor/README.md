# AssetEditing: editor

Generated AssetType slice for the AssetEditing benchmark.

## Supported Operations

| Operation | Tasks |
|---|---:|
| `creation_or_import` | 5 |
| `edit` | 5 |
| `save` | 4 |
| `readback_verify` | 5 |

## Lifecycle

| Lifecycle phase | Tasks |
|---|---:|
| `create` | 1 |
| `create_save` | 4 |

## Test Cases

| Case | Tasks | Module | Case file |
|---|---:|---|---|
| `empty_map_asset` | 1 | `asset_authoring.editor.empty_map_asset` | `testcases\empty_map_asset.json` |
| `map_actor_defaults` | 1 | `asset_authoring.editor.map_actor_defaults` | `testcases\map_actor_defaults.json` |
| `nav_harness_map` | 1 | `asset_authoring.editor.nav_harness_map` | `testcases\nav_harness_map.json` |
| `package_dirty_save` | 1 | `asset_authoring.editor.package_dirty_save` | `testcases\package_dirty_save.json` |
| `texture_flipbook_atlas` | 1 | `asset_authoring.editor.texture_flipbook_atlas` | `testcases\texture_flipbook_atlas.json` |

## Common Commands

```powershell
python Scripts\asset_editing_benchmark.py select --testset-module asset_authoring.editor
```

`tasks.jsonl` contains the full task payloads for this AssetType. `testcases\*.json` files contain one edit-domain slice each.
