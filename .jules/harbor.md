## 2026-05-03 - [Docs Version Sync]
**Release risk:** Stale version numbers and out-of-sync capability counts (e.g. action counts in API_REFERENCE.md and README.md vs live specs) degrade confidence and confuse agents attempting to use discovery.
**Learning:** Hard-coded versions in multiple files (`API_REFERENCE.md`, `SPEC_CORE.md`, all module specs) and action counts across docs require manual sweeping when versions bump.
**Prevention:** Always grep for `\*\*Version:\*\*` and specific action count integers in markdown docs as a post-release check.
**Avoid:** Avoid leaving README.md counts (like UI and Editor) decoupled from the detailed counts in the API reference and specs.

## 2026-05-05 - Remove auto-updater fallback to source zipballs
**Release risk:** Auto-updater falling back to source zipballs when a compiled release zip is missing or malformed.
**Learning:** Monolith is an Unreal Engine plugin that relies on shipping precompiled Binaries/ for non-C++ users. GitHub's `zipball_url` provides only raw repository source without these binaries. If the auto-updater falls back to `zipball_url`, it downloads an uncompiled plugin, which will cause the plugin to fail to load or prompt for compilation on the user's end.
**Prevention:** The auto-updater must strictly verify that the selected asset ends with `.zip` (the compiled release asset) and abort if no valid release asset is found. Never fall back to `zipball_url`.
**Avoid:** Falling back to source zipballs (`zipball_url`) for C++ Unreal Engine plugins that require precompiled binaries.

## 2026-05-06 - Auto-updater platform-aware zip asset selection
**Release risk:** The auto-updater might accidentally download the `-macOS.zip` on Windows, or `.zip` on macOS, which would cause an update failure or broken binary.
**Learning:** Monolith provides macOS-specific binary releases (`-macOS.zip`) which should only be downloaded on Mac, and Windows should ignore them.
**Prevention:** Check `PLATFORM_MAC` and conditionally look for the `*-macOS.zip` file and `Monolith-macOS-SHA256` hash in `MonolithUpdateSubsystem.cpp`.
**Avoid:** Trusting `.EndsWith(".zip")` blindly when multiple platforms exist.

## 2026-05-10 - Add missing release workflow to CONTRIBUTING.md
**Release risk:** Incomplete documentation on the release process can lead to forgotten steps, specifically around generating and adding the `Monolith-SHA256:` marker into the GitHub release notes body. Missing this causes auto-updater failures.
**Learning:** Monolith relies heavily on `Scripts/make_release.ps1` for building and signing its release ZIPs (ensuring `Installed=true` and extracting the SHA256 marker). This requirement was not formally documented in `CONTRIBUTING.md`, leading to manual or incorrect releases.
**Prevention:** Include a dedicated 'Release Workflow' section in `CONTRIBUTING.md` explicitly detailing version updates, script execution, and the mandatory SHA256 marker inclusion for the auto-updater to function correctly.
**Avoid:** Missing documentation on the exact commands needed to prepare and verify a release before pushing to GitHub.

## 2026-05-10 - [Version Sync]
**Release risk:** Stale version numbers and out-of-sync capability counts (e.g. action counts in API_REFERENCE.md and README.md vs live specs) degrade confidence and confuse agents attempting to use discovery.
**Learning:** Hard-coded versions in multiple files (`API_REFERENCE.md`, `README.md`, all module specs) and action counts across docs require manual sweeping when versions bump.
**Prevention:** Always grep for specific action count integers in markdown docs as a post-release check to ensure consistency.
**Avoid:** Avoid leaving README.md counts (like UI, Animation and Editor) decoupled from the detailed counts in the API reference and specs.

## 2026-05-11 - Correctly output macOS release zip and marker from release script
**Release risk:** The `make_release.ps1` script didn't generate `-macOS.zip` output names or the `Monolith-macOS-SHA256` marker prefix when executed on macOS, causing friction and manual renaming during macOS release builds.
**Learning:** Monolith's C++ auto-updater explicitly expects `Monolith-vX.Y.Z-macOS.zip` and parses `Monolith-macOS-SHA256: <hash>` on macOS, but the PowerShell build script used a hardcoded Windows `.zip` naming and standard `Monolith-SHA256:` prefix regardless of platform.
**Prevention:** Check `$IsMacOS` in `Scripts/make_release.ps1` and correctly branch the output filename and SHA256 marker prefix. Ensure `CONTRIBUTING.md` mentions the macOS marker.
**Avoid:** Hardcoding platform-specific release artifact names and checksum prefixes in cross-platform build scripts.

