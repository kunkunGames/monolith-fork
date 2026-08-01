# Upstream v0.22 Fork Sync Verification

**Date:** 2026-08-01
**Target repository:** `kunkunGames/monolith-fork`
**Target base:** `ff4c4aec1a263353b1ae4acbc334fc711d5a351e` (`contrib/master`)
**Upstream:** `tumourlove/monolith@e67544caa9e11569e87c1a4f616568544822d8b6` (`v0.22.0`)
**Integration commit:** `fb0e9776d8cb77a1eb19369fbc18f85255dd869b`
**Scope:** High-ROI upstream integration, merge-conflict resolution, cross-engine compatibility hardening, focused automation, and release-gate verification
**Engines:** Unreal Engine 5.7 and 5.8, resolved from isolated hosts' `EngineAssociation`
**Status:** PASS; implementation published and record finalized

---

## 1. Goal and Acceptance Boundary

Integrate the complete coherent upstream `v0.22.0` release into the public fork without discarding the fork's newer namespace work or hiding compatibility failures behind fallbacks. The accepted result must:

- preserve both parents in a real merge commit;
- retain the fork's 26-plus namespace surface while accepting upstream indexing, search, HTTP lifecycle, Blueprint grammar/resolver, risk-discovery, release-gate, and animation improvements;
- compile the same final implementation on UE 5.7 and UE 5.8 with strict, non-unity, warnings-as-errors builds;
- pass focused automation for the affected contracts on both engines;
- produce a fresh offline query executable and pass the Windows PowerShell 5.1 release-body self-test;
- add no static-analysis blocker relative to the previous fork `master`; and
- leave the primary Speed/Monolith checkout, its Perforce changelists, and its externally owned editor process untouched.

The entire upstream release was chosen instead of cherry-picking isolated commits because the ten upstream commits form one release unit: later recovery, release, and animation changes depend on the earlier search, HTTP, and Blueprint contract work. A partial port would create a new divergence point and duplicate future conflict cost.

---

## 2. Integration and Conflict Resolution

The repositories diverged from common base `18dbf57d81666980fe79ec3e25d85cdf8e4f4aa4`: the fork was 16 commits ahead and upstream was 10 commits ahead. The integration commit has target base `ff4c4aec...` as first parent and upstream release `e67544ca...` as second parent.

| Conflict or pre-existing defect | Resolution | Contract retained |
|---|---|---|
| `CHANGELOG.md` | Kept the fork's Unreleased work above upstream's promoted `0.22.0` release | Both post-fork work and the upstream release history remain visible |
| `Docs/API_REFERENCE.md` | Combined upstream animation-layer additions with the fork's broader namespace/API roster | Neither the animation API nor fork-only namespaces are lost |
| `Docs/SPEC_CORE.md` | Retained upstream animation behavior and fork Chooser coverage | Core documentation matches the combined module surface |
| `Docs/specs/SPEC_MonolithIndex.md` | Kept the upstream project-index recovery contract and the fork collection/index schema additions; schema remains v3 | Interrupted-index recovery and fork collection behavior coexist |
| `Monolith.uplugin` | Promoted version metadata to `0.22.0`, retained the fork's 26-plus namespace description, and removed a duplicate module-object `Name` key | The descriptor parses strictly and contains one `MonolithInterchange` and one `MonolithGameplayMessage` module |

The duplicate `Name` field was a real defect on the fork base, not an upstream behavior choice: strict UBT descriptor parsing treated one JSON object as both `MonolithGameplayMessage` and `MonolithInterchange`. Removing the redundant key repairs the source descriptor rather than weakening validation.

---

## 3. Cross-Engine Root Fixes

