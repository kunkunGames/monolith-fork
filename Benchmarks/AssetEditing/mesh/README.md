# AssetEditing: mesh

Generated AssetType slice for the AssetEditing benchmark.

## Supported Operations

| Operation | Tasks |
|---|---:|
| `creation_or_import` | 20 |
| `edit` | 20 |
| `save` | 20 |
| `readback_verify` | 20 |

## Lifecycle

| Lifecycle phase | Tasks |
|---|---:|
| `create_save` | 20 |

## Test Cases

| Edit domain | Tasks | Module | Case file |
|---|---:|---|---|
| `building_shell_static_mesh` | 1 | `asset_authoring.mesh.building_shell_static_mesh` | `testcases\building_shell_static_mesh.json` |
| `fix_quality` | 1 | `asset_authoring.mesh.fix_quality` | `testcases\fix_quality.json` |
| `fragment_static_mesh` | 1 | `asset_authoring.mesh.fragment_static_mesh` | `testcases\fragment_static_mesh.json` |
| `geometry_direct_ops` | 1 | `asset_authoring.mesh.geometry_direct_ops` | `testcases\geometry_direct_ops.json` |
| `geometry_handle_save` | 1 | `asset_authoring.mesh.geometry_handle_save` | `testcases\geometry_handle_save.json` |
| `handle_boolean_remesh_material_ids` | 1 | `asset_authoring.mesh.handle_boolean_remesh_material_ids` | `testcases\handle_boolean_remesh_material_ids.json` |
| `horror_prop_static_mesh` | 1 | `asset_authoring.mesh.horror_prop_static_mesh` | `testcases\horror_prop_static_mesh.json` |
| `maze_static_mesh` | 1 | `asset_authoring.mesh.maze_static_mesh` | `testcases\maze_static_mesh.json` |
| `merge_actors` | 1 | `asset_authoring.mesh.merge_actors` | `testcases\merge_actors.json` |
| `parametric_static_mesh` | 1 | `asset_authoring.mesh.parametric_static_mesh` | `testcases\parametric_static_mesh.json` |
| `plane_cut_mirror_repair` | 1 | `asset_authoring.mesh.plane_cut_mirror_repair` | `testcases\plane_cut_mirror_repair.json` |
| `procedural_pipe_network` | 1 | `asset_authoring.mesh.procedural_pipe_network` | `testcases\procedural_pipe_network.json` |
| `procedural_structure_static_mesh` | 1 | `asset_authoring.mesh.procedural_structure_static_mesh` | `testcases\procedural_structure_static_mesh.json` |
| `procedural_terrain_patch` | 1 | `asset_authoring.mesh.procedural_terrain_patch` | `testcases\procedural_terrain_patch.json` |
| `proxy_mesh_from_scene_actors` | 1 | `asset_authoring.mesh.proxy_mesh_from_scene_actors` | `testcases\proxy_mesh_from_scene_actors.json` |
| `simplify_uv_static_mesh` | 1 | `asset_authoring.mesh.simplify_uv_static_mesh` | `testcases\simplify_uv_static_mesh.json` |
| `static_mesh_auto_lod` | 1 | `asset_authoring.mesh.static_mesh_auto_lod` | `testcases\static_mesh_auto_lod.json` |
| `static_mesh_export` | 1 | `asset_authoring.mesh.static_mesh_export` | `testcases\static_mesh_export.json` |
| `static_mesh_import` | 1 | `asset_authoring.mesh.static_mesh_import` | `testcases\static_mesh_import.json` |
| `static_mesh_lod_quality` | 1 | `asset_authoring.mesh.static_mesh_lod_quality` | `testcases\static_mesh_lod_quality.json` |

## Common Commands

```powershell
python Scripts\asset_editing_benchmark.py select --testset-module asset_authoring.mesh
```

`tasks.jsonl` contains the full task payloads for this AssetType. `testcases\*.json` files contain one edit-domain slice each.
