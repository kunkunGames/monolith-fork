---
name: unreal-materials
description: Use when creating, editing, or inspecting Unreal Engine material assets via Monolith MCP (material plus asset namespaces) - PBR setup, material-graph building, material functions and parameters, material instances (MIC) and dynamic instances (MID), templates, HLSL custom nodes, domains, translucent/decal/substrate setup, validation, and previews. Here texture means material-graph wiring, not texture ingest or generation. For a PBR rule, HLSL gotcha, or shader budget lookup use material-reference; for generic ingest/save/delete of the texture asset use unreal-asset; if the texture must be AI-generated use unreal-imagegen; for Niagara/particle materials use unreal-niagara. Triggers on material, shader, PBR, texture, material instance, MIC, MID, dynamic material instance, material function, material parameter, material domain, material graph, material node, connect material pins, translucent material, decal material, substrate.
---

# Unreal Material Workflows

Drives the **material** namespace (material assets, graphs, instances, functions) plus the generic **asset** namespace for texture ingest used by PBR builds. **64 material actions** via `material_query()`; the `asset` namespace adds workflows via `asset_query()` for texture/font ingest, file texture import, asset save/delete, typed asset inspection, naming validation, and batch rename hygiene.

Discover first (action names below are a snapshot; the live catalog is authoritative):

```
monolith_discover({ namespace: "material" })   // list material actions
monolith_discover({ namespace: "asset" })      // list asset actions
describe_query("action_schema", { namespace: "material", action: "build_material_graph" })
```

## When to use / Use a different skill for

- **This skill:** wiring and editing material/material-instance assets and their graphs (nodes, pins, functions, parameters, HLSL, instances, previews).
- **material-reference** — you only need to look up a PBR value/rule, an HLSL gotcha, or a shader instruction/sampler budget (knowledge, not actions).
- **unreal-asset** — generic ingest/save/delete/move/rename of the texture or material asset, not graph work.
- **unreal-imagegen** — the texture feeding the material must be AI-generated.
- **unreal-niagara** — the material is a Niagara/particle material handled in the VFX system.

## Key Parameters

- `asset_path` -- material asset path (NOT `asset`)

## Action Reference

Param notation: `name*` required, `name?` optional, `name=val` default, `a/b/c` allowed, `[w]` mutates. Signatures are a snapshot of the live catalog — for the exact full schema call `monolith_discover` with mode `schema` (or `describe_query("action_schema", { namespace, action })`). The discover-first block above remains the authority.

Material actions live in the **material** namespace; the asset-ingest/lifecycle actions in the final table live in the **asset** namespace.

### Read (21)

| Action | Signature | Purpose |
|--------|-----------|---------|
| `get_all_expressions` | `asset_path*` | List all expression nodes |
| `get_expression_details` | `asset_path*` `expression_name*` | Node properties and pins |
| `get_expression_connections` | `asset_path*` `expression_name*` | Input/output connections |
| `get_full_connection_graph` | `asset_path*` | Complete topology |
| `get_material_parameters` | `asset_path*` | All scalar/vector/texture params |
| `get_material_properties` | `asset_path*` | Settings: blend_mode, shading_model, domain, two_sided, usage. Works on UMaterial + UMaterialInstance |
| `get_compilation_stats` | `asset_path*` | VS/PS instruction counts, samplers, compile status |
| `get_layer_info` | `asset_path*` | Layer/blend stack (Material Layer or Layer Blend asset) |
| `list_expression_classes` | `filter?` `category?` | Available classes (cached). `filter`=class-name substring, `category`=MenuCategories substring |
| `get_expression_pin_info` | `class_name*` | Pin names/types without instance (e.g. `Multiply` or `MaterialExpressionMultiply`) |
| `list_material_instances` | `parent_path*` `recursive=true` | All instances (recursive walks instance-of-instance chains) |
| `get_function_info` | `asset_path*` | Function inputs, outputs, expressions |
| `[w] export_material_graph` | `asset_path*` `include_properties=true` `include_positions=true` | Serialize to JSON. `include_properties:false` reduces ~70% |
| `[w] export_function_graph` | `asset_path*` `include_properties=true` `include_positions=true` | Serialize MaterialFunction to JSON |
| `get_function_instance_info` | `asset_path*` | MFI parent chain, 11 override types |
| `get_thumbnail` | `asset_path*` `resolution=256` `save_to_file?` | Use `save_to_file:<path>` (base64 wastes context) |
| `validate_material` | `asset_path*` `fix_issues=false` | Broken connections, unused nodes |
| `[w] render_preview` | `asset_path*` `resolution=256` `uv_tiling=1.0` `preview_mesh=sphere` | Compile + preview. `preview_mesh`: plane/sphere/cube; tiling check at `uv_tiling` 3 or 5 |
| `get_texture_properties` | `asset_path*` | sRGB, dims, compression, recommended_sampler_type |
| `check_tiling_quality` | `asset_path*` | Detect tiling issues, missing anti-tiling/macro variation |
| `preview_texture` | `asset_path*` `resolution=256` `output_path?` | Texture preview + full metadata (default out: Saved/Monolith/previews/) |

