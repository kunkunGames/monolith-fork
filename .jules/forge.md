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

## 2026-05-10 - Monolith.uplugin CommonUI optional flag
**Build pattern:** The `CommonUI` plugin was listed as a required dependency in `Monolith.uplugin`, even though `MonolithUI.Build.cs` conditionally checks for its existence and provides a fallback using `WITH_COMMONUI`.
**Learning:** For optional Engine/Marketplace plugins that are conditionally queried in a module's `Build.cs`, failing to explicitly mark them as `"Optional": true` in the `.uplugin` file will cause the Engine to refuse to load the plugin entirely when the optional dependency is missing, rendering the conditional fallback logic useless.
**Prevention:** Always ensure that dynamically checked optional dependencies in `Build.cs` have `"Optional": true` defined in `Monolith.uplugin`.
**Avoid:** Do not leave `Enabled: true` without `Optional: true` for dynamically-checked plugins.

## 2026-05-10 - MonolithAI StateTree optional uplugin guard consistency
**Build pattern:** `StateTree`, `SmartObjects`, and `GameplayStateTree` plugins were conditionally linked in `MonolithAI.Build.cs` but listed without `"Optional": true` in `Monolith.uplugin`.
**Learning:** For optional Engine plugins that are conditionally queried in a module's `Build.cs`, failing to explicitly mark them as `"Optional": true` in the `.uplugin` file will cause the Engine to refuse to load the plugin entirely when the optional dependency is missing, defeating the conditional fallback logic.
**Prevention:** Always ensure that dynamically checked optional dependencies in `Build.cs` have `"Optional": true` defined in `Monolith.uplugin`.
**Avoid:** Do not leave `Enabled: true` without `Optional: true` for dynamically-checked plugins.

## 2026-05-13 - [Safe directory existence check in Build.cs]
**Build pattern:** `DirectoryNotFoundException` when using `Directory.GetDirectories` on a directory path that might not exist (e.g. `EnginePluginsDir`).
**Learning:** `Directory.GetDirectories` throws an exception if the base path is missing. Using it to probe for optional plugin subdirectories inside the engine or marketplace directories can break the build if the base directory itself was never created.
**Prevention:** Always use `Directory.Exists(Path.Combine(Dir, "SubDir"))` instead of `Directory.GetDirectories` for exact-match subdirectory existence checks to avoid exceptions.
**Avoid:** Using `Directory.GetDirectories` with an exact string (no wildcard) just to check if a single subdirectory exists within an unverified base path.
2026-05-14 - [Precise wildcard for optional plugin detection]
Build pattern: Falsely detecting an optional plugin because the wildcard used in `Directory.GetDirectories` is too broad and matches other plugins (e.g., `Gameplaya*` matching `GameplayAbilities` instead of the intended `BlueprintAttributes` plugin).
Learning: Using vague wildcards when searching for optional plugins can trigger invalid build dependencies.
Prevention: Use precise wildcard prefixes that uniquely match the target optional plugin directory.
Avoid: Using short or generic wildcards like `Gameplaya*` when the plugin name is `BlueprintAttributes`.

## 2026-05-15 - [Add missing MassEntity optional plugin dependency]
**Build pattern:** The `MassEntity` plugin was conditionally linked in `MonolithAI.Build.cs` but was missing from the `Monolith.uplugin` configuration.
**Learning:** For optional Engine plugins that are conditionally queried and linked in a module's Build.cs, failing to explicitly mark them as `"Optional": true` in the `.uplugin` file can cause the Engine to refuse to load the plugin entirely or fail dependency resolution when the optional dependency is enabled.
**Prevention:** Always ensure that dynamically checked optional dependencies in `Build.cs` have a corresponding `"Optional": true` entry defined in `Monolith.uplugin`.
**Avoid:** Linking optional plugins in `Build.cs` without adding them to `.uplugin`.

