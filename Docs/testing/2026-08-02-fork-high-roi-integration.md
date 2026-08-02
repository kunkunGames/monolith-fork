# Monolith Fork High-ROI Follow-up Integration Verification

**Date:** 2026-08-03 (KST)

**Origin baseline:** `3683c00e066f312c526a8054477c990519f967ab`

**Verified source head:** `39e9ba81cda0d6e4c9c659ea7b65ff177e420e06`

**Scope:** Selectively port the merge-ready reliability work from
`kunkunGames/monolith-fork` to `kunkunGames/monolith` without merging the
fork's broadly divergent `master`, then verify the resulting source against
Unreal Engine 5.7 and 5.8.

**Result:** VERIFIED

---

## 1. Selection Boundary

The origin and fork branches have thousands of unrelated commits on opposite
sides of their split, so a broad merge would reintroduce retired ownership and
create low-signal conflicts. The integration replays only the reviewed,
high-ROI fixes whose behavior remains applicable to the current origin
architecture.

| Fork PR | Integrated behavior | Origin integration commits |
|---|---|---|
| #14 | Bound texture byte/base64 dimensions and decode allocations before expensive work | `b8f37bdf`, plus DDS follow-up `1676d178` |
| #15 | Preflight strict asset-reference fixups before mutation | `6f6e4c45` |
| #16 | Stage exporter outputs and commit/rollback them transactionally | `887de143` |
| #17 | Bound font-family ingest inputs | `552961ad` |
| #19 | Make `MonolithAudioRuntime` package-target compatible | `c37e9c76` |
| #20 | Reject unsupported DDS array/cubemap/volume surface shapes before allocation/import | `1676d178` |
| #21 | Reject case-only/non-portable export aliases while preserving host path ownership semantics | `376a2b8b`, `98a8117c` |

Fork PR #18 was deliberately excluded. Current origin already owns a stronger,
source-addressed immutable proxy/manifest flow, so replaying the fork proxy
slice would duplicate and weaken the active architecture rather than improve
it. Fork proof documents for the selected changes are retained in the same
history so each contract remains reviewable with its implementation.

---

## 2. Origin Compatibility Repairs

### 2.1 Plugin dependency resolution

Clean packaged hosts exposed two descriptor bugs that ordinary compilation did
not catch:

| Failure | Root cause | Repair |
|---|---|---|
| Chooser could not be loaded through required PoseSearch | A direct enabled-but-optional `Chooser` reference was filtered before PoseSearch's required transitive edge could enqueue it | Remove the direct Chooser reference and let required PoseSearch own the dependency |
| `MonolithIndex` failed to load `UnrealEditor-GameplayAbilities.dll` | `MonolithIndex` and `MonolithGAS` hard-link GameplayAbilities modules while `Monolith.uplugin` declared the plugin optional | Make GameplayAbilities an enabled, non-optional plugin reference |

`.github/monolith-static-ci.json` and `Scripts/ci_static_checks.py` now encode
both invariants. The checker rejects a direct optional Chooser reference and
requires exactly one GameplayAbilities reference with `Enabled=true` and
without `Optional=true`; matching is case-insensitive, and any duplicate
case-only, disabled, or optional reference fails the contract. Its self-test
contains a mutation case for each regression, including a valid entry paired
with a conflicting lowercase duplicate.

The final packaged descriptors on both engines contain zero direct Chooser
references and exactly one enabled, non-optional GameplayAbilities reference.
Minimal-host logs confirm that Unreal mounts both Chooser (through PoseSearch)
and GameplayAbilities, with no Monolith module load/preload failure.

The final manual dependency pass found a second half of the Chooser contract:
`MonolithAnimation.Build.cs` still compiled `WITH_CHOOSER=0` whenever
`MONOLITH_RELEASE_BUILD=1`, even though required PoseSearch guarantees Chooser
on both supported engines. That silently removed the ten Monolith-owned Chooser
actions from release binaries. `MonolithAnimation` now follows the required
transitive edge with unconditional `Chooser` and `GameplayTags` module links
and fixes `WITH_CHOOSER=1` for every supported target. Enabling that previously
dark translation unit exposed and fixed its missing direct
`MonolithJsonUtils.h` include.

### 2.2 Parameter-validation boundaries

The fork's Interchange ParamGuard expected malformed optional JSON types to
reach the action handler and return a row-level error. Current origin validates
every declared schema before dispatch. The port keeps that stronger global
contract and updates the focused test to require the registry's
`failure_cause=invalid_param` result naming the exact bad field. The later
native Texture2D PNG export, staged replacement, rollback, and cleanup
assertions remain unchanged.

The newly active `Monolith.ParamGuard.Animation.ValidateChooser` test had the
same stale assumption: it expected handler-local wording even though registry
schema validation rejects the malformed optional string first. The final test
requires `ErrorCode=invalid_params`, structured
`failure_cause=invalid_param`, and an error naming the exact field plus
`expected string`. Both UE 5.7 and UE 5.8 pass that contract from their final
packaged editor modules.

