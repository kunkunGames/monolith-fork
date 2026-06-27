# Monolith — MonolithImageGen Module

**Parent:** [../SPEC_CORE.md](../SPEC_CORE.md)
**Engine:** Unreal Engine 5.7+
**Owner module:** `MonolithImageGen`
**Namespace:** `imagegen`
**MCP tool:** `imagegen_query`
**Status:** Implemented (2026-06-05 SVG source actions + initial MSDF Texture2D/material pipeline; 2026-05-21 namespace split + ima2 bridge + source PNG mirror); SVG raster preview conversion planned

---

## 1. Scope

`MonolithImageGen` owns the `imagegen` namespace: generated-image provider discovery, deterministic local placeholder Texture2D generation, ima2/imag2-gen HTTP generation, caller-supplied generated-image import, postprocessed generated PNG archival, redacted provenance metadata, safe SVG source generation/import/validation for web and Unreal editor source files, and editor-time MSDF Texture2D/material generation from validated `msdf_source` SVG.

## 2. Namespace Ownership

Action implementations live in `Source/MonolithImageGen` and export through `MONOLITHIMAGEGEN_API`. `FMonolithImageGenActions` owns image-generation provider/import/provenance registration. `MonolithImageGen::ShutdownModule` unregisters the `imagegen` namespace through owner-scoped action cleanup.

The module depends on `MonolithAsset` for the exported `MonolithAsset::FTextureIngestActions::HandleImportTextureFromBytes` helper, which remains the single Texture2D import path for generated image bytes. HTTP generation is isolated to the ima2/imag2-gen bridge; provider credentials remain outside Monolith. Reference image path/base64 normalization, root-level reference PNG archival, and postprocessed generated PNG mirroring are owned by `MonolithImageGen`.

## 3. Registered Actions

| Action | Purpose |
|--------|---------|
| `list_image_models` | List Monolith-native and external-boundary image generation providers. |
| `get_image_generation_defaults` | Return default provider/model, ima2 bridge settings, destination, source/reference PNG directories, aspect ratios, payload cap, texture settings, and prompt policy. |
| `generate_image` | Generate a deterministic local PNG placeholder, import it as a Texture2D, and save a postprocessed PNG mirror when enabled. |
| `generate_image_via_ima2` | POST a generation request to an external ima2/imag2-gen server, import the first returned image as a Texture2D, save a postprocessed PNG mirror when enabled, and attach redacted provenance metadata. |
| `import_generated_image` | Import external base64 image bytes or a local generated image file as a Texture2D, save a postprocessed PNG mirror when enabled, and attach redacted provenance metadata. |
| `generate_svg` | Generate a deterministic sanitized SVG source from `svg_spec` or prompt placeholder metadata, write a `.svg` plus sidecar when requested, and report geometry/MSDF readiness. |
| `import_generated_svg` | Import externally generated SVG text, base64 bytes, or a local `.svg` file through the same sanitizer/provenance boundary. |
| `validate_svg` | Validate and summarize SVG source without writing files, including sanitizer removals, topology checks, and `msdf_ready` blockers. |
| `generate_msdf_from_svg` | Convert a validated `msdf_source` SVG into a generated MSDF PNG/Texture2D, verify pixel/channel samples, and optionally build/render an unlit MSDF material preview. |
| `get_generated_asset_provenance` | Read `Monolith.Generated.*` metadata from a generated Texture2D asset. |

## 4. Build.cs Dependencies

| Scope | Modules |
|-------|---------|
| Public | `Core`, `CoreUObject`, `Engine`, `MonolithCore` |
| Private | `MonolithAsset`, `UnrealEd`, `HTTP`, `ImageWrapper`, `Json`, `JsonUtilities`, `XmlParser` |
| Runtime action dependency | Optional `generate_msdf_from_svg.create_material` calls the registered `material` namespace dynamically to create and preview an MSDF material; `MonolithImageGen` does not take a compile-time `MonolithMaterial` dependency. |

## 5. Settings

| Setting | Default | Effect |
|---------|---------|--------|
| `bEnableImageGen` | `true` | Enables `MonolithImageGen` startup registration for `imagegen` actions. Restart required after changing. |
| `ImageGenBridgeServerUrl` | `http://192.168.1.147:3333` | Base URL for the external ima2/imag2-gen server used by `generate_image_via_ima2`. |
| `ImageGenBridgeProvider` | `oauth` | Provider forwarded to ima2. `oauth` uses the server host's Codex OAuth session; `api` requires the server host to provide `OPENAI_API_KEY`. |
| `ImageGenBridgeDefaultModel` | `gpt-5.5` | Model forwarded to ima2 when `generate_image_via_ima2` omits `model`. |
| `ImageGenBridgeTimeoutSeconds` | `420.0` | Blocking HTTP timeout for a generation request. |

## 6. Provider Boundary

`generate_image` is local deterministic only: provider `local_deterministic`, model `monolith/local-gradient-png-v1`, output PNG. It does not call remote providers or read API keys. The legacy `monolith/local-gradient-bmp-v1` model name is accepted as an alias for compatibility but still produces PNG.

