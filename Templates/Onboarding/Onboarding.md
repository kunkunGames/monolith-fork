# Monolith Agent Onboarding

This file collects the setup steps needed for agents to use Monolith MCP and the in-repo Monolith skills without copying skill files.

Windows is the supported automation environment. Use `Scripts\onboard_monolith.ps1` as the primary entrypoint, then fall back to the lower-level commands in this file only when you need to inspect or repair one step manually.

## 0. Recommended Windows Flow

Preview the full Codex and Claude onboarding plan:

```powershell
powershell -ExecutionPolicy Bypass -File Scripts\onboard_monolith.ps1 -Targets Codex,Claude -Plan
```

Apply the plan and replace existing copied skill installs with backed-up junctions:

```powershell
powershell -ExecutionPolicy Bypass -File Scripts\onboard_monolith.ps1 -Targets Codex,Claude -Execute -ReplaceCopies
```

Preview every onboarding target adapter registered under `Templates\Onboarding`:

```powershell
powershell -ExecutionPolicy Bypass -File Scripts\onboard_monolith.ps1 -Targets All -Plan
```

Use the proxy MCP template instead of the HTTP template:

```powershell
powershell -ExecutionPolicy Bypass -File Scripts\onboard_monolith.ps1 -Targets Codex,Claude -McpMode Proxy -Plan
```

Create a project-scoped `.mcp.json` only for clients that require one:

```powershell
powershell -ExecutionPolicy Bypass -File Scripts\onboard_monolith.ps1 -Targets GenericMcp -ProjectMcpConfig -Plan
```

The script performs the steps in this order:

1. Validate repository skills.
2. Link Monolith skills into selected global skill roots.
3. Configure selected global MCP clients, such as Codex and Claude user-level MCP config.
4. Create, update, or skip project instruction blocks.

Target behavior is data-driven through `Templates\Onboarding\*.json`. Add a new Windows agent target there before changing script logic.

## 1. Configure Monolith MCP

The recommended Codex and Claude setup is global user-level MCP config, not a project `.mcp.json` checked into or left beside the game project.

The onboarding script verifies existing `monolith` entries and adds missing ones through each target CLI when `-Execute` is supplied. It does not overwrite a different existing entry unless `-ReplaceMcpConfig` is also supplied.

### HTTP transport

Use this when the Monolith MCP server is already reachable at the editor/proxy endpoint.

Codex:

```powershell
codex mcp add monolith --url http://localhost:9316/mcp
```

Claude Code:

```powershell
claude mcp add --scope user --transport http monolith http://localhost:9316/mcp
```

Equivalent project-scoped `.mcp.json` clients can use `Templates/.mcp.json.example` manually or run the onboarding script with `-ProjectMcpConfig`.

```json
{
  "mcpServers": {
    "monolith": {
      "type": "http",
      "url": "http://localhost:9316/mcp"
    }
  }
}
```

### Proxy command

Use this when the client should launch the Monolith proxy process. The onboarding script uses an absolute path to `Binaries\monolith_proxy.exe` for global CLI config. Equivalent project-scoped `.mcp.json` clients can use `Templates/.mcp.json.proxy.example` manually or run with `-ProjectMcpConfig -McpMode Proxy`.

```json
{
  "mcpServers": {
    "monolith": {
      "command": "Plugins/Monolith/Binaries/monolith_proxy.exe",
      "args": []
    }
  }
}
```

After connecting, verify with `monolith_status()` before calling editor-backed actions. If the MCP endpoint is unreachable in the Go checkout, start the project-owned headless editor wrapper and reconnect:

```powershell
<project-root>\BatchFiles\RunHeadlessEditor.bat
```

`Plugins\Monolith\Scripts\recover_mcp.ps1` runs that recovery deterministically (probe `/health`, launch the wrapper when no editor-server instance exists, wait, report `RESULT=` plus exit code); see `Docs/specs/SPEC_MonolithAgentOpsScripts.md`.

Then use `monolith_find("<task>")` for routing and `monolith_discover({ "namespace": "<ns>", "action": "<action>", "mode": "schema" })` for exact parameters.

## 2. Add Project Instructions

Use `Templates/CLAUDE.md.example` as the source material for project-level agent instructions. For a project that uses Monolith, add the same Monolith MCP rules to whichever file the agent runtime reads:

- `CLAUDE.md` for Claude Code.
- `AGENTS.md` for Codex and agent runtimes that honor AGENTS files.

At minimum, the project instructions should cover:

- Prefer Monolith MCP over ad-hoc Unreal editor scripting for editor, asset, Blueprint, C++, build, source-control, and project-index work.
- Start from `monolith_find`, `monolith_status`, and focused `monolith_discover` schema reads.
- Use `Binaries\monolith_query.exe` only as the read-only offline fallback when MCP/editor access is down.
- Do not commit `Logs/*`; Monolith invocation logs are local diagnostics.
- For missing editor capability, add a proper Monolith action with schema, tests, and docs instead of relying on one-off `run_python`.

`Scripts\onboard_monolith.ps1` can create or update a managed instruction block for target adapters that declare `instructionFiles`. This repository's `AGENTS.md` and `CLAUDE.md` already include Monolith-specific coordination rules, so the script detects them and skips duplicate insertion.

Keep this onboarding file, `Templates\Onboarding\*.json`, and `Docs/specs/SPEC_MonolithSkillsSymlinkDistribution.md` in sync when changing skill distribution or MCP setup.

## 3. Link Skills Into Codex And Claude

The repository source of truth is:

```text
Skills/<skill-name>/SKILL.md
```

Do not install Monolith skills by copying `SKILL.md` files into product roots. Link each skill directory so edits in this repository are immediately visible to agent targets.

The recommended path is the full onboarding script in section 0. For manual repair of the skill-link step only, run validation first:

```powershell
powershell -ExecutionPolicy Bypass -File Scripts\validate_monolith_skills.ps1
```

Preview Codex and Claude global skill installation. Dry-run is the default:

```powershell
powershell -ExecutionPolicy Bypass -File Scripts\install_monolith_skills.ps1 -Targets Codex,Claude
```

Create links after reviewing the dry-run:

```powershell
powershell -ExecutionPolicy Bypass -File Scripts\install_monolith_skills.ps1 -Targets Codex,Claude -Execute
```

If existing copied installs are present, back them up and replace them with links:

```powershell
powershell -ExecutionPolicy Bypass -File Scripts\install_monolith_skills.ps1 -Targets Codex,Claude -Execute -ReplaceCopies
```

Windows defaults to directory junctions because they work without Developer Mode or administrator privileges in more environments. Use true directory symlinks only when the environment supports them:

```powershell
powershell -ExecutionPolicy Bypass -File Scripts\install_monolith_skills.ps1 -Targets Codex,Claude -Execute -ReplaceCopies -UseSymlink
```

Verify linked installs after execution:

```powershell
powershell -ExecutionPolicy Bypass -File Scripts\validate_monolith_skills.ps1 -InstalledRoots "$env:USERPROFILE\.codex\skills","$env:USERPROFILE\.claude\skills"
```

The installed roots must contain directory links or junctions for Monolith-owned skills, and each linked `SKILL.md` hash must match the repository source.
