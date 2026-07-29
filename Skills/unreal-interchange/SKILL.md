---
name: unreal-interchange
description: Use for the Unreal Interchange import/export framework via Monolith MCP (interchange namespace) - import pipelines and pipeline stacks, glTF/FBX/USD/OBJ import, typed mesh/skeletal-mesh/scene/texture/audio import, reimport, export, and source-data inspection. For generic asset lifecycle (save/duplicate/move/delete/metadata) and direct Texture2D/font ingest use unreal-asset; to inspect or edit a mesh after import (LOD/collision/texel-density/GeometryScript) use unreal-mesh; for sprite-sheet ingest contracts use unreal-sprite; when the source texture should be AI-generated instead of imported from a file use unreal-imagegen. Triggers on interchange, import, reimport, import asset, glTF, gltf, GLB, FBX, USD, USDZ, OBJ, import pipeline, pipeline stack, import mesh, import skeletal mesh, import scene, import texture, import audio, export asset, source data, reimport path, supported formats.
---

# unreal-interchange

**15 actions** via `interchange_query(action, params)`. Drives the Monolith MCP `interchange` namespace - the guarded Interchange import/reimport/export pipeline. Action names below are the live registry surface; call `monolith_discover` for exact parameter schemas.

## Discovery

```
monolith_discover({ namespace: "interchange" })  # terse action list
describe_query("action_schema", {
  "target_namespace": "interchange",
  "target_action": "<action>"
})  # exact params for one action
```

When the right action is unclear, `monolith_find("<task>")` suggests candidates across namespaces.

Module spec: `Docs/specs/SPEC_MonolithInterchange.md`

## When To Use / Use A Different Skill For

Use this skill to drive the import pipeline: validate a source file, run a typed or generic import, reimport, update a reimport path, or export an asset to disk.

| Instead, route to | When |
| --- | --- |
| `unreal-asset` | Generic asset lifecycle (save/duplicate/move/delete/metadata, naming, batch rename) or direct Texture2D / TTF-OTF font ingest, not the import-pipeline framework. |
| `unreal-mesh` | Inspecting or editing a mesh AFTER import - LOD/collision/texel-density/GeometryScript - rather than driving the import. |
| `unreal-sprite` | Sprite-sheet ingest contracts specifically, rather than the general Interchange import stack. |
| `unreal-imagegen` | The source texture should be AI-generated instead of imported from an existing file. |

## Action Reference

Param notation: `name*` required, `name?` optional, `name=val` default, `a/b/c` allowed values, and `[w]` means the action has external side effects. `[w]` does not promise editor Undo: imports and reimports use Unreal's handler-specific behavior, reimport-path updates dirty import metadata, and filesystem exports are not undoable. Signatures are a snapshot of the live catalog — use `describe_query("action_schema", ...)` for the exact schema.

The typed import entrypoints (`import_mesh`/`import_skeletal_mesh`/`import_texture`/`import_audio`/`import_scene`) share the same signature as `import_asset` below; `conflict_policy` is REQUIRED on every import. They enforce the requested source kind and, except for scene imports, verify the returned object class.

### Read-only (4)

| Action | Signature | Purpose |
|--------|-----------|---------|
| `get_supported_formats` | _(none)_ | List Monolith Interchange import/export validation capabilities |
| `can_import` | `source_file*` `destination_path?` `allow_external?=false` | Validate whether a source file is importable |
| `can_reimport` | `asset_path*` | Check whether an asset has source import data usable for reimport |
| `get_import_data` | `asset_path*` | Read import source metadata from an asset without mutation |

### Import (7, all `[w]`)

| Action | Signature | Purpose |
|--------|-----------|---------|
| `[w] import_asset` | `source_file*` `destination_path*` `conflict_policy*` (`fail`/`overwrite`/`rename`) `allow_external?=false` `confirm?=false` `dry_run?=false` | Import one source file with actual translator/factory and collision checks |
| `[w] import_assets` | `source_files*` (array) `destination_path*` `conflict_policy*` `allow_external?=false` `confirm?=false` `dry_run?=false` | Import multiple source files, one result row per source |
| `[w] import_mesh` | _(same as import_asset)_ | Configure static-mesh import and verify a `UStaticMesh` result |
| `[w] import_skeletal_mesh` | _(same as import_asset)_ | Configure FBX skeletal import and verify a `USkeletalMesh` result |
| `[w] import_texture` | _(same as import_asset)_ | Require a texture source and verify a `UTexture` result |
| `[w] import_audio` | _(same as import_asset)_ | Require wave audio and verify a `USoundWave` result |
| `[w] import_scene` | _(same as import_asset)_ | Require a scene-capable source and scene factory |

