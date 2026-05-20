#!/usr/bin/env python3
"""
Monolith MCP stdio-to-HTTP proxy.

Sits between Claude Code (stdio) and Monolith (HTTP on localhost).
Handles initialize locally, forwards tool calls to Monolith.
Survives editor restarts — proxy process never dies.
Background health poll auto-detects when the editor comes online.

Usage (in .mcp.json):
  {"mcpServers": {"monolith": {"command": "python", "args": ["Plugins/Monolith/Scripts/monolith_proxy.py"]}}}

Requirements: Python 3.8+ (stdlib only, no pip install needed)
"""

# PEP 563: defer annotation evaluation so PEP 604 unions (`str | None`) below
# parse on Python 3.8/3.9 too (macOS ships 3.9 by default via Xcode).
from __future__ import annotations

import json
import hashlib
import os
import sys
import threading
import time
import tempfile
import urllib.error
import urllib.request
from datetime import datetime, timezone
from io import TextIOWrapper
from pathlib import Path

MONOLITH_URL = os.environ.get("MONOLITH_URL", "http://localhost:9316/mcp")
MONOLITH_HEALTH = MONOLITH_URL.replace("/mcp", "/health")
PROXY_NAME = "monolith-proxy"
PROXY_VERSION = "1.1.1"
TIMEOUT = 30.0
POLL_INTERVAL = 5.0
POLL_START_DELAY = 3.0

# Track Monolith availability for list_changed notifications
_monolith_was_up = None
_stdout_lock = threading.Lock()
_tool_log_lock = threading.Lock()
_tool_log_sequence = 0
_recent_tool_log_signatures: dict[str, tuple[float, bool]] = {}

_SENSITIVE_KEY_FRAGMENTS = (
    "authorization",
    "bearer",
    "token",
    "api_key",
    "apikey",
    "password",
    "passwd",
    "secret",
    "cookie",
    "private_key",
    "session_id",
)
_MAX_LOG_FIELD_BYTES = 256 * 1024
_REPEAT_LOG_WINDOW_SECONDS = 15.0

CORE_QUERY_TOOLS = [
    "blueprint_query",
    "material_query",
    "animation_query",
    "niagara_query",
    "editor_query",
    "config_query",
    "project_query",
    "source_query",
    "ui_query",
    "mesh_query",
    "gas_query",
    "combograph_query",
    "ai_query",
    "logicdriver_query",
    "audio_query",
    "level_sequence_query",
    "movie_render_query",
]


def _log(msg: str) -> None:
    """Log to stderr (visible in Claude Code debug mode, never interferes with stdio)."""
    print(f"[monolith-proxy] {msg}", file=sys.stderr, flush=True)


def _tool_log_enabled() -> bool:
    return os.environ.get("MONOLITH_TOOL_LOG_ENABLED", "1") != "0"


def _find_plugin_root() -> Path:
    override = os.environ.get("MONOLITH_TOOL_LOG_DIR")
    if override:
        return Path(override)

    here = Path(__file__).resolve()
    for parent in [here.parent, *here.parents]:
        if (parent / "Monolith.uplugin").exists():
            return parent / "Logs"
    return Path.cwd() / "Logs"


def _daily_log_path() -> Path:
    day = datetime.now().strftime("%Y%m%d")
    return _find_plugin_root() / f"{day}_proxy.log"


def _now_iso() -> str:
    return datetime.now().astimezone().isoformat(timespec="milliseconds")


def _redact(value):
    if isinstance(value, dict):
        out = {}
        for key, item in value.items():
            key_text = str(key)
            if any(fragment in key_text.lower() for fragment in _SENSITIVE_KEY_FRAGMENTS):
                out[key_text] = "[REDACTED]"
            else:
                out[key_text] = _redact(item)
        return out
    if isinstance(value, list):
        return [_redact(item) for item in value]
    if isinstance(value, str):
        stripped = value.strip()
        if (stripped.startswith("{") and stripped.endswith("}")) or (stripped.startswith("[") and stripped.endswith("]")):
            try:
                return _redact(json.loads(stripped))
            except Exception:
                return value
    return value


def _json_bytes(value) -> bytes:
    return json.dumps(value, ensure_ascii=False, sort_keys=True, separators=(",", ":")).encode("utf-8", errors="replace")


