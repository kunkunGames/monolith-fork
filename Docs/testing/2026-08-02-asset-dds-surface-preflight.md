# MonolithAsset DDS Surface Preflight Verification

**Date:** 2026-08-02
**Branch:** `jules/codex/asset/dds-surface-preflight`
**Verified source head:** `f586dc89310b6143999211c61234b7cd5b9e463d`
**Fork baseline:** AudioRuntime package fix content through `eac23d1c80819815a5bbb4e4d35047e963e09b09` (merged by PR #19 as `fffae542e07830493d87ebe97055d1e4d76ed02b`)
**Engine floor:** Unreal Engine 5.7
**Additional engine:** Unreal Engine 5.8
**Status:** VERIFIED

---

## 1. Purpose

Verify that `asset.import_texture_from_bytes` explicitly rejects DDS arrays, cubemaps, and volume textures before `IImageWrapper` can allocate a decoded pixel buffer. The action creates one `UTexture2D`, so accepting more than one DDS surface would violate both the allocation bound and the output contract.

## 2. Unreal Contract Review

UE 5.7 and UE 5.8 expose the same public `UE::DDS::FDDSFile::CreateFromDDSInMemory` API with `EDDSReadMipMode::HeaderOnly`. Both versions expose `IsValidTexture2D`, and their `FDdsImageWrapper::SetCompressed` implementations currently reject complex DDS resources. Monolith now performs that validation explicitly instead of relying on a private wrapper implementation detail.

| Contract | Required behavior |
|---|---|
| DDS metadata read | Header-only; no mip payload or decoded BGRA allocation |
| Accepted shape | UE-valid `Texture2D` resource with exactly one array surface; UE represents a 1D DDS as a height-one `UTexture2D` |
| Rejected shapes | Texture arrays, cubemaps/cubemap arrays, and volume textures |
| Failure | `-32602` before `IImageWrapper::SetCompressed`, package creation, or replacement mutation |

## 3. Verification Gates

| Gate | Expected result | Status |
|---|---|---|
| UE 5.7 BuildPlugin | Current source head compiles and links | PASS — UAT exit 0 and `BUILD SUCCESSFUL`; Editor 531/531, Development game 5/5, Shipping game 5/5. Package: `D:\P4\MonolithDdsRebasedUE57Package`. |
| UE 5.8 BuildPlugin | Current source head compiles and links | PASS — UAT exit 0 and `BUILD SUCCESSFUL`; Editor 531/531, Development game 5/5, Shipping game 5/5. Package: `D:\P4\MonolithDdsRebasedUE58Package`. |
| `MonolithAsset.ImportTextureFromBytes.DdsSurfacePreflight` | A one-surface DDS passes metadata validation; a two-slice DX10 DDS is rejected with explicit evidence and no asset | PASS on UE 5.7 and UE 5.8; each run found 1 test and completed with `Result={Success}`. Reports: `D:\P4\MonolithDdsRebasedUE57AutomationHost\Saved\AutomationReports\DdsSurfacePreflight` and `D:\P4\MonolithDdsRebasedUE58AutomationHost\Saved\AutomationReports\DdsSurfacePreflight`. |
| Full `MonolithAsset.ImportTextureFromBytes` filter | All byte-ingest regressions pass | PASS on UE 5.7 and UE 5.8: 14/14 tests completed with `Result={Success}` in each engine. Reports: `D:\P4\MonolithDdsRebasedUE57AutomationHost\Saved\AutomationReports\ImportTextureFromBytes` and `D:\P4\MonolithDdsRebasedUE58AutomationHost\Saved\AutomationReports\ImportTextureFromBytes`. |
| `git diff --check` | No whitespace errors | PASS after verification documentation finalization. |

### 3.1 Editor module artifact identities

| Engine | Artifact | Size | SHA-256 |
|---|---|---:|---|
| UE 5.7 | `D:\P4\MonolithDdsRebasedUE57Package\Binaries\Win64\UnrealEditor-MonolithAsset.dll` | 1,635,840 bytes | `FB97E2F43EE2E4D972E732D43FDC3441709C040D1845DB20CAE1524C967132C5` |
| UE 5.8 | `D:\P4\MonolithDdsRebasedUE58Package\Binaries\Win64\UnrealEditor-MonolithAsset.dll` | 1,543,168 bytes | `DBAA57D870C70C940BEAD3DDFA9B73C4C90681FC5F5CBF3169E8A40EE4788444` |

## 4. Visual and Discord Evidence

Screenshot verification is **N/A**. The change affects a headless asset-ingest validation path and has no gameplay, runtime UI, editor UI, VFX, animation, material, or visual-presentation effect.

Discord screenshot upload is **N/A**. No `Build\BatchFiles\Script\UploadScreenshotTestsToDiscord.bat` command is applicable because there is no meaningful PC 1920x1080 visual artifact for this API-only change.

## 5. Result

The DDS surface-shape contract is verified on the supported UE 5.7 floor and the UE 5.8 ceiling. Complex DDS resources fail as invalid parameters before wrapper decode or asset mutation, while the complete byte-ingest regression filter remains green in both engines.
