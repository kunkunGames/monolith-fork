# Onboarding Target Adapters

Each JSON file describes one Windows agent onboarding target for `Scripts/onboard_monolith.ps1`.

Fields:

- `id`: Stable command-line target name.
- `displayName`: Human-readable target name for logs.
- `skillRoot`: Windows skill directory. Environment variables such as `%USERPROFILE%` are expanded by the script.
- `instructionFiles`: Files to create or update under the selected instruction root.
- `supportsSkills`: Whether Monolith should link `Skills/<skill>/SKILL.md` into the target skill root.
- `supportsInstructions`: Whether the script should manage an instruction block for the target.
- `supportsMcpConfig`: Whether the script should manage MCP setup for this target.
- `mcpConfigKind`: MCP setup handler. `codex-cli` uses `codex mcp`, `claude-cli` uses `claude mcp`, and `project-file` requires `-ProjectMcpConfig` to write `.mcp.json`.
- `mcpServerName`: MCP server name, normally `monolith`.
- `mcpUrl`: HTTP MCP endpoint used by CLI-backed global config.
- `mcpScope`: Config scope when applicable, for example Claude's `user` scope.

Keep adapters data-only. Add target-specific behavior in `Scripts/onboard_monolith.ps1` only when a data field cannot express it.
