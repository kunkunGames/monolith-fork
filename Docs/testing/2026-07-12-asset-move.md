# Monolith exact asset move verification

| Metadata | Value |
|----------|-------|
| Date | 2026-07-12 |
| Scope | `asset.move_assets` registration, guards, no-load dry-run, exact cross-mount commit, redirector cleanup, and postconditions |
| Module | `MonolithAsset` |
| Changelist | `1123` |

---

## 1. Contract under test

`asset.move_assets` accepts at most 512 explicit `{source,destination}` package pairs. It has no overwrite or filename fallback, defaults to `dry_run=true`, requires `confirm=true` for mutation, and requires non-empty source/destination allowlists whose checks are package-segment bounded. Dry-run must not call `FAssetData::GetAsset()` and reports hard/soft AssetRegistry package referencer counts. Commit must dispatch one `IAssetTools::RenameAssets` batch and evaluate real AssetRegistry/file/redirector postconditions per row. Redirector cleanup is attempted only after full rename success; global/partial failure preserves redirectors and reports cleanup as skipped.

---

## 2. Automation coverage

| Test | Coverage |
|------|----------|
| `Monolith.Asset.MoveAssets.RegistryAndGuards` | Registration; explicit `track_dirty_packages` policy without transaction wrapping; missing moves and required allowlists; confirm guard; duplicate destination; chain/cycle rejection. |
| `Monolith.Asset.MoveAssets.DryRunDoesNotLoadOrMutate` | Saved and unloaded temporary source; primary asset preflight; required package-root allowlists; hard/soft referencer count fields; `loaded_asset_count=0`; no destination registry asset; source remains unloaded, clean, and unchanged on disk. |
| `Monolith.Asset.MoveAssets.CommitAndCleanupPostconditions` | Confirmed exact cross-mount rename; optional redirector cleanup; source registry absence; destination primary registry asset; non-empty destination package file; moved-count result. |

---

## 3. Verification commands and results

| Gate | Command | Result |
|------|---------|--------|
| Full editor build | Primary `SpeedEditor Win64 Development` UBT command resolved from `Speed.uproject` | Failed outside this action: concurrent `SpeedCoreRuntime`/`LyraEditor` sources have unrelated compile errors, and many versioned prebuilt plugin DLLs are read-only at link time. The first pass also found one action-local invalid include, which was removed. |
| Isolated module compile | Primary UBT command plus `-Module=MonolithAsset -NoLink` | Passed after the review fixes; UBT explicitly compiled both `MonolithAssetMoveActions.cpp` and `MoveAssetsTests.cpp` with no diagnostics. The preceding full pass also compiled `MonolithAssetModule.cpp` and the existing `Module.MonolithAsset.cpp` unity unit before unrelated failures. |
| Focused automation | `Automation RunTests Monolith.Asset.MoveAssets` in an existing editor session | Not run in this source-only pass because the current task explicitly forbids launching or recovering the editor. |
| Runtime catalog | `monolith_discover(namespace="asset", action="move_assets", mode="schema")` | Pending a live editor reload; the current endpoint is intentionally not started/recovered. |
| Perforce | `p4 opened -c 1123` | Passed for the action source/header, registration, automation tests, API/spec/skill docs, and this verification record. Unrelated files already present in CL1123 were preserved. |

---

## 4. Pass criteria

The build must compile `MonolithAsset` and its automation tests. A later live-editor verification passes only when all three focused tests pass, discovery reports the exact schema/defaults and destructive/non-idempotent metadata, dry-run produces no package/object mutation, and the commit test proves destination and cleanup postconditions. Visual screenshot verification and Discord upload are not applicable because this change adds an editor automation/API surface and no visual presentation.
