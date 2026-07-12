#!/usr/bin/env python3
"""
Monolith MCP stdio-to-HTTP proxy.

Sits between Claude Code (stdio) and Monolith (HTTP on localhost).
Handles initialize locally, forwards tool calls to Monolith.
Survives editor restarts — proxy process never dies.
Background health poll auto-detects when the editor comes online.

Usage (in .mcp.json):
  {"mcpServers": {"monolith": {"command": "<project-root>/Plugins/Monolith/Scripts/monolith_proxy.sh"}}} (or .bat on Windows)

Requirements: Python 3.8+ (stdlib only, no pip install needed)
"""

# PEP 563: defer annotation evaluation so PEP 604 unions (`str | None`) below
# parse on Python 3.8/3.9 too (macOS ships 3.9 by default via Xcode).
from __future__ import annotations

import hashlib
import json
import os
import socket
import sys
import threading
import time
import tempfile
import urllib.error
import urllib.request
from datetime import datetime
from io import TextIOWrapper
from pathlib import Path

MONOLITH_URL = os.environ.get("MONOLITH_URL", "http://localhost:9316/mcp")
MONOLITH_HEALTH = MONOLITH_URL.replace("/mcp", "/health")
PROXY_NAME = "monolith-proxy"
PROXY_VERSION = "1.1.1"
TIMEOUT = 30.0
POLL_INTERVAL = 5.0
POLL_START_DELAY = 3.0
# Transient-connection retry: the editor MCP endpoint at 9316 flickers (a request fails to
# connect, the next succeeds) during busy/GC/build windows. Retry ONLY send-side connection
# failures — the request never reached the server, so retrying cannot double-execute a
# mutation. Read timeouts are deliberately NOT retried (the request may already be applied).
CONNECT_RETRIES = max(0, int(os.environ.get("MONOLITH_CONNECT_RETRIES", "3")))
CONNECT_RETRY_BACKOFF = 0.25
# Total wall-clock budget for retries. Real editor flicker refuses in ~ms, so several retries
# fit easily; a fully-down/blackholed endpoint refuses slowly, so the budget bails after ~1
# slow attempt instead of stacking timeouts (keeps the offline graceful-error path prompt).
CONNECT_RETRY_BUDGET = float(os.environ.get("MONOLITH_CONNECT_RETRY_BUDGET", "1.5"))

# Track Monolith availability for list_changed notifications
_monolith_was_up = None
_stdout_lock = threading.Lock()
_tool_log_lock = threading.Lock()
_tool_log_sequence = 0
_process_instance_id: str | None = None
_last_record_id: str | None = None
_last_record_start_perf: float | None = None
_last_error_record_id: str | None = None
_recent_find: dict | None = None
_recent_discover: dict | None = None
_recent_tool_log_signatures: dict[str, dict] = {}

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
_DEFAULT_MAX_LOG_FIELD_BYTES = 256 * 1024
_REPEAT_LOG_WINDOW_SECONDS = 15.0

# Call-log state (Phase 4 / survivor F)
#
# NOTE: Saved/Logs/MonolithCalls.jsonl is project-root-relative and excluded
# from crash zip generation by UE's crash reporter (Saved/Logs/ tail capture
# only includes editor logs, not arbitrary jsonl). If a crash collector pattern
# elsewhere DOES sweep Saved/Logs/*, the user should add MonolithCalls.jsonl to
# the exclusion list. Single-user local dev tool; no phone-home.
# Per-namespace *_query seed tools are no longer used — agents dispatch via
# the single monolith_query tool. Kept empty for structural compatibility.
CORE_QUERY_TOOLS = []


def _log(msg: str) -> None:
    """Log to stderr (visible in Claude Code debug mode, never interferes with stdio)."""
    print(f"[monolith-proxy] {msg}", file=sys.stderr, flush=True)


def _tool_log_enabled() -> bool:
    return os.environ.get("MONOLITH_TOOL_LOG_ENABLED", "1") != "0"


