## 2026-05-05 - Add missing ignore rules for generated databases and release zips
**Hygiene issue:** Tools generate SQLite databases (`EngineSource.db`, `ProjectIndex.db`) and release scripts create zip packages (`Monolith-v*.zip`) that lack explicit ignores, increasing the risk of accidental commits.
**Learning:** Monolith uses standalone Python scripts and PowerShell wrappers that generate artifacts at root or subfolders which were never explicitly ignored, likely because they fall outside typical Unreal `Intermediate/` or `Saved/` patterns.
**Prevention:** Add explicit `EngineSource.db`, `ProjectIndex.db`, and `Monolith-v*.zip` entries to `.gitignore`.
**Avoid:** Assuming UE's standard `Saved/` or `Intermediate/` ignores will cover all generated database and release artifacts created by custom tooling.
## 2026-05-08 - make_release.ps1 dynamic vswhere
**Hygiene issue:** make_release.ps1 hardcoded C:\Program Files paths to find dumpbin.exe for the post-build smoke check.
**Learning:** Hardcoded paths break on non-standard installations or alternate drive letters.
**Prevention:** Use vswhere.exe dynamically to locate VC Tools instead of hardcoding static paths.
**Avoid:** Hardcoded C:\Program Files or specific Visual Studio version paths in build and release scripts.
## 2026-05-08 - make_release.ps1 dynamic UBT detection
**Hygiene issue:** make_release.ps1 hardcoded the UnrealBuildTool (UBT) path to `C:\Program Files (x86)\UE_5.7\...`, which breaks on non-standard installations or alternate drive letters.
**Learning:** Hardcoded paths for build tools create fragile scripts that fail across different developer environments and CI runners.
**Prevention:** Use environment variables like `UE_57`, `UE_ROOT`, or `UE_57_UBT` to dynamically locate tools (like UBT) instead of relying solely on hardcoded static paths.
**Avoid:** Hardcoded `C:\Program Files` paths for Unreal Engine binaries in build and release scripts.
## 2026-05-10 - Fail fast on dynamic tool resolution
**Hygiene issue:** Build scripts (`Tools/MonolithProxy/build_proxy.bat` and `Tools/MonolithQuery/build.bat`) hardcoded fallback paths like `C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat` if `vswhere.exe` failed, leading to brittle toolchains.
**Learning:** Hardcoded `C:\Program Files` paths cause mysterious build failures in non-standard environments, and silent fallbacks hide the root cause of `vswhere.exe` resolution issues.
**Prevention:** Build scripts should fail fast with clear errors when dynamic tool resolution (e.g., `vswhere.exe`) fails, rather than falling back to hardcoded paths.
**Avoid:** Hardcoded `C:\Program Files` paths for Visual Studio tools in batch scripts.
## 2026-05-11 - make_release.ps1 dynamic UBT detection without silent skip
**Hygiene issue:** `make_release.ps1` silently skipped the UBT path when it couldn't locate it dynamically (with no hardcoded fallbacks), which creates confusing build behavior.
**Learning:** Silently skipping critical build tools leads to brittle scripts that fail later in confusing ways instead of at the point of tool discovery.
**Prevention:** Build scripts should fail fast with clear errors when dynamic tool resolution (e.g., UBT) fails, rather than warning and continuing without building.
**Avoid:** Silently skipping build tool steps when dynamic resolution fails.

## 2026-05-13 - Ignore CI artifacts and build staging directories
**Hygiene issue:** The macOS release pipeline can leave `_stage/`, and local static checks can leave `markdownlint.log`.
**Learning:** CI and release helper artifacts often appear at repository root, outside Unreal's standard `Saved/` and `Intermediate/` ignore coverage.
**Prevention:** Ignore only the known generated artifacts (`_stage/` and `markdownlint.log`) unless there is concrete evidence that a broader log pattern is safe.
**Avoid:** Creating multiple overlapping `.gitignore` PRs for the same artifact family or adding broad ignore rules without checking active Curator/Harbor PRs.
## 2026-05-14 - Ignore temporary workflow artifacts
**Hygiene issue:** Temporary workflow files such as pr_body.txt, commit_msg.txt, and prs.json leaked into working directories during agent runs.
**Learning:** These workflow artifacts are not captured by standard Unreal ignores and create noise in git status during orchestration runs.
**Prevention:** Add explicit ignore rules for known workflow files like pr_body.txt, commit_msg.txt, prs.json, and no_op_reason.txt in .gitignore.
**Avoid:** Committing or leaving workflow artifacts unstaged, which can cause spurious diffs in CI or subsequent tasks.
## 2026-05-19 - Ignore Ruff cache directories
**Hygiene issue:** Ruff linter creates `.ruff_cache/` directories during local execution, which lack explicit ignores and leak into the working directory.
**Learning:** Python tooling generates cache directories that aren't captured by standard Unreal ignores.
**Prevention:** Add explicit ignore rules for Python tooling caches like `.ruff_cache/` in `.gitignore`.
**Avoid:** Committing or leaving linter cache directories unstaged, which can cause spurious diffs.
