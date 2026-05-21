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
| Post-processing | Source review of `asset.import_texture_from_bytes` | PASS: UI/sprite/decal roles apply edge-background alpha extraction for opaque-background generated images plus transparent-pixel RGB alpha bleed; world_tile applies opposite-edge seam harmonization before validation; world/material roles apply wrap/clamp addressing and mip/LOD/compression settings appropriate to the role. |
| Validation metadata | Source review of `asset.import_texture_from_bytes` | PASS: result returns `texture_role`, `settings_applied`, and non-blocking `validation` with alpha stats, channel stats, postprocess counts, tile seam warnings, normal plausibility warnings, mask dynamic-range warnings, and power-of-two warnings. |
| ImageGen integration | Source review of `imagegen.generate_image`, `imagegen.generate_image_via_ima2`, and `imagegen.import_generated_image` | PASS: generated imports default to `texture_role=basecolor`; callers can override `texture_role`; generated imports mirror the postprocessed imported PNG under `<ProjectDir>/GeneratedImages`; provenance stores the imported role plus `source_png_kind=postprocessed` and source PNG metadata. |
| Asset inspection integration | Source review of `asset.inspect_asset` and `asset.validate_specialized_asset` | PASS: `Texture2D` is now a specialized asset enricher and reports import settings plus generated texture-role metadata/warnings. |

## 7. Follow-up Verification -- Texture Role Automation Matrix

| Check | Command / source | Result |
|-------|------------------|--------|
| Full editor target probe | `UnrealBuildTool.exe GoGameEditor Win64 Development -Project="D:\P4\game\GO.uproject" -WaitMutex -NoHotReloadFromIDE` | BLOCKED by unrelated current game-code errors in `Source/GoCore/Private/Pool/GoPoolable.cpp`, `Source/GoGame/Private/Character/GoSkillComponent.cpp`, and `Source/GoCore/Private/Pool/GoActorPoolSubsystem.cpp`; Monolith test files compiled before the target failed. Monolith DLL linking also required checking out read-only Perforce-managed plugin DLLs. |
| Monolith module build | `UnrealBuildTool.exe UnrealEditor Win64 Development -Project="D:\P4\game\GO.uproject" -Module=MonolithAsset -Module=MonolithImageGen -WaitMutex -NoHotReloadFromIDE` | PASS: compiled `MonolithAssetTextureIngestActions.cpp`, `ImportTextureFromBytesTests.cpp`, `MonolithImageGenActions.cpp`, `ImageGenTextureRoleTests.cpp`, `Module.MonolithAsset.cpp`, and `Module.MonolithImageGen.cpp`; linked `UnrealEditor-MonolithAsset.dll` and `UnrealEditor-MonolithImageGen.dll`. |
| MonolithImageGen rebuild after test expectation fix | `UnrealBuildTool.exe UnrealEditor Win64 Development -Project="D:\P4\game\GO.uproject" -Module=MonolithImageGen -WaitMutex -NoHotReloadFromIDE` | PASS: rebuilt and linked the updated ImageGen texture-role tests. |
| Asset role automation | `UnrealEditor-Cmd.exe D:\P4\game\GO.uproject -NullRHI -NoSplash -Unattended -NoSound -ExecCmds="Automation RunTests MonolithAsset.ImportTextureFromBytes; Quit" -TestExit="Automation Test Queue Empty"` | PASS: 3/3 tests succeeded: `BasicPNG`, `TextureRoleNormal`, and `TextureRolePresetMatrix`. Matrix covers all nine roles, transparent-PNG alpha bleed, opaque-background alpha extraction, processed PNG return, and world_tile seam harmonization. |
| ImageGen role automation | `UnrealEditor-Cmd.exe D:\P4\game\GO.uproject -NullRHI -NoSplash -Unattended -NoSound -ExecCmds="Automation RunTests MonolithImageGen.TextureRoles; Quit" -TestExit="Automation Test Queue Empty"` | PASS: 3/3 tests succeeded: `Defaults`, `GenerateLocalForwardsRole`, and `ImportGeneratedImageForwardsRole`. The import test covers `import_generated_image(file_path=...)` provenance source `external_file` and `save_source_png` writing a postprocessed PNG mirror with alpha. |

