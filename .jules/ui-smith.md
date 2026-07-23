# UISmith PR Guidance

## 2026-07-22 - Forbid numeric branch evasion
**Coordination issue:** UISmith generated multiple branches with large numeric suffixes (e.g., `-3205246524475073317`, `-2637841846785424356`) to bypass collision checks when branch names were taken.
**Learning:** General instructions in `AGENTS.md` to avoid random suffixes are missed unless directly included in the agent's specific instructions.
**Prevention:** Never append numeric task IDs, UUIDs, or timestamp suffixes to branch names. If your chosen branch name is taken or overlapping work exists, stop without PR instead of renaming the branch to bypass collision checks.
**Avoid:** Generating branches with `-<number>` suffixes.
