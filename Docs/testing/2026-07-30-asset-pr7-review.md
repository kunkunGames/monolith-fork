# Asset PR #7 Review Hardening Verification

**Date:** 2026-07-30
**Pull request:** `kunkunGames/monolith-fork#7`
**Branch:** `jules/codex/asset/lifecycle-actions`
**Reviewed head:** `0af08f40e130699549b19160c7b8f979f5f18820`
**Scope:** File-import primitive type safety and mounted soft-reference inspection
**Engines:** Unreal Engine 5.7 and 5.8, resolved from each verification host's `EngineAssociation`

---

## 1. Goal

Close the two automated review findings without weakening the original
20-action exact-path asset contract:

1. malformed `srgb`/`tiling` strings must not pass UE 5.7 boolean coercion and
   reach file-import mutation;
2. missing soft references under `/Engine` and plugin mounts must not be
   reported as existing merely because they are outside `/Game`.

The change must preserve native-boolean imports, real Engine references,
`/Script` class references, and all existing lifecycle/package-graph behavior.

---

## 2. Review Findings and Resolution

| Finding | Root cause | Resolution | Regression coverage |
|---|---|---|---|
| File import accepted string booleans | `ReadBoolSetting` called UE 5.7 `TryGetBool` without checking `FJsonValue::Type` | Require `EJson::Boolean` before reading both nested and compatibility top-level values | Four cases cover nested/top-level `srgb` and `tiling`; every request returns `-32602` and creates no asset |
| Non-`/Game` soft references always appeared to exist | Reference serialization used `/Game ? AssetExists : true` | One shared resolver checks an already loaded object, preserves `/Script` as a non-asset reference, and queries AssetRegistry for every mounted content path | Missing plugin and Engine paths are false; real Engine asset and `/Script/Engine.Texture2D` are true |

The shared resolver is used by both reflected reference rows and property-value
serialization so the two inspection surfaces cannot drift.

---

## 3. Static and Documentation Gates

| Gate | Result | Evidence |
|---|---|---|
| Boolean accessor audit | PASS | Every external `TryGetBool` in `Source\MonolithAsset` now has a native `EJson::Boolean` gate; result-parsing calls are excluded |
| Non-`/Game` shortcut audit | PASS | No `AssetPath.StartsWith("/Game") ? AssetExists : true` pattern remains |
| Diff hygiene | PASS | `git diff --check` returned no whitespace errors |
| API/spec/skill sync | PASS | `Docs\API_REFERENCE.md`, `Docs\specs\SPEC_MonolithAsset.md`, and `Skills\unreal-asset\SKILL.md` describe the corrected contracts |
| Focused tests | PASS | Added `MonolithAsset.ImportTextureFromFile.StrictBooleanSettings` and `MonolithAsset.InspectAsset.MountedSoftReferenceExistence` |

---

## 4. UE 5.7 Verification

The isolated host
`D:\P4\MonolithAssetUE57Host\MonolithAssetUE57Host.uproject` resolves Unreal
Engine 5.7 from its project association and junctions `Plugins\Monolith` to the
review worktree.

| Gate | Result | Evidence |
|---|---|---|
| Final Editor build | PASS | `22/22` actions; `Result: Succeeded`; `13.18` seconds |
| Build log | PASS | `D:\P4\MonolithAssetUE57Host\Saved\Logs\Asset-Review2-UBT-UE57-20260730.log` |
| Full focused automation | PASS | `49/49`: `37` success plus `12` expected-warning success; failed `0`; not-run `0` |
| Report | PASS | `D:\P4\MonolithAssetUE57Host\Saved\Automation\Asset-ReviewFull-UE57-20260730\index.json` |
| Critical log scan | PASS | No fatal error, assertion failure, ensure failure, or unhandled exception |

An initial test-harness attempt derived a test class from MinimalAPI
`UTexture2D`, which required 24 non-exported UE 5.7 virtual symbols and failed
linking with `LNK2001/LNK1120`. The test was redesigned to call the exact
private production resolver through a dev-automation-only hook; no product
fallback, alternate implementation, or unresolved link dependency remains.

