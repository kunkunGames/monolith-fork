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
| `AssetTools`, `UnrealEd`, `InterchangeEngine` | Guarded import/reimport/export integration points and translator availability checks. |
| `Json`, `JsonUtilities` | Action response payloads. |

---

## 3. Action Surface

| Action | Params | Description |
|--------|--------|-------------|
| `interchange.get_supported_formats` | none | Lists known extensions, module availability, default allowed roots, and policy notes. |
| `interchange.can_import` | `source_file`, `destination_path`?, `allow_external`? | Validates source existence, extension, a registered translator or factory, link-safe root policy, and optional destination path. |
| `interchange.can_reimport` | `asset_path` | Reports `FReimportManager::CanReimport` and the handler-reported source files. |
| `interchange.get_import_data` | `asset_path` | Returns reflected import source file rows without mutation. |
| `interchange.import_asset` | `source_file`, `destination_path`, `conflict_policy` | Imports one source with `fail`, `overwrite`, or unique-name `rename` behavior. |
| `interchange.import_assets` | `source_files`, `destination_path`, `conflict_policy` | Imports files sequentially and returns one row per source. |
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
| Typed result | Mesh, skeletal-mesh, texture, and audio entrypoints verify the imported object class before reporting success. |
| Reimport handler | Reimport and source-path updates use `FReimportManager`; path updates report success only after handler readback matches. |
| Conflict policy | `fail` rejects collisions, `overwrite` enables replacement, and `rename` supplies `UAssetImportTask::DestinationName` from `CreateUniqueAssetName`. |
| Batch behavior | Batch actions return per-row status/messages and continue after row-level failures. |

---

## 5. Verification Gates

| Gate | Evidence |
|------|----------|
| Registration | `FMonolithInterchangeModule::StartupModule` registers 15 `interchange` actions. |
| Namespace isolation | `MonolithMesh` remains unchanged; `MonolithInterchange` alone owns registration and shutdown for the `interchange` namespace. |
| Registration and parameter guard | `FMonolithParamGuardInterchangeImportMalformedParamsTest` verifies all 15 registrations, rejects malformed params, and proves an audio action rejects a texture extension before import. |
| UE 5.7 build | Full plugin UBT build must succeed with the engine root resolved from the host `.uproject`. |
| UE 5.8 build | Full plugin UBT build must succeed with the engine root resolved from the host `.uproject`. |
| Live action contract | Discovery must expose all 15 actions; guarded dry runs must reject absent sources, typed mismatches, unavailable backends, and unsafe linked paths without creating content. |
