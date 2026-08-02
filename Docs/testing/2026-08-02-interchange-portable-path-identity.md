# Monolith Interchange Portable Path Identity Verification

**Date:** 2026-08-02 (KST)
**Branch:** `jules/codex/interchange/portable-path-identity`
**Verified source head:** `7439bd3965c6effa13d416168394086d8f432004`
**Fork baseline:** AudioRuntime package fix content through `eac23d1c80819815a5bbb4e4d35047e963e09b09` (merged by PR #19 as `fffae542e07830493d87ebe97055d1e4d76ed02b`)
**Engine floor:** Unreal Engine 5.7
**Engine ceiling tested:** Unreal Engine 5.8
**Status:** VERIFIED

---

## 1. Purpose

Verify that `interchange.export_asset` treats output filenames that differ only by letter case as one portable filesystem identity without weakening requested-directory, staging-root, or ownership checks. Output names are resolved before the destination exists, so the action cannot safely probe the destination volume's case behavior; an explicit portable filename contract prevents a set accepted on a case-sensitive host from aliasing on default case-insensitive macOS or Windows storage.

## 2. Implemented Contract

| Stage | Required behavior |
|---|---|
| Exporter output planning | Keep Unicode directory names under host semantics; require an exporter-owned filename of at most 255 ASCII letters, digits, `.`, `_`, or `-`, with no trailing dot or Windows reserved device stem; lowercase that complete set before duplicate checks |
| Requested directory and staging ownership | Use the current host filesystem semantics, matching Unreal Engine 5.7 and 5.8 `FPaths::IsSamePath` / `FPaths::IsUnderDirectory` behavior instead of the portable alias key |
| Staging declaration and scan | Use one filename identity for expected and observed outputs; reject invalid names and consume each observed identity at most once so a second case alias is an explicit duplicate |
| Commit preflight | Reject case-only staged or destination aliases before any move |
| Cross-platform policy | Case-only distinct outputs and non-portable exporter filenames are intentionally unsupported on every platform; Unicode parent directories remain supported |

One shared `TryMonolithInterchangePortableFilenameKey` implementation owns portable filename validation and identity. `MonolithInterchangePathsMatchHostSemantics` separately owns directory and path equality so portable alias detection cannot expand an exporter's requested boundary.

## 3. Verification Gates

| Gate | Expected result | Status |
|---|---|---|
| UE 5.7 BuildPlugin | Current source head compiles and links as a release package | PASS — `MONOLITH_RELEASE_BUILD=1`, UAT exit 0 and `BUILD SUCCESSFUL`; full Win64 host plus Development and Shipping target packaging. Package: `D:\P4\MonolithPortablePathsAsciiUE57Package`. |
| UE 5.8 BuildPlugin | Current source head compiles and links as a release package | PASS — `MONOLITH_RELEASE_BUILD=1`, UAT exit 0 and `BUILD SUCCESSFUL`; full Win64 host plus Development and Shipping target packaging. Package: `D:\P4\MonolithPortablePathsAsciiUE58Package`. |
| Optional dependency isolation | Release package does not hard-import Blueprint Assist | PASS — `llvm-objdump -p` on both `UnrealEditor-MonolithBABridge.dll` files lists Core, CoreUObject, and MonolithCore imports and no `UnrealEditor-BlueprintAssist.dll`. |
| Unreal path semantics | Ownership checks match the supported engine implementation | PASS — direct UE 5.7 `Paths.cpp:1902-1933` and UE 5.8 `Paths.cpp:1900-1931` inspection confirms case-insensitive Microsoft and case-sensitive non-Microsoft comparisons for both APIs. |
| `Monolith.Interchange.ExportTransaction` | Portable ASCII case aliases, non-ASCII/trailing-dot/reserved/overlength rejection, host path semantics, and duplicate actual identities pass with the existing replacement/rollback suite | PASS on UE 5.7 and UE 5.8; each run found 1 test and completed with `Result={Success}`. Report SHA-256: UE 5.7 `D8BCD5A3D81252A16472E9C515D971106A277B5E768CDE42276D1E1256218CE6`; UE 5.8 `2D4C48ADDFEEA40D34C41401C0EB7A51A853140057203BFA5A0C26373929F461`. |
| Interchange parameter/export guard | Confirmed native export still succeeds through staging | PASS on UE 5.7 and UE 5.8. `Monolith.ParamGuard.MonolithInterchange.ImportRejectsMalformedParams` found 1 test and completed with `Result={Success}` in each engine, including its transactional native texture-export replacement assertions. Report SHA-256: UE 5.7 `FD6FE7F8FFEC1FF06E5116532FD3D2C3DC6C4E741C7FAA28DAEED507DBB4AD49`; UE 5.8 `EF03008D3B52B9EAB62386A70052B75CEA070DCC5AECC3FD30778F1591D15B80`. |
| `git diff --check` | No whitespace errors | PASS after verification documentation finalization. |

### 3.1 Editor module artifact identities

| Engine | Artifact | Size | SHA-256 |
|---|---|---:|---|
| UE 5.7 | `D:\P4\MonolithPortablePathsAsciiUE57Package\Binaries\Win64\UnrealEditor-MonolithInterchange.dll` | 453,632 bytes | `206A5D504675F823DD2E3B900E3395E28C1B2F128C865E3DBB276D458D1C95FA` |
| UE 5.8 | `D:\P4\MonolithPortablePathsAsciiUE58Package\Binaries\Win64\UnrealEditor-MonolithInterchange.dll` | 429,056 bytes | `6FF1E534CD01AA626D7FD92BAD1F7EE127678DA662F800E10626ADB19E1012CF` |

## 4. Visual and Discord Evidence

Screenshot verification is **N/A**. This change affects headless editor-side filesystem identity validation and has no gameplay, runtime UI, editor UI, VFX, animation, material, or visual-presentation effect.

Discord screenshot upload is **N/A**. No `Build\BatchFiles\Script\UploadScreenshotTestsToDiscord.bat` command is applicable because there is no meaningful PC 1920x1080 visual artifact for this API-only change.

## 5. Result

The portable identity contract is verified on UE 5.7 and UE 5.8. Case-only output aliases and incomplete Unicode/Windows filename identities are rejected before filesystem mutation, host-sensitive directory ownership remains intact, every observed staging identity is consumed at most once, existing replacement/rollback behavior stays green, and the production transactional export path continues to stage and commit a native texture export successfully.
