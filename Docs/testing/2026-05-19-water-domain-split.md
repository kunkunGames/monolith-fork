# MonolithWater Domain Split Verification

**Date:** 2026-05-19
**Scope:** `water.get_status`, `water.list_bodies`
**Branch:** `codex/water-domain-split`
**Result:** PASS

---

## 1. Change Under Test

Water discovery moved from the broad `MonolithMesh` scene action surface into a dedicated `MonolithWater` module and `water` namespace.

| Before | After |
|--------|-------|
| `MonolithMesh` registered Water discovery as `mesh.get_water_status`. | `MonolithWater` registers `water.get_status`. |
| `MonolithMesh` registered Water actor listing as `mesh.list_water_bodies`. | `MonolithWater` registers `water.list_bodies`. |
| Water status lived inside mesh docs/action counts. | Water has its own module spec and proxy tool route. |

---

## 2. Verification

| Gate | Command / Evidence | Result |
|------|--------------------|--------|
| Whitespace | `git diff --check` | PASS |
| Plugin descriptor JSON | `uv run python -m json.tool Monolith.uplugin` | PASS |
| Static CI parity | `uv run python Scripts\ci_static_checks.py --config .github\monolith-static-ci.json --github check` | PASS: 0 blocking findings; existing `.claude/agents` external-prerequisite advisory only. |
| Source route cleanup | `rg -F "get_water_status" Source` and `rg -F "list_water_bodies" Source` | PASS: no source matches. |
| Registration grouping | Source registration scan | PASS: `MonolithWater, water` has 2 registrations. Follow-up routing cleanup on 2026-05-20 reduced the live `mesh` namespace to 62 registrations by moving scene, level-design, worldgen, modelgen, and asset-hygiene actions to their own namespaces. |
| UE 5.7 plugin build | `UnrealBuildTool.exe UnrealEditor Win64 Development -Plugin="<worktree>\Monolith.uplugin" -WaitMutex -NoHotReloadFromIDE -NoUBTMakefiles` using the engine root resolved from `D:\P4\game\GO.uproject`. | PASS: `Result: Succeeded`; `UnrealEditor-MonolithWater.dll` built. |

---

## 3. Residual Notes

- This slice remains read-only and reflection-only. It does not add Water, WaterEditor, Landscape, or LandscapeEditor module dependencies.
- Actor, spline, zone, buoyancy, landscape, and rebuild mutations remain future work for the `water` namespace.
- Remaining broad-module namespace owners found during the audit are tracked as follow-up candidates rather than bundled into this Water split.
