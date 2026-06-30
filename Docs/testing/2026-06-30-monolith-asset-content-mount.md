# Monolith Asset Content Mount Registration

Metadata

| Field | Value |
| --- | --- |
| Date | 2026-06-30 |
| Project | Speed |
| Area | MonolithAsset, package graph copy, content mount points |
| Scope | Monolith-only implementation and verification for `asset.register_content_mount_points`. |
| Runtime asset edits | None |

---

## 1. Summary

`asset.register_content_mount_points` adds a guarded Monolith asset action for process-local Unreal content root registration before package graph planning/copying.

The action defaults to `dry_run=true`, requires `confirm=true` for `dry_run=false`, validates every row before mutation, blocks core mount roots and conflicting existing roots by default, and reports rows plus probe-package results. It mutates the current editor process mount table only; it does not edit assets, plugin descriptors, or project config.

---

## 2. Implementation

| File | Change |
| --- | --- |
| `Source\MonolithAsset\Private\MonolithAssetPackageGraphActions.cpp` | Added `asset.register_content_mount_points`, mount-spec parsing, resolver validation for `content_dir`, `plugin_name`, and `project_plugin_dir`, preflight reporting, optional AssetRegistry scanning, and probe-package checks. |
| `Source\MonolithAsset\Public\MonolithAssetPackageGraphActions.h` | Exposed the new action handler. |
| `Source\MonolithAsset\Private\Tests\PackageGraphCopyActionsTests.cpp` | Extended package graph automation to cover action registration, policy, guard rejection, duplicate resolver rejection, same-request root conflict rejection, dry-run non-mutation, project-plugin resolver dry-run through a temporary generic test folder, confirmed registration, idempotent repeat, and conflict rejection. |
| `Source\MonolithAsset\MonolithAsset.Build.cs` | Added the `Projects` dependency for `IPluginManager` resolver support. |

---

## 3. Verification

| Check | Result |
| --- | --- |
| UBT | Passed `SpeedEditor Win64 Development` via `Build\BatchFiles\Script\ResolveUnrealEngine.ps1`; `PackageGraphCopyActionsTests.cpp`, `UnrealEditor-MonolithAsset.lib`, and `UnrealEditor-MonolithAsset.dll` rebuilt with `Result: Succeeded`. |
| Automation | Passed `Monolith.Asset.PackageGraph.RegistryAndParamGuards` with `succeeded=1`, `failed=0`, `warnings=0` in `Saved\Logs\Automation\MonolithAssetPackageGraphMount\index.json`. The latest suite covers same-request root conflict rejection and no longer depends on a Speed-specific `GameFeatures\SpeedMaps` path. |
| Live MCP smoke | Passed live discovery and action smoke through `monolith_query`: `asset` namespace reported 18 actions including `register_content_mount_points`; dry-run returned `status=dry_run`, `ok=true`, and `would_register_count=1`; `dry_run=false` without `confirm=true` returned the expected structured `-32602` guard error. Confirmed mount and idempotent repeat registration are covered by automation. |
| Skill drift | Passed `Scripts\check_skill_catalog_drift.ps1 -Skill unreal-asset` with `RESULT=OK`, `documented_actions=18`, `hard_drift=0`, `gated=0`, `xref=0`, `undocumented=0`. |
| Screenshot/Discord | Not applicable; this is editor tooling with no visual/runtime presentation output. |
