# MonolithAsset Exact-Path Lifecycle Action Port Verification

**Date:** 2026-07-29
**Branch:** `jules/codex/asset/lifecycle-actions`
**Fork base:** `kunkunGames/monolith-fork@ee1dae25f9a90a45ae768abbfcb0d9356810b0c4`
**Reference source:** `kunkunGames/monolith@a3ca8bf69319f04a5d520736d3f31bdc44c4f0e1`
**Engine floor:** Unreal Engine 5.7
**Additional engine:** Unreal Engine 5.8
**Status:** PASS

---

## 1. Purpose

Verify the focused port of the generic `asset` namespace into the public fork. The change adds exact-path Texture2D/font ingest, save/delete/move lifecycle operations, typed inspection, live AssetRegistry search, naming hygiene, and guarded package-graph copy/fixup/closure workflows without adding security, benchmark, invocation-log, metadata-analytics, or reinforcement-learning features.

The port also makes the shared MonolithCore fuzzy matcher public, hardens registry invalid-parameter behavior, and adds strict native JSON array/object and exact boolean contracts required by the new action schemas.

## 2. Action Surface

The generated catalog contains exactly these 20 `asset` actions:

| Action | Primary verification focus |
|---|---|
| `batch_rename_assets` | Hygiene schema and registry coverage |
| `cleanup_moved_redirectors` | Exact redirector targets, source-control gate, idempotency |
| `copy_package_graph_with_remap` | Confirmation guard, collisions, package duplication |
| `copy_package_graph_with_strategy` | Explicit strategy planning and unsupported blockers |
| `delete_assets` | Segment-bounded guards and disk/source-control postconditions |
| `find_assets` | Shared fuzzy matcher, score bounds, transposition option |
| `fixup_copied_references` | Reflected hard/soft-reference remap |
| `import_font_family` | Absolute `.ttf` source, exact names, invalid-type rejection |
| `import_texture_from_bytes` | Exact path, rollback, role/settings validation |
| `import_texture_from_file` | Exact requested path and source metadata |
| `inspect_asset` | Typed read-only enrichment |
| `inspect_assets_batch` | Per-row result contract |
| `list_supported_asset_enrichers` | Stable typed-enricher discovery |
| `move_assets` | No-load dry run, exact move, postconditions |
| `plan_package_graph_copy` | Read-only dependency/remap plan |
| `register_content_mount_points` | Dry-run default and explicit confirmation |
| `save_asset` | On-disk/non-empty and optional reload proof |
| `validate_dependency_closure` | Destination/source-root closure checks |
| `validate_naming_conventions` | Naming-rule diagnostics |
| `validate_typed_asset` | Typed validation without mutation |

## 3. Isolated Host Setup

| Engine | Host | Plugin source | Isolation result |
|---|---|---|---|
| UE 5.7 | `D:\P4\MonolithAssetUE57Host` | `D:\P4\MonolithForkAsset` | Branch worktree built directly; `bEnableAsset=True`, MCP server disabled for automation |
| UE 5.8 | `D:\P4\MonolithAssetUE58CleanHost` | `D:\P4\MonolithForkAssetUE58Source` | Detached source worktree with the exact staged patch; no binary or Intermediate reuse from UE 5.7 |

The source-and-descriptor patch identity copied into the detached UE 5.8 source worktree was:

```text
82d6e173bd5717455f7e271db4ab855c9c403b13
```

Both hosts resolve the engine install from the `.uproject` `EngineAssociation` registry entry. Neither host hard-codes an alternate engine checkout.

The font-ingest fixture used by both hosts is:

```text
Content\UI\Fonts\Atkinson\AtkinsonHyperlegible-Regular.ttf
size:   1,045,720 bytes
SHA256: B3658EADAE55E682B5F69EB64C439C1ECC8F196C0BB8D4756D145D13BC86476A
```

## 4. Build Results

| Engine | Gate | Result | Evidence |
|---|---|---|---|
| UE 5.7 | Final full editor/plugin build | PASS, 203/203 actions, fresh `UnrealEditor-MonolithAsset.dll` and `UnrealEditor-MonolithCore.dll`, `Result: Succeeded` | `D:\P4\MonolithAssetUE57Host\Saved\Logs\MonolithAssetUE57BuildFinal.stdout.log` |
| UE 5.8 | Final independent editor/plugin build | PASS, 203/203 actions, fresh `UnrealEditor-MonolithAsset.dll` and `UnrealEditor-MonolithCore.dll`, `Result: Succeeded` | `D:\P4\MonolithAssetUE58CleanHost\Saved\Logs\MonolithAssetUE58BuildFinal.stdout.log` |