`generate_image_via_ima2` calls the configured ima2/imag2-gen `/api/generate` endpoint. By default it targets `http://192.168.1.147:3333` with `provider="oauth"`, `model="gpt-5.5"`, `quality="high"`, `size="1024x1024"`, `format="png"`, `background="auto"`, `compose_prompt=true`, and `moderation="low"`. Monolith accepts only PNG bridge output; JPEG, WebP, and other provider formats are rejected before the bridge call so generated assets enter the project through one lossless Texture2D path. Monolith does not read, store, or forward OpenAI API keys; OAuth/API-key ownership stays on the ima2/imag2-gen server host.

Provider HTTP failures return structured diagnostics. For ima2/OpenAI parameter rejection, `error_data.request_summary` includes only safe routing fields (`provider`, `model`, `quality`, `size`, `format`, `provider_background`, `moderation`, `mode`, and `reference_count`) plus a retry hint; prompt text, references, request IDs, sessions, and credentials are not echoed. Provider rate limits (HTTP 429, bridge code `RATE_LIMITED`, or a relayed "rate limit" message) additionally set `error_data.error_class="provider_rate_limited"`, copy a bridge-provided retry window into `error_data.retry_after_seconds` when present, and attach a hint telling agents not to retry the identical request inside the provider window — added 2026-06-12 after invocation logs showed 19-25x identical-signature retry storms against a rate-limited provider.

External provider results still enter Monolith through the same Texture2D import boundary. `generate_image`, `generate_image_via_ima2`, and `import_generated_image` import through `MonolithAsset::FTextureIngestActions::HandleImportTextureFromBytes`, save the role-postprocessed imported pixels as a PNG mirror by default when the imported package is saved, and tag assets with redacted `Monolith.Generated.*` metadata. `generate_image_via_ima2` and `import_generated_image` validate compressed payload size and accept PNG generated payloads only before import; `import_generated_image` accepts either `bytes_b64` or `file_path`/`path` for local generated PNG files. Prompt text is not persisted; provenance stores prompt hashes plus `prompt_redacted=true`. Generated imports default to `texture_role="basecolor"` unless the caller supplies another role.

## 7. Paths, Resolution, and References

| Concern | Contract |
|---------|----------|
| Default asset destination | If neither `destination` nor `asset_path` is supplied, generated Texture2D packages are imported under `/Game/GeneratedImages`. |
| Explicit destination | `destination` still overrides `asset_path` + `asset_name`; otherwise `asset_path` chooses the Unreal folder and `asset_name` chooses the sanitized texture asset name. |
| Resolution | `generate_image_via_ima2` accepts `size` (`"1024x1024"`) or `resolution`; `resolution` accepts a number, `"WIDTHxHEIGHT"` string, `[width,height]`, or `{width,height}` and overrides `size`. The local deterministic placeholder action accepts the same `resolution` shape and uses it instead of `aspect_ratio`. |
| Output format | `generate_image_via_ima2` accepts only `format="png"` for provider output. `import_generated_image` also accepts only PNG generated payloads. JPEG, WebP, and other formats are rejected before any bridge call or generated import. |
| Background | `generate_image_via_ima2` accepts caller `background` values `transparent`, `opaque`, and `auto`, but never forwards `transparent` to ima2/OpenAI. After validating role compatibility, Monolith sends provider `background="auto"` for omitted or transparent requests, stores both `requested_background` and `provider_background` in provenance, and requires PNG output. Data and tile roles (`world_tile`, `normal`, `orm_mask`, `height`) reject `background="transparent"`. |
| Texture role | `generate_image`, `generate_image_via_ima2`, and `import_generated_image` accept `texture_role` and forward it to `asset.import_texture_from_bytes`. Supported roles are `ui_icon`, `sprite`, `decal`, `basecolor`, `world_tile`, `normal`, `orm_mask`, `height`, and `emissive`; `generate_image_via_ima2` validates the role before calling the bridge. The action result returns `settings_applied` and `validation`. |
| Prompt composition | `generate_image_via_ima2` accepts `compose_prompt`. It defaults to `true`, which appends Unreal Texture2D and `texture_role` constraints to the provider prompt, including strict evenly spaced grid constraints for multi-frame `sprite` output. The provider `background` option remains `auto` for omitted or transparent requests, including alpha roles (`ui_icon`, `sprite`, `decal`), for ima2/gpt-5.5 compatibility; the composed prompt still asks for transparent output and the import path can extract edge-background alpha. Provenance records only prompt hashes (`prompt_hash` for the effective prompt and `caller_prompt_hash` for the caller prompt). Set `compose_prompt=false` to send the caller `prompt` verbatim. |
| Generated source archive | `generate_image`, `generate_image_via_ima2`, and `import_generated_image` accept `save_source_png`. It defaults to the `save` value, so normal saved generations also write a postprocessed PNG under `<ProjectDir>/GeneratedImages` using the imported asset path relative to `/Game/GeneratedImages` when applicable. Example: `/Game/GeneratedImages/basecolor/T_Stone` mirrors to `<ProjectDir>/GeneratedImages/basecolor/T_Stone.png`. For destinations outside `/Game/GeneratedImages`, the path is mirrored relative to `/Game`, e.g. `/Game/Tests/T_Image` -> `<ProjectDir>/GeneratedImages/Tests/T_Image.png`. Provenance stores `source_png_kind="postprocessed"` when the mirror came from imported role-processing output. |
| Reference input | `generate_image_via_ima2` accepts `references`, `reference_images`, `reference_image_paths`, `reference_png_paths`, and `reference_asset_paths`. Image items can be local file paths, data URLs, raw base64 strings, or objects with `path`, `file_path`, `bytes_b64`, `data_url`, and optional `format_hint`; `reference_png_paths` is the explicit local-PNG path alias. |
| Texture reference input | `reference_asset_paths` accepts Unreal Texture2D package paths or object paths, including objects with `asset_path`, `path`, or `package_path`. Monolith loads the Texture2D, reads valid Source art from the top mip, converts TSF_BGRA8/TSF_G8/TSF_G16 source data to PNG, archives that PNG, and forwards it with the prompt as an ima2 reference. |
| Reference archive | Every reference image is decoded or extracted, re-encoded as PNG, saved under `<ProjectDir>/GeneratedImages`, and forwarded to ima2 as PNG base64 before the `/api/generate` call. The action result returns `reference_png_dir` and `reference_png_files`; provenance stores `reference_count` and reference PNG hashes. |
| External import input | `import_generated_image` accepts PNG `bytes_b64` for inline generated bytes or `file_path`/`path` for a local generated PNG file. Local-file imports use provenance `source="external_file"`; inline imports use `source="external_bytes"`. |
| MSDF destination | `generate_msdf_from_svg` defaults to `/Game/GeneratedImages/MSDF`, writes a postprocessed MSDF PNG mirror when `save_source_png=true`, and imports through the same generated Texture2D boundary with explicit MSDF texture settings. |

