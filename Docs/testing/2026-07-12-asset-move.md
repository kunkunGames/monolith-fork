# Monolith Exact Asset Move And Redirector Cleanup Verification

| Metadata | Value |
|----------|-------|
| Date | 2026-07-12 |
| Scope | `asset.move_assets`, recoverable `asset.cleanup_moved_redirectors`, hardened `asset.delete_assets(require_source_control=true)`, package-graph cleanup delegation, and exact postconditions |
| Module | `MonolithAsset` |
| Submitted baseline | CL `1136` |
| Current hardening | Pending CL `1135` |
| Status | Passed: focused build, action automation, 196-package dry-run/commit cleanup, and Perforce delete proof; final parent-task changelist audit remains open |

---

## 1. Contract Under Test

`asset.move_assets` accepts at most 512 explicit `{source,destination}` package pairs. It has no overwrite, filename fallback, or silent alternate path; it defaults to `dry_run=true`, requires `confirm=true` for mutation, and requires non-empty package-segment-bounded source/destination allowlists. Dry-run does not call `FAssetData::GetAsset()` and reports hard/soft AssetRegistry package referencer counts. A committed move dispatches one `IAssetTools::RenameAssets` batch and evaluates real destination, source, disk, class, and redirector postconditions per row.

`cleanup_redirectors=true` is nonmodal. After a fully successful rename it captures and revalidates every redirector object in each source package, including Blueprint generated-class/CDO companion rows. The exact requested redirector must target the exact destination object; every companion must be a redirector whose target remains inside the exact destination package. Any non-redirector row, foreign-package companion target, remaining hard/soft referencer, destination drift, or partial rename blocks cleanup. The full object set is submitted once through `asset.delete_assets(require_source_control=true)`, with a maximum of 200 redirector objects rather than 200 packages. `IAssetTools::FixupReferencers` and its modal report are not used.

`asset.cleanup_moved_redirectors` exposes the same cleanup as an idempotent recovery action after a rename has already completed. It accepts exact optional source/destination object paths, permits many source packages to share a destination, validates the complete source-package redirector set without loading in dry-run, and requires an editor game-thread call, a completed AssetRegistry scan, and available source control for mutation. On confirmed mutation, an already-cleaned source succeeds only when Perforce also proves the expected delete or revert-add state; dry-run remains observational. Results preserve partial-failure diagnostics rather than treating a partially deleted batch as success.

---

## 2. Automation Coverage

| Suite / Test | Coverage |
|------|----------|
| `Monolith.Asset.MoveAssets` (6/6) | Action/policy registration, confirmation and path guards, no-load dry-run, source-control failure before rename, captured post-rename redirector cleanup, exact move/recovery postconditions, and explicit CDO warning handling. |
| `Monolith.Asset.CleanupMovedRedirectors` (4/4) | Already-cleaned source-control proof, exact non-leaf object paths, many-to-one destinations, generated companion redirectors, foreign companion target rejection, soft-referencer blocking, committed source-control deletion, and idempotent second cleanup. |
| `Monolith.Asset.PackageGraph.RegistryAndParamGuards` (1/1) | `copy_package_graph_with_strategy(cleanup_redirectors=true)` delegates one exact affected-package batch to `asset.cleanup_moved_redirectors` and never calls modal `FixupReferencers`. |

---

## 3. Verification Commands And Results

| Gate | Command / Evidence | Result |
|------|--------------------|--------|
| Focused module build | Resolver-derived UBT output `Saved\BuildMonolithAsset-multi-redirector-fix-20260712.log` | Passed. `MonolithAsset` and its updated automation sources compiled and linked successfully. |
| Cleanup automation | Run `automation-20260712T084004Z-4A6D044E` | Passed `4/4`, failed `0`. |
| Move automation | Run `automation-20260712T084017Z-DB08EEE0` | Passed `6/6`, failed `0`. |
| Package-graph automation | Run `automation-20260712T084020Z-B7E6C952` | Passed `1/1`, failed `0`. |
| Runtime catalog | Live `asset` discovery/action dispatch after editor reload | Passed. The namespace exposes 20 actions including `move_assets` and `cleanup_moved_redirectors`. |
| Production dry-run | `asset.cleanup_moved_redirectors` over the completed Speed migration | Passed for `196` source packages and `200` resolved redirector objects, with `0` blocked rows and `0` redirector loads. This proves the object-count cap and generated companion rows independently of package count. |
| Production commit | Same exact 196-row request with `dry_run=false`, `confirm=true` | Passed: `196/196` source packages reached source registry/file removal postconditions; `200` redirector objects were submitted through one source-control-required delete batch; `0` rows failed. |
| Perforce delete proof | CL `1135` opened-state inspection | Passed for the migration output: `196` moved source package deletes plus `SpeedMaps.uasset`, `Resources\Icon128.png`, and `SpeedMaps.uplugin` total `199` SpeedMaps deletes in CL `1135`. Final whole-task default-changelist audit remains owned by the parent verification. |

---

## 4. Pass Criteria And Applicability

The root failure is resolved only when cleanup never enters the modal `SFixupRedirectorsReport` path, package and redirector-object counts are reported separately, all source-package rows are revalidated before mutation, the exact source object and every companion target satisfy the destination contract, source control is proven before deletion, and every destination/source registry/file postcondition succeeds. The focused build, `11/11` relevant automation tests, and the real 196-package/200-object cleanup satisfy those criteria.

Visual screenshot verification, GIF generation, and Discord upload are `N/A` for this Monolith API hardening because it changes an editor lifecycle action and does not alter rendered presentation. Speed gameplay and GameFeature migration visuals are tracked separately in `Docs\testing\2026-07-12-speed-gamefeature-split.md`.
