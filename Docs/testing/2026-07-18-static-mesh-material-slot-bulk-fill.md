# StaticMesh Material Slot Bulk-Fill Verification

| Field | Value |
|---|---|
| Owner | MonolithMesh |
| Status | Reusable source hardening complete; isolated compile and static contract audit passed; final linked-binary acceptance is aggregate-owned |
| Date | 2026-07-19 |
| Changelist | 1206 |
| Target project | Speed |

---

## 1. Scope and Ownership

This record verifies the reusable `bulk_fill.apply(target_namespace="mesh")` fill kind `StaticMeshMaterialSlots`. CL 1206 owns only the Monolith dispatcher policy, mesh adapter, exact-name helper, regression tests, specification, and authoring reference. It owns no `.uasset` or production map change.

The 2026-07-18 five-mesh round trip in section 3 is retained as historical first-use evidence. It does not define the final production cohort or asset ownership. The canonical production application is the 12-asset cohort in section 4, owned by CL 1207.

---

## 2. Source, Build, and Automation Evidence

| Gate | Result |
|---|---|
| Historical protected build | `Build\BatchFiles\BuildGameEditorAndRun.bat`, `SKIP_EDITOR_LAUNCH=1`, numbered changelist context: passed on 2026-07-18 |
| Historical focused automation | `automation-20260718T153255Z-EF850A91`: `Contract` and `Guards`, `2/2` passed, `0` errors, `0` warnings |
| Current isolated compile | `P4_BUILD_CHANGELIST=1206`; `Build\BatchFiles\BuildGameEditorStrictNonUnity.bat -NoLink "-Module=MonolithCore+MonolithMesh"`: `119/119` compile actions passed, exit `0`, `0` compile errors or warnings |
| Current adapter ownership audit | `11/11` registered `*BulkFillAdapter.cpp` implementations own a transaction boundary |
| Current central policy audit | `6/6` dispatcher-policy fields match explicit dirty-package tracking with adapter-owned transaction and post-edit validation |
| Current material-slot invariant audit | `6/6` preflight, exact-case, transaction, edit-notification, dirtying, and idempotence invariants present |
| Current focused regression audit | `4/4` policy, case-only mismatch, dirty-bit idempotence, and strict guard assertion groups present |
| Final linked-binary acceptance | The submit coordinator runs the protected aggregate linked build and the two focused tests once against that final binary; this isolated CL 1206 pass intentionally does not duplicate the aggregate build or asynchronous automation run |

The historical run predates the exact-case, handler-owned-transaction, and dirty-bit-idempotence assertions. It proves the original live path only. The current StrictNonUnity compile proves the hardened source and both focused fixtures compile together across `MonolithCore` and `MonolithMesh`; the final aggregate run is the sole acceptance result for the rebuilt linked binary.

---

## 3. Historical First-Use Live Round Trip

Every historical target used this strict tree:

```json
{
  "fill_kind": "StaticMeshMaterialSlots",
  "slots": [
    {
      "slot_index": 0,
      "expected_slot_name": "Material_0",
      "material_path": "/SpeedCore/Meshes/Wall/MI_ProcGrid.MI_ProcGrid"
    }
  ]
}
```

| Historical target | Triangles before/after | Simple boxes before/after | Collision | Slot readback |
|---|---:|---:|---|---|
| `/SpeedCore/Meshes/Wall/SM_Wall_I_400` | 12 / 12 | 1 / 1 | `UseSimpleAndComplex` | `Material_0 -> MI_ProcGrid` |
| `/SpeedCore/Meshes/Wall/SM_Pillar_100` | 12 / 12 | 1 / 1 | `UseSimpleAndComplex` | `Material_0 -> MI_ProcGrid` |
| `/SpeedCore/Meshes/Wall/SM_Wall_L_400_200` | 20 / 20 | 2 / 2 | `UseSimpleAndComplex` | `Material_0 -> MI_ProcGrid` |
| `/SpeedCore/Meshes/Wall/SM_Wall_T_400_200` | 28 / 28 | 2 / 2 | `UseSimpleAndComplex` | `Material_0 -> MI_ProcGrid` |
| `/SpeedCore/Meshes/Wall/SM_Wall_Z_400_400` | 28 / 28 | 4 / 4 | `UseSimpleAndComplex` | `Material_0 -> MI_ProcGrid` |