## 2026-05-16 - Add missing PropertyBindingUtils and MovieRenderPipeline to Monolith.uplugin
**Build pattern:** `PropertyBindingUtils` (required by `StateTree`) and `MovieRenderPipeline` were conditionally linked in their respective `Build.cs` modules but were missing from `Monolith.uplugin`.
**Learning:** For optional Engine plugins that are conditionally linked, failing to explicitly mark them as `"Optional": true` in the `.uplugin` file can cause the Engine to refuse to load the plugin entirely, rendering the conditional fallback logic useless.
**Prevention:** Always ensure that dynamically checked optional dependencies in `Build.cs` have `"Optional": true` defined in `Monolith.uplugin`.
**Avoid:** Do not omit optional plugins from `.uplugin` when they are explicitly queried in `Build.cs`.

## 2026-05-17 - [Add missing Dataflow optional plugin dependency]
**Build pattern:** The `Dataflow` plugin was conditionally linked in `MonolithDataflow.Build.cs` via engine path checks but was missing from the `Monolith.uplugin` configuration list of Plugins.
**Learning:** For optional Engine plugins that are conditionally queried and linked in a module's Build.cs, failing to explicitly mark them as `"Optional": true` in the `.uplugin` file can cause the Engine to refuse to load the plugin entirely or fail dependency resolution when the optional dependency is enabled.
**Prevention:** Always ensure that dynamically checked optional dependencies in `Build.cs` have a corresponding `"Optional": true` entry defined in `Monolith.uplugin`.
**Avoid:** Linking optional plugins in `Build.cs` without adding them to `.uplugin`.

## 2026-05-18 - [Safe Dataflow plugin detection in binary engine installs]
**Build pattern:** Optional engine plugins falsely reported as missing when checking the `Engine/Source/Runtime/` tree.
**Learning:** `MonolithDataflow.Build.cs` checked for the `Dataflow` plugin by looking inside the engine's `Source` directory. In binary engine distributions (like Epic Games Launcher installs), the `Source` directory may not exist or may be incomplete, meaning the Dataflow plugin support in Monolith was incorrectly disabled even when the plugin was installed and enabled.
**Prevention:** Always check for optional plugins in the `Plugins` directory (e.g., `EnginePluginsDir` and `ProjectPluginsDir`) alongside or instead of `Source` paths, specifically checking `Plugins/Experimental/Dataflow` or `Plugins/Runtime/Dataflow`.
**Avoid:** Checking only engine `Source` paths to detect optional runtime or experimental plugins, as this assumes the user has engine source code installed.

## 2026-05-22 - [Add missing GameplayAbilities optional plugin dependency]
**Build pattern:** The `GameplayAbilities` plugin was conditionally linked in `MonolithAI.Build.cs` via engine path checks but was listed without `"Optional": true` in `Monolith.uplugin`.
**Learning:** For optional Engine plugins that are conditionally queried and linked in a module's Build.cs, failing to explicitly mark them as `"Optional": true` in the `.uplugin` file can cause the Engine to refuse to load the plugin entirely or fail dependency resolution when the optional dependency is enabled.
**Prevention:** Always ensure that dynamically checked optional dependencies in `Build.cs` have a corresponding `"Optional": true` entry defined in `Monolith.uplugin`.
**Avoid:** Linking optional plugins in `Build.cs` without making them `"Optional": true` in `.uplugin`.

## 2026-05-18 - [Fix nested compile guards in optional features]
**Build pattern:** Optional features (e.g., `WITH_ZONEGRAPH`) nested inside the compile guards of other optional features (e.g., `WITH_MASSENTITY`).
**Learning:** This caused `ZoneGraph` actions to be completely unavailable unless `MassEntity` was also enabled, even though the `.Build.cs` probes for them independently and they are independent engine features.
**Prevention:** Ensure that independent compile guards like `#if WITH_ZONEGRAPH` are kept at the top level of the `#if` structure in header and implementation files. They should not be nested under `#if WITH_MASSENTITY` unless there is an explicit cross-module dependency.
**Avoid:** Nesting `#if WITH_...` optional guards for independent features.