## 8. SVG / Vector Source Extension

### 8.1 Goals

The P1 SVG extension adds a vector-source path to the `imagegen` namespace without weakening the current generated PNG Texture2D contract. SVG output is treated as an authored source artifact first, not as a trusted Unreal runtime asset. The same source must be usable for:

| Use case | Output contract |
|----------|-----------------|
| Web | Sanitized standalone `.svg` file with stable `viewBox`, no script, no external resources, and optional returned inline SVG text. |
| Unreal editor | Sanitized `.svg` source under the project `GeneratedImages` tree plus provenance sidecar; optional editor-only helper paths may consume it for Slate/vector-image previews later. |
| Runtime MSDF | A stricter `msdf_source` profile that proves the SVG can be converted into path contours suitable for editor-time Multi-channel Signed Distance Field texture generation. |

Source and runtime non-goals:

- Do not import SVG directly as a `UTexture2D`.
- Do not render SVG at game runtime. Runtime consumers should sample precomputed PNG/Texture2D or MSDF atlas assets; live SVG parsing, tessellation, rasterization, or contour conversion belongs in editor/import/cook-time tooling only.
- Do not make `generate_svg` or `import_generated_svg` create runtime assets. Conversion belongs in explicit conversion actions such as `generate_msdf_from_svg`.
- Do not call external text/vector generation providers from Monolith or read API keys.
- Do not accept arbitrary browser-grade SVG. Unsupported features must be rejected or reported before any file is written.

### 8.2 Research Basis and Generation Philosophy

The SVG extension follows a source-first pipeline:

1. Generate or import a small, sanitized SVG source.
2. Canonicalize the SVG into deterministic source text and bounded vector geometry.
3. Validate the geometry profile required by the intended consumer.
4. Convert once, at editor/import/cook time, into the runtime format: PNG/Texture2D preview or MSDF atlas.
5. Runtime rendering samples textures/materials only.

The important distinction is that a browser-valid SVG is not automatically a font-valid or MSDF-valid vector shape. SVG fill behavior is intentionally flexible: the SVG fill model defines inside/outside through `fill-rule`, and open subpaths may still be filled as if closed. Browsers may also render valid segments up to a path-data error. That behavior is useful for web display but unsafe as the acceptance contract for generated game resources. Monolith must therefore validate path grammar and geometry strictly before writing files.

MSDF and font-oriented output must be treated as contour topology, not decorative SVG markup. The MSDF construction literature represents shapes as closed, oriented contours made from line/quadratic/cubic edge segments and explicitly requires no self-intersections. Font validation tools make the same practical assumptions: no open contours, no intersecting paths, and consistent contour direction. OpenType can represent overlapping contours, but broad interoperability can require overlap flags or contour merging, so Monolith's `msdf_source` profile should normalize to merged, non-overlapping contours instead of relying on renderer-specific overlap behavior.

Generation philosophy:

