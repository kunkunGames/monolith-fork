# ProjectIndexer PR Guidance

## PR Intent
ProjectIndexer PRs maintain the MonolithIndex module and related features.

## Review Gate
- Ensure index operations degrade cleanly.
- Verify changes against the latest schema.

## 2026-06-11 - Forbid template-echo PR titles and numeric branch evasion
**Coordination issue:** ProjectIndexer used generic "concise project-index improvement." PR titles (e.g. a2d429a) and generated branches with large numeric suffixes (e.g., `-6266559999062214808`) despite rules forbidding this.
**Learning:** General instructions in `AGENTS.md` to avoid generic placeholder names and random suffixes are often missed by agents unless directly included in their specific `.jules/<agent>.md` instructions. When an agent creates a PR title of "concise project-index improvement.", it is echoing the prompt's instructions rather than describing the actual change.
**Prevention:** Always replace the PR title placeholder with a concrete description of the change. Never append numeric task IDs or UUIDs to branch names to evade collision checks. If your chosen branch name is taken, stop without PR instead of renaming it.
**Avoid:** Using `ProjectIndexer: concise project-index improvement.` as a PR title or generating branches with `-<number>` suffixes.

## 2026-06-20 - Forbid announcing no-ops via branches/PRs
**Coordination issue:** ProjectIndexer created a branch (`jules/project-indexer/no-op-4474653514809561051`) and PR ("ProjectIndexer: no-op, duplicate parameter hardening") simply to announce that no work was needed.
**Learning:** General instructions in `AGENTS.md` ("Never push a branch or open a PR to announce a no-op") are sometimes missed unless explicitly added to the agent's instructions. Pushing no-op branches clutters the repository and triggers unnecessary CI runs.
**Prevention:** When ownership is elsewhere, the queue already covers it, or no safe non-overlapping candidate exists, stop without creating a branch or PR. Report the findings in the task log.
**Avoid:** Pushing any branch or PR to announce a no-op.
