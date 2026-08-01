# Monolith - MonolithAsset Module

**Parent:** [../SPEC_CORE.md](../SPEC_CORE.md)
**Engine:** Unreal Engine 5.7+
**Owner module:** `MonolithAsset`
**Namespace:** `asset`
**MCP tool:** `asset_query`
**Status:** Implemented (2026-07-29 fork port)

---

## 1. Scope

`MonolithAsset` owns generic asset workflows that are not specific to one content domain: Texture2D/font ingest, file texture import, asset save/delete lifecycle operations, exact package moves and source-control-verified redirector cleanup, naming/rename hygiene, typed read-only asset inspection, and guarded package graph copy/remap planning, duplication, reference fixup, and closure validation.

This module keeps asset-level actions out of `MonolithUI`, `MonolithMesh`, and `MonolithMaterial` so those modules can focus on their native domains.

## 2. Namespace Ownership

`FMonolithAssetModule` registers all 20 `asset` actions directly with `FMonolithToolRegistry` and unregisters the `asset` namespace during shutdown. The module is gated by `UMonolithSettings::bEnableAsset`.

No cross-namespace compatibility aliases or alternate legacy handlers are registered. Callers use the canonical `asset.*` actions.

Every `asset` action schema opts into `FParamSchemaBuilder::StrictComplexTypes()`: arrays and objects must arrive as native JSON values and are never recovered from JSON-encoded strings. Handlers also require exact JSON boolean values before calling `TryGetBoolField`, because UE 5.7 otherwise coerces `"true"` and `"false"` strings. Type violations return JSON-RPC invalid params (`-32602`) before any asset mutation.

## 3. Registered Actions

| Action | Owner class | Purpose |
|--------|-------------|---------|
| `import_texture_from_bytes` | `MonolithAsset::FTextureIngestActions` | Decode bounded base64 compressed image bytes and create or replace a `UTexture2D` under `/Game/...`; `conflict_policy=fail|replace|unique` defaults to exact-path `fail`, while optional `texture_role` applies Unreal texture-role import presets, role post-processing, validation metadata, and optional postprocessed PNG return. |
| `import_font_family` | `MonolithAsset::FFontIngestActions` | Import one or more absolute `.ttf` files as a composite `UFont` plus `UFontFace` assets. Every source and requested package is preflighted before package creation; exact names are the default, while `allow_unique_names=true` explicitly opts into suffixed names. Unknown loading/hinting values and malformed booleans are errors. Headless runs skip only Slate preview-cache refresh calls. |
| `import_texture_from_file` | `FMonolithAssetLifecycleActions` | Import an external image file to the exact requested `UTexture2D` path with validated compression, sRGB, tiling, max-size, and LOD-group settings. Nested and compatibility top-level sRGB/tiling values must be native JSON booleans. The action rejects malformed or duplicate setting sources and never returns a source-basename substitute; unexpected newly imported packages are cleaned and reported as an error. |
| `save_asset` | `FMonolithAssetLifecycleActions` | Save any loaded asset package to disk, enforce clean/on-disk/non-empty-file postconditions, and optionally prove persistence through a non-interactive package reload. |
| `delete_assets` | `FMonolithAssetLifecycleActions` | Delete assets by path with segment-bounded prefix guards, per-target postconditions, dry-run validation, non-interactive editor closing, optional source-control-aware disk-residual removal, and an all-target `require_source_control` preflight. |
| `move_assets` | `FMonolithAssetMoveActions` | Guarded exact package moves through `IAssetTools::RenameAssets`; defaults to a no-load dry-run, never overwrites, rejects chains/cycles, verifies destination/source/redirector postconditions, and can explicitly accept only the known CDO/config warning in an interactive editor. |
| `cleanup_moved_redirectors` | `FMonolithAssetMoveActions` | Idempotently finalize completed moves without another rename or reference rewrite by validating exact object targets, every companion redirector in each source package, zero hard/soft referencers, destination integrity, source-control state, and source-removal postconditions. |
| `validate_naming_conventions` | `FMonolithAssetHygieneActions` | Scan assets under a content path and report prefix-rule violations. |
| `list_supported_asset_enrichers` | `FMonolithAssetInspectionActions` | List typed read-only enrichers supported by `inspect_asset`. |
| `inspect_asset` | `FMonolithAssetInspectionActions` | Inspect one asset with typed enrichment and optional reflected references. Soft-reference existence is resolved from loaded objects or the live AssetRegistry for project, engine, and plugin mounts; only `/Script` class paths bypass asset lookup. |
| `inspect_assets_batch` | `FMonolithAssetInspectionActions` | Inspect multiple assets with per-row success/error results. |
| `validate_typed_asset` | `FMonolithAssetInspectionActions` | Validate a typed asset and report warnings without mutation. |
| `batch_rename_assets` | `FMonolithAssetHygieneActions` | Preview or apply batch asset renames through `IAssetTools::RenameAssets`. |
| `find_assets` | `FMonolithAssetFindActions` | Fuzzy, scored, typo-tolerant search over the live `AssetRegistry`; ranks by asset name/path/class (and optional tags) via the shared MonolithCore `FMonolithFuzzyMatch` engine, with `allow_transposition` controlling Damerau adjacent-swap tolerance. |
| `register_content_mount_points` | `FMonolithAssetPackageGraphActions` | Guarded process-local content mount registration through `FPackageName::RegisterMountPoint`; defaults to `dry_run=true`, requires `confirm=true` for mutation, and resolves exactly one of explicit `content_dir`, loaded `plugin_name`, or `project_plugin_dir` specs. |
| `plan_package_graph_copy` | `FMonolithAssetPackageGraphActions` | Read-only AssetRegistry traversal that emits a source-to-destination package plan, dependency edges, external dependencies, and destination collisions from explicit root remaps. |
| `copy_package_graph_with_remap` | `FMonolithAssetPackageGraphActions` | Guarded package graph duplication through `IAssetTools::DuplicateAsset`; requires `dry_run=true` or `confirm=true`, never overwrites existing destinations, and can save duplicated packages. |
| `copy_package_graph_with_strategy` | `FMonolithAssetPackageGraphActions` | Guarded package graph workflow orchestrator. It emits `strategy_plan[]` before mutation, executes `duplicate_asset`, `advanced_copy`, `header_patched_advanced_copy`, and opt-in `raw_package_file_copy` rows, and reports manual strategy rows as explicit unsupported blockers rather than silently falling back. |
| `fixup_copied_references` | `FMonolithAssetPackageGraphActions` | Guarded reflected hard/soft object reference rewrite inside copied destination packages from source roots to root-remapped destination roots; requires `dry_run=true` or `confirm=true`. |
| `validate_dependency_closure` | `FMonolithAssetPackageGraphActions` | Read-only validation that destination packages do not depend on disallowed external or legacy source roots after a copy/remap plan. |

