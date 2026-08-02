# Monolith Interchange Portable Path Identity Verification

**Date:** 2026-08-02 (KST)
**Branch:** `jules/codex/interchange/portable-path-identity`
**Verified source head:** `b88abacc9f0ff91eb199da2319cba923897eea73`
**Fork baseline:** AudioRuntime package fix content through `eac23d1c80819815a5bbb4e4d35047e963e09b09` (merged by PR #19 as `fffae542e07830493d87ebe97055d1e4d76ed02b`)
**Engine floor:** Unreal Engine 5.7
**Engine ceiling tested:** Unreal Engine 5.8
**Status:** VERIFIED

---

## 1. Purpose

Verify that `interchange.export_asset` treats output paths that differ only by letter case as one portable filesystem identity. Output names are resolved before the destination exists, so the action cannot safely probe the destination volume's case behavior; an explicit portable contract prevents a set accepted on a case-sensitive host from aliasing on default case-insensitive macOS or Windows storage.

## 2. Implemented Contract

| Stage | Required behavior |
|---|---|
| Exporter output planning | Normalize to absolute slash form and case-fold before duplicate checks |
| Staging declaration and scan | Use the same identity for expected and observed output paths |
| Commit preflight | Reject case-only staged or destination aliases before any move |
| Cross-platform policy | Case-only distinct outputs are intentionally unsupported on every platform |

One shared `MonolithInterchangePortablePathKey` implementation owns this identity so planning and commit cannot drift apart.

## 3. Verification Gates

| Gate | Expected result | Status |
|---|---|---|
| UE 5.7 BuildPlugin | Current source head compiles and links | PASS — UAT exit 0 and `BUILD SUCCESSFUL`; Editor 531/531, Development game 5/5, Shipping game 5/5. Package: `D:\P4\MonolithPortablePathsRebasedUE57Package`. |
| UE 5.8 BuildPlugin | Current source head compiles and links | PASS — UAT exit 0 and `BUILD SUCCESSFUL`; Editor 531/531, Development game 5/5, Shipping game 5/5. Package: `D:\P4\MonolithPortablePathsRebasedUE58Package`. |
| `Monolith.Interchange.ExportTransaction` | Case-only destination names are rejected before the injected move callback; existing transaction/rollback tests pass | PASS on UE 5.7 and UE 5.8; each run found 1 test and completed with `Result={Success}`. Reports: `D:\P4\MonolithPortablePathsRebasedUE57AutomationHost\Saved\AutomationReports\ExportTransaction` and `D:\P4\MonolithPortablePathsRebasedUE58AutomationHost\Saved\AutomationReports\ExportTransaction`. |
| Interchange parameter/export guard | Confirmed native export still succeeds through staging | PASS on UE 5.7 and UE 5.8. `Monolith.ParamGuard.MonolithInterchange.ImportRejectsMalformedParams` completed with `Result={Success}` in each engine, including its transactional native texture-export replacement assertions. |
| `git diff --check` | No whitespace errors | PASS after verification documentation finalization. |

### 3.1 Editor module artifact identities

| Engine | Artifact | Size | SHA-256 |
|---|---|---:|---|
| UE 5.7 | `D:\P4\MonolithPortablePathsRebasedUE57Package\Binaries\Win64\UnrealEditor-MonolithInterchange.dll` | 441,344 bytes | `1085679B8CD2A6C63B62A4AD63DC5BE461A5B8A6E2BC121C6EB330167E97CC20` |
| UE 5.8 | `D:\P4\MonolithPortablePathsRebasedUE58Package\Binaries\Win64\UnrealEditor-MonolithInterchange.dll` | 416,768 bytes | `7FFECCA08ECEB740755F93AFC2406F19A530F3430410A007FDE959B26DC25E12` |

## 4. Visual and Discord Evidence

Screenshot verification is **N/A**. This change affects headless editor-side filesystem identity validation and has no gameplay, runtime UI, editor UI, VFX, animation, material, or visual-presentation effect.

Discord screenshot upload is **N/A**. No `Build\BatchFiles\Script\UploadScreenshotTestsToDiscord.bat` command is applicable because there is no meaningful PC 1920x1080 visual artifact for this API-only change.

## 5. Result

The portable identity contract is verified on UE 5.7 and UE 5.8. Case-only output aliases are rejected before filesystem mutation, existing replacement/rollback behavior stays green, and the production transactional export path continues to stage and commit a native texture export successfully.