| Area | Failure exposed by verification | Root fix |
|---|---|---|
| Asset lifecycle diagnostics | UE 5.7 rejected a conditional format string passed to checked `FString::Printf` | Use a compile-time literal at each branch while keeping the same diagnostic fields |
| Optional GameFeatures tests | `GameFeatureData.h` was included even when `WITH_MONOLITH_GAMEFEATURES` disabled the optional dependency | Move the include inside the same feature guard as the tests |
| UE 5.8 deprecations | Strict warnings-as-errors rejected APIs retired or changed in 5.8 | Add narrow engine-version branches for object flags, post-engine-init delegate access, texture sampler inference, MirrorTable update, PoseSearch mutation, IK runtime settings, and linker-mismatch load flags |
| UE 5.7 compatibility | A one-engine-only modernization could silently raise the minimum engine version | Keep the original 5.7 calls below the explicit 5.8 boundary and build both engines from the same source |
| Release-body self-test | Windows PowerShell 5.1 evaluated `$PSScriptRoot` as empty inside a parameter default before `-SelfTest` could run | Resolve the default artifact directory after parameter binding, so the self-test is independent of invocation location |

No runtime asset fallback, alternate engine checkout, silent legacy path, or relaxed compiler setting was introduced.

---

## 4. Protected Strict Build Results

Each isolated host carries an exact copy of the protected strict wrapper and its three helper scripts. This makes the wrapper derive `PROJECT_ROOT`, mutex identity, and writable-output checks from the isolated host instead of the active Speed checkout. Setting only `UE_PROJECT` was insufficient because the Speed wrapper correctly continued to protect Speed's own 68 loaded editor DLLs.

| Gate | UE 5.7 | UE 5.8 |
|---|---|---|
| Host | `Saved\BuildHosts\Monolith-fork-upstream-v0-22-UE57` | `Saved\BuildHosts\Monolith-fork-upstream-v0-22-UE58` |
| Engine resolution | Host `EngineAssociation=5.7` | Host `EngineAssociation=5.8` |
| Build mode | `-OverrideBuildEnvironment -Strict -WarningsAsErrors -DisableUnity -NoUBTMakefiles` | `-OverrideBuildEnvironment -Strict -WarningsAsErrors -DisableUnity -NoUBTMakefiles` |
| Full implementation compile | PASS; the complete module surface compiled before the final affected-source relink | PASS, 516/516 actions, 233.19 seconds |
| Final exact-source relink | PASS, 40/40 affected actions | Included in the 516/516 full build |
| Monolith DLL surface | PASS, all 27 plugin modules linked | PASS, all 27 plugin modules linked |
| Warnings-as-errors result | 0 build failures | 0 build failures |

Representative final DLL hashes prove engine-specific linkage rather than reuse of stale binaries:

| Module | UE 5.7 SHA-256 | UE 5.8 SHA-256 |
|---|---|---|
| `UnrealEditor-MonolithAnimation.dll` | `65EC53...BD2BB` | `9BB20F...A510A` |
| `UnrealEditor-MonolithBlueprint.dll` | `57B456...D0C44` | `C03E6F...26A15` |
| `UnrealEditor-MonolithIndex.dll` | `20D807...2A96B` | `819811...911A` |
| `UnrealEditor-MonolithUI.dll` | `108B02...3DC1` | `8952D2...0C6CB` |

The abbreviated hashes are recorded only as human-readable evidence; build success and output timestamps were verified against the detached source worktrees at the exact integration commit.

---

## 5. Focused Automation Results

Both engines ran this exact combined filter:

`Monolith.PinTypeGrammar+Monolith.Discover.Terse+Monolith.GameFeatures+Monolith.Index.Recovery+Monolith.ProjectSearch+Monolith.Source.Indexer.WriterOpenFailureBroadcastsCompletion+Monolith.PCG.GraphAuthoring.GraphContents.LargeGraphBoundedPersistentComparison+MonolithUI.CreateAnimationV2.Basic`

| Gate | UE 5.7 | UE 5.8 |
|---|---|---|
| Tests performed | 23 | 23 |
| Clean successes | 22 | 22 |
| Success with warnings | 1 | 1 |
| Failures / not run / still running | 0 / 0 / 0 | 0 / 0 / 0 |
| Queue and process exit | Empty queue; `TestExit` exit 0 | Empty queue; `TestExit` exit 0 |
| Report | `Saved\Automation\MonolithForkUpstreamV022\final-469dd656-UE57\index.json` | `Saved\Automation\MonolithForkUpstreamV022\final-469dd656-UE58-mcp-disabled\index.json` |

