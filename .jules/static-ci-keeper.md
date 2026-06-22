# StaticCIKeeper PR Guidance

## PR Intent
StaticCIKeeper PRs improve static CI coverage, false positives/negatives, workflow hygiene, or script diagnostics.

## Code Work Improvements
- Do not make format-only changes or introduce arbitrary logic without corresponding tests.

## 2026-06-11 - Forbid template-echo PR titles and numeric branch evasion
**Coordination issue:** StaticCIKeeper used generic "concise static-ci improvement." PR titles and generated branches with large numeric suffixes (e.g., `-8976756420109382388`, `-11641849521723171082`) despite rules forbidding this.
**Prevention:** Always replace the PR title placeholder with a concrete description of the change (e.g., `StaticCIKeeper: fix dispatcher regex pattern`). Never append numeric task IDs or UUIDs to branch names to evade collision checks. If your chosen branch name is taken, stop without PR instead of renaming it.
