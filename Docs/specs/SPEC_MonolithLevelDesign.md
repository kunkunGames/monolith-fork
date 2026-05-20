# Monolith — MonolithLevelDesign Module

**Parent:** [../SPEC_CORE.md](../SPEC_CORE.md)
**Engine:** Unreal Engine 5.7+
**Owner module:** `MonolithLevelDesign`
**Primary namespace:** `leveldesign`
**MCP tool:** `leveldesign_query`
**Shared registrations:** `scene`, `mesh`, `level_instance`
**Status:** Implemented (2026-05-20 namespace split)

---

## 1. Scope

``MonolithLevelDesign`` owns the focused ``leveldesign`` namespace — horror/encounter design, AI-tactical analysis (sightlines, choke points, stealth maps, ambush points), pacing/tension, accessibility reports, acoustic design analysis, composition/framing review, monster reveal evaluation, and co-op spatial balance. Split out of ``mesh`` on 2026-05-20; broad editor-world placement, mesh optimization, and prefab actions now register under their runtime domains instead of being forced into ``leveldesign``.

## 2. Namespace ownership

Action implementations now live in `Source/MonolithLevelDesign` and export through `MONOLITHLEVELDESIGN_API`. `MonolithLevelDesign::StartupModule` registers level-design-owned action families through owner-scoped registration and `MonolithLevelDesign::ShutdownModule` unregisters only actions owned by the `MonolithLevelDesign` module, so shared namespaces are not cleared accidentally during hot reload or module shutdown. The module depends on exported `MonolithMesh` helpers for shared analysis utilities and on `MonolithScene` for lighting-capture helpers.

## 3. Registered actions

- `FMonolithLevelDesignHorrorActions` — sightlines, hiding spots, ambush points, choke points, escape routes, tension, pacing, and dead-end analysis.
- `FMonolithLevelDesignHorrorDesignActions` — path prediction, spawn evaluation, scare positioning, and encounter pacing.
- `FMonolithLevelDesignEncounterActions` — encounter design, patrol routing, AI territory, safe-room evaluation, pacing structure, scare sequences, and hospice reports.
- `FMonolithLevelDesignAccessibilityActions` — accessibility/hospice pathing, contrast, reach, rest-point, and report actions.
- `FMonolithLevelDesignAudioActions` — acoustic design analysis (room acoustics, sound propagation, footstep estimation, stealth maps, audio-volume suggestions).
- `FMonolithLevelDesignQualityActions` — composition/framing, monster reveal, and co-op spatial balance review actions that used to be registered under `mesh`.
- `FMonolithLevelDesignEditingActions` — shared `scene` actions for light/material/component editing and shared `mesh` actions for mesh replacement, LOD screen sizes, instancing candidates, and HISM conversion.
- `FMonolithLevelDesignPlacementActions` — shared `scene` actions for sublevels, Blueprint actor placement, splines, transform randomization, actor enumeration, and distance measurement, plus shared `level_instance` prefab helpers.

## 4. Build.cs dependencies

Public: `Core`, `CoreUObject`, `Engine`
Private: `MonolithCore`, `MonolithMesh` (for exported shared mesh-family helpers), `MonolithScene` (lighting capture helper), `MonolithIndex`, `SQLiteCore`, `UnrealEd`, `EditorSubsystem`, `MeshDescription`, `StaticMeshDescription`, `MeshConversion`, `PhysicsCore`, `NavigationSystem`, `RenderCore`, `RHI`, `EditorScriptingUtilities`, `Json`, `JsonUtilities`, `Slate`, `SlateCore`, `AssetRegistry`, `AssetTools`, `MeshReductionInterface`, `MeshMergeUtilities`, `LevelInstanceEditor`, `ImageCore`.

## 5. Notes

Acoustic design analysis lives here (not in MonolithAudio) because the work is level/scene-design analytical, not asset authoring; sound asset authoring stays in ``audio_query``. The acoustic helper header/source moved with the level-design implementation so downstream modules link against `MonolithLevelDesign`, not `MonolithMesh`, for this domain logic.

## 6. Per-action reference

Current static registration audit reports 43 `leveldesign` actions. The same module also contributes 11 `scene`, 4 `mesh`, and 3 `level_instance` actions via owner-scoped shared namespace registration. For exact params and live ownership, call `monolith_discover({ "namespace": "<namespace>", "mode": "actions" })`.
