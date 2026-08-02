# MonolithAsset DDS Surface Preflight Verification

**Date:** 2026-08-02
**Branch:** `jules/codex/asset/dds-surface-preflight`
**Fork base:** `kunkunGames/monolith-fork@41329470a1fe07337ca45fc63b2f129371bbc0d6`
**Engine floor:** Unreal Engine 5.7
**Additional engine:** Unreal Engine 5.8
**Status:** PENDING VERIFICATION

---

## 1. Purpose

Verify that `asset.import_texture_from_bytes` explicitly rejects DDS arrays, cubemaps, and volume textures before `IImageWrapper` can allocate a decoded pixel buffer. The action creates one `UTexture2D`, so accepting more than one DDS surface would violate both the allocation bound and the output contract.

## 2. Unreal Contract Review

UE 5.7 and UE 5.8 expose the same public `UE::DDS::FDDSFile::CreateFromDDSInMemory` API with `EDDSReadMipMode::HeaderOnly`. Both versions expose `IsValidTexture2D`, and their `FDdsImageWrapper::SetCompressed` implementations currently reject complex DDS resources. Monolith now performs that validation explicitly instead of relying on a private wrapper implementation detail.

| Contract | Required behavior |
|---|---|
| DDS metadata read | Header-only; no mip payload or decoded BGRA allocation |
| Accepted shape | Dimension 2, depth 1, and exactly one array surface |
| Rejected shapes | Texture arrays, cubemaps/cubemap arrays, and volume textures |
| Failure | `-32602` before `IImageWrapper::SetCompressed`, package creation, or replacement mutation |

## 3. Verification Gates

| Gate | Expected result | Status |
|---|---|---|
| UE 5.7 BuildPlugin | Current branch compiles and links | Pending |
| UE 5.8 BuildPlugin | Current branch compiles and links | Pending |
| `MonolithAsset.ImportTextureFromBytes.DdsSurfacePreflight` | A one-surface DDS passes metadata validation; a two-slice DX10 DDS is rejected with explicit evidence and no asset | Pending |
| Full `MonolithAsset.ImportTextureFromBytes` filter | All byte-ingest regressions pass | Pending |
| `git diff --check` | No whitespace errors | Pending |

## 4. Visual and Discord Evidence

Screenshot verification is **N/A**. The change affects a headless asset-ingest validation path and has no gameplay, runtime UI, editor UI, VFX, animation, material, or visual-presentation effect.

Discord screenshot upload is **N/A**. No `Build\BatchFiles\Script\UploadScreenshotTestsToDiscord.bat` command is applicable because there is no meaningful PC 1920x1080 visual artifact for this API-only change.

## 5. Result

Pending current-byte UE 5.7 and UE 5.8 build and automation evidence.