## 4. Build.cs Dependencies

| Scope | Modules |
|-------|---------|
| Public | `Core`, `CoreUObject`, `Engine`, `MonolithCore` |
| Private | `UnrealEd`, `Json`, `JsonUtilities`, `AssetRegistry`, `AssetTools`, `EditorScriptingUtilities`, `ImageWrapper`, `ImageCore`, `Projects`, `RenderCore`, `RHI`, `SourceControl`, `Slate`, `SlateCore` |

## 5. Asset Persistence and Delete Contracts

### 5.1 Texture-byte collision policy

`asset.import_texture_from_bytes` accepts `conflict_policy=fail|replace|unique`, defaulting to `fail`:

| Policy | Contract |
|--------|----------|
| `fail` | The requested package path is exact. If that package already exists on disk or in memory, the action returns an invalid-params error and never creates a suffixed asset. |
| `unique` | The only policy allowed to call `IAssetTools::CreateUniqueAssetName`; `asset_path` may differ from `requested_asset_path`. |
| `replace` | If the exact package exists, it must resolve to the exact top-level `UTexture2D`. The action updates that same `UTexture2D` and `UPackage` identity in place; redirect resolution, a missing top-level asset, or another asset class is rejected. If no package exists, the exact requested path is created. |

In-place replacement completes independently fallible image processing (including optional processed-PNG encoding) and builds the new platform data before touching the existing texture. Before mutation it moves the complete original `FTextureSource` object into an armed RAII snapshot, preserving its editor bulk-data payload and identifiers, package/virtualization attachment, compression form, long-lat flag, and blocked/layered topology without reconstruction. It also snapshots package dirty state, the six caller-facing settings (`CompressionSettings`, `SRGB`, `MipGenSettings`, `LODGroup`, `AddressX`, `AddressY`), and the additional values that Unreal can normalize or regenerate during `PostEditChange`/save, then transfers ownership of the complete running and cooked `FTexturePlatformData` state. The platform transfer preserves mip bulk/derived-data handles, VT data, CPU copies, encoder metadata, hashes, and DDC/fetch keys instead of reconstructing only pixel bytes. Source/platform replacement uses the normal texture edit notification, while each caller-facing setting is dispatched through its own property-specific `PreEditChange` / `PostEditChangeProperty` pair so Unreal's per-property invalidation paths run.

