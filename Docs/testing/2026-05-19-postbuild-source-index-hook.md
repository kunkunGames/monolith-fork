# Post-Build Source Index Hook — Verification

**Date:** 2026-05-19
**Spec:** [specs/SPEC_MonolithRoutingCohesionRefactor.md](../specs/SPEC_MonolithRoutingCohesionRefactor.md) — IX1 (build side), IX3 (CRG with indexing)
**Scope:** `Source/GoGameEditor.Target.cs` `PostBuildSteps` → `BatchFiles\PostBuildSourceIndex.bat`

---

## What was added

| Artifact | Purpose |
|----------|---------|
| `BatchFiles\PostBuildSourceIndex.bat` | Detached, single-instance-locked, **always exit 0**. Writes `Saved/PostBuild/reindex.request` (consumed by the pending in-editor incremental reindex) and best-effort runs CRG `update`. Resolves engine Python portably via `ResolveUnrealEngine.ps1` (no hard-coded engine path). |
| `Source/GoGameEditor.Target.cs` | `PostBuildSteps.Add(... PostBuildSourceIndex.bat "$(ProjectDir)")`. UE `.uproject` carries no post-build hook, so Target.cs is the correct equivalent. |

The legacy Python `source_indexer`/`index_project.py` is **deliberately not used** (schema-unsafe, uninvoked since 2026-03-15). A headless editor is **not used** (known crash). The engine-source DB is still built only by the safe in-editor C++ `UMonolithSourceSubsystem`; this hook just signals it and rebuilds CRG.

## Verification (2026-05-19, no full UE build required)

| Check | Result |
|-------|--------|
| `.bat` parses under `cmd.exe` | PASS — required a CRLF fix (Write produced LF; cmd needs CRLF). 115 CRLF, 0 lone LF, no BOM. |
| Worker mode exit code | **0** (non-fatal contract holds). |
| Reindex marker | PASS — `Saved/PostBuild/reindex.request` written: `{"requested":"…","reason":"post_build","project":"D:\P4\game"}`. |
| Portable engine Python | PASS — resolved `…\UE_5.7\Engine\Binaries\ThirdParty\Python3\Win64\python.exe` via `ResolveUnrealEngine.ps1`. |
| CRG step actually runs | PASS — `uv run code-review-graph update` bootstrapped `.venv` (81 pkgs), migrated CRG graph schema v1→v9, rebuilt FTS (919 rows), loaded 792 nodes / 44382 edges. |
| CRG failure non-fatal | PASS — CRG errors logged, batch still exits 0. |
| Single-instance lock | PASS — concurrent run skips; lock dir created/removed around `:main`; stale lock (>15 min) auto-cleared. |
| `MONOLITH_SKIP_CRG=1` / `MONOLITH_SKIP_POSTBUILD_INDEX=1` | PASS — toggles honored. |

## Update (2026-05-19, later) — offline EngineSource.db builder added

The marker-handshake was upgraded to a real editor-less builder:

- **`UMonolithReindexCommandlet`** (`Source/MonolithSource/Private/MonolithReindexCommandlet.{h,cpp}`) — `UnrealEditor-Cmd -run=MonolithReindex [-mode=project|full]`. Reuses `FMonolithSourceIndexer::RunSynchronous()` (zero parser/schema divergence). No `Build.cs` change (deps already present). MonolithSource is `Type: Editor` so it loads under `UnrealEditor-Cmd`.
- **Batch rewired**: `PostBuildSourceIndex.bat` now resolves `UnrealEditor-Cmd.exe` portably and runs `-mode=project` when `EngineSource.db` exists; if missing it logs the `-mode=full` bootstrap command and skips (heavy full index never auto-runs). Marker + CRG steps retained. Re-smoke-tested: CRLF OK (135/0), exit 0, marker written, engine resolved (`D:\Engine\UE_5.7`), skip toggles (`MONOLITH_SKIP_SOURCE_INDEX`/`_CRG`) honored, lock lifecycle OK.

### Status

| Item | State |
|------|-------|
| Commandlet source + batch wiring + structural smoke | DONE |
| Commandlet **compile-verify** (`GoGameEditor` UBT build) | PENDING — gate; run the primary build command. |
| Runtime: `-mode=project` actually rebuilds `EngineSource.db` (mtime bump) | PENDING — needs the compile first; offline `-mode=project` is incremental (DB present, 4.69 GB). |
| First real post-build CRG run | Heavy once (`uv` builds `.venv`, ~81 pkgs, background, non-fatal); later runs reuse `.venv`. |
| Go-project docs-sync | `Source/GoGameEditor.Target.cs` changed — Go `Docs/SPEC_CORE.md` one-line note still flagged (not done; out of Monolith scope). |
