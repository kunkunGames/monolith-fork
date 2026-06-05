---
name: unreal-interchange
description: "Use for the Interchange import/export framework via Monolith MCP: import pipelines and pipeline stacks, glTF/FBX/USD import, and reimport. Triggers on interchange, import, reimport, gltf, fbx, usd, obj, import pipeline, pipeline stack, asset import, source data."
---

# unreal-interchange

**16 actions** via `interchange_query(action, params)`. Action names below are the live registry surface; call `monolith_discover` for exact parameter schemas.

## Discovery

```
monolith_discover({ namespace: "interchange" })                      # all actions in this namespace
monolith_discover({ namespace: "interchange", action: "<action>", mode: "schema" })  # exact params
```

## Action Reference

### Core (16)

| Action | Purpose |
|--------|---------|
| `can_import` | Validate whether a source file can be handed to an Interchange import workflow. |
| `can_reimport` | Check whether an existing asset has source import data usable for reimport. |
| `export_asset` | Export one asset to a local file through UAssetExportTask after path validation. |
| `get_import_data` | Read import source metadata from an existing asset without mutation. |
| `get_supported_formats` | List Monolith Interchange import/export validation capabilities without mutating assets. |
| `import_asset` | Import one source file with root, destination, conflict, confirmation, and dry-run guardrails. |
| `import_assets` | Import multiple source files sequentially and return one result row per source. |
| `import_audio` | Typed audio import entrypoint over the guarded Interchange import implementation. |
| `import_mesh` | Typed mesh import entrypoint over the guarded Interchange import implementation. |
| `import_scene` | Typed scene import entrypoint over the guarded Interchange import implementation. |
| `import_skeletal_mesh` | Typed skeletal mesh import entrypoint over the guarded Interchange import implementation. |
| `import_texture` | Typed texture import entrypoint over the guarded Interchange import implementation. |
| `import_with_options` | Guarded import entrypoint that accepts a forward-compatible options object. |
| `reimport_asset` | Reimport one existing asset through Unreal's reimport manager. |
| `reimport_assets` | Reimport multiple assets sequentially and return one result row per asset. |
| `update_reimport_path` | Update an asset reimport source path after source/root validation. |

## Notes

- This reference is generated from the live `RegisterAction` surface. If an action is missing or renamed, re-run `monolith_discover({ namespace: "interchange" })` — the catalog is the source of truth.
- Pass `mode: "schema"` to `monolith_discover` for required/optional params and types before calling an action.