| Principle | Contract |
|-----------|----------|
| Source SVG is not runtime SVG | SVG is a versionable authoring source. Runtime uses generated textures/MSDF, never live SVG parsing. |
| Simple filled contours beat decorative SVG | Prefer closed filled paths and basic shapes. Avoid filters, masks, CSS, animation, text, gradients, and stroke semantics for MSDF. |
| Sanitized is not geometry-valid | XML/security sanitization is P1. Geometry/topology validation is a separate pass with separate result fields. |
| Reject ambiguous topology | Self-intersections, overlapping filled contours, open paths, zero-area contours, near-zero edges, tangent duplicates, and ambiguous fill rules must block `msdf_ready`. |
| Bake presentation into geometry | Transforms, symbols, use references, and strokes must be flattened or rejected before MSDF conversion. |
| Holes must be explicit | A hole must be represented as a separate contour with a known opposite winding, or as a canonicalized path set after boolean normalization. |
| Keep the coordinate system boring | Stable `viewBox`, bounded coordinate magnitude, bounded precision, deterministic contour order, and enough margin around the shape are required. |
| Keep repair explicit | Auto-repair may create a normalized companion SVG in a later phase, but the result must report every repair and keep the original source hash for audit. P1 does not perform topology-changing repair. |

P1 implementation intentionally favors a narrow, predictable subset over full SVG rendering fidelity. The sanitizer uses UE `XmlParser` after preflight rejection of DTD/entity expansion, XML stylesheets, CSS imports, and unsafe tags. The path grammar accepts bounded `M/L/H/V/Q/C/Z` path data plus generated `rect` and `polygon` primitives; arcs and shorthand commands are rejected until a conversion path can flatten them deterministically.

### 8.3 Action Phases

| Action | Phase | Purpose |
|--------|-------|---------|
| `generate_svg` | P1 implemented | Build a deterministic sanitized SVG from `svg_spec` and optional `prompt` metadata. A prompt-only call creates a deterministic placeholder vector and is not AI generation. |
| `import_generated_svg` | P1 implemented | Import externally generated SVG text, base64 bytes, or a local `.svg` file through the same sanitizer/provenance boundary. This is the safe boundary for LLM/provider-created SVG. |
| `validate_svg` | P1 implemented | Validate and summarize an SVG without writing files. Returns profile readiness, node counts, bounds, blocked features, warnings, and `msdf_ready`. |
| `rasterize_svg_to_texture` | P2 planned, not registered | Optional future conversion from sanitized SVG source to PNG/Texture2D preview. Requires a chosen rasterizer implementation and explicit verification before registration. |
| `generate_msdf_from_svg` | P3 initial implemented | Convert validated `msdf_source` SVG into a local CPU-baked MSDF PNG/Texture2D, apply MSDF-safe texture settings, verify sampled pixels/channels, and optionally create/render a material preview. |

Conversion actions must not be stub-registered unless they can return deterministic, verified output or an explicit unavailable error with a concrete dependency reason.

### 8.4 Action Schemas

#### `imagegen.generate_svg`

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `svg_spec` | object | optional | Deterministic vector description. P1 supports `paths[].d`, `rects`, `polygons`, `viewBox`/`view_box`, `width`, and `height`. Required unless `prompt` placeholder mode is accepted. |
| `prompt` | string | optional | Human prompt for provenance only. Stored as a hash; not persisted verbatim. |
| `profile` | enum | optional | `web`, `editor`, or `msdf_source`. Default: `editor`. |
| `destination` | string | optional | Logical `/Game/...`-style destination used to derive the source file path. Does not create a `.uasset` in P1. |
| `asset_path` | string | optional | Folder-style alias used with `asset_name` when `destination` is omitted. Default: `/Game/GeneratedImages/Vector`. |
| `asset_name` | string | optional | Sanitized source basename. `V_` prefix is added when absent. |
| `overwrite_policy` | enum | optional | `unique` or `fail`. Default: `unique`. |
| `view_box` | array/object/string | optional | Explicit SVG viewBox. If omitted, derived from width/height or spec bounds. |
| `width` / `height` | number | optional | Optional display dimensions. The canonical `viewBox` remains authoritative. |
| `return_svg` | bool | optional | Include sanitized SVG text in the result. Default: `false` when saved, `true` when `save=false`. |
| `save` | bool | optional | Write `.svg` and `.json` provenance sidecar. Default: `true`. |
| `strict` | bool | optional | Treat sanitizer warnings as errors. Default: `true` for `msdf_source`, `false` otherwise. |
| `geometry_policy` | enum | optional | `sanitize_only`, `validate`, or `normalize`. Default: `validate`. In P1, `normalize` canonicalizes and validates but does not perform topology-changing boolean repair. |
| `fill_rule_policy` | enum | optional | `preserve`, `nonzero`, or `reject_evenodd`. Default: `preserve` for web/editor and `nonzero` for `msdf_source`. |
| `margin` | number | optional | Additional source-space margin for future raster/MSDF framing. Ignored for pure validation. |