### Reimport / export (4, all `[w]`)

| Action | Signature | Purpose |
|--------|-----------|---------|
| `[w] reimport_asset` | `asset_path*` `source_file?` `source_file_index?=-1` `allow_external?=false` `confirm?=false` `dry_run?=false` | Reimport one asset via Unreal's reimport manager |
| `[w] reimport_assets` | `asset_paths*` (array) `allow_external?=false` `confirm?=false` `dry_run?=false` | Validate stored sources, then reimport multiple assets |
| `[w] update_reimport_path` | `asset_path*` `source_file*` `source_file_index?=-1` `allow_external?=false` `confirm?=false` `dry_run?=false` | Repoint an asset's reimport source path |
| `[w] export_asset` | `asset_path*` `file_path*` `replace_existing?=false` `allow_external?=false` `confirm?=false` `dry_run?=false` | Export one asset to a local file after path validation |

## Common Workflows

```
# Check a source file is importable before committing to a destination.
interchange_query("can_import", { "source_file": "Imports/hero.fbx" })

# Import a mesh through the typed entrypoint with a dry run first, then for real (conflict_policy is required).
interchange_query("import_mesh", { "source_file": "Imports/hero.fbx", "destination_path": "/Game/Meshes", "conflict_policy": "rename", "dry_run": true })
interchange_query("import_mesh", { "source_file": "Imports/hero.fbx", "destination_path": "/Game/Meshes", "conflict_policy": "rename", "confirm": true })

# Reimport an existing asset, then read back its import source metadata.
interchange_query("reimport_asset", { "asset_path": "/Game/Meshes/Hero", "confirm": true })
interchange_query("get_import_data", { "asset_path": "/Game/Meshes/Hero" })

# Repoint a moved source file before reimporting.
interchange_query("update_reimport_path", { "asset_path": "/Game/Meshes/Hero", "source_file": "Imports/hero.fbx", "confirm": true })

# Export an asset back out to a local file.
interchange_query("export_asset", { "asset_path": "/Game/Meshes/Hero", "file_path": "Exports/hero.fbx", "confirm": true })
```

## Gotchas / Rules

- `destination_path` is a UE content folder such as `/Game/Imported` (not a `.uasset` filesystem path); `source_file` / export `file_path` are on-disk paths.
- Relative import paths resolve under the project directory; relative export paths resolve under `Saved`. Absolute external paths require `allow_external: true`.
- Default-root checks reject any source/output path that traverses a symlink or junction below an allowed root. Use a direct path; only use `allow_external: true` after caller-side policy explicitly permits it.
- Import and reimport are high-impact mutations (`[w]`): `conflict_policy` is required, and writes need `confirm: true` unless `dry_run: true`. Use `dry_run` plus `can_import` / `can_reimport` first.
- Typed imports can fail after Unreal creates objects. `rollback_complete=true` means Monolith removed every new returned asset; `status=partial_import` / `partial_mutation=true` means something remained and must be inspected before retrying. Do not collapse that state into an ordinary error.
- Do not assume `[w]` actions can be reverted with editor Undo. Verify the resulting asset/import metadata after a confirmed write, and treat exported files as normal filesystem side effects.
- A successful dry run means the concrete importer, reimport handler, or exporter exists; it is not inferred from module presence alone.
- After importing a mesh, hand off mesh inspection/edit (LOD, collision, texel density, GeometryScript) to `unreal-mesh`; this skill owns the import pipeline, not post-import editing.
- For direct Texture2D or TTF/OTF font ingest that does not need the Interchange stack, use `unreal-asset`; for AI-generated source textures use `unreal-imagegen`.
- `get_supported_formats` reports the validation capabilities Monolith exposes, not the full engine importer matrix; confirm a format with `can_import` against the actual file.

## Notes

- This reference is generated from the live `RegisterAction` surface. If an action is missing or renamed, re-run `monolith_discover({ namespace: "interchange" })` — the catalog is the source of truth.
- Call `describe_query("action_schema", target_namespace="interchange", target_action="<action>")` for required/optional params and types before calling an action; `monolith_discover({ namespace: "interchange", detail: true })` is the larger all-action alternative.
