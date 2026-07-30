# Monolith - MonolithInterchange Module

**Parent:** [SPEC_CORE.md](../SPEC_CORE.md)
**Engine:** Unreal Engine 5.7+
**Version:** 0.21.3
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
| `interchange.import_asset` | `source_file`, `destination_path`, `conflict_policy` | Imports one source with `fail`, `overwrite`, or unique-name `rename` behavior. |
| `interchange.import_assets` | `source_files`, `destination_path`, `conflict_policy` | Imports files sequentially and returns one row per source. Dry-run reserves prospective package names across rows so intra-batch conflicts match confirmation. |
| `interchange.import_scene` | same as `import_asset` | Requires a scene-capable format and registered scene factory. |
| `interchange.import_mesh` | same as `import_asset` | Explicitly configures static-mesh import and verifies the returned class. |
| `interchange.import_skeletal_mesh` | same as `import_asset` | Explicitly configures FBX skeletal import and verifies the returned class. |
| `interchange.import_texture` | same as `import_asset` | Requires a texture source and verifies a texture result. |
| `interchange.import_audio` | same as `import_asset` | Requires a wave-audio source and verifies a sound-wave result. |
| `interchange.update_reimport_path` | `asset_path`, `source_file` | Requires a registered reimport handler and verifies the updated path through handler readback. |
| `interchange.reimport_asset` | `asset_path` | Reimports one existing asset through Unreal's reimport manager. |
| `interchange.reimport_assets` | `asset_paths`, `allow_external`? | Validates handler-reported sources, then reimports assets sequentially. |
| `interchange.export_asset` | `asset_path`, `file_path` | Requires a matching exporter before dry-run success or export. |

---

## 4. Safety Contract

| Gate | Requirement |
|------|-------------|
| Mutation confirmation | Import, reimport, source-path update, and export writes require `confirm=true` unless `dry_run=true`. |
| Source root | Source files must be under project/content/saved roots and must not traverse a symlink/junction below those roots unless `allow_external=true`. |
| Destination root | Import destination package paths must be valid `/Game/...` long package paths. |
| Output root | Export destinations must stay under default roots without linked-path traversal unless `allow_external=true`. |
| Backend availability | Import dry runs require an actual Interchange translator or legacy factory; export dry runs require a matching exporter. |
| Typed result | Mesh, skeletal-mesh, texture, and audio entrypoints verify the imported object class before reporting success. A mismatch removes newly returned asset objects; complete cleanup reports `error`, while any retained pre-existing, unmanaged, or undeletable object reports `partial_import` plus `partial_mutation=true` and structured rollback evidence. An overwrite target that exists on disk but is not yet indexed or loaded is force-loaded before the rollback snapshot is taken, so it is never misclassified as newly created and deleted. A partial rollback reports `deleted_object_paths` for exactly the candidates that were actually removed, not only when every deletion succeeds. |
| Reimport handler | Reimport and source-path updates use `FReimportManager`; every retained stored source passes existence/root/link checks, optional source indexes require exact integer JSON, and path updates report success only after handler readback matches. A readback mismatch never reports a bare `error`, because the handler has already mutated the asset: the previous path is restored and the call reports `error` with `mutation_committed=true`, or, when restoration fails, `partial_mutation` plus `reimport_path_rollback_failed`. |
| Conflict policy | `fail` rejects collisions, `overwrite` enables replacement, and `rename` resolves a unique package. All three policies assign the preflight-resolved name to `UAssetImportTask::DestinationName`, including sanitized names such as `123.png` → `Asset_123`. |
| Batch behavior | Batch actions return per-row status/messages and continue after row-level failures. Import dry-runs reserve successful prospective package names so later rows see same-batch collisions. |
| Undo scope | `[w]` means external side effects, not universal editor Undo. Import/reimport behavior is handler-specific, reimport-path changes dirty metadata, and filesystem exports are not undoable. |

---

## 5. Verification Gates

| Gate | Evidence |
|------|----------|
| Registration | `FMonolithInterchangeModule::StartupModule` registers 15 `interchange` actions. |
| Namespace isolation | `MonolithMesh` remains unchanged; `MonolithInterchange` alone owns registration and shutdown for the `interchange` namespace. |
| Registration and parameter guard | `FMonolithParamGuardInterchangeImportMalformedParamsTest` verifies all 15 registrations, rejects malformed params, proves an audio action rejects a texture extension before import, removes a newly created typed-mismatch fixture, and preserves a pre-existing fixture for explicit partial-mutation reporting. |
| UE 5.7 build | Full plugin UBT build must succeed with the engine root resolved from the host `.uproject`. |
| UE 5.8 build | Full plugin UBT build must succeed with the engine root resolved from the host `.uproject`. |
| Live action contract | Discovery must expose all 15 actions; guarded dry runs must reject absent sources, typed mismatches, unavailable backends, and unsafe linked paths without creating content. |
