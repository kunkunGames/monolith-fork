# Monolith - MonolithInterchange Module

**Parent:** [SPEC_CORE.md](../SPEC_CORE.md)
**Engine:** Unreal Engine 5.7+
**Version:** 0.22.0
**Status:** Implemented

---

## 1. Purpose

`MonolithInterchange` owns the `interchange` namespace for normalized import/export validation, guarded import mutation, reimport metadata, reimport, and export actions. It keeps the complete import pipeline behind one independently loadable editor module instead of coupling that lifecycle to `MonolithMesh`.

---

## 2. Ownership

| Class | Responsibility |
|-------|----------------|
| `FMonolithInterchangeModule` | Registers and unregisters the `interchange` namespace. |
| `FMonolithInterchangeActions` | Implements supported-format discovery, source/destination validation, import metadata inspection, guarded import/reimport, and export handlers. |
| `CommitMonolithInterchangeExportFiles` and commit structs | Commit declared staged exporter outputs, preserve existing destinations as rollback backups, and restore the complete output set when promotion fails. |

| Dependency | Purpose |
|------------|---------|
| `MonolithCore` | Tool registry, action result, and parameter schema contracts. |
| `AssetRegistry`, `AssetTools`, `UnrealEd`, `InterchangeEngine` | Destination snapshots, guarded import/reimport/export integration points, rollback, and translator availability checks. `Monolith.uplugin` enables the owning engine `Interchange` plugin because this is a hard module dependency. |
| `Json`, `JsonUtilities` | Action response payloads. |

---

## 3. Action Surface

| Action | Params | Description |
|--------|--------|-------------|
| `interchange.get_supported_formats` | none | Lists known extensions, module availability, default allowed roots, and policy notes. |
| `interchange.can_import` | `source_file`, `destination_path`?, `allow_external`? | Normalizes harmless destination-folder formatting, then validates source existence, extension, a registered translator or factory, link-safe root policy, and optional destination path. |
| `interchange.can_reimport` | `asset_path` | Reports `FReimportManager::CanReimport` and the handler-reported source files. |
| `interchange.get_import_data` | `asset_path` | Returns reflected import source file rows without mutation. |
| `interchange.import_asset` | `source_file`, `destination_path`, `conflict_policy` | Imports one source with `fail`, `overwrite`, or unique-name `rename` behavior for single-output formats. Scene/mesh sources require `fail` and a dedicated, bounded-proven-empty destination because their importer can create secondary packages. |
| `interchange.import_assets` | `source_files`, `destination_path`, `conflict_policy` | Imports files sequentially and returns one row per source. Dry-run reserves prospective package names across rows so intra-batch conflicts match confirmation. Multi-source batches reject scene/mesh formats whose secondary package names cannot be predicted. |
| `interchange.import_scene` | same as `import_asset` | Requires a scene-capable format, registered scene factory, `conflict_policy=fail`, and a dedicated empty destination. |
| `interchange.import_mesh` | same as `import_asset` | Explicitly configures static-mesh import, verifies the returned class, and applies the multi-output destination guard. |
| `interchange.import_skeletal_mesh` | same as `import_asset` | Explicitly configures FBX skeletal import, verifies the returned class, and applies the multi-output destination guard. |
| `interchange.import_texture` | same as `import_asset` | Requires a texture source and verifies a texture result. |
| `interchange.import_audio` | same as `import_asset` | Requires a wave-audio source and verifies a sound-wave result. |
| `interchange.update_reimport_path` | `asset_path`, `source_file` | Requires a registered reimport handler and verifies the updated path through handler readback. |
| `interchange.reimport_asset` | `asset_path` | Reimports one existing asset through Unreal's reimport manager. An optional replacement `source_file` must be an exact non-empty string and compatible with the asset type. |
| `interchange.reimport_assets` | `asset_paths`, `allow_external`? | Validates handler-reported sources, then reimports assets sequentially. |
| `interchange.export_asset` | `asset_path`, `file_path` | Resolves and bounds every exporter-declared output, rejects script exporters whose side effects cannot be contained, writes into an isolated same-parent staging directory, inspects at most 256 immediate entries without recursion, and promotes the complete output set only after postcondition checks. Dry runs and preflight errors retain the stable transaction response shape. |

---

## 4. Safety Contract