def _max_log_field_bytes() -> int:
    raw = os.environ.get("MONOLITH_TOOL_LOG_MAX_FIELD_BYTES")
    if not raw:
        return _DEFAULT_MAX_LOG_FIELD_BYTES
    try:
        return max(1024, min(int(raw), 16 * 1024 * 1024))
    except ValueError:
        return _DEFAULT_MAX_LOG_FIELD_BYTES


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
    return _find_plugin_root() / day / "proxy.jsonl"


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


def _bounded(value, max_bytes: int | None = None):
    if max_bytes is None:
        max_bytes = _max_log_field_bytes()
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


def _make_log_id(prefix: str, payload: str) -> str:
    digest = hashlib.sha256(payload.encode("utf-8", errors="replace")).hexdigest()
    return f"{prefix}-{digest[:32]}"


def _get_process_instance_id() -> str:
    global _process_instance_id
    if _process_instance_id is None:
        _process_instance_id = _make_log_id("proc", f"proxy-python:{os.getpid()}:{_now_iso()}")
    return _process_instance_id


def _tool_namespace_action(tool_name: str, arguments) -> tuple[str, str]:
    args = arguments if isinstance(arguments, dict) else {}
    if tool_name.startswith("monolith_"):
        return "monolith", tool_name[len("monolith_"):]
    if tool_name.endswith("_query"):
        return tool_name[:-len("_query")], str(args.get("action") or "")
    return "", ""


def _namespace_source(tool_name: str) -> str:
    if tool_name.startswith("monolith_"):
        return "core_tool"
    if tool_name.endswith("_query"):
        return "domain_query"
    return "unknown"


def _infer_intent(namespace: str, action: str, outcome: str) -> tuple[str, str]:
    ns = namespace.lower()
    act = action.lower()
    if ns == "monolith" and act in ("find", "discover"):
        return "schema_discovery", "high"
    if any(token in act for token in ("health", "status", "check", "validate", "test")):
        return "verification", "medium"
    if ns == "source" or any(token in act for token in ("source", "symbol", "reference", "caller", "callee")):
        return "source_lookup", "medium"
    if ns in ("project", "asset") or "asset" in act:
        return "asset_search", "medium"
    if ns == "editor" and any(token in act for token in ("build", "compile", "log", "crash")):
        return "build_diagnostics", "medium"
    if any(token in act for token in ("repair", "reindex", "rebuild", "snapshot")):
        return "maintenance", "medium"
    if outcome != "success":
        return "error_recovery", "low"
    if act.startswith(("create", "set", "add", "remove", "delete", "import", "build")):
        return "mutation", "medium"
    return "unknown", "low"


def _workflow_step(intent: str, outcome: str) -> str:
    if outcome != "success":
        return "recover"
    if intent == "schema_discovery":
        return "discover"
    if intent == "verification":
        return "verify"
    if intent == "maintenance":
        return "maintenance"
    if intent == "mutation":
        return "execute"
    if intent in ("source_lookup", "asset_search", "build_diagnostics"):
        return "inspect"
    return "unknown"


def _with_trace(msg: dict, trace_id: str, parent_span_id: str, routing_context: dict, session_key: str) -> dict:
    forwarded = dict(msg)
    forwarded["_monolith_trace_id"] = trace_id
    forwarded["_monolith_parent_span_id"] = parent_span_id
    forwarded["_monolith_session_key"] = session_key
    forwarded["_monolith_routing_context"] = routing_context
    return forwarded


def _drop_empty(value):
    if isinstance(value, dict):
        cleaned = {}
        for key, item in value.items():
            cleaned_item = _drop_empty(item)
            if cleaned_item is None or cleaned_item == "" or cleaned_item == [] or cleaned_item == {}:
                continue
            cleaned[key] = cleaned_item
        return cleaned
    if isinstance(value, list):
        return [_drop_empty(item) for item in value if _drop_empty(item) not in (None, "", [], {})]
    return value


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
    max_field_bytes = _max_log_field_bytes()
    if result_bytes > max_field_bytes:
        tags.append("large_result")
    if arg_bytes > max_field_bytes:
        tags.append("large_result")

    return outcome, error_class, error_code, sorted(set(tags))


