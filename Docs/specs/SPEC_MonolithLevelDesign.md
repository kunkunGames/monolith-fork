# Monolith — MonolithLevelDesign Module

**Parent:** [../SPEC_CORE.md](../SPEC_CORE.md)
**Engine:** Unreal Engine 5.7+
**Owner module:** `MonolithLevelDesign`
**Namespace:** `leveldesign`
**MCP tool:** `leveldesign_query`
**Status:** Implemented (2026-05-20 namespace split)

---

## 1. Scope

``MonolithLevelDesign`` owns the ``leveldesign`` namespace — horror/encounter design, AI-tactical analysis (sightlines, choke points, stealth maps, ambush points), pacing/tension, accessibility reports, acoustic design analysis, composition/framing review, monster reveal evaluation, and co-op spatial balance. Split out of ``mesh`` on 2026-05-20.

## 2. Namespace ownership

Action implementations now live in `Source/MonolithLevelDesign` and export through `MONOLITHLEVELDESIGN_API`. `MonolithLevelDesign::StartupModule` registers the level-design-owned action families and `MonolithLevelDesign::ShutdownModule` unregisters only actions owned by the `MonolithLevelDesign` module, so shared namespaces are not cleared accidentally during hot reload or module shutdown. The module depends on exported `MonolithMesh` helpers for shared analysis utilities and on `MonolithScene` for lighting-capture helpers.

## 3. Registered actions

- `FMonolithMeshHorrorActions` — horror props, damage application.
- `FMonolithMeshHorrorDesignActions` — scare sequence design, monster reveal evaluation.
- `FMonolithMeshEncounterActions` — encounter design, pacing evaluation.
- `FMonolithMeshLevelDesignActions` — sightlines, choke points, stealth map, ambush points.
- `FMonolithMeshAdvancedLevelActions` — pacing curve, tension profiles.
- `FMonolithMeshAccessibilityActions` — accessibility/hospice reports.
- `FMonolithMeshAudioActions` — acoustic design analysis (room acoustics, sound propagation, footstep estimation).
- `FMonolithLevelDesignQualityActions` — composition/framing, monster reveal, and co-op spatial balance review actions that used to be registered under `mesh`.

## 4. Build.cs dependencies

Public: `Core`, `CoreUObject`, `Engine`
Private: `MonolithCore`, `MonolithMesh` (for exported shared mesh-family helpers), `MonolithScene` (lighting capture helper), `MonolithIndex`, `SQLiteCore`, `UnrealEd`, `EditorSubsystem`, `MeshDescription`, `StaticMeshDescription`, `MeshConversion`, `PhysicsCore`, `NavigationSystem`, `RenderCore`, `RHI`, `EditorScriptingUtilities`, `Json`, `JsonUtilities`, `Slate`, `SlateCore`, `AssetRegistry`, `AssetTools`, `MeshReductionInterface`, `MeshMergeUtilities`, `LevelInstanceEditor`, `ImageCore`.

## 5. Notes

Acoustic design analysis lives here (not in MonolithAudio) because the work is level/scene-design analytical, not asset authoring; sound asset authoring stays in ``audio_query``. The acoustic helper header/source moved with the level-design implementation so downstream modules link against `MonolithLevelDesign`, not `MonolithMesh`, for this domain logic.

## 6. Per-action reference

See the namespace section in [SPEC_MonolithMesh.md](SPEC_MonolithMesh.md) for the per-action params/descriptions (the split table at the top of that spec is authoritative for namespace attribution; row params/descriptions remain in-place pending the per-action re-tabulation backlog item in [../TODO.md](../TODO.md)).