#### `imagegen.import_generated_svg`

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `svg_text` | string | conditional | Inline SVG text. Required unless `bytes_b64` or `file_path` is supplied. |
| `bytes_b64` | string | conditional | Base64 SVG bytes. Data URL prefix `data:image/svg+xml;base64,` is allowed. |
| `file_path` / `path` | string | conditional | Local SVG file path. |
| `format_hint` | string | optional | Must be `svg` or `svg+xml` when present. |
| `prompt` | string | optional | External generation prompt for provenance hash only. |
| `provider` | string | optional | External provider id for provenance. Default: `external`. |
| `model` | string | optional | External model id for provenance. Default: `unknown`. |
| `profile` | enum | optional | `web`, `editor`, or `msdf_source`. Default: `editor`. |
| `destination` / `asset_path` / `asset_name` | string | optional | Same path contract as `generate_svg`. |
| `overwrite_policy` | enum | optional | `unique` or `fail`. Default: `unique`. |
| `return_svg` | bool | optional | Include sanitized SVG text in the result. |
| `save` | bool | optional | Write sanitized `.svg` and provenance sidecar. Default: `true`. |
| `strict` | bool | optional | Treat sanitizer warnings as errors. Default: `true` for `msdf_source`, `false` otherwise. |
| `geometry_policy` | enum | optional | `sanitize_only`, `validate`, or `normalize`. Same behavior as `generate_svg`. |
| `fill_rule_policy` | enum | optional | `preserve`, `nonzero`, or `reject_evenodd`. Same behavior as `generate_svg`. |
| `margin` | number | optional | Additional source-space margin for future raster/MSDF framing. |

#### `imagegen.validate_svg`

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `svg_text` | string | conditional | Inline SVG text. Required unless `file_path`, `path`, or `bytes_b64` is supplied. |
| `file_path` / `path` | string | conditional | Local SVG file path. |
| `bytes_b64` | string | conditional | Base64 SVG bytes or data URL. |
| `profile` | enum | optional | `web`, `editor`, or `msdf_source`. Default: `editor`. |
| `strict` | bool | optional | Treat warnings as errors. Default: `false` for validation. |
| `return_sanitized_svg` | bool | optional | Include sanitized SVG text in result. Default: `false`. |
| `geometry_policy` | enum | optional | `sanitize_only`, `validate`, or `normalize`. Default: `validate`. |
| `fill_rule_policy` | enum | optional | `preserve`, `nonzero`, or `reject_evenodd`. |

#### `imagegen.generate_msdf_from_svg`

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `svg_text` | string | conditional | Inline SVG source. Required unless `bytes_b64` or `file_path`/`path` is supplied. The action sanitizes it with `profile="msdf_source"` before conversion. |
| `bytes_b64` | string | conditional | Base64 SVG bytes or `data:image/svg+xml;base64,...` input. |
| `file_path` / `path` | string | conditional | Local SVG source path. |
| `size` / `resolution` | int/string/array/object | optional | Square output texture size. `resolution` overrides `size`; the current MSDF action requires square dimensions. Default comes from imagegen defaults. |
| `pixel_range` | number | optional | Distance-field normalization range in output pixels. Larger values preserve a wider edge band for material thresholding. |
| `destination` / `asset_path` / `asset_name` | string | optional | Texture2D package destination. Defaults to `/Game/GeneratedImages/MSDF`. |
| `overwrite_policy` | enum | optional | `unique` or `fail`. Default: `unique`. |
| `save` | bool | optional | Save the generated Texture2D package. Default: `true`. |
| `save_source_png` | bool | optional | Write the baked MSDF PNG mirror under `<ProjectDir>/GeneratedImages`. Defaults to `save`. |
| `return_png` | bool | optional | Include the generated PNG as base64 in the result. Default: `false` when saved, `true` when `save=false`. |
| `verify_samples` | bool | optional | Sample center/outside/edge pixels, median distance, and channel spread; fail the action if validation does not pass. Default: `true`. |
| `create_material` | bool | optional | Create an unlit masked MSDF material using the generated texture. Default: `false`. |
| `verify_material_render` | bool | optional | Render and decode a material preview PNG to prove the material is non-empty/non-uniform. Implies `create_material`. Default: `false`. |
| `material_destination` / `material_asset_path` / `material_asset_name` | string | optional | Material package destination when `create_material=true`. |
| `material_overwrite_policy` | enum | optional | `unique` or `fail`. Default: `unique`. |

### 8.5 SVG Profile Rules

| Rule | `web` | `editor` | `msdf_source` |
|------|-------|----------|---------------|
| Valid XML and `<svg>` root | Required | Required | Required |
| Stable `viewBox` | Required | Required | Required |
| External URLs/resources | Reject | Reject | Reject |
| JavaScript, event attrs, `foreignObject` | Reject | Reject | Reject |
| Embedded raster images | Reject by default | Reject by default | Reject |
| Filters, animation, CSS imports | Reject | Reject | Reject |
| Gradients | Allow sanitized | Allow sanitized | Reject |
| Text nodes | Allow sanitized | Allow sanitized | Reject unless converted to paths before import |
| Stroke-only geometry | Allow | Allow | Warn or reject; MSDF-ready output should be filled closed paths |
| Paths and basic shapes | Allow | Allow | Allow after conversion to path contours |
| Clip/mask | Allow only if bounded and sanitizer-supported | Allow only if bounded and sanitizer-supported | Reject |
| Transforms / `use` / symbols | Allow if sanitizer-supported | Allow if sanitizer-supported | Flatten or reject |
| `fill-rule="evenodd"` | Allow sanitized | Allow sanitized | Normalize to nonzero or reject with blocker |
| Multiple overlapping filled shapes | Allow sanitized | Allow sanitized | Boolean-union or reject |

