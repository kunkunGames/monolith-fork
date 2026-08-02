# Monolith Fork High-ROI Follow-up Integration Verification

**Date:** 2026-08-03 (KST)

**Origin baseline:** `3683c00e066f312c526a8054477c990519f967ab`

**Verified source head:** `580b70abff7a2d21927ed127899d796686f04563`

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
requires GameplayAbilities to be present with `Enabled=true` and without
`Optional=true`; matching is case-insensitive so a case-only alias cannot evade
the contract. Its self-test contains a mutation case for each regression.

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
| UE 5.7 | `D:\P4\MonolithOriginHighRoiChooserUE57FinalPackage` | PASS | PASS | PASS | `BUILD SUCCESSFUL`, exit 0 |
| UE 5.8 | `D:\P4\MonolithOriginHighRoiChooserUE58FinalPackage` | PASS | PASS | PASS | `BUILD SUCCESSFUL`, exit 0 |

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
| UE 5.7 | `UnrealEditor-MonolithAnimation.dll` | 3,357,696 bytes | `A5CA2693F3A8B5C8FBAB0DF8E761D1C7C2F7E3291A897A9CEC2BE17211B45F89` |
| UE 5.7 | `UnrealEditor-MonolithInterchange.dll` | 457,728 bytes | `A4B773467C709CE9626C808C92EE54EEDD28F0221F0949B51FCB340D07F05FAC` |
| UE 5.7 | `UnrealEditor-MonolithIndex.dll` | 1,912,832 bytes | `451595854B65F7EF5D75C62B3641F4180A643AA120F97B5DA877DC7D26845D50` |
| UE 5.8 | `UnrealEditor-MonolithAnimation.dll` | 3,169,792 bytes | `A3EE2BC169F19EE773A185C0B0A432346A21AD03B3CAE05C75C36E6640CBCE40` |
| UE 5.8 | `UnrealEditor-MonolithInterchange.dll` | 433,152 bytes | `11FAE7FDCA681B081602D4093CB24C0C76EC9A8DCA42AF87C8308652A11DCFD9` |
| UE 5.8 | `UnrealEditor-MonolithIndex.dll` | 1,827,328 bytes | `4ECF41F63939C19A4B2D36B87332EC256B16CFC696F1A77EBACF2D0A0373131F` |

---

## 4. Packaged Minimal-Host Automation

Each final package was copied into a new project-shaped host. The host enabled
only Monolith explicitly, disabled the MCP listener, used `-NullRHI`, and ran
the tests through the packaged editor modules rather than the source worktree.

| Engine | Test | Result | Report SHA-256 |
|---|---|---|---|
| UE 5.7 | `Monolith.Interchange.ExportTransaction` | 1 succeeded, 0 failed, 0 not run | `19803E885FD6283465E88C6A026C4B295B461324375D2D7EBFB412FF0E5D80E3` |
| UE 5.7 | `Monolith.ParamGuard.MonolithInterchange.ImportRejectsMalformedParams` | 1 succeeded, 0 failed, 0 not run | `5C23551B65A2D64C3A808190726CD5E547C7EBDF512AD535F42A89CA9BB4201B` |
| UE 5.7 | `Monolith.ParamGuard.Animation.ValidateChooser` | 1 succeeded, 0 failed, 0 not run | `53ABD15832F2841A392D4DE2087FA976E69506300DAB6B513481C901B5584DA8` |
| UE 5.8 | `Monolith.Interchange.ExportTransaction` | 1 succeeded, 0 failed, 0 not run | `36E0D0C399D26453AB04628B819586716C879E8CEEED41194633C47A9410A416` |
| UE 5.8 | `Monolith.ParamGuard.MonolithInterchange.ImportRejectsMalformedParams` | 1 succeeded, 0 failed, 0 not run | `7609731B46DFC0D5C1994DA79AB0AC5E92EB9D6BF4F573CD3343EC6371D88E21` |
| UE 5.8 | `Monolith.ParamGuard.Animation.ValidateChooser` | 1 succeeded, 0 failed, 0 not run | `0DD583A62F5432C7A05BD6E47E5DA57BFF2A34DBB01060FBC5FEDF676904099C` |

Report locations:

- `D:\P4\MonolithOriginHighRoiChooserUE57FinalAutomationHost\Saved\AutomationReports`
- `D:\P4\MonolithOriginHighRoiChooserUE58FinalAutomationHost\Saved\AutomationReports`

All six selected logs contain GameplayAbilities and Chooser mount records and contain
zero `Plugin 'Monolith' failed`, Monolith DLL load failure, or Monolith DLL
preload failure records.

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
| `python Scripts/ci_static_checks.py selftest` | PASS, including required-plugin and case-insensitive forbidden-optional regression mutations |
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
export transaction tests, a real staged Texture2D export, and the always-on
Chooser schema guard all pass. Release builds retain the ten Chooser actions
through PoseSearch's required dependency edge. The integration is suitable for
exact-head review and merge; it is not a wholesale fork-master merge.
