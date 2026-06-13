---
name: unreal-modelgen
description: Use when submitting or managing AI/procedural 3D model (StaticMesh) generation jobs via Monolith MCP (modelgen namespace) - submit text-to-mesh jobs, read/cancel a job, list providers, import the result as StaticMesh assets, and read generation provenance. For a 2D image/texture generative job use unreal-imagegen; to inspect/edit the generated mesh afterward (LOD/collision/tris/UVs) use unreal-mesh; to import an externally-authored model file (glTF/FBX/USD/OBJ) through the pipeline use unreal-interchange. Triggers on model gen, modelgen, generate model, generate 3D model, mesh generation, AI mesh, generative mesh, text to mesh, text-to-3D, model job, import generated model, model provenance, generation provider.
---

# unreal-modelgen

Drives the **modelgen** namespace for AI/procedural 3D model generation: submit a text-to-StaticMesh job, manage its lifecycle, import the result, and track provenance. **7 actions** via `modelgen_query(action, params)`. Action names below are the live registry surface; call `monolith_discover` for exact parameter schemas.

## Discovery

```
monolith_discover({ namespace: "modelgen" })                      # all actions in this namespace
monolith_discover({ namespace: "modelgen", action: "<action>", mode: "schema" })  # exact params
```

## When to use / Use a different skill for

- **This skill:** submitting and managing a generative 3D MODEL job (text-to-StaticMesh), importing the completed artifact as StaticMesh assets, and reading/attaching generation provenance.
- **unreal-imagegen** — the generative job is a 2D image/texture (PNG/SVG/Texture2D), not a 3D mesh.
- **unreal-mesh** — you need to inspect/edit/validate the mesh AFTER a job completes (tris/verts, LODs, collision, UVs, quality), versus submitting or managing the job here.
- **unreal-interchange** — you are importing an externally-authored model FILE (glTF/FBX/USD/OBJ) through the import pipeline, versus AI/procedural model generation.

## Action Reference

Param notation: `name*` required, `name?` optional, `name=val` default, `a/b/c` allowed, `[w]` mutates. Signatures are a snapshot of the live catalog — for the exact full schema call `monolith_discover` with `mode: "schema"`. The Discovery block above stays the authority.

### Core (7)

| Action | Params | Purpose |
|--------|--------|---------|
| `list_model_generation_providers` | (none) | List Monolith-native generated-model provider boundaries. Remote generation is caller-owned; Monolith imports local artifacts. |
| `get_generated_model_job` | `job_id*` | Read a generated model job manifest by job_id. |
| `get_generated_model_provenance` | `asset_path*` | Read Monolith generation provenance metadata from a StaticMesh asset. |
| `[w] submit_generated_model_job` | `prompt*` `provider?=local_deterministic` `model?=monolith/local-obj-v1` `asset_name?` | Submit a local deterministic text-to-StaticMesh placeholder job. Writes a completed OBJ job under Project/Saved/Monolith/GeneratedModels. |
| `[w] cancel_generated_model_job` | `job_id*` | Cancel a generated model job if it has not already completed. Local deterministic jobs complete immediately. |
| `[w] download_generated_model_result` | `job_id*` | Resolve the local artifact path for a completed generated model job. No network download is performed. |
| `[w] import_generated_model` | `destination*` `job_id?` `file_path?` (used when job_id absent) `provider?=external` `model?=unknown` `prompt?` `source_image_hash?` `replace_existing?=false` `material_import?=create_new` (create_new/find_existing/skip) `save?=true` | Import a completed generated model job or caller-supplied FBX/OBJ/GLB/GLTF file as StaticMesh assets and attach redacted provenance. |

## Common workflows

```
# Submit a local deterministic text-to-StaticMesh job (completes immediately, writes an OBJ under Project/Saved/Monolith/GeneratedModels)
modelgen_query("submit_generated_model_job", { prompt: "<text>" })

# Read the job manifest, then resolve the local artifact path
modelgen_query("get_generated_model_job", { job_id: "<id>" })
modelgen_query("download_generated_model_result", { job_id: "<id>" })

# Import the completed job (or a caller-supplied FBX/OBJ/GLB/GLTF) as StaticMesh assets with redacted provenance (destination is required)
modelgen_query("import_generated_model", { job_id: "<id>", destination: "/Game/GeneratedModels" })

# Read generation provenance back off the imported StaticMesh
modelgen_query("get_generated_model_provenance", { asset_path: "<path>" })
```

## Notes

- This reference is generated from the live `RegisterAction` surface. If an action is missing or renamed, re-run `monolith_discover({ namespace: "modelgen" })` — the catalog is the source of truth.
- Pass `mode: "schema"` to `monolith_discover` for required/optional params and types before calling an action.
- Generation is local and deterministic: `submit_generated_model_job` writes a completed OBJ placeholder job; `cancel_generated_model_job` only matters for jobs that have not already completed, and `download_generated_model_result` resolves a local path with no network download.
- Remote generation is caller-owned. `list_model_generation_providers` reports the Monolith-native provider boundaries; Monolith imports local artifacts rather than calling remote model providers itself.
- After `import_generated_model`, hand the resulting StaticMesh to **unreal-mesh** for LOD/collision/tri-count/UV inspection and editing — that work does not belong in this namespace.