All profiles must cap input size, XML depth, element count, path command count, coordinate magnitude, and output byte size. The sanitizer must reject DTD/entity expansion and never resolve external references. XML namespace attributes such as `xmlns` are allowed but treated only as declarations, not fetchable resources.

### 8.6 MSDF Source Geometry Rules

`msdf_source` is stricter than SVG validity. It is a promise that the source can become an oriented contour shape suitable for distance-field generation.

| Geometry issue | `msdf_source` behavior |
|----------------|------------------------|
| Invalid path grammar | Reject. Do not rely on SVG user-agent partial rendering. |
| Open subpath | Reject unless explicitly closed by a normalization step and reported. |
| Self-intersection | Reject. A bow-tie path or any edge crossing itself is not MSDF-ready. |
| Intersecting contours | Reject unless a deterministic boolean union/difference step removes the overlap and records the repair. |
| Overlapping filled contours | Reject or normalize to a merged contour set; do not rely on OpenType overlap flags or renderer behavior for `msdf_ready`. |
| Wrong hole winding | Normalize or reject. External contours and cut-out contours must have opposite, recorded winding. |
| `evenodd` holes | Normalize to explicit nonzero winding contours or reject. |
| Stroke, cap, join, miter | Reject unless converted into filled outlines and then revalidated for overlap/self-intersection/slivers. |
| Text elements | Reject unless converted to path outlines before import, because font availability and shaping are external dependencies. |
| Zero-area contour | Reject. |
| Near-zero segment or duplicate adjacent points | Reject or simplify with a reported repair. |
| Extremely acute sliver or tiny island | Warn for web/editor; block `msdf_ready` unless within configured tolerance. |
| Unbaked transform | Flatten or reject. |
| Unbounded coordinate magnitude or excessive precision | Reject or quantize with a reported repair. |

The preferred normalized MSDF source is a single SVG root containing only path elements with filled, closed contours, canonical `viewBox`, explicit `fill`, no stroke, no transform, no external style, deterministic contour order, and a provenance sidecar that records geometry validation.

### 8.7 Runtime Consumption Policy

Runtime SVG rendering is out of scope because it shifts parsing, style resolution, geometry flattening, tessellation/rasterization, caching, and error handling into gameplay time. That work is hard to bound and easy to hitch. Monolith should instead generate runtime-ready outputs before play:

| Consumer | Runtime asset |
|----------|---------------|
| Web/docs preview | Sanitized `.svg` may be used directly outside Unreal runtime. |
| Unreal editor preview | Cached raster preview or editor-only vector preview is allowed. |
| UMG/game UI | PNG/Texture2D or MSDF Texture2D generated from SVG at import/cook time. |
| 3D/world glyph/icon | MSDF/MTSDF Texture2D plus material and metrics metadata generated at import/cook time. |

If a future action exposes editor-side SVG preview, it must be clearly editor-only and must cache its raster/vector output. A game runtime feature must never depend on reparsing `.svg` source files.

### 8.8 Storage and Provenance

SVG source files mirror the existing generated PNG source convention but do not imply a `UTexture2D` asset:

| Concern | Contract |
|---------|----------|
| Default source root | `<ProjectDir>/GeneratedImages/Vector`. |
| Destination mapping | `/Game/GeneratedImages/Vector/V_Icon` writes `<ProjectDir>/GeneratedImages/Vector/V_Icon.svg`. Destinations outside `/Game/GeneratedImages` mirror relative to `/Game`, e.g. `/Game/UI/Icon/V_Play` -> `<ProjectDir>/GeneratedImages/UI/Icon/V_Play.svg`. |
| Sidecar | Each saved SVG writes `<basename>.monolith.json` beside the SVG. The sidecar stores provenance hashes, profile, validation summary, sanitizer version, source hash, sanitized hash, and future conversion hints. |
| Prompt privacy | Prompt text is never written. Store `prompt_hash`, `caller_prompt_hash` where applicable, and `prompt_redacted=true`. |
| Metadata | Because P1 creates source files rather than assets, metadata lives in the sidecar. If P2 creates a Texture2D preview, the existing `Monolith.Generated.*` metadata keys should point back to `source_svg_path` and `source_svg_hash`. |
| P4/source control | Write actions must use normal Monolith source-control helpers when available, but the first implementation may keep source files on disk and report paths for caller-managed checkout/add. |
| Normalized companion | Future topology-changing normalization should write `<basename>.normalized.svg` or record that the saved `.svg` is already normalized. P1 records canonical sanitized hashes and reports blockers instead of rewriting topology. |

### 8.9 Result Shape

All P1 actions return a structured object:

