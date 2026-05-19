# Monolith - MonolithInterchange Module

**Parent:** [SPEC_CORE.md](../SPEC_CORE.md)
**Engine:** Unreal Engine 5.7+
**Version:** 0.14.10
**Status:** Implemented

---

## 1. Purpose

`MonolithInterchange` owns the `interchange` namespace for normalized import/export validation, guarded import mutation, reimport metadata, reimport, and export actions. The namespace was already separate from `mesh`; this module makes the ownership explicit so import/export workflows are not coupled to `MonolithMesh` startup or shutdown.

---

## 2. Ownership

| Class | Responsibility |
|-------|----------------|
| `FMonolithInterchangeModule` | Registers and unregisters the `interchange` namespace. |
| `FMonolithInterchangeActions` | Implements supported-format discovery, source/destination validation, import metadata inspection, guarded import/reimport, and export handlers. |

| Dependency | Purpose |
|------------|---------|
| `MonolithCore` | Tool registry, action result, and parameter schema contracts. |
| `AssetTools`, `UnrealEd` | Guarded import/reimport/export integration points. |
| `Json`, `JsonUtilities` | Action response payloads. |

---

## 3. Action Surface

| Action | Params | Description |
|--------|--------|-------------|
| `interchange.get_supported_formats` | none | Lists known extensions, module availability, default allowed roots, and policy notes. |
| `interchange.can_import` | `source_file`, `destination_path`?, `allow_external`? | Validates source existence, extension, Interchange availability, root policy, and optional destination path. |
| `interchange.can_reimport` | `asset_path` | Reports reflected source import data and source file existence. |
| `interchange.get_import_data` | `asset_path` | Returns reflected import source file rows without mutation. |
| `interchange.import_asset` | `source_file`, `destination_path`, `conflict_policy` | Imports one source file with guardrails and a structured result row. |
| `interchange.import_assets` | `source_files`, `destination_path`, `conflict_policy` | Imports files sequentially and returns one row per source. |
| `interchange.import_scene` | same as `import_asset` | Typed scene import entrypoint over guarded import. |
| `interchange.import_mesh` | same as `import_asset` | Typed mesh import entrypoint over guarded import. |
| `interchange.import_skeletal_mesh` | same as `import_asset` | Typed skeletal mesh import entrypoint over guarded import. |
| `interchange.import_texture` | same as `import_asset` | Typed texture import entrypoint over guarded import. |
| `interchange.import_audio` | same as `import_asset` | Typed audio import entrypoint over guarded import. |
| `interchange.import_with_options` | same as `import_asset`, plus `options`? | Guarded import with a forward-compatible options object. |
| `interchange.update_reimport_path` | `asset_path`, `source_file` | Updates a reflected source path after source/root validation. |
| `interchange.reimport_asset` | `asset_path` | Reimports one existing asset through Unreal's reimport manager. |
| `interchange.reimport_assets` | `asset_paths` | Reimports assets sequentially and returns one row per asset. |
| `interchange.export_asset` | `asset_path`, `file_path` | Exports one asset after output path validation. |

---

## 4. Safety Contract

| Gate | Requirement |
|------|-------------|
| Mutation confirmation | Import, reimport, source-path update, and export writes require `confirm=true` unless `dry_run=true`. |
| Source root | Source files must be under project/content/saved roots unless `allow_external=true`. |
| Destination root | Import destination package paths must be valid `/Game/...` long package paths. |
| Output root | Export destinations must stay under default roots unless `allow_external=true`. |
| Batch behavior | Batch actions return per-row status/messages and continue after row-level failures. |

---

## 5. Verification Gates

| Gate | Evidence |
|------|----------|
| Registration | `FMonolithInterchangeModule::StartupModule` registers 16 `interchange` actions. |
| Ownership cleanup | `MonolithMesh` no longer registers or unregisters the `interchange` namespace. |
| Parameter guard | `FMonolithParamGuardInterchangeImportMalformedParamsTest` rejects malformed import params through the new module. |
| UE 5.7 build | Full plugin UBT build must succeed with the engine root resolved from `GO.uproject`. |