def _bounded(value, max_bytes: int = _MAX_LOG_FIELD_BYTES):
    data = _json_bytes(value)
    if len(data) <= max_bytes:
        return value, False, len(data), None

    digest = hashlib.sha256(data).hexdigest()
    preview = data[:max_bytes].decode("utf-8", errors="replace")
    return {
        "truncated": True,
        "original_bytes": len(data),
        "sha256": f"sha256:{digest}",
        "preview": preview,
    }, True, len(data), f"sha256:{digest}"


def _retry_signature(tool_name: str, arguments) -> str:
    payload = {"tool": tool_name, "arguments": _redact(arguments)}
    return "sha256:" + hashlib.sha256(_json_bytes(payload)).hexdigest()


def _extract_response(response: str):
    try:
        return json.loads(response)
    except Exception:
        return response


def _classify_response(response_obj, repeated: bool, duration_ms: float, arg_bytes: int, result_bytes: int) -> tuple[str, str | None, int | None, list[str]]:
    outcome = "unknown"
    error_class = None
    error_code = None
    tags: list[str] = []

    if isinstance(response_obj, dict) and "error" in response_obj:
        outcome = "jsonrpc_error"
        error = response_obj.get("error") or {}
        if isinstance(error, dict):
            error_code = error.get("code")
            message = str(error.get("message", ""))
        else:
            message = str(error)
        lower = message.lower()
        if "unknown action" in lower or "method not found" in lower:
            error_class = "unknown_action"
            tags.append("missing_action")
        elif "missing required" in lower or "invalid param" in lower:
            error_class = "missing_param"
            tags.append("schema_confusing")
        else:
            error_class = "jsonrpc_error"
    elif isinstance(response_obj, dict) and response_obj.get("result", {}).get("isError"):
        outcome = "tool_error"
        content = response_obj.get("result", {}).get("content", [])
        message = json.dumps(content, ensure_ascii=False)
        lower = message.lower()
        if "not available" in lower or "not running" in lower or "unreachable" in lower:
            outcome = "editor_unavailable"
            error_class = "editor_unavailable"
            tags.append("editor_unavailable")
        elif "blocked" in lower:
            error_class = "profile_blocked"
            tags.append("profile_blocked")
        else:
            error_class = "tool_error"
    else:
        outcome = "success"

    if repeated:
        tags.append("repeated_call")
    if duration_ms > 5000:
        tags.append("slow_action")
    if result_bytes > _MAX_LOG_FIELD_BYTES:
        tags.append("large_result")
    if arg_bytes > _MAX_LOG_FIELD_BYTES:
        tags.append("large_result")

    return outcome, error_class, error_code, sorted(set(tags))


def _append_tool_log(record: dict) -> None:
    if not _tool_log_enabled():
        return

    try:
        path = _daily_log_path()
        line = json.dumps(record, ensure_ascii=False, separators=(",", ":")) + "\n"
        with _tool_log_lock:
            path.parent.mkdir(parents=True, exist_ok=True)
            lock_path = path.with_suffix(path.suffix + ".lock")
            with open(lock_path, "a+", encoding="utf-8") as lock_file:
                if os.name == "nt":
                    import msvcrt

                    lock_file.seek(0)
                    if not lock_file.read(1):
                        lock_file.write("0")
                        lock_file.flush()
                    lock_file.seek(0)
                    msvcrt.locking(lock_file.fileno(), msvcrt.LK_LOCK, 1)
                    try:
                        with open(path, "a", encoding="utf-8", newline="\n") as log_file:
                            log_file.write(line)
                    finally:
                        lock_file.seek(0)
                        msvcrt.locking(lock_file.fileno(), msvcrt.LK_UNLCK, 1)
                else:
                    import fcntl

                    fcntl.flock(lock_file.fileno(), fcntl.LOCK_EX)
                    try:
                        with open(path, "a", encoding="utf-8", newline="\n") as log_file:
                            log_file.write(line)
                    finally:
                        fcntl.flock(lock_file.fileno(), fcntl.LOCK_UN)
    except Exception as e:
        _log(f"Tool daily log failed: {e}")


