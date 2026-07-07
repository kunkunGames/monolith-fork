# Forge PR Guidance

## PR Intent
Forge PRs improve build, tooling, and generated-code reliability while preserving developer environment portability.

## Code Work Improvements
- Replace hardcoded tool paths with explicit dynamic resolution and fail-fast errors.
- Keep script changes narrow and covered by syntax or smoke checks that can run without UE when possible.
- Avoid mixing Build.cs, packaging, and CI regex changes unless one verified bug requires all of them.

## Review Gate
- Verify changed script paths on Windows PowerShell semantics, not POSIX assumptions.
- Check adjacent StaticCIKeeper, Harbor, Curator, and optional dependency PRs before editing shared scripts or Build.cs files.
- Report unavailable UE/UBT verification honestly with the required blocked wording.

## 2026-06-23 - [Add missing ToolsetRegistry optional plugin dependency]
**Build pattern:** The `ToolsetRegistry` plugin was conditionally linked in `MonolithToolsetBridge.Build.cs` via `IsPluginEnabled` engine path checks but was missing from the `Monolith.uplugin` configuration list of Plugins.
**Learning:** For optional Engine plugins that are conditionally queried and linked in a module's Build.cs, failing to explicitly mark them as `"Optional": true` in the `.uplugin` file can cause the Engine to refuse to load the plugin entirely or fail dependency resolution when the optional dependency is enabled.
**Prevention:** Always ensure that dynamically checked optional dependencies in `Build.cs` have a corresponding `"Optional": true` entry defined in `Monolith.uplugin`.
**Avoid:** Linking optional plugins in `Build.cs` without adding them to `.uplugin`.

## 2026-06-30 - Forbid numeric branch evasion
**Coordination issue:** Forge generated multiple branches with large numeric suffixes (e.g., `-8306146049153877827`, `-12874753197961638571`) to bypass collision checks when branch names were taken.
**Learning:** General instructions in `AGENTS.md` to avoid random suffixes are missed unless directly included in the agent's specific instructions.
**Prevention:** Never append numeric task IDs, UUIDs, or timestamp suffixes to branch names. If your chosen branch name is taken or overlapping work exists, stop without PR instead of renaming the branch to bypass collision checks.
**Avoid:** Generating branches with `-<number>` suffixes.
