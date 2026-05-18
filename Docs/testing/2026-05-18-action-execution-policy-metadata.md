# Action Execution Policy Metadata Verification

**Date:** 2026-05-18 / updated 2026-05-19
**Engine:** Unreal Engine 5.7+
**Scope:** MonolithCore execution policy metadata, policy-gated dirty tracking, transaction wrapping, post-edit validation hooks, developer override, and UE 5.7 compile blockers found during verification.

---

## 1. Spec Source

| Artifact | Purpose |
|----------|---------|
| `Docs/specs/SPEC_MonolithActionExecutionPolicy.md` | Defines registry policy metadata, policy-gated dirty tracking, transaction wrapping, post-edit validator hooks, and developer override behavior. |
| `Docs/specs/SPEC_MonolithCore.md` | Links Core registry and audit behavior to the policy execution slice. |
| `Docs/specs/SPEC_MonolithMesh.md`, `Docs/specs/SPEC_MonolithMaterial.md`, `Docs/specs/SPEC_MonolithIndex.md` | Record UE 5.7 compile rules for high-ROI build blockers found during full HostProject verification. |
| `PRD/AgentIntegrationKitGapSpecs/ApplyToMonolith/41-safe-tool-execution-rollback.md` | Source gap spec that made dirty-package tracking, transaction wrapping, and post-edit validation hooks the next high-ROI steps after policy metadata. |

---

## 2. Verification Results

| Gate | Evidence | Result |
|------|----------|--------|
| Registry default | `FMonolithActionExecutionPolicyDiscoverTest` registers an action without explicit policy and expects `policy_id=read_only`, `defaulted=true`. | Added; compile reached this test file. |
| Discovery output | `FMonolithActionExecutionPolicyDiscoverTest` calls `HandleDiscover` and checks `execution_policy` exists on action rows. | Added; compile reached this test file. |
| Domain catalog output | `FMonolithActionExecutionPolicyDomainCatalogTest` calls `HandleDescribeDomain` and checks policy metadata is present. | Added; compile reached this test file. |
| Audit output | `FMonolithActionExecutionPolicyAuditTest` executes the test action and checks the recent audit row includes policy metadata. | Added; compile reached this test file. |
| Read-only fast path | `FMonolithActionExecutionPolicyAuditTest` checks default actions report `dirty_package_tracking_status=skipped_by_policy`. | Added in second slice; HostProject compile produced `MonolithActionExecutionPolicyTests.cpp.obj`. |
| Policy override | `FMonolithActionExecutionPolicyOverrideTest` updates a known action to `track_dirty_packages` and verifies registry/discovery metadata changed. | Added in second slice; HostProject compile produced `MonolithActionExecutionPolicyTests.cpp.obj`. |
| Post-edit validation override | `FMonolithActionExecutionPolicyOverrideAcceptsValidationTest` updates a known action to `post_edit_validate` and verifies dirty tracking, transaction wrapping, validation, and enforcement flags. | Added in third slice; full HostProject build compiled and linked `MonolithCore`. |
| Legacy validation flag rejection | `FMonolithActionExecutionPolicyOverrideRejectsLegacyValidationFlagTest` rejects `policy.post_edit_validate` so callers cannot silently send an ignored legacy boolean alias. | Added in third slice review follow-up; current HostProject UBT rerun returned up to date / succeeded. |
| Validator hook | `FMonolithActionExecutionPolicyPostEditValidationHookTest` registers pass/fail validators and checks `post_edit_validation_status` in audit output. | Added in third slice; full HostProject build compiled and linked `MonolithCore`. |
| Full project build | `UnrealBuildTool.exe GoGameEditor Win64 Development -Project="D:\P4\game\GO.uproject" -WaitMutex -NoHotReloadFromIDE` | Blocked before C++ compile by duplicate Monolith module rule definitions under `D:\P4\game\Plugins\Monolith-worktrees\slate-readonly`. |
| HostProject compile | `UnrealBuildTool.exe UnrealEditor Win64 Development -Project="D:\P4\game\Saved\MonolithPolicyBuild_20260519_004408\HostProject\HostProject.uproject" -plugin="D:\P4\game\Saved\MonolithPolicyBuild_20260519_004408\HostProject\Plugins\Monolith\Monolith.uplugin" -WaitMutex -NoHotReloadFromIDE` | New/modified Core files compiled and produced objects; build later failed in pre-existing `MonolithMesh` anonymous-namespace helper collisions. |
| MonolithCore targeted compile | Same HostProject UBT command with `-Module=MonolithCore` | New/modified Core files were up to date from the HostProject compile; targeted build stopped in pre-existing `MonolithJsonUtilsTests.cpp` UE 5.7 `GetField` API drift and `MonolithMcpCompatibilityOptionsTests.cpp` anonymous-namespace helper collision. |
| MonolithCore targeted recompile | `UnrealBuildTool.exe UnrealEditor Win64 Development -Project="D:\P4\game\Saved\MonolithPolicyBuild_20260519_010346\HostProject\HostProject.uproject" -plugin="D:\P4\game\Saved\MonolithPolicyBuild_20260519_010346\HostProject\Plugins\Monolith\Monolith.uplugin" -Module=MonolithCore -WaitMutex -NoHotReloadFromIDE` | PASS: `UnrealEditor-MonolithCore.dll` linked after UE 5.7 JSON API and test helper collision fixes. |
| Latest MonolithCore recompile | `UnrealBuildTool.exe UnrealEditor Win64 Development -Project="D:\P4\game\Saved\MonolithPolicyBuild_20260519_010948\HostProject\HostProject.uproject" -plugin="D:\P4\game\Plugins\Monolith\Monolith.uplugin" -Module=MonolithCore -WaitMutex -NoHotReloadFromIDE -NoUBTMakefiles` | PASS: `MonolithExecutionGuardActions.cpp`, `MonolithActionExecutionPolicyTests.cpp`, and linked `UnrealEditor-MonolithCore.dll` after legacy alias rejection follow-up. |
| Full isolated HostProject build | `UnrealBuildTool.exe UnrealEditor Win64 Development -Project="D:\P4\game\Saved\MonolithPolicyBuild_20260519_010948\HostProject\HostProject.uproject" -plugin="D:\P4\game\Saved\MonolithPolicyBuild_20260519_010948\HostProject\Plugins\Monolith\Monolith.uplugin" -WaitMutex -NoHotReloadFromIDE` | PASS: all Monolith modules compiled and linked under UE 5.7. |
| Latest isolated HostProject rerun | Same full isolated HostProject command, resolved through `ResolveUnrealEngine.ps1` from `GO.uproject`. | PASS: `Target is up to date` and `Result: Succeeded`. |

---

## 3. Notes

- Full isolated HostProject verification excludes local sibling worktrees and CRG scratch content, so it validates the Monolith plugin itself without the project-level duplicate `Build.cs` blocker.
- The full project build command against `D:\P4\game\GO.uproject` remains blocked before C++ compile until the local `Plugins\Monolith-worktrees\*` duplicate module-rule directories are removed, moved, or excluded from that project build.