def _log_tools_call(msg: dict, start_time: str, start_perf: float, response: str, repeated: bool, retry_signature: str) -> None:
    if not _tool_log_enabled():
        return

    global _tool_log_sequence
    end_time = _now_iso()
    duration_ms = (time.perf_counter() - start_perf) * 1000.0
    params = msg.get("params", {}) if isinstance(msg.get("params"), dict) else {}
    tool_name = params.get("name", "unknown")
    arguments = params.get("arguments", {})
    redacted_args = _redact(arguments)
    response_obj = _extract_response(response)
    redacted_response = _redact(response_obj)
    bounded_args, args_truncated, arg_bytes, arg_hash = _bounded(redacted_args)
    bounded_response, response_truncated, result_bytes, result_hash = _bounded(redacted_response)
    outcome, error_class, error_code, tags = _classify_response(
        response_obj, repeated, duration_ms, arg_bytes, result_bytes)

    with _tool_log_lock:
        _tool_log_sequence += 1
        sequence = _tool_log_sequence

    record = {
        "format_version": 1,
        "surface": "proxy",
        "sequence": sequence,
        "start_time": start_time,
        "end_time": end_time,
        "duration_ms": round(duration_ms, 3),
        "pid": os.getpid(),
        "thread_id": threading.get_ident(),
        "status": "success" if outcome == "success" else "error",
        "client": {
            "name": "unknown",
            "version": "",
            "protocol_version": "",
            "proxy_runtime": "python",
            "proxy_version": PROXY_VERSION,
        },
        "call": {
            "jsonrpc_id": msg.get("id"),
            "tool_name_original": tool_name,
            "tool_name_forwarded": tool_name,
            "arguments": bounded_args,
            "retry_signature": retry_signature,
        },
        "return": {
            "jsonrpc_id": response_obj.get("id") if isinstance(response_obj, dict) else None,
            "response": bounded_response,
            "result_bytes": result_bytes,
        },
        "redaction": {
            "applied": True,
            "truncated": args_truncated or response_truncated,
            "argument_bytes": arg_bytes,
            "result_bytes": result_bytes,
            "argument_sha256": arg_hash,
            "result_sha256": result_hash,
        },
        "agent_signal": {
            "outcome": outcome,
            "error_code": error_code,
            "error_class": error_class or "",
            "hints_returned": 0,
            "discovery_context": "unknown",
            "retry_signature": retry_signature,
            "repeat_within_window": repeated,
            "argument_bytes": arg_bytes,
            "result_bytes": result_bytes,
            "improvement_tags": tags,
        },
    }
    _append_tool_log(record)


def _post_monolith(body: str, timeout: float = TIMEOUT) -> str | None:
    """POST JSON-RPC to Monolith. Returns response body or None on failure."""
    try:
        req = urllib.request.Request(
            MONOLITH_URL,
            data=body.encode("utf-8"),
            headers={"Content-Type": "application/json"},
            method="POST",
        )
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            return resp.read().decode("utf-8")
    except (urllib.error.URLError, OSError, TimeoutError) as e:
        _log(f"Monolith unreachable: {e}")
        return None


def _write(stdout, msg: str) -> None:
    """Write a JSON-RPC message to stdout (thread-safe)."""
    with _stdout_lock:
        stdout.write(msg + "\n")
        stdout.flush()


def _result(id, result: dict) -> str:
    return json.dumps({"jsonrpc": "2.0", "id": id, "result": result})


def _tool_error(id, message: str) -> str:
    """Return a tool result with isError=true (graceful failure, not protocol error)."""
    return json.dumps({
        "jsonrpc": "2.0",
        "id": id,
        "result": {
            "content": [{"type": "text", "text": message}],
            "isError": True,
        },
    })


def _jsonrpc_error(id, code: int, message: str) -> str:
    """Return a JSON-RPC protocol-level error."""
    return json.dumps({
        "jsonrpc": "2.0",
        "id": id,
        "error": {"code": code, "message": message},
    })


def _sanitize_cache_part(value: str) -> str:
    return "".join(c if c.isalnum() or c in "-_" else "_" for c in value)


def _tools_cache_path() -> Path:
    base = Path(os.environ.get("LOCALAPPDATA") or tempfile.gettempdir())
    cache_dir = base / "Monolith"
    cache_dir.mkdir(parents=True, exist_ok=True)

    host_port = MONOLITH_HEALTH.replace("http://", "").replace("https://", "")
    host_port = host_port.split("/", 1)[0]
    return cache_dir / f"monolith_proxy_tools_{_sanitize_cache_part(host_port)}.json"


