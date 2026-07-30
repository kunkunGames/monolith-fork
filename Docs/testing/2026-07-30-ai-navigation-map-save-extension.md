# Monolith AI Navigation Map Save-Extension Verification

**Date:** 2026-07-30
**Scope:** `ai.rebuild_navigation(save_after=true)` canonical package filename resolution
**Result:** Live map rebuild and save passed; protected build and focused automation are recorded below

---

## 1. Contract

Navigation rebuild persistence must preserve the package's canonical Unreal
extension:

| Package kind | Required extension | Failure behavior |
|---|---|---|
| Map package (`PKG_ContainsMap`) | `.umap` | Never create a same-name `.uasset` shadow |
| Ordinary content package | `.uasset` | Never serialize it as a map |
| Unresolvable package name | None | Report the package as failed instead of guessing a filename |

The implementation resolves the filename through
`FPackageName::TryConvertLongPackageNameToFilename` and exposes `is_map`,
`disk_path`, and `saved` per package in the action result.

## 2. Focused Automation

The regression test is:

```text
Monolith.AI.Navigation.PackageSaveExtension
```

It creates one ordinary package and one package carrying `PKG_ContainsMap`,
then verifies `.uasset`, `.umap`, and null-package fail-closed behavior.

## 3. Live SpeedBox Readback

The live editor loaded the canonical map:

```text
/SpeedBox/Maps/L_Playground_Box
```

Pre-build navigation readback found one `NavMeshBoundsVolume`, one registered
navigation bound, 192 Recast tiles, no remaining dirty areas, and a completed
navigation build.

The schema-discovered action was then executed with:

```json
{
  "save_after": true,
  "timeout_seconds": 60
}
```

The result reported:

| Field | Value |
|---|---|
| `build_started` | `true` |
| `generation_complete` | `true` |
| `is_building` | `false` |
| `remaining_build_tasks` | `0` |
| `is_built` | `true` |
| `wait_ticks` | `1` |
| `save_status.saved_count` | `1` |
| `save_status.failed_count` | `0` |
| Saved package | `/SpeedBox/Maps/L_Playground_Box` |
| `is_map` | `true` |
| Disk path suffix | `Plugins/GameFeatures/SpeedBox/Content/Maps/L_Playground_Box.umap` |
| `saved` | `true` |

Disk readback confirmed that the saved canonical file is the non-empty
`.umap`. A stale same-size `.uasset` add in the default changelist was reverted
and moved, without deletion, to:

```text
Saved/Recovery/SpeedBox/L_Playground_Box-accidental-uasset-20260730-0031.uasset
```

After that repair, the content directory contained only the canonical `.umap`
package. During the final open-changelist review, a concurrent CL 1329 network
test flow independently reopened that `.umap` in the default changelist. The
map is not part of CL 1345 and was deliberately left with its external owner;
this record does not claim that the current default changelist is empty.

## 4. Protected Build

The repository-protected build was run with `P4_BUILD_CHANGELIST=1345` and
`SKIP_EDITOR_LAUNCH=1` through:

```powershell
Build\BatchFiles\BuildGameEditorAndRun.bat
```

Result: **passed** (`Result: Succeeded`; `Build succeeded`). The build used the
project's `Speed.uproject` association to resolve UE 5.8 and retained the
aggregate binary ownership in CL 1325. Evidence:

```text
Saved/Logs/Codex/20260730_OpenCLReview/ProtectedBuild_CL1345.stdout.log
```

The freshly linked editor then ran the exact regression through the live
Monolith automation action:

| Run | Matched | Passed | Failed | Warnings |
|---|---:|---:|---:|---:|
| `automation-20260729T195557Z-9FF30D19` | 1 | 1 | 0 | 0 |

## 5. Visual Evidence and Discord

Not applicable. The change corrects the disk extension used while persisting
navigation data and does not alter the level's geometry, materials, UI,
gameplay presentation, or camera output. The live action and disk/P4 readback
are the acceptance evidence; no screenshot or Discord upload is required.

## 6. Perforce Ownership

CL 1345 contains only the navigation implementation, its package-filename
helper, focused regression test, module specification, and this verification
record. The concurrently opened
`Plugins/GameFeatures/SpeedBox/Content/Maps/L_Playground_Box.umap` remains
outside CL 1345, and no task file is left in the default changelist by this
change.
