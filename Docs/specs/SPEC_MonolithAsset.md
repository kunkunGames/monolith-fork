# Monolith - MonolithAsset Module

**Parent:** [../SPEC_CORE.md](../SPEC_CORE.md)
**Engine:** Unreal Engine 5.7+
**Owner module:** `MonolithAsset`
**Namespace:** `asset`
**MCP tool:** `asset_query`
**Status:** Implemented (2026-05-21 asset namespace split)

---

## 1. Scope

`MonolithAsset` owns generic asset workflows that are not specific to one content domain: Texture2D/font ingest, file texture import, asset save/delete lifecycle operations, naming/rename hygiene, typed read-only asset inspection, and guarded package graph copy/remap planning, duplication, reference fixup, and closure validation.

This module keeps asset-level actions out of `MonolithUI`, `MonolithMesh`, and `MonolithMaterial` so those modules can focus on their native domains.

## 2. Namespace Ownership

`FMonolithAssetModule` registers all `asset` actions with owner `MonolithAsset` and unregisters them through owner-scoped cleanup during shutdown. The module is gated by `UMonolithSettings::bEnableAsset`.

`MonolithImageGen` depends on `MonolithAsset` for `MonolithAsset::FTextureIngestActions::HandleImportTextureFromBytes`, so generated-image imports and direct texture-byte imports share one Texture2D creation path. `MonolithBlueprint` depends on `MonolithAsset` for the shared save-asset helper used by its internal batch edit op.

No compatibility aliases are registered for the move from the old `ui` ingest actions or the generic lifecycle actions formerly registered under `editor`/`blueprint`. Callers should use `asset.import_texture_from_bytes`, `asset.import_font_family`, `asset.import_texture_from_file`, `asset.save_asset`, and `asset.delete_assets`.

## 3. Registered Actions

| Action | Owner class | Purpose |
|--------|-------------|---------|
| `import_texture_from_bytes` | `MonolithAsset::FTextureIngestActions` | Decode base64 compressed image bytes and create a `UTexture2D` under `/Game/...`; optional `texture_role` applies Unreal texture-role import presets, role post-processing, validation metadata, and optional postprocessed PNG return. |
| `import_font_family` | `MonolithAsset::FFontIngestActions` | Import one or more TTF files as a composite `UFont` plus `UFontFace` assets. Headless-safe: `PostEditChange()` on the face/family assets is skipped when `FSlateApplication::IsInitialized()` is false (the call only flushes live Slate font caches and asserts in commandlets without a Slate application); asset data, registry notification, and save behavior are unchanged. |
| `import_texture_from_file` | `FMonolithAssetLifecycleActions` | Import an external image file as a `UTexture2D` with optional compression, sRGB, tiling, max-size, and LOD-group settings. Accepts `source_file`/`file_path`/`path` aliases for `source_path`, `destination_path`/`dest_path` aliases for `destination`, optional `asset_name` when the destination is a folder, `overwrite_policy=overwrite|replace` as a compatibility alias for `replace_existing=true`, and UI compression aliases such as `UserInterface2D`. |
| `save_asset` | `FMonolithAssetLifecycleActions` | Save any loaded asset package to disk. |
| `delete_assets` | `FMonolithAssetLifecycleActions` | Delete assets by path with optional prefix guard, dry-run validation, non-interactive editor closing, and optional force deletion. |
| `validate_naming_conventions` | `FMonolithAssetHygieneActions` | Scan assets under a content path and report prefix-rule violations. |
| `list_supported_asset_enrichers` | `FMonolithAssetInspectionActions` | List typed read-only enrichers supported by `inspect_asset`. |
| `inspect_asset` | `FMonolithAssetInspectionActions` | Inspect one asset with typed enrichment and optional reflected references. |
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

## 5. Lifecycle Delete Contract

`asset.delete_assets` runs non-interactively. For each loaded target it clears the package dirty flag and closes open asset editors before deleting under an unattended-script guard. `force=false` uses `ObjectTools::DeleteObjects`; `force=true` uses `ObjectTools::ForceDeleteObjects` for referenced assets. Failed deletion attempts are reported in `failed_to_delete[]` without aborting the response.

## 6. Settings

| Setting | Default | Effect |
|---------|---------|--------|
| `bEnableAsset` | `true` | Enables `MonolithAsset` startup registration for `asset` actions. Restart required after changing. |

## 7. Action Groups