| Gate | Requirement |
|------|-------------|
| Mutation confirmation | Import, reimport, source-path update, and export writes require `confirm=true` unless `dry_run=true`. |
| Source root | Source files must be under project/content/saved roots and must not traverse a symlink/junction below those roots unless `allow_external=true`. |
| Destination root | Import destination package paths must be valid `/Game/...` long package paths. |
| Multi-output imports | Scene and mesh sources may create importer-defined material, texture, or scene packages. They are accepted only one source at a time, with `conflict_policy=fail`, after the destination is proven empty through complete Asset Registry, loaded-object, and bounded filesystem inspection. An incomplete inspection is an explicit error, never an empty-directory assumption. |
| Output root | Export destinations must stay under default roots without linked-path traversal unless `allow_external=true`. |
| Export transaction | Exporters may declare at most 256 distinct files, and every resolved output must remain in the requested directory. Native outputs are generated in an isolated staging directory and checked for missing, duplicate, symlink/junction-traversing, extra-file, and extra-directory results before promotion. Inspection retains at most 256 immediate entries and never descends into a directory because any nested output is already invalid. Existing destinations move to recovery backups first; any promotion failure force-removes an already promoted file even when it is read-only, then restores the full set, with promotion/restoration moves configured for immediate failure instead of engine retry delays or UI error handling. Dry runs and preflight errors return the full transaction field set with explicit not-attempted defaults. An incomplete restore reports `status=partial_export`, `partial_mutation=true`, preserves staging, and lists only recovery files that actually exist in exact `retained_paths`. Cleanup deletes only bounded immediate files from a completely scanned flat staging directory; an entry-budget failure or any directory leaves staging intact and reports incomplete cleanup rather than recursively traversing exporter-controlled paths. Every cleanup-error branch reports the staging path only if that directory still exists. Blueprint/script exporters are rejected because their arbitrary filesystem side effects cannot be bounded by the native filename contract. |
| Backend availability | Import dry runs require an actual Interchange translator or legacy factory; export dry runs require a matching exporter. |
| Typed result | Mesh, skeletal-mesh, texture, and audio entrypoints verify the imported object class before reporting success. A mismatch removes newly returned asset objects; complete cleanup reports `error`, while any retained pre-existing, unmanaged, or undeletable object reports `partial_import` plus `partial_mutation=true` and structured rollback evidence. An overwrite target that exists on disk but is not yet indexed or loaded is force-loaded before the rollback snapshot is taken, so it is never misclassified as newly created and deleted. A partial rollback reports `deleted_object_paths` for exactly the candidates that were actually removed, not only when every deletion succeeds. |
| Reimport handler | Reimport and source-path updates use `FReimportManager`; every retained stored source passes existence/root/link checks, optional source indexes require exact integer JSON, and an optional replacement source requires an exact non-empty string plus asset-kind/backend compatibility. Path updates report success only after handler readback matches. A readback mismatch never reports a bare `error`, because the handler has already mutated the asset: the previous path is restored and the call reports `error` with `mutation_committed=true`, or, when restoration fails, `partial_mutation` plus `reimport_path_rollback_failed`. |
| Conflict policy | `fail` rejects collisions, `overwrite` enables replacement, and `rename` resolves a unique package. All three policies assign the preflight-resolved name to `UAssetImportTask::DestinationName`, including sanitized names such as `123.png` → `Asset_123`. |
| Batch behavior | Batch actions return per-row status/messages and continue after row-level failures. Import dry-runs reserve successful prospective package names so later rows see same-batch collisions. |
| Undo scope | `[w]` means external side effects, not universal editor Undo. Import/reimport behavior is handler-specific, reimport-path changes dirty metadata, and filesystem exports are not undoable. |

---

## 5. Verification Gates

| Gate | Evidence |
|------|----------|
| Registration | `FMonolithInterchangeModule::StartupModule` registers 15 `interchange` actions. |
| Namespace isolation | `MonolithMesh` remains unchanged; `MonolithInterchange` alone owns registration and shutdown for the `interchange` namespace. |
| Registration and parameter guard | `FMonolithParamGuardInterchangeImportMalformedParamsTest` verifies all 15 registrations, rejects malformed params, proves an audio action rejects a texture extension before import, removes a newly created typed-mismatch fixture, preserves a pre-existing fixture for explicit partial-mutation reporting, and confirms a real texture export replaces an existing file only through staged commit. |
| Export rollback | `FMonolithInterchangeExportTransactionTest` verifies successful replacement, complete two-file rollback after a later promotion fails, forced removal of a promoted read-only file before restoration, staged-output link traversal detection, fail-closed late collisions, exact existing recovery evidence when restoration itself is injected to fail, a non-recursive staging scan with a strict entry budget, successful flat cleanup, and refusal to descend into an unexpected directory. |
| UE 5.7 build | Full plugin UBT build must succeed with the engine root resolved from the host `.uproject`. |
| UE 5.8 build | Full plugin UBT build must succeed with the engine root resolved from the host `.uproject`. |
| Live action contract | Discovery must expose all 15 actions; guarded dry runs must reject absent sources, typed mismatches, unavailable backends, and unsafe linked paths without creating content. |
