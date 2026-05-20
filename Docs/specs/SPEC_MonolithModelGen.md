# Monolith — MonolithModelGen Module

**Parent:** [../SPEC_CORE.md](../SPEC_CORE.md)
**Engine:** Unreal Engine 5.7+
**Owner module:** `MonolithModelGen`
**Namespace:** `modelgen`
**MCP tool:** `modelgen_query`
**Status:** Implemented (2026-05-20 namespace split)

---

## 1. Scope

``MonolithModelGen`` owns the ``modelgen`` namespace — text-to-StaticMesh and caller-supplied generated-model jobs, provenance tracking, and import. Split out of ``mesh`` on 2026-05-20 (extracted from ``FMonolithMeshTechArtActions``).

## 2. Namespace ownership

Action implementations now live in `Source/MonolithModelGen` and export through `MONOLITHMODELGEN_API`. `FMonolithModelGenActions` owns model-generation provider/job/import/provenance registration; imports reuse the exported `FMonolithMeshTechArtActions::ImportMesh` helper for the final StaticMesh import path. ``MonolithModelGen::ShutdownModule`` unregisters the ``modelgen`` namespace.

## 3. Registered actions

- `list_model_generation_providers` — list local generated-model provider boundaries.
- `submit_generated_model_job` — submit a local deterministic text-to-StaticMesh job (writes OBJ under `Project/Saved/Monolith/GeneratedModels`).
- `get_generated_model_job` — read a job manifest.
- `cancel_generated_model_job` — cancel a still-cancelable job.
- `download_generated_model_result` — resolve the local artifact path for a completed job.
- `import_generated_model` — import a completed job or caller-supplied FBX/OBJ/GLB/GLTF as StaticMesh + provenance.
- `get_generated_model_provenance` — read provenance metadata from a StaticMesh.

## 4. Build.cs dependencies

Public: `Core`, `CoreUObject`, `Engine`
Private: `MonolithCore`, `MonolithMesh` (for exported mesh import helper and shared mesh-family utilities), `MonolithIndex`, `SQLiteCore`, `UnrealEd`, `EditorSubsystem`, `MeshDescription`, `StaticMeshDescription`, `MeshConversion`, `PhysicsCore`, `NavigationSystem`, `RenderCore`, `RHI`, `EditorScriptingUtilities`, `Json`, `JsonUtilities`, `Slate`, `SlateCore`, `AssetRegistry`, `AssetTools`, `MeshReductionInterface`, `MeshMergeUtilities`, `LevelInstanceEditor`, `ImageCore`.

## 5. Notes

Remote generation is caller-owned; Monolith imports local artifacts only. The public `modelgen` namespace remains stable even though the implementation moved out of `MonolithMesh`.

## 6. Per-action reference

See the namespace section in [SPEC_MonolithMesh.md](SPEC_MonolithMesh.md) for the per-action params/descriptions (the split table at the top of that spec is authoritative for namespace attribution; row params/descriptions remain in-place pending the per-action re-tabulation backlog item in [../TODO.md](../TODO.md)).