def _summarize_response(response_obj, result_bytes: int, truncated: bool) -> dict:
    summary = {
        "result_bytes": result_bytes,
        "truncated": truncated,
    }
    if isinstance(response_obj, dict):
        if "error" in response_obj:
            summary["result_shape"] = "error"
        elif isinstance(response_obj.get("result"), dict) and isinstance(response_obj.get("result", {}).get("content"), list):
            summary["result_shape"] = "mcp_content"
        elif isinstance(response_obj.get("result"), dict):
            summary["result_shape"] = "object"
        elif isinstance(response_obj.get("result"), list):
            summary["result_shape"] = "list"
        elif response_obj.get("result") is None:
            summary["result_shape"] = "empty"
        else:
            summary["result_shape"] = "text"
        summary["response_top_keys"] = sorted(response_obj.keys())[:20]
        if "error" in response_obj:
            error = response_obj.get("error") or {}
            if isinstance(error, dict):
                summary["jsonrpc_error_code"] = error.get("code")
                summary["jsonrpc_error_message"] = str(error.get("message", ""))[:240]
            else:
                summary["jsonrpc_error_message"] = str(error)[:240]
        result = response_obj.get("result")
        if isinstance(result, dict):
            summary["result_top_keys"] = sorted(result.keys())[:20]
            if "isError" in result:
                summary["is_error"] = bool(result.get("isError"))
            content = result.get("content")
            if isinstance(content, list):
                summary["content_count"] = len(content)
            tools = result.get("tools")
            if isinstance(tools, list):
                summary["tools_count"] = len(tools)
            resources = result.get("resources")
            if isinstance(resources, list):
                summary["resources_count"] = len(resources)
        elif result is not None:
            summary["result_type"] = type(result).__name__
    else:
        summary["response_type"] = type(response_obj).__name__
    return _drop_empty(summary)


def _build_routing_context(
    tool_name: str,
    arguments,
    retry_signature: str,
    repeated: bool,
    outcome: str,
    namespace: str,
    action: str,
    intent: str,
    confidence: str,
) -> dict:
    decision_source = "direct"
    discovery_root_record_id = None
    matched_discovered_action = False
    if repeated:
        previous = _recent_tool_log_signatures.get(retry_signature)
        if previous and previous.get("failed"):
            decision_source = "retry_after_error"
        else:
            decision_source = "fallback"
    elif _recent_discover:
        discovered_namespace = _recent_discover.get("namespace")
        discovered_action = _recent_discover.get("action")
        if discovered_namespace and discovered_namespace == namespace and (not discovered_action or discovered_action == action):
            decision_source = "after_discover"
            matched_discovered_action = True
            discovery_root_record_id = _recent_discover.get("record_id")
    elif _recent_find and tool_name not in ("monolith_find", "monolith_discover"):
        decision_source = "after_find"
        discovery_root_record_id = _recent_find.get("record_id")

    routing = {
        "decision_source": decision_source,
        "namespace_source": _namespace_source(tool_name),
        "recent_find_trace_id": _recent_find.get("trace_id") if _recent_find else None,
        "recent_discover_trace_id": _recent_discover.get("trace_id") if _recent_discover else None,
        "matched_discovered_action": matched_discovered_action,
        "inferred_intent": intent,
        "intent_confidence": confidence,
        "discovery_root_record_id": discovery_root_record_id,
    }
    return _drop_empty(routing)


def _remember_tool_outcome(tool_name: str, arguments, trace_id: str, record_id: str | None, retry_signature: str, now: float, failed: bool) -> None:
    global _recent_find
    global _recent_discover
    global _last_error_record_id
    if not record_id:
        return

    args = arguments if isinstance(arguments, dict) else {}
    namespace, action = _tool_namespace_action(tool_name, args)
    _recent_tool_log_signatures[retry_signature] = {
        "at": now,
        "failed": failed,
        "record_id": record_id,
    }
    if failed:
        _last_error_record_id = record_id
    elif _last_error_record_id:
        _last_error_record_id = None

    if tool_name == "monolith_find":
        _recent_find = {"trace_id": trace_id, "record_id": record_id}
    elif tool_name == "monolith_discover":
        _recent_discover = {
            "trace_id": trace_id,
            "record_id": record_id,
            "namespace": str(args.get("namespace") or ""),
            "action": str(args.get("action") or ""),
        }
    elif namespace and action and _recent_discover:
        discovered_namespace = _recent_discover.get("namespace")
        discovered_action = _recent_discover.get("action")
        if discovered_namespace == namespace and (not discovered_action or discovered_action == action):
            _recent_discover = None