Byte imports use the engine's padding-aware exact base64 size calculation before allocating the decoded buffer, then use the `IImageWrapper` header contract after `SetCompressed` to inspect width and height before `GetRaw` performs decompression. Compressed input is capped at 256 MiB, each axis at 16,384 pixels, and the expected BGRA8 surface at 512 MiB. These limits are fail-closed `-32602` validation errors; the importer never decodes, creates a package, or mutates an existing texture when a limit is exceeded. Failed-new-texture rollback uses `GARBAGE_COLLECTION_KEEPFLAGS`, preserving unrelated standalone editor assets while the detached failed texture is collected.

The replacement snapshot remains armed until the operation succeeds. A save failure or later early return restores the same texture/package identity, exact source object state, complete running/cooked platform ownership, texture/editor side-effect values, CPU-copy helper identity, settings, material notifications, and dirty state, then verifies those rollback postconditions; an incomplete rollback is surfaced in the error. A texture whose original running platform data is null is restored to null rather than rejected or synthesized. Replacement is rejected before mutation only when ownership cannot be transferred safely: an active running/cooked platform-data build, non-null `ResourceMem`, or aliased/duplicate cooked platform-data ownership. Replacement-created cooked data is unique-deleted while preserving a running allocation even if an anomalous alias appears. Failure after creating a new texture unregisters and detaches the object/package and removes the header plus package sidecars. `AssetCreated` is called only for a newly created asset. The response reports `requested_asset_path`, resolved `asset_path`, `created`, `replaced`, and normalized `conflict_policy` in addition to the image/settings/validation fields.

### 5.2 Save postconditions and reload verification

`asset.save_asset` always verifies that the package is clean after save, that its package file exists, and that the file size is greater than zero. The response reports the canonical `asset_path`, `package_name`, `class`, `saved`, `was_dirty`, `dirty_after_save`, `exists_on_disk`, `filename`, and `file_size`.

`verify_reload=true` adds a non-interactive `UPackageTools::ReloadPackages` proof after the save. The action re-resolves the canonical asset path and requires the class to match and the reloaded package to remain clean; it reports `verify_reload`, `reloaded`, and `reloaded_class`. Reload verification rejects both `UWorld` objects and every package for which `UPackage::ContainsMap()` is true, as well as assets with an open editor, because unloading or replacing those live objects is unsafe.

### 5.3 Delete target and residual postconditions

`asset.delete_assets` requires `asset_paths` to be a non-empty array of non-empty strings. When `allowed_prefixes` is present, it must also be a non-empty array whose every entry is a non-empty string; malformed members fail the whole request instead of silently disabling the guard.

Committed deletion runs non-interactively. For each loaded target it clears the package dirty flag and closes open asset editors before deleting under an unattended-script guard. Garbage collection uses Unreal's editor `GARBAGE_COLLECTION_KEEPFLAGS`, not `RF_NoFlags`: deleted targets have already been detached or marked as garbage, while unrelated `RF_Standalone` assets and their unsaved edits remain alive across the target eviction. `force=false` performs the normal object deletion path. `force=true` also handles packages that have no loaded object or AssetRegistry row, including disk-only package files. `dry_run=true` is observational: it does not clear dirty state, close editors, unregister string tables, modify packages, invoke object deletion, issue source-control operations, delete files, or rescan the registry.

`allowed_prefixes` is a package-segment guard, not a raw string prefix: a target must equal an allowed package prefix or begin with `prefix + "/"`. For example, `/Game/Foo` permits `/Game/Foo/Asset` but rejects `/Game/FooSibling/Asset`. Trailing slashes and object-path forms are normalized before comparison.

