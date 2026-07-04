# StaticCIKeeper PR Guidance

## PR Intent
StaticCIKeeper PRs improve static CI coverage, false positives/negatives, workflow hygiene, or script diagnostics.

## Code Work Improvements
- Do not make format-only changes or introduce arbitrary logic without corresponding tests.

## 2026-06-11 - Forbid template-echo PR titles and numeric branch evasion
**Coordination issue:** StaticCIKeeper used generic "concise static-ci improvement." PR titles and generated branches with large numeric suffixes (e.g., `-8976756420109382388`, `-11641849521723171082`) despite rules forbidding this.
**Prevention:** Always replace the PR title placeholder with a concrete description of the change (e.g., `StaticCIKeeper: fix dispatcher regex pattern`). Never append numeric task IDs or UUIDs to branch names to evade collision checks. If your chosen branch name is taken, stop without PR instead of renaming it.

## 2026-07-04 - Forbid announcing no-ops via branches/PRs
**Coordination issue:** StaticCIKeeper created a branch (e.g., `no-op-14285775667520913326`) simply to announce that no work was needed.
**Learning:** General instructions in `AGENTS.md` ("Never push a branch or open a PR to announce a no-op") are sometimes missed unless explicitly added to the agent's instructions. Pushing no-op branches clutters the repository and triggers unnecessary CI runs.
**Prevention:** When ownership is elsewhere, the queue already covers it, or no safe non-overlapping candidate exists, you must stop without creating a branch or PR. Report your findings in the task log using the `done` tool instead.
**Avoid:** Pushing any branch or PR (such as `jules/static-ci-keeper/no-op`) to announce a no-op.
