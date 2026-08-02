# Asset Reference Fix-up Strict Preflight Verification

**Date:** 2026-08-02
**Branch:** `jules/codex/asset/reference-fixup-exactness`
**Reviewed base head:** `07faaf0583a8190f5aa021c9c6cc22fe556427c5`
**Scope:** `asset.fixup_copied_references` strict blocker preflight, hard-reference type safety, mutation ordering, traversal cap, diagnostics, and regression coverage
**Engines:** Unreal Engine 5.7 and 5.8, resolved from installed associations matching the verification hosts

---

## 1. Goal

Prevent a confirmed `strict=true` reference fix-up from partially rewriting a
destination package before a later missing target or traversal blocker is
reported. Strict execution must inspect the complete bounded candidate set
without mutation, reject every blocker with structured diagnostics, and only
perform the second applying traversal after the preflight succeeds.

---

## 2. Engine Contract Review

Both supported engine versions expose the same reflected test surface through
`UPrimaryAssetLabel::ExplicitAssets`, an editable
`TArray<TSoftObjectPtr<UObject>>`. The regression fixture therefore exercises
the same `FSoftObjectProperty` and `FScriptArrayHelper` path used by production
assets rather than a test-only parser.

The UE 5.7 and UE 5.8 implementations of `FScopedTransaction::Cancel()` call
`GEditor->CancelTransaction()`. `UTransBuffer::Cancel()` removes the active
transaction record; it does not restore property values already written by the
caller. A transaction cancel is therefore not a substitute for a complete
non-mutating blocker preflight.

| Engine source | Verified contract |
|---|---|
| `Engine\Source\Editor\UnrealEd\Private\ScopedTransaction.cpp` | `Cancel()` delegates to the editor transaction buffer. |
| `Engine\Source\Editor\UnrealEd\Private\EditorTransaction.cpp` | `UTransBuffer::Cancel()` cancels/removes transaction bookkeeping without replaying object state. |
| `Engine\Source\Runtime\Engine\Classes\Engine\PrimaryAssetLabel.h` | `ExplicitAssets` is a reflected array of soft object references in both UE 5.7 and 5.8. |
| `Engine\Source\Runtime\Engine\Classes\Engine\Font.h` | `UFont::Textures` is a reflected array of hard `UTexture2D` references in both UE 5.7 and 5.8, providing a production reflected-property path for incompatible-target regression coverage. |

---

## 3. Finding and Resolution

| Finding | Before | Resolution | Regression proof |
|---|---|---|---|
| Late strict blocker left earlier edits applied | One traversal both validated and wrote references; the handler returned `preflight_failed` only after earlier candidates could already be changed | Confirmed strict runs the complete traversal with `dry_run=true` first, then runs the applying traversal only when no blocker exists | A valid first soft reference followed by a missing second target returns failure, preserves the first source path, and leaves the package clean |
| Traversal cap permitted partial strict mutation | `max_packages` set `truncated=true` but still applied the bounded prefix | Strict mutation treats truncation itself as a blocker and returns before visiting or changing a package | Two requested packages with `max_packages=1` return `truncated=true`, `applied_count=0`, and preserve the reference |
| A later non-blocking hard-target miss could clear an earlier blocker | Hard-object miss assigned the aggregate blocker flag | Aggregate with logical OR so an earlier blocker cannot be erased | Both supported engines compile and run the complete fixture |
| Incompatible hard target could pass strict preflight | Hard-target class mismatch copied `require_targets`; with `require_targets=false`, strict mode could report the mismatch but continue | A resolved incompatible hard target is unconditionally blocking in strict mode; `require_targets` applies only to absent targets | A real `UFont::Textures` `UTexture2D*` reference remapped to a `UCurveFloat` target returns `preflight_failed`, preserves the original pointer, and leaves the package clean |
| Package-load failure could erase an earlier blocker | The aggregate blocker flag was assigned instead of accumulated | Package-load failure now uses logical OR, preserving every prior blocker | The complete strict fixture succeeds on both supported engines without aggregate-state regression |
| Strict success path needed two-pass coverage | Existing tests only covered guards and a generic dry-run | The fixture removes the missing row, reruns the same confirmed strict action, and asserts one real rewrite plus package dirtying | `applied_count=1` and the reference points to the remapped destination object |

Structured strict failures now return `checked_packages`, reference and warning
arrays, checked object/package counts, `candidate_count`, `applied_count`,
`changed_package_count`, `truncated`, and `status="preflight_failed"`.

---

## 4. Current-byte Identity

Git blob object IDs identify the canonical committed source independently of
checkout line endings. The SHA-256 column is the exact Windows worktree byte
sequence copied into both the UE 5.7 and UE 5.8 verification packages.

| File | Git blob OID | Windows checkout and both packages SHA-256 |
|---|---|---|
| `Source\MonolithAsset\Private\MonolithAssetPackageGraphActions.cpp` | `c9c09e02a5ace1bcfc891c39a1959298a956ea43` | `19C479F66E6AB318A157257FD7AD1A98160C5DCC78BE8975B3B5E347714D5802` |
| `Source\MonolithAsset\Private\Tests\PackageGraphCopyActionsTests.cpp` | `0f182f07049c0a58554456a4366f56c81bfa3365` | `D2B40B642F3F6BF2C85CFF62A18F8760EBB64029D234F7F9E1B61384EBA082F3` |

