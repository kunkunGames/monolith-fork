# Monolith MCP Proxy Configuration

## Current Configuration

The Monolith MCP is configured to use the C++ proxy executable:

```json
{
  "command": "<project-root>/Plugins/Monolith/Binaries/monolith_proxy.exe",
  "args": []
}
```

This configuration is set in:
- `.mcp.json` (project-level)
- `~/.claude.json` (user-level)

## Rollback to Script Proxy (if needed)

If the C++ proxy encounters issues, you can revert to the script proxy by updating both config files.

Windows:
```json
{
  "command": "<project-root>/Plugins/Monolith/Scripts/monolith_proxy.bat",
  "args": []
}
```

macOS / Linux:
```json
{
  "command": "<project-root>/Plugins/Monolith/Scripts/monolith_proxy.sh",
  "args": []
}
```

Update the monolith entry in:
1. `<project-root>/.mcp.json`
2. `%USERPROFILE%\.claude.json` (Windows) or `~/.claude.json` (macOS / Linux)

Then restart Claude Code.

## Proxy Details

- **Script proxy:** `Scripts/monolith_proxy.bat` (Windows) / `Scripts/monolith_proxy.sh` (macOS/Linux) — Stdio-to-HTTP proxy launchers. The Windows launcher probes Python 3.8+, `python3`, Node.js, then `py -3`; the macOS/Linux launcher probes `python3`, `python` (3.8+), then Node.js. They wrap `monolith_proxy.py` or `monolith_proxy.js`, which survive editor restarts via background health polling
- **C++ proxy:** `Plugins/Monolith/Binaries/monolith_proxy.exe` — Native executable, faster startup
- **Backend:** Both connect to the same Monolith HTTP server running in the Unreal Editor
- **Editor-down startup:** Both proxies return a cached Monolith tool list when available, or a stable seed list of namespace/meta tools. This prevents MCP clients that do not fully refresh on `tools/list_changed` from starting with an empty Monolith catalog.