| Field | Description |
|-------|-------------|
| `success` | Boolean success marker. |
| `profile` | Effective profile. |
| `source_svg_path` | Written SVG path when `save=true`. |
| `sidecar_path` | Written provenance sidecar path when `save=true`. |
| `svg_hash` | Hash of sanitized SVG text. |
| `original_svg_hash` | Hash of input SVG when importing. Omitted for deterministic generation. |
| `view_box` | Canonical viewBox. |
| `bounds` | Computed geometry bounds when available. |
| `element_count` / `path_count` / `path_command_count` | Bounded complexity summary. |
| `sanitizer_removed` | Array of stripped or rejected feature names. Empty when none. |
| `warnings` | Non-blocking warnings. |
| `svg_valid` | XML and supported SVG grammar validation result. |
| `sanitized` | Whether sanitizer completed. |
| `geometry_valid` | Geometry/topology validation result. |
| `msdf_ready` | Boolean readiness for P3 conversion. |
| `msdf_blockers` | Reasons `msdf_ready=false`. |
| `fill_rule` | Effective fill rule after normalization. |
| `contour_count` / `open_contour_count` | Contour summary. |
| `self_intersection_count` | Count of detected self-intersections when geometry backend can compute it. |
| `overlap_count` | Count of contour overlaps/intersections when geometry backend can compute it. |
| `winding_issues` | Array of detected winding/hole direction problems. |
| `repair_actions` | Array of sanitizer/normalizer repairs applied. |
| `svg_text` | Present only when requested by `return_svg`. |

`generate_msdf_from_svg` additionally returns:

| Field | Description |
|-------|-------------|
| `texture_asset_path` | Imported Texture2D package path. |
| `source_png_path` / `source_png_hash` | Baked MSDF PNG mirror and hash when `save_source_png=true`. |
| `width` / `height` / `pixel_range` | Generated MSDF dimensions and distance range. |
| `samples` | Named sample points with RGBA values, per-channel spread, and median-derived distance value. |
| `sample_validation` | Boolean and diagnostics for center/outside/edge/channel-spread checks. |
| `texture_settings` | Applied MSDF settings: `TC_Masks`, `sRGB=false`, `TMGS_NoMipmaps`, UI group, clamp addressing, `NeverStream=true`, and max texture size. |
| `material_asset_path` | Created material package path when `create_material=true`. |
| `material_render` | Preview render proof when requested, including decoded preview size and non-empty/non-uniform pixel stats. |

### 8.10 Implementation Notes

- Add SVG parsing/sanitizing code under `Source/MonolithImageGen`, not `MonolithAsset`, because P1 is source generation/provenance rather than texture import.
- Use UE's `XmlParser` module with a preflight boundary that rejects DTD/entity expansion, XML stylesheets, CSS imports, unsafe tags, unsafe attributes, and nonlocal references. Do not parse SVG with ad hoc string replacement.
- Keep path command validation bounded and deterministic. P1 supports `M/L/H/V/Q/C/Z` path commands and generated `rect`/`polygon` contours. Full SVG rendering fidelity is not required for P1, but unsupported constructs such as arcs, shorthand curves, filters, masks, `use`/symbol expansion, and unflattened transforms must be rejected or reported explicitly. Invalid path data must reject the whole resource instead of accepting the browser behavior of drawing only the valid prefix.
- Prefer a canonical writer that normalizes root attributes, element ordering where safe, numeric precision, and XML escaping so generated files are diffable.
- Reuse the existing prompt-hash/provenance helpers where possible. Do not store raw prompts, API keys, bearer tokens, cookies, or provider secrets.
- Geometry validation needs more than XML parsing. `msdf_ready=true` is claimed only after the sanitizer/path pass accepts closed, non-overlapping contours with supported grammar.
- Do not auto-expand strokes for `msdf_source` until the stroke expansion path is followed by remove-overlap/self-intersection validation. Complex strokes and tight curves can create topology artifacts.
- P2 rasterization should be implemented only after choosing a rasterizer that is deterministic in editor automation. The conversion result must go through `asset.import_texture_from_bytes` as PNG.
- The initial P3 MSDF path uses a Monolith-local CPU contour baker over the validated subset and imports PNG bytes through `asset.import_texture_from_bytes`. A future `msdfgen`-class dependency may replace the baker only if it keeps the same validation, texture settings, provenance, sample checks, and material preview proof.
- `generate_msdf_from_svg` applies the MSDF texture contract explicitly because `asset.import_texture_from_bytes` does not yet expose a general `texture_role="msdf"` role.

### 8.11 Verification Gates