### Instance (5)

| Action | Signature | Purpose |
|--------|-----------|---------|
| `get_instance_parameters` | `asset_path*` | All overrides: scalar, vector, texture, static switch |
| `[w] set_instance_parameters` | `asset_path*` `parameters*` | Batch-set, single recompile. `parameters`=array of `{name, type (scalar/vector/texture/switch), value}` |
| `[w] set_instance_parent` | `asset_path*` `new_parent*` | Reparent (reports lost/kept params) |
| `[w] clear_instance_parameter` | `asset_path*` `parameter_name?` `parameter_type=all` | Remove override. `parameter_type`: scalar/vector/texture/switch/all; omit `parameter_name` for all |
| `[w] save_material` | `asset_path*` `only_if_dirty=true` | Save to disk |

### Function (9)

| Action | Signature | Purpose |
|--------|-----------|---------|
| `[w] create_material_function` | `asset_path*` `description?` `expose_to_library=true` `library_categories?` `type=MaterialFunction` | Create function. `type`: MaterialFunction/MaterialLayer/MaterialLayerBlend |
| `[w] build_function_graph` | `asset_path*` `graph_spec*` `clear_existing=false` | Build with typed I/O. `graph_spec`: `{ inputs:[{name,type,sort_priority?,preview_value?}], outputs:[{name,sort_priority?}], nodes:[...], custom_hlsl_nodes?, connections? }` |
| `[w] set_function_metadata` | `asset_path*` `description?` `expose_to_library?` `library_categories?` | Update metadata |
| `[w] delete_function_expression` | `asset_path*` `expression_name*` | Remove expression(s); `expression_name` accepts comma-separated names |
| `[w] update_material_function` | `asset_path*` | Recompile + cascade to referencing materials (MaterialFunction or MFI) |
| `[w] create_function_instance` | `asset_path*` `parent*` `scalar_overrides?` `vector_overrides?` `texture_overrides?` `static_switch_overrides?` | Create MFI. Overrides: scalar `{name:value}`, vector `{name:{r,g,b,a}}`, texture `{name:path}`, switch `{name:bool}` |
| `[w] set_function_instance_parameter` | `asset_path*` `parameter_name*` `scalar_value?` `vector_value?` `texture_value?` `switch_value?` | Set MFI override. `vector_value`=`{r,g,b,a}` |
| `[w] layout_function_expressions` | `asset_path*` | Auto-arrange function graph |
| `[w] rename_function_parameter_group` | `asset_path*` `old_group*` `new_group*` | Rename param group |

### Write (23)