The UE 5.8 Asset DLL was produced at `2026-07-30T01:34:33.712+09:00`, size `1,455,616` bytes, SHA256 `B9EEDA41203D44493158B25A37641A7A71F9FA2CCBA3208FCE70C51B638B5E92`.

## 5. Automation Results

Both engines ran the same focused filter:

```text
Monolith.Asset+
MonolithAsset+
Monolith.Core.FuzzyMatch+
Monolith.Describe.ActionSchema.MissingBoth
```

| Engine | Success | Success with expected warnings | Failed | Not run | Duration | Report |
|---|---:|---:|---:|---:|---:|---|
| UE 5.7 | 36 | 11 | 0 | 0 | 15.078 s | `D:\P4\MonolithAssetUE57Host\Saved\Automation\Asset-Final-20260730-012701\index.json` |
| UE 5.8 | 36 | 11 | 0 | 0 | 12.712 s | `D:\P4\MonolithAssetUE58CleanHost\Saved\Automation\Asset-Final-20260730-013550\index.json` |

Both logs end with `TEST COMPLETE. EXIT CODE: 0`:

- `D:\P4\MonolithAssetUE57Host\Saved\Logs\MonolithAssetUE57AutomationFinal-20260730-012701.log`
- `D:\P4\MonolithAssetUE58CleanHost\Saved\Logs\MonolithAssetUE58AutomationFinal-20260730-013550.log`

Expected-warning tests deliberately provoke unavailable source files, save failures, read-only rollback, and AssetRegistry reload notices to verify fail-closed behavior. No test reported a failed or not-run state.

## 6. Catalog and Static Consistency

The generated source catalog reports:

| Metric | Result |
|---|---|
| Total actions | 1,581 |
| Total namespaces | 25 |
| `asset` actions | 20 |
| Source hash | `f8112f5c1fe1c8ea20e66b384599c2a23416daaef36df65c63bb7fc062bdb9f9` |
| Snapshot SHA256 | `D1B1E841D4ACF9C42BEFCF030374A65A55CC0129B59322DA5C7D4BA51CD32523` |
| Snapshot | `D:\P4\MonolithAssetUE57Host\Saved\Automation\Asset-Final-20260730-012701\monolith_catalog_snapshot.json` |

The current Speed checker is newer than this fork baseline, so its unavailable proxy/analyzer/skill/benchmark/offline/workflow checks and repository-wide CRLF findings were excluded symmetrically. The remaining checker surface was run against the exact clean fork base and the port:

| Tree | Blockers | Advisories |
|---|---:|---:|
| Clean base `ee1dae25` | 7 | 11 |
| Asset port | 7 | 11 |
| New delta | **0** | **0** |

The remaining findings are therefore pre-existing fork/checker incompatibilities, not regressions introduced by `MonolithAsset`. `git diff --check` also passes.

## 7. Contract Review

| Contract | Result |
|---|---|
| Native complex JSON types | PASS — every Asset schema opts into `StrictComplexTypes()` and emits `allow_string_encoded_complex=false`; JSON-encoded array/object strings are rejected |
| Exact booleans | PASS — top-level and nested Asset booleans require `EJson::Boolean`; UE 5.7 string coercion cannot reach mutation |
| Missing required params | PASS — registry returns JSON-RPC `-32602` and automation covers missing namespace/action schema fields |
| Exact package paths | PASS — default collision behavior never invents a substitute path; unique naming is explicit opt-in only |
| Shared fuzzy matching | PASS — `asset.find_assets` uses public `FMonolithFuzzyMatch`; no duplicate edit-distance implementation |
| Mutation safety | PASS — confirmation, dry-run, source-control, collision, rollback, and final postcondition gates are explicit |
| Module ownership | PASS — `MonolithAsset` owns the namespace and unload unregisters it; `bEnableAsset` is the module gate |
| Excluded feature classes | PASS — no security, benchmark, invocation-log, analytics-metadata, or reinforcement-learning feature was added |

## 8. Visual and Discord Evidence

Screenshot capture is **N/A**. This change adds headless MCP action handlers, schemas, tests, and documentation and does not alter gameplay, runtime UI, editor UI, VFX, animation, material presentation, or another visual state.

Discord screenshot upload is therefore **N/A**. No `Build\BatchFiles\Script\UploadScreenshotTestsToDiscord.bat` call was made because there is no meaningful PC `1920x1080` visual artifact for this API-only change.

## 9. Result

PASS. The fork gains a coherent 20-action `asset` namespace with exact-path and fail-closed contracts, compiles on the supported UE 5.7 floor and UE 5.8, and passes 47/47 focused automation tests on both independent hosts with zero new static-check findings relative to the exact fork base.
