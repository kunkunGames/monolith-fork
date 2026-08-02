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

The primary checkout's pre-existing ignored `Binaries\monolith_query.current.json`
was deliberately not overwritten. It still declares plugin `0.21.3` while its
selected executable reports `0.22.0`, so `monolith status` fails closed on that
identity mismatch. The exact v0.22 bundle above remains in the isolated local
integration worktree and passes `--version`, manifest SHA-256 checks, and
`monolith status`. Updating the primary ignored bundle is a separate binary-owner
operation, not part of this source merge or CL 1411.

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

The integration delta was reconciled narrowly into task CL 1411: 23 adds, 130
edits, and 4 deletes (157 paths total). Two overlapping working-tree paths were deliberately
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

---

## 8. 2026-08-03 High-ROI Reliability Follow-up

The later fork reliability series was integrated selectively rather than by
merging the fork's divergent `master`. Fork PRs #14, #15, #16, #17, #19, #20,
and #21 contributed bounded ingest, strict package-reference preflight,
transactional Interchange export, package-target audio, DDS-shape, and portable
path-identity repairs. Fork PR #18 remained excluded because current origin
already owns a stronger source-addressed immutable proxy flow.

| Git boundary | Commit | Result |
|---|---|---|
| Exact source verified on UE 5.7 and UE 5.8 | `39e9ba81cda0d6e4c9c659ea7b65ff177e420e06` | Full release package builds and focused packaged-host automation passed |
| Final PR head | `2bb7b214948b9fbbcc18b23ec64ff46caf4e21ee` | Adds only the refreshed verification record after the exact source head |
| `kunkunGames/monolith` PR #2048 merge | `cf8c8461fdfbc668ee8a04b475b0f9a2c1764540` | Exact-head merge completed after final Codex review found no major issues and both review threads were resolved |
| Speed local Monolith merge | `3b6c157c04718ffe0462d96d88eb4ce4ff1094e0` | Parents are prior local head `f425c00b` and origin merge `cf8c8461`; both are ancestors |

The two review findings were fixed before merge. Strict `save=true`
package-reference repair now proves every planned save destination and performs
source-control preparation plus non-destructive write probes before the first
mutation. Required-plugin static validation now requires exactly one
case-insensitive entry and rejects valid-plus-disabled/optional duplicates.

### 8.1 Local-byte and Perforce ownership

Before synchronization, `p4 reconcile -n Plugins/Monolith/...` reported no
differences: the 22 tracked and 2 untracked Git working entries already matched
the current Perforce depot bytes. Safety stash
`5612186c117649dcea89a83e4c3a2fd1a064f571` preserved those 24 entries while the
primary checkout fast-forwarded. Applying the stash produced no conflict, and
all 24 resulting file hashes matched their corresponding stash objects. The
stash remains retained as a recoverable safety copy.

The first post-merge reconcile preview contained exactly the 37 source and
documentation paths in the Git delta from `f425c00b` to `3b6c157c`. Those paths,
this verification-record update, and the protected build outputs are owned by
pending CL 1441:

| CL 1441 group | Paths |
|---|---:|
| High-ROI source and documentation delta | 37 |
| This local follow-up verification update | 1 |
| Rebuilt Monolith DLL/PDB outputs | 20 |
| Other Speed/GameFeature linked outputs from the protected build | 9 |
| **Total** | **67** |

The default changelist remained empty. A later workspace-wide reconcile preview
also reported older unrelated UI test assets under `Content\Tests\Monolith`, a
validation host under
`System.Management.Automation.Internal.Host.InternalHost`, and sprite candidates
under `candidates\idle_00`. Their timestamps predate this local verification;
none was opened, moved, or claimed by CL 1441.

### 8.2 Protected local build

`Speed.uproject` resolved `EngineAssociation=5.8` to `D:\Engine\UE_5.8`. With
`P4_BUILD_CHANGELIST=1441` and `SKIP_EDITOR_LAUNCH=1`, the repository-owned
`Build\BatchFiles\BuildGameEditorAndRun.bat` completed all 90 actions, linked
the current local modules, reported `Result: Succeeded`, and exited 0. It did
not launch the editor. The full log is
`Saved\Logs\MonolithHighRoiIntegration_CL1441_Build.log`.

| Local editor artifact | Size | SHA-256 |
|---|---:|---|
| `Plugins\Monolith\Binaries\Win64\UnrealEditor-MonolithAsset.dll` | 1,669,632 bytes | `E664C985A11FA0A496792E1BD114A3B5BA71D1E14DF941B066C41D747D39192B` |
| `Plugins\Monolith\Binaries\Win64\UnrealEditor-MonolithInterchange.dll` | 526,336 bytes | `C9D6D16A95C969A2C0BA1A4FF6264440984CBBFA97B668FA073FBF72D4C4A308` |
| `Plugins\Monolith\Binaries\Win64\UnrealEditor-MonolithAnimation.dll` | 3,257,856 bytes | `0649EDC7B1796E028B18E59B56C49CEF181F0E0BE02722C992C1DC4D2ADAEFD3` |
| `Plugins\Monolith\Binaries\Win64\UnrealEditor-MonolithAudioRuntime.dll` | 262,144 bytes | `7529F7E7F010761E0416273061C7FA07E084D0BBECC6C72C91605216C7D49ACD` |

### 8.3 Local focused automation

The built Speed project ran each regression in a new report directory with the
Monolith MCP listener disabled. All processes exited 0 and wrote empty stderr:

| Test | Result | Report SHA-256 |
|---|---|---|
| `Monolith.Asset.PackageGraph.RegistryAndParamGuards` | 1 succeeded, 0 failed, 0 not run | `ECBC5CEA981C1ADC9E8ED16A29C5ACEA9E5AE08D6D829235795150ADBC9708F6` |
| `Monolith.Interchange.ExportTransaction` | 1 succeeded, 0 failed, 0 not run | `ACB965392FB5E2CC91C5888D53A75146317C8353F3B96E091A8AFE0A17C8D260` |
| `Monolith.ParamGuard.MonolithInterchange.ImportRejectsMalformedParams` | 1 succeeded, 0 failed, 0 not run | `5E1901389332793401CFD257A15A7066155D71D9790F25AF0885E96E460BAF8D` |
| `Monolith.ParamGuard.Animation.ValidateChooser` | 1 succeeded, 0 failed, 0 not run | `4D0BED74A1C4D131EBB0FA49DF64C47C615BF266D1FBE0699E6876D60F8196A8` |

Reports live under
`Saved\AutomationReports\MonolithHighRoiIntegration_CL1441`. The local static
checker self-test also passed. Hosted Static CI remains at the documented clean
origin baseline of 8 benchmark/project-shape blockers and 24 advisories, with
zero new `uplugin-dependency` findings; it is not represented as green.

Screenshot verification and Discord upload remain **N/A** for this source,
build, and repository-integration change. No visual presentation contract was
modified, so `Build\BatchFiles\Script\UploadScreenshotTestsToDiscord.bat` was
not invoked.