def _append_tool_log(record: dict) -> float | None:
    if not _tool_log_enabled():
        return None

    write_start = time.perf_counter()
    try:
        path = _daily_log_path()
        line = json.dumps(_drop_empty(record), ensure_ascii=False, separators=(",", ":")) + "\n"
        with _tool_log_lock:
            path.parent.mkdir(parents=True, exist_ok=True)
            lock_path = path.with_suffix(path.suffix + ".lock")
            lock_fd = None
            lock_start = time.time()
            while lock_fd is None:
                try:
                    lock_fd = os.open(str(lock_path), os.O_CREAT | os.O_EXCL | os.O_WRONLY)
                    os.write(lock_fd, b"0")
                except FileExistsError:
                    try:
                        if time.time() - lock_path.stat().st_mtime > 30:
                            lock_path.unlink()
                    except FileNotFoundError:
                        pass
                    if time.time() - lock_start > 5:
                        raise TimeoutError("timed out acquiring tool log lock")
                    time.sleep(0.025)
            try:
                with open(path, "a", encoding="utf-8", newline="\n") as log_file:
                    log_file.write(line)
            finally:
                if lock_fd is not None:
                    os.close(lock_fd)
                try:
                    lock_path.unlink()
                except FileNotFoundError:
                    pass
    except Exception as e:
        _log(f"Tool daily log failed: {e}")
        return None
    return (time.perf_counter() - write_start) * 1000.0


