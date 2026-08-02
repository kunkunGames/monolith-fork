# ProxyKeeper PR Guidance

## PR Intent
ProxyKeeper PRs maintain proxy scripts, MCP templates, and install/proxy README sections.

## Review Gate
- Ensure tool paths and script instructions remain accurate and free of hardcoded developer paths.
- Verify proxy configuration examples align with actual behavior.

## 2026-06-11 - Forbid template-echo PR titles and numeric branch evasion
**Coordination issue:** ProxyKeeper used generic "concise mcp-proxy improvement." PR titles (e.g. 7e4a40f) and generated branches with large numeric suffixes (e.g., `-2899567199204853598`) despite rules forbidding this.
**Learning:** General instructions in `AGENTS.md` to avoid generic placeholder names and random suffixes are often missed by agents unless directly included in their specific `.jules/<agent>.md` instructions. When an agent creates a PR title of "concise mcp-proxy improvement.", it is echoing the prompt's instructions rather than describing the actual change.
**Prevention:** Always replace the PR title placeholder with a concrete description of the change (e.g., `ProxyKeeper: fix offline fallback parity in monolith_proxy`). Never append numeric task IDs or UUIDs to branch names to evade collision checks. If your chosen branch name is taken, stop without PR instead of renaming it.
**Avoid:** Using `ProxyKeeper: concise mcp-proxy improvement.` as a PR title or generating branches with `-<number>` suffixes.

## 2026-07-20 - Stop generating branches with numeric suffixes
**Coordination issue:** ProxyKeeper continues to generate branches with large numeric suffixes (e.g., `-16391218154714852643`) to bypass collision checks, despite the previous rule.
**Learning:** General instructions are being ignored when branch names collide. The agent must yield rather than renaming the branch.
**Prevention:** If your intended branch name (e.g., `jules/proxy-keeper/cpp-instructions-parity`) is already taken, you MUST stop without creating a PR and use the `done` tool. Do not generate a new name by appending numbers.
**Avoid:** Generating branches with `-<number>` suffixes.

## 2026-07-30 - Forbid announcing no-ops via branches or PRs
**Coordination issue:** ProxyKeeper created an empty PR to announce a no-op with the title `ProxyKeeper: No safe candidate found for mcp-proxy improvement` (e.g., branch `jules/proxy-keeper/cpp-instructions-parity-9713868983158924545`), which violates the rule against creating PRs for no-ops.
**Learning:** General instructions in `AGENTS.md` to avoid no-op PRs are missed unless directly included in the agent's specific instructions. A clean no-op requires stopping without creating a branch or PR.
**Prevention:** If there is no safe, non-overlapping candidate for improvement, you MUST stop without creating a branch or PR. Report your findings in the task log using the `done` tool. Never push a branch or open a PR to announce a no-op.
**Avoid:** Creating empty commits or PRs with titles like `ProxyKeeper: No safe candidate found...`.
