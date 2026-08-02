# Monolith Interchange Portable Path Identity Verification

**Date:** 2026-08-02 (KST)
**Branch:** `jules/codex/interchange/portable-path-identity`
**Base:** `kunkunGames/monolith-fork@41329470a1fe07337ca45fc63b2f129371bbc0d6`
**Engine floor:** Unreal Engine 5.7
**Engine ceiling tested:** Unreal Engine 5.8
**Status:** PENDING VERIFICATION

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
| UE 5.7 BuildPlugin | Current branch compiles and links | Pending |
| UE 5.8 BuildPlugin | Current branch compiles and links | Pending |
| `Monolith.Interchange.ExportTransaction` | Case-only destination names are rejected before the injected move callback; existing transaction/rollback tests pass | Pending |
| Interchange parameter/export guard | Confirmed native export still succeeds through staging | Pending |
| `git diff --check` | No whitespace errors | Pending |

## 4. Visual and Discord Evidence

Screenshot verification is **N/A**. This change affects headless editor-side filesystem identity validation and has no gameplay, runtime UI, editor UI, VFX, animation, material, or visual-presentation effect.

Discord screenshot upload is **N/A**. No `Build\BatchFiles\Script\UploadScreenshotTestsToDiscord.bat` command is applicable because there is no meaningful PC 1920x1080 visual artifact for this API-only change.

## 5. Result

Pending current-byte UE 5.7 and UE 5.8 build and automation evidence.