The force path checks binary asset/map and text asset/map headers (`.uasset`, `.umap`, `.utxt`, `.utxtmap`) plus Unreal package segments for exports and bulk/payload data (`.uexp`, `.ubulk`, `.uptnl`, `.m.ubulk`, `.upayload`). Added or checked-out files are reverted as required, tracked files are marked for delete, and only source-control-module-disabled writable files or provider-confirmed untracked writable files may be deleted directly. If source control is enabled, provider/state unavailability or a failed/unsupported revert/delete operation blocks direct deletion, sets that target's `source_control_failure`, and makes the target fail even if the filesystem otherwise appears clean. `require_source_control=true` performs the provider and every-file state/transition preflight before the first mutation; any unavailable, unknown, unsupported, or unverifiable state fails the entire request so callers such as redirector cleanup cannot obtain a partial filesystem-only result.

Each non-dry-run target succeeds only when all three final postconditions hold: no loaded package, no AssetRegistry entries for the package, and no header or sidecar package files on disk. Force mode additionally requires no source-control failure.

The response includes `targets[]` rows with `requested_path`, normalized `package_name`, `status`, `success`, discovery state, residual-removal state, `source_control_failure`, and final postcondition diagnostics. Commit statuses are `deleted`, `residual_removed`, `already_absent`, or `failed`; dry-run statuses are `would_delete`, `would_remove_residual`, `already_absent` (force mode), or `not_found`. Top-level `success` is the conjunction of the per-target results, while legacy summary fields such as `deleted`, `requested`, `found`, `not_found`, `failed_to_delete`, and residual/source-control arrays remain available.

### 5.4 Exact package move contract

`asset.move_assets` accepts 1-512 explicit `{source,destination}` rows. Both values must be exact writable long package names. A source package must expose exactly one non-redirector primary AssetRegistry asset whose name matches the package leaf. Every destination must be absent from AssetRegistry, loaded-package state, and disk. Sources and destinations are unique case-insensitively; any destination that is also a source is rejected so chains and cycles cannot depend on rename ordering. There is no overwrite, unique-name, skip-existing, raw file-copy, or alternate-component fallback.

`allowed_source_roots` and `allowed_destination_roots` are required non-empty guards. They compare an exact root or `root + "/"` descendant, so `/Plugin/UI` accepts `/Plugin/UI/WBP_Menu` but rejects `/Plugin/UISibling/WBP_Menu`. The action defaults to `dry_run=true`; `dry_run=false` requires `confirm=true`. Dry-run uses only AssetRegistry, `FindPackage`, package-file queries, and package dependency queries, reports `hard_referencer_count`, `soft_referencer_count`, and `loaded_asset_count=0`, and defers `FAssetData::GetAsset()` until every row has passed preflight and mutation has been authorized.

Committed execution loads all sources before mutation, submits one `FAssetRenameData` batch to `IAssetTools::RenameAssets`, and then rescans affected source/destination paths. Unreal owns reference fixup, active-provider source-control checkout/branch behavior, destination and referencer saves, and whether a source redirector is required. `accept_cdo_reference_warning=true` is an explicit, narrowly scoped policy for AssetRenameManager's exact CDO/config-reference warning; it is allowed only in an interactive editor and rejects every unexpected modal. It is not a generic unattended-dialog bypass.

`cleanup_redirectors=true` runs only when the whole rename batch succeeds. It captures and revalidates the complete redirector-object set in every remaining source package: the exact requested source redirector must target the exact requested destination object, every additional generated-class/CDO companion row must also be a redirector, and every companion must target within the exact destination package. It then requires zero hard/soft source-package referencers and intact destination state before submitting all eligible objects to one `asset.delete_assets(require_source_control=true)` batch. The 200-item safety limit counts redirector objects rather than source packages. Cleanup does not run a second reference-rewrite pass and never opens the `IAssetTools::FixupReferencers` report. A rename, referencer, target-integrity, companion-row, source-control, deletion, or final-postcondition failure is surfaced explicitly; global or partial rename failure preserves redirectors and skips cleanup.

Each row proves that the destination primary asset is registered with the original class, its package file exists and is non-empty, and the original non-redirector asset is gone. With cleanup disabled, either the source package is absent or every remaining source-package row is a redirector whose exact/companion targets satisfy the requested destination contract. With cleanup enabled, source AssetRegistry and disk state must both be absent. The top-level result is an error on any failed row and distinguishes `failed` from `partial_failure`; it never reports the requested count as moved without checking actual postconditions. The handler owns dirty-package tracking and deliberately does not claim transaction rollback because `RenameAssets` saves packages and performs source-control operations that an editor transaction cannot reverse.