| Gate | Required proof |
|------|----------------|
| Build | Primary Monolith `<Project>Editor Win64 Development` build succeeds after action registration. |
| Schema | Action registration tests prove `generate_svg`, `import_generated_svg`, `validate_svg`, and `generate_msdf_from_svg` are present; live `monolith_discover({ namespace: "imagegen", action: "<svg action>", mode: "schema" })` should also show exact params when an editor MCP endpoint is attached. |
| Sanitizer unit tests | Reject `script`, `onload`, `foreignObject`, external `href`, CSS imports, DTD/entity expansion, oversized inputs, path command bombs, and invalid XML. |
| Geometry validation tests | Reject bow-tie self-intersections, open contours, duplicate adjacent points, zero-area contours, wrong hole winding, overlapping filled shapes, invalid path grammar, and unflattened transforms under `msdf_source`. |
| Deterministic generation | Same `svg_spec`, profile, destination, and overwrite policy produce stable sanitized SVG text/hash in `save=false` tests. |
| Import round trip | `import_generated_svg` writes sanitized SVG + sidecar, `validate_svg` reads it back, hashes match, and raw prompt text is absent from the sidecar. |
| Profile checks | A gradient/text web SVG can pass `web`/`editor`; the same SVG reports `msdf_ready=false` with explicit blockers under `msdf_source`. |
| MSDF source fixture | A simple closed path icon reports `msdf_ready=true` under `msdf_source`. |
| MSDF Texture2D conversion | `generate_msdf_from_svg` tests must inspect actual generated PNG bytes, Texture2D source dimensions, compression/sRGB/mipmap/addressing/streaming settings, and provenance/source PNG mirror fields. |
| MSDF pixel/channel validation | Tests must sample representative inside, outside, and edge pixels and prove channel divergence exists where MSDF corner data is expected. |
| MSDF material render | When material creation is requested, tests must render and decode a preview PNG and prove it is non-empty and non-uniform under a real render path, not just graph-created. |
| Runtime policy | Runtime-facing tests or docs must prove consumers use precomputed Texture2D/MSDF assets and do not parse SVG during gameplay. |

P1 verification evidence:

| Date | Gate | Evidence |
|------|------|----------|
| 2026-06-05 | Build | `<Project>Editor Win64 Development` UBT build succeeded after SVG action registration. |
| 2026-06-05 | SVG automation | `Saved/Automation/MonolithImageGenSvgSource_20260605/index.json` reports 6/6 `MonolithImageGen.SvgSource` tests passing with no warnings or failures. |
| 2026-06-05 | MSDF Texture2D/material automation | `Saved/Automation/MonolithImageGenSvgSourceMsdf_20260605/index.json` reports 9/9 `MonolithImageGen.SvgSource` tests passing with no warnings or failures, including Texture2D setting checks, channel samples, invalid-source rejection, and material preview render decoding. |
| 2026-06-05 | Texture role regression | `Saved/Automation/MonolithImageGenTextureRoles_20260605_SvgSource/index.json` reports no failures; the one warning is the existing timeout fixture against `127.0.0.1:9`. |
| 2026-06-05 | Source index | `MonolithReindex -mode=project` completed with `errors=0`; `source search_source HandleGenerateSvg` finds the new SVG source file and CRG parity was repaired. |

### 8.12 Reference Notes

This design follows the current `msdfgen` input model for SVG/font/vector-shape conversion, the small-subset parser approach used by NanoSVG, the SVG 2 path and fill-rule behavior, OpenType overlap interoperability guidance, and font validation practice around closed non-intersecting contours. These references motivate the split between XML sanitization, geometry validation, future topology normalization, and runtime texture/MSDF consumption.

| Reference | Design implication |
|-----------|--------------------|
| [`msdfgen` README](https://github.com/Chlumsky/msdfgen) | Treat MSDF input as vector/font shape data, not as arbitrary SVG presentation markup. `msdfgen -svg` also documents a narrow SVG path-loading behavior, so Monolith should normalize source paths explicitly. |
| [NanoSVG](https://github.com/memononen/nanosvg) | A practical SVG import path can be useful while still intentionally small. Monolith follows that philosophy for P1: accept a safe path/basic-shape subset and report unsupported presentation features instead of pretending to be a browser renderer. |
| [Improved Corners with Multi-channel Signed Distance Fields](https://dcgi.fel.cvut.cz/wp-content/wpallimport-dist/publications/pdf/publications-2018-sloup-cgf-msdf-paper.pdf) | MSDF construction assumes closed, oriented edge contours and no self-intersections. This is the basis for `geometry_valid` and `msdf_ready` being separate from XML/SVG validity. |
| [SVG 2 Paths](https://w3c.github.io/svgwg/svg2-draft/paths.html) | SVG user agents may render valid path segments up to a path-data error. Generated asset tooling should reject invalid path data instead of accepting partial rendering. |
| [SVG 2 Filling and Stroking](https://www.w3.org/TR/2012/WD-SVG2-20120828/painting.html) | SVG fill rules make self-intersecting paths and enclosed subpaths renderable but ambiguous for MSDF/font topology. `msdf_source` must normalize or reject ambiguous fill behavior. |
| [OpenType `glyf` table](https://learn.microsoft.com/en-us/typography/opentype/spec/glyf) | Overlapping contours can exist in fonts, but broad interoperability may require overlap flags or merged contours. Monolith should merge/reject overlaps for MSDF rather than rely on renderer-specific behavior. |
| [FontForge validation](https://fontforge.org/docs/ui/dialogs/validation.html) | Font tooling validates closed contours, non-intersecting paths, and consistent contour direction. These constraints map directly to the `msdf_source` geometry gate. |
| [FontForge stroke expansion](https://fontforge.org/docs/techref/stroke.html) | Stroke expansion can create self-intersections and overlap-removal artifacts, especially with tight curves. `msdf_source` should reject strokes unless they are expanded and revalidated. |
