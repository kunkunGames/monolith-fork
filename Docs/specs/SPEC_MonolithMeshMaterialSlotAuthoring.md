# MonolithMesh StaticMesh Material Slot Authoring Specification

**Status:** Reusable hardening complete; isolated source gate passed; final linked-binary acceptance is aggregate-owned
**Owner:** `MonolithMesh`
**Date:** 2026-07-19
**Changelist:** 1206

---

## 1. Purpose

`bulk_fill.apply` with `target_namespace="mesh"` owns transactional material assignment for an existing `UStaticMesh`. The `StaticMeshMaterialSlots` fill kind closes the previous contract gap where `describe.schema` exposed reflected `StaticMaterials` fields but the mesh adapter could not write them.

The action does not create substitute materials, accept null material fallbacks, remap geometry material IDs, or alter slot names. It assigns an already-existing `UMaterialInterface` to an exact slot while preserving the mesh, collision, geometry, LOD, section, and slot identity.

---

## 2. API Contract

| Field | Type | Required | Contract |
|---|---|---:|---|
| `target_namespace` | string | Yes | Must be `mesh` |
| `target` | asset path | Yes | Must resolve to an existing `UStaticMesh` |
| `tree.fill_kind` | string | Yes | Must be `StaticMeshMaterialSlots` |
| `tree.slots` | array | Yes | Must contain at least one unique assignment |
| `slot_index` | int32 | Yes | Must be within the target's current `StaticMaterials` range |
| `expected_slot_name` | FName display string | Yes | Must equal the current `MaterialSlotName` at `slot_index` with case-sensitive spelling; `Material_0` does not accept `material_0` |
| `material_path` | asset path | Yes | Must resolve to an existing `UMaterialInterface` |
| `dry_run` | bool | No | `true` validates and reports without mutation |
| `strict` | bool | No | Callers should use `true`; unknown fields are rejected by this fill kind in every mode |

Example:

```json
{
  "target_namespace": "mesh",
  "target": "/SpeedCore/Meshes/Wall/SM_Wall_I_400",
  "tree": {
    "fill_kind": "StaticMeshMaterialSlots",
    "slots": [
      {
        "slot_index": 0,
        "expected_slot_name": "Material_0",
        "material_path": "/SpeedCore/Meshes/Wall/MI_ProcGrid.MI_ProcGrid"
      }
    ]
  },
  "dry_run": true,
  "strict": true
}
```

---

## 3. Transaction and Failure Contract

| Gate | Required behavior |
|---|---|
| Full preflight | Resolve every assignment, validate every slot index/name/material, and reject duplicates or unknown fields before mutation |
| Registry execution policy | `bulk_fill.apply` uses explicit `track_dirty_packages`; the central registry does not create a nested scope whose partial cancel would warn and cancel the entire outer transaction |
| Transaction | The selected adapter owns one `FScopedTransaction` and one target `Modify()` envelope per asset |
| Editor notifications | Call `PreEditChange` before the first assignment and `PostEditChange` after the last assignment |
| Persistence boundary | Mark the package dirty; callers persist with `asset.save_asset(verify_reload=true)` |
| Idempotence | Reapplying the same material returns no persisted write and leaves an explicitly clean package clean |
| Error policy | Any invalid entry prevents all assignments; no partial write, default material, or silent legacy branch |

---

## 4. Verification Gates

| Gate | Evidence |
|---|---|
| Schema | `describe.schema(target_namespace="mesh", target=<StaticMesh>)` returns the typed `StaticMeshMaterialSlots` tree |
| Unit contract | `Monolith.Mesh.BulkFill.StaticMeshMaterialSlots.Contract` proves the handler-owned transaction policy, dry-run cleanliness, commit dirtying, exact readback, and dirty-bit idempotence on transient fixtures |
| Guard contract | `Monolith.Mesh.BulkFill.StaticMeshMaterialSlots.Guards` proves case-sensitive slot-name, duplicate-index, and unknown-field rejection |
| Isolated source gate | With `P4_BUILD_CHANGELIST=1206`, run `Build\BatchFiles\BuildGameEditorStrictNonUnity.bat -NoLink "-Module=MonolithCore+MonolithMesh"` |
| Final integration gate | Run the protected aggregate linked build through `Build\BatchFiles\BuildGameEditorAndRun.bat`, then run both focused tests once against that final binary |
| Live round trip | Strict dry-run, commit, `asset.save_asset(verify_reload=true)`, then `mesh.get_mesh_materials` readback |