def _log_tools_call(
    msg: dict,
    start_time: str,
    start_perf: float,
    response: str,
    repeated: bool,
    retry_signature: str,
    trace_id: str,
    span_id: str,
) -> str | None:
    if not _tool_log_enabled():
        return None

    log_prepare_start = time.perf_counter()
    global _tool_log_sequence
    global _last_record_id
    global _last_record_start_perf
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
    namespace, action = _tool_namespace_action(tool_name, arguments)
    intent, confidence = _infer_intent(namespace, action, outcome)

    with _tool_log_lock:
        _tool_log_sequence += 1
        sequence = _tool_log_sequence
        process_id = _get_process_instance_id()
        record_id = _make_log_id("rec", f"{process_id}:proxy:{sequence}:{trace_id}:{span_id}:{start_time}")
        previous_record_id = _last_record_id
        previous_start_perf = _last_record_start_perf
        _last_record_id = record_id
        _last_record_start_perf = start_perf

    return_summary = _summarize_response(response_obj, result_bytes, args_truncated or response_truncated)

    redaction = {
        "argument_bytes": arg_bytes,
        "result_bytes": result_bytes,
    }
    if args_truncated or response_truncated:
        redaction["truncated"] = True
    if arg_hash:
        redaction["argument_sha256"] = arg_hash
    if result_hash:
        redaction["result_sha256"] = result_hash

    agent_signal = {
        "outcome": outcome,
        "hints_returned": 0,
    }
    if error_code is not None:
        agent_signal["error_code"] = error_code
    if error_class:
        agent_signal["error_class"] = error_class
    if repeated:
        agent_signal["repeat_within_window"] = True
    if tags:
        agent_signal["improvement_tags"] = tags

    routing_context = _build_routing_context(tool_name, arguments, retry_signature, repeated, outcome, namespace, action, intent, confidence)
    workflow = {
        "step": _workflow_step(intent, outcome),
    }
    previous_signature = _recent_tool_log_signatures.get(retry_signature)
    if previous_signature and previous_signature.get("record_id") and repeated:
        workflow["retry_of_record_id"] = previous_signature["record_id"]
    if _last_error_record_id and outcome == "success":
        workflow["recovery_from_record_id"] = _last_error_record_id
    if routing_context.get("discovery_root_record_id"):
        workflow["discovery_root_record_id"] = routing_context["discovery_root_record_id"]

    phase_timing = _drop_empty({
        "parse_ms": msg.get("_monolith_parse_ms"),
        "rewrite_ms": msg.get("_monolith_rewrite_ms"),
        "dedup_ms": msg.get("_monolith_dedup_ms"),
        "http_roundtrip_ms": msg.get("_monolith_http_roundtrip_ms"),
        "fallback_ms": msg.get("_monolith_fallback_ms"),
    })
    phase_timing["log_prepare_ms"] = (time.perf_counter() - log_prepare_start) * 1000.0
    if previous_record_id and previous_start_perf is not None:
        time_since_previous_ms = (start_perf - previous_start_perf) * 1000.0
    else:
        time_since_previous_ms = None

    return_record = {
        "response": bounded_response,
    }
    response_id = response_obj.get("id") if isinstance(response_obj, dict) else None
    if response_id != msg.get("id"):
        return_record["jsonrpc_id"] = response_id

    record = {
        "format_version": 3,
        "surface": "proxy",
        "record_id": record_id,
        "sequence": sequence,
        "trace_id": trace_id,
        "span_id": span_id,
        "session_key": "stateless",
        "process_instance_id": process_id,
        "call_index": sequence,
        "previous_record_id": previous_record_id,
        "time_since_previous_ms": time_since_previous_ms,
        "start_time": start_time,
        "end_time": end_time,
        "duration_ms": round(duration_ms, 3),
        "pid": os.getpid(),
        "thread_id": threading.get_ident(),
        "status": "success" if outcome == "success" else "error",
        "client": {
            "proxy_runtime": "python",
            "proxy_version": PROXY_VERSION,
        },
        "routing_context": routing_context,
        "workflow": workflow,
        "phase_timing": phase_timing,
        "call": {
            "jsonrpc_id": msg.get("id"),
            "tool_name_original": tool_name,
            "tool_name_forwarded": tool_name,
            "arguments": bounded_args,
            "retry_signature": retry_signature,
        },
        "return": return_record,
        "return_summary": return_summary,
        "redaction": redaction,
        "agent_signal": agent_signal,
    }
    write_ms = _append_tool_log(record)
    if write_ms is not None:
        # `log_write_ms` is intentionally not patched into the already-written row;
        # tests and analyzers should treat it as optional for script proxies.
        pass
    return record_id


def _is_timeout_error(e: BaseException) -> bool:
    """A read timeout means the request may already have been processed — never retry it."""
    if isinstance(e, (TimeoutError, socket.timeout)):
        return True
    reason = getattr(e, "reason", None)
    if isinstance(reason, (TimeoutError, socket.timeout)):
        return True
    return "timed out" in str(reason if reason is not None else e).lower()


def _is_retryable_connection_error(e: BaseException) -> bool:
    """True only for send-side connection failures (request never reached the server)."""
    if _is_timeout_error(e):
        return False
    if isinstance(e, ConnectionError):
        return True
    reason = getattr(e, "reason", None)
    if isinstance(reason, ConnectionError):
        return True
    text = str(reason if reason is not None else e).lower()
    return any(s in text for s in (
        "refused", "actively refused", "reset", "aborted",
        "cannot connect", "failed to establish", "connection error", "not connected",
    ))


