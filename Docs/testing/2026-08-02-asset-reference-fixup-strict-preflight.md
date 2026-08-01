# Asset Reference Fix-up Strict Preflight Verification

**Date:** 2026-08-02
**Branch:** `jules/codex/asset/reference-fixup-exactness`
**Reviewed base head:** `07faaf0583a8190f5aa021c9c6cc22fe556427c5`
**Scope:** `asset.fixup_copied_references` strict blocker preflight, mutation ordering, traversal cap, diagnostics, and regression coverage
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

---

## 3. Finding and Resolution

| Finding | Before | Resolution | Regression proof |
|---|---|---|---|
| Late strict blocker left earlier edits applied | One traversal both validated and wrote references; the handler returned `preflight_failed` only after earlier candidates could already be changed | Confirmed strict runs the complete traversal with `dry_run=true` first, then runs the applying traversal only when no blocker exists | A valid first soft reference followed by a missing second target returns failure, preserves the first source path, and leaves the package clean |
| Traversal cap permitted partial strict mutation | `max_packages` set `truncated=true` but still applied the bounded prefix | Strict mutation treats truncation itself as a blocker and returns before visiting or changing a package | Two requested packages with `max_packages=1` return `truncated=true`, `applied_count=0`, and preserve the reference |
| A later non-blocking hard-target miss could clear an earlier blocker | Hard-object miss assigned the aggregate blocker flag | Aggregate with logical OR so an earlier blocker cannot be erased | Both supported engines compile and run the complete fixture |
| Strict success path needed two-pass coverage | Existing tests only covered guards and a generic dry-run | The fixture removes the missing row, reruns the same confirmed strict action, and asserts one real rewrite plus package dirtying | `applied_count=1` and the reference points to the remapped destination object |

Structured strict failures now return `checked_packages`, reference and warning
arrays, checked object/package counts, `candidate_count`, `applied_count`,
`changed_package_count`, `truncated`, and `status="preflight_failed"`.

---

## 4. Current-byte Identity

The final build staging copies were compared to the branch source before the
last engine builds.

| File | SHA-256 |
|---|---|
| `Source\MonolithAsset\Private\MonolithAssetPackageGraphActions.cpp` | `AB11258CD0FBAADB7899B7C935698A0800AFB4DD69FB7EDC1D26AE96DD3B50D1` |
| `Source\MonolithAsset\Private\Tests\PackageGraphCopyActionsTests.cpp` | `2DDAB3E0C1CF195D54963BDED781C044A1B7D112D7EFB4E1A196585A69476B6E` |

The same hashes were observed under the final UE 5.7 release package and UE
5.8 foreign-plugin host immediately before compilation.

---

## 5. UE 5.7 Verification

The final UE 5.7 package was built with `MONOLITH_RELEASE_BUILD=1` so optional
Marketplace dependencies are excluded, matching Monolith's distributable
plugin contract.

| Gate | Result | Evidence |
|---|---|---|
| Final Editor build | PASS | `529/529`; `Result: Succeeded`; `D:\P4\MonolithFollowupUE57Host\Saved\Logs\PackageGraphFixup-UE57-FinalIncremental-20260802.log` |
| Changed production TU | PASS | `MonolithAssetPackageGraphActions.cpp` compiled in the final build |
| Changed regression TU | PASS | `PackageGraphCopyActionsTests.cpp` compiled in the final build |
| Focused automation | PASS | `1/1` succeeded, `0` failed, test warnings `0`, test errors `0` |
| Test | PASS | `Monolith.Asset.PackageGraph.RegistryAndParamGuards` |
| Report | PASS | `D:\P4\MonolithPackageGraphFixupUE57ReleaseAutomationHost\Saved\Automation\PackageGraphFixup-UE57-Final-20260802\index.json` |
| Final module binary | PASS | `UnrealEditor-MonolithAsset.dll`; SHA-256 `BD55E41C286B60D6B12DDA43E51507DDAB8F9890F04571851407C0F6964FFE4A` |

---

## 6. UE 5.8 Verification

The final UE 5.8 build used a foreign-plugin host so the externally owned Speed
editor and its Live Coding session remained untouched.

| Gate | Result | Evidence |
|---|---|---|
| Full Editor baseline build | PASS | The isolated package completed `529/529` before its unrelated optional `UnrealGame` pass; Editor `Result: Succeeded` |
| Final current-byte Editor build | PASS | `83/83`; `Result: Succeeded`; `D:\P4\MonolithFollowupUE58Host\Saved\Logs\PackageGraphFixup-UE58-Final-20260802.log` |
| Changed production TU | PASS | Final action `13/83` compiled `MonolithAssetPackageGraphActions.cpp` |
| Changed regression TU | PASS | Final action `8/83` compiled `PackageGraphCopyActionsTests.cpp` |
| Focused automation | PASS | `1/1` succeeded, `0` failed, test warnings `0`, test errors `0` |
| Test | PASS | `Monolith.Asset.PackageGraph.RegistryAndParamGuards` |
| Report | PASS | `D:\P4\MonolithPackageGraphFixupUE58Package\HostProject\Saved\Automation\PackageGraphFixup-UE58-Final-20260802\index.json` |
| Final module binary | PASS | `UnrealEditor-MonolithAsset.dll`; SHA-256 `220E38B4EA91DAD36F00C7AFBB99C5C2604DA5C5390F120177B5EE01F88AA0E2` |

The first UE 5.8 `BuildPlugin` invocation continued after the successful Editor
build into an optional `UnrealGame` target and exposed pre-existing
`MonolithAudioRuntime` compile errors. The final verification explicitly builds
the supported Editor target only and succeeds. This distinction is not treated
as a source regression in `MonolithAsset`.

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
that can be discovered by the bounded traversal. UE 5.7 and UE 5.8 compile the
same final production and regression sources and pass the same production-path
automation fixture.
