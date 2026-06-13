# material-reference — Material Systems

Master material architecture, instancing, parameter collections, function libraries, layers,
branching, and a debugging checklist. Quick rules live in `material-reference/SKILL.md`;
HLSL details in `hlsl-custom-node-guide.md`, cost details in `material-performance.md`.

## Master Material Architecture

One master `UMaterial` per shading family; ship variation through instances, not new masters.

- Expose every artist-tunable value as a named parameter (Scalar / Vector / Texture / StaticSwitch / StaticBool).
- Gate optional features behind static switches so unused branches compile out per instance.
- **Static switches:** each doubles shader permutations. Max ~6 per master material.
- **Sampler limit:** 16 per material — use `Shared:Wrap` sampler source to pool samplers across textures.
- Keep the graph topologically clean: `build_material_graph` (single JSON spec) is fastest; run `auto_layout` and `validate_material` after edits.
- Watch instruction budgets: Opaque < 150, Translucent < 200, Post-process < 100 (`get_compilation_stats`).

## MIC vs MID (vs MFI)

| Term | Asset / Object | Created at | Cost | Use for |
|------|----------------|-----------|------|---------|
| **MIC** — Material Instance Constant | `UMaterialInstanceConstant` (on-disk asset) | Edit/cook time | Free at runtime | Authored variants, per-asset tuning, the default choice |
| **MID** — Material Instance Dynamic | `UMaterialInstanceDynamic` (transient, in-memory) | Runtime, in code/BP | Per-instance, breaks batching | Values that change while playing (hit flash, fade, progress) |
| **MFI** — Material Function Instance | `UMaterialFunctionInstance` | Edit time | Free | Override inputs of a shared material function |

- Create MIC with `create_material_instance` (parent = master); set values via `set_instance_parameters` (batch = single recompile).
- Graph editing works only on base `UMaterial`, never on a MIC/MID — instances only override exposed parameters.
- **MID breaks draw-call batching.** Each MID is a unique material proxy.
- **Prefer Custom Primitive Data (CPD) over MID** when you only need a few per-instance scalars/vectors: CPD keeps instances batched. Reach for a MID only when you must swap textures or drive many params at runtime.
- Reparent with `set_instance_parent` (reports kept vs lost overrides); `clear_instance_parameter` removes one override (`"all"` clears every override).

## Material Parameter Collections (MPC)

`UMaterialParameterCollection` = a small global uniform buffer (Scalars + Vectors) read by any material via a `CollectionParameter` node. Set from Blueprint/C++ (`SetScalarParameterValue` / `SetVectorParameterValue`) to drive many materials at once — time of day, wetness, global tint, gameplay state.

- **Limit: 2 MPCs referenced per material.**
- Global write, no per-instance variation; changing a value updates every material that reads it.
- Keep collections small and grouped by update cadence; do not use an MPC as a per-object parameter channel (that is what instances / CPD are for).

## Material Function Libraries

`UMaterialFunction` = reusable graph fragment with typed inputs/outputs, shared across materials.

- Create with `create_material_function` (`type`: function / layer / layer_blend); `expose_to_library: true` lists it in the palette under `library_categories`.
- Build the body with `build_function_graph` (same spec shape as `build_material_graph`); typed I/O via `FunctionInput` / `FunctionOutput`.
- `update_material_function` recompiles and cascades to every referencing material — one edit propagates everywhere.
- Override per-use values without editing the function via an MFI (`create_function_instance`).
- Curate a library of named helpers (anti-tiling, packing, blends) so masters compose from shared, tested nodes instead of duplicated graph.

## Material Layers

Layered Materials stack `MaterialLayer` assets blended by `MaterialLayerBlend` assets, authored as a `MaterialAttributes` stack on the master — artists add/reorder layers per instance without touching the graph.

- Layers and blends are special material-function types; inspect the stack with `get_layer_info`.
- Each layer outputs a full `FMaterialAttributes` set; blends mix two layers (often by a mask).
- Powerful but heavier: every active layer adds instructions and samplers — budget accordingly and prefer a small fixed layer count.

## Static vs Dynamic Branch

| | Static (`StaticSwitch` / `StaticBool`) | Dynamic (`If` node / runtime compare) |
|---|---|---|
| Resolved | At compile time, per instance | Every pixel, every frame |
| Cost | Dead branch removed — zero runtime cost | Both sides may execute; full cost |
| Permutations | Doubles per switch | None |
| Use for | Feature on/off, quality tiers, platform variants | Values that truly vary at runtime |

Rule of thumb: if the choice is fixed for an instance, use a **static** switch (compiles the branch out). Use a dynamic `If` only when the condition changes at runtime, and keep both branches cheap. Note `clip()` is disabled on Nanite passes — gate masking accordingly.

## Debugging Checklist

1. **Compiles?** `validate_material` (broken connections, unused nodes) → `recompile_material` if stale.
2. **Within budget?** `get_compilation_stats` for VS/PS instruction counts and sampler count vs the budgets above.
3. **Right output plugged?** `get_full_connection_graph` — confirm BaseColor / Normal / Roughness / Metallic feed the intended pins; ORM packing usually R=AO, G=Roughness, B=Metallic.
4. **Blend mode matches usage?** Connection warnings are informational — an input stays inactive until the blend/shading mode matches (e.g. Opacity needs a translucent or masked mode). Never use `BLEND_Translucent` for render-target alpha; use `BLEND_AlphaComposite`.
5. **Parameters actually exposed?** `get_material_parameters` (master) vs `get_instance_parameters` (instance) — an override does nothing if the name does not match a master parameter.
6. **Normal map sampler type = Normal?** Wrong `SamplerType` (e.g. Color on a normal map) is a common wash-out / banding cause.
7. **Permutation blow-up?** Count static switches (≤ ~6) and samplers (≤ 16); collapse rarely-used switches into separate instances.
8. **Runtime tint/animation wrong?** Confirm CPD vs MID vs MPC: per-object = CPD/MID, global = MPC; a value driven on the wrong channel silently no-ops.
9. **Visual check:** `render_preview` / `editor_query("inspect_material_pbr")` for a no-render reflection walk; preview tiling at 3x before finalizing.
