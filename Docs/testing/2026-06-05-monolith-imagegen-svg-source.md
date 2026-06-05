# MonolithImageGen SVG Source and MSDF Texture Verification

**Date:** 2026-06-05
**Editor:** Unreal Engine 5.7, Go project
**Module:** `MonolithImageGen`
**Scope:** `imagegen.generate_svg`, `imagegen.import_generated_svg`, `imagegen.validate_svg`, `imagegen.generate_msdf_from_svg`

## Purpose

Verify the SVG source pipeline and the initial MSDF Texture2D pipeline: action registration, bounded SVG sanitizer, MSDF-oriented geometry checks, deterministic local SVG generation, generated SVG import round trip, prompt-redacted sidecars, MSDF PNG/Texture2D creation, pixel/channel sample validation, optional MSDF material render preview, and no regression in existing generated PNG Texture2D role handling.

## Commands

```powershell
$engineRoot = powershell -NoProfile -ExecutionPolicy Bypass -File "D:\P4\game\BatchFiles\Script\ResolveUnrealEngine.ps1" -Project "D:\P4\game\GO.uproject" -Output Root
& "$engineRoot\Engine\Binaries\DotNET\UnrealBuildTool\UnrealBuildTool.exe" GoGameEditor Win64 Development -Project="D:\P4\game\GO.uproject" -WaitMutex -NoHotReloadFromIDE
```

```powershell
$engineRoot = powershell -NoProfile -ExecutionPolicy Bypass -File "D:\P4\game\BatchFiles\Script\ResolveUnrealEngine.ps1" -Project "D:\P4\game\GO.uproject" -Output Root
& "$engineRoot\Engine\Binaries\Win64\UnrealEditor-Cmd.exe" "D:\P4\game\GO.uproject" -NullRHI -Unattended -NoSplash -NoSound -ExecCmds="Automation RunTests MonolithImageGen.SvgSource; Quit" -TestExit="Automation Test Queue Empty" -ReportExportPath="D:\P4\game\Saved\Automation\MonolithImageGenSvgSource_20260605" -log
```

```powershell
$engineRoot = powershell -NoProfile -ExecutionPolicy Bypass -File "D:\P4\game\BatchFiles\Script\ResolveUnrealEngine.ps1" -Project "D:\P4\game\GO.uproject" -Output Root
& "$engineRoot\Engine\Binaries\Win64\UnrealEditor-Cmd.exe" "D:\P4\game\GO.uproject" -NullRHI -Unattended -NoSplash -NoSound -ExecCmds="Automation RunTests MonolithImageGen.TextureRoles; Quit" -TestExit="Automation Test Queue Empty" -ReportExportPath="D:\P4\game\Saved\Automation\MonolithImageGenTextureRoles_20260605_SvgSource" -log
```

```powershell
$engineRoot = powershell -NoProfile -ExecutionPolicy Bypass -File "D:\P4\game\BatchFiles\Script\ResolveUnrealEngine.ps1" -Project "D:\P4\game\GO.uproject" -Output Root
& "$engineRoot\Engine\Binaries\Win64\UnrealEditor-Cmd.exe" "D:\P4\game\GO.uproject" -Unattended -NoSplash -NoSound -ExecCmds="Automation RunTests MonolithImageGen.SvgSource; Quit" -TestExit="Automation Test Queue Empty" -ReportExportPath="D:\P4\game\Saved\Automation\MonolithImageGenSvgSourceMsdf_20260605" -log
```

```powershell
$engineRoot = powershell -NoProfile -ExecutionPolicy Bypass -File "D:\P4\game\BatchFiles\Script\ResolveUnrealEngine.ps1" -Project "D:\P4\game\GO.uproject" -Output Root
& "$engineRoot\Engine\Binaries\Win64\UnrealEditor-Cmd.exe" "D:\P4\game\GO.uproject" -run=MonolithReindex -mode=project -unattended -nopause -nosplash -nullrhi
Plugins\Monolith\Binaries\monolith_query.exe source search_source HandleGenerateSvg --limit=3
Plugins\Monolith\Binaries\monolith_query.exe source search_source HandleGenerateMsdfFromSvg --limit=5
Plugins\Monolith\Binaries\monolith_query.exe source repair_crg_cache --execute=true
Plugins\Monolith\Binaries\monolith_query.exe source health --include-counts=false
```

## Results

