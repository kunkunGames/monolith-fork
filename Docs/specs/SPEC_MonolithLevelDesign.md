# Monolith — MonolithLevelDesign Module

**Parent:** [../SPEC_CORE.md](../SPEC_CORE.md)
**Engine:** Unreal Engine 5.7+
**Owner module:** `MonolithLevelDesign`
**Namespace:** `leveldesign`
**MCP tool:** `leveldesign_query`
**Status:** Implemented (2026-05-20 namespace split)

---

## 1. Scope

``MonolithLevelDesign`` owns the ``leveldesign`` namespace — horror/encounter design, AI-tactical analysis (sightlines, choke points, stealth maps, ambush points), pacing/tension, accessibility reports, and acoustic design analysis. Split out of ``mesh`` on 2026-05-20.

## 2. Namespace ownership

Thin registration shim — action implementations live in MonolithMesh DLL via ``MONOLITHMESH_API`` and ``MonolithLevelDesign::StartupModule`` calls their ``RegisterActions`` cross-DLL. ``MonolithLevelDesign::ShutdownModule`` unregisters the ``leveldesign`` namespace.

## 3. Registered actions

- `FMonolithMeshHorrorActions` — horror props, damage application.
- `FMonolithMeshHorrorDesignActions` — scare sequence design, monster reveal evaluation.
- `FMonolithMeshEncounterActions` — encounter design, pacing evaluation.
- `FMonolithMeshLevelDesignActions` — sightlines, choke points, stealth map, ambush points.
- `FMonolithMeshAdvancedLevelActions` — pacing curve, tension profiles.
- `FMonolithMeshAccessibilityActions` — accessibility/hospice reports.
- `FMonolithMeshAudioActions` — acoustic design analysis (room acoustics, sound propagation, footstep estimation).

## 4. Build.cs dependencies

Public: `Core`, `CoreUObject`, `Engine`
Private: `MonolithCore`, `MonolithMesh` (for shared mesh-family helpers and `MONOLITHMESH_API`-decorated action classes), `MonolithIndex`, `SQLiteCore`, `UnrealEd`, `EditorSubsystem`, `MeshDescription`, `StaticMeshDescription`, `MeshConversion`, `PhysicsCore`, `NavigationSystem`, `RenderCore`, `RHI`, `EditorScriptingUtilities`, `Json`, `JsonUtilities`, `Slate`, `SlateCore`, `AssetRegistry`, `AssetTools`, `MeshReductionInterface`, `MeshMergeUtilities`, `LevelInstanceEditor`, `ImageCore`.

## 5. Notes

Acoustic design analysis lives here (not in MonolithAudio) because the work is level/scene-design analytical, not asset authoring; sound asset authoring stays in ``audio_query``.

## 6. Per-action reference

See the namespace section in [SPEC_MonolithMesh.md](SPEC_MonolithMesh.md) for the per-action params/descriptions (the split table at the top of that spec is authoritative for namespace attribution; row params/descriptions remain in-place pending the per-action re-tabulation backlog item in [../TODO.md](../TODO.md)).