| Action | Signature | Purpose |
|--------|-----------|---------|
| `[w] auto_layout` | `asset_path*` `formatter=auto` | Topological-sort layout (UMaterial + UMaterialFunction). `formatter`: auto/monolith (blueprint_assist disabled) |
| `[w] create_material` | `asset_path*` `blend_mode=Opaque` `shading_model=DefaultLit` `material_domain=Surface` `two_sided=false` | Create empty material. `blend_mode`: Opaque/Masked/Translucent/Additive/Modulate/AlphaComposite; `shading_model`: DefaultLit/Unlit/Subsurface/SubsurfaceProfile/ClearCoat/TwoSidedFoliage; `material_domain`: Surface/DeferredDecal/PostProcess/LightFunction/UI |
| `[w] build_material_graph` | `asset_path*` `graph_spec*` `clear_existing=true` | Build from JSON spec (fastest) |
| `[w] create_custom_hlsl_node` | `asset_path*` `code*` `description?` `output_type?` `pos_x?` `pos_y?` `inputs?` `additional_outputs?` | Add Custom HLSL. `output_type`: float/float2/float3/float4 |
| `[w] update_custom_hlsl_node` | `asset_path*` `expression_name*` `code?` `description?` `output_type?` `inputs?` `additional_outputs?` `include_file_paths?` `additional_defines?` | Edit HLSL without rebuild. `output_type`: Float1/Float2/Float3/Float4; `inputs`/`additional_outputs` replace all |
| `[w] replace_expression` | `asset_path*` `expression_name*` `new_class*` `new_properties?` `preserve_connections=true` | Swap node in-place. `new_class` e.g. VectorParameter/Multiply |
| `[w] rename_expression` | `asset_path*` `expression_name*` `new_desc*` | Set label (Desc / visible graph label) |
| `[w] duplicate_expression` | `asset_path*` `expression_name*` `offset_x=50` `offset_y=50` | Duplicate (no connections) |
| `[w] move_expression` | `asset_path*` `expression_name?` `pos_x?` `pos_y?` `relative=false` `expressions?` | Reposition. Single via `expression_name`, batch via `expressions`=array of `{name,x,y}`; `relative` treats pos as offsets |
| `[w] set_material_property` | `asset_path*` `blend_mode?` `shading_model?` `material_domain?` `two_sided?` `opacity_mask_clip_value?` `dithered_lod_transition?` `fully_rough?` `cast_shadow_as_masked?` `output_velocity?` `used_with_*?` | Short or prefixed enums. `used_with_*`: skeletal_mesh/particle_sprites/niagara_sprites/niagara_meshes/niagara_ribbons/morph_targets/instanced_static_meshes/static_lighting |
| `[w] set_expression_property` | `asset_path*` `expression_name*` `property_name*` `value*` | Property on expression. `property_name` e.g. R/ParameterName/SamplerType/Texture; `value` string (ImportText) or number |
| `[w] connect_expressions` | `asset_path*` `from_expression*` `from_output?` `to_expression?` `to_input?` `to_property?` | Wire nodes or to output. `from_output` alias `from_pin`; `to_input` alias `to_pin`; `to_property`=material property (BaseColor, Roughness, ...) |
| `[w] disconnect_expression` | `asset_path*` `expression_name*` `input_name?` `disconnect_outputs=false` `target_expression?` `output_index?` | Remove connections. `target_expression`/`output_index` require `disconnect_outputs=true` |
| `[w] delete_expression` | `asset_path*` `expression_name*` | Delete node |
| `[w] delete_expressions` | `asset_path*` `expression_names*` | Batch delete; `expression_names`=array of name strings |
| `[w] clear_graph` | `asset_path*` `preserve_parameters=false` | Remove all expressions (`preserve_parameters` keeps Scalar/Vector/etc. params) |
| `[w] create_material_instance` | `asset_path*` `parent_material*` `scalar_parameters?` `vector_parameters?` `texture_parameters?` `static_switch_parameters?` | Create MIC. `vector_parameters`=`{name:{R,G,B,A}}`, `texture_parameters`=`{name:path}` |
| `[w] set_instance_parameter` | `asset_path*` `parameter_name*` `scalar_value?` `vector_value?` `texture_value?` `switch_value?` | Set one instance param. `vector_value`=`{R,G,B,A}` |
| `[w] duplicate_material` | `source_path*` `dest_path*` | Duplicate |
| `[w] recompile_material` | `asset_path*` `include_stats?` | Force recompile. `include_stats:true` waits + returns errors/instruction counts |
| `[w] import_material_graph` | `asset_path*` `graph_json*` `mode=overwrite` | Import from JSON. `mode`: overwrite/merge |
| `[w] begin_transaction` | `transaction_name*` | Open undo group |
| `[w] end_transaction` | (none) | Close undo group |

### Batch (4)