The package copies were hashed after `BuildPlugin` completed and before the
focused automation hosts were populated from those packages.

---

## 5. UE 5.7 Verification

The final UE 5.7 package was built with `MONOLITH_RELEASE_BUILD=1` so optional
Marketplace dependencies are excluded, matching Monolith's distributable
plugin contract.

| Gate | Result | Evidence |
|---|---|---|
| Final Editor plugin build | PASS | `529/529`; `Result: Succeeded`; `BUILD SUCCESSFUL`; package `D:\P4\MonolithReferenceReviewUE57MergeReady404997cPackage` |
| Changed production TU | PASS | `MonolithAssetPackageGraphActions.cpp` compiled in the final build |
| Changed regression TU | PASS | `PackageGraphCopyActionsTests.cpp` compiled in the final build |
| Focused automation | PASS | `1/1` succeeded, `0` failed, test warnings `0`, test errors `0` |
| Test | PASS | `Monolith.Asset.PackageGraph.RegistryAndParamGuards` |
| Report | PASS | `D:\P4\MonolithReferenceReviewUE57MergeReadyHost\Saved\Automation\ReferenceFixup-MergeReady-UE57\index.json`; SHA-256 `2C0259EFB67AFFE845F2CB6EF5F75BA85730D0B0C4A34E038B650F6CC32BD9BC` |
| Final module binary | PASS | `UnrealEditor-MonolithAsset.dll`; 1,605,120 bytes; SHA-256 `C9573039F3914FED68210CF47483ABDF002CED25ABE7A6269C054C3BBAC463FB` |

---

## 6. UE 5.8 Verification

The final UE 5.8 build used a foreign-plugin host so the externally owned Speed
editor and its Live Coding session remained untouched.

| Gate | Result | Evidence |
|---|---|---|
| Final Editor plugin build | PASS | `529/529`; `Result: Succeeded`; `BUILD SUCCESSFUL`; package `D:\P4\MonolithReferenceReviewUE58MergeReady404997cPackage` |
| Changed production TU | PASS | `MonolithAssetPackageGraphActions.cpp` compiled in the final build |
| Changed regression TU | PASS | `PackageGraphCopyActionsTests.cpp` compiled in the final build |
| Focused automation | PASS | `1/1` succeeded, `0` failed, test warnings `0`, test errors `0` |
| Test | PASS | `Monolith.Asset.PackageGraph.RegistryAndParamGuards` |
| Report | PASS | `D:\P4\MonolithReferenceReviewUE58MergeReadyHost\Saved\Automation\ReferenceFixup-MergeReady-UE58\index.json`; SHA-256 `CED3B1704DA8DB52433DB805DB838673D684A92DEB7B043E34E435838C79351A` |
| Final module binary | PASS | `UnrealEditor-MonolithAsset.dll`; 1,514,496 bytes; SHA-256 `702D29B3C7906355FE5BBCB4434A3CDB8668F84C90DF0DBA97209C8B33472221` |

Both minimal automation hosts log an unrelated missing `GameFeatureData` Asset
Manager rule. They also cannot bind Monolith HTTP port `9316` because the
user-owned Speed editor already owns that endpoint. The existing editor was
not stopped; the focused automation report itself has zero warnings and zero
errors.

---

## 7. Static and Publication Gates

| Gate | Result | Evidence |
|---|---|---|
| Diff hygiene | PASS | `git diff --check` returned no whitespace errors |
| Proxy seed parity | PASS | `python Scripts/test_proxy_seed_parity.py`; 19 native/Python dispatchers match |
| Repository-prescribed static command | UNAVAILABLE | `Scripts/ci_static_checks.py` and `.github/monolith-static-ci.json` are absent from this fork base; the exact command exits before analysis |
| Latest-checker differential | PASS | With only the two incompatible offline executable gates disabled symmetrically, base and branch both report 36 blockers / 962 advisories; new findings `0` |
| Public collision scan | PASS | No open PR in `kunkunGames/monolith`, `kunkunGames/monolith-fork`, or `tumourlove/monolith` changes the package-graph implementation or test files |
| API contract | PASS | `Docs\API_REFERENCE.md` documents preflight-before-apply strict behavior |
| Module spec | PASS | `Docs\specs\SPEC_MonolithAsset.md` documents blockers, truncation, diagnostics, and best-effort opt-out |

---

## 8. Visual and Delivery Scope

| Gate | Result | Reason |
|---|---|---|
| PC 1920x1080 screenshot | N/A | This change affects a headless editor package-reference writer and structured diagnostics; no runtime UI, gameplay, VFX, animation, material, or asset presentation changes. |
| Discord screenshot upload | N/A | No relevant screenshot artifact exists, so `Build\BatchFiles\Script\UploadScreenshotTestsToDiscord.bat` was not invoked. |

---

## 9. Result

Confirmed strict reference fix-up is now fail-before-mutation for every blocker
that can be discovered by the bounded traversal, including an incompatible
resolved hard-reference target even when missing targets are optional. UE 5.7
and UE 5.8 compile the same final production and regression sources and pass the
same production-path automation fixture.