---

## 5. UE 5.8 Verification

The independent host
`D:\P4\MonolithAssetUE58Host\MonolithAssetUE58Host.uproject` resolves Unreal
Engine 5.8 from its project association and junctions `Plugins\Monolith` to the
same review worktree.

| Gate | Result | Evidence |
|---|---|---|
| Fresh full Editor build | PASS | `455/455` actions; `Result: Succeeded`; `186.51` seconds |
| Build log | PASS | `D:\P4\MonolithAssetUE58Host\Saved\Logs\Asset-Review-UBT-UE58-20260730.log` |
| Full focused automation | PASS | `49/49`: `37` success plus `12` expected-warning success; failed `0`; not-run `0` |
| Report | PASS | `D:\P4\MonolithAssetUE58Host\Saved\Automation\Asset-ReviewFull-UE58-20260730\index.json` |
| Final module binary | PASS | `1463808` bytes; SHA-256 `DE5A0DE0B18D8E677CC559A44024F238A97A7017836982FA202E05738BF95088` |
| Critical log scan | PASS | No fatal error, assertion failure, ensure failure, or unhandled exception |

UE 5.8 emitted pre-existing cross-platform SDK availability messages and
deprecation warnings outside the changed Asset code. The focused automation
report contains no failed or not-run test.

---

## 6. Live MCP Verification

The UE 5.8 host ran a review-only stateless MCP endpoint on port `9438`.
Readiness was confirmed through `/mcp` initialize with protocol `2025-03-26`
and Monolith `0.21.3`; this fork version does not use `/health` as its
readiness contract.

Discovery occurred before mutation:

- `monolith_discover(asset)` returned exactly `20` actions;
- `describe_query(action_schema)` confirmed exact schemas for
  `import_texture_from_file`, `inspect_asset`, and `delete_assets`;
- complex object/array params reported
  `allow_string_encoded_complex=false`.

| Scenario | Result |
|---|---|
| Nested `settings.srgb="false"` | Explicit `Setting 'srgb' must be a boolean` error; destination remained absent |
| Native boolean import | Exact `/Game/MonolithReview/T_PR7_StrictBool`; 8x8 `Texture2D`; `srgb=false` read back |
| Existing Engine inspection | `/Engine/EngineResources/DefaultTexture.DefaultTexture` loaded and inspected successfully |
| Guarded cleanup preview | `delete_assets(dry_run=true)` found one target and reported `would_delete` without mutation |
| Guarded cleanup commit | Deleted `1/1`; no loaded package, registry row, file, or residual sidecar remained |
| Post-delete readback | `inspect_asset` returned not found and the `.uasset` was absent |
| Shutdown | `QUIT_EDITOR` completed; validation PID exited and port `9438` closed |

Live log:
`D:\P4\MonolithAssetUE58Host\Saved\Logs\Asset-Review-LiveMCP-UE58-20260730.log`.
The temporary MCP enable/port/index settings were restored after shutdown.

---

## 7. Visual and Delivery Scope

| Gate | Result | Reason |
|---|---|---|
| PC 1920x1080 screenshot | N/A | This review changes headless asset parameter validation and read-only reference diagnostics; it has no gameplay, UI, VFX, animation, material, or asset-presentation output. |
| Discord screenshot upload | N/A | No screenshot artifact is relevant, so `Build\BatchFiles\Script\UploadScreenshotTestsToDiscord.bat` was not invoked. |

---

## 8. Result

Both review findings are closed by shared production-path changes with focused
regression coverage. UE 5.7 and UE 5.8 compile and pass the full 49-test Asset
scope, while the live UE 5.8 endpoint proves pre-mutation rejection, valid
native-boolean import/readback, Engine-mounted inspection, guarded deletion,
postcondition cleanup, and clean shutdown.
