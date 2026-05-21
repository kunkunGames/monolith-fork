# Monolith ImageGen Module Split Verification

**Date:** 2026-05-21
**Engine:** Unreal Engine 5.7, resolved from `D:\P4\game\GO.uproject`
**Scope:** `Plugins/Monolith`
**Result:** PASS.

---

## 1. Change Under Test

The `imagegen` namespace implementation moved out of `MonolithUI` into a dedicated `MonolithImageGen` editor module, then gained the ima2/imag2-gen HTTP bridge action.

| Area | Result |
|------|--------|
| Module ownership | `MonolithImageGen` registers six `imagegen.*` actions through owner-scoped registry cleanup. |
| Texture import reuse | `MonolithImageGen` calls the exported `MonolithAsset::FTextureIngestActions::HandleImportTextureFromBytes` helper instead of duplicating import logic. |
| ima2 bridge | `imagegen.generate_image_via_ima2` calls the configured ima2/imag2-gen `/api/generate` endpoint and then reuses the same Texture2D import/provenance path. |
| Credential boundary | `ImageGenBridgeProvider=oauth` is the default. Monolith stores no API key; `provider=api` requires API-key configuration on the ima2/imag2-gen server host. |
| UI cleanup | `MonolithUI` no longer registers generated-image actions. |
| Settings | `UMonolithSettings::bEnableImageGen` controls startup registration. |

---

## 2. Verification Results

| Check | Command / source | Result |
|-------|------------------|--------|
| Stale source reference scan | Targeted scan over `Source`, `Docs`, `README.md`, and `Monolith.uplugin` for the old image-generation class and file identifiers. | Passed; no remaining source/doc references to the old class or filename. |
| Action registration count | `rg -c "Registry\.RegisterAction\(" Plugins\Monolith\Source\MonolithImageGen\Private\MonolithImageGenActions.cpp` | PASS: returned `6`. |
| UHT | `UnrealBuildTool.exe GoGameEditor Win64 Development -Project="D:\P4\game\GO.uproject" -WaitMutex -NoHotReloadFromIDE` | Passed; UHT processed `GoGameEditor`. |
| New module compile/link | Same UBT invocation | Passed through `Compile [x64] MonolithImageGenActions.cpp`, `Compile [x64] MonolithImageGenModule.cpp`, `Link [x64] UnrealEditor-MonolithImageGen.lib`, and `Link [x64] UnrealEditor-MonolithImageGen.dll`. |
| Full editor target link | Same UBT invocation after closing the running editor and checking out Perforce-managed binary metadata | PASS: `Result: Succeeded`. |

---

## 3. Notes

The first verification attempt was blocked by a running `UnrealEditor.exe` holding Monolith DLLs. The second attempt linked the DLLs but failed on read-only `UnrealEditor.modules`; after checking out the Perforce-managed metadata file, the final UBT invocation succeeded.

## 4. Follow-up Verification — Defaults, Resolution, and References

| Check | Command / source | Result |
|-------|------------------|--------|
| UHT | `UnrealBuildTool.exe GoGameEditor Win64 Development -Project="D:\P4\game\GO.uproject" -WaitMutex -NoHotReloadFromIDE` | Passed; UHT processed `GoGameEditor`. |
| Full editor target link | Same UBT invocation after closing the running editor and checking out Perforce-managed Monolith binaries plus `UnrealEditor.modules` | PASS: `Result: Succeeded`. |
| Live schema | `monolith_discover` over `namespace=imagegen` | PASS: `generate_image` exposes `resolution`; `generate_image_via_ima2` exposes `model=gpt-5.5`, `resolution`, `references`, `reference_images`, `reference_image_paths`, and `/Game/GeneratedImages` default destination. |
| Defaults action | `imagegen.get_image_generation_defaults` | PASS: returned provider `ima2-gen`, action `imagegen.generate_image_via_ima2`, `model=gpt-5.5`, `asset_path=/Game/GeneratedImages`, `ima2_server_url=http://192.168.0.10:3333`, `ima2_provider=oauth`, and `reference_png_dir=D:/P4/game/GeneratedImages`. |
| Local resolution + default path | `imagegen.generate_image` with `resolution=[128,64]`, no destination, asset name `T_ImgGenLocalResolutionSmoke_20260521` | PASS: imported `/Game/GeneratedImages/T_ImgGenLocalResolutionSmoke_20260521`, width `128`, height `64`, saved `true`. |
| Explicit destination | `imagegen.generate_image` with `resolution={width=96,height=96}` and `destination=/Game/GeneratedImages/Explicit/T_ImgGenExplicitPathSmoke_20260521` | PASS: imported exactly to `/Game/GeneratedImages/Explicit/T_ImgGenExplicitPathSmoke_20260521`, width `96`, height `96`, saved `true`. |
| ima2 bridge no reference | `imagegen.generate_image_via_ima2` with default model omitted, `resolution=1024x1024`, asset name `T_ImgGenIma2NoRefSmoke_20260521` | PASS: called `http://192.168.0.10:3333/api/generate` through `provider=oauth`, imported a 1024x1024 PNG under `/Game/GeneratedImages`, provenance model `gpt-5.5`, `reference_count=0`. |
| ima2 bridge with reference path | `imagegen.generate_image_via_ima2` with `reference_image_paths=["D:\P4\game\Saved\MonolithImageGenTests\reference-red-blue.png"]`, asset name `T_ImgGenIma2ReferenceSmoke_20260521_Retry` | PASS: archived `D:/P4/game/GeneratedImages/Ref_20260521_044809_00_a1a0ddcf.png`, sent one reference to ima2, imported 1024x1024 PNG under `/Game/GeneratedImages`, provenance `reference_count=1`, `reference_hashes=a1a0ddcf61f2f8aba704eb69e57ace60`. |
| Provenance readback | `imagegen.get_generated_asset_provenance` for both ima2 smoke assets | PASS: found both assets and returned redacted prompt hashes, bridge server URL, request IDs, model `gpt-5.5`, size, quality, moderation, and reference metadata. |

