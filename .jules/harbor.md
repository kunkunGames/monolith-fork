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
