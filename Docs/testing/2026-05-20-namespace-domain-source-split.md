# Monolith Namespace Domain Source Split Verification

**Date:** 2026-05-20
**Scope:** `Plugins/Monolith`
**Owner:** Monolith routing/cohesion refactor
**Result:** Passed with pre-existing index health warnings

---

## 1. Change Under Test

Mesh-family namespace implementations were physically moved out of `MonolithMesh` into their owning source modules:

| Namespace | Owning source module | Notes |
|-----------|----------------------|-------|
| `scene` | `MonolithScene` | Actor, spatial, volume, lighting, decal, spatial registry, auto-volume, and debug-view actions now compile in the scene module. |
| `leveldesign` | `MonolithLevelDesign` | Horror, encounter, accessibility, acoustic, and level-design quality actions now compile in the level-design module. |
| `worldgen` | `MonolithWorldGen` | Blockout, templates, presets, context props, floor plans, furnishing, building validation, and GeometryScript town-generation actions now compile in the world-gen module. |
| `modelgen` | `MonolithModelGen` | Generated-model provider/job/import/provenance actions now compile in the model-gen module. |
| `mesh` | `MonolithMesh` | Mesh inspection, operations, validation, performance, actor merge, HLOD, level instance, procedural mesh, cache, and tech-art actions remain in the mesh module. |

---

## 2. Verification Results

| Check | Command / source | Result |
|-------|------------------|--------|
| Full editor target build | `UnrealBuildTool.exe GoGameEditor Win64 Development -Project="D:\P4\game\GO.uproject" -WaitMutex -NoHotReloadFromIDE` | Passed. |
| Source CRG rebuild | `Plugins\Monolith\Binaries\monolith_query.exe source repair_crg_cache --execute` | Passed after waiting for a prior parallel health query to release the SQLite lock. Built `crg_nodes`, `crg_edges`, and `crg_node_metrics` from `Saved\EngineSource.db`. |
| Source index health | `Plugins\Monolith\Binaries\monolith_query.exe source health` | Core tables, FTS, triggers, schema, CRG tables, parity, metrics, and scoring passed. Existing warning remains for orphan reference rows. |
| Project index health | `Plugins\Monolith\Binaries\monolith_query.exe project health` | Project CRG parity passed. Existing warning remains for missing `meta.schema_version`. |
| Static namespace counts | `rg -F 'RegisterAction(TEXT("<namespace>")' Plugins\Monolith\Source\...` | `mesh=66`, `worldgen=63`, `leveldesign=61`, `modelgen=7`, and scene-module-owned `scene=58` registrations. |
| Stale routing scan | `rg` for old shim/co-register helpers and old counts | No remaining `RegisterModelGenActions`, `RegisterLevelDesignActions`, shim/co-register prose, `mesh=62`, or `worldgen=67` references in current source/docs. |

---

## 3. Notes

Do not run `source repair_crg_cache --execute` and `source health` concurrently against the same `EngineSource.db`; SQLite can report `database is locked`. Run repair first, then health.
