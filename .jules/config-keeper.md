# ConfigKeeper PR Guidance

## PR Intent
ConfigKeeper PRs improve config-write correctness, test for config get/set, harden external-param reads, or spec/skill parity.

## 2026-06-11 - Forbid template-echo PR titles and numeric branch evasion
**Coordination issue:** ConfigKeeper used generic "concise config-domain improvement." PR titles and generated branches with large numeric suffixes (e.g., `-15913867061983513676`) despite rules forbidding this.
**Learning:** General instructions in `AGENTS.md` to avoid generic placeholder names and random suffixes are often missed by agents unless directly included in their specific `.jules/<agent>.md` instructions. When an agent creates a PR title of "concise config-domain improvement.", it is echoing the prompt's instructions rather than describing the actual change.
**Prevention:** Always replace the PR title placeholder with a concrete description of the change. Never append numeric task IDs or UUIDs to branch names to evade collision checks. If your chosen branch name is taken, stop without PR instead of renaming it.
**Avoid:** Using `ConfigKeeper: concise config-domain improvement.` as a PR title or generating branches with `-<number>` suffixes.
