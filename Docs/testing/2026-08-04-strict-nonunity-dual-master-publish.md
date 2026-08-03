# Strict Non-Unity Dual-Master Publish Verification

**Date:** 2026-08-04

**Scope:** Validate the fork-first Monolith integration against Unreal Engine 5.8, repair strict-build and focused-contract failures at their owning boundaries, and publish one verified Git commit to both `kunkunGames/monolith:master` and `kunkunGames/monolith-fork:master`

**Perforce ownership:** Pending CL 1441

**Result:** UE 5.8 strict non-unity compilation passed 907/907 actions and the final focused Monolith automation set passed 34/34 tests. Generated-catalog, release-marker, query-publish, JSON, Python, and static-checker self-tests passed. Hosted GitHub results remain an external post-push gate and must be reported separately from the local build verdict.

---

## 1. Execution Boundary

The Speed checkout already had an externally owned headless Unreal Editor process. It was neither terminated nor used as a build host. The exact Monolith source was instead exposed as the only `Plugins\Monolith` checkout in an isolated project-shaped host whose `.uproject` declares `EngineAssociation` `5.8`.

| Boundary | Verified value |
| --- | --- |
| Source checkout | `D:\P4\speed\Plugins\Monolith` |
| Isolated source worktree | `D:\P4\MonolithStrictCurrentSource20260804` |
| Isolated UE 5.8 host | `D:\P4\MonolithStrictCurrentUE58Host20260804` |
| Protected entry point | `Build\BatchFiles\BuildGameEditorStrictNonUnity.bat` |
| Build flags | `-OverrideBuildEnvironment -Strict -WarningsAsErrors -DisableUnity -NoUBTMakefiles` |
| P4 build boundary | `P4_BUILD_CHANGELIST=default` inside the isolated non-P4 validation host; primary task files remain owned by CL 1441 |

No alternate engine checkout was hard-coded. The host build script resolved Unreal Engine from the host `.uproject` association.

---

## 2. Strict-Build Findings and Root Fixes

The first strict build scanned 954 actions and failed on three source-contract mismatches. Each failure was corrected at the owning source or test boundary rather than hidden with a compiler, unity, warning, or version bypass.

| Failure | Root cause | Correction |
| --- | --- | --- |
| `MonolithUpdateSubsystem.cpp` format validation | The platform-specific update command format string no longer contained the removed updater-preservation fields, but stale argument triplets remained. | Removed the obsolete Windows and macOS/Linux triplets so format placeholders and arguments have one exact contract. |
| `MonolithIndexParamGuardTests.cpp` compile failure | The test called `FJsonObject::IsValid()` through the JSON object instead of validating the `TSharedPtr<FJsonObject>`. | Guarded the schema object and validated the shared pointer before inspecting the parameter schema. |
| `MonolithUIParamGuardTests.cpp` compile failure | A fixture still called the retired eight-argument helper signature. | Migrated the fixture to the current bounded helper contract. |

The first repaired build completed 907/907 actions. Subsequent focused automation exposed five semantic test failures, which were also repaired before the final strict rebuild:

| Contract | Root fix |
| --- | --- |
| UI boolean preflight | `auto_size` and `compile` now require exact JSON booleans before Widget Blueprint loading or mutation; permissive string-to-bool conversion is rejected. |
| Complex parameter compatibility | Registry dispatch again performs schema-bounded recovery of string-encoded objects and arrays before required/type validation; explicit exact types and `StrictComplexTypes` remain strict. |
| GAS shared-key semantics | Tests now match Unreal Enhanced Input: different actions may intentionally share a key, while exact duplicate action/key bindings remain hard conflicts. |
| GAS project mounts | Assertions now reflect supported mounted project plugin content rather than the retired `/Game`-only contract. |
| Save-failure package accounting | The test counts only the two new placeholder packages that actually reach linker creation; preloaded fixture packages are not misclassified as new saves. |

---

## 3. Final Strict Build

| Gate | Result | Evidence |
| --- | --- | --- |
| Unreal Engine | PASS | UE 5.8 resolved from the validation host `.uproject` |
| Strict non-unity compile/link | PASS | 907/907 actions; `Result: Succeeded`; wrapper exit code 0 |
| Warnings-as-errors boundary | PASS | `-Strict -WarningsAsErrors -DisableUnity -NoUBTMakefiles` remained enabled |
| Build duration | PASS | 290.70 seconds in UBT; approximately 293.1 seconds wall time |
| Final log | PASS | `D:\P4\MonolithStrictCurrentUE58Host20260804\Saved\Logs\StrictBuild-after-automation-root-fixes.log` |

