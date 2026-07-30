# GameFeatures Action Port Verification

**Date:** 2026-07-29
**Branch:** `jules/codex/gamefeatures/modular-actions`
**Fork base:** `ee1dae25f9a90a45ae768abbfcb0d9356810b0c4`
**Scope:** Optional `MonolithGameFeatures` module and the 15-action upstream `gamefeatures` namespace
**Engines:** Unreal Engine 5.7 and 5.8, resolved from each verification host's `EngineAssociation`

---

## 1. Goal

Verify that the fork gains the current upstream `gamefeatures` action surface
without importing upstream-only action-search/planning metadata,
execution-policy infrastructure, benchmark/logging features, or the later local
`add_action_set_components` action. The accepted surface must:

- add exactly the 15 actions present on GitHub upstream;
- expose 9 actions when the optional dependency is compiled and inspection is
  disabled, and 15 when inspection is enabled;
- fail closed to `get_status` only when `GameFeatures` is disabled;
- compile and pass focused automation on both the UE 5.7 support floor and UE
  5.8.

---

## 2. Catalog and Static Gates

| Gate | Result | Evidence |
|---|---|---|
| Fork-base catalog | PASS | `1561` actions across `24` namespaces from `D:\P4\MonolithForkBaseActionCatalog.json` |
| Feature catalog | PASS | `1576` actions across `25` namespaces from `D:\P4\MonolithForkGameFeaturesActionCatalog.json` |
| Exact delta | PASS | `15` additions, `0` removals; every addition is under `gamefeatures` |
| Registration count | PASS | `15` explicit `RegisterAction(TEXT("gamefeatures"), ...)` calls |
| Exclusion scan | PASS | No `FMonolithActionSearchMetadata`, `FMonolithActionPlanningMetadata`, `FMonolithActionExecutionPolicy`, `EnableValidation`, `GetNamespaceActionCount`, or `add_action_set_components` reference in the port |
| Upstream static checker | PASS | `0` blockers, `0` advisories using `D:\P4\MonolithPortAudit\Scripts\ci_static_checks.py` and a temporary fork-root config |
| Diff hygiene | PASS | `git diff --check` returned no whitespace errors |

The fork does not contain upstream's `Scripts/ci_static_checks.py` or
`.github/monolith-static-ci.json`. The equivalent check therefore ran the
upstream checker against the feature worktree:

```powershell
python D:\P4\MonolithPortAudit\Scripts\ci_static_checks.py `
  --config .github\monolith-static-port-check.json `
  --github check
```

The temporary config enabled module/descriptor mapping, `Build.cs` structure
and release-guard checks, implementation-module coverage, automation-name
uniqueness, duplicate action registration, generated-header order, UTF-8 and
repository hygiene, and workflow-path validation. Benchmark, invocation-log,
proxy, analyzer, offline-parity, skill-drift, and routing-metadata checks were
disabled because those systems are absent from the fork or explicitly outside
this port. The temporary config was removed after the passing run.

---

## 3. Action Delta

| Added action | Mode |
|---|---|
| `gamefeatures.get_status` | Always available, including the dependency-unavailable stub |
| `gamefeatures.add_action_set_input_mapping` | Compiled default writer |
| `gamefeatures.set_primary_asset_scan` | Compiled default writer |
| `gamefeatures.add_game_feature_data_input_mapping` | Compiled default writer |
| `gamefeatures.add_game_feature_data_widgets` | Compiled default writer |
| `gamefeatures.add_game_feature_data_components` | Compiled default writer |
| `gamefeatures.add_game_feature_data_gameplay_cue_paths` | Compiled default writer |
| `gamefeatures.add_game_feature_data_abilities` | Compiled default writer |
| `gamefeatures.remove_game_feature_data_action` | Compiled default writer |
| `gamefeatures.list_plugins` | Inspection opt-in |
| `gamefeatures.find_game_feature_data` | Inspection opt-in |
| `gamefeatures.describe_game_feature_data` | Inspection opt-in |
| `gamefeatures.list_action_classes` | Inspection opt-in |
| `gamefeatures.describe_action_set` | Inspection opt-in |
| `gamefeatures.validate_plugin` | Inspection opt-in |

---

## 4. UE 5.7 Verification

The isolated host
`D:\P4\MonolithGameFeaturesUE57Host\MonolithGameFeaturesUE57Host.uproject`
explicitly enables `Monolith` and `GameFeatures`, sets
`bEnableGameFeatureActions=true`, and resolves engine `5.7` from its project
association. `MONOLITH_RELEASE_BUILD` was unset so the functional dependency
path was compiled.