For each historical target, strict dry-run reported one `None -> MI_ProcGrid` write, `would_apply=false`, and `errors=0`. The matching commit reported `would_apply=true`. `asset.save_asset(verify_reload=true)` then saved a non-empty package, reloaded it non-interactively, and resolved the same `/Script/Engine.StaticMesh` class.

`MI_ProcGrid` readback was `BLEND_Opaque`, `MSM_DefaultLit`, `MD_Surface`, non-translucent, non-masked, with parent `/SpeedCore/Meshes/Wall/M_ProcGrid.M_ProcGrid`. Historical Data Validation passed `5/5` with no warning or invalid asset. These results are retained as baseline evidence and are not attributed to CL 1206 or used as the final CL 1207 production validation.

---

## 4. Canonical Production Application

CL 1207 owns the final 12-asset production application: 11 wall meshes plus `SM_Pillar_100`.

| Canonical asset | Production owner |
|---|---|
| `/SpeedCore/Meshes/Wall/SM_Pillar_100` | CL 1207 |
| `/SpeedCore/Meshes/Wall/SM_Wall_I_100` | CL 1207 |
| `/SpeedCore/Meshes/Wall/SM_Wall_I_200` | CL 1207 |
| `/SpeedCore/Meshes/Wall/SM_Wall_I_300` | CL 1207 |
| `/SpeedCore/Meshes/Wall/SM_Wall_I_400` | CL 1207 |
| `/SpeedCore/Meshes/Wall/SM_Wall_L_200_200` | CL 1207 |
| `/SpeedCore/Meshes/Wall/SM_Wall_L_300_300` | CL 1207 |
| `/SpeedCore/Meshes/Wall/SM_Wall_L_400_200` | CL 1207 |
| `/SpeedCore/Meshes/Wall/SM_Wall_T_400_200` | CL 1207 |
| `/SpeedCore/Meshes/Wall/SM_Wall_T_400_400` | CL 1207 |
| `/SpeedCore/Meshes/Wall/SM_Wall_Z_200_400` | CL 1207 |
| `/SpeedCore/Meshes/Wall/SM_Wall_Z_400_400` | CL 1207 |

Geometry, collision, map consumption, Data Validation, runtime presentation, screenshot inspection, and Discord proof for this cohort belong to CL 1207's verification record. CL 1206 supplies the reusable authoring primitive and regression coverage only.

---

## 5. Validation and Source-Control State

| Gate | Result |
|---|---|
| Root-cause review | No additional source defect found: the adapter fully preflights before mutation, uses exact-case slot matching, owns one transaction, and keeps unchanged requests clean |
| CL 1206 contents | Exactly seven tool/source/test/spec/reference files; no `.uasset`, `.umap`, or production content |
| CL 1206 resolves and default CL | No pending resolves; no CL 1206 task file left in the default changelist |
| Historical persistence | Five first-use packages saved/reloaded successfully and left `0` non-transient dirty packages; retained only as historical evidence |
| Final production validation | Owned and recorded by CL 1207 for the canonical 12-asset cohort |
| Screenshot / Discord upload | N/A for CL 1206 because it changes reusable source, tests, and documentation only; visual production proof belongs to CL 1207 |

---

## 6. Result

The reusable material-slot authoring gap is fixed at its source. CL 1206 introduces no default material, runtime fallback, geometry replacement, raw asset rewrite, alternate project checkout, or silent legacy path. The remaining acceptance gate is intentionally centralized: after the final aggregate linked build, the submit coordinator runs `Monolith.Mesh.BulkFill.StaticMeshMaterialSlots.Contract` and `Monolith.Mesh.BulkFill.StaticMeshMaterialSlots.Guards` once against that binary.