## 8. Follow-up Verification -- Texture Role Regeneration Remediation

| Check | Command / source | Result |
|-------|------------------|--------|
| Alpha roles transparent request | `imagegen.generate_image_via_ima2` with `background=transparent`, then `background=auto` for `ui_icon`, `sprite`, and `decal` | `background=transparent` was rejected by the provider. `background=auto` generated images, but Monolith validation reported `has_alpha=false` for all three roles. |
| Alpha postprocess | Edge-background alpha extraction on `D:\P4\game\GeneratedImages\{ui_icon,sprite,decal}\*_Fix_*.png` | PASS: produced `*_FixAlpha_20260521_1806.png`; source metrics show non-opaque pixels for all three: ui_icon `803785`, sprite `720298`, decal `735984`. |
| World tile regenerate | `imagegen.generate_image_via_ima2` with seamless/tileable PBR prompt for `world_tile` | FAIL: generated `/Game/GeneratedImages/world_tile/T_TextureRole_world_tile_Fix_20260521_1757` still returned `tile_edge_mismatch`, edge average delta `8.0703` in Monolith validation. |
| World tile seam postprocess | Edge-pair seam harmonization on source PNG | PASS: produced `T_TextureRole_world_tile_FixSeam_20260521_1806.png`; local edge metrics improved from `edgeAvgDelta=10.6138`, `edgeMaxDelta=39` to `edgeAvgDelta=0.0000`, `edgeMaxDelta=0`. |
| Remote regeneration for data/emissive roles | `imagegen.generate_image_via_ima2` for `normal`, `orm_mask`, `height`, and `emissive` | PASS: all four remote generations completed without local fallback. Monolith validation passed with expected role settings: normal `TC_Normalmap/sRGB=false`, ORM `TC_Masks/sRGB=false`, height `TC_Grayscale/sRGB=false`, emissive `TEXTUREGROUP_Effects/sRGB=true`. |
| Postprocessed asset import | `asset.import_texture_from_file` for `*_FixAlpha_20260521_1806.png` and `*_FixSeam_20260521_1806.png` | PASS with caveat for the manual import path: imported and saved assets under `/Game/GeneratedImages/...`; `asset.inspect_asset` reports source dimensions `1024x1024` and `validate_specialized_asset` reports no warnings. Runtime `GetSizeX/GetSizeY` returned `0` in that editor session, while source metadata reports valid `1024x1024`. |
| MonolithImageGen postprocessed mirror build | `UnrealBuildTool.exe UnrealEditor Win64 Development -Project="D:\P4\game\GO.uproject" -Module=MonolithAsset -Module=MonolithImageGen -WaitMutex -NoHotReloadFromIDE` | PASS: rebuilt and linked `UnrealEditor-MonolithAsset.dll` and `UnrealEditor-MonolithImageGen.dll` after adding processed PNG return, edge-background alpha extraction, world_tile seam harmonization, and `source_png_kind=postprocessed` provenance. |
| MonolithImageGen postprocessed mirror automation | `Automation RunTests MonolithAsset.ImportTextureFromBytes`; `Automation RunTests MonolithImageGen.TextureRoles` | PASS: 6/6 targeted tests succeeded. Asset tests cover processed PNG return, alpha extraction, alpha bleed, and seam harmonization; ImageGen tests cover `save_source_png` writing a postprocessed alpha PNG mirror. |

Notes: commandlet startup still logs existing PaperZD member-initialization errors, but the targeted Monolith automation tests completed successfully. The first `MonolithImageGen.TextureRoles.Defaults` run failed because the test expected role presets as nested objects while the action returns a string map; the test expectation was corrected and the rerun passed.

## 9. Follow-up Verification -- Reference Asset and PNG Inputs