def _query_tool_schema() -> dict:
    return {
        "type": "object",
        "properties": {
            "action": {
                "type": "string",
                "description": "The action to execute. Use monolith_discover first when the editor is available.",
            },
            "params": {
                "type": "object",
                "description": "Parameters for the selected action.",
            },
        },
        "required": ["action"],
    }


def _empty_object_schema() -> dict:
    return {"type": "object", "properties": {}}


def _make_tool(name: str, description: str, schema: dict) -> dict:
    return {"name": name, "description": description, "inputSchema": schema}


def _seed_tools() -> list[dict]:
    tools = []
    for name in CORE_QUERY_TOOLS:
        domain = name[:-6] if name.endswith("_query") else name
        tools.append(_make_tool(
            name,
            f"Query the {domain} domain. The editor may be offline at session start; retry after Monolith is healthy.",
            _query_tool_schema(),
        ))

    tools.append(_make_tool(
        "monolith_discover",
        "List available tool namespaces and their actions. Pass namespace and optional category to filter.",
        {
            "type": "object",
            "properties": {
                "namespace": {"type": "string", "description": "Optional: filter to a specific namespace"},
                "category": {"type": "string", "description": "Optional: filter actions within the namespace by category"},
            },
        },
    ))
    tools.append(_make_tool(
        "monolith_status",
        "Get Monolith server health: version, uptime, port, registered action count, and module status.",
        _empty_object_schema(),
    ))
    tools.append(_make_tool(
        "monolith_update",
        "Check for or install Monolith updates from GitHub Releases.",
        {
            "type": "object",
            "properties": {
                "action": {
                    "type": "string",
                    "description": "'check' to compare versions, 'install' to download and stage update",
                    "default": "check",
                }
            },
        },
    ))
    tools.append(_make_tool(
        "monolith_reindex",
        "Re-index the Monolith project database. Requires the editor-side Monolith server.",
        _empty_object_schema(),
    ))
    return tools


def _write_tools_cache(resp: str) -> None:
    try:
        payload = json.loads(resp)
        tools = payload.get("result", {}).get("tools", [])
        if isinstance(tools, list) and tools:
            _tools_cache_path().write_text(json.dumps(tools), encoding="utf-8")
    except Exception as e:
        _log(f"Failed to write tools/list cache: {e}")


def _read_tools_cache() -> list[dict] | None:
    try:
        path = _tools_cache_path()
        if not path.exists():
            return None
        tools = json.loads(path.read_text(encoding="utf-8"))
        if isinstance(tools, list) and tools:
            return tools
    except Exception as e:
        _log(f"Failed to read tools/list cache: {e}")
    return None


def _fallback_tools_list(msg: dict) -> str:
    cached = _read_tools_cache()
    if cached:
        _log("Monolith down during tools/list — returning cached tools")
        return _result(msg.get("id"), {"tools": cached})

    _log("Monolith down during tools/list — returning seed tools")
    return _result(msg.get("id"), {"tools": _seed_tools()})


def _check_monolith_up() -> bool:
    """Lightweight health check via GET /health endpoint."""
    try:
        req = urllib.request.Request(MONOLITH_HEALTH, method="GET")
        with urllib.request.urlopen(req, timeout=3) as resp:
            return resp.status == 200
    except Exception:
        return False


def _send_list_changed(stdout) -> bool:
    """Send tools/list_changed notification. Returns False if stdout is broken."""
    try:
        _write(stdout, json.dumps({
            "jsonrpc": "2.0",
            "method": "notifications/tools/list_changed",
        }))
        return True
    except (BrokenPipeError, OSError):
        return False


def check_monolith_state_change(stdout) -> None:
    """Check for state transition and notify if changed."""
    global _monolith_was_up
    is_up = _check_monolith_up()

    if _monolith_was_up is not None and is_up != _monolith_was_up:
        direction = "online" if is_up else "offline"
        _log(f"Monolith went {direction} — sending tools/list_changed")
        _send_list_changed(stdout)

    _monolith_was_up = is_up


def _health_poll_thread(stdout) -> None:
    """Background thread that polls Monolith and sends list_changed on state transitions."""
    time.sleep(POLL_START_DELAY)
    _log(f"Health poll started (interval={POLL_INTERVAL}s)")

    while True:
        try:
            check_monolith_state_change(stdout)
        except (BrokenPipeError, OSError):
            _log("stdout broken, health poll exiting")
            return
        except Exception as e:
            _log(f"Health poll error: {e}")

        time.sleep(POLL_INTERVAL)