The one warning-success on each engine is the existing PCG large-graph bounded-comparison test. Its 14 warnings describe deliberate action overwrite and AssetRegistry cleanup exercised by the test; the result itself is successful and no test was suppressed.

The UI animation test generated one `WBP_AnimCoreTest.uasset` fixture per host. Both host `Content` trees were returned to zero files. The two fixtures were moved recoverably to `Saved\Automation\MonolithForkUpstreamV022\residual-fixtures` instead of being destructively deleted.

---

## 6. Release and Offline-Tool Gates

| Gate | Result |
|---|---|
| `Tools\MonolithQuery\build.bat` | PASS under the VS2022 developer environment |
| `monolith_query.exe` size | 2,307,584 bytes |
| `monolith_query.exe` SHA-256 | `EED5745A8CF63CD8024C8D0C72E194077A1EEE59C90B04D1F8125CFCBDFB90D1` |
| Version report | Plugin `0.22.0`; parity `2026-05-29.1`; embedded source hash `a3f38aa0df5f1515` |
| Offline executable freshness | PASS; current source hash and executable hash both `a3f38aa0df5f1515` |
| Windows PowerShell 5.1 release-body self-test | PASS; 7 rejection fixtures and 2 acceptance fixtures |

The PowerShell fix was validated with Windows PowerShell 5.1, not only PowerShell 7, because release operators can invoke the checked-in `.ps1` through the legacy host.

---

## 7. Static Differential and Hygiene

The target repository does not carry the local aggregate checker/configuration, so the existing Speed checker was used read-only against exact base and final worktrees. Only its incompatible offline-executable freshness probe was disabled; the dedicated freshness gate in Section 6 ran independently against the final source.

| Metric | Fork base `ff4c4aec...` | Integration `fb0e9776...` | Delta |
|---|---:|---:|---:|
| Blockers | 37 | 37 | 0 |
| Advisories | 954 | 961 | +7 |

The seven new advisories are CRLF text-hygiene observations for seven new C++ test/resolver/grammar files materialized in this Windows worktree. They add no behavior, schema, security, or compile blocker; the Git blobs remain normalized. Broad line-ending normalization was intentionally excluded because it would expand the merge beyond the release's functional scope.

Additional final hygiene gates:

- `git diff --check` reports no whitespace errors;
- no unresolved merge markers remain;
- the descriptor contains unique module keys;
- upstream `e67544ca...` is an ancestor of the integration head; and
- the integration worktree is clean after committing this record.

---

## 8. Ownership, Runtime, and Visual Boundaries

| Boundary | Result |
|---|---|
| Primary `Plugins\Monolith` checkout | Preserved at `c745cc4e...` on `jules/codex/monolith-source/retire-graph-db` |
| User source modification | Preserved byte-for-byte: `Source\MonolithIndex\MonolithIndex.Build.cs`, SHA-256 `3B62C78BA4670DDE814C6C5B98FE86A87D5F1DF70383DAA3A6422FC3F2C86393` |
| Existing Perforce work | Preserved in CL1325; no task file was reopened or moved |
| Externally owned Speed editors | No process was killed and no running-editor override was used. Visible PID 60548 remained alive through validation and was absent at the post-publication audit; the separately owned headless MCP editor remained outside this task's scope |
| Task delivery ownership | Git-only isolated worktrees under `Saved`; no task file is opened in Perforce |
| Visual/PC1080 verification | N/A: source/release integration changes have no gameplay, UI presentation, level, material, VFX, or editor-tool visual acceptance surface |
| Discord screenshot upload | N/A for the same non-visual scope; `UploadScreenshotTestsToDiscord.bat` was not invoked |

---

## 9. Conclusion

The high-ROI release integration was published to `kunkunGames/monolith-fork:master` by normal fast-forward after the remote target head was rechecked against `ff4c4aec...`. The combined source preserves fork functionality, includes all upstream `v0.22.0` work, compiles strictly on both supported engines, passes the focused 23-test matrix on each engine, and adds no static blocker relative to the target base.
