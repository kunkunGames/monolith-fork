# Upstream Fork-Chain Integration Verification

**Date:** 2026-07-31
**Scope:** `tumourlove/monolith` → `kunkunGames/monolith-fork` → `kunkunGames/monolith` → Speed `Plugins/Monolith`
**Host project:** `D:\P4\speed\Speed.uproject`
**Engine:** Unreal Engine 5.8, resolved from `Speed.uproject` `EngineAssociation`
**Perforce changelist:** `1392`

---

## 1. Goal

Integrate the three-repository Monolith chain into the Speed checkout without replaying obsolete duplicate implementations. The selected result must preserve Git ancestry, prefer the most cohesive and reusable implementation at each overlap, compile under UE 5.8 strict non-unity rules, retain unrelated Perforce work, and leave a reproducible evidence record.

---

## 2. Provenance and Ancestry

| Role | Ref | Verified SHA |
|---|---|---|
| Public upstream | `tumourlove/master` | `18dbf57d81666980fe79ec3e25d85cdf8e4f4aa4` |
| Contribution fork | `contrib/master` (`kunkunGames/monolith-fork`) | `ff4c4aec1a263353b1ae4acbc334fc711d5a351e` |
| Previous integration target | `origin/master` (`kunkunGames/monolith`) | `a3ca8bf69319f04a5d520736d3f31bdc44c4f0e1` |
| Published integration target | `origin/master` | `b52a90d5014ddaa1811ebfce1da960b95c945a02` |
| Local Speed Monolith base | local branch before merge | `92a8e55d` |
| Explicit three-way merge base | shared integration base | `a3ca8bf69319f04a5d520736d3f31bdc44c4f0e1` |

`git merge-base --is-ancestor` passed for:

- `tumourlove/master` → `contrib/master`
- `tumourlove/master` → published `origin/master`
- `contrib/master` → published `origin/master`
- previous `origin/master` → published `origin/master`

The final remote update was a non-force fast-forward from `a3ca8bf6` to `b52a90d5`. `git ls-remote origin refs/heads/master` returned the same full SHA after push.

---

## 3. Cohesive Selection Decisions

| Overlap | Selected direction | Reason |
|---|---|---|
| Asset authoring and inspection | Fork hardening plus the shared `FMonolithActionResult` object contract | Retains atomic save/rollback behavior while removing stale `FJsonValue` compatibility code |
| PCG graph/component authoring | Fork graph/component implementation, with UE 5.8 `EGetObjectsFlags` traversal APIs | Maximizes reuse of the common PCG authoring helpers and removes deprecated boolean traversal overloads |
| Parameter schemas | Strict complex-type opt-out and string-encoded array/object recovery | Preserves client compatibility while allowing mutation-sensitive actions to demand exact JSON types |
| GAS Enhanced Input | Strict path/type validation, mutation gates, player-mappable metadata, and registration tracking | Keeps one complete authoring contract instead of parallel partial implementations |
| GameFeatures ActionSet components | Local high-value action ported onto the fork's `PrepareInstancedActionEdit` → transient validation → `CommitInstancedActionEdit` pipeline | Reuses the stronger shared dry-run and commit path rather than reviving the obsolete `EnsureInstancedActionObject` branch |
| Duplicate surfaces | Removed obsolete Interchange, Chooser, Fuzzy include, and duplicate Editor parameter-test surfaces | Eliminates conflicting registrations/tests while retaining the unique `clear_baseline` assertion in the consolidated test |
| Core/source-control integration | Retained current module registration and source-control prepare-decision contracts | Avoids regressing newer local routing and batch behavior |

The local merge used an explicit base because Git's criss-cross automatic base selection produced 37 textual conflicts. Recomputing against `a3ca8bf6` reduced the review set to 13 true overlaps, which were then resolved by contract rather than by whole-file side selection.

---

## 4. Catalog and Static Verification

| Gate | Command | Result |
|---|---|---|
| Catalog generation | `python Tools\MonolithQuery\generate_monolith_catalog_snapshot.py` | PASS; local Speed catalog contains 2,077 actions |
| Catalog freshness | `python Tools\MonolithQuery\generate_monolith_catalog_snapshot.py --check` | PASS; snapshot current |
| Catalog generator tests | `python -m unittest Tools.MonolithQuery.test_generate_monolith_catalog_snapshot` | PASS; 4/4 |
| Static checker selftest | `python Scripts\ci_static_checks.py selftest` | PASS |
| Offline parity unit test | `python -m unittest Scripts.tests.test_ci_static_offline_parity` | PASS |
| Whitespace/conflict gate | `git diff --cached --check` plus conflict-marker scan | PASS |
| Full hosted static configuration | `python Scripts\ci_static_checks.py --config .github\monolith-static-ci.json --github check` | 1 blocker, 323 advisories; no merge-owned blocker |