### 5.5 Completed-move redirector recovery contract

`asset.cleanup_moved_redirectors` accepts 1-200 completed move rows with exact source/destination packages. A row may additionally provide both `source_object_path` and `destination_object_path`; this supports non-leaf object names and many source packages converging on one destination package without weakening source uniqueness. Dry-run is the default and loads no redirectors. It reports each source package's complete AssetRegistry row set, exact target metadata, package/object counts, remaining hard/soft referencers, destination registry/class/file integrity, and whether the source is already clean.

Preflight permits multiple AssetRegistry rows inside one source package only when every row is a redirector; input rows themselves still require unique source packages/objects. The exact requested redirector must resolve to the exact requested destination object, while every companion redirector must resolve somewhere inside the same requested destination package. A generated-class/CDO companion that targets another package, a non-redirector row, missing destination, class/file drift, or any remaining hard/soft referencer blocks the whole request before deletion. The 200 limit is rechecked against resolved redirector objects, not merely the input package count.

Confirmed mutation requires the editor game thread, a completed AssetRegistry scan, and an enabled/available source-control provider. The complete captured object set is revalidated immediately before one `asset.delete_assets(require_source_control=true)` call. Afterward, destination integrity and complete source package/registry/file removal are checked per row. An already-removed source is an idempotent success only when source control proves the expected delete or revert-add state; otherwise it is a source-control preflight failure. Results report `already_cleaned`, `success`, `partial_failure`, or `failed` without masking a partially cleaned batch.

### 5.6 Reflected soft-reference existence

`asset.inspect_asset` and typed validation serialize a soft reference as
`path`, `asset_path`, `valid`, and `exists`. Existence is true when the exact
soft object is already loaded or when the live AssetRegistry contains its
mounted asset path. The same rule applies to `/Game`, `/Engine`, and plugin
mounts; missing engine/plugin assets are not assumed to exist. `/Script`
class paths are the explicit non-asset exception because they name reflected
types rather than AssetRegistry content. Missing mounted content produces an
`unresolved_soft_reference` warning.

## 6. Settings

| Setting | Default | Effect |
|---------|---------|--------|
| `bEnableAsset` | `true` | Enables `MonolithAsset` startup registration for `asset` actions. Restart required after changing. |

## 7. Action Groups

| Group | Files | Notes |
|-------|-------|-------|
| Texture ingest | `Public/MonolithAssetTextureIngestActions.h`, `Private/MonolithAssetTextureIngestActions.cpp` | Public domain-consumer helper; supports PNG, JPEG, BMP, EXR, TGA, HDR, TIFF, and DDS through `IImageWrapper`, with explicit `fail`, `replace`, and `unique` collision policies. Optional `texture_role` values are `ui_icon`, `sprite`, `decal`, `basecolor`, `world_tile`, `normal`, `orm_mask`, `height`, and `emissive`. |
| Font ingest | `Public/MonolithAssetFontIngestActions.h`, `Private/MonolithAssetFontIngestActions.cpp` | Uses UE 5.7-safe `UFont::GetMutableInternalCompositeFont()` for composite-font writes. |
| Lifecycle | `Public/MonolithAssetLifecycleActions.h`, `Private/MonolithAssetLifecycleActions.cpp` | Owns generic file texture import, asset save, and guarded delete operations previously scattered under editor/blueprint. |
| Exact move and cleanup | `Public/MonolithAssetMoveActions.h`, `Private/MonolithAssetMoveActions.cpp` | Owns guarded explicit package relocation, path/root validation, AssetTools rename dispatch, explicit known-CDO-warning policy, recoverable/idempotent moved-redirector cleanup, and per-row registry/file/source-control postconditions. |
| Hygiene | `Public/MonolithAssetHygieneActions.h`, `Private/MonolithAssetHygieneActions.cpp` | Owns naming convention validation and batch rename fixup. |
| Inspection | `Public/MonolithAssetInspectionActions.h`, `Private/MonolithAssetInspectionActions.cpp` | Former typed asset inspection surface, now independent from `MonolithMaterial`; mounted soft-reference existence uses loaded-object/AssetRegistry proof with `/Script` as the only intentional non-asset exception. |
| Find | `Public/MonolithAssetFindActions.h`, `Private/MonolithAssetFindActions.cpp` | Fuzzy live-`AssetRegistry` search (`asset.find_assets`); thin consumer of MonolithCore `FMonolithFuzzyMatch`, owns its own corpus/fields/weights and the `allow_transposition` option. Distinct from exact-name `FMonolithAssetUtils::FindAssetCandidates` and offline `project` FTS search. |
| Package graph | `Public/MonolithAssetPackageGraphActions.h`, `Private/MonolithAssetPackageGraphActions.cpp` | Safe content mount registration (`asset.register_content_mount_points`), copy/remap planning (`asset.plan_package_graph_copy`), guarded duplication (`asset.copy_package_graph_with_remap`), strategy planning/orchestration (`asset.copy_package_graph_with_strategy`), reflected hard/soft reference rewrite (`asset.fixup_copied_references`), optional redirector cleanup inside the strategy workflow, and dependency closure validation (`asset.validate_dependency_closure`). |

