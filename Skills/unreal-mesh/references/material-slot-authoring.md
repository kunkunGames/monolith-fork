# StaticMesh material-slot authoring

Use `bulk_fill.apply` with `target_namespace="mesh"` and `tree.fill_kind="StaticMeshMaterialSlots"` when assigning existing `UMaterialInterface` assets to existing `UStaticMesh` slots.

Workflow:

1. Read the exact target schema with `describe.schema(target_namespace="mesh", target=<StaticMesh path>)`.
2. Read current slots with `mesh.get_mesh_materials`.
3. Check out the target asset into its numbered task changelist.
4. Run `bulk_fill.apply` with `dry_run=true, strict=true`.
5. Review every reported slot path, current value, proposed value, and error.
6. Re-run the identical tree with `dry_run=false, strict=true`.
7. Persist with `asset.save_asset(asset_path=<StaticMesh path>, verify_reload=true)`.
8. Read back with `mesh.get_mesh_materials` and validate the assigned material itself.

Tree shape:

```json
{
  "fill_kind": "StaticMeshMaterialSlots",
  "slots": [
    {
      "slot_index": 0,
      "expected_slot_name": "Material_0",
      "material_path": "/Game/Materials/MI_Wall.MI_Wall"
    }
  ]
}
```

`expected_slot_name` is a case-sensitive optimistic-concurrency guard: `Material_0` and `material_0` are different contracts even though Unreal's ordinary `FName` equality treats them as equal. The adapter rejects missing assets, non-material assets, fractional/out-of-range indices, slot-name drift, duplicate slot indices, empty batches, and unknown fields before any mutation.

The central `bulk_fill.apply` registry policy tracks dirty packages but does not add an undo wrapper. The selected namespace adapter owns the single transaction around its fully preflighted mutation. This avoids UE's nested-scope behavior where `Cancel(Index > 0)` warns that partial cancellation is unsupported and cancels the entire outer transaction, keeping cancellation and the undo title unambiguous. Do not substitute `replace_static_mesh_geometry_in_place`, `geometry_material_ids`, raw reflection, editor Python, or a default material fallback.
