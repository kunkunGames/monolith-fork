# OptionalDependencyScout PR Guidance

## 2026-07-15 - Forbid numeric branch evasion and no-op branches
**Coordination issue:** OptionalDependencyScout generated multiple branches with large numeric suffixes (e.g., `-14320660304689837896`, `-12344010001561010781`) and pushed a branch to announce a no-op (e.g., `no-op-14167747329402907475`).
**Learning:** General instructions in `AGENTS.md` to avoid random suffixes and no-op branches are missed unless directly included in the agent's specific instructions.
**Prevention:** Never append numeric task IDs, UUIDs, or timestamp suffixes to branch names. If your chosen branch name is taken, overlapping work exists, or no safe non-overlapping candidate exists, stop without creating a branch or PR. Never push a branch or open a PR to announce a no-op.
**Avoid:** Generating branches with `-<number>` suffixes or creating branches for no-op runs.