| Gate | Result | Evidence |
|---|---|---|
| Build | PASS | UBT `GoGameEditor Win64 Development` completed successfully after SVG and MSDF action registration. Existing project/plugin warnings were non-blocking. |
| SVG/MSDF automation | PASS, 9/9 | `Saved/Automation/MonolithImageGenSvgSourceMsdf_20260605/index.json`: `succeeded=9`, `succeededWithWarnings=0`, `failed=0`, `reportCreatedOn=2026.06.05-14.14.41`. |
| Action registration/defaults | PASS | `MonolithImageGen.SvgSource.DefaultsAndRegistration` verifies the SVG/MSDF actions are registered and defaults/model lists expose SVG source and `png_texture2d_msdf` entries. |
| Sanitizer security | PASS | `MonolithImageGen.SvgSource.SanitizerSecurity` rejects script, event attributes, `foreignObject`, DTD/entity input, and external image references. |
| MSDF geometry | PASS | `MonolithImageGen.SvgSource.MsdfGeometryValidation` accepts a simple closed path and blocks bow-tie self-intersection, open contours, duplicate adjacent points, wrong hole winding, overlap, unflattened transform, and invalid path grammar. |
| Determinism/import/profile | PASS | Tests cover prompt-only deterministic generation, import/write/readback sidecar round trip without raw prompt persistence, and web/editor SVG profile differences versus `msdf_source`. |
| MSDF Texture2D creation | PASS | `MonolithImageGen.SvgSource.GenerateMsdfTexture` creates a 64x64 MSDF PNG/Texture2D, verifies the saved source PNG mirror/hash, and checks `TC_Masks`, `sRGB=false`, `TMGS_NoMipmaps`, `TEXTUREGROUP_UI`, clamp addressing, `NeverStream=true`, and `MaxTextureSize=64`. |
| MSDF pixel/channel sampling | PASS | `GenerateMsdfTexture` samples center, outside-corner, and edge-mid pixels; `verify_samples=true` requires expected median behavior and channel spread so non-MSDF flat output cannot pass. |
| MSDF invalid-source rejection | PASS | `MonolithImageGen.SvgSource.GenerateMsdfRejectsInvalidSource` proves non-`msdf_ready` SVG is rejected before Texture2D creation. |
| MSDF material render | PASS | `MonolithImageGen.SvgSource.GenerateMsdfMaterialRender` creates an unlit masked MSDF material, renders a preview through the material namespace, decodes the preview PNG, and verifies it is non-empty and non-uniform. |
| Texture role regression | PASS, 5/5 with expected warning | `Saved/Automation/MonolithImageGenTextureRoles_20260605_SvgSource/index.json`: `succeeded=4`, `succeededWithWarnings=1`, `failed=0`. The warning is the existing `ReferenceInputsArchive` timeout against `http://127.0.0.1:9/api/generate`. |
| Source index freshness | PASS with pre-existing warning | `MonolithReindex -mode=project` completed with `errors=0`; source searches find `HandleGenerateSvg` and `HandleGenerateMsdfFromSvg` in `MonolithImageGenSvgSourceActions`. CRG parity was repaired with `source repair_crg_cache --execute=true`; final source health has only the pre-existing orphan reference warning. |

## Retest

| Date | Gate | Result | Evidence |
|---|---|---|---|
| 2026-06-05 | SVG automation rerun | PASS, 6/6 | `Saved/Automation/MonolithImageGenSvgSource_20260605_Retest/index.json`: `succeeded=6`, `succeededWithWarnings=0`, `failed=0`. |
| 2026-06-05 | Full editor rebuild rerun after MSDF work | PASS | UBT rebuilt `UnrealEditor-MonolithImageGen.dll`; the earlier unrelated `MonolithEditor` link blocker did not reproduce in the final build. |
| 2026-06-05 | D3D12 material-render automation | PASS, 9/9 | `Saved/Automation/MonolithImageGenSvgSourceMsdf_20260605/index.json`: `succeeded=9`, `succeededWithWarnings=0`, `failed=0`. |

## Notes

`generate_svg` and `import_generated_svg` deliberately write SVG source and `.monolith.json` provenance sidecars only. `generate_msdf_from_svg` is the explicit conversion boundary: it validates the `msdf_source` SVG, bakes a precomputed MSDF PNG/Texture2D, applies MSDF-safe texture settings, and can create/render a material preview. Gameplay/runtime code still does not parse or render SVG.