## 8. Package Graph Copy/Remap Pipeline

`asset.register_content_mount_points` is a guarded preflight action for package graph workflows that need to read packages from content roots not already mounted in the editor process. Inputs are explicit `mount_points[]` specs. Each spec can provide `root`/`mount_point` plus exactly one resolver: explicit `content_dir`, loaded `plugin_name`, or `project_plugin_dir` relative to `FPaths::ProjectPluginsDir()`. `project_plugin_dir` must stay relative and cannot contain `.` or `..` path segments. `plugin_name` rows require `IPlugin::CanContainContent()` and must match the plugin mounted asset path when a root is provided. The action defaults to `dry_run=true`, refuses `/Game/`, `/Engine/`, and `/Script/` unless `allow_core_mount_points=true`, refuses conflicting existing root mappings unless `allow_override=true`, rejects duplicate/conflicting root specs within the same request before mutation, validates that resolved content directories exist, optionally `scan_asset_registry=true` after confirmed registration, and supports `probe_packages[]` existence checks. The response returns `mount_points[]`, `preflight_errors[]`, `probe_packages[]`, `would_register_count`, `registered_count`, `already_registered_count`, `conflict_count`, `missing_dir_count`, and `next_recommended_action="asset.plan_package_graph_copy"`.

`asset.plan_package_graph_copy` is a no-mutation planner for workflows that need to copy a related package graph from one content root to another. Inputs are explicit:

| Param | Contract |
|-------|----------|
| `root_packages` | One or more source package paths to seed traversal. Paths must fall under a source root in `root_remaps`. |
| `root_remaps` | Object mapping source roots to destination roots, e.g. `{"/ShooterMaps": "/SpeedMaps"}`. The longest matching source root wins. |
| `dependency_kinds` | Optional array of `hard` and/or `soft`; omitted means both. |
| `strategy` | Currently only `registry_only_plan`. Unknown strategies hard-error so future copy/fixup behavior cannot silently fall back. |
| `max_packages` | Traversal cap. The response sets `truncated=true` when the cap is hit. |
| `check_collisions` | When true, existing destination packages are reported in `collisions[]`. |

The response includes `package_map[]`, `dependency_edges[]`, `external_dependencies[]`, `collisions[]`, `package_count`, and edge/collision counts. Dependencies under remapped source roots are queued for the plan. Dependencies outside those roots are reported as external and are not copied by this action.

`asset.copy_package_graph_with_remap` reuses the same planning contract, then duplicates the planned package primary assets from each source package to the mapped destination package. The action is guarded:

| Param | Contract |
|-------|----------|
| `dry_run` | Returns the plan and copy rows without loading or duplicating destination assets. |
| `confirm` | Required when `dry_run=false`. |
| `collision_policy` | `fail_if_exists` rejects any existing destination package. `skip_existing` leaves existing destinations untouched. Overwrite/replace is intentionally unsupported. |
| `save` | Saves duplicated destination packages when true. |

The copy response includes the embedded plan, `copies[]`, `preflight_errors[]`, `saved_packages[]`, `would_copy_count`, `copied_count`, `skipped_count`, and `next_recommended_action="asset.fixup_copied_references"`.

`asset.copy_package_graph_with_strategy` is the higher-level package-copy workflow surface. It separates two concepts:

