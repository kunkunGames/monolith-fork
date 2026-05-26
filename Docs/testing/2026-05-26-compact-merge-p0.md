# Verification - Compact Merge Contract Pass

**Date:** 2026-05-26
**Scope:** `SPEC_Monolith0150CompactMerge.md` implementation pass: bulk-fill registry lock scope, reflection writer contracts, `monolith_discover` action schema mode, ProjectIndex/search/write boundary, Material/UI non-mutating surfaces, `describe.list_targets` optional inventory contract, guide parity, DataTable guards, and UI spec/path-cache regressions.
**Engine:** UE 5.7 resolved from `GO.uproject`, Win64 Development editor.
**Spec:** [../specs/SPEC_Monolith0150CompactMerge.md](../specs/SPEC_Monolith0150CompactMerge.md)

---

## 1. Build

| Gate | Command | Result |
|------|---------|--------|
| Full editor target | Resolve the engine through `BatchFiles\Script\ResolveUnrealEngine.ps1`, then run `UnrealBuildTool.exe GoGameEditor Win64 Development -Project=D:\P4\game\GO.uproject -WaitMutex -NoHotReloadFromIDE`. | PASS, exit 0. |

The final code build completed after the `monolith_discover` schema-mode change, `describe.schema` target relaxation, DataTable bool guard, and UI widget-variable GUID reconciliation fix.

## 2. Automation Results

Headless runner:

```powershell
UnrealEditor-Cmd.exe D:\P4\game\GO.uproject -NullRHI -Unattended -NoSplash -NoSound -ExecCmds="Automation RunTests <filter>; Quit" -TestExit="Automation Test Queue Empty" -ReportExportPath=<report>
```

| Filter or area | Report | Result |
|----------------|--------|--------|
| `Leviathan.Monolith.Reflection` | `Saved\Automation\MonolithCompactMerge_Reflection_20260526_222730\index.json` | PASS: 5 succeeded, 0 failed. |
| Core bulk-fill/describe contracts | `Saved\Automation\MonolithCompactMerge_CoreContracts_20260526_223350\index.json` | PASS: 4 succeeded, 0 failed. Covers list-target inventory states and namespace-level `describe.schema` without `target`. |
| Guide parity | `Saved\Automation\MonolithCompactMerge_Guide_20260526_222730\index.json` | PASS: 3 succeeded, 0 failed. |
| Material bulk-fill | `Saved\Automation\MonolithCompactMerge_MaterialBulkFill_20260526_222730\index.json` | PASS: 1 succeeded, 0 failed. |
| UI menu spec contracts | `Saved\Automation\MonolithCompactMerge_UIBuildMenu_20260526_222730\index.json` | PASS: 2 succeeded, 0 failed. |
| Action execution/discover policy | `Saved\Automation\MonolithCompactMerge_ActionExecutionPolicy_20260526_222705\index.json` | PASS: 6 succeeded, 3 succeeded with warnings, 0 failed. Warnings are expected policy-override diagnostics. |
| Blueprint DataTable guards | `Saved\Automation\MonolithCompactMerge_DataTable_20260526_223825\index.json` | PASS: 5 succeeded with warnings, 0 failed. Warnings are repeated test action re-registration messages. |
| UI property path cache | `Saved\Automation\MonolithCompactMerge_UIPathCache_20260526_224610\index.json` | PASS: 1 succeeded, 0 failed. |
| UI spec builder | `Saved\Automation\MonolithCompactMerge_UISpecBuilder_20260526_224635\index.json` | PASS: 10 succeeded, 1 succeeded with warnings, 0 failed. The remaining warning-only case is `RollbackOnMidWalkFailure`. |

## 3. Live MCP And Offline Smokes

| Surface | Result |
|---------|--------|
| `monolith_status()` | PASS: `0.15.0`, 1584 actions, 45 namespaces. |
| MCP `tools/list` | PASS: 77 tools exposed to clients, including 44 `_query` namespace dispatch tools. `monolith_discover` exposes `namespace`, `action`, `category`, and `mode`; `mode` accepts `summary`, `actions`, and `schema`. |
| `monolith_discover` action schema mode | PASS: `monolith_discover(namespace="material", action="build_material_graph", mode="schema")` returns the exact action schema without unknown-param warnings. |
| `describe.schema` namespace-level shape | PASS: `describe_query schema` with only `target_namespace="material"` succeeds and returns a namespace-level descriptor. |
| `bulk_fill.apply` Material graph audit | PASS: `BuildMaterialGraph` dry-run/audit returns `would_apply=false`, an error-shaped audit report, and silent-drop details instead of committed-success status. |
| `bulk_fill.list_namespaces` | PASS: 11 in-tree adapters reported: `blueprint`, `material`, `animation`, `niagara`, `ui`, `mesh`, `gas`, `combograph`, `ai`, `logicdriver`, and `audio`. |
| `describe.list_targets material` | PASS: returns optional inventory metadata with `inventory_supported=false` and `contract=optional_inventory_not_implemented`. |
| `monolith_find` | PASS: material graph authoring routes to `material.build_material_graph`, preserving guide/find role separation. |
| `monolith_guide section=decisions` | PASS: returns editorial decision guidance with the live 1584-action registry overlay. |
| Offline guide | PASS: `Plugins\Monolith\Binaries\monolith_query.exe monolith guide` returns the same section keys: `onboarding`, `recipes`, `decisions`, `errors`, `skills_map`, `gotchas`. |
| Project health | PASS after `project repair_crg_cache --execute`: `Plugins\Monolith\Binaries\monolith_query.exe project health` returned `status=ok`. |
| Source health | PASS with known warning: `source health` remained readable; existing orphan-reference warnings are unrelated to this compact-merge pass. |

## 4. Covered Contracts

| Area | Verified behavior |
|------|-------------------|
| Reflection walker | Writes scalar/container/nested fields, rejects object-to-scalar coercion, reports unknown keys, reports enum typos, and dry-run inspection has no object side effects. |
| Registry callback dispatch | Bulk-fill callbacks are copied under lock and invoked after the lock is released. |
| `monolith_discover` | Keeps routing/schema ownership in `monolith_discover`; exact action schemas use `action` + `mode=schema`. |
| `describe` | Owns writable-shape introspection. `target` is optional for namespace-level descriptors; target listing is optional inventory, not a required adapter capability. |
| ProjectIndex boundary | Project search remains discovery/review only; write validation stays in `describe` or explicit action schemas. |
| Material bulk-fill | `BuildMaterialGraph` remains audit-only and cannot be mistaken for a committed graph edit. |
| UI menu/spec paths | Kind-only `build_menu_from_spec` screen entries return non-mutating status; UI spec builder no longer retains removed widget-variable GUIDs. |
| Blueprint DataTable | Malformed boolean params are rejected before asset load instead of being coerced from strings. |

## 5. Notes

- `project repair_crg_cache --execute` first failed while the editor owned `ProjectIndex.db`; after stopping the editor, the repair succeeded and `project health` returned `ok`.
- No PIE screenshot verification was required because this pass changed editor/tooling contracts and headless automation only, not visual/gameplay presentation.