## 2026-05-26 - [Directory.GetDirectories exception guard in Build.cs]
**Build pattern:** `DirectoryNotFoundException` when using `Directory.GetDirectories` on a directory path that might not exist (e.g. `ProjectPluginsDir`, `MarketplaceDir`, `EnginePluginsDir`).
**Learning:** `Directory.GetDirectories` throws an exception if the base path is missing. Using it to probe for optional plugin subdirectories inside the engine, marketplace, or project directories can break the build if the base directory itself was never created. This is especially true for `ProjectPluginsDir` and `MarketplaceDir` which might not exist in a fresh project or engine install.
**Prevention:** Always check if the base path exists using `Directory.Exists(BaseDir)` before calling `Directory.GetDirectories(BaseDir, ...)` to avoid exceptions.
**Avoid:** Calling `Directory.GetDirectories` without verifying the base directory exists.

2026-05-18 - Optional Plugin Detection Exception Prevention

Build pattern: Optional plugin detection via `Directory.GetDirectories(..., "PluginName_*", ...)` can throw `DirectoryNotFoundException` if the base path does not exist. It also risks incorrectly identifying obfuscated backup or disabled folders if they don't contain the actual `.uplugin` file.
Learning: `Target.RelativeEnginePath` and common folder assumptions like `Plugins/Marketplace/` or `Plugins/Runtime/` do not guarantee those directories exist on every machine (e.g. source-build engines or stripped project folders).
Prevention: Always ensure optional plugin detection guards use a helper that first checks `Directory.Exists(BaseDir)`. When checking wildcard directories (e.g. `PluginName_*`), always verify `File.Exists(Path.Combine(Dir, PluginName + ".uplugin"))` inside the matched directory to ensure it is the real plugin and not a disabled scratch folder.
Avoid: Using `Directory.GetDirectories()` on optional or assumed paths without a preceding `Directory.Exists()` check, and returning true simply because a wildcard directory matched without verifying the `.uplugin` descriptor.

2026-05-13 - [Safeguard Optional Plugin Discovery]
Build pattern: Optional plugins checked using `Directory.GetDirectories(..., "PluginName_*", ...)` which throws `DirectoryNotFoundException` if the base directory does not exist.
Learning: Uninitialized submodules or missing marketplace folders can trick raw path strings into causing build exceptions when checking for suffixed directories.
Prevention: Always ensure the base directory exists before calling `Directory.GetDirectories`. Use a robust `HasPluginDir` helper that checks both existence and the presence of the `.uplugin` descriptor to avoid mistaking scratch/backup folders for the real plugin.
Avoid: Directly calling `Directory.GetDirectories` on dynamically constructed plugin root paths without `Directory.Exists` guards.

## 2026-06-12 - [Add missing Chooser optional plugin dependency]
**Build pattern:** The `Chooser` plugin was conditionally linked in `MonolithAnimation.Build.cs` via `bHasChooser` engine path checks but was missing from the `Monolith.uplugin` configuration list of Plugins.
**Learning:** For optional Engine plugins that are conditionally queried and linked in a module's Build.cs, failing to explicitly mark them as `"Optional": true` in the `.uplugin` file can cause the Engine to refuse to load the plugin entirely or fail dependency resolution when the optional dependency is enabled.
**Prevention:** Always ensure that dynamically checked optional dependencies in `Build.cs` have a corresponding `"Optional": true` entry defined in `Monolith.uplugin`.
**Avoid:** Linking optional plugins in `Build.cs` without adding them to `.uplugin`.

## 2026-06-16 - [Standardize optional plugin detection in AI module]
**Build pattern:** Hardcoded directory checks like `Directory.Exists(Path.Combine(EnginePluginsDir, "Runtime", "StateTree"))` and ad-hoc project plugin functions were used to conditionally link optional modules like `StateTree`, `SmartObjects`, `GameplayAbilities`, `MassEntity`, and `ZoneGraph`.
**Learning:** Hard-rolling path existence checks rather than using a standardized `HasPluginDir` wrapper bypasses verification of the actual `.uplugin` descriptor, increasing the risk of false positives from leftover empty folders.
**Prevention:** Always use the standardized `HasPluginDir` helper which checks both directory existence and `.uplugin` existence, and properly accounts for `_*` variant suffix folders consistently across the whole repository.
**Avoid:** Writing new manual `Directory.Exists()` or `Directory.GetDirectories()` plugin path checks instead of using `HasPluginDir`.
