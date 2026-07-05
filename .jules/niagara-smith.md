# NiagaraSmith PR Guidance

## 2026-06-27 - Forbid template-echo PR titles and numeric branch evasion
**Coordination issue:** NiagaraSmith used generic "concise niagara-domain improvement." PR titles (e.g. 87b049f) and generated multiple branches with large numeric suffixes (e.g., `-6420493798690740788`, `-11100370434435694513`) to bypass collision checks when branch names were taken.
**Learning:** General instructions in `AGENTS.md` to avoid generic placeholder names and random suffixes are missed unless directly included in the agent's specific instructions. When an agent creates a PR title of "concise niagara-domain improvement.", it is echoing the prompt's instructions rather than describing the actual change.
**Prevention:** Always replace the PR title placeholder with a concrete description of the change (e.g., `NiagaraSmith: harden emitter params in rename actions`). Never append numeric task IDs, UUIDs, or timestamp suffixes to branch names. If your chosen branch name is taken or overlapping work exists, stop without PR instead of renaming the branch to bypass collision checks.
**Avoid:** Using `NiagaraSmith: concise niagara-domain improvement.` as a PR title or generating branches with `-<number>` suffixes.
