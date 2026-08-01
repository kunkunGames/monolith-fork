# Monolith Upstream-to-Local v0.22 Integration Verification

**Date:** 2026-08-02

**Scope:** Consolidate `tumourlove/monolith` through
`kunkunGames/monolith-fork` into `kunkunGames/monolith`, then make the verified
result available to Speed's local `Plugins\Monolith` checkout without losing
user-owned Git or Perforce work

**Result:** Fork v0.22 behavior was ported onto the current origin architecture;
UE 5.7 and UE 5.8 strict non-unity builds, focused editor automation, offline
query packaging, release validation, and static-contract differential checks
passed at the documented boundary

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

The source tree used for build and test verification was
`79f209a3ae5802de6cb8072a2664429ff53bd043`.

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
| UE 5.7 | `Strict`, `WarningsAsErrors`, `DisableUnity`, `NoUBTMakefiles`, all requested Monolith modules | Passed; final link completed and the version-only confirmation completed 7/7 actions |
| UE 5.8 | Same contract and module coverage | Passed; clean exact-source build completed 949/949 actions, then the v0.22 stamp rebuild completed 902/902 actions |

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
| UE 5.7 | `Saved\BuildHosts\Monolith-origin-v0-22-integration-UE57\Saved\Automation\MonolithOriginSync\final-79f209a3-UE57-20260802-024532\index.json` | 26/26 completed; zero failed, not-run, or in-process tests |
| UE 5.8 | `Saved\BuildHosts\Monolith-origin-v0-22-integration-UE58\Saved\Automation\MonolithOriginSync\final-79f209a3-UE58-20260802-024646\index.json` | 26/26 completed; zero failed, not-run, or in-process tests |

One PCG large-graph case completed successfully with its existing intentional
action-overwrite and AssetRegistry cleanup warnings; the other 25 cases were
clean successes on each engine.

## 5. Offline Query and Release Gates

The offline executable build first failed closed because the generated catalog
was absent. After generating the catalog through the repository-owned generator,
all release gates used the current source:

| Verification | Result |
|---|---|
| Generated catalog | Current, 2,074 actions across 61 namespaces |
| Immutable query bundle | Source hash `c8429d0120de7888`; executable SHA-256 `a1ee16f6dc539629d581200ee989bc417e4d49973f773fba5a88357c11b96a91` |
| `monolith_query.exe --version` | Plugin `0.22.0`, parity `2026-05-29.1` |
| `monolith_query.exe monolith status` | Loaded the generated snapshot and reported all 2,074 actions |
| Windows PowerShell 5.1 release-body self-test | Passed 7 reject and 2 accept cases |
| Python release/catalog tests | 18/18 passed |
| `git diff --check` | Passed |
| Static-checker self-test | Passed |

The freshly generated catalog and executable are ignored verification outputs;
they are not part of the source integration commit.

## 6. Static-CI Differential Boundary

The full static checker reported ten benchmark blockers in both the integrated
worktree and the clean pre-integration `origin/master` baseline. Eight directly
reported contract-test failures were rerun one by one against both trees and
had identical exit status and failure signatures:

| Existing contract family | Baseline | Integrated tree |
|---|---|---|
| SourceIndex and ProjectIndex benchmark contracts | Failed | Same failure |
| AssetEditing and AI capability benchmark contracts | Failed | Same failure |
| ActionGuidance and common task corpus | Failed | Same failure |
| Schema-completeness enumeration and CI inventory | Failed | Same failure |

The dominant environment error is the existing project-root guard expecting one
`.uproject` beside `Plugins` while a bare Git worktree has none. Offline-parity
and accepted-binary inventory findings likewise depend on ignored database and
binary state outside the source diff. The only integration change under
`Scripts`, `Benchmarks`, or `.github` is the independently passing
`Scripts\verify_release_body.ps1` hardening. Therefore the full static run is not
claimed green, but the differential shows that this v0.22 integration introduced
no new blocker in those existing failing contracts.

## 7. Local-Work and Visual Boundary

Before publication, the primary Speed checkout's dirty Git bytes and Perforce
ownership are hashed and audited. The remote merge is prepared in a clean
worktree first; local advancement is allowed only if those dirty bytes remain
identical and task files are assigned to a numbered changelist without moving
existing CL 1325 or CL 1407 work.

Screenshot verification is not applicable. This change integrates Git history,
C++, scripts, and documentation without a runtime visual, gameplay, UI, VFX,
material, or asset-presentation change. No `1920x1080` capture or
`Build\BatchFiles\Script\UploadScreenshotTestsToDiscord.bat` upload is required.