def handle_initialize(msg: dict) -> str:
    """Handle initialize locally. Proxy is always available."""
    client_version = msg.get("params", {}).get("protocolVersion", "2025-11-25")
    supported = {"2024-11-05", "2025-03-26", "2025-06-18", "2025-11-25"}
    version = client_version if client_version in supported else "2025-11-25"

    return _result(msg.get("id"), {
        "protocolVersion": version,
        "capabilities": {
            "tools": {"listChanged": True},
        },
        "serverInfo": {"name": PROXY_NAME, "version": PROXY_VERSION},
        "instructions": (
            "Monolith MCP proxy. Tools are forwarded to the Unreal Editor. "
            "If tools return errors about the editor not running, wait and retry."
        ),
    })


def handle_ping(msg: dict) -> str:
    return _result(msg.get("id"), {})


def handle_tools_list(msg: dict) -> str:
    """Forward tools/list to Monolith. Stable cached/seed list if down."""
    resp = _post_monolith(json.dumps(msg))
    if resp:
        _write_tools_cache(resp)
        return resp
    return _fallback_tools_list(msg)


def handle_tools_call(msg: dict) -> str:
    """Forward tools/call to Monolith. Graceful error if down."""
    start_time = _now_iso()
    start_perf = time.perf_counter()
    params = msg.get("params", {}) if isinstance(msg.get("params"), dict) else {}
    tool_name = params.get("name", "unknown")
    arguments = params.get("arguments", {})
    retry_signature = _retry_signature(tool_name, arguments)
    now = time.monotonic()
    previous = _recent_tool_log_signatures.get(retry_signature)
    repeated = bool(previous and now - previous[0] <= _REPEAT_LOG_WINDOW_SECONDS)

    resp = _post_monolith(json.dumps(msg))
    if resp:
        try:
            response_obj = _extract_response(resp)
            failed = isinstance(response_obj, dict) and (
                "error" in response_obj or response_obj.get("result", {}).get("isError"))
            _recent_tool_log_signatures[retry_signature] = (now, failed)
            _log_tools_call(msg, start_time, start_perf, resp, repeated, retry_signature)
        except Exception as e:
            _log(f"Tool daily log wrapper failed: {e}")
        return resp

    response = _tool_error(
        msg.get("id"),
        f"Monolith MCP is not available (Unreal Editor not running). "
        f"Tool '{tool_name}' cannot execute. Start the editor and try again.",
    )
    _recent_tool_log_signatures[retry_signature] = (now, True)
    _log_tools_call(msg, start_time, start_perf, response, repeated, retry_signature)
    return response


def main() -> None:
    # Use binary-safe IO for Windows compatibility
    stdin = TextIOWrapper(sys.stdin.buffer, encoding="utf-8", newline="\n")
    stdout = TextIOWrapper(sys.stdout.buffer, encoding="utf-8", newline="\n")

    _log(f"Started. Forwarding to {MONOLITH_URL}")

    # Start background health poller
    poller = threading.Thread(
        target=_health_poll_thread,
        args=(stdout,),
        daemon=True,
        name="monolith-health-poll",
    )
    poller.start()

    for line in stdin:
        line = line.strip()
        if not line:
            continue

        try:
            msg = json.loads(line)
        except json.JSONDecodeError as e:
            _log(f"Bad JSON: {e}")
            continue

        method = msg.get("method", "")
        msg_id = msg.get("id")  # None for notifications
        response = None

        if method == "initialize":
            response = handle_initialize(msg)
            _log("Initialized")

        elif method in ("notifications/initialized", "initialized"):
            # Notification — no response. Check if Monolith is up.
            check_monolith_state_change(stdout)

        elif method == "ping":
            response = handle_ping(msg)

        elif method == "tools/list":
            check_monolith_state_change(stdout)
            response = handle_tools_list(msg)

        elif method == "tools/call":
            response = handle_tools_call(msg)

        else:
            # Forward unknown methods to Monolith
            resp = _post_monolith(json.dumps(msg))
            if resp:
                response = resp
            elif msg_id is not None:
                response = _jsonrpc_error(msg_id, -32601, f"Method not found: {method}")

        if response:
            _write(stdout, response)


if __name__ == "__main__":
    main()
