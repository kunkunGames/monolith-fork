# ComboGraphKeeper PR Guidance

## PR Intent
ComboGraphKeeper PRs improve MonolithComboGraph action contracts, optional-plugin gating, graph behavior, docs/skill parity, or reflection-safety.

## 2026-06-11 - Forbid template-echo PR titles and numeric branch evasion
**Coordination issue:** ComboGraphKeeper used generic "concise combograph-domain improvement." PR titles and generated branches with large numeric suffixes (e.g., `-9765663199722375601`) despite rules forbidding this.
**Learning:** General instructions in `AGENTS.md` to avoid generic placeholder names and random suffixes are often missed by agents unless directly included in their specific `.jules/<agent>.md` instructions. When an agent creates a PR title of "concise combograph-domain improvement.", it is echoing the prompt's instructions rather than describing the actual change.
**Prevention:** Always replace the PR title placeholder with a concrete description of the change (e.g., `ComboGraphKeeper: harden optional parameters in actions`). Never append numeric task IDs or UUIDs to branch names to evade collision checks. If your chosen branch name is taken, stop without PR instead of renaming it.
**Avoid:** Using `ComboGraphKeeper: concise combograph-domain improvement.` as a PR title or generating branches with `-<number>` suffixes.