---

## 4. Focused Automation

The final source ran the affected Core, GAS, UI, Source Control, Editor automation-session, Blueprint, and Index contract set in the isolated UE 5.8 host.

| Gate | Result | Evidence |
| --- | --- | --- |
| Final test count | PASS | 34/34 `Success`; zero failed and zero test errors |
| Core complex-parameter contracts | PASS | String-encoded compatibility recovery and explicit strict rejection both passed |
| GAS Input Assets | PASS | 9/9 affected tests passed, including shared-key and exact-duplicate behavior |
| UI parameter guard | PASS | Invalid `auto_size` and `compile` types fail before mutation |
| Report | PASS | `D:\P4\MonolithStrictCurrentUE58Host20260804\Saved\AutomationReports\StrictNonUnityDualMaster_Final_20260804_012102` |

Three Editor tests each reported 812 `LogMetaSound` warnings while enumerating Unreal's full automation catalog. Every warning was the engine message `Failed to register automation test tags`; no Monolith assertion or test error accompanied them. The three tests still completed successfully. This engine-wide catalog noise is recorded explicitly and is not represented as a clean zero-warning engine run.

---

## 5. Static, Catalog, and Offline Query Gates

| Gate | Result |
| --- | --- |
| Generated source catalog | PASS; 2,079 actions and immediate `--check` success |
| Catalog generator tests | PASS; 4/4 |
| Static-checker self-test | PASS |
| Python `compileall` | PASS |
| `Monolith.uplugin` and static-CI JSON parsing | PASS |
| Release SHA-256 marker contract | PASS; 4/4 |
| Offline query publish tests | PASS; 10/10 |
| Query executable freshness | PASS; source hash `b74c52beac12abc3`, executable SHA-256 `0e434bf10669307217350519a7ec59b4c8f6f8179c7d8c9544794578f6d94a66`, plugin `0.22.0` |
| Git whitespace check | PASS |
| Full hosted-static checker, local execution | EXPECTED REVALIDATION; 1 blocker and 308 advisories |

The single full-check blocker is `benchmark-contract-tests`: `Scripts/tests/test_benchmark_ci_inventory.py` reports that accepted OfflineParity input size for `Binaries/monolith_query.current.json` drifted. Publishing the newly generated query bundle intentionally invalidates that old accepted evidence until OfflineParity is rerun and promoted against the authoritative EngineSource database. The accepted benchmark record was not rewritten without that rerun. This is a documented benchmark-revalidation boundary, not a C++ strict-build failure. The 308 advisories are repository-wide CRLF hygiene findings plus the existing external `.claude/agents` prerequisite and unavailable live-catalog drift check; none is a new strict compile or focused-test failure.

`Binaries\monolith_query.exe`, its immutable source-addressed executable, generated catalog, and current manifest are ignored Git artifacts but are tracked by the Speed Perforce workspace. Their refreshed bytes are owned by CL 1441.

---

## 6. Source-Control and Publication Contract

The Git working tree began from a commit that already contained both remote master histories: `kunkunGames/monolith:master` was 30 commits behind the local baseline and `kunkunGames/monolith-fork:master` was 2,467 commits behind it, with neither remote ahead. Immediately before publication both remotes must be fetched again and must remain ancestors of the exact commit being pushed.

One exact commit is published to both master refs. A force push, history rewrite, unrelated working-tree cleanup, P4 submit, editor termination, or default-changelist adoption is outside this verification contract.

All task source, documentation, test, and generated P4 artifacts are assigned to pending CL 1441. Unrelated user-opened default-changelist files remain untouched.

---

## 7. Activation and Visual Boundary

The persistent operator controls remain present after integration:

| Command | Ownership |
| --- | --- |
| `Monolith.StartServer` / `Monolith.StopServer` | Monolith Core server activation |
| `Monolith.StartIndexing` / `Monolith.StopIndexing` | Monolith Source indexing activation |

Screenshot and Discord verification are not applicable. This scope changes C++, tests, build contracts, source-control behavior, and documentation without changing runtime visual, gameplay, UMG presentation, VFX, animation, material, or asset appearance. No PC `1920x1080` capture was required, and `Build\BatchFiles\Script\UploadScreenshotTestsToDiscord.bat` was not run.