def _post_monolith(body: str, timeout: float = TIMEOUT, retry: bool = False) -> str | None:
    """POST JSON-RPC to Monolith. Returns response body or None on failure.

    When ``retry`` is set, retries transient send-side connection failures (endpoint flicker)
    with bounded backoff; read timeouts and valid JSON-RPC error responses are returned as-is.
    Retry is enabled only for ``tools/call`` (real action forwarding where a transient drop
    loses work). ``tools/list`` and unknown-method forwards stay fast-fail because they have a
    cached/seed fallback — retrying there would only delay the offline path.
    """
    attempt = 0
    deadline = time.monotonic() + CONNECT_RETRY_BUDGET
    while True:
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
            if (retry and attempt < CONNECT_RETRIES and time.monotonic() < deadline
                    and _is_retryable_connection_error(e)):
                attempt += 1
                _log(f"Monolith transient connection failure (attempt {attempt}/{CONNECT_RETRIES}), retrying: {e}")
                time.sleep(CONNECT_RETRY_BACKOFF * attempt)
                continue
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
    if sys.platform == "win32":
        base = Path(os.environ.get("LOCALAPPDATA") or tempfile.gettempdir())
    else:
        base = Path(os.environ.get("XDG_CACHE_HOME") or Path.home() / ".cache")
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
            "_fields": {
                "type": "array",
                "items": {"type": "string"},
                "description": "Optional top-level whitelist — return only these top-level fields of the response. Mutually exclusive with _omit.",
            },
            "_omit": {
                "type": "array",
                "items": {"type": "string"},
                "description": "Optional top-level blacklist — remove these top-level fields from the response. Mutually exclusive with _fields.",
            },
            "_compact_json": {
                "type": "boolean",
                "description": "Optional — when true, drop top-level fields whose value is null, empty string, empty array, or empty object.",
            },
        },
        "required": ["action"],
    }


def _empty_object_schema() -> dict:
    return {
        "type": "object",
        "properties": {
            "_fields": {
                "type": "array",
                "items": {"type": "string"},
                "description": "Optional top-level whitelist — return only these top-level fields of the response. Mutually exclusive with _omit.",
            },
            "_omit": {
                "type": "array",
                "items": {"type": "string"},
                "description": "Optional top-level blacklist — remove these top-level fields from the response. Mutually exclusive with _fields.",
            },
            "_compact_json": {
                "type": "boolean",
                "description": "Optional — when true, drop top-level fields whose value is null, empty string, empty array, or empty object.",
            },
        },
    }


def _make_tool(name: str, description: str, schema: dict) -> dict:
    return {"name": name, "description": description, "inputSchema": schema}


def _monolith_query_tool() -> dict:
    return _make_tool(
        "monolith_query",
        "Execute any Monolith action. Use monolith_find(query) to locate the right action, "
        "monolith_discover(namespace) to inspect its schema, then call monolith_query with "
        "the resolved namespace, action, and params.",
        {
            "type": "object",
            "properties": {
                "namespace": {
                    "type": "string",
                    "description": "Target namespace, e.g. 'blueprint', 'source', 'gas'. Call monolith_discover() with no args to list all available namespaces.",
                },
                "action": {
                    "type": "string",
                    "description": "Target action name within the namespace.",
                },
                "params": {
                    "type": "object",
                    "description": "Arguments for the action.",
                },
            },
            "required": ["namespace", "action"],
        },
    )


