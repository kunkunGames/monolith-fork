# Monolith Fork High-ROI Follow-up Integration Verification

**Date:** 2026-08-02 (KST)

**Origin baseline:** `3683c00e066f312c526a8054477c990519f967ab`

**Verified source head:** `3eafd563d6832a80f1fd45c750a85914c0c53838`

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
`Optional=true`; its self-test contains a mutation case for each regression.

The final packaged descriptors on both engines contain zero direct Chooser
references and exactly one enabled, non-optional GameplayAbilities reference.
Minimal-host logs confirm that Unreal mounts both Chooser (through PoseSearch)
and GameplayAbilities, with no Monolith module load/preload failure.

### 2.2 Parameter-validation boundary

The fork's Interchange ParamGuard expected malformed optional JSON types to
reach the action handler and return a row-level error. Current origin validates
every declared schema before dispatch. The port keeps that stronger global
contract and updates the focused test to require the registry's
`failure_cause=invalid_param` result naming the exact bad field. The later
native Texture2D PNG export, staged replacement, rollback, and cleanup
assertions remain unchanged.

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
| UE 5.7 | `D:\P4\MonolithOriginHighRoiExactUE57Package` | PASS | PASS | PASS | `BUILD SUCCESSFUL`, exit 0 |
| UE 5.8 | `D:\P4\MonolithOriginHighRoiExactUE58Package` | PASS | PASS | PASS | `BUILD SUCCESSFUL`, exit 0 |

One earlier UE 5.7 diagnostic command omitted `-TargetPlatforms=Win64`; its
Editor target compiled successfully but UAT then selected unavailable Android
support and exited 6. That package is excluded from evidence. All tabled final
packages were created later from the exact verified source with Win64 explicit.

### 3.1 Final editor artifact identities

| Engine | Artifact | Size | SHA-256 |
|---|---|---:|---|
| UE 5.7 | `UnrealEditor-MonolithInterchange.dll` | 457,728 bytes | `B3810D535DDE472B85E28B734F51ADBF8EBC26AA4C59EA0264C63DECBB25B9CE` |
| UE 5.7 | `UnrealEditor-MonolithIndex.dll` | — | `39BBD3E9DCF4370A14CB6B0E4185BAB67EC2AB55016D3732EF05380508957E74` |
| UE 5.8 | `UnrealEditor-MonolithInterchange.dll` | 433,152 bytes | `8A5A52CB581429C94C59D2F5279E49983BD879CDC4A18504140981371CA26EF6` |
| UE 5.8 | `UnrealEditor-MonolithIndex.dll` | — | `00CDEFBF292533C002CA21363845A976064B24E2EC31DCC09EF81C86C8A39B8D` |

---

## 4. Packaged Minimal-Host Automation

Each final package was copied into a new project-shaped host. The host enabled
only Monolith explicitly, disabled the MCP listener, used `-NullRHI`, and ran
the tests through the packaged editor modules rather than the source worktree.

| Engine | Test | Result | Report SHA-256 |
|---|---|---|---|
| UE 5.7 | `Monolith.Interchange.ExportTransaction` | 1 succeeded, 0 failed, 0 not run | `DE5B3FD3D44B639399016763BA903E5618A450189D70689BCFF2092C08BF14CC` |
| UE 5.7 | `Monolith.ParamGuard.MonolithInterchange.ImportRejectsMalformedParams` | 1 succeeded, 0 failed, 0 not run | `EBA08E28B55C66056DC7B09FDB152EFEBB416AD78DECA4E38981B52CF46499C9` |
| UE 5.8 | `Monolith.Interchange.ExportTransaction` | 1 succeeded, 0 failed, 0 not run | `21A83A2203F1D2FD4D247B42125B5C9E0385FAD802FB8DB0B6CBF40F06C87312` |
| UE 5.8 | `Monolith.ParamGuard.MonolithInterchange.ImportRejectsMalformedParams` | 1 succeeded, 0 failed, 0 not run | `5B04512FC3E005376E089F69A2364571DC5E72F53DB4360CC1691405637A4FC9` |

Report locations:

- `D:\P4\MonolithOriginHighRoiExactUE57AutomationHost\Saved\AutomationReports`
- `D:\P4\MonolithOriginHighRoiExactUE58AutomationHost\Saved\AutomationReports`

All four logs contain GameplayAbilities and Chooser mount records and contain
zero `Plugin 'Monolith' failed`, Monolith DLL load failure, or Monolith DLL
preload failure records.

---

## 5. Static and Repository Gates

| Gate | Result |
|---|---|
| `python Scripts/ci_static_checks.py selftest` | PASS, including both plugin-dependency regression mutations |
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
export transaction tests, and a real staged Texture2D export all pass. The
integration is suitable for exact-head review and merge; it is not a wholesale
fork-master merge.
