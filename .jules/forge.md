## 2026-05-04 - [Null guard Target.ProjectFile in Build.cs]
**Build pattern:** `NullReferenceException` during `Target.ProjectFile.Directory` access when compiling Unreal Engine plugins outside the context of a project (e.g. `RunUAT BuildPlugin`).
**Learning:** `Target.ProjectFile` is null during engine-only builds or Program targets. Several optional-dependency integration modules failed to check this before traversing up the path.
**Prevention:** Always wrap `Target.ProjectFile.Directory` access with `if (Target.ProjectFile != null)` in `Build.cs`.
**Avoid:** Attempting to query the `ProjectPluginsDir` using `Target.ProjectFile` unconditionally.