def _inject_monolith_query(response_str: str) -> str:
    try:
        payload = json.loads(response_str)
        tools = payload.get("result", {}).get("tools")
        if not isinstance(tools, list):
            return response_str

        for t in tools:
            if t.get("name") == "monolith_query":
                return response_str

        tools.append(_make_tool(
            "monolith_query",
            "Execute any Monolith action. Use monolith_find(query) to locate the right action, "
            "monolith_discover(namespace) to inspect its schema, then call monolith_query with "
            "the resolved namespace, action, and params.",
            {
                "type": "object",
                "properties": {
                    "namespace": {
                        "type": "string",
                        "description": "Target namespace, e.g. 'blueprint', 'source', 'gas'. Call monolith_discover() with no args to list all available namespaces.",
                    },
                    "action": {
                        "type": "string",
                        "description": "Action to execute. Call monolith_discover(namespace) for the "
                        "full action list and monolith_discover(namespace, action, mode='schema') "
                        "for exact parameter schemas.",
                    },
                    "params": {
                        "type": "object",
                        "description": "Arguments for the action.",
                    },
                },
                "required": ["namespace", "action"],
            }
        ))
        return json.dumps(payload)
    except Exception as e:
        _log(f"_inject_monolith_query parse error: {e}")
        return response_str


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
                "_fields": {
                    "type": "array",
                    "items": {"type": "string"},
                    "description": "Optional top-level whitelist — return only these top-level fields of the response. Mutually exclusive with _omit.",
                },
                "_omit": {
                    "type": "array",
                    "items": {"type": "string"},
                    "description": "Optional top-level blacklist — remove these top-level fields from the response. Mutually exclusive with _fields.",
                },
                "_compact_json": {
                    "type": "boolean",
                    "description": "Optional — when true, drop top-level fields whose value is null, empty string, empty array, or empty object.",
                },
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
                },
                "_fields": {
                    "type": "array",
                    "items": {"type": "string"},
                    "description": "Optional top-level whitelist — return only these top-level fields of the response. Mutually exclusive with _omit.",
                },
                "_omit": {
                    "type": "array",
                    "items": {"type": "string"},
                    "description": "Optional top-level blacklist — remove these top-level fields from the response. Mutually exclusive with _fields.",
                },
                "_compact_json": {
                    "type": "boolean",
                    "description": "Optional — when true, drop top-level fields whose value is null, empty string, empty array, or empty object.",
                },
            },
        },
    ))
    tools.append(_make_tool(
        "monolith_reindex",
        "Re-index the Monolith project database. Requires the editor-side Monolith server.",
        _empty_object_schema(),
    ))
    tools.append(_monolith_query_tool())
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
            "Monolith MCP proxy for Unreal Engine. Tools forward to the Unreal Editor.\n"
            "\n"
            "ROUTING:\n"
            "  monolith_find(query)                            — find the right action\n"
            "  monolith_discover()                             — list all namespaces\n"
            "  monolith_discover(namespace)                    — list actions in a namespace\n"
            "  monolith_discover(namespace, action, 'schema')  — fetch exact param schema\n"
            "  monolith_query({namespace, action, params})     — execute any action\n"
            "\n"
            "SKILL LOADING: domain skills live in Skills/<namespace>/SKILL.md and document\n"
            "available actions and params for that namespace.\n"
            "\n"
            "EDITOR OFFLINE: offline fallback is disabled for this proxy process, so editor transport failures remain unavailable errors.\n"
            "Before calling a domain action, check its schema instead of guessing. "
            "monolith_discover() lists namespaces and monolith_discover(namespace='<namespace>', "
            "mode='actions') lists actions; monolith_discover(namespace='<namespace>', action='<action>', mode='schema') fetches the exact live schema. "
            "Offline schema mode returns explicitly degraded catalog guidance rather than fabricating a live JSON schema. "
            "If an editor-only tool returns a transport-unavailable error, run Scripts/recover_mcp.ps1, wait for the configured endpoint, and retry."
        ),
    })


def handle_ping(msg: dict) -> str:
    return _result(msg.get("id"), {})


def handle_tools_list(msg: dict) -> str:
    """Forward tools/list to Monolith. Stable cached/seed list if down."""
    resp = _post_monolith(json.dumps(msg))

    if resp:
        _write_tools_cache(resp)
        return _inject_monolith_query(resp)
    return _fallback_tools_list(msg)


