# ActionCountKeeper PR Guidance

## PR Intent
ActionCountKeeper PRs synchronize action counts across all count-bearing documents based on live registrations.

## 2026-07-26 - Forbid numeric branch evasion
**Coordination issue:** ActionCountKeeper generated multiple branches with large numeric suffixes (e.g., `-10951624597645671994`, `-11985645716520099538`, `-13911127569431974190`) to bypass collision checks when branch names were taken.
**Learning:** General instructions in `AGENTS.md` to avoid random suffixes are missed unless directly included in the agent's specific instructions.
**Prevention:** Never append numeric task IDs, UUIDs, or timestamp suffixes to branch names. If your chosen branch name is taken or overlapping work exists, stop without PR instead of renaming the branch to bypass collision checks.
**Avoid:** Generating branches with `-<number>` suffixes.