---

## 5. Ownership Boundaries

`mesh.replace_static_mesh_geometry_in_place` is not a material assignment action: it intentionally preserves the target `StaticMaterials` array exactly. `mesh.geometry_material_ids` edits per-triangle IDs on an in-memory mesh handle and does not assign a `UMaterialInterface` to an existing asset slot. `asset.save_asset` persists an already-dirty package but does not author slot values.

CL 1206 owns the reusable dispatcher policy, mesh adapter, exact-name helper, focused regression tests, specification, and authoring reference. It owns no production `.uasset` or `.umap`. CL 1207 owns the canonical production application to 12 assets: 11 `SM_Wall_*` meshes and `/SpeedCore/Meshes/Wall/SM_Pillar_100`. Production geometry/collision invariants, map consumption, Data Validation, screenshots, and Discord proof are therefore CL 1207 gates, not CL 1206 gates.

---

## 6. Verification State

### 6.1 Current CL 1206 Source Gate

| Gate | Result |
|---|---|
| StrictNonUnity no-link compile | `MonolithCore+MonolithMesh`, `119/119` actions passed, exit `0`, `0` compile errors or warnings |
| Adapter transaction ownership | `11/11` registered bulk-fill adapters own a transaction boundary |
| Central policy fields | `6/6` required policy fields present and correctly configured |
| Material-slot adapter invariants | `6/6` preflight, exact-case, transaction, notification, dirtying, and idempotence invariants present |
| Focused regression assertions | `4/4` assertion groups present for policy, case-only mismatch, dirty-bit idempotence, and strict guards |
| Source review decision | No additional implementation change required; the hardened adapter and dispatcher contract are coherent |

### 6.2 Historical Baseline Evidence

The 2026-07-18 run predates the exact-case, transaction-ownership, and dirty-bit-idempotence assertions. It remains evidence for the original five-mesh live round trip, but it is not presented as proof of the hardened assertions.

| Gate | Historical result |
|---|---|
| Protected editor build | Passed through `Build\BatchFiles\BuildGameEditorAndRun.bat` with `SKIP_EDITOR_LAUNCH=1` and numbered changelist context |
| Focused automation | `automation-20260718T153255Z-EF850A91`: `2/2` passed, `0` errors, `0` warnings |
| Live material round trip | Five first-use meshes completed strict dry-run, commit, save/reload, exact slot readback, and unchanged geometry/collision checks |
| Data Validation | Historical first-use set passed `5/5` with `0` warnings and `0` invalid assets |

### 6.3 Aggregate Acceptance and Production Cohort

The submit coordinator owns one final protected aggregate linked build followed by `Monolith.Mesh.BulkFill.StaticMeshMaterialSlots.Contract` and `Monolith.Mesh.BulkFill.StaticMeshMaterialSlots.Guards` against that rebuilt binary. The isolated CL 1206 pass intentionally does not duplicate that linked build or asynchronous automation run.

The final production cohort is owned by CL 1207:

| Cohort | Assets |
|---|---|
| Pillar | `SM_Pillar_100` |
| Straight walls | `SM_Wall_I_100`, `SM_Wall_I_200`, `SM_Wall_I_300`, `SM_Wall_I_400` |
| Corner walls | `SM_Wall_L_200_200`, `SM_Wall_L_300_300`, `SM_Wall_L_400_200` |
| Junction walls | `SM_Wall_T_400_200`, `SM_Wall_T_400_400` |
| Offset walls | `SM_Wall_Z_200_400`, `SM_Wall_Z_400_400` |

Screenshot verification and Discord upload are N/A for CL 1206 because it changes reusable source, tests, and documentation only. CL 1207 owns the production visual proof.

The detailed evidence is recorded in `Plugins\Monolith\Docs\testing\2026-07-18-static-mesh-material-slot-bulk-fill.md`.
