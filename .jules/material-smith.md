# MaterialSmith PR Guidance

## PR Intent
MaterialSmith PRs maintain the MonolithMaterial module, related specs, and skills.

## Review Gate
- Ensure material and graph building actions use safe parameter parsing.

## 2026-06-22 - Forbid template-echo PR titles and numeric branch evasion
**Coordination issue:** MaterialSmith used generic PR titles (template echoes) and generated branches with large numeric suffixes (e.g., `-10416055177652110227`, `-17258070212617953658`) despite rules forbidding this.
**Learning:** General instructions in `AGENTS.md` to avoid generic placeholder names and random suffixes are often missed by agents unless directly included in their specific `.jules/<agent>.md` instructions. When an agent creates a PR title of "concise material-domain improvement", it is echoing the prompt's instructions rather than describing the actual change.
**Prevention:** Always replace the PR title placeholder with a concrete description of the change (e.g., `MaterialSmith: harden parameter parsing in graph builder`). Never append numeric task IDs or UUIDs to branch names to evade collision checks. If your chosen branch name is taken, stop without PR instead of renaming it.
**Avoid:** Using `MaterialSmith: concise material-domain improvement.` as a PR title or generating branches with `-<number>` suffixes.