| Check | Command / source | Result |
|-------|------------------|--------|
| MonolithImageGen module build | `UnrealBuildTool.exe UnrealEditor Win64 Development -Project="D:\P4\game\GO.uproject" -Module=MonolithImageGen -WaitMutex -NoHotReloadFromIDE` | PASS: compiled `MonolithImageGenActions.cpp`, `ImageGenTextureRoleTests.cpp`, and dependent MonolithAsset test/import files; linked `UnrealEditor-MonolithImageGen.dll`. |
| ImageGen reference input automation | `UnrealEditor-Cmd.exe D:\P4\game\GO.uproject -NullRHI -NoSplash -Unattended -NoSound -ExecCmds="Automation RunTests MonolithImageGen.TextureRoles; Quit" -TestExit="Automation Test Queue Empty"` | PASS: 4/4 tests succeeded, including `MonolithImageGen.TextureRoles.ReferenceInputsArchive`. The new test covers `reference_png_paths` and `reference_asset_paths` archiving PNG references before the bridge call. |
| Action contract review | Source review of `imagegen.generate_image_via_ima2` | PASS: `reference_png_paths` is accepted as an explicit local-PNG path alias, and `reference_asset_paths` loads Texture2D Source art from package/object paths, extracts the top mip as PNG for TSF_BGRA8/TSF_G8/TSF_G16, archives it under `<ProjectDir>/GeneratedImages`, and forwards the PNG base64 with the prompt. |
| Live reference asset smoke | `imagegen.generate_image_via_ima2` with `reference_asset_paths=["/Game/Design/PC/Texture/T_Sprite_00"]`, `reference_png_paths=["D:\P4\game\GeneratedImages\Ref_20260521_044809_00_a1a0ddcf.png"]`, `texture_role=sprite`, and `destination=/Game/GeneratedImages/reference_asset/T_Sprite_00_RefAssetSmoke_20260521` | PASS: archived two references, including extracted `/Game/Design/PC/Texture/T_Sprite_00.T_Sprite_00` Source PNG `D:/P4/game/GeneratedImages/Ref_20260521_113214_01_d5590ac5.png`; generated and saved `/Game/GeneratedImages/reference_asset/T_Sprite_00_RefAssetSmoke_20260521`; wrote postprocessed mirror `D:/P4/game/GeneratedImages/reference_asset/T_Sprite_00_RefAssetSmoke_20260521.png`; validation passed for `sprite` with alpha. |

## 10. Follow-up Verification -- Edge-Connected Alpha Reprocess

| Check | Command / source | Result |
|-------|------------------|--------|
| MonolithAsset module build | `UnrealBuildTool.exe GoGameEditor Win64 Development -Project="D:\P4\game\GO.uproject" -Module=MonolithAsset -WaitMutex -NoHotReloadFromIDE` | PASS: rebuilt and linked `UnrealEditor-MonolithAsset.dll` after changing sprite/UI/decal alpha extraction to flood-fill only edge-connected background candidates. A full editor target build was blocked by unrelated read-only plugin binaries, so the build was narrowed to the changed module. |
| Asset role automation | `Automation RunTests MonolithAsset.ImportTextureFromBytes.TextureRolePresetMatrix` | PASS: targeted role matrix succeeded; the opaque-background fixture now verifies that an enclosed interior pixel matching the edge background remains opaque. |
| HQ warrior sprite reprocess | `imagegen.import_generated_image` using `T_Sprite_00_HQWarriorSpriteSheet_20260521_whitefilled_input.png`, `texture_role=sprite`, and destination `/Game/GeneratedImages/reference_asset/T_Sprite_00_HQWarriorSpriteSheet_20260521` | PASS: regenerated the same asset and postprocessed mirror through MonolithImageGen; validation passed with `has_alpha=true`, `alpha_coverage=0.3765`, and no warnings. Checker preview confirmed the character head/hair color remains filled while only the connected white background is transparent. |