| Group | Files | Notes |
|-------|-------|-------|
| Texture ingest | `Public/MonolithAssetTextureIngestActions.h`, `Private/MonolithAssetTextureIngestActions.cpp` | Public helper reused by `MonolithImageGen`; supports PNG, JPEG, BMP, EXR, TGA, HDR, TIFF, and DDS through `IImageWrapper`. Optional `texture_role` values are `ui_icon`, `sprite`, `decal`, `basecolor`, `world_tile`, `normal`, `orm_mask`, `height`, and `emissive`. |
| Font ingest | `Public/MonolithAssetFontIngestActions.h`, `Private/MonolithAssetFontIngestActions.cpp` | Uses UE 5.7-safe `UFont::GetMutableInternalCompositeFont()` for composite-font writes. |
| Lifecycle | `Public/MonolithAssetLifecycleActions.h`, `Private/MonolithAssetLifecycleActions.cpp` | Owns generic file texture import, asset save, and guarded delete operations previously scattered under editor/blueprint. |
| Hygiene | `Public/MonolithAssetHygieneActions.h`, `Private/MonolithAssetHygieneActions.cpp` | Owns naming convention validation and batch rename fixup. |
| Inspection | `Public/MonolithAssetInspectionActions.h`, `Private/MonolithAssetInspectionActions.cpp` | Former typed asset inspection surface, now independent from `MonolithMaterial`. |
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

`cleanup_redirectors=true` adds a post-fixup phase inside `asset.copy_package_graph_with_strategy` that scans the copied destination roots/package paths for `UObjectRedirector` assets and calls `IAssetTools::FixupReferencers` with no checkout dialog. The phase is explicit so copy/fixup behaviour cannot hide redirector side effects.

When `collision_policy=skip_existing` is used, skipped destination packages are excluded from downstream fixup, redirector cleanup, and closure validation. The workflow reports both `planned_package_paths` and affected `package_paths`; only affected packages are eligible for mutation after the copy phase.

`asset.fixup_copied_references` scans destination packages and rewrites reflected hard object references, `TSoftObjectPtr`/soft object references, and `FSoftObjectPath` struct values whose package path falls under a `root_remaps` source root. The action is guarded by `dry_run`/`confirm`, reports every candidate in `references[]`, reports missing targets as `target_missing`, and only saves changed packages when `save=true`. `require_targets=true` treats missing remapped targets as blocking so copy workflows cannot silently retain source-root dependencies.

`asset.validate_dependency_closure` validates destination packages after or before a copy/remap operation. It accepts `destination_roots`, optional explicit `package_paths`, `allowed_external_roots`, `legacy_source_roots`, `dependency_kinds`, and `max_packages`. The response returns `ok=false` with `violations[]` for packages outside destination roots, disallowed external dependencies, and legacy source-root dependencies.

`asset.plan_package_graph_copy` and `asset.validate_dependency_closure` are read-only. `asset.register_content_mount_points` uses `track_dirty_packages` because it mutates process mount state rather than package contents. `asset.copy_package_graph_with_remap`, `asset.copy_package_graph_with_strategy`, and `asset.fixup_copied_references` use `transaction_optional` policy and explicit `dry_run=true` or `confirm=true` (`register_content_mount_points` defaults to dry-run). Raw package file copy remains opt-in because it copies package bytes without semantic repair; callers should follow it with fixup and closure validation. Material graph repair, widget subtree repair, and Blueprint graph clone repair remain owned by their domain modules.

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
| Source stale scan | No old UI ingest action names, old UI ingest classes, or old typed-asset inspection class names remain in source. |
| Build | Run the primary `<Project>Editor` UBT command after closing any editor process that locks Monolith DLLs. |
| Runtime discovery | `monolith_discover({ "namespace": "asset" })` should list 18 actions owned by `MonolithAsset`. |
| Find engine reuse | `asset.find_assets` consumes `FMonolithFuzzyMatch` (MonolithCore); it must not duplicate edit-distance/tokenization, `allow_transposition` must flow into `ScoreCandidate`, and `FMonolithAssetUtils::FindAssetCandidates` stays exact-name. |
| Package graph automation | `Monolith.Asset.PackageGraph.RegistryAndParamGuards` must cover registration, read/write policy, mutation guard rejection, duplicate resolver rejection, same-request root conflict rejection, dry-run report shape/non-mutation, project-plugin-dir resolver dry-run with a temporary generic test folder, confirmed mount idempotency, conflicting existing mount rejection, and package graph strategy report shape. |
