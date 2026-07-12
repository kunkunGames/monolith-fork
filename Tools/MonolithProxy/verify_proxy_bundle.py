#!/usr/bin/env python3
"""Verify a staged native Proxy + Query/catalog bundle through real stdio MCP."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import queue
import re
import subprocess
import sys
import tempfile
import threading
import time


PROXY_FIELDS = {
    "schema_version",
    "tool",
    "runtime",
    "file",
    "version",
    "source_hash",
    "sha256",
}
EXPECTED_TOOLS = {
    "monolith_query",
    "monolith_status",
    "monolith_discover",
    "monolith_find",
}


class VerificationError(RuntimeError):
    pass


def _reject_constant(value: str) -> None:
    raise VerificationError(f"non-finite JSON constant is forbidden: {value}")


def _reject_duplicate_keys(pairs: list[tuple[str, object]]) -> dict[str, object]:
    result: dict[str, object] = {}
    for key, value in pairs:
        if key in result:
            raise VerificationError(f"duplicate JSON key: {key}")
        result[key] = value
    return result


def _strict_json(text: str, label: str) -> object:
    try:
        return json.loads(
            text,
            parse_constant=_reject_constant,
            object_pairs_hook=_reject_duplicate_keys,
        )
    except (json.JSONDecodeError, VerificationError) as error:
        raise VerificationError(f"{label} is not strict JSON: {error}") from error


def _resolve_proxy(binaries_root: Path) -> tuple[Path, dict[str, object]]:
    root = binaries_root.resolve(strict=True)
    manifest_path = root / "monolith_proxy.current.json"
    try:
        manifest = _strict_json(manifest_path.read_text(encoding="utf-8"), "proxy manifest")
    except (OSError, UnicodeError) as error:
        raise VerificationError(f"proxy manifest could not be read: {error}") from error
    if not isinstance(manifest, dict) or set(manifest) != PROXY_FIELDS:
        raise VerificationError("proxy manifest has an unexpected field set")
    if type(manifest["schema_version"]) is not int or manifest["schema_version"] != 1:
        raise VerificationError("proxy manifest schema_version must be integer 1")
    if manifest["tool"] != "monolith-proxy" or manifest["runtime"] != "native-cpp":
        raise VerificationError("proxy manifest tool/runtime identity is invalid")
    file_name = manifest["file"]
    source_hash = manifest["source_hash"]
    expected_sha = manifest["sha256"]
    if (
        not isinstance(file_name, str)
        or not isinstance(source_hash, str)
        or re.fullmatch(r"monolith_proxy-([0-9a-f]{16})\.exe", file_name) is None
        or file_name != f"monolith_proxy-{source_hash}.exe"
        or Path(file_name).name != file_name
        or not isinstance(expected_sha, str)
        or re.fullmatch(r"[0-9a-f]{64}", expected_sha) is None
    ):
        raise VerificationError("proxy manifest leaf/source/SHA binding is invalid")
    proxy = (root / file_name).resolve(strict=True)
    if proxy.parent != root or proxy.is_symlink() or not proxy.is_file():
        raise VerificationError("proxy manifest target is escaped, linked, or not regular")
    if hashlib.sha256(proxy.read_bytes()).hexdigest() != expected_sha:
        raise VerificationError("proxy manifest target SHA-256 mismatch")
    return proxy, manifest


class Session:
    def __init__(self, proxy: Path, env: dict[str, str]):
        creationflags = getattr(subprocess, "CREATE_NO_WINDOW", 0)
        self.process = subprocess.Popen(
            [str(proxy)],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            encoding="utf-8",
            errors="strict",
            env=env,
            creationflags=creationflags,
        )
        self.lines: queue.Queue[str | None] = queue.Queue()
        self.reader = threading.Thread(target=self._read_stdout, daemon=True)
        self.reader.start()

    def _read_stdout(self) -> None:
        assert self.process.stdout is not None
        for line in self.process.stdout:
            self.lines.put(line)
        self.lines.put(None)

    def request(self, request_id: int, method: str, params: dict[str, object]) -> dict:
        assert self.process.stdin is not None
        payload = {
            "jsonrpc": "2.0",
            "id": request_id,
            "method": method,
            "params": params,
        }
        self.process.stdin.write(json.dumps(payload, separators=(",", ":")) + "\n")
        self.process.stdin.flush()
        deadline = time.monotonic() + 30
        while time.monotonic() < deadline:
            try:
                line = self.lines.get(timeout=max(0.01, deadline - time.monotonic()))
            except queue.Empty as error:
                raise VerificationError(f"proxy response {request_id} timed out") from error
            if line is None:
                raise VerificationError(f"proxy exited before response {request_id}")
            response = _strict_json(line, "proxy response")
            if isinstance(response, dict) and response.get("id") == request_id:
                return response
        raise VerificationError(f"proxy response {request_id} timed out")

    def close(self) -> str:
        if self.process.stdin is not None:
            self.process.stdin.close()
        try:
            self.process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            self.process.kill()
            self.process.wait(timeout=5)
        stderr = self.process.stderr.read() if self.process.stderr is not None else ""
        if self.process.returncode != 0:
            raise VerificationError(
                f"proxy exited with {self.process.returncode}: {stderr[-2000:]}"
            )
        return stderr


def _result_text(response: dict) -> str:
    result = response.get("result", {})
    content = result.get("content", []) if isinstance(result, dict) else []
    if content and isinstance(content[0], dict):
        return str(content[0].get("text", ""))
    return ""


def verify(binaries_root: Path) -> dict[str, object]:
    proxy, manifest = _resolve_proxy(binaries_root)
    with tempfile.TemporaryDirectory(prefix="monolith-release-bundle-smoke-") as temp:
        env = os.environ.copy()
        env.update(
            {
                "MONOLITH_URL": "http://127.0.0.1:9/mcp",
                "MONOLITH_EXPECTED_PROJECT_ROOT": str(binaries_root.resolve().parent),
                "MONOLITH_OFFLINE_FALLBACK": "1",
                "MONOLITH_CALL_LOG": "0",
                "MONOLITH_TOOL_LOG_ENABLED": "0",
                "LOCALAPPDATA": temp,
            }
        )
        env.pop("MONOLITH_QUERY_EXE", None)
        env.pop("MONOLITH_CATALOG_SNAPSHOT", None)
        session = Session(proxy, env)
        try:
            initialized = session.request(
                1,
                "initialize",
                {
                    "protocolVersion": "2025-11-25",
                    "capabilities": {},
                    "clientInfo": {"name": "release-bundle-smoke", "version": "1"},
                },
            )
            if initialized.get("result", {}).get("serverInfo", {}).get("name") != "monolith-proxy":
                raise VerificationError("proxy initialize returned the wrong server identity")
            tools = session.request(2, "tools/list", {})
            tool_names = {
                item.get("name")
                for item in tools.get("result", {}).get("tools", [])
                if isinstance(item, dict)
            }
            if tool_names != EXPECTED_TOOLS:
                raise VerificationError(f"cold tools differ from four-tool contract: {tool_names}")
            discovered = session.request(
                3,
                "tools/call",
                {
                    "name": "monolith_discover",
                    "arguments": {
                        "namespace": "project",
                        "mode": "actions",
                        "limit": 1,
                    },
                },
            )
            result = discovered.get("result", {})
            structured = result.get("structuredContent", {}) if isinstance(result, dict) else {}
            if (
                result.get("isError") is not False
                or structured.get("status") != "degraded"
                or structured.get("completion_class") != "degraded_guidance"
            ):
                raise VerificationError(
                    f"staged offline discover failed: {_result_text(discovered)[:1000]}"
                )
            if structured.get("_monolith", {}).get("offline_fallback") is not True:
                raise VerificationError("staged offline discover did not use the bundled Query")
        finally:
            session.close()
    return {
        "status": "passed",
        "proxy_file": manifest["file"],
        "proxy_source_hash": manifest["source_hash"],
        "offline_tools": len(EXPECTED_TOOLS),
        "offline_discover": "degraded_guidance",
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--binaries-root", type=Path, required=True)
    args = parser.parse_args()
    try:
        print(json.dumps(verify(args.binaries_root), indent=2))
        return 0
    except (OSError, VerificationError, subprocess.SubprocessError) as error:
        print(f"FAILED: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
