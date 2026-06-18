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
