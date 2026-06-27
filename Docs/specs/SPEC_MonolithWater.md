# Monolith - MonolithWater Module

**Parent:** [SPEC_CORE.md](../SPEC_CORE.md)
**Engine:** Unreal Engine 5.7+
**Version:** 0.14.10
**Status:** Implemented

---

## 1. Purpose

`MonolithWater` owns the `water` namespace for optional Water/Landscape discovery. The module is intentionally read-only and dependency-light: it uses loaded editor-world actors, reflected class names, and module-status probes only. It does not include Water headers, does not mutate actors, splines, water zones, buoyancy components, landscapes, or rebuild state, and does not require the Water plugin at compile time.

---

## 2. Ownership

| Class | Responsibility |
|-------|----------------|
| `FMonolithWaterModule` | Registers and unregisters the `water` namespace. |
| `FMonolithWaterActions` | Implements read-only Water status and Water-like actor/component listing. |

| Dependency | Purpose |
|------------|---------|
| `MonolithCore` | Tool registry, action result, and parameter schema contracts. |
| `Engine`, `UnrealEd` | Current editor-world access and reflected actor/component inspection. |
| `Json`, `JsonUtilities` | Action response payloads. |

---

## 3. Action Surface

| Action | Params | Description |
|--------|--------|-------------|
| `water.get_status` | none | Reports Water, WaterEditor, Landscape, and LandscapeEditor module availability plus Water-like actor counts. |
| `water.list_bodies` | `limit`?, `actor_name_filter`? | Lists reflected Water-like actors/components in the current editor world. |

---

## 4. Safety Contract

| Gate | Requirement |
|------|-------------|
| Optional dependency | No Water, WaterEditor, Landscape, or LandscapeEditor include paths or module dependencies. |
| Read-only behavior | Actions must not mutate actors, components, splines, zones, landscapes, packages, transactions, or rebuild state. |
| Reflection boundary | Water-like identity is determined from loaded class paths/names only; no class loads or asset loads are required. |
| Output bounds | `limit` clamps to `1..500`; actor rows include bounded component summaries and no pixel/mesh payloads. |

---

## 5. Verification Gates

| Gate | Evidence |
|------|----------|
| Registration | `FMonolithWaterModule::StartupModule` registers `water.get_status` and `water.list_bodies`. |
| Route cleanup | `MonolithMesh` no longer registers `mesh.get_water_status` or `mesh.list_water_bodies`. |
| UE 5.7 build | Full plugin UBT build must succeed with the engine root resolved from the host `.uproject`. |