### 2.3 Review-driven strict-save preflight

The initial strict reference-fixup port proved that every candidate reference
could be rewritten before mutation, but its `save=true` path did not prove that
every changed package could be persisted before applying the first rewrite.
That left a late save failure capable of producing a partially saved and dirty
result despite the strict contract.

The final action now performs a dry traversal first and records the complete
`planned_changed_packages` set. Before any property mutation, every planned
package filename and parent directory is validated, conflicting file/directory
shapes are rejected, source-controlled destinations are prepared as one batch,
and every destination receives a non-destructive write probe. A failed check
returns `status=preflight_failed`, `applied_count=0`, per-package
`save_preflight` diagnostics, and leaves the candidate reference and package
dirty state unchanged. The focused automation test creates a file where a
required save directory must exist and verifies that the action rejects that
destination before mutation.

---

## 3. Full Release Package Builds

Both engines built the verified source with release optional-dependency gates
enabled and a fresh package directory:

```powershell
$env:MONOLITH_RELEASE_BUILD = "1"
& <Engine>\Engine\Build\BatchFiles\RunUAT.bat BuildPlugin `
  -Plugin=D:\P4\MonolithOriginHighRoiIntegration\Monolith.uplugin `
  -Package=<FreshPackage> `
  -TargetPlatforms=Win64 `
  -Rocket
```

| Engine | Package | Editor | Development | Shipping | UAT result |
|---|---|---|---|---|---|
| UE 5.7 | `D:\P4\MonolithOriginReviewFixExact39e9ba81UE57Package` | PASS | PASS | PASS | `BUILD SUCCESSFUL`, exit 0 |
| UE 5.8 | `D:\P4\MonolithOriginReviewFixExact39e9ba81UE58Package` | PASS | PASS | PASS | `BUILD SUCCESSFUL`, exit 0 |

One earlier UE 5.7 diagnostic command omitted `-TargetPlatforms=Win64`; its
Editor target compiled successfully but UAT then selected unavailable Android
support and exited 6. That package is excluded from evidence. All tabled final
packages were created later from the exact verified source with Win64 explicit.

The first UE 5.7 build after making Chooser always-on failed while compiling
`MonolithChooserAuthoringActions.cpp`: that previously dark translation unit
used `FMonolithJsonUtils::ErrInvalidParams` without including
`MonolithJsonUtils.h`. The direct include was added before both clean final
packages above; the failed diagnostic package is excluded from evidence.

### 3.1 Final editor artifact identities

| Engine | Artifact | Size | SHA-256 |
|---|---|---:|---|
| UE 5.7 | `UnrealEditor-MonolithAsset.dll` | 1,676,288 bytes | `9EB769249237A9B90443268DC78E0EF9B329933CA0C4B2CC51B829CD105144B6` |
| UE 5.7 | `UnrealEditor-MonolithAnimation.dll` | 3,357,696 bytes | `05EA460B0F8B2E555AF333CF4120D3E3A7C2FE06AAE9F7D8AAC1554F02F93495` |
| UE 5.7 | `UnrealEditor-MonolithInterchange.dll` | 457,728 bytes | `36CC8C7C7C358F0AFB7EC7250427A991FE0D8EC26E5CC49257984E8E84C1C537` |
| UE 5.7 | `UnrealEditor-MonolithIndex.dll` | 1,912,832 bytes | `3068D0FAB8D6801D014A9A4161F0D04A748E4F77A223222C567DC6A0C2C1C270` |
| UE 5.8 | `UnrealEditor-MonolithAsset.dll` | 1,579,008 bytes | `56CDB33D0FBCE1AE59652F4B68EC298B1C81604A9318720780AC0762D803E060` |
| UE 5.8 | `UnrealEditor-MonolithAnimation.dll` | 3,169,792 bytes | `6EF43A859FBA9E6B224CB707F66C23EDE5A455C32B97E9A35E407BD595EFCB22` |
| UE 5.8 | `UnrealEditor-MonolithInterchange.dll` | 433,152 bytes | `0CC22A7DF19D673C7FBD4F5EE4D8942EE5FFEADCF2482264410C287D90116FF6` |
| UE 5.8 | `UnrealEditor-MonolithIndex.dll` | 1,827,328 bytes | `29933B7D02D45C9473D4F49342DBFF361EB5287DC6B5AAE271664B9404B4B78C` |

---

## 4. Packaged Minimal-Host Automation

Each final package was copied into a new project-shaped host. The host enabled
only Monolith explicitly, disabled the MCP listener, used `-NullRHI`, and ran
the tests through the packaged editor modules rather than the source worktree.

