# Monolith Upstream-to-Local v0.22 Integration Verification

**Date:** 2026-08-02

**Scope:** Consolidate `tumourlove/monolith` through
`kunkunGames/monolith-fork` into `kunkunGames/monolith`, then make the verified
result available to Speed's local `Plugins\Monolith` checkout without losing
user-owned Git or Perforce work

**Result:** Fork v0.22 behavior was ported onto the current origin architecture
and published to `kunkunGames/monolith:master`; the result was then merged with
Speed's local Monolith history. UE 5.7 and UE 5.8 strict non-unity builds,
focused editor automation, offline query packaging, release validation, local
byte preservation, and static-contract differential checks passed at the
documented boundary. The clean-checkout catalog regressions found by the first
hosted run were repaired before the final local synchronization

---

## 1. Integration Contract

The three remote tips were fetched before integration:

| Remote line | Verified tip | Relationship |
|---|---|---|
| `tumourlove/master` | `e67544caa9e11569e87c1a4f616568544822d8b6` | Ancestor of `contrib/master` |
| `contrib/master` (`kunkunGames/monolith-fork`) | `07faaf0583a8190f5aa021c9c6cc22fe556427c5` | Contains 19 commits beyond upstream |
| `origin/master` (`kunkunGames/monolith`) | `b52a90d5014ddaa1811ebfce1da960b95c945a02` | Current architecture and 2,338 commits beyond the fork split |

Because `tumourlove/master` was already an ancestor of the fork, the first arrow
required no new upstream commit. The fork and origin had diverged substantially,
so mechanically taking either side of broad conflicts would have restored
retired ownership and duplicate implementations. The integration instead
replayed the fork's still-relevant behavior as cohesive commits on
`origin/master`, then records the fork tip as an explicit merge parent.

The remote semantic plugin source tree was `79f209a3ae5802de6cb8072a2664429ff53bd043`.
Merge commit `135572da39beaf4e71d9fea4abc4ec9b799efd44` records
`kunkunGames/monolith-fork:master` as the second parent without changing that
verified tree. Clean-checkout CI contract fixes then advanced the published
`kunkunGames/monolith:master` tip to
`3683c00e066f312c526a8054477c990519f967ab` without changing the verified C++
module tree.

Speed's divergent local history was then merged in an isolated worktree. Build
failures exposed lifecycle, package-residency, version-gate, and shared-helper
contract mismatches; those were resolved at their owning module boundaries. The
exact local code tree used for final build, automation, and offline-query proof
is `3ef280a268c35617fd67d01826d03bd1b7fa8efd`.

## 2. High-ROI Semantic Integration

| Area | Integrated behavior | Cohesion repair |
|---|---|---|
| Animation | ABP-native animation-layer graph authoring and node-property validation | Reused the consolidated graph/pin grammar instead of restoring fork-local parser copies |
| Blueprint | Component values resolve against the requested Blueprint and enum pins use `PC_Byte` | Kept one component resolver and one shared pin-type contract |
| Indexing | Source-query failures are explicit; interrupted full indexes resume atomically | Preserved current database ownership and added transaction-safe recovery rather than reviving retired Graph DB paths |
| Reflection risk | Git repository roots are discovered at runtime | Removed machine-specific repository lists |
| Release | Offline query build is a real gate; pre-v2 checksum markers fail closed | Kept one generated catalog and one immutable executable-bundle identity |
| Engine support | The same source supports UE 5.7 and UE 5.8 | Centralized version boundaries for traversal, delegates, StringTables, materials, RigVM, mesh merge, PoseSearch, and validation APIs |

The consolidated release version is `0.22.0` in `Monolith.uplugin`, the public
core version macro, release notes, API documentation, and affected module specs.

## 3. Protected Strict Builds

The exact verified source was exposed as the only `Plugins\Monolith` directory
inside separate UE 5.7 and UE 5.8 project-shaped hosts. Each host used
`Build\BatchFiles\BuildGameEditorStrictNonUnity.bat`; engine roots were resolved
from that host's `.uproject` `EngineAssociation`.

| Engine | Build contract | Result |
|---|---|---|
| UE 5.7 | `Strict`, `WarningsAsErrors`, `DisableUnity`, `NoUBTMakefiles`, all requested Monolith modules | Passed; exact local integration linked successfully and its final confirmation completed 67/67 actions |
| UE 5.8 | Same contract and module coverage | Passed; clean exact-local-source build completed 900/900 actions |

No existing Speed editor, headless editor, PIE process, or build host was
terminated or bypassed to obtain these results.

## 4. Focused Editor Automation

Both engines ran the same combined automation filter:

```text
Monolith.PinTypeGrammar
Monolith.Discover.Terse
Monolith.GameFeatures
Monolith.Index.Recovery
Monolith.ProjectSearch
Monolith.Source.Indexer.WriterOpenFailureBroadcastsCompletion
Monolith.PCG.GraphAuthoring.GraphContents.LargeGraphBoundedPersistentComparison
MonolithUI.CreateAnimationV2.Basic
Monolith.Activation
Monolith.Core.McpHostRole.Classification
```