## 2026-05-14 - Fix misleading missing-marker docs
**Release risk:** Misleading release docs imply updates fail without a marker, hiding the fact that they proceed without integrity checks.
**Learning:** The actual auto-updater behavior logs a warning and proceeds if the SHA256 marker is completely missing.
**Prevention:** Always verify documentation against the actual updater C++ code (`MonolithUpdateSubsystem.cpp`).
**Avoid:** Writing release docs that claim the updater is stricter than it actually is.

## 2026-05-16 - Preserve developer directories in release ZIP and auto-updater
**Release risk:** Developer workflows (such as `.jules/` agent memories or `.github/` CI) accidentally shipping in end-user binaries, or being deleted by the auto-updater for source users.
**Learning:** Monolith's release script packages the entire directory, meaning `.jules/` and `.github/` must be explicitly excluded. Additionally, the auto-updater does a destructive swap, so developer directories present in a local clone must be explicitly preserved from the backup.
**Prevention:** Ensure `.github/` and `.jules/` are excluded in `Scripts/make_release.ps1`. Ensure the updater's `monolith_swap.bat` and `monolith_swap.sh` preserve these directories.
**Avoid:** Packaging workflow artifacts into public release ZIPs or wiping out local developer state during an auto-update.

## 2026-05-21 - Ignore .pytest_cache in release ZIP and source control
**Release risk:** Python tool caches like `.pytest_cache/` and `.ruff_cache/` can be accidentally tracked or packaged into release ZIPs, bloating the plugin or causing local collisions for users running their own Python environment.
**Learning:** Monolith's standard `.gitignore` rule for Python caches is `__pycache__/`, which does not cover tool-specific cache folders like `.pytest_cache/` or `.ruff_cache/`. Likewise, `make_release.ps1` explicitly checks for many local developer folders but may miss new tool caches.
**Prevention:** Ensure tool caches like `.pytest_cache/` and `.ruff_cache/` are explicitly ignored in `.gitignore`, and excluded in `Scripts/make_release.ps1` hygiene checks. Also, preserve them in the updater scripts (`monolith_swap.bat` and `monolith_swap.sh`) so dev state isn't wiped during auto-updates.
**Avoid:** Trusting standard language ignores to cover tool-specific generated folders that might pollute release artifacts or source control.

## 2026-05-24 - Preserve IDE directories in auto-updater
**Release risk:** Developer workspaces using `.vscode`, `.vs`, or `.idea` can be completely destroyed during a Monolith auto-update because the auto-updater blindly swaps the folder, dropping local directories that weren't explicitly preserved.
**Learning:** Monolith's C++ updater uses a destructive swap script (`monolith_swap.bat`/`.sh`) that moves the old plugin folder to backup, and copies only explicitly listed developer directories back to the new folder.
**Prevention:** Add explicit rules to preserve IDE directories (`.vscode`, `.vs`, `.idea`) in `monolith_swap.bat` and `monolith_swap.sh`.
**Avoid:** Deleting untracked workspace and developer settings during an auto-update.

## 2026-05-30 - Preserve AI developer directories in auto-updater
**Release risk:** Developer workspaces using AI tooling like `.claude` or `.jules` can be completely destroyed during a Monolith auto-update because the auto-updater blindly swaps the folder, dropping local directories that weren't explicitly preserved.
**Learning:** Monolith's C++ updater uses a destructive swap script (`monolith_swap.bat`/`.sh`) that moves the old plugin folder to backup, and copies only explicitly listed developer directories back to the new folder.
**Prevention:** Add explicit rules to preserve AI tooling directories (`.claude`, `.jules`) in `monolith_swap.bat` and `monolith_swap.sh`.
**Avoid:** Deleting untracked workspace and developer settings during an auto-update.

## 2026-06-05 - Preserve .code-review-graph directory in auto-updater
**Release risk:** Developer workspaces using the Code Review Graph tool have their `.code-review-graph/` directory destroyed during a Monolith auto-update because the auto-updater blindly swaps the folder, dropping local directories that weren't explicitly preserved.
**Learning:** Monolith's C++ updater uses a destructive swap script (`monolith_swap.bat`/`.sh`) that moves the old plugin folder to backup, and copies only explicitly listed developer directories back to the new folder.
**Prevention:** Added explicit rules to preserve the `.code-review-graph/` directory in `monolith_swap.bat` and `monolith_swap.sh`, and excluded it from public release zips in `make_release.ps1`.
**Avoid:** Deleting untracked workspace tools and developer settings during an auto-update.
