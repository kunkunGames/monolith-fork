#!/usr/bin/env bash
# Monolith MCP Proxy launcher (macOS / Linux).
# Finds Python automatically and runs the proxy.
# Usage in .mcp.json:
#   {"mcpServers": {"monolith": {"command": "Plugins/Monolith/Scripts/monolith_proxy.sh"}}}

set -eu

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" >/dev/null 2>&1 && pwd)"
PROXY_PY="$SCRIPT_DIR/monolith_proxy.py"
PROXY_JS="$SCRIPT_DIR/monolith_proxy.js"

if [ ! -f "$PROXY_PY" ] && [ ! -f "$PROXY_JS" ]; then
	echo "[monolith-proxy] ERROR: proxy scripts not found at $SCRIPT_DIR" 1>&2
	exit 1
fi

# Prefer python3 (standard on macOS 12+ and most Linux distros), fall back to python.
if [ -f "$PROXY_PY" ] && command -v python3 >/dev/null 2>&1; then
	if python3 -c 'import sys; sys.exit(0 if sys.version_info >= (3, 8) else 1)' >/dev/null 2>&1; then
		exec python3 "$PROXY_PY" "$@"
	fi
fi

if [ -f "$PROXY_PY" ] && command -v python >/dev/null 2>&1; then
	# Some systems still ship only `python`; require Python 3.8+.
	if python -c 'import sys; sys.exit(0 if sys.version_info >= (3, 8) else 1)' >/dev/null 2>&1; then
		exec python "$PROXY_PY" "$@"
	fi
fi

# Fallback to Node.js when Python is unavailable or broken.
if command -v node >/dev/null 2>&1; then
	if node -e 'process.exit(parseInt(process.versions.node) >= 18 ? 0 : 1)' >/dev/null 2>&1; then
		if [ -f "$PROXY_JS" ]; then
			exec node "$PROXY_JS" "$@"
		fi
	fi
fi

echo "[monolith-proxy] ERROR: Python 3.8+ or Node.js not found. Install Python 3.8+ or Node.js 18+." 1>&2
exit 1
