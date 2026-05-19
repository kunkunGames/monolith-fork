# Monolith GameFeatures Read-Only Inspection Verification

Metadata

| Field | Value |
|---|---|
| Date | 2026-05-19 |
| Scope | `gamefeatures.get_status`, `gamefeatures.list_plugins`, `gamefeatures.find_game_feature_data`, `gamefeatures.describe_game_feature_data`, `gamefeatures.validate_plugin` |
| Branch | `codex/gamefeatures-readonly` |
| Engine | UE 5.7 target, UE 5.8 `GameFeaturesToolset` reference |

---

## 1. Coverage

| Area | Result | Notes |
|---|---|---|
| Spec-first contract | PASS | `Docs/specs/SPEC_MonolithGameFeatures.md`, `Docs/SPEC_CORE.md`, and `Docs/API_REFERENCE.md` document the optional namespace before implementation acceptance. |
| UE reference parity | PASS | UE 5.8 `Engine/Plugins/Experimental/Toolsets/GameFeaturesToolset` treats a Game Feature plugin as a descriptor with an enabled `GameFeatures` dependency; Monolith mirrors that contract without linking ToolsetRegistry. |
| Module ownership | PASS | The `gamefeatures` namespace is registered by `MonolithGameFeatures`, not `MonolithIndex` or `MonolithMesh`, matching the optional-plugin domain split used by PCG/Paper2D/Dataflow. |
| Default safety | PASS | `bEnableGameFeatureActions=false` keeps only `gamefeatures.get_status` registered; the four detailed inspection actions require opt-in and editor restart. |
| Read-only scope | PASS | No create, activate, deactivate, overwrite, delete, or plugin descriptor mutation action is registered. |
| Creation guard | PASS | `bAllowGameFeaturePluginCreation` is reported only; creation remains reserved for a later manifest-based slice. |

---

## 2. Verification Gates

| Gate | Command | Status | Notes |
|---|---|---|---|
| Whitespace | `git diff --check` | PASS | No whitespace errors. |
| Static CI | `Scripts\ci_static_checks.py --config .github\monolith-static-ci.json --github check` | PASS | Blocking findings `0`; existing advisory only for external `.claude/agents` prerequisite. |
| UBT plugin build | `UnrealBuildTool.exe UnrealEditor Win64 Development -Plugin="<worktree>\Monolith.uplugin" -WaitMutex -NoHotReloadFromIDE -NoUBTMakefiles` | PASS | `Result: Succeeded`; MassEntity deprecation warnings are pre-existing. |
| Automation attempt | `UnrealEditor-Cmd.exe ... -plugin="<worktree>\Monolith.uplugin" -ExecCmds="Automation RunTests Monolith.GameFeatures; Quit"` | NOT RUN | The rewritten module was validated with static checks and full UBT. Standalone `-plugin=` automation remains lower confidence because prior attempts in this worktree hit the existing `MonolithBABridge` standalone-load limitation before tests launched. |

---

## 3. Contracts

| Contract | Result |
|---|---|
| `gamefeatures.get_status` reports mode, flags, module load state, scan roots, registered actions, actions available after opt-in, and reserved creation actions | PASS |
| `gamefeatures.list_plugins` returns bounded descriptor rows and redacts project/engine paths | PASS |
| `gamefeatures.find_game_feature_data` resolves by `plugin_name` or `asset_path` through AssetRegistry metadata | PASS |
| `gamefeatures.describe_game_feature_data` loads only the resolved `GameFeatureData` asset and returns capped reflected action summaries | PASS |
| `gamefeatures.validate_plugin` checks descriptor, enabled `GameFeatures` dependency, content root, GameFeatureData presence, and creation gate | PASS |
