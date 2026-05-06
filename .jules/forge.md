## 2026-05-04 - [Null guard Target.ProjectFile in Build.cs]
**Build pattern:** `NullReferenceException` during `Target.ProjectFile.Directory` access when compiling Unreal Engine plugins outside the context of a project (e.g. `RunUAT BuildPlugin`).
**Learning:** `Target.ProjectFile` is null during engine-only builds or Program targets. Several optional-dependency integration modules failed to check this before traversing up the path.
**Prevention:** Always wrap `Target.ProjectFile.Directory` access with `if (Target.ProjectFile != null)` in `Build.cs`.
**Avoid:** Attempting to query the `ProjectPluginsDir` using `Target.ProjectFile` unconditionally.

## 2026-05-05 - [Add missing AssetRegistry dependency]
**Build pattern:** Header files including `AssetRegistry/IAssetRegistry.h` or `AssetRegistry/AssetRegistryModule.h` when `AssetRegistry` is not listed in `PrivateDependencyModuleNames`.
**Learning:** MonolithAudio relied on transitive dependencies for `AssetRegistry`. This is a fragile pattern that can lead to build breakages if the transitive include chains change.
**Prevention:** Explicitly list required modules like `AssetRegistry` in `[Public/Private]DependencyModuleNames` if their headers are directly included in the module's source files.
**Avoid:** Relying on transitive dependencies to resolve explicit includes.