| Engine | Report | Result |
|---|---|---|
| UE 5.7 | `Saved\BuildHosts\Monolith-origin-v0-22-integration-UE57\Saved\Automation\MonolithOriginSync\final-3ef280a2-UE57-20260802-032946\index.json` | 25 clean successes plus 1 warning-success; zero failed, not-run, or in-process tests |
| UE 5.8 | `Saved\BuildHosts\Monolith-origin-v0-22-integration-UE58\Saved\Automation\MonolithOriginSync\final-3ef280a2-UE58-20260802-033027\index.json` | 25 clean successes plus 1 warning-success; zero failed, not-run, or in-process tests |

One PCG large-graph case completed successfully with its existing intentional
action-overwrite and AssetRegistry cleanup warnings; the other 25 cases were
clean successes on each engine.

## 5. Offline Query and Release Gates

The offline executable build first failed closed because the generated catalog
was absent. After generating the catalog through the repository-owned generator,
all release gates used the current source:

| Verification | Result |
|---|---|
| Generated catalog | Current, 2,079 actions across 61 namespaces; semantic SHA-256 `20e64ec18e101841ebe2107298f8e4c6ff7ea988ef7f605d3fab6b30b5a62883` |
| Immutable query bundle | Source hash `b74c52beac12abc3`; executable SHA-256 `0e434bf10669307217350519a7ec59b4c8f6f8179c7d8c9544794578f6d94a66` |
| `monolith_query.exe --version` | Plugin `0.22.0`, parity `2026-05-29.1` |
| `monolith_query.exe monolith status` | Loaded the generated snapshot and reported all 2,079 actions |
| Windows PowerShell 5.1 release-body self-test | Passed 7 reject and 2 accept cases |
| Python release/catalog tests | 18/18 passed |
| `git diff --check` | Passed |
| Static-checker self-test | Passed |

The freshly generated catalog and executable are ignored verification outputs;
they are not part of the source integration commit.

## 6. Static-CI Differential Boundary

The first full static comparison reported ten blockers in both the integrated
worktree and the clean pre-integration `origin/master` baseline. Eight direct
benchmark contract tests were rerun one by one against both trees and had
identical exit status and failure signatures:

| Existing contract family | Baseline | Integrated tree |
|---|---|---|
| SourceIndex and ProjectIndex benchmark contracts | Failed | Same failure |
| AssetEditing and AI capability benchmark contracts | Failed | Same failure |
| ActionGuidance routing weight and common task corpus | Failed | Same failure |
| Schema-completeness enumeration and CI inventory | Failed | Same failure |

The dominant remaining environment error is the existing project-root guard
expecting one `.uproject` beside `Plugins` while a bare Git worktree has none.
Offline-parity inventory also requires the ignored release-only
`Binaries\monolith_query.exe`. Those eight baseline contracts remain separate
from this v0.22 integration and are not reclassified as passing.

The hosted progression makes the integration-owned boundary explicit:

| Run | Commit | Result | Interpretation |
|---|---|---|---|
| `30711596980` | `135572da39beaf4e71d9fea4abc4ec9b799efd44` | 10 blockers, 24 advisories | First clean checkout exposed a missing generated source catalog in addition to the eight baseline contracts. |
| `30714369700` | `e5d6923c9e05472bea28c0e52b02c50b474557a7` | 9 blockers, 24 advisories | Workflow generation removed the offline-snapshot blocker; ActionGuidance still incorrectly depended on a release bundle. |
| `30714512827` | `3683c00e066f312c526a8054477c990519f967ab` | 8 blockers, 24 advisories | ActionGuidance now validates against the generated source catalog; both clean-checkout regressions are gone. |

A separate detached clean-worktree proof confirmed the final contract with no
`Binaries\monolith_query.current.json`: source generation produced 2,074 actions,
its immediate `--check` passed, ActionGuidance passed, and the static-checker
self-test passed. The final hosted run remains red only for the eight documented
baseline contracts and is not claimed green:
`https://github.com/kunkunGames/monolith/actions/runs/30714512827`.

## 7. Local-Work and Visual Boundary

The primary `Plugins\Monolith` checkout advanced from
`c745cc4e819467d58b5ab47c30ff2a06f527c098` to the verified local integration
through a scoped safety stash. All 14 pre-existing modified files and the one
pre-existing untracked verification record were restored and checked against
their pre-merge SHA-256 values; all 15 hashes matched exactly, including mixed
CRLF/LF files. Existing Perforce ownership in CL 1325, CL 1407, and CL 1408 was
not moved.

The integration delta was reconciled narrowly into task CL 1411: 22 adds, 123
edits, and 4 deletes. Two overlapping working-tree paths were deliberately
excluded from the task reconcile: `Docs\specs\SPEC_MonolithUI.md` remains owned
by CL 1407, while the pre-existing unowned modification in
`Source\MonolithIndex\MonolithIndex.Build.cs` remains byte-identical and is not
silently claimed by this task. The clean Git integration contains the v0.22
versions of both files; their primary-checkout working copies remain the
user-owned overlays until those separate changes are resolved.

Screenshot verification is not applicable. This change integrates Git history,
C++, scripts, and documentation without a runtime visual, gameplay, UI, VFX,
material, or asset-presentation change. No `1920x1080` capture or
`Build\BatchFiles\Script\UploadScreenshotTestsToDiscord.bat` upload is required.