| Gate | Result | Evidence |
|---|---|---|
| Fresh Editor build | PASS | `439` build actions; `Result: Succeeded` |
| Module startup | PASS | `MonolithGameFeatures: Loaded (15 actions, inspection=enabled)` |
| Focused automation | PASS | `Monolith.GameFeatures.StatusAndReadOnlyGuards`, `1/1` success, `0` warnings, `0` errors |
| Report | PASS | `D:\P4\MonolithGameFeaturesUE57Host\Saved\Automation\GameFeatures-20260729-231412\index.json` |
| Core DLL | PASS | `1072128` bytes; SHA-256 `8AA424FACB21ADE3683CE7C7DE2CE182859E7DC3F320132A2087A181795934D4` |
| GameFeatures DLL | PASS | `469504` bytes; SHA-256 `E135D68A446026878DBB78CB7C694C3254A62AD1C647690FFFE0B0395C85CBC6` |

---

## 5. UE 5.8 Verification

The isolated host
`D:\P4\MonolithGameFeaturesUE58Host\MonolithGameFeaturesUE58Host.uproject`
uses a detached source worktree whose pre-build patch ID matched the branch
worktree (`765691c91ac3e261e597a6dd70d751c468115469`). Engine `5.8` was resolved
from the host's project association.

| Gate | Result | Evidence |
|---|---|---|
| Fresh full Editor build | PASS | `439` build actions; `Result: Succeeded`; fresh `MonolithGameFeatures` link |
| First functional automation | PASS | `Monolith.GameFeatures.StatusAndReadOnlyGuards`, `1/1` success, `0` warnings, `0` errors |
| First report | PASS | `D:\P4\MonolithGameFeaturesUE58Host\Saved\Automation\GameFeatures-20260729-232524\index.json` |
| Dependency-disabled rebuild | PASS | `-DisablePlugin=GameFeatures` changed the target arguments and rebuilt/relinked the module in `7` actions |
| Dependency-disabled startup | PASS | `MonolithGameFeatures: Loaded (1 actions, inspection=enabled)` |
| Dependency-disabled automation | PASS | `Monolith.GameFeatures.StatusUnavailable`, `1/1` success, `0` warnings, `0` errors |
| Dependency-disabled report | PASS | `D:\P4\MonolithGameFeaturesUE58Host\Saved\Automation\GameFeaturesStub-20260729-232732\index.json` |
| Final functional rebuild | PASS | Removing `-DisablePlugin=GameFeatures` rebuilt/relinked the full module in `7` actions |
| Final functional startup | PASS | `MonolithGameFeatures: Loaded (15 actions, inspection=enabled)` |
| Final functional automation | PASS | `Monolith.GameFeatures.StatusAndReadOnlyGuards`, `1/1` success, `0` warnings, `0` errors |
| Final report | PASS | `D:\P4\MonolithGameFeaturesUE58Host\Saved\Automation\GameFeaturesFinal-20260729-232837\index.json` |
| Final GameFeatures DLL | PASS | `450560` bytes; SHA-256 `B28ABD5EDA57016A06B3FFD1C7C2DF6A423F03EE80BF521D4D12DBB26CED24F4` |

The dependency-disabled DLL was materially different (`94720` bytes, SHA-256
`322226E18B91BDE8F33B7FA75300282DA470D3AD5D539E1713EDCE2BEC67EAA5`),
which confirms that the stub result did not reuse the functional binary.

UE 5.8 emitted pre-existing C4996 deprecation warnings from `MonolithUI` and
platform-SDK availability messages for non-Windows targets. The GameFeatures
automation reports themselves contain zero warnings and zero errors.

---

## 6. Visual and Delivery Scope

| Gate | Result | Reason |
|---|---|---|
| PC 1920x1080 screenshot | N/A | This change adds a headless editor action namespace and no visual, gameplay, UI, VFX, animation, material, or asset-presentation behavior. |
| Discord screenshot upload | N/A | No screenshot artifact is relevant, so `Build\BatchFiles\Script\UploadScreenshotTestsToDiscord.bat` was not invoked. |

---

## 7. Result

The port meets the 1/9/15 registration contract, preserves UE 5.7 and UE 5.8
compatibility, links a fresh functional binary after the stub test, and changes
the generated catalog by exactly the intended 15 upstream actions with no
removals. No security, benchmark, invocation-log, action-metadata, or
reinforcement-learning feature is included.