| Concept | Contract |
|---------|----------|
| `workflow` | Which phases to run: `plan_only`, `copy_only`, `copy_fixup`, or `copy_fixup_validate`. |
| `copy_strategy` | Which copy mechanism is expected: `auto`, `duplicate_asset`, `advanced_copy`, `raw_package_file_copy`, or `header_patched_advanced_copy`. |

The action always runs the planner and returns `strategy_plan[]` before mutation. With `copy_strategy=auto`, caller-provided selectors such as `header_patched_roots`, `raw_package_roots`, and `manual_copy_packages` classify rows deterministically; there is no implicit `/Game/UI`, Speed, Shooter, or Lyra root. Confirmed runs execute `duplicate_asset` rows through `IAssetTools::DuplicateAsset`, `advanced_copy` rows through `IAssetTools::AdvancedCopyPackages`, `header_patched_advanced_copy` rows by enabling `AssetTools.UseHeaderPatchingAdvancedCopy` only for that copy call, and `raw_package_file_copy` rows only when `allow_raw_package_copy=true`. Confirmed AdvancedCopy/header-patched rows require `save=true` because Unreal `IAssetTools::AdvancedCopyPackages` can save copied packages during execution. Manual single-object duplication remains unsupported and hard-errors before copying instead of silently using the wrong strategy. `dry_run=true` does not pretend that copied destination packages exist; post-copy fixup, redirector cleanup, and closure phases are skipped or planned by default and only scan existing destination packages when the caller requests the relevant dry-run scan.

`cleanup_redirectors=true` adds a post-fixup phase inside `asset.copy_package_graph_with_strategy`. The workflow builds one exact affected-package cleanup batch and delegates it to `asset.cleanup_moved_redirectors`, retaining that action's full source-package redirector validation, zero hard/soft referencer requirement, 200-object cap, source-control proof, idempotent already-cleaned handling, and postconditions. It never calls `IAssetTools::FixupReferencers`, so an unattended run cannot open the modal fixup report; cleanup failure remains an explicit workflow failure.

When `collision_policy=skip_existing` is used, skipped destination packages are excluded from downstream fixup, redirector cleanup, and closure validation. The workflow reports both `planned_package_paths` and affected `package_paths`; only affected packages are eligible for mutation after the copy phase.

`asset.fixup_copied_references` scans destination packages and rewrites reflected hard object references, `TSoftObjectPtr`/soft object references, and `FSoftObjectPath` struct values whose package path falls under a `root_remaps` source root. The action is guarded by `dry_run`/`confirm`, reports every candidate in `references[]`, reports missing targets as `target_missing`, and only saves changed packages when `save=true`. `require_targets=true` treats missing remapped targets as blocking so copy workflows cannot silently retain source-root dependencies.

`asset.validate_dependency_closure` validates destination packages after or before a copy/remap operation. It accepts `destination_roots`, optional explicit `package_paths`, `allowed_external_roots`, `legacy_source_roots`, `dependency_kinds`, and `max_packages`. The response returns `ok=false` with `violations[]` for packages outside destination roots, disallowed external dependencies, and legacy source-root dependencies.

`asset.plan_package_graph_copy` and `asset.validate_dependency_closure` are read-only. `asset.register_content_mount_points`, `asset.copy_package_graph_with_remap`, `asset.copy_package_graph_with_strategy`, and `asset.fixup_copied_references` enforce their mutation contracts in the handlers: mount registration defaults to `dry_run=true`, and the copy/fixup writers require `dry_run=true` or `confirm=true`. Raw package file copy remains opt-in because it copies package bytes without semantic repair; callers should follow it with fixup and closure validation. Material graph repair, widget subtree repair, and Blueprint graph clone repair remain owned by their domain modules.

## 9. Texture Role Pipeline

`asset.import_texture_from_bytes` accepts `texture_role` (or `role`) for Unreal-specific import behavior. Role presets apply before explicit `settings`; caller-provided `settings` fields remain the final override.

