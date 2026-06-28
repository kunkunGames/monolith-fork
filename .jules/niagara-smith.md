# NiagaraSmith PR Guidance

## 2026-06-27 - Forbid numeric branch evasion
**Coordination issue:** NiagaraSmith generated multiple branches with large numeric suffixes (e.g., `-6420493798690740788`, `-11100370434435694513`) to bypass collision checks when branch names were taken.
**Learning:** General instructions in `AGENTS.md` to avoid random suffixes are missed unless directly included in the agent's specific instructions.
**Prevention:** Never append numeric task IDs, UUIDs, or timestamp suffixes to branch names. If your chosen branch name is taken or overlapping work exists, stop without PR instead of renaming the branch to bypass collision checks.
**Avoid:** Generating branches with `-<number>` suffixes.