The one local static blocker is the pre-existing accepted OfflineParity size drift for `Binaries/monolith_query.current.json`. That ignored local/release artifact was last written before this integration and was not modified or opened in changelist `1392`. The standalone Git worktree additionally lacks an owning `.uproject` and `Binaries\monolith_query.exe`; its environment-dependent benchmark checks were therefore recorded as unavailable rather than treated as source regressions. The source-owned duplicate Automation Test name found by the standalone run was fixed by consolidating the unique `clear_baseline` case and removing the duplicate test file.

The local full static output is recorded at:

`Saved\Logs\MonolithChainStaticCI-20260731-043923.stdout.log`

---

## 5. UE 5.8 Build Verification

All compile verification used `P4_BUILD_CHANGELIST=1392` and the protected Speed build entry point.

| Gate | Command / scope | Result |
|---|---|---|
| Full target strict non-unity, no link | `Build\BatchFiles\BuildGameEditorStrictNonUnity.bat -NoLink` | BLOCKED outside Monolith by six pre-existing first-header include violations in `SpeedCore` and `SpeedSwitching`; all scheduled Monolith actions reached completion |
| Changed Monolith modules | `-NoLink -Module=MonolithCore+MonolithAsset+MonolithConfig+MonolithGAS+MonolithGameFeatures+MonolithIndex+MonolithInterchange+MonolithPCG+MonolithSourceControl` | PASS; 233/233 strict non-unity actions |
| Consolidated Editor parameter test | `-NoLink -Module=MonolithEditor` | PASS; 34/34 strict non-unity actions |

The unrelated full-target violations were:

- `Plugins/GameFeatures/SpeedCore/Source/SpeedCoreRuntime/Private/UI/SPDTagChaseHUDWidget.cpp`
- `Plugins/GameFeatures/SpeedCore/Source/SpeedCoreRuntime/Private/UI/SPDTagChaseLifeGlyphWidget.cpp`
- `Plugins/GameFeatures/SpeedCore/Source/SpeedCoreRuntime/Private/UI/SPDTagChaseLivesWidget.cpp`
- `Plugins/GameFeatures/SpeedCore/Source/SpeedCoreRuntime/Private/UI/SPDTagChasePlayerIconWidget.cpp`
- `Plugins/GameFeatures/SpeedSwitching/Source/SpeedSwitchingRuntime/Private/UI/SPDSwitchingHUDWidget.cpp`
- `Plugins/GameFeatures/SpeedSwitching/Source/SpeedSwitchingRuntime/Private/UI/SPDSwitchingLeaderboardRowWidget.cpp`

A linked full editor build was not used to overwrite binaries already owned by unrelated changelist `1325`. The focused `-NoLink` gates prove compilation of every changed Monolith module while preserving that changelist's binary state.

---

## 6. Source-Control Isolation

| Check | Result |
|---|---|
| Task changelist | All task source, tests, docs, generated catalog, and delete actions are assigned to CL `1392` |
| Unrelated changelist | Existing CL `1325` binary and benchmark artifacts were not moved or overwritten |
| Default changelist | No task file was intentionally left in the default changelist |
| Git remote update | `kunkunGames/monolith` master was updated without force; third-party repositories were only fetched/read |

---

## 7. Visual and Delivery Scope

| Gate | Result | Reason |
|---|---|---|
| 1920x1080 screenshot | N/A | The change is Git integration, C++ contracts, tests, docs, and catalog metadata; it has no gameplay, UI, VFX, material, asset, or presentation delta. |
| Discord screenshot upload | N/A | Screenshot verification was not relevant, so `Build\BatchFiles\Script\UploadScreenshotTestsToDiscord.bat` was not run. |

---

## 8. Conclusion

PASS for the requested Monolith integration scope. The published Git chain preserves ancestry, duplicate implementations were resolved toward the shared higher-cohesion contracts, the local catalog is current at 2,077 actions, all affected Monolith modules compile under UE 5.8 strict non-unity rules, and unrelated Perforce work remains isolated.
