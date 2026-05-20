---
name: unreal-modelgen
description: "Use for AI/procedural model generation workflows exposed by Monolith MCP: submit and manage mesh/model generation jobs. Triggers on model gen, modelgen, generate model, mesh generation, AI mesh, generative mesh, model job."
---

# unreal-modelgen

**7 actions** via `modelgen_query(action, params)`. Action names below are the live registry surface; call `monolith_discover` for exact parameter schemas.

## Discovery

```
monolith_discover({ namespace: "modelgen" })                      # all actions in this namespace
monolith_discover({ namespace: "modelgen", action: "<action>", mode: "schema" })  # exact params
```

## Action Reference

### Core (7)

| Action | Purpose |
|--------|---------|
| `cancel_generated_model_job` | Cancel a generated model job if it has not already completed. Local deterministic jobs complete immediately. |
| `download_generated_model_result` | Resolve the local artifact path for a completed generated model job. No network download is performed. |
| `get_generated_model_job` | Read a generated model job manifest by job_id. |
| `get_generated_model_provenance` | Read Monolith generation provenance metadata from a StaticMesh asset. |
| `import_generated_model` | Import a completed generated model job or caller-supplied FBX/OBJ/GLB/GLTF file as StaticMesh assets and attach redacted provenance. |
| `list_model_generation_providers` | List Monolith-native generated-model provider boundaries. Remote generation is caller-owned; Monolith imports local artifacts. |
| `submit_generated_model_job` | Submit a local deterministic text-to-StaticMesh placeholder job. Writes a completed OBJ job under Project/Saved/Monolith/GeneratedModels. |

## Notes

- This reference is generated from the live `RegisterAction` surface. If an action is missing or renamed, re-run `monolith_discover({ namespace: "modelgen" })` — the catalog is the source of truth.
- Pass `mode: "schema"` to `monolith_discover` for required/optional params and types before calling an action.