Note: the first reference-path smoke attempt closed the MCP connection while the editor restarted, but the same server reference payload succeeded through a direct `/api/generate` request and the Monolith reference-path retry passed. No reproducible reference-path failure remained after the retry.

## 5. Follow-up Verification -- OpenAI Background API Options

| Check | Command / source | Result |
|-------|------------------|--------|
| OpenAI option contract | Official OpenAI image generation guide and local `openai` SDK `responses.d.ts` | PASS: Responses `image_generation` tools expose `background=transparent|opaque|auto` and `output_format=png|webp|jpeg`. |
| ima2-gen typecheck | `npm run typecheck` in `D:\P4\imag2-gen` | PASS. |
| ima2-gen provider parity tests | `node --import tsx --test tests/api-provider-parity.test.ts tests/generate-route-validation-error.test.ts tests/cli-feature-parity-contract.test.js` | PASS: API provider forwards `background=transparent` and `output_format=png`; CLI exposes `--background`; `background=transparent` with JPEG is rejected before upstream. |
| Monolith build | `UnrealBuildTool.exe GoGameEditor Win64 Development -Project="D:\P4\game\GO.uproject" -WaitMutex -NoHotReloadFromIDE` | PASS: `Result: Succeeded`. |
| Monolith request validation | Source review of `imagegen.generate_image_via_ima2` implementation | PASS: optional `background` defaults to `auto`, accepts `transparent`, `opaque`, and `auto`, forwards the value to ima2-gen, records provenance, and rejects `transparent` unless `format` is PNG or WebP. |

## 6. Follow-up Verification -- Texture Role Import Pipeline

| Check | Command / source | Result |
|-------|------------------|--------|
| Texture role compile | `UnrealBuildTool.exe GoGameEditor Win64 Development -Project="D:\P4\game\GO.uproject" -WaitMutex -NoHotReloadFromIDE` | PASS: compiled `MonolithAssetTextureIngestActions.cpp`, `MonolithAssetInspectionActions.cpp`, `ImportTextureFromBytesTests.cpp`, and `MonolithImageGenActions.cpp`; final build returned `Result: Succeeded` after checking out Perforce-managed `UnrealEditor.modules`. |
| Texture role automation | `UnrealEditor-Cmd.exe D:\P4\game\GO.uproject -NullRHI -NoSplash -Unattended -NoSound -ExecCmds="Automation RunTests MonolithAsset.ImportTextureFromBytes.TextureRoleNormal; Quit" -TestExit="Automation Test Queue Empty"` | PASS: `MonolithAsset.ImportTextureFromBytes.TextureRoleNormal` completed with `Result={Success}`. |
| Import action contract | Source review of `asset.import_texture_from_bytes` | PASS: optional `texture_role` supports `ui_icon`, `sprite`, `decal`, `basecolor`, `world_tile`, `normal`, `orm_mask`, `height`, and `emissive`; role presets apply before explicit `settings` overrides. |
| Post-processing | Source review of `asset.import_texture_from_bytes` | PASS: UI/sprite/decal roles apply transparent-pixel RGB alpha bleed; world/material roles apply wrap/clamp addressing and mip/LOD/compression settings appropriate to the role. |
| Validation metadata | Source review of `asset.import_texture_from_bytes` | PASS: result returns `texture_role`, `settings_applied`, and non-blocking `validation` with alpha stats, channel stats, tile seam warnings, normal plausibility warnings, mask dynamic-range warnings, and power-of-two warnings. |
| ImageGen integration | Source review of `imagegen.generate_image`, `imagegen.generate_image_via_ima2`, and `imagegen.import_generated_image` | PASS: generated imports default to `texture_role=basecolor`; callers can override `texture_role`; provenance stores the imported role. |
| Asset inspection integration | Source review of `asset.inspect_asset` and `asset.validate_specialized_asset` | PASS: `Texture2D` is now a specialized asset enricher and reports import settings plus generated texture-role metadata/warnings. |
