# Monolith Master Branch Consolidation Verification

**Date:** 2026-07-27

**Scope:** Consolidate reusable work from the `tumourlove/monolith`, `kunkunGames/monolith-fork`, and `kunkunGames/monolith` branch families into `master`

**Result:** Integrated source and focused verification passed; protected strict non-unity build linked every Monolith module with zero build errors

---

## 1. Integration Contract

The consolidation started from `origin/master` commit
`b766aed48b10dbdec34e92e55ecb0c80d946047f`. Candidate branches were compared by
patch identity and behavior instead of merging branch tips mechanically.
Reusable fixes were replayed as cohesive commits, overlapping implementations
were reduced to one owner, and redundant or superseded branches were excluded.

| Candidate | Disposition | Consolidated value |
|---|---|---|
| `origin/pr/1864` | Integrated with conflict repair | Blueprint parser, ReflectionIntel, Query, release, and UE 5.8 reliability work |
| `tumourlove/pr/104` | Integrated | Updater accepts only fail-closed binary release archives |
| `tumourlove/pr/112` | Integrated | Bounded cross-namespace `find` discovery |
| `tumourlove/pr/113` | Selectively integrated | Structured project-search content and FTS behavior without restoring retired Graph DB ownership |
| `tumourlove/pr/114` and the local graph-retirement line | Rebased and consolidated | Persistent `Monolith.StartServer` / `Monolith.StartIndexing` controls and Graph DB retirement |
| `origin/pr/1934`, `origin/pr/1938`, `origin/pr/1939`, `origin/pr/1940` | Integrated | Focused validation coverage, spec synchronization, and release notes |
| `origin/pr/1935` | Excluded as redundant | Numeric-suffix duplicate behavior was already covered by the consolidated implementation |

## 2. Cohesion Repairs

The branch combination exposed several independently correct changes that
overlapped after rebase. The final integration keeps one reusable owner for each
contract:

| Contract | Consolidated owner |
|---|---|
| SHA-256 byte hashing | `FMonolithHashUtils::TrySha256Bytes`; the duplicate updater-local SHA implementation was removed |
| Blueprint nested graph enumeration | Shared `GetAllGraphsUnique` traversal and graph classification |
| Blueprint function-call reference binding | One function-library-safe resolver and node-class selection path |
| Core discovery parsing | Shared filter, detail, pagination, and `MonolithToolText` helpers |
| Level loading | `MonolithEditorMapLoad::LoadLevelWithPreflight`; the duplicate legacy load was removed |
| Modal telemetry | `FMonolithModalTelemetryState` |
| Temporary package/world residency | `FMonolithPackageResidency`, including Mesh, Niagara, MetaSound, IndexSubsystem, and LevelIndexer callers |

This removes duplicate implementations instead of retaining compatibility
branches whose behavior could drift independently.

## 3. Protected Strict Build

The integration commit was checked out as `Plugins\Monolith` in an isolated
UE 5.8 project host. The host reused the repository-protected build entry point
and resolved the engine from the host `.uproject` `EngineAssociation`:

```powershell
$env:SKIP_EDITOR_LAUNCH = "1"
& Build\BatchFiles\BuildGameEditorStrictNonUnity.bat `
  -Module="<all 46 Monolith modules joined with +>"
```

Result:

| Build property | Evidence |
|---|---|
| Mode | `Strict`, `WarningsAsErrors`, `DisableUnity`, `NoUBTMakefiles` |
| Scope | All 46 modules declared by `Monolith.uplugin` |
| Link coverage | 138 actions, including every requested `.lib` and `.dll` |
| Result | `Succeeded`, wrapper exit `0`, UnrealBuildTool time `13.70s` |
| Incremental confirmation | 5 actions, `Succeeded`, wrapper exit `0`, UnrealBuildTool time `8.45s` |

## 4. Focused Verification

All commands ran against the same integrated source in the project-shaped
verification host.

| Verification | Result |
|---|---|
| `python Scripts\test_project_index_benchmark.py` | 23/23 passed |
| `python Scripts\test_source_index_benchmark.py` | 41/41 passed |
| `python Scripts\test_offline_parity_benchmark.py` | 13/13 passed |
| Static OfflineParity CI contract | 1/1 passed |
| Release SHA-256 marker contract | 4/4 passed |
| Catalog snapshot generator contract | 4/4 passed |
| CRG write-preflight contract | 1/1 passed |
| ActionGuidance corpus/contract validation | Passed |
| `MonolithActivationParity.Tests.ps1` | 2/2 passed |
| `MonolithActivationState.Tests.ps1` | 8/8 passed |
| `CheckIndexFreshness.Tests.ps1` | 7/7 passed |
| `git diff --check` | Passed |

The first ProjectIndex attempt was intentionally discarded because a bare Git
worktree under `Saved\GitWorktrees` is not a valid project-shaped plugin host.
The same test passed without source changes after running beside exactly one
`Speed.uproject`; the fail-closed project-root check was preserved.

## 5. OfflineParity and Static CI

A freshly built `Binaries\monolith_query.exe` was verified against the current
`Saved\EngineSource.db`:

```text
311 MATCH | 0 DIFF | 0 ERROR | 6 explicit decision-id-dependent SKIP
offline_parity_score = 1.0000
```

The final full static-CI run therefore removed both earlier OfflineParity score
and error-rate findings. It reported one independent benchmark-publication
finding:

```text
accepted OfflineParity input size drifted: Binaries/monolith_query.exe
```

That finding compares the freshly built Query executable with the older
checked-in accepted bundle's pinned binary identity. It is not a compile,
link, unit-test, or executable/Python parity failure. Replacing the accepted
bundle is kept separate from branch consolidation because source health
currently requests `source.repair_crg_cache --scope=all`; promoting a new
canonical bundle before that database maintenance would certify a derived cache
known to be stale.

## 6. Source-Control and Cleanup Boundary

The primary `Plugins\Monolith` checkout and every dirty user-owned worktree were
treated as read-only during integration. Temporary clean integration/build
worktrees were removed only after the pushed `master` commit was verified from
the remote.

| Cleanup | Result |
|---|---|
| GitHub PRs | Closed superseded PRs `#1864`, `#1934`, `#1935`, `#1938`, `#1939`, and `#1940` after posting the consolidation disposition |
| `origin` branches | Removed 69 branches belonging to closed PRs or this integration; retained only `master` and the recovery branch `p4-snapshot/monolith-ue58-divergent-20260620` |
| Local branches | Removed 16 integrated, externally reproducible, or abandoned temporary branches and fast-forwarded local `master` to the verified remote |
| Git worktrees | Removed 9 clean/temporary/corrupt worktrees, including the isolated build host, both integration baselines, and the final consolidation worktree after publication |
| Preserved work | Kept 13 user-owned or active external-review worktrees, including every dirty checkout and a concurrently created Interchange worktree |
| External remotes | Did not delete any `contrib/*` or `tumourlove/*` branch |

The primary checkout accumulated additional concurrent user edits while this
integration was running. Its final dirty inventory was therefore intentionally
not reset to the earlier snapshot; no integration command wrote, staged,
stashed, or cleaned those files.

## 7. Screenshot and Discord Upload

Not applicable. This is Git integration, C++/script/config consolidation, and
build verification with no runtime visual, UI, gameplay, VFX, material, or
asset-presentation change. No `1920x1080` screenshot or
`Build\BatchFiles\Script\UploadScreenshotTestsToDiscord.bat` upload was
required.
