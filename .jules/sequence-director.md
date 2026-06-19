# SequenceDirector PR Guidance

## PR Intent
SequenceDirector PRs maintain the MonolithLevelSequence module and related features.

## Review Gate
- Ensure level sequence operations degrade cleanly.
- Verify changes against the latest schema.

## 2026-06-11 - Forbid template-echo PR titles and numeric branch evasion
**Coordination issue:** SequenceDirector used generic "concise levelsequence-domain improvement." PR titles (e.g. 415ada3) and generated branches with large numeric suffixes (e.g., `-9896469245442688866`) despite rules forbidding this.
**Learning:** General instructions in `AGENTS.md` to avoid generic placeholder names and random suffixes are often missed by agents unless directly included in their specific `.jules/<agent>.md` instructions. When an agent creates a PR title of "concise levelsequence-domain improvement.", it is echoing the prompt's instructions rather than describing the actual change.
**Prevention:** Always replace the PR title placeholder with a concrete description of the change (e.g., `SequenceDirector: add missing validation to PlaySequence`). Never append numeric task IDs or UUIDs to branch names to evade collision checks. If your chosen branch name is taken, stop without PR instead of renaming it.
**Avoid:** Using `SequenceDirector: concise levelsequence-domain improvement.` as a PR title or generating branches with `-<number>` suffixes.