| Action | Signature | Purpose |
|--------|-----------|---------|
| `[w] batch_set_material_property` | `asset_paths*` `blend_mode?` `shading_model?` `material_domain?` `two_sided?` `opacity_mask_clip_value?` `dithered_lod_transition?` `fully_rough?` `cast_shadow_as_masked?` `used_with_*?` | Same props to multiple materials (same enum/usage flags as `set_material_property`) |
| `[w] batch_recompile` | `asset_paths*` | Recompile multiple (Max: 200), returns instruction counts |
| `[w] import_texture` | `source_file*` `dest_path*` `dest_name?` `compression=Default` `srgb=true` `lod_group?` `max_size?` `replace_existing=false` | Import from disk (material namespace). `compression`: Default/Normalmap/NormalmapBC5/NormalmapLA/Grayscale/Alpha/Masks/HDR/...; `max_size` 0=no limit |
| `preview_textures` | `asset_paths*` `per_texture_size=128` `output_path?` | Contact sheet of multiple textures (Max: 100) |

### Compound (1)

| Action | Signature | Purpose |
|--------|-----------|---------|
| `[w] create_pbr_material_from_disk` | `material_path*` `texture_folder*` `maps*` `blend_mode=Opaque` `shading_model=DefaultLit` `material_domain=Surface` `two_sided=false` `max_texture_size=2048` `opacity_from_alpha=false` `replace_existing=false` | Import PBR textures + create material + build graph + compile. `maps` keys: basecolor/albedo, normal, roughness, metallic/metalness, ao, height, emissive, opacity |

### asset namespace — texture/font ingest & lifecycle (12)

Call these with `asset_query()`. Used by PBR builds for the source texture/font assets and for generic save/delete/inspect hygiene.

| Action | Signature | Purpose |
|--------|-----------|---------|
| `[w] import_texture_from_file` | `source_path*` `destination*` `asset_name?` `settings?` `replace_existing=false` `overwrite_policy?` | Import texture from disk. `source_path` aliases: source_file/file_path/path; `settings`=`{compression, srgb, tiling, max_size, lod_group}` (compression accepts TC_* plus UI/UserInterface2D aliases) |
| `[w] import_texture_from_bytes` | `destination*` `bytes_b64*` `format_hint*` `texture_role?` `settings?` `save=true` `return_processed_png=false` | Import texture from base64 bytes. `format_hint`: png/jpg/jpeg/bmp/exr/tga/hdr/tif/tiff/dds; `texture_role`: ui_icon/sprite/decal/basecolor/world_tile/normal/orm_mask/height/emissive |
| `[w] import_font_family` | `destination*` `family_name*` `faces*` `loading_policy=LazyLoad` `hinting=Default` `save=true` | Import composite UFont. `faces`=typeface specs with `typeface` + absolute `source_path`; `loading_policy`: LazyLoad/Stream/Inline; `hinting`: Default/Auto/AutoLight/Monochrome/None |
| `[w] save_asset` | `asset_path*` | Save asset package |
| `[w] delete_assets` | `asset_paths*` `allowed_prefixes?` `dry_run=false` `force=false` | Delete assets. `allowed_prefixes` gate paths; `force` closes editors and deletes referenced assets |
| `validate_naming_conventions` | `scan_path=/Game` `max_results=100` `custom_rules?` | Scan naming violations. `custom_rules`=`{"ClassName":"Prefix_"}` |
| `list_supported_asset_enrichers` | (none) | List available asset enrichers |
| `inspect_asset` | `asset_path*` `include_references=true` `array_limit=32` | Generic reflected inspection |
| `inspect_assets_batch` | `asset_paths*` `include_references=false` `array_limit=16` | Batch reflected inspection |
| `validate_typed_asset` | `asset_path*` `array_limit=32` | Validate a typed asset (large-payload warnings) |
| `find_assets` | `query*` `_validate_types?` `path=/Game` `recursive=true` `class_names?` `limit=20` `threshold?` `include_tags=false` `include_score_breakdown=false` `allow_transposition=true` | Fuzzy live asset find. `class_names` e.g. Texture2D/Blueprint or /Script/Module.Class |
| `[w] batch_rename_assets` | `asset_paths*` `find?` `replace?` `add_prefix?` `remove_prefix?` `add_suffix?` `remove_suffix?` `dry_run=false` | Batch rename hygiene |

