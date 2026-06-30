# AIDirector PR Guidance

## PR Intent
AIDirector PRs maintain the MonolithAI module, improving parameter hardening, input validation, domain correctness, focused tests under Source/MonolithAI/Private/Tests/, and parity of its module spec and skill docs.

## 2026-06-30 - Forbid numeric branch evasion
**Coordination issue:** AIDirector generated multiple branches with large numeric suffixes (e.g., `-17731659365476389627`, `-18406880885814042958`) to bypass collision checks when branch names were taken.
**Learning:** General instructions in `AGENTS.md` to avoid random suffixes are missed unless directly included in the agent's specific instructions.
**Prevention:** Never append numeric task IDs, UUIDs, or timestamp suffixes to branch names. If your chosen branch name is taken or overlapping work exists, stop without PR instead of renaming the branch to bypass collision checks.
**Avoid:** Generating branches with `-<number>` suffixes.