| Engine | Test | Result | Report SHA-256 |
|---|---|---|---|
| UE 5.7 | `Monolith.Asset.PackageGraph.RegistryAndParamGuards` | 1 succeeded, 0 failed, 0 not run | `EB757184D0A5D78C87DA419EB8DE70EBF45FAB2924F3F6D7865AE2B7DBED2546` |
| UE 5.7 | `Monolith.Interchange.ExportTransaction` | 1 succeeded, 0 failed, 0 not run | `81243F1CA3F577620C09BA04C94663BB22312BCD75EA33E36B9B32D239CCFA72` |
| UE 5.7 | `Monolith.ParamGuard.MonolithInterchange.ImportRejectsMalformedParams` | 1 succeeded, 0 failed, 0 not run | `D681D70857D8D5C4F8AE5F15A191677AE742253CA8C14C44849546B878E546D5` |
| UE 5.7 | `Monolith.ParamGuard.Animation.ValidateChooser` | 1 succeeded, 0 failed, 0 not run | `95E0D0C09E0B47637CFCC80D62A0A271195398E1F13712D3E27DDE04C04D3961` |
| UE 5.8 | `Monolith.Asset.PackageGraph.RegistryAndParamGuards` | 1 succeeded, 0 failed, 0 not run | `5EA1DB88B84C02EE8793D7931E2E79AE2CBD19A925FE0A1B2B195490911F075F` |
| UE 5.8 | `Monolith.Interchange.ExportTransaction` | 1 succeeded, 0 failed, 0 not run | `FC3B7EF0F80380EDCE26BCFEF2FC39FF440BC93C378BDA16C898D29DEEC6C0D0` |
| UE 5.8 | `Monolith.ParamGuard.MonolithInterchange.ImportRejectsMalformedParams` | 1 succeeded, 0 failed, 0 not run | `2DD89DF8E1852284558D80146B59F8E018C335623359721AB9D4063A4180CFB2` |
| UE 5.8 | `Monolith.ParamGuard.Animation.ValidateChooser` | 1 succeeded, 0 failed, 0 not run | `ED43F6D7E0AB23302DB3E7E2144A87F392546FAAADB877B6640E60C46C284A7B` |

Report locations:

- `D:\P4\MonolithOriginReviewFixExact39e9ba81UE57AutomationHost\Saved\AutomationReports`
- `D:\P4\MonolithOriginReviewFixExact39e9ba81UE58AutomationHost\Saved\AutomationReports`

All eight selected logs contain GameplayAbilities and Chooser mount records and contain
zero `Plugin 'Monolith' failed`, Monolith DLL load failure, or Monolith DLL
preload failure records.

One preliminary UE 5.8 package from an earlier review-fix head compiled, but
the new strict-save test classified a file occupying the required parent path
as `destination_not_writable` instead of the more exact
`save_directory_unavailable`. The action still made zero changes and left the
package clean. The implementation was corrected to detect that file/directory
shape explicitly, then both exact-head packages and all tabled reports were
regenerated; the preliminary package is excluded from evidence.

The first attempt to run UE 5.7 and UE 5.8 commandlets concurrently produced
one UE 5.7 exit 3 before test discovery: both installed engines switched the
shared Zen DDC service at the same time, temporarily leaving UE 5.7 with no
writable DDC node. The engines were then run sequentially without changing the
source or cache configuration; all six tabled final reports passed. The
environment-failed invocation is excluded from the report table.

---

## 5. Static and Repository Gates

| Gate | Result |
|---|---|
| `python Scripts/ci_static_checks.py selftest` | PASS, including exactly-one required-plugin, conflicting duplicate, and case-insensitive forbidden-optional regression mutations |
| Full hosted static checker | 8 blockers / 24 advisories, byte-for-byte category baseline already present on clean origin |
| New `uplugin-dependency` findings | 0 |
| `git diff --check` | PASS before verification-document commit |

The eight existing blockers are benchmark/environment and release-artifact
prerequisite failures: several benchmark suites assume a project-shaped root
with exactly one `.uproject`, and offline inventory expects an ignored
`Binaries\monolith_query.exe`. This integration does not claim the full static
suite green; it claims and verifies no blocker delta from the clean origin
baseline.

---

## 6. Visual and Discord Boundary

Screenshot verification is **N/A**. This integration changes editor-side C++,
filesystem/import safety, module dependency descriptors, tests, CI contracts,
and documentation. It has no gameplay, runtime UI, editor UI, VFX, animation,
material, or other visual-presentation change.

Discord screenshot upload is **N/A**. No meaningful PC 1920x1080 artifact
exists for this source/API-only change, so
`Build\BatchFiles\Script\UploadScreenshotTestsToDiscord.bat` was not invoked.

---

## 7. Conclusion

The selected fork reliability fixes are compatible with current origin after
preserving origin's stronger proxy ownership and schema-validation boundary.
Clean UE 5.7 and UE 5.8 package builds, packaged-host module loading, portable
export transaction tests, a real staged Texture2D export, strict saveability
preflight, and the always-on Chooser schema guard all pass. Release builds
retain the ten Chooser actions through PoseSearch's required dependency edge.
The integration is suitable for exact-head review and merge; it is not a
wholesale fork-master merge.