## PBR Workflow

**CRITICAL:** `build_material_graph` requires a `graph_spec` wrapper. Spec goes INSIDE `graph_spec`.

```
// Create + build graph
material_query({ action: "create_material", params: {
  asset_path: "/Game/Materials/M_Rock", shading_model: "DefaultLit"
}})
material_query({ action: "build_material_graph", params: {
  asset_path: "/Game/Materials/M_Rock", clear_existing: true,
  graph_spec: {
    nodes: [
      { id: "TexBC", class: "TextureSample", props: { Texture: "/Game/Textures/T_Rock_D" }, pos: [-400, 0] },
      { id: "TexN", class: "TextureSample", props: { Texture: "/Game/Textures/T_Rock_N", SamplerType: "Normal" }, pos: [-400, 200] },
      { id: "TexORM", class: "TextureSample", props: { Texture: "/Game/Textures/T_Rock_ORM" }, pos: [-400, 400] }
    ],
    outputs: [
      { from: "TexBC", from_pin: "RGB", to_property: "BaseColor" },
      { from: "TexN", from_pin: "RGB", to_property: "Normal" },
      { from: "TexORM", from_pin: "G", to_property: "Roughness" },
      { from: "TexORM", from_pin: "B", to_property: "Metallic" }
    ]
  }
}})
```

**graph_spec fields:**
- `nodes[]` -- `{ id, class, props?, pos? }`
- `custom_hlsl_nodes[]` -- `{ id, code, description?, output_type?, inputs?, additional_outputs?, pos? }`
- `connections[]` -- `{ from, to, from_pin?, to_pin? }` (inter-node)
- `outputs[]` -- `{ from, from_pin?, to_property }` (to material output)

`clear_existing: true` clears expressions but preserves material properties. Blend mode warnings are informational (connection made but inactive until mode matches).

### One-Shot PBR from Disk
```
material_query({ action: "create_pbr_material_from_disk", params: {
  material_path: "/Game/Materials/SIGIL/M_BloodConcrete",
  texture_folder: "/Game/Textures/SIGIL/BloodConcrete",
  maps: { basecolor: "D:/output/basecolor.png", normal: "D:/output/normal.png", roughness: "D:/output/roughness.png" },
  max_texture_size: 2048
}})
```

For decals: add `blend_mode: "Translucent"`, `material_domain: "DeferredDecal"`, `opacity_from_alpha: true`.

## Editing Existing Materials

Inspect first: `get_all_expressions` + `get_full_connection_graph`. Wrap in `begin_transaction`/`end_transaction` for undo. Use `export_material_graph` to snapshot before destructive changes (`include_properties: false` reduces ~70%).

## Tiling Quality

Action side: call `check_tiling_quality` to auto-flag missing anti-tiling / macro variation, and `render_preview` with `uv_tiling: 3` (or `5`) to inspect repetition before finalizing. For the full anti-tiling checklist, technique selection, macro-variation values, and FluidNinja texture recommendations, see the **material-reference** skill (anti-tiling reference).

## AI Introspection (editor:: actions)

For AI agents reasoning about materials without round-tripping through `render_preview`:

- `editor_query("inspect_material_pbr", { asset_path })` — pure reflection walk of `UMaterialInterface`. Classifies texture params by name (basecolor/normal/roughness/metallic) and detects ORM/ARM/MRA channel packing. Returns structured JSON, no render.
- `editor_query("capture_material_grid", { material_paths: [...] })` — N material instances (1-16) side-by-side under shared lighting; auto-grid `ceil(sqrt(N))` columns. Returns one PNG.
- `editor_query("capture_scene_preview", { asset_path, asset_type: "material" })` — single-material preview render.

See `monolith_guide(section="recipes")` entries "Visual introspection — going beyond thumbnails" and "Reading asset structure without rendering".

## Rules

- Graph editing only works on base Materials, not MICs
- Always `validate_material` after graph changes
- `build_material_graph` is fastest for complex graphs (single JSON)
- Use `render_preview` or `get_thumbnail` with `save_to_file: true` (no base64)
- `get_compilation_stats` catches runaway instruction counts after changes
