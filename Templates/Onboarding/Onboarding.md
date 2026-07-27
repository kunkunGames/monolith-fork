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

The native proxy is the default MCP transport because it keeps the client
session alive across editor restarts and serves the fixed read-only fallback
surface while the editor transport is unavailable. Preview it explicitly:

```powershell
powershell -ExecutionPolicy Bypass -File Scripts\onboard_monolith.ps1 -Targets Codex,Claude -McpMode Proxy -Plan
```

Use `-McpMode Http` only for clients that manage HTTP server restarts
themselves or cannot launch stdio MCP commands.

Create a project-scoped `.mcp.json` only for clients that require one:

```powershell
powershell -ExecutionPolicy Bypass -File Scripts\onboard_monolith.ps1 -Targets GenericMcp -ProjectMcpConfig -Plan
```

The script performs the steps in this order:

1. Check Proxy-mode prerequisites before any onboarding mutation.
2. Validate repository skills.
3. Link Monolith skills into selected global skill roots.
4. Configure selected global MCP clients, such as Codex and Claude user-level MCP config.
5. Create, update, or skip project instruction blocks.

Target behavior is data-driven through `Templates\Onboarding\*.json`. Add a new Windows agent target there before changing script logic.

## 1. Configure Monolith MCP

The recommended Codex and Claude setup is the native proxy in global user-level MCP config, not a direct editor URL and not a project `.mcp.json` checked into or left beside the game project. The proxy remains UE-DLL-free and forwards healthy live calls to `http://localhost:9316/mcp`; only a fixed read-only query surface falls back to the immutable Query/catalog pair selected by `Binaries/monolith_query.current.json` when that transport is unavailable.

The onboarding script verifies existing `monolith` entries and adds missing ones through each target CLI when `-Execute` is supplied. It does not overwrite a different existing entry unless `-ReplaceMcpConfig` is also supplied.

In the default `Proxy` mode, `Binaries\monolith_proxy.current.json` is
authoritative. `-Plan` validates it and reports the selected absolute immutable
`Binaries\monolith_proxy-<16-lowercase-source-hash>.exe` path without changing
MCP config. `-Execute` performs the same validation before any onboarding
mutation. Validation requires the exact schema-1 identity fields (`tool` =
`monolith-proxy`, `runtime` = `native-cpp`), exact leaf naming, matching
filename/manifest/`--version` source hashes, matching manifest and
`--version` versions, matching SHA-256 bytes, and a regular non-reparse image
directly inside a regular non-reparse `Binaries` directory. For a fresh source
checkout, build and publish the native proxy first:

```powershell
cmd /c Tools\MonolithProxy\build.bat
```

Alternatively, install a packaged Monolith release containing both the
manifest and its immutable image, then rerun onboarding with `-Execute`.
The fixed `Binaries\monolith_proxy.exe` is compatibility-only; onboarding never
falls back to it. An invalid or missing manifest makes Execute fail before all
mutation, while Plan prints the concrete problem and skips MCP config planning.
`-McpMode Http` has no native-proxy prerequisite. `-SkipMcpConfig` also skips
the prerequisite because it performs no MCP registration; `-SkipSkills`
continues to skip only repository skill validation and skill-link mutation.

### Proxy command (recommended and default)

Use this when the client can launch a stdio MCP process. The onboarding script
uses the manifest-selected immutable absolute path for both global CLI config
and `-ProjectMcpConfig -McpMode Proxy`. The proxy template supplies project
config structure only; onboarding replaces its non-runnable
`__MONOLITH_IMMUTABLE_PROXY_PATH__` placeholder before
writing, so do not install the raw template as an authoritative command.

```json
{
  "mcpServers": {
    "monolith": {
      "command": "D:\\project\\Plugins\\Monolith\\Binaries\\monolith_proxy-0123456789abcdef.exe",
      "args": []
    }
  }
}
```

Existing direct-HTTP entries are replaced only when `-ReplaceMcpConfig` is
explicitly supplied. During one executed global update, the script snapshots
the selected Codex and Claude user config files in memory before the first CLI
mutation, verifies each resulting entry with the owning CLI, and restores all
selected config files byte-for-byte when any remove, add, verification, or
later-client conflict fails. Rollback first compare-and-swaps against the exact
state produced by each CLI mutation and refuses to overwrite a later external
edit. It never prints or leaves a backup copy of those potentially
secret-bearing global config files. Existing proxy entries match only when the
CLI reports exactly one `command:` field whose normalized absolute path equals
the validated immutable image; a wrapper that merely mentions that path in its
arguments is not accepted. `CODEX_HOME` is supported;
when `CLAUDE_CONFIG_DIR` is set, use project config or temporarily remove that
override because Claude's CLI does not expose an authoritative user-config path
for safe rollback. As with any multi-process transaction, a hard process kill
or power loss is outside this in-process rollback guarantee.

Project `.mcp.json` and managed project-instruction writes use a temporary file
in the destination directory followed by an atomic move/replace. Reparse-point
targets are rejected. Project-file replacement creates a timestamped
`.backup-*` copy as part of the same atomic replace.

### Direct HTTP transport (explicit opt-in)

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

`Scripts\onboard_monolith.ps1` can create or update a managed instruction block for target adapters that declare `instructionFiles`. Creates, replacements, and appends are committed with atomic same-directory file replacement so an interrupted write cannot leave a partially written instruction file. This repository's `AGENTS.md` and `CLAUDE.md` already include Monolith-specific coordination rules, so the script detects them and skips duplicate insertion.

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

The current `unreal-cpp` and `monolith-mcp` skills route `source.search_crg_graph`
to the graph-node VIEW/FTS in `Saved\EngineSource.db`. They must not instruct an
agent to build, poll, or maintain a separate graph export. Because installs are
links, updating the checkout updates this routing contract without a copy or
reinstall; run `validate_monolith_skills.ps1` to detect a stale copied install.
