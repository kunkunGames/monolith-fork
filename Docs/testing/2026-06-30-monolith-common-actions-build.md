# Monolith Common Actions Verification

**Date:** 2026-06-30
**Scope:** GameFeatures write/action-removal actions, generic WorldSettings/DataValidation actions, Enhanced Input mapping clone/idempotence, UIExtension point authoring, PrimaryGameLayout layer-widget authoring, and SourceControl P4 path mapping
**Result:** Compile passed; full link blocked by environment import-library state

---

## 1. Build

Command:

```powershell
$projectRoot = (Get-Location).Path
$uproject = Join-Path $projectRoot "Speed.uproject"
$resolver = Join-Path $projectRoot "Build\BatchFiles\Script\ResolveUnrealEngine.ps1"
$engineRoot = powershell -NoProfile -ExecutionPolicy Bypass -File $resolver -Project $uproject -Output Root
& "$engineRoot\Engine\Binaries\DotNET\UnrealBuildTool\UnrealBuildTool.exe" SpeedEditor Win64 Development "-Project=$uproject" -WaitMutex -NoHotReloadFromIDE -NoLink
```

Result: `Succeeded`.

Follow-up compile after adding `gamefeatures.remove_game_feature_data_action`
and `ui.add_primary_game_layout_layer`: `Succeeded`.

---

## 2. Full Link Attempt

Command:

```powershell
$projectRoot = (Get-Location).Path
$uproject = Join-Path $projectRoot "Speed.uproject"
$resolver = Join-Path $projectRoot "Build\BatchFiles\Script\ResolveUnrealEngine.ps1"
$engineRoot = powershell -NoProfile -ExecutionPolicy Bypass -File $resolver -Project $uproject -Output Root
& "$engineRoot\Engine\Binaries\DotNET\UnrealBuildTool\UnrealBuildTool.exe" SpeedEditor Win64 Development "-Project=$uproject" -WaitMutex -NoHotReloadFromIDE
```

Result: link failed before producing `SpeedEditor` because engine/plugin import libraries such as `UnrealEditor-Engine.lib`, `UnrealEditor-Json.lib`, and `SQLiteCore.lib` were missing from the expected link inputs. A compile error in the new UI schema was found during this run and fixed before the successful `-NoLink` build.

---

## 3. Screenshot Scope

No screenshot capture or Discord upload was required. The change is editor/tooling API surface only and does not alter runtime gameplay, UMG presentation, VFX, materials, animation, or player-facing visuals.