| Role | Import / post-processing contract |
|------|-----------------------------------|
| `ui_icon`, `sprite` | `sRGB=true`, `TEXTUREGROUP_UI`, `TMGS_NoMipmaps`, clamp addressing, edge-connected background alpha extraction for opaque-background generated images, and transparent-pixel RGB alpha bleed. |
| `decal` | `sRGB=true`, `TEXTUREGROUP_Effects`, group mips, clamp addressing, edge-connected background alpha extraction, alpha bleed, power-of-two warning. |
| `basecolor` | `sRGB=true`, `TC_Default`, `TEXTUREGROUP_World`, group mips, wrap addressing. |
| `world_tile` | Base-color world settings plus wrap addressing, one-pixel opposite-edge seam harmonization, power-of-two check, and opposite-edge seam validation. |
| `normal` | `sRGB=false`, `TC_Normalmap`, `TEXTUREGROUP_WorldNormalMap`, wrap addressing, power-of-two and tangent-space normal plausibility validation. |
| `orm_mask` | `sRGB=false`, `TC_Masks`, `TEXTUREGROUP_WorldSpecular`, wrap addressing, channel-range validation. |
| `height` | `sRGB=false`, `TC_Grayscale`, `TEXTUREGROUP_World`, wrap addressing, channel-range validation. |
| `emissive` | `sRGB=true`, `TEXTUREGROUP_Effects`, group mips, clamp addressing. |

For opaque-background generated images, alpha extraction first classifies pixels similar to the sampled edge background color, then flood-fills only those candidate pixels that are connected to an image edge. Disconnected interior pixels that resemble the background color remain opaque so highlights, pale hair, and enclosed details are not erased by global color keying.

The action result includes `texture_role`, `settings_applied`, and `validation`. Validation is non-destructive and returns warnings instead of failing the import unless normal parameter validation fails. `settings.alpha_from_edge_background=false` disables generated alpha extraction, `settings.tile_seam_harmonize=false` disables world-tile seam harmonization, and `return_processed_png=true` returns the imported post-processing result as `processed_png_b64`. `asset.inspect_asset` and `asset.validate_typed_asset` recognize `Texture2D` assets and report generated texture-role settings from `Monolith.Generated.texture_role` metadata.

## 10. Verification

| Gate | Requirement |
|------|-------------|
| Fork compatibility scan | No owner-scoped registry, execution-policy, action-search/planning/annotation metadata, or unsupported schema-builder calls remain in `Source/MonolithAsset`. |
| Build | Build fresh UE 5.7 and UE 5.8 editor hosts with `MonolithAsset` enabled. |
| Runtime discovery | `monolith_discover({ "namespace": "asset" })` should list 20 actions owned by `MonolithAsset`, including `move_assets` and `cleanup_moved_redirectors`. |
| Parameter type contract | Every action schema marks arrays/objects `allow_string_encoded_complex=false`; JSON-encoded strings are rejected. Every external bool input requires `EJson::Boolean`, including nested texture settings, and malformed values return `-32602` before decode/load/mutation. |
| Inspection reference contract | `MonolithAsset.InspectAsset.MountedSoftReferenceExistence` must reject missing plugin/engine asset paths, accept a real `/Engine` asset, and preserve `/Script` as an intentional non-asset reference. |
| Find engine reuse | `asset.find_assets` consumes `FMonolithFuzzyMatch` (MonolithCore); it must not duplicate edit-distance/tokenization, `allow_transposition` must flow into `ScoreCandidate`, and `FMonolithAssetUtils::FindAssetCandidates` stays exact-name. |
| Import automation | `MonolithAsset.ImportTextureFromFile`, `MonolithAsset.ImportTextureFromBytes`, and `MonolithAsset.ImportFontFamily` cover exact paths, invalid-setting rejection, explicit unique-name opt-in, source-pixel/readback metadata, save/rollback behavior, and headless operation. |
| Package graph automation | `Monolith.Asset.PackageGraph.RegistryAndParamGuards` must cover registration, handler mutation-guard rejection, duplicate resolver rejection, same-request root conflict rejection, dry-run report shape/non-mutation, project-plugin-dir resolver dry-run with a temporary generic test folder, confirmed mount idempotency, conflicting existing mount rejection, and package graph strategy report shape. |
| Move automation | `Monolith.Asset.MoveAssets` covers registration, confirmation and uniqueness/chain guards, no-load dry-run, exact cross-mount commit/recovery, explicit CDO-warning policy, and source-control-gated postconditions. `Monolith.Asset.CleanupMovedRedirectors` covers exact non-leaf object paths, many-to-one destinations, generated companion redirectors, foreign companion rejection, soft-referencer blocking, committed source-control deletion, and idempotent already-cleaned proof. |