def handle_tools_call(msg: dict) -> str:
    """Forward tools/call to Monolith. Graceful error if down."""
    start_time = _now_iso()
    start_perf = time.perf_counter()
    parse_start = time.perf_counter()
    params = msg.get("params", {}) if isinstance(msg.get("params"), dict) else {}
    tool_name = params.get("name", "unknown")
    arguments = params.get("arguments", {})
    parse_ms = (time.perf_counter() - parse_start) * 1000.0
    dedup_start = time.perf_counter()
    retry_signature = _retry_signature(tool_name, arguments)
    now = time.monotonic()
    previous = _recent_tool_log_signatures.get(retry_signature)
    repeated = bool(previous and now - previous.get("at", 0.0) <= _REPEAT_LOG_WINDOW_SECONDS)
    dedup_ms = (time.perf_counter() - dedup_start) * 1000.0
    rewrite_start = time.perf_counter()
    trace_id = _make_log_id("trace", f"{start_time}:{os.getpid()}:{threading.get_ident()}:{retry_signature}")
    span_id = _make_log_id("span", f"{trace_id}:proxy:{msg.get('id')}:{start_time}")
    namespace, action = _tool_namespace_action(tool_name, arguments)
    intent, confidence = _infer_intent(namespace, action, "unknown")
    routing_context = _build_routing_context(tool_name, arguments, retry_signature, repeated, "unknown", namespace, action, intent, confidence)
    forwarded_msg = _with_trace(msg, trace_id, span_id, routing_context, "stateless")
    rewrite_ms = (time.perf_counter() - rewrite_start) * 1000.0

    http_start = time.perf_counter()
    resp = _post_monolith(json.dumps(forwarded_msg), retry=True)
    http_roundtrip_ms = (time.perf_counter() - http_start) * 1000.0
    log_msg_context = dict(msg)
    log_msg_context["_monolith_parse_ms"] = parse_ms
    log_msg_context["_monolith_rewrite_ms"] = rewrite_ms
    log_msg_context["_monolith_dedup_ms"] = dedup_ms
    log_msg_context["_monolith_http_roundtrip_ms"] = http_roundtrip_ms
    if resp:
        try:
            response_obj = _extract_response(resp)
            failed = isinstance(response_obj, dict) and (
                "error" in response_obj or response_obj.get("result", {}).get("isError"))
            record_id = _log_tools_call(log_msg_context, start_time, start_perf, resp, repeated, retry_signature, trace_id, span_id)
            _remember_tool_outcome(tool_name, arguments, trace_id, record_id, retry_signature, now, failed)
        except Exception as e:
            _log(f"Tool daily log wrapper failed: {e}")
        return resp

    fallback_start = time.perf_counter()
    response = _tool_error(
        msg.get("id"),
        f"Monolith MCP is not available (Unreal Editor not running). "
        f"Tool '{tool_name}' cannot execute. Start the editor and try again.",
    )
    log_msg_context["_monolith_fallback_ms"] = (time.perf_counter() - fallback_start) * 1000.0
    record_id = _log_tools_call(log_msg_context, start_time, start_perf, response, repeated, retry_signature, trace_id, span_id)
    _remember_tool_outcome(tool_name, arguments, trace_id, record_id, retry_signature, now, True)
    return response


def _print_help() -> None:
    print(
        """Usage:
  python monolith_proxy.py --help
  python monolith_proxy.py --version
  python monolith_proxy.py

Role:
  Stdio-to-HTTP MCP bridge for the editor-hosted Monolith server.
  Default target: MONOLITH_URL=http://localhost:9316/mcp

Environment:
  MONOLITH_URL                         Editor MCP endpoint.
  MONOLITH_TOOL_LOG_ENABLED            Set 0 to disable daily proxy logs.
  MONOLITH_TOOL_LOG_DIR                Redirect Logs/yyyyMMdd/proxy.jsonl.
  MONOLITH_TOOL_LOG_MAX_FIELD_BYTES    Bound captured log fields.
  LOCALAPPDATA / XDG_CACHE_HOME        Used for script-proxy tool cache fallback paths.

Runtime support notes:
  MONOLITH_SPLIT_EDITOR_QUERY, MONOLITH_EDITOR_ACTION_ALLOWLIST, and
  MONOLITH_EDITOR_ACTION_DENYLIST are native C++ proxy controls.
  MONOLITH_CALL_LOG and MONOLITH_PROJECT_ROOT control the native C++ proxy call log.

MCP config example:
  {"mcpServers":{"monolith":{"command":"<project-root>/Plugins/Monolith/Scripts/monolith_proxy.sh"}}} (or .bat on Windows)

Offline fallback:
  Use Binaries/monolith_query (or .exe on Windows) for read-only source/project/bridge/console queries when the editor or MCP server is unavailable.
"""
    )


def _print_version() -> None:
    print(json.dumps({"tool": PROXY_NAME, "version": PROXY_VERSION, "runtime": "python"}, indent=2))


def _handle_help_or_version() -> bool:
    for arg in sys.argv[1:]:
        if arg in ("--help", "-h", "help"):
            _print_help()
            return True
        if arg in ("--version", "-v", "version"):
            _print_version()
            return True
    return False


def main() -> None:
    if _handle_help_or_version():
        return

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
