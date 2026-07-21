#!/usr/bin/env python3
"""Cold-start smoke test for the native proxy's offline query fallback."""

from __future__ import annotations

import argparse
import hashlib
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import os
from pathlib import Path
import queue
import re
import shutil
import socket
import sqlite3
import subprocess
import sys
import tempfile
import threading
import time


PLUGIN_ROOT = Path(__file__).resolve().parents[2]
PROJECT_ROOT = Path(__file__).resolve().parents[4]
SCRIPTS_ROOT = PLUGIN_ROOT / "Scripts"
sys.path.insert(0, str(SCRIPTS_ROOT))
try:
    from source_generation_hash import (
        SOURCE_GENERATION_SPECS,
        compute_tool_source_generation_hash,
    )
finally:
    sys.path.remove(str(SCRIPTS_ROOT))


EXPECTED_PROXY_BUILD_CONTRACT = (
    b"monolith-proxy-build-v3-mt-o2-brepro-msvc-deterministic-pathmap"
)


def assert_reproducible_build_contract() -> None:
    """Keep checkout-independent MSVC codegen and link flags fail-closed."""
    proxy_spec = SOURCE_GENERATION_SPECS["proxy"]
    assert proxy_spec.build_contract == EXPECTED_PROXY_BUILD_CONTRACT

    build_text = (PLUGIN_ROOT / "Tools" / "MonolithProxy" / "build.bat").read_text(
        encoding="utf-8"
    )
    compile_lines = [
        line.strip()
        for line in build_text.splitlines()
        if line.lstrip().lower().startswith("cl ") and " /c " in line.lower()
    ]
    assert len(compile_lines) == 2, compile_lines
    for compile_line in compile_lines:
        normalized = compile_line.lower()
        assert " /brepro " in normalized, compile_line
        assert " /experimental:deterministic " in normalized, compile_line
        assert '"/pathmap:%monolith_plugin_root%=."' in normalized, compile_line

    link_lines = [
        line.strip()
        for line in build_text.splitlines()
        if line.lstrip().lower().startswith("cl ") and " /c " not in line.lower()
    ]
    assert len(link_lines) == 1, link_lines
    normalized_link = link_lines[0].lower()
    assert " /link /brepro" in normalized_link, link_lines[0]

    cmake_text = (
        PLUGIN_ROOT / "Tools" / "MonolithProxy" / "CMakeLists.txt"
    ).read_text(encoding="utf-8")
    assert "file(TO_NATIVE_PATH" in cmake_text
    assert "/experimental:deterministic" in cmake_text
    assert '"/pathmap:${MONOLITH_PLUGIN_ROOT_NATIVE}=."' in cmake_text
    assert "target_link_options(monolith_proxy PRIVATE /Brepro)" in cmake_text


def current_proxy_from_manifest() -> Path:
    manifest_path = PLUGIN_ROOT / "Binaries" / "monolith_proxy.current.json"
    try:
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise SystemExit(
            f"cannot read authoritative proxy manifest {manifest_path}: {error}"
        ) from error

    if type(manifest.get("schema_version")) is not int or manifest[
        "schema_version"
    ] != 1:
        raise SystemExit("proxy manifest schema_version must be integer 1")
    file_name = manifest.get("file")
    source_hash_value = manifest.get("source_hash")
    expected_sha256 = manifest.get("sha256")
    if manifest.get("tool") != "monolith-proxy":
        raise SystemExit("proxy manifest tool must be monolith-proxy")
    if manifest.get("runtime") != "native-cpp":
        raise SystemExit("proxy manifest runtime must be native-cpp")
    if not isinstance(manifest.get("version"), str) or not manifest["version"]:
        raise SystemExit("proxy manifest version must be a non-empty string")
    if not isinstance(file_name, str):
        raise SystemExit("proxy manifest file must be a string")
    file_match = re.fullmatch(r"monolith_proxy-([0-9a-f]{16})\.exe", file_name)
    if file_match is None or Path(file_name).name != file_name:
        raise SystemExit("proxy manifest file is not a source-addressed leaf name")
    if source_hash_value != file_match.group(1):
        raise SystemExit("proxy manifest source_hash does not match its filename")
    if not isinstance(expected_sha256, str) or re.fullmatch(
        r"[0-9a-f]{64}", expected_sha256
    ) is None:
        raise SystemExit("proxy manifest sha256 is invalid")

    binaries_root = (PLUGIN_ROOT / "Binaries").resolve()
    proxy = (binaries_root / file_name).resolve()
    if proxy.parent != binaries_root or not proxy.is_file() or proxy.is_symlink():
        raise SystemExit("proxy manifest target is missing, escaped, or a link")
    actual_sha256 = hashlib.sha256(proxy.read_bytes()).hexdigest()
    if actual_sha256 != expected_sha256:
        raise SystemExit("proxy manifest target SHA-256 mismatch")
    version_result = subprocess.run(
        [str(proxy), "--version"],
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="strict",
        timeout=10,
        check=True,
    )
    executable_identity = json.loads(version_result.stdout)
    for field in ("tool", "runtime", "version", "source_hash"):
        if executable_identity.get(field) != manifest.get(field):
            raise SystemExit(
                f"proxy manifest {field} does not match executable identity"
            )
    return proxy


def current_query_bundle_from_manifest() -> tuple[Path, Path, dict]:
    manifest_path = PLUGIN_ROOT / "Binaries" / "monolith_query.current.json"

    def reject_constant(value: str) -> None:
        raise ValueError(f"non-finite JSON constant is forbidden: {value}")

    try:
        manifest = json.loads(
            manifest_path.read_text(encoding="utf-8"),
            parse_constant=reject_constant,
        )
    except (OSError, ValueError, json.JSONDecodeError) as error:
        raise SystemExit(
            f"cannot read authoritative Query bundle manifest {manifest_path}: {error}"
        ) from error

    expected_fields = {
        "schema_version",
        "tool",
        "runtime",
        "file",
        "plugin_version",
        "parity_spec_rev",
        "source_hash",
        "sha256",
        "catalog_file",
        "catalog_source_hash",
        "catalog_sha256",
    }
    if not isinstance(manifest, dict) or set(manifest) != expected_fields:
        raise SystemExit("Query bundle manifest has an unexpected field set")
    if type(manifest["schema_version"]) is not int or manifest["schema_version"] != 1:
        raise SystemExit("Query bundle manifest schema_version must be integer 1")
    if manifest["tool"] != "monolith_query" or manifest["runtime"] != "native-cpp":
        raise SystemExit("Query bundle manifest tool/runtime identity is invalid")
    for field in expected_fields - {"schema_version"}:
        if not isinstance(manifest[field], str) or not manifest[field]:
            raise SystemExit(f"Query bundle manifest {field} must be a non-empty string")

    query_match = re.fullmatch(
        r"monolith_query-([0-9a-f]{16})\.exe", manifest["file"]
    )
    catalog_match = re.fullmatch(
        r"monolith_catalog-([0-9a-f]{64})\.json", manifest["catalog_file"]
    )
    if query_match is None or Path(manifest["file"]).name != manifest["file"]:
        raise SystemExit("Query bundle executable is not a source-addressed leaf name")
    if (
        catalog_match is None
        or Path(manifest["catalog_file"]).name != manifest["catalog_file"]
    ):
        raise SystemExit("Query bundle catalog is not a semantic-addressed leaf name")
    if query_match.group(1) != manifest["source_hash"]:
        raise SystemExit("Query bundle executable filename/source_hash mismatch")
    if catalog_match.group(1) != manifest["catalog_source_hash"]:
        raise SystemExit("Query bundle catalog filename/source_hash mismatch")
    for field in ("sha256", "catalog_sha256"):
        if re.fullmatch(r"[0-9a-f]{64}", manifest[field]) is None:
            raise SystemExit(f"Query bundle manifest {field} is invalid")

    binaries_root = (PLUGIN_ROOT / "Binaries").resolve()
    query = (binaries_root / manifest["file"]).resolve()
    catalog = (binaries_root / manifest["catalog_file"]).resolve()
    for label, path in (("Query executable", query), ("catalog", catalog)):
        if path.parent != binaries_root or not path.is_file() or path.is_symlink():
            raise SystemExit(f"Query bundle {label} is missing, escaped, or a link")
    if hashlib.sha256(query.read_bytes()).hexdigest() != manifest["sha256"]:
        raise SystemExit("Query bundle executable SHA-256 mismatch")
    catalog_bytes = catalog.read_bytes()
    if hashlib.sha256(catalog_bytes).hexdigest() != manifest["catalog_sha256"]:
        raise SystemExit("Query bundle catalog SHA-256 mismatch")
    try:
        catalog_payload = json.loads(
            catalog_bytes.decode("utf-8"), parse_constant=reject_constant
        )
    except (UnicodeDecodeError, ValueError, json.JSONDecodeError) as error:
        raise SystemExit(f"Query bundle catalog is not strict JSON: {error}") from error
    if (
        not isinstance(catalog_payload, dict)
        or catalog_payload.get("source_hash") != manifest["catalog_source_hash"]
        or not isinstance(catalog_payload.get("actions"), list)
    ):
        raise SystemExit("Query bundle catalog semantic identity is invalid")
    executable_identity = executable_version(query)
    for field in (
        "tool",
        "runtime",
        "plugin_version",
        "parity_spec_rev",
        "source_hash",
    ):
        if executable_identity.get(field) != manifest[field]:
            raise SystemExit(
                f"Query bundle manifest {field} does not match executable identity"
            )
    return query, catalog, manifest


def health_payload(port: int) -> dict:
    return {
        "status": "ok",
        "pid": os.getpid(),
        "port": port,
        "version": "test",
        "uptime_seconds": 1,
        "tools_registered": 4,
        "mcp_transport": {"primary_route": "/mcp"},
    }


def status_response(
    request_id: object,
    project_root: Path = PROJECT_ROOT,
    *,
    editor_pid: int | None = None,
    commandlet: bool = False,
) -> dict:
    if editor_pid is None:
        editor_pid = os.getpid()
    return {
        "jsonrpc": "2.0",
        "id": request_id,
        "result": {
            "content": [{"type": "text", "text": "status"}],
            "structuredContent": {
                "recovery_plan": {
                    "editor_candidate_status": {
                        "status": "current_editor_process",
                        "pid": editor_pid,
                        "commandlet": commandlet,
                        "unattended": True,
                        "host_project_root": str(project_root),
                    }
                }
            },
            "isError": False,
        },
    }


class LiveErrorHandler(BaseHTTPRequestHandler):
    """Minimal live backend that returns a valid tool-level error."""

    health_delay_seconds = 0.0
    health_calls = 0
    status_delay_seconds = 0.0
    status_calls = 0

    def log_message(self, _format: str, *args: object) -> None:
        del args

    def _write_json(self, payload: dict) -> None:
        body = json.dumps(payload, separators=(",", ":")).encode("utf-8")
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self) -> None:
        type(self).health_calls += 1
        if self.health_delay_seconds:
            time.sleep(self.health_delay_seconds)
        self._write_json(health_payload(self.server.server_port))

    def do_POST(self) -> None:
        length = int(self.headers.get("Content-Length", "0"))
        request = json.loads(self.rfile.read(length).decode("utf-8"))
        if request.get("params", {}).get("name") == "monolith_status":
            type(self).status_calls += 1
            if self.status_delay_seconds:
                time.sleep(self.status_delay_seconds)
            self._write_json(status_response(request.get("id")))
            return
        self._write_json(
            {
                "jsonrpc": "2.0",
                "id": request.get("id"),
                "result": {
                    "content": [{"type": "text", "text": "LIVE_SENTINEL"}],
                    "isError": True,
                },
            }
        )


class DelayedIdentityHandler(LiveErrorHandler):
    """Valid endpoint whose identity exceeds the old 250 ms request budget."""

    status_delay_seconds = 0.6
    status_calls = 0


class DelayedHealthHandler(LiveErrorHandler):
    """Valid endpoint whose health response exceeds the old 250 ms budget."""

    health_delay_seconds = 0.6
    health_calls = 0


class InvalidLiveHandler(BaseHTTPRequestHandler):
    """Backend that is reachable but returns a non-JSON MCP response."""

    def log_message(self, _format: str, *args: object) -> None:
        del args

    def do_GET(self) -> None:
        body = json.dumps(
            health_payload(self.server.server_port), separators=(",", ":")
        ).encode(
            "utf-8"
        )
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_POST(self) -> None:
        length = int(self.headers.get("Content-Length", "0"))
        request = json.loads(self.rfile.read(length).decode("utf-8"))
        if request.get("params", {}).get("name") == "monolith_status":
            body = json.dumps(
                status_response(request.get("id")), separators=(",", ":")
            ).encode("utf-8")
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return
        body = b"<html>not-json</html>"
        self.send_response(200)
        self.send_header("Content-Type", "text/html")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)


class HangingLiveHandler(BaseHTTPRequestHandler):
    """Endpoint that accepts connections but cannot complete startup requests."""

    def log_message(self, _format: str, *args: object) -> None:
        del args

    def do_GET(self) -> None:
        time.sleep(5)

    def do_POST(self) -> None:
        time.sleep(5)


class TrickleToolsListHandler(LiveErrorHandler):
    """Keeps a valid tools/list body moving past the total metadata deadline."""

    def do_POST(self) -> None:
        length = int(self.headers.get("Content-Length", "0"))
        request = json.loads(self.rfile.read(length).decode("utf-8"))
        if request.get("params", {}).get("name") == "monolith_status":
            self._write_json(status_response(request.get("id")))
            return
        if request.get("method") == "tools/list":
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", "100")
            self.end_headers()
            try:
                for _ in range(20):
                    self.wfile.write(b" ")
                    self.wfile.flush()
                    time.sleep(0.2)
            except OSError:
                pass
            return
        self._write_json(
            {
                "jsonrpc": "2.0",
                "id": request.get("id"),
                "result": {
                    "content": [{"type": "text", "text": "LIVE_SENTINEL"}],
                    "isError": True,
                },
            }
        )


class TrickleHealthHandler(LiveErrorHandler):
    """Sends health chunks inside each phase timeout but past the total budget."""

    def do_GET(self) -> None:
        body = json.dumps(
            health_payload(self.server.server_port), separators=(",", ":")
        ).encode("utf-8")
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        try:
            for offset in range(0, len(body), 20):
                self.wfile.write(body[offset : offset + 20])
                self.wfile.flush()
                time.sleep(1.0)
        except OSError:
            pass


class OversizedLiveHandler(LiveErrorHandler):
    """Advertises a response beyond the proxy's bounded live body limit."""

    def do_POST(self) -> None:
        length = int(self.headers.get("Content-Length", "0"))
        request = json.loads(self.rfile.read(length).decode("utf-8"))
        if request.get("params", {}).get("name") == "monolith_status":
            self._write_json(status_response(request.get("id")))
            return
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(16 * 1024 * 1024 + 1))
        self.end_headers()


class SlowMutationHandler(LiveErrorHandler):
    """Completes one mutation after the historical three-second dedup window."""

    mutation_calls = 0

    def do_POST(self) -> None:
        length = int(self.headers.get("Content-Length", "0"))
        request = json.loads(self.rfile.read(length).decode("utf-8"))
        if request.get("params", {}).get("name") == "monolith_status":
            self._write_json(status_response(request.get("id")))
            return
        params = request.get("params", {})
        arguments = params.get("arguments", {})
        if (
            params.get("name") == "monolith_query"
            and arguments.get("namespace") == "asset"
            and arguments.get("action") == "save_asset"
        ):
            type(self).mutation_calls += 1
            time.sleep(4.0)
            self._write_json(
                {
                    "jsonrpc": "2.0",
                    "id": request.get("id"),
                    "result": {
                        "content": [{"type": "text", "text": "MUTATION_DONE"}],
                        "isError": False,
                    },
                }
            )
            return
        self._write_json(
            {
                "jsonrpc": "2.0",
                "id": request.get("id"),
                "result": {
                    "content": [{"type": "text", "text": "LIVE_SENTINEL"}],
                    "isError": True,
                },
            }
        )


class SlowSchemaDiscoveryHandler(LiveErrorHandler):
    """Completes authoritative schema discovery after the fast-fallback budget."""

    schema_calls = 0

    def do_POST(self) -> None:
        length = int(self.headers.get("Content-Length", "0"))
        request = json.loads(self.rfile.read(length).decode("utf-8"))
        params = request.get("params", {})
        if params.get("name") == "monolith_status":
            self._write_json(status_response(request.get("id")))
            return
        arguments = params.get("arguments", {})
        if (
            params.get("name") == "monolith_discover"
            and arguments.get("mode") == "schema"
        ):
            type(self).schema_calls += 1
            time.sleep(4.0)
            self._write_json(
                {
                    "jsonrpc": "2.0",
                    "id": request.get("id"),
                    "result": {
                        "content": [
                            {"type": "text", "text": "LIVE_SCHEMA_SENTINEL"}
                        ],
                        "isError": False,
                    },
                }
            )
            return
        self._write_json(
            {
                "jsonrpc": "2.0",
                "id": request.get("id"),
                "result": {
                    "content": [{"type": "text", "text": "LIVE_SENTINEL"}],
                    "isError": True,
                },
            }
        )


class IncompleteHealthHandler(LiveErrorHandler):
    """Looks healthy at a glance but omits required health contract fields."""

    def do_GET(self) -> None:
        self._write_json(
            {
                "status": "ok",
                "pid": os.getpid(),
                "tools_registered": 4,
                "mcp_transport": {"primary_route": "/mcp"},
            }
        )


class SpoofedHealthPidHandler(LiveErrorHandler):
    """Reports a PID that does not own this process's listening socket."""

    def do_GET(self) -> None:
        payload = health_payload(self.server.server_port)
        payload["pid"] = os.getpid() + 1
        self._write_json(payload)


class InvalidEditorIdentityHandler(LiveErrorHandler):
    """Base for status identities that disagree with the validated health PID."""

    editor_pid_delta = 0
    commandlet = False

    def do_POST(self) -> None:
        length = int(self.headers.get("Content-Length", "0"))
        request = json.loads(self.rfile.read(length).decode("utf-8"))
        if request.get("params", {}).get("name") == "monolith_status":
            self._write_json(
                status_response(
                    request.get("id"),
                    editor_pid=os.getpid() + self.editor_pid_delta,
                    commandlet=self.commandlet,
                )
            )
            return
        self._write_json(
            {
                "jsonrpc": "2.0",
                "id": request.get("id"),
                "result": {
                    "content": [{"type": "text", "text": "LIVE_SENTINEL"}],
                    "isError": True,
                },
            }
        )


class MismatchedStatusPidHandler(InvalidEditorIdentityHandler):
    """Status PID differs while commandlet mode remains otherwise acceptable."""

    editor_pid_delta = 1


class CommandletIdentityHandler(InvalidEditorIdentityHandler):
    """Status PID matches, but the endpoint reports commandlet mode."""

    commandlet = True


class IPv6ThreadingHTTPServer(ThreadingHTTPServer):
    address_family = socket.AF_INET6


class ForeignProjectHandler(LiveErrorHandler):
    """Healthy Monolith-shaped endpoint for a different project checkout."""

    def do_POST(self) -> None:
        length = int(self.headers.get("Content-Length", "0"))
        request = json.loads(self.rfile.read(length).decode("utf-8"))
        if request.get("params", {}).get("name") == "monolith_status":
            self._write_json(
                status_response(request.get("id"), Path("D:/ForeignProject"))
            )
            return
        self._write_json(
            {
                "jsonrpc": "2.0",
                "id": request.get("id"),
                "result": {
                    "content": [{"type": "text", "text": "FOREIGN_SENTINEL"}],
                    "isError": True,
                },
            }
        )


class SwitchingProjectHandler(LiveErrorHandler):
    """Starts as this project, then reuses the same PID for a foreign root."""

    foreign = False

    def do_POST(self) -> None:
        length = int(self.headers.get("Content-Length", "0"))
        request = json.loads(self.rfile.read(length).decode("utf-8"))
        if request.get("params", {}).get("name") == "monolith_status":
            root = Path("D:/ForeignAfterAccept") if self.foreign else PROJECT_ROOT
            self._write_json(status_response(request.get("id"), root))
            return
        self._write_json(
            {
                "jsonrpc": "2.0",
                "id": request.get("id"),
                "result": {
                    "content": [
                        {"type": "text", "text": "FOREIGN_AFTER_SWAP"}
                    ],
                    "isError": True,
                },
            }
        )


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--proxy",
        type=Path,
        default=None,
        help=(
            "Proxy executable override; default validates and selects "
            "Binaries/monolith_proxy.current.json."
        ),
    )
    parser.add_argument(
        "--query",
        type=Path,
        default=None,
        help="Optional MONOLITH_QUERY_EXE override; default tests co-location.",
    )
    return parser.parse_args()


class ProxySession:
    def __init__(self, process: subprocess.Popen[str]) -> None:
        self.process = process
        self.responses: queue.Queue[dict] = queue.Queue()
        self.stderr_lines: list[str] = []
        self.stdout_thread = threading.Thread(
            target=self._read_stdout, daemon=True
        )
        self.stderr_thread = threading.Thread(
            target=self._read_stderr, daemon=True
        )
        self.stdout_thread.start()
        self.stderr_thread.start()

    def _read_stdout(self) -> None:
        assert self.process.stdout is not None
        for line in self.process.stdout:
            line = line.strip()
            if not line:
                continue
            try:
                self.responses.put(json.loads(line))
            except json.JSONDecodeError as error:
                self.responses.put(
                    {"_decode_error": str(error), "_raw_line": line}
                )

    def _read_stderr(self) -> None:
        assert self.process.stderr is not None
        for line in self.process.stderr:
            self.stderr_lines.append(line.rstrip())

    def request(self, request_id: int, method: str, params: dict) -> dict:
        assert self.process.stdin is not None
        payload = {
            "jsonrpc": "2.0",
            "id": request_id,
            "method": method,
            "params": params,
        }
        self.process.stdin.write(
            json.dumps(payload, ensure_ascii=False, separators=(",", ":"))
            + "\n"
        )
        self.process.stdin.flush()

        deadline = time.monotonic() + 20.0
        while time.monotonic() < deadline:
            try:
                response = self.responses.get(
                    timeout=max(0.01, deadline - time.monotonic())
                )
            except queue.Empty as error:
                raise AssertionError(
                    f"timed out waiting for response id={request_id}; "
                    f"stderr={self.stderr_lines}"
                ) from error
            if response.get("_decode_error"):
                raise AssertionError(f"invalid proxy stdout: {response}")
            if response.get("id") == request_id:
                return response
            # Ignore asynchronous tools/list_changed notifications.
        raise AssertionError(f"no response for id={request_id}")

    def close(self) -> None:
        if self.process.stdin is not None and not self.process.stdin.closed:
            self.process.stdin.close()
        forced = False
        try:
            self.process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            forced = True
            self.process.terminate()
            self.process.wait(timeout=5)
        self.stdout_thread.join(timeout=1)
        self.stderr_thread.join(timeout=1)
        if forced:
            raise AssertionError(
                "proxy did not exit cleanly after stdin EOF; "
                f"stderr={self.stderr_lines}"
            )


def call_tool(
    session: ProxySession, request_id: int, name: str, arguments: dict
) -> dict:
    return session.request(
        request_id,
        "tools/call",
        {"name": name, "arguments": arguments},
    )


def wait_for_list_changed(session: ProxySession, timeout: float = 8.0) -> None:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        try:
            response = session.responses.get(
                timeout=max(0.01, deadline - time.monotonic())
            )
        except queue.Empty as error:
            raise AssertionError(
                "timed out waiting for tools/list_changed; "
                f"stderr={session.stderr_lines}"
            ) from error
        if response.get("method") == "notifications/tools/list_changed":
            return
    raise AssertionError("no tools/list_changed notification received")


def launch_proxy(proxy: Path, env: dict[str, str]) -> ProxySession:
    process = subprocess.Popen(
        [str(proxy)],
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        encoding="utf-8",
        errors="replace",
        bufsize=1,
        env=env,
    )
    return ProxySession(process)


def create_hot_rollback_journal(database: Path) -> Path:
    helper = """
import os
import sqlite3
import sys

connection = sqlite3.connect(sys.argv[1])
connection.execute("PRAGMA journal_mode=DELETE")
connection.execute("PRAGMA synchronous=FULL")
connection.execute("PRAGMA cache_size=10")
connection.execute("CREATE TABLE probe (id INTEGER PRIMARY KEY, value TEXT)")
connection.executemany(
    "INSERT INTO probe(value) VALUES (?)",
    [("before-" + ("x" * 1024),) for _ in range(1024)],
)
connection.commit()
connection.execute("BEGIN EXCLUSIVE")
connection.execute("UPDATE probe SET value='after-' || substr(value, 8)")
os._exit(0)
"""
    subprocess.run(
        [sys.executable, "-c", helper, str(database)],
        check=True,
        capture_output=True,
        text=True,
    )
    journal = Path(f"{database}-journal")
    assert journal.is_file(), journal
    return journal


def create_source_fixture(database: Path, indexed_source: Path) -> None:
    """Create a deterministic, tiny EngineSource-compatible read fixture."""
    connection = sqlite3.connect(database)
    try:
        connection.executescript(
            """
CREATE TABLE meta (key TEXT PRIMARY KEY, value TEXT);
CREATE TABLE modules (
  id INTEGER PRIMARY KEY, name TEXT NOT NULL, path TEXT NOT NULL,
  module_type TEXT NOT NULL, build_cs_path TEXT, UNIQUE(name, path));
CREATE TABLE files (
  id INTEGER PRIMARY KEY, path TEXT NOT NULL UNIQUE,
  module_id INTEGER REFERENCES modules(id), file_type TEXT NOT NULL,
  line_count INTEGER NOT NULL DEFAULT 0,
  last_modified REAL NOT NULL DEFAULT 0.0);
CREATE TABLE symbols (
  id INTEGER PRIMARY KEY, name TEXT NOT NULL, qualified_name TEXT NOT NULL,
  kind TEXT NOT NULL, file_id INTEGER REFERENCES files(id),
  line_start INTEGER, line_end INTEGER,
  parent_symbol_id INTEGER REFERENCES symbols(id), access TEXT,
  signature TEXT, docstring TEXT,
  is_ue_macro INTEGER NOT NULL DEFAULT 0);
CREATE TABLE inheritance (
  id INTEGER PRIMARY KEY, child_id INTEGER NOT NULL,
  parent_id INTEGER NOT NULL, UNIQUE(child_id, parent_id));
CREATE TABLE "references" (
  id INTEGER PRIMARY KEY, from_symbol_id INTEGER NOT NULL,
  to_symbol_id INTEGER NOT NULL, ref_kind TEXT NOT NULL,
  file_id INTEGER, line INTEGER);
CREATE TABLE includes (
  id INTEGER PRIMARY KEY, file_id INTEGER NOT NULL,
  included_path TEXT NOT NULL, line INTEGER);
CREATE TABLE symbol_deprecations (
  symbol_name TEXT PRIMARY KEY, version TEXT,
  message TEXT, kind TEXT);
CREATE TABLE reflect_uproperties (
  owning_class TEXT, property_name TEXT, property_type TEXT,
  cpp_module TEXT, blueprint_visibility TEXT, specifiers TEXT);
CREATE TABLE decision_records (
  decision_id TEXT PRIMARY KEY, source_path TEXT, source_mtime INTEGER);
CREATE TABLE risk_hotspot_scores (
  file_path TEXT PRIMARY KEY, score REAL, churn INTEGER,
  complexity_proxy INTEGER);
CREATE VIRTUAL TABLE symbols_fts USING fts5(
  name, qualified_name, docstring, content=symbols, content_rowid=id);
CREATE VIRTUAL TABLE source_fts USING fts5(
  file_id UNINDEXED, line_number UNINDEXED, text);
CREATE TRIGGER symbols_ai AFTER INSERT ON symbols BEGIN SELECT 1; END;
CREATE TRIGGER symbols_ad AFTER DELETE ON symbols BEGIN SELECT 1; END;
CREATE INDEX idx_symbols_parent_name_kind
  ON symbols(parent_symbol_id, name, kind);
CREATE TABLE crg_nodes (
  id INTEGER PRIMARY KEY, domain TEXT, native_table TEXT,
  native_id INTEGER, stable_key TEXT, kind TEXT, name TEXT,
  path TEXT, module TEXT, source_revision TEXT, extra TEXT,
  updated_at INTEGER);
CREATE TABLE crg_edges (
  id INTEGER PRIMARY KEY, domain TEXT, source_node_id INTEGER,
  target_node_id INTEGER, edge_kind TEXT, edge_subkind TEXT,
  weight REAL, native_table TEXT, native_id INTEGER, updated_at INTEGER);
CREATE TABLE crg_node_metrics (
  node_id INTEGER PRIMARY KEY, fan_in INTEGER DEFAULT 0,
  fan_out INTEGER DEFAULT 0, hard_in INTEGER DEFAULT 0,
  descendants INTEGER DEFAULT 0, risk_score REAL DEFAULT 0,
  risk_tier TEXT DEFAULT 'low', reasons_json TEXT DEFAULT '[]',
  raw_counts_json TEXT DEFAULT '{}', scoring_version TEXT DEFAULT '3',
  computed_at INTEGER DEFAULT 0);
CREATE TABLE crg_meta (key TEXT PRIMARY KEY, value TEXT NOT NULL);
CREATE TABLE source_override_edges (
  child_symbol_id INTEGER NOT NULL, parent_symbol_id INTEGER NOT NULL,
  confidence TEXT NOT NULL DEFAULT 'high', reason TEXT NOT NULL DEFAULT '',
  updated_at INTEGER NOT NULL DEFAULT 0,
  PRIMARY KEY(child_symbol_id, parent_symbol_id));
"""
        )
        connection.execute(
            "INSERT INTO meta(key,value) VALUES ('schema_version','3')"
        )
        connection.executemany(
            "INSERT INTO modules(id,name,path,module_type,build_cs_path) "
            "VALUES (?,?,?,?,?)",
            [
                (1, "CoreUObject", "Engine/Source/Runtime/CoreUObject", "Runtime", ""),
                (2, "MonolithCore", "Plugins/Monolith/Source/MonolithCore", "Plugin", ""),
            ],
        )
        connection.executemany(
            "INSERT INTO files(id,path,module_id,file_type,line_count,last_modified) "
            "VALUES (?,?,?,?,?,?)",
            [
                (
                    1,
                    "D:/UnrealEngine/Engine/Source/Runtime/CoreUObject/Public/UObject/Object.h",
                    1,
                    "header",
                    100,
                    0.0,
                ),
                (2, str(indexed_source), 2, "source", 100, 0.0),
            ],
        )
        symbols = [
            (1, "UActorComponent", "UActorComponent", "class", 1, 10, 20, None, "public", "class UActorComponent", "", 1),
            (2, "UFixtureComponent", "UFixtureComponent", "class", 2, 10, 20, None, "public", "class UFixtureComponent", "", 1),
            (3, "BeginPlay", "UActorComponent::BeginPlay", "function", 1, 30, 31, 1, "public", "void BeginPlay()", "", 0),
            (4, "BeginPlay", "UFixtureComponent::BeginPlay", "function", 2, 30, 31, 2, "public", "void BeginPlay()", "", 0),
            (5, "UObject", "UObject", "class", 1, 40, 80, None, "public", "class UObject", "Base Unreal object", 1),
            (6, "AActor", "AActor", "class", 1, 90, 120, None, "public", "class AActor", "Actor derives from UObject", 1),
        ]
        connection.executemany(
            "INSERT INTO symbols(id,name,qualified_name,kind,file_id,line_start,line_end,parent_symbol_id,access,signature,docstring,is_ue_macro) "
            "VALUES (?,?,?,?,?,?,?,?,?,?,?,?)",
            symbols,
        )
        connection.execute(
            "INSERT INTO inheritance(id,child_id,parent_id) VALUES (1,2,1)"
        )
        connection.execute(
            'INSERT INTO "references"(id,from_symbol_id,to_symbol_id,ref_kind,file_id,line) '
            "VALUES (1,6,5,'type',1,95)"
        )
        connection.execute(
            "INSERT INTO symbol_deprecations(symbol_name,version,message,kind) "
            "VALUES ('OldFixtureSymbol','5.7','fixture','function')"
        )
        connection.executemany(
            "INSERT INTO symbols_fts(rowid,name,qualified_name,docstring) "
            "VALUES (?,?,?,?)",
            [(row[0], row[1], row[2], row[10]) for row in symbols],
        )
        connection.execute(
            "INSERT INTO source_fts(file_id,line_number,text) VALUES (1,40,?)",
            ("class UObject is the base Unreal object;",),
        )
        connection.commit()
    finally:
        connection.close()


def backup_readonly_database(source: Path, destination: Path) -> None:
    source_connection = sqlite3.connect(
        f"{source.resolve().as_uri()}?mode=ro", uri=True, timeout=5
    )
    destination_connection = sqlite3.connect(destination)
    try:
        source_connection.backup(destination_connection)
    finally:
        destination_connection.close()
        source_connection.close()


def result_text(response: dict) -> str:
    content = response["result"]["content"]
    assert content and content[0]["type"] == "text", response
    return content[0]["text"]


def find_proxy_log_record(log_root: Path, request_id: int) -> dict:
    records: list[dict] = []
    for path in log_root.glob("*/proxy.jsonl"):
        for line in path.read_text(encoding="utf-8").splitlines():
            if line.strip():
                records.append(json.loads(line))
    for record in records:
        if record.get("call", {}).get("jsonrpc_id") == request_id:
            return record
    raise AssertionError(
        f"proxy log record id={request_id} not found under {log_root}"
    )


def executable_version(path: Path) -> dict:
    completed = subprocess.run(
        [str(path), "--version"],
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="strict",
        timeout=10,
        check=True,
    )
    return json.loads(completed.stdout)


def main() -> int:
    args = parse_args()
    assert_reproducible_build_contract()
    proxy = (
        args.proxy.resolve()
        if args.proxy is not None
        else current_proxy_from_manifest()
    )
    if not proxy.is_file():
        raise SystemExit(f"proxy executable not found: {proxy}")
    bundle_query, bundle_catalog, bundle_manifest = (
        current_query_bundle_from_manifest()
    )
    query_for_freshness = (
        args.query.resolve()
        if args.query is not None
        else bundle_query
    )
    if not query_for_freshness.is_file():
        raise SystemExit(f"query executable not found: {query_for_freshness}")

    expected_proxy_hash = compute_tool_source_generation_hash(
        PLUGIN_ROOT,
        "proxy",
    )
    actual_proxy_hash = executable_version(proxy).get("source_hash")
    assert actual_proxy_hash == expected_proxy_hash, (
        actual_proxy_hash,
        expected_proxy_hash,
    )

    expected_query_hash = compute_tool_source_generation_hash(
        PLUGIN_ROOT,
        "query",
    )
    actual_query_hash = executable_version(query_for_freshness).get(
        "source_hash"
    )
    assert actual_query_hash == expected_query_hash, (
        actual_query_hash,
        expected_query_hash,
    )

    with tempfile.TemporaryDirectory(prefix="monolith-proxy-offline-") as temp:
        temp_root = Path(temp)
        log_root = temp_root / "logs"
        hot_database = temp_root / "hot-journal.db"
        hot_journal = create_hot_rollback_journal(hot_database)
        indexed_source = (
            PROJECT_ROOT
            / "Plugins"
            / "Monolith"
            / "Source"
            / "MonolithCore"
            / "Private"
            / "MonolithHttpServer.cpp"
        )
        assert indexed_source.is_file(), indexed_source
        source_body_sentinel = "MonolithActionExecutionGuard.h"
        source_fixture = temp_root / "source-fixture.db"
        create_source_fixture(source_fixture, indexed_source)
        project_fixture = temp_root / "project-fixture.db"
        backup_readonly_database(
            PLUGIN_ROOT / "Saved" / "ProjectIndex.db", project_fixture
        )
        attacker_source_db = temp_root / "attacker-source.db"
        connection = sqlite3.connect(attacker_source_db)
        try:
            connection.execute(
                "CREATE TABLE files (id INTEGER PRIMARY KEY, path TEXT NOT NULL)"
            )
            connection.execute(
                "INSERT INTO files(path) VALUES (?)",
                ("C:/Windows/win.ini",),
            )
            connection.commit()
        finally:
            connection.close()
        env = os.environ.copy()
        env.update(
            {
                "MONOLITH_URL": "http://127.0.0.1:9/mcp",
                "MONOLITH_OFFLINE_FALLBACK": "1",
                "MONOLITH_CALL_LOG": "0",
                "MONOLITH_TOOL_LOG_ENABLED": "1",
                "MONOLITH_TOOL_LOG_DIR": str(log_root),
                "LOCALAPPDATA": str(temp_root / "localappdata"),
                "MONOLITH_EXPECTED_PROJECT_ROOT": str(PROJECT_ROOT),
                "MONOLITH_CATALOG_SNAPSHOT": str(
                    PLUGIN_ROOT
                    / "Tools"
                    / "MonolithQuery"
                    / "Generated"
                    / "monolith_catalog_snapshot.json"
                ),
                "MONOLITH_OFFLINE_SOURCE_DB": str(source_fixture),
                "MONOLITH_OFFLINE_PROJECT_DB": str(project_fixture),
            }
        )
        if args.query is None:
            env.pop("MONOLITH_QUERY_EXE", None)
        else:
            query = args.query.resolve()
            if not query.is_file():
                raise SystemExit(f"query executable not found: {query}")
            env["MONOLITH_QUERY_EXE"] = str(query)

        direct_query_root = temp_root / "direct-query-plugin"
        direct_query_binaries = direct_query_root / "Binaries"
        direct_query_binaries.mkdir(parents=True)
        shutil.copy2(
            PLUGIN_ROOT / "Monolith.uplugin",
            direct_query_root / "Monolith.uplugin",
        )
        shutil.copy2(
            PLUGIN_ROOT / "Binaries" / "monolith_query.current.json",
            direct_query_binaries / "monolith_query.current.json",
        )
        shutil.copy2(
            bundle_query,
            direct_query_binaries / bundle_manifest["file"],
        )
        shutil.copy2(
            bundle_catalog,
            direct_query_binaries / bundle_manifest["catalog_file"],
        )
        direct_fixed_query = direct_query_binaries / "monolith_query.exe"
        shutil.copy2(bundle_query, direct_fixed_query)
        direct_query_command = [
            str(direct_fixed_query),
            "--readonly",
            "source",
            "health",
            f"--source-db={source_fixture}",
            "--include-counts=false",
        ]
        direct_fixed_valid = subprocess.run(
            direct_query_command,
            capture_output=True,
            text=True,
            encoding="utf-8",
            errors="strict",
            timeout=30,
            check=False,
        )
        assert direct_fixed_valid.returncode == 0, direct_fixed_valid
        (direct_query_binaries / "monolith_query.current.json").write_text(
            "{}", encoding="utf-8"
        )
        direct_fixed_invalid = subprocess.run(
            direct_query_command,
            capture_output=True,
            text=True,
            encoding="utf-8",
            errors="strict",
            timeout=30,
            check=False,
        )
        assert direct_fixed_invalid.returncode != 0, direct_fixed_invalid
        assert "manifest" in direct_fixed_invalid.stderr.lower(), (
            direct_fixed_invalid
        )

        # Exercise the production branch independently of the mutable fixed-name
        # compatibility executable and the source-tree catalog override.  The
        # isolated Binaries directory contains only the manifest-selected
        # immutable Query/catalog generation.
        production_bundle_root = temp_root / "production-bundle" / "Binaries"
        production_bundle_root.mkdir(parents=True)
        production_proxy = production_bundle_root / proxy.name
        shutil.copy2(proxy, production_proxy)
        shutil.copy2(
            PLUGIN_ROOT / "Binaries" / "monolith_query.current.json",
            production_bundle_root / "monolith_query.current.json",
        )
        shutil.copy2(
            bundle_query,
            production_bundle_root / bundle_manifest["file"],
        )
        shutil.copy2(
            bundle_catalog,
            production_bundle_root / bundle_manifest["catalog_file"],
        )
        production_bundle_env = env.copy()
        production_bundle_env.pop("MONOLITH_QUERY_EXE", None)
        production_bundle_env.pop("MONOLITH_CATALOG_SNAPSHOT", None)
        production_bundle_env["MONOLITH_TOOL_LOG_DIR"] = str(
            temp_root / "production-bundle-logs"
        )
        production_bundle_session = launch_proxy(
            production_proxy, production_bundle_env
        )
        production_manifest_path = (
            production_bundle_root / "monolith_query.current.json"
        )
        production_manifest_bytes = production_manifest_path.read_bytes()
        try:
            production_bundle_session.request(
                690,
                "initialize",
                {
                    "protocolVersion": "2025-11-25",
                    "capabilities": {},
                    "clientInfo": {
                        "name": "immutable-query-bundle-smoke",
                        "version": "1",
                    },
                },
            )
            production_tools = production_bundle_session.request(
                691, "tools/list", {}
            )
            assert {
                tool["name"] for tool in production_tools["result"]["tools"]
            } == {
                "monolith_query",
                "monolith_status",
                "monolith_discover",
                "monolith_find",
            }
            production_source = call_tool(
                production_bundle_session,
                692,
                "monolith_query",
                {
                    "namespace": "source",
                    "action": "health",
                    "params": {"include_counts": True},
                },
            )
            assert production_source["result"]["isError"] is False, (
                production_source,
                production_bundle_session.stderr_lines,
            )
            production_catalog = call_tool(
                production_bundle_session,
                693,
                "monolith_discover",
                {"namespace": "project", "mode": "actions", "limit": 1},
            )
            assert production_catalog["result"]["isError"] is False, (
                production_catalog,
                production_bundle_session.stderr_lines,
            )
            assert (
                production_catalog["result"]["structuredContent"]["status"]
                == "degraded"
            )

            replacement_manifest = production_manifest_path.with_name(
                "monolith_query.current.replacement.json"
            )
            replacement_manifest.write_text("{}", encoding="utf-8")
            os.replace(replacement_manifest, production_manifest_path)
            pinned_source = call_tool(
                production_bundle_session,
                694,
                "monolith_query",
                {
                    "namespace": "source",
                    "action": "health",
                    "params": {"include_counts": False},
                },
            )
            assert pinned_source["result"]["isError"] is False, pinned_source
            pinned_catalog = call_tool(
                production_bundle_session,
                695,
                "monolith_discover",
                {"namespace": "project", "mode": "actions", "limit": 2},
            )
            assert pinned_catalog["result"]["isError"] is False, pinned_catalog
        finally:
            restore_manifest = production_manifest_path.with_name(
                "monolith_query.current.restore.json"
            )
            restore_manifest.write_bytes(production_manifest_bytes)
            os.replace(restore_manifest, production_manifest_path)
            production_bundle_session.close()

        def assert_production_bundle_rejected(
            binaries_root: Path, request_base: int, log_name: str
        ) -> None:
            rejected_env = production_bundle_env.copy()
            rejected_env["MONOLITH_TOOL_LOG_DIR"] = str(temp_root / log_name)
            rejected_session = launch_proxy(
                binaries_root / proxy.name, rejected_env
            )
            try:
                rejected_session.request(
                    request_base,
                    "initialize",
                    {
                        "protocolVersion": "2025-11-25",
                        "capabilities": {},
                        "clientInfo": {
                            "name": "invalid-query-bundle-smoke",
                            "version": "1",
                        },
                    },
                )
                rejected = call_tool(
                    rejected_session,
                    request_base + 1,
                    "monolith_discover",
                    {"namespace": "project", "mode": "actions", "limit": 1},
                )
                assert rejected["result"]["isError"] is True, (
                    rejected,
                    rejected_session.stderr_lines,
                )
            finally:
                rejected_session.close()

        sha_tamper_root = temp_root / "sha-tampered-bundle" / "Binaries"
        shutil.copytree(production_bundle_root, sha_tamper_root)
        with (sha_tamper_root / bundle_manifest["catalog_file"]).open("ab") as output:
            output.write(b" ")
        assert_production_bundle_rejected(
            sha_tamper_root, 700, "sha-tampered-bundle-logs"
        )

        query_sha_tamper_root = (
            temp_root / "query-sha-tampered-bundle" / "Binaries"
        )
        shutil.copytree(production_bundle_root, query_sha_tamper_root)
        with (query_sha_tamper_root / bundle_manifest["file"]).open("ab") as output:
            output.write(b" ")
        assert_production_bundle_rejected(
            query_sha_tamper_root, 705, "query-sha-tampered-bundle-logs"
        )

        missing_query_root = temp_root / "missing-query-bundle" / "Binaries"
        shutil.copytree(production_bundle_root, missing_query_root)
        (missing_query_root / bundle_manifest["file"]).unlink()
        assert_production_bundle_rejected(
            missing_query_root, 707, "missing-query-bundle-logs"
        )

        leaf_tamper_root = temp_root / "leaf-tampered-bundle" / "Binaries"
        shutil.copytree(production_bundle_root, leaf_tamper_root)
        leaf_manifest_path = leaf_tamper_root / "monolith_query.current.json"
        leaf_manifest = json.loads(leaf_manifest_path.read_text(encoding="utf-8"))
        leaf_manifest["catalog_file"] = "../outside-catalog.json"
        leaf_manifest_path.write_text(
            json.dumps(leaf_manifest, separators=(",", ":")), encoding="utf-8"
        )
        assert_production_bundle_rejected(
            leaf_tamper_root, 710, "leaf-tampered-bundle-logs"
        )

        invalid_bundle_live_server = ThreadingHTTPServer(
            ("127.0.0.1", 0), LiveErrorHandler
        )
        invalid_bundle_live_thread = threading.Thread(
            target=invalid_bundle_live_server.serve_forever, daemon=True
        )
        invalid_bundle_live_thread.start()
        invalid_bundle_live_env = production_bundle_env.copy()
        invalid_bundle_live_env["MONOLITH_URL"] = (
            f"http://127.0.0.1:{invalid_bundle_live_server.server_port}/mcp"
        )
        invalid_bundle_live_env["MONOLITH_TOOL_LOG_DIR"] = str(
            temp_root / "invalid-bundle-live-logs"
        )
        invalid_bundle_live_session = launch_proxy(
            sha_tamper_root / proxy.name, invalid_bundle_live_env
        )
        try:
            invalid_bundle_live_session.request(
                720,
                "initialize",
                {
                    "protocolVersion": "2025-11-25",
                    "capabilities": {},
                    "clientInfo": {
                        "name": "invalid-bundle-live-route-smoke",
                        "version": "1",
                    },
                },
            )
            live_despite_invalid_bundle = call_tool(
                invalid_bundle_live_session,
                721,
                "monolith_discover",
                {"namespace": "project", "mode": "actions", "limit": 1},
            )
            assert result_text(live_despite_invalid_bundle) == "LIVE_SENTINEL", (
                live_despite_invalid_bundle,
                invalid_bundle_live_session.stderr_lines,
            )
            assert "offline_fallback" not in json.dumps(
                live_despite_invalid_bundle
            )
        finally:
            invalid_bundle_live_session.close()
            invalid_bundle_live_server.shutdown()
            invalid_bundle_live_server.server_close()
            invalid_bundle_live_thread.join(timeout=2)

        cache_dir = Path(env["LOCALAPPDATA"]) / "Monolith"
        cache_dir.mkdir(parents=True, exist_ok=True)
        cache_path = cache_dir / "monolith_proxy_tools_127_0_0_1_9.json"
        cache_path.write_text(
            json.dumps(
                [
                    {
                        "name": "cached_probe_tool",
                        "description": "stale profile cache probe",
                        "inputSchema": {
                            "type": "object",
                            "properties": {},
                        },
                    }
                ],
                separators=(",", ":"),
            ),
            encoding="utf-8",
        )

        session = launch_proxy(proxy, env)
        try:
            initialized = session.request(
                1,
                "initialize",
                {
                    "protocolVersion": "2025-11-25",
                    "capabilities": {},
                    "clientInfo": {"name": "offline-smoke", "version": "1"},
                },
            )
            assert initialized["result"]["serverInfo"]["name"] == "monolith-proxy"

            tools_list = session.request(2, "tools/list", {})
            tool_names = {
                tool["name"] for tool in tools_list["result"]["tools"]
            }
            expected_offline_tools = {
                "monolith_query",
                "monolith_status",
                "monolith_discover",
                "monolith_find",
            }
            assert tool_names == expected_offline_tools, sorted(tool_names)
            assert "cached_probe_tool" not in tool_names
            assert "source_query" not in tool_names
            tools_by_name = {
                tool["name"]: tool for tool in tools_list["result"]["tools"]
            }
            for shaped_tool_name in expected_offline_tools:
                shaped_properties = tools_by_name[shaped_tool_name][
                    "inputSchema"
                ]["properties"]
                assert {
                    "_fields",
                    "_omit",
                    "_compact_json",
                } <= shaped_properties.keys(), shaped_tool_name

            generic_source_health = call_tool(
                session,
                77,
                "monolith_query",
                {
                    "namespace": "source",
                    "action": "health",
                    "params": {"include_counts": False},
                },
            )
            assert generic_source_health["result"]["isError"] is False, (
                generic_source_health,
                session.stderr_lines,
            )
            assert generic_source_health["result"]["structuredContent"][
                "status"
            ] == "ok"

            source_health = call_tool(
                session,
                3,
                "source_query",
                {"action": "health", "params": {"include_counts": False}},
            )
            assert source_health["result"]["isError"] is False, source_health
            structured = source_health["result"]["structuredContent"]
            assert structured["status"] == "ok", structured
            offline = structured["_monolith"]
            assert offline["offline_fallback"] is True
            assert (
                offline["routing_context"]["decision_source"]
                == "offline_fallback"
            )
            assert offline["routing_context"]["namespace"] == "source"

            shaped_query_fields = call_tool(
                session,
                36,
                "source_query",
                {
                    "action": "health",
                    "params": {"include_counts": False},
                    "_fields": ["status"],
                },
            )
            assert shaped_query_fields["result"]["isError"] is False
            shaped_fields_structured = shaped_query_fields["result"][
                "structuredContent"
            ]
            assert shaped_fields_structured == {"status": "ok"}
            assert json.loads(result_text(shaped_query_fields)) == (
                shaped_fields_structured
            )

            shaped_query_nested_precedence = call_tool(
                session,
                37,
                "source_query",
                {
                    "action": "health",
                    "params": {
                        "include_counts": False,
                        "_fields": ["status"],
                    },
                    "_fields": ["summary"],
                },
            )
            nested_precedence_structured = shaped_query_nested_precedence[
                "result"
            ]["structuredContent"]
            assert nested_precedence_structured == {"status": "ok"}

            shaped_query_omit = call_tool(
                session,
                50,
                "source_query",
                {
                    "action": "health",
                    "params": {"include_counts": False},
                    "_omit": ["summary"],
                },
            )
            omit_structured = shaped_query_omit["result"]["structuredContent"]
            assert shaped_query_omit["result"]["isError"] is False
            assert "summary" not in omit_structured
            assert omit_structured["status"] == "ok"
            assert json.loads(result_text(shaped_query_omit)) == omit_structured

            shaped_stringified_fields = call_tool(
                session,
                51,
                "source_query",
                {
                    "action": "health",
                    "params": {"include_counts": False},
                    "_fields": json.dumps(["status"]),
                },
            )
            assert shaped_stringified_fields["result"]["isError"] is False
            assert shaped_stringified_fields["result"][
                "structuredContent"
            ] == {"status": "ok"}

            shaped_query_compact = call_tool(
                session,
                38,
                "source_query",
                {
                    "action": "health",
                    "params": {"include_counts": False},
                    "_compact_json": True,
                },
            )
            compact_structured = shaped_query_compact["result"][
                "structuredContent"
            ]
            assert shaped_query_compact["result"]["isError"] is False
            assert "warnings" not in compact_structured
            assert json.loads(result_text(shaped_query_compact)) == (
                compact_structured
            )

            shaped_query_unknown_field = call_tool(
                session,
                39,
                "source_query",
                {
                    "action": "health",
                    "params": {"include_counts": False},
                    "_fields": ["definitely_missing_response_field"],
                },
            )
            assert shaped_query_unknown_field["result"]["isError"] is False
            assert shaped_query_unknown_field["result"]["structuredContent"] == {}
            assert json.loads(result_text(shaped_query_unknown_field)) == {}

            invalid_shaping_array = call_tool(
                session,
                40,
                "source_query",
                {
                    "action": "health",
                    "params": {"include_counts": False},
                    "_fields": ["status", 7],
                },
            )
            assert invalid_shaping_array["result"]["isError"] is True
            assert "must contain only strings" in result_text(
                invalid_shaping_array
            )

            indexed_source_read = call_tool(
                session,
                22,
                "source_query",
                {
                    "action": "read_file",
                    "params": {
                        "file_path": str(indexed_source),
                        "start": 1,
                        "end": 8,
                    },
                },
            )
            assert indexed_source_read["result"]["isError"] is False
            assert source_body_sentinel in result_text(indexed_source_read)
            post_source_read_health = call_tool(
                session,
                23,
                "source_query",
                {
                    "action": "health",
                    "params": {
                        "include_counts": False,
                        "include_deep_checks": False,
                    },
                },
            )
            assert post_source_read_health["result"]["isError"] is False

            attacker_db_override = call_tool(
                session,
                83,
                "source_query",
                {
                    "action": "read_file",
                    "params": {
                        "file_path": "C:/Windows/win.ini",
                        "source_db": str(attacker_source_db),
                    },
                },
            )
            assert attacker_db_override["result"]["isError"] is True
            assert "proxy-controlled query path" in result_text(
                attacker_db_override
            )

            unindexed_system_file = call_tool(
                session,
                78,
                "source_query",
                {
                    "action": "read_file",
                    "params": {
                        "file_path": "C:/Windows/win.ini",
                    },
                },
            )
            assert unindexed_system_file["result"]["isError"] is True
            assert "restricted to indexed source paths" in result_text(
                unindexed_system_file
            )

            database_override_guard = call_tool(
                session,
                21,
                "source_query",
                {
                    "action": "health",
                    "params": {"source_db": str(hot_database)},
                },
            )
            assert database_override_guard["result"]["isError"] is True
            assert "proxy-controlled query path" in (
                result_text(database_override_guard)
            )
            assert hot_journal.is_file(), hot_journal

            catalog_status = call_tool(session, 4, "monolith_status", {})
            assert catalog_status["result"]["isError"] is False, catalog_status
            catalog_structured = catalog_status["result"]["structuredContent"]
            assert (
                catalog_structured["_monolith"]["routing_context"]["action"]
                == "status"
            )
            editor_recovery = catalog_structured["recovery_plan"][
                "editor_candidate_status"
            ]
            assert editor_recovery["headless_editor_command_exists"] is True
            assert editor_recovery["headless_editor_command"].replace(
                "\\", "/"
            ).endswith("/Build/BatchFiles/RunHeadlessEditor.bat")
            assert catalog_structured["counts"]["offline_actions"] >= 90

            shaped_meta_collision = call_tool(
                session,
                41,
                "monolith_status",
                {"_fields": ["status"], "_omit": ["status"]},
            )
            collision_structured = shaped_meta_collision["result"][
                "structuredContent"
            ]
            assert shaped_meta_collision["result"]["isError"] is False
            assert collision_structured["status"] == "ok"
            assert set(collision_structured) == {"status", "warnings"}
            assert any(
                "mutually exclusive" in warning
                for warning in collision_structured["warnings"]
            )
            assert json.loads(result_text(shaped_meta_collision)) == (
                collision_structured
            )

            invalid_meta_compact = call_tool(
                session,
                42,
                "monolith_status",
                {"_compact_json": {"not": "a boolean"}},
            )
            assert invalid_meta_compact["result"]["isError"] is True
            assert "must be a boolean" in result_text(invalid_meta_compact)

            snapshot_override = call_tool(
                session,
                84,
                "monolith_status",
                {"snapshot": "C:/untrusted/catalog.json"},
            )
            assert snapshot_override["result"]["isError"] is True
            assert "proxy-controlled query path" in result_text(
                snapshot_override
            )

            project_discovery = call_tool(
                session,
                24,
                "monolith_discover",
                {"namespace": "project", "mode": "actions", "limit": 200},
            )
            assert project_discovery["result"]["isError"] is False
            project_discovery_content = project_discovery["result"][
                "structuredContent"
            ]
            assert project_discovery_content["projection"] == "compact"
            discovered_project_rows = {
                row["action"]: row for row in project_discovery_content["results"]
            }
            discovered_project_actions = set(discovered_project_rows)
            assert {
                "search",
                "find_by_type",
                "get_asset_details",
            } <= discovered_project_actions
            project_search_discovery = discovered_project_rows["search"]
            assert project_search_discovery["available_offline"] is True
            assert project_search_discovery["executes_offline"] is True
            assert project_search_discovery["offline_mode"] == "execute"
            assert project_search_discovery["requires_live_editor"] is False

            console_discovery = call_tool(
                session,
                41,
                "monolith_discover",
                {"namespace": "console", "mode": "actions", "limit": 200},
            )
            assert console_discovery["result"]["isError"] is False
            discovered_console_rows = {
                row["action"]: row
                for row in console_discovery["result"]["structuredContent"][
                    "results"
                ]
            }
            console_execute_discovery = discovered_console_rows["execute"]
            assert console_execute_discovery["available_offline"] is True
            assert console_execute_discovery["executes_offline"] is False
            assert console_execute_discovery["offline_mode"] == "guidance"
            assert console_execute_discovery["requires_live_editor"] is True

            source_discovery = call_tool(
                session,
                42,
                "monolith_discover",
                {
                    "namespace": "source",
                    "mode": "actions",
                    "detail": True,
                    "limit": 200,
                },
            )
            assert source_discovery["result"]["isError"] is False
            source_discovery_structured = source_discovery["result"][
                "structuredContent"
            ]
            assert source_discovery_structured["status"] == "degraded"
            assert (
                source_discovery_structured["completion_class"]
                == "degraded_guidance"
            )
            assert source_discovery_structured["catalog_matches_live"] == "unknown"
            discovered_source_rows = {
                row["action"]: row
                for row in source_discovery_structured["results"]
            }
            source_reindex_discovery = discovered_source_rows["trigger_reindex"]
            assert source_reindex_discovery["available_offline"] is True
            assert source_reindex_discovery["executes_offline"] is False
            assert source_reindex_discovery["offline_mode"] == "guidance"
            assert source_reindex_discovery["requires_live_editor"] is True
            assert (
                source_reindex_discovery["implementation_status"]
                == "live_only_guidance"
            )
            assert "source_scanned_candidate" in source_reindex_discovery[
                "planning_signals"
            ]

            degraded_schema = call_tool(
                session,
                79,
                "monolith_discover",
                {
                    "namespace": "project",
                    "action": "find_by_type",
                    "mode": "schema",
                    "planning_detail": "full",
                    "schema_detail": "full",
                },
            )
            assert degraded_schema["result"]["isError"] is False
            degraded_structured = degraded_schema["result"]["structuredContent"]
            assert degraded_structured["status"] == "degraded"
            assert degraded_structured["completion_class"] == "degraded_guidance"
            assert degraded_structured["schema_available"] is False
            assert degraded_structured["catalog_matches_live"] == "unknown"

            project_find = call_tool(
                session,
                25,
                "monolith_find",
                {"query": "find_by_type", "limit": 10},
            )
            assert project_find["result"]["isError"] is False
            project_find_structured = project_find["result"]["structuredContent"]
            assert project_find_structured["status"] == "degraded"
            assert (
                project_find_structured["completion_class"]
                == "degraded_guidance"
            )
            assert project_find_structured["catalog_matches_live"] == "unknown"
            assert any(
                row.get("full_name") == "project.find_by_type"
                for row in project_find_structured["results"]
            )
            project_find_by_type_row = next(
                row
                for row in project_find_structured["results"]
                if row.get("full_name") == "project.find_by_type"
            )
            assert (
                project_find_by_type_row["implementation_status"]
                == "implemented_offline"
            )

            projected_find = call_tool(
                session,
                80,
                "monolith_find",
                {
                    "query": "find_by_type",
                    "namespace": "project",
                    "fields": ["action_id", "description", "status"],
                    "include_schema": False,
                    "planning_detail": "compact",
                    "limit": 5,
                },
            )
            assert projected_find["result"]["isError"] is False
            projected_rows = projected_find["result"]["structuredContent"][
                "results"
            ]
            assert projected_rows
            assert set(projected_rows[0]) <= {
                "action_id",
                "description",
                "status",
            }

            live_only_export = call_tool(
                session,
                81,
                "monolith_query",
                {
                    "namespace": "project",
                    "action": "export_asset_text",
                    "params": {"asset_path": "/Game/Probe"},
                },
            )
            assert live_only_export["result"]["isError"] is True
            export_structured = live_only_export["result"]["structuredContent"]
            assert export_structured["status"] == "live_only"
            assert export_structured["completion_class"] == "requires_live"

            catalog_guide = call_tool(
                session,
                12,
                "monolith_guide",
                {"section": "recipes"},
            )
            assert catalog_guide["result"]["isError"] is False, catalog_guide
            guide_structured = catalog_guide["result"]["structuredContent"]
            assert "output" in guide_structured
            assert "recipes" in guide_structured["output"].lower()
            assert (
                guide_structured["_monolith"]["routing_context"]["action"]
                == "guide"
            )

            project_health = call_tool(
                session,
                8,
                "project_query",
                {"action": "health", "params": {"include_counts": False}},
            )
            assert project_health["result"]["isError"] is False, project_health
            project_structured = project_health["result"]["structuredContent"]
            assert project_structured["status"] in {"ok", "warning"}
            assert (
                project_structured["_monolith"]["routing_context"]["namespace"]
                == "project"
            )

            source_text = call_tool(
                session,
                13,
                "source_query",
                {
                    "action": "search_source",
                    "params": {"query": "UObject", "limit": 1},
                },
            )
            assert source_text["result"]["isError"] is False, source_text
            source_text_structured = source_text["result"]["structuredContent"]
            assert "output" in source_text_structured
            assert "uobject" in source_text_structured["output"].lower()

            query_top_level_params = call_tool(
                session,
                43,
                "source_query",
                {"action": "search_source", "query": "UObject", "limit": 1},
            )
            assert query_top_level_params["result"]["isError"] is False
            assert "uobject" in result_text(query_top_level_params).lower()

            query_nested_precedence = call_tool(
                session,
                44,
                "source_query",
                {
                    "action": "search_source",
                    "query": "DefinitelyMissingTopLevelSymbol",
                    "limit": 1,
                    "params": {"query": "UObject", "limit": 1},
                },
            )
            assert query_nested_precedence["result"]["isError"] is False
            assert "uobject" in result_text(query_nested_precedence).lower()

            query_string_params = call_tool(
                session,
                45,
                "source_query",
                {
                    "action": "search_source",
                    "params": json.dumps({"query": "UObject", "limit": 1}),
                },
            )
            assert query_string_params["result"]["isError"] is False
            assert "uobject" in result_text(query_string_params).lower()

            malformed_query_string_params = call_tool(
                session,
                46,
                "source_query",
                {"action": "search_source", "params": "{not-json"},
            )
            assert malformed_query_string_params["result"]["isError"] is True
            assert "valid JSON-encoded object" in result_text(
                malformed_query_string_params
            )

            meta_nested_precedence = call_tool(
                session,
                47,
                "monolith_find",
                {
                    "query": "DefinitelyMissingTopLevelAction",
                    "params": {"query": "find_by_type", "limit": 1},
                },
            )
            assert meta_nested_precedence["result"]["isError"] is False
            assert any(
                row.get("full_name") == "project.find_by_type"
                for row in meta_nested_precedence["result"][
                    "structuredContent"
                ]["results"]
            )

            meta_string_params = call_tool(
                session,
                48,
                "monolith_guide",
                {"params": json.dumps({"section": "recipes"})},
            )
            assert meta_string_params["result"]["isError"] is False
            assert "recipes" in result_text(meta_string_params).lower()

            malformed_meta_string_params = call_tool(
                session,
                49,
                "monolith_guide",
                {"params": "[]"},
            )
            assert malformed_meta_string_params["result"]["isError"] is True
            assert "must decode to a JSON object" in result_text(
                malformed_meta_string_params
            )

            source_symbol_kind = call_tool(
                session,
                17,
                "source_query",
                {
                    "action": "search_source",
                    "params": {
                        "query": "UObject",
                        "symbol_kind": "class",
                        "limit": 1,
                    },
                },
            )
            assert source_symbol_kind["result"]["isError"] is False

            unsupported_mode = call_tool(
                session,
                18,
                "source_query",
                {
                    "action": "search_source",
                    "params": {"query": "UObject", "mode": "regex"},
                },
            )
            assert unsupported_mode["result"]["isError"] is True
            assert "mode=fts" in result_text(unsupported_mode)
            assert "exact" in result_text(unsupported_mode)

            supported_path_filter = call_tool(
                session,
                19,
                "source_query",
                {
                    "action": "search_source",
                    "params": {
                        "query": "UObject",
                        "path_filter": "CoreUObject",
                        "limit": 1,
                    },
                },
            )
            assert supported_path_filter["result"]["isError"] is False
            assert "uobject" in result_text(supported_path_filter).lower()

            exact_source_search = call_tool(
                session,
                82,
                "source_query",
                {
                    "action": "search_source",
                    "params": {"query": "UObject", "mode": "exact", "limit": 1},
                },
            )
            assert exact_source_search["result"]["isError"] is False
            assert "uobject" in result_text(exact_source_search).lower()

            unsupported_example_cursor = call_tool(
                session,
                20,
                "source_query",
                {
                    "action": "find_example_usage",
                    "params": {"symbol": "UObject", "cursor": "opaque"},
                },
            )
            assert unsupported_example_cursor["result"]["isError"] is True
            assert "does not support parameter 'cursor'" in result_text(
                unsupported_example_cursor
            )

            source_references = call_tool(
                session,
                15,
                "source_query",
                {
                    "action": "find_references",
                    "params": {"symbol": "UObject", "limit": 1},
                },
            )
            assert source_references["result"]["isError"] is False, source_references
            assert "references" in result_text(source_references).lower()

            deprecations = call_tool(
                session,
                17,
                "source_query",
                {
                    "action": "check_deprecations",
                    "params": {"symbols": ["UObject", "AActor"]},
                },
            )
            assert deprecations["result"]["isError"] is False, deprecations
            assert "uobject" in result_text(deprecations).lower()

            assets_by_type = call_tool(
                session,
                18,
                "project_query",
                {
                    "action": "find_by_type",
                    "params": {"asset_type": "Blueprint", "limit": 1},
                },
            )
            assert assets_by_type["result"]["isError"] is False, assets_by_type
            assets_structured = assets_by_type["result"]["structuredContent"]
            assert assets_structured["success"] is True
            assert assets_structured["limit"] == 1
            assert assets_structured["offset"] == 0
            assert assets_structured["assets"], assets_structured
            assert assets_structured["results"] == assets_structured["assets"]

            existing_asset_path = assets_structured["assets"][0]["package_path"]
            project_references = call_tool(
                session,
                26,
                "project_query",
                {
                    "action": "find_references",
                    "params": {"asset_path": existing_asset_path},
                },
            )
            references_structured = project_references["result"]["structuredContent"]
            assert project_references["result"]["isError"] is False
            assert references_structured["asset_path"] == existing_asset_path
            assert set(references_structured["references"]) == {
                "depends_on",
                "referenced_by",
            }

            project_details = call_tool(
                session,
                27,
                "project_query",
                {
                    "action": "get_asset_details",
                    "params": {"asset_path": existing_asset_path},
                },
            )
            details_structured = project_details["result"]["structuredContent"]
            assert project_details["result"]["isError"] is False
            assert details_structured["success"] is True
            assert isinstance(details_structured["asset"]["file_size_bytes"], int)
            assert "references" in details_structured["asset"]

            project_stats = call_tool(
                session,
                28,
                "project_query",
                {"action": "get_stats", "params": {}},
            )
            stats_structured = project_stats["result"]["structuredContent"]
            assert project_stats["result"]["isError"] is False
            assert stats_structured["success"] is True
            assert stats_structured["indexing"] is False
            assert isinstance(stats_structured["stats"]["assets"], int)

            changed_ranges = call_tool(
                session,
                19,
                "source_query",
                {
                    "action": "detect_changes",
                    "params": {
                        "paths": ["Source/Foo.cpp"],
                        "changed_ranges": [
                            {
                                "path": "Source/Foo.cpp",
                                "ranges": [[10, 10]],
                            }
                        ],
                    },
                },
            )
            assert changed_ranges["result"]["isError"] is False, changed_ranges
            changed_input = changed_ranges["result"]["structuredContent"]["input"]
            assert changed_input["precision"] == "line"
            assert changed_input["range_paths"] == 1

            missing_asset = call_tool(
                session,
                16,
                "project_query",
                {
                    "action": "get_asset_details",
                    "params": {"asset_path": "/Game/DefinitelyMissingAsset"},
                },
            )
            assert missing_asset["result"]["isError"] is True, missing_asset
            missing_structured = missing_asset["result"]["structuredContent"]
            assert "Asset not found" in missing_structured["error"]
            assert missing_structured["_monolith"]["process"]["exit_code"] == 0

            source_review_error = call_tool(
                session,
                29,
                "source_query",
                {
                    "action": "risk_score",
                    "params": {"symbol": "DefinitelyMissingSymbolForOfflineReview"},
                },
            )
            assert source_review_error["result"]["isError"] is True
            assert "Symbol not found" in result_text(source_review_error)

            source_check_failure = call_tool(
                session,
                30,
                "source_query",
                {"action": "detect_changes", "params": {}},
            )
            assert source_check_failure["result"]["isError"] is False
            assert (
                source_check_failure["result"]["structuredContent"]["status"]
                == "error"
            )

            invalid_limit_type = call_tool(
                session,
                31,
                "project_query",
                {
                    "action": "find_by_type",
                    "params": {"asset_type": "Blueprint", "limit": "bogus"},
                },
            )
            assert invalid_limit_type["result"]["isError"] is True
            assert "must be an integer" in result_text(invalid_limit_type)

            invalid_bool_type = call_tool(
                session,
                32,
                "source_query",
                {
                    "action": "health",
                    "params": {"include_counts": "bogus"},
                },
            )
            assert invalid_bool_type["result"]["isError"] is True
            assert "must be a boolean" in result_text(invalid_bool_type)

            invalid_string_type = call_tool(
                session,
                33,
                "source_query",
                {
                    "action": "search_source",
                    "params": {"query": True},
                },
            )
            assert invalid_string_type["result"]["isError"] is True
            assert "must be a string" in result_text(invalid_string_type)

            invalid_null = call_tool(
                session,
                34,
                "source_query",
                {"action": "search_source", "params": {"query": None}},
            )
            assert invalid_null["result"]["isError"] is True
            assert "must be non-null" in result_text(invalid_null)

            invalid_symbols_type = call_tool(
                session,
                35,
                "source_query",
                {
                    "action": "check_deprecations",
                    "params": {"symbols": "UObject,AActor"},
                },
            )
            assert invalid_symbols_type["result"]["isError"] is True
            assert "must be an array" in result_text(invalid_symbols_type)

            integral_float_limit = call_tool(
                session,
                36,
                "project_query",
                {
                    "action": "find_by_type",
                    "params": {"asset_type": "Blueprint", "limit": 1.0},
                },
            )
            assert integral_float_limit["result"]["isError"] is False
            assert integral_float_limit["result"]["structuredContent"]["limit"] == 1

            fractional_integer = call_tool(
                session,
                37,
                "project_query",
                {
                    "action": "find_by_type",
                    "params": {"asset_type": "Blueprint", "limit": 1.5},
                },
            )
            assert fractional_integer["result"]["isError"] is True
            assert "must be an integer" in result_text(fractional_integer)

            alias_collision = call_tool(
                session,
                38,
                "project_query",
                {
                    "action": "find_by_type",
                    "params": {
                        "asset_type": "Blueprint",
                        "asset_class": "Material",
                    },
                },
            )
            assert alias_collision["result"]["isError"] is True
            assert "aliases of the same canonical parameter" in result_text(
                alias_collision
            )

            override_cursor = call_tool(
                session,
                39,
                "source_query",
                {
                    "action": "find_overrides",
                    "params": {
                        "symbol": "UActorComponent::BeginPlay",
                        "cursor": "1",
                    },
                },
            )
            assert override_cursor["result"]["isError"] is False

            invalid_override_cursor = call_tool(
                session,
                40,
                "source_query",
                {
                    "action": "find_overrides",
                    "params": {
                        "symbol": "UActorComponent::BeginPlay",
                        "cursor": "opaque",
                    },
                },
            )
            assert invalid_override_cursor["result"]["isError"] is True
            assert "numeric cursor" in result_text(invalid_override_cursor)

            cppreflect_string_limit = call_tool(
                session,
                41,
                "cppreflect_query",
                {
                    "action": "list_uproperties",
                    "params": {"limit": "5"},
                },
            )
            assert cppreflect_string_limit["result"]["isError"] is True
            assert "must be an integer" in result_text(
                cppreflect_string_limit
            ), cppreflect_string_limit

            decision_string_age = call_tool(
                session,
                42,
                "decision_query",
                {
                    "action": "list_stale",
                    "params": {"max_age_days": "30"},
                },
            )
            assert decision_string_age["result"]["isError"] is True
            assert "must be an integer" in result_text(decision_string_age)

            risk_string_since = call_tool(
                session,
                43,
                "risk_query",
                {
                    "action": "get_release_window_hotspots",
                    "params": {"since_unix": "1"},
                },
            )
            assert risk_string_since["result"]["isError"] is True
            assert "must be an integer" in result_text(risk_string_since)

            unsupported_parameter = call_tool(
                session,
                44,
                "source_query",
                {
                    "action": "health",
                    "params": {"path_filter": "Core"},
                },
            )
            assert unsupported_parameter["result"]["isError"] is True
            assert "does not support parameter 'path_filter'" in result_text(
                unsupported_parameter
            )

            payload_error = call_tool(
                session,
                14,
                "console_query",
                {
                    "action": "get_object",
                    "params": {"name": ""},
                },
            )
            assert payload_error["result"]["isError"] is True, payload_error
            assert payload_error["result"]["structuredContent"]["error"]

            unsupported = call_tool(
                session,
                5,
                "mesh_query",
                {"action": "list_assets", "params": {}},
            )
            assert unsupported["result"]["isError"] is True, unsupported
            assert "unavailable" in result_text(unsupported)
            assert "offline_fallback" not in result_text(unsupported)

            readonly_guard = call_tool(
                session,
                6,
                "source_query",
                {"action": "repair_fts", "params": {"execute": True}},
            )
            assert readonly_guard["result"]["isError"] is True, readonly_guard
            assert "--readonly cannot be combined" in result_text(readonly_guard)

            comma_array_preserved = call_tool(
                session,
                7,
                "source_query",
                {
                    "action": "detect_changes",
                    "params": {"paths": ["Source/Foo,Bar.cpp"]},
                },
            )
            assert comma_array_preserved["result"]["isError"] is False
            assert comma_array_preserved["result"]["structuredContent"][
                "input"
            ]["changed_paths"] == ["Source/Foo,Bar.cpp"]

            child_stderr_primary = call_tool(
                session,
                76,
                "source_query",
                {"action": "definitely_missing_action", "params": {}},
            )
            assert child_stderr_primary["result"]["isError"] is True
            child_error = child_stderr_primary["result"]["structuredContent"]
            assert "Unknown action: source.definitely_missing_action" in (
                child_error["error"]
            )
            assert child_error["error"] == child_error["stderr"]

            nul_guard = call_tool(
                session,
                9,
                "source_query",
                {"action": "health\u0000ignored", "params": {}},
            )
            assert nul_guard["result"]["isError"] is True, nul_guard
            assert "action strings cannot contain NUL bytes" in result_text(nul_guard)

            for request_id, invalid_action in (
                (43, "--version"),
                (44, "--help"),
                (45, "health/../../version"),
            ):
                action_name_guard = call_tool(
                    session,
                    request_id,
                    "source_query",
                    {"action": invalid_action, "params": {}},
                )
                assert action_name_guard["result"]["isError"] is True
                assert "action names must match [A-Za-z0-9_]+" in result_text(
                    action_name_guard
                )

            readonly_override_guard = call_tool(
                session,
                10,
                "source_query",
                {
                    "action": "repair_fts",
                    "params": {"execute": True, "readonly": False},
                },
            )
            assert (
                readonly_override_guard["result"]["isError"] is True
            ), readonly_override_guard
            assert "proxy-controlled query path or safety option" in result_text(
                readonly_override_guard
            )

            no_log_override_guard = call_tool(
                session,
                11,
                "source_query",
                {"action": "health", "params": {"no_log": True}},
            )
            assert (
                no_log_override_guard["result"]["isError"] is True
            ), no_log_override_guard
            assert "proxy-controlled query path or safety option" in result_text(
                no_log_override_guard
            )
        finally:
            session.close()

        source_log = find_proxy_log_record(log_root, 3)
        assert (
            source_log["routing_context"]["decision_source"]
            == "offline_fallback"
        ), source_log
        assert source_log["routing_context"]["namespace_source"] == "child_query"
        assert source_log["phase_timing"]["offline_fallback_ms"] >= 0
        assert "return" not in source_log, source_log
        assert source_log["redaction"]["result_omitted"] is True

        all_log_text = "\n".join(
            path.read_text(encoding="utf-8", errors="replace")
            for path in log_root.rglob("*.jsonl")
        )
        assert source_body_sentinel not in all_log_text

        unsupported_log = find_proxy_log_record(log_root, 5)
        assert unsupported_log["agent_signal"]["outcome"] == "editor_unavailable"

        opt_out_env = env.copy()
        opt_out_env["MONOLITH_OFFLINE_FALLBACK"] = "0"
        opt_out_env["MONOLITH_TOOL_LOG_DIR"] = str(temp_root / "opt-out-logs")
        opt_out_session = launch_proxy(proxy, opt_out_env)
        try:
            opt_out_initialize = opt_out_session.request(
                100,
                "initialize",
                {
                    "protocolVersion": "2025-11-25",
                    "capabilities": {},
                    "clientInfo": {
                        "name": "offline-opt-out-smoke",
                        "version": "1",
                    },
                },
            )
            assert "offline fallback is disabled" in opt_out_initialize["result"][
                "instructions"
            ].lower()
            opt_out_tools = opt_out_session.request(102, "tools/list", {})
            opt_out_names = {
                tool["name"] for tool in opt_out_tools["result"]["tools"]
            }
            assert opt_out_names == expected_offline_tools
            query_tool = next(
                tool
                for tool in opt_out_tools["result"]["tools"]
                if tool["name"] == "monolith_query"
            )
            assert "bundled offline" not in query_tool["description"]
            opted_out = call_tool(
                opt_out_session,
                101,
                "source_query",
                {"action": "health", "params": {}},
            )
            assert opted_out["result"]["isError"] is True, opted_out
            assert "unavailable" in result_text(opted_out)
            assert "offline_fallback" not in result_text(opted_out)
        finally:
            opt_out_session.close()

        empty_output_env = env.copy()
        empty_output_env["MONOLITH_QUERY_EXE"] = str(proxy)
        empty_output_env["MONOLITH_TOOL_LOG_DIR"] = str(
            temp_root / "empty-output-logs"
        )
        empty_output_session = launch_proxy(proxy, empty_output_env)
        try:
            empty_output_session.request(
                200,
                "initialize",
                {
                    "protocolVersion": "2025-11-25",
                    "capabilities": {},
                    "clientInfo": {
                        "name": "offline-empty-output-smoke",
                        "version": "1",
                    },
                },
            )
            empty_stdout = call_tool(
                empty_output_session,
                202,
                "source_query",
                {"action": "health", "params": {}},
            )
            assert empty_stdout["result"]["isError"] is True, empty_stdout
            assert "produced empty stdout" in result_text(empty_stdout)
        finally:
            empty_output_session.close()

        remote_http_env = env.copy()
        remote_http_env["MONOLITH_URL"] = "http://example.com:9316/mcp"
        remote_http = subprocess.run(
            [str(proxy)],
            input="",
            capture_output=True,
            text=True,
            encoding="utf-8",
            errors="replace",
            env=remote_http_env,
            timeout=5,
            check=False,
        )
        assert remote_http.returncode == 2, remote_http
        assert "Plain HTTP MONOLITH_URL is restricted to loopback" in (
            remote_http.stderr
        )

        incomplete_health_server = ThreadingHTTPServer(
            ("127.0.0.1", 0), IncompleteHealthHandler
        )
        incomplete_health_thread = threading.Thread(
            target=incomplete_health_server.serve_forever, daemon=True
        )
        incomplete_health_thread.start()
        incomplete_health_env = env.copy()
        incomplete_health_env["MONOLITH_URL"] = (
            f"http://127.0.0.1:{incomplete_health_server.server_port}/mcp"
        )
        incomplete_health_env["MONOLITH_TOOL_LOG_DIR"] = str(
            temp_root / "incomplete-health-logs"
        )
        incomplete_health_session = launch_proxy(proxy, incomplete_health_env)
        try:
            incomplete_health_session.request(
                250,
                "initialize",
                {
                    "protocolVersion": "2025-11-25",
                    "capabilities": {},
                    "clientInfo": {
                        "name": "incomplete-health-smoke",
                        "version": "1",
                    },
                },
            )
            incomplete_health_result = call_tool(
                incomplete_health_session,
                251,
                "monolith_query",
                {
                    "namespace": "asset",
                    "action": "save_asset",
                    "params": {"asset_path": "/Game/Probe"},
                },
            )
            assert incomplete_health_result["result"]["isError"] is True
            assert "LIVE_SENTINEL" not in result_text(incomplete_health_result)
            assert "does not serve namespace 'asset'" in result_text(
                incomplete_health_result
            )
        finally:
            incomplete_health_session.close()
            incomplete_health_server.shutdown()
            incomplete_health_server.server_close()
            incomplete_health_thread.join(timeout=2)

        trickle_health_server = ThreadingHTTPServer(
            ("127.0.0.1", 0), TrickleHealthHandler
        )
        trickle_health_thread = threading.Thread(
            target=trickle_health_server.serve_forever, daemon=True
        )
        trickle_health_thread.start()
        trickle_health_env = env.copy()
        trickle_health_env["MONOLITH_URL"] = (
            f"http://127.0.0.1:{trickle_health_server.server_port}/mcp"
        )
        trickle_health_env["MONOLITH_TOOL_LOG_DIR"] = str(
            temp_root / "trickle-health-logs"
        )
        trickle_health_session = launch_proxy(proxy, trickle_health_env)
        try:
            trickle_health_session.request(
                255,
                "initialize",
                {
                    "protocolVersion": "2025-11-25",
                    "capabilities": {},
                    "clientInfo": {
                        "name": "trickle-health-deadline-smoke",
                        "version": "1",
                    },
                },
            )
            trickle_health_start = time.monotonic()
            trickle_health_result = call_tool(
                trickle_health_session,
                256,
                "monolith_query",
                {
                    "namespace": "asset",
                    "action": "save_asset",
                    "params": {"asset_path": "/Game/Probe"},
                },
            )
            trickle_health_seconds = time.monotonic() - trickle_health_start
            assert 4.5 <= trickle_health_seconds < 6.5, trickle_health_seconds
            assert trickle_health_result["result"]["isError"] is True
            assert "LIVE_SENTINEL" not in result_text(trickle_health_result)
        finally:
            trickle_health_session.close()
            trickle_health_server.shutdown()
            trickle_health_server.server_close()
            trickle_health_thread.join(timeout=2)

        spoofed_pid_server = ThreadingHTTPServer(
            ("127.0.0.1", 0), SpoofedHealthPidHandler
        )
        spoofed_pid_thread = threading.Thread(
            target=spoofed_pid_server.serve_forever, daemon=True
        )
        spoofed_pid_thread.start()
        spoofed_pid_env = env.copy()
        spoofed_pid_env["MONOLITH_URL"] = (
            f"http://127.0.0.1:{spoofed_pid_server.server_port}/mcp"
        )
        spoofed_pid_env["MONOLITH_TOOL_LOG_DIR"] = str(
            temp_root / "spoofed-pid-logs"
        )
        spoofed_pid_session = launch_proxy(proxy, spoofed_pid_env)
        try:
            spoofed_pid_session.request(
                260,
                "initialize",
                {
                    "protocolVersion": "2025-11-25",
                    "capabilities": {},
                    "clientInfo": {
                        "name": "spoofed-health-pid-smoke",
                        "version": "1",
                    },
                },
            )
            spoofed_pid_result = call_tool(
                spoofed_pid_session,
                261,
                "monolith_query",
                {
                    "namespace": "asset",
                    "action": "save_asset",
                    "params": {"asset_path": "/Game/Probe"},
                },
            )
            assert spoofed_pid_result["result"]["isError"] is True
            assert "LIVE_SENTINEL" not in result_text(spoofed_pid_result)
            assert "does not serve namespace 'asset'" in result_text(
                spoofed_pid_result
            )
        finally:
            spoofed_pid_session.close()
            spoofed_pid_server.shutdown()
            spoofed_pid_server.server_close()
            spoofed_pid_thread.join(timeout=2)

        invalid_editor_identity_cases = (
            (MismatchedStatusPidHandler, "status-pid-mismatch", 265),
            (CommandletIdentityHandler, "status-commandlet", 267),
        )
        for identity_handler, identity_label, request_base in (
            invalid_editor_identity_cases
        ):
            invalid_editor_identity_server = ThreadingHTTPServer(
                ("127.0.0.1", 0), identity_handler
            )
            invalid_editor_identity_thread = threading.Thread(
                target=invalid_editor_identity_server.serve_forever, daemon=True
            )
            invalid_editor_identity_thread.start()
            invalid_editor_identity_env = env.copy()
            invalid_editor_identity_env["MONOLITH_URL"] = (
                f"http://127.0.0.1:{invalid_editor_identity_server.server_port}/mcp"
            )
            invalid_editor_identity_env["MONOLITH_TOOL_LOG_DIR"] = str(
                temp_root / f"{identity_label}-logs"
            )
            invalid_editor_identity_session = launch_proxy(
                proxy, invalid_editor_identity_env
            )
            try:
                invalid_editor_identity_session.request(
                    request_base,
                    "initialize",
                    {
                        "protocolVersion": "2025-11-25",
                        "capabilities": {},
                        "clientInfo": {
                            "name": f"{identity_label}-smoke",
                            "version": "1",
                        },
                    },
                )
                invalid_editor_identity_result = call_tool(
                    invalid_editor_identity_session,
                    request_base + 1,
                    "monolith_query",
                    {
                        "namespace": "asset",
                        "action": "save_asset",
                        "params": {"asset_path": "/Game/Probe"},
                    },
                )
                assert invalid_editor_identity_result["result"]["isError"] is True
                assert "LIVE_SENTINEL" not in result_text(
                    invalid_editor_identity_result
                )
            finally:
                invalid_editor_identity_session.close()
                invalid_editor_identity_server.shutdown()
                invalid_editor_identity_server.server_close()
                invalid_editor_identity_thread.join(timeout=2)

        multi_owner_server = ThreadingHTTPServer(
            ("127.0.0.1", 0), LiveErrorHandler
        )
        multi_owner_thread = threading.Thread(
            target=multi_owner_server.serve_forever, daemon=True
        )
        multi_owner_thread.start()
        extra_listener_script = r"""
import socket
import sys

listener = socket.socket(socket.AF_INET6, socket.SOCK_STREAM)
listener.setsockopt(socket.IPPROTO_IPV6, socket.IPV6_V6ONLY, 1)
listener.bind(("::1", int(sys.argv[1])))
listener.listen(1)
print("READY", flush=True)
sys.stdin.buffer.read()
"""
        extra_listener = subprocess.Popen(
            [
                sys.executable,
                "-c",
                extra_listener_script,
                str(multi_owner_server.server_port),
            ],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            encoding="utf-8",
            errors="replace",
        )
        assert extra_listener.stdout is not None
        ready_line = extra_listener.stdout.readline().strip()
        assert ready_line == "READY", (
            ready_line,
            extra_listener.stderr.read() if extra_listener.stderr else "",
        )
        multi_owner_env = env.copy()
        multi_owner_env["MONOLITH_URL"] = (
            f"http://127.0.0.1:{multi_owner_server.server_port}/mcp"
        )
        multi_owner_env["MONOLITH_TOOL_LOG_DIR"] = str(
            temp_root / "multi-owner-logs"
        )
        multi_owner_session = launch_proxy(proxy, multi_owner_env)
        try:
            multi_owner_session.request(
                270,
                "initialize",
                {
                    "protocolVersion": "2025-11-25",
                    "capabilities": {},
                    "clientInfo": {
                        "name": "multi-owner-listener-smoke",
                        "version": "1",
                    },
                },
            )
            multi_owner_result = call_tool(
                multi_owner_session,
                271,
                "monolith_query",
                {
                    "namespace": "asset",
                    "action": "save_asset",
                    "params": {"asset_path": "/Game/Probe"},
                },
            )
            assert multi_owner_result["result"]["isError"] is True
            assert "LIVE_SENTINEL" not in result_text(multi_owner_result)
            assert "does not serve namespace 'asset'" in result_text(
                multi_owner_result
            )
        finally:
            multi_owner_session.close()
            if extra_listener.stdin is not None:
                extra_listener.stdin.close()
            try:
                extra_listener.wait(timeout=5)
            except subprocess.TimeoutExpired:
                extra_listener.terminate()
                extra_listener.wait(timeout=5)
            multi_owner_server.shutdown()
            multi_owner_server.server_close()
            multi_owner_thread.join(timeout=2)

        ipv6_server = IPv6ThreadingHTTPServer(("::1", 0), LiveErrorHandler)
        ipv6_thread = threading.Thread(
            target=ipv6_server.serve_forever, daemon=True
        )
        ipv6_thread.start()
        ipv6_env = env.copy()
        ipv6_env["MONOLITH_URL"] = (
            f"http://[::1]:{ipv6_server.server_port}/mcp"
        )
        ipv6_env["MONOLITH_TOOL_LOG_DIR"] = str(
            temp_root / "ipv6-live-logs"
        )
        ipv6_session = launch_proxy(proxy, ipv6_env)
        try:
            ipv6_session.request(
                270,
                "initialize",
                {
                    "protocolVersion": "2025-11-25",
                    "capabilities": {},
                    "clientInfo": {
                        "name": "ipv6-live-smoke",
                        "version": "1",
                    },
                },
            )
            ipv6_live = call_tool(
                ipv6_session,
                271,
                "monolith_query",
                {
                    "namespace": "asset",
                    "action": "save_asset",
                    "params": {"asset_path": "/Game/Probe"},
                },
            )
            assert result_text(ipv6_live) == "LIVE_SENTINEL"
            assert "offline_fallback" not in json.dumps(ipv6_live)
        finally:
            ipv6_session.close()
            ipv6_server.shutdown()
            ipv6_server.server_close()
            ipv6_thread.join(timeout=2)

        DelayedHealthHandler.health_calls = 0
        delayed_health_server = ThreadingHTTPServer(
            ("127.0.0.1", 0), DelayedHealthHandler
        )
        delayed_health_thread = threading.Thread(
            target=delayed_health_server.serve_forever, daemon=True
        )
        delayed_health_thread.start()
        delayed_health_env = env.copy()
        delayed_health_env["MONOLITH_URL"] = (
            f"http://127.0.0.1:{delayed_health_server.server_port}/mcp"
        )
        delayed_health_env["MONOLITH_TOOL_LOG_DIR"] = str(
            temp_root / "delayed-health-logs"
        )
        delayed_health_session = launch_proxy(proxy, delayed_health_env)
        try:
            delayed_health_session.request(
                275,
                "initialize",
                {
                    "protocolVersion": "2025-11-25",
                    "capabilities": {},
                    "clientInfo": {
                        "name": "delayed-health-live-smoke",
                        "version": "1",
                    },
                },
            )
            delayed_health_start = time.monotonic()
            delayed_health_live = call_tool(
                delayed_health_session,
                276,
                "monolith_query",
                {
                    "namespace": "asset",
                    "action": "save_asset",
                    "params": {"asset_path": "/Game/Probe"},
                },
            )
            delayed_health_seconds = time.monotonic() - delayed_health_start
            assert DelayedHealthHandler.health_calls >= 1
            assert delayed_health_seconds >= 0.5, delayed_health_seconds
            assert result_text(delayed_health_live) == "LIVE_SENTINEL"
            assert "offline_fallback" not in json.dumps(delayed_health_live)
        finally:
            delayed_health_session.close()
            delayed_health_server.shutdown()
            delayed_health_server.server_close()
            delayed_health_thread.join(timeout=2)

        DelayedIdentityHandler.status_calls = 0
        delayed_identity_server = ThreadingHTTPServer(
            ("127.0.0.1", 0), DelayedIdentityHandler
        )
        delayed_identity_thread = threading.Thread(
            target=delayed_identity_server.serve_forever, daemon=True
        )
        delayed_identity_thread.start()
        delayed_identity_env = env.copy()
        delayed_identity_env["MONOLITH_URL"] = (
            f"http://127.0.0.1:{delayed_identity_server.server_port}/mcp"
        )
        delayed_identity_env["MONOLITH_TOOL_LOG_DIR"] = str(
            temp_root / "delayed-identity-logs"
        )
        delayed_identity_session = launch_proxy(proxy, delayed_identity_env)
        try:
            delayed_identity_session.request(
                280,
                "initialize",
                {
                    "protocolVersion": "2025-11-25",
                    "capabilities": {},
                    "clientInfo": {
                        "name": "delayed-identity-live-smoke",
                        "version": "1",
                    },
                },
            )
            delayed_identity_start = time.monotonic()
            delayed_identity_live = call_tool(
                delayed_identity_session,
                281,
                "monolith_query",
                {
                    "namespace": "asset",
                    "action": "save_asset",
                    "params": {"asset_path": "/Game/Probe"},
                },
            )
            delayed_identity_seconds = (
                time.monotonic() - delayed_identity_start
            )
            assert DelayedIdentityHandler.status_calls >= 1
            assert delayed_identity_seconds >= 0.5, delayed_identity_seconds
            assert result_text(delayed_identity_live) == "LIVE_SENTINEL"
            assert "offline_fallback" not in json.dumps(delayed_identity_live)
        finally:
            delayed_identity_session.close()
            delayed_identity_server.shutdown()
            delayed_identity_server.server_close()
            delayed_identity_thread.join(timeout=2)

        live_server = ThreadingHTTPServer(
            ("127.0.0.1", 0), LiveErrorHandler
        )
        live_thread = threading.Thread(
            target=live_server.serve_forever, daemon=True
        )
        live_thread.start()
        live_env = env.copy()
        live_env["MONOLITH_URL"] = (
            f"http://127.0.0.1:{live_server.server_port}/mcp"
        )
        live_env["MONOLITH_QUERY_EXE"] = str(
            temp_root / "must-not-launch" / "monolith_query.exe"
        )
        live_env["MONOLITH_TOOL_LOG_DIR"] = str(temp_root / "live-logs")
        live_session = launch_proxy(proxy, live_env)
        try:
            live_session.request(
                300,
                "initialize",
                {
                    "protocolVersion": "2025-11-25",
                    "capabilities": {},
                    "clientInfo": {
                        "name": "live-priority-smoke",
                        "version": "1",
                    },
                },
            )
            cold_live_only = call_tool(
                live_session,
                302,
                "monolith_query",
                {
                    "namespace": "asset",
                    "action": "save_asset",
                    "params": {"asset_path": "/Game/Probe"},
                },
            )
            assert result_text(cold_live_only) == "LIVE_SENTINEL"
            assert "offline_fallback" not in json.dumps(cold_live_only)
            live_error = call_tool(
                live_session,
                301,
                "source_query",
                {"action": "health", "params": {}},
            )
            assert live_error["result"]["isError"] is True, live_error
            assert result_text(live_error) == "LIVE_SENTINEL", live_error
            assert "structuredContent" not in live_error["result"], live_error
            assert "offline_fallback" not in json.dumps(live_error), live_error
            invalid_live_list = live_session.request(303, "tools/list", {})
            invalid_live_names = {
                tool["name"]
                for tool in invalid_live_list["result"]["tools"]
            }
            assert invalid_live_names == expected_offline_tools
            post_invalid_list_live = call_tool(
                live_session,
                304,
                "source_query",
                {"action": "health", "params": {"include_counts": True}},
            )
            assert result_text(post_invalid_list_live) == "LIVE_SENTINEL"
            assert "offline_fallback" not in json.dumps(post_invalid_list_live)
        finally:
            live_session.close()
            live_server.shutdown()
            live_server.server_close()
            live_thread.join(timeout=2)

        invalid_live_server = ThreadingHTTPServer(
            ("127.0.0.1", 0), InvalidLiveHandler
        )
        invalid_live_thread = threading.Thread(
            target=invalid_live_server.serve_forever, daemon=True
        )
        invalid_live_thread.start()
        invalid_live_env = env.copy()
        invalid_live_env["MONOLITH_URL"] = (
            f"http://127.0.0.1:{invalid_live_server.server_port}/mcp"
        )
        invalid_live_env["MONOLITH_TOOL_LOG_DIR"] = str(
            temp_root / "invalid-live-logs"
        )
        invalid_live_session = launch_proxy(proxy, invalid_live_env)
        try:
            invalid_live_session.request(
                400,
                "initialize",
                {
                    "protocolVersion": "2025-11-25",
                    "capabilities": {},
                    "clientInfo": {
                        "name": "invalid-live-fallback-smoke",
                        "version": "1",
                    },
                },
            )
            wait_for_list_changed(invalid_live_session)
            invalid_live_fallback = call_tool(
                invalid_live_session,
                401,
                "source_query",
                {"action": "health", "params": {}},
            )
            assert invalid_live_fallback["result"]["isError"] is False
            assert (
                invalid_live_fallback["result"]["structuredContent"][
                    "_monolith"
                ]["offline_fallback"]
                is True
            )
            circuit_start = time.monotonic()
            immediate_second_fallback = call_tool(
                invalid_live_session,
                402,
                "source_query",
                {"action": "health", "params": {"include_counts": False}},
            )
            circuit_seconds = time.monotonic() - circuit_start
            assert immediate_second_fallback["result"]["isError"] is False
            assert circuit_seconds < 1.0, circuit_seconds
            time.sleep(6.0)
            post_poll_start = time.monotonic()
            post_poll_fallback = call_tool(
                invalid_live_session,
                403,
                "source_query",
                {"action": "health", "params": {}},
            )
            post_poll_seconds = time.monotonic() - post_poll_start
            assert post_poll_fallback["result"]["isError"] is False
            assert post_poll_seconds < 1.0, post_poll_seconds
        finally:
            invalid_live_session.close()
            invalid_live_server.shutdown()
            invalid_live_server.server_close()
            invalid_live_thread.join(timeout=2)

        foreign_server = ThreadingHTTPServer(
            ("127.0.0.1", 0), ForeignProjectHandler
        )
        foreign_thread = threading.Thread(
            target=foreign_server.serve_forever, daemon=True
        )
        foreign_thread.start()
        foreign_env = env.copy()
        foreign_env["MONOLITH_URL"] = (
            f"http://127.0.0.1:{foreign_server.server_port}/mcp"
        )
        foreign_env["MONOLITH_TOOL_LOG_DIR"] = str(
            temp_root / "foreign-project-logs"
        )
        foreign_session = launch_proxy(proxy, foreign_env)
        try:
            foreign_session.request(
                450,
                "initialize",
                {
                    "protocolVersion": "2025-11-25",
                    "capabilities": {},
                    "clientInfo": {
                        "name": "foreign-project-identity-smoke",
                        "version": "1",
                    },
                },
            )
            # Give the background health probe time to reject the valid-looking
            # endpoint identity; no list_changed should be required.
            time.sleep(3.5)
            foreign_fallback = call_tool(
                foreign_session,
                451,
                "source_query",
                {"action": "health", "params": {"include_counts": False}},
            )
            assert foreign_fallback["result"]["isError"] is False
            assert "FOREIGN_SENTINEL" not in result_text(foreign_fallback)
            assert foreign_fallback["result"]["structuredContent"][
                "_monolith"
            ]["offline_fallback"] is True
        finally:
            foreign_session.close()
            foreign_server.shutdown()
            foreign_server.server_close()
            foreign_thread.join(timeout=2)

        SwitchingProjectHandler.foreign = False
        switching_server = ThreadingHTTPServer(
            ("127.0.0.1", 0), SwitchingProjectHandler
        )
        switching_thread = threading.Thread(
            target=switching_server.serve_forever, daemon=True
        )
        switching_thread.start()
        switching_env = env.copy()
        switching_env["MONOLITH_URL"] = (
            f"http://127.0.0.1:{switching_server.server_port}/mcp"
        )
        switching_env["MONOLITH_TOOL_LOG_DIR"] = str(
            temp_root / "switching-project-logs"
        )
        switching_session = launch_proxy(proxy, switching_env)
        try:
            switching_session.request(
                460,
                "initialize",
                {
                    "protocolVersion": "2025-11-25",
                    "capabilities": {},
                    "clientInfo": {
                        "name": "project-swap-identity-smoke",
                        "version": "1",
                    },
                },
            )
            initial_live = call_tool(
                switching_session,
                461,
                "monolith_query",
                {
                    "namespace": "asset",
                    "action": "save_asset",
                    "params": {"asset_path": "/Game/Probe"},
                },
            )
            assert result_text(initial_live) == "FOREIGN_AFTER_SWAP"
            SwitchingProjectHandler.foreign = True
            swapped = call_tool(
                switching_session,
                462,
                "monolith_query",
                {
                    "namespace": "asset",
                    "action": "save_asset",
                    "params": {"asset_path": "/Game/Probe2"},
                },
            )
            assert "FOREIGN_AFTER_SWAP" not in result_text(swapped)
            assert swapped["result"]["isError"] is True
            assert "does not serve namespace 'asset'" in result_text(swapped)
        finally:
            switching_session.close()
            switching_server.shutdown()
            switching_server.server_close()
            switching_thread.join(timeout=2)

        survival_env = env.copy()
        survival_env["MONOLITH_TOOL_LOG_DIR"] = str(
            temp_root / "request-survival-logs"
        )
        survival_session = launch_proxy(proxy, survival_env)
        try:
            assert survival_session.process.stdin is not None
            survival_session.process.stdin.write(
                json.dumps(
                    {
                        "jsonrpc": "2.0",
                        "id": 500,
                        "method": "tools/call",
                        "params": [],
                    },
                    separators=(",", ":"),
                )
                + "\n"
            )
            survival_session.process.stdin.flush()
            invalid_request = survival_session.responses.get(timeout=5)
            assert invalid_request["id"] == 500
            assert invalid_request["error"]["code"] == -32602
            surviving_ping = survival_session.request(501, "ping", {})
            assert surviving_ping["result"] == {}
            assert survival_session.process.poll() is None
        finally:
            survival_session.close()

        hanging_server = ThreadingHTTPServer(
            ("127.0.0.1", 0), HangingLiveHandler
        )
        hanging_thread = threading.Thread(
            target=hanging_server.serve_forever, daemon=True
        )
        hanging_thread.start()
        hanging_env = env.copy()
        hanging_env["MONOLITH_URL"] = (
            f"http://127.0.0.1:{hanging_server.server_port}/mcp"
        )
        hanging_env["MONOLITH_TOOL_LOG_DIR"] = str(
            temp_root / "hanging-live-logs"
        )
        hanging_session = launch_proxy(proxy, hanging_env)
        try:
            hanging_session.request(
                600,
                "initialize",
                {
                    "protocolVersion": "2025-11-25",
                    "capabilities": {},
                    "clientInfo": {
                        "name": "cold-list-latency-smoke",
                        "version": "1",
                    },
                },
            )
            cold_list_start = time.monotonic()
            cold_list = hanging_session.request(601, "tools/list", {})
            cold_list_seconds = time.monotonic() - cold_list_start
            assert cold_list_seconds < 1.5, cold_list_seconds
            cold_names = {
                tool["name"] for tool in cold_list["result"]["tools"]
            }
            assert cold_names == expected_offline_tools
            unknown_start = time.monotonic()
            unknown_method = hanging_session.request(
                602, "resources/list", {}
            )
            unknown_seconds = time.monotonic() - unknown_start
            assert unknown_method["result"] == {"resources": []}
            assert unknown_seconds < 0.5, unknown_seconds
        finally:
            hanging_session.close()
            hanging_server.shutdown()
            hanging_server.server_close()
            hanging_thread.join(timeout=2)

        trickle_server = ThreadingHTTPServer(
            ("127.0.0.1", 0), TrickleToolsListHandler
        )
        trickle_thread = threading.Thread(
            target=trickle_server.serve_forever, daemon=True
        )
        trickle_thread.start()
        trickle_env = env.copy()
        trickle_env["MONOLITH_URL"] = (
            f"http://127.0.0.1:{trickle_server.server_port}/mcp"
        )
        trickle_env["MONOLITH_TOOL_LOG_DIR"] = str(
            temp_root / "trickle-live-logs"
        )
        trickle_session = launch_proxy(proxy, trickle_env)
        try:
            trickle_session.request(
                610,
                "initialize",
                {
                    "protocolVersion": "2025-11-25",
                    "capabilities": {},
                    "clientInfo": {
                        "name": "trickle-list-deadline-smoke",
                        "version": "1",
                    },
                },
            )
            wait_for_list_changed(trickle_session)
            trickle_start = time.monotonic()
            trickle_list = trickle_session.request(611, "tools/list", {})
            trickle_seconds = time.monotonic() - trickle_start
            assert trickle_seconds < 1.5, trickle_seconds
            assert {
                tool["name"] for tool in trickle_list["result"]["tools"]
            } == expected_offline_tools
            post_trickle_live = call_tool(
                trickle_session,
                612,
                "source_query",
                {"action": "health", "params": {"include_counts": True}},
            )
            assert result_text(post_trickle_live) == "LIVE_SENTINEL"
            assert "offline_fallback" not in json.dumps(post_trickle_live)
        finally:
            trickle_session.close()
            trickle_server.shutdown()
            trickle_server.server_close()
            trickle_thread.join(timeout=2)

        oversized_server = ThreadingHTTPServer(
            ("127.0.0.1", 0), OversizedLiveHandler
        )
        oversized_thread = threading.Thread(
            target=oversized_server.serve_forever, daemon=True
        )
        oversized_thread.start()
        oversized_env = env.copy()
        oversized_env["MONOLITH_URL"] = (
            f"http://127.0.0.1:{oversized_server.server_port}/mcp"
        )
        oversized_env["MONOLITH_TOOL_LOG_DIR"] = str(
            temp_root / "oversized-live-logs"
        )
        oversized_session = launch_proxy(proxy, oversized_env)
        try:
            oversized_session.request(
                620,
                "initialize",
                {
                    "protocolVersion": "2025-11-25",
                    "capabilities": {},
                    "clientInfo": {
                        "name": "oversized-live-response-smoke",
                        "version": "1",
                    },
                },
            )
            wait_for_list_changed(oversized_session)
            oversized_start = time.monotonic()
            oversized_result = call_tool(
                oversized_session,
                621,
                "source_query",
                {"action": "health", "params": {"include_counts": True}},
            )
            oversized_seconds = time.monotonic() - oversized_start
            assert oversized_seconds < 1.5, oversized_seconds
            assert oversized_result["result"]["isError"] is False
            assert (
                oversized_result["result"]["structuredContent"]["_monolith"]
                ["offline_fallback"]
                is True
            )
        finally:
            oversized_session.close()
            oversized_server.shutdown()
            oversized_server.server_close()
            oversized_thread.join(timeout=2)

        SlowMutationHandler.mutation_calls = 0
        slow_mutation_server = ThreadingHTTPServer(
            ("127.0.0.1", 0), SlowMutationHandler
        )
        slow_mutation_thread = threading.Thread(
            target=slow_mutation_server.serve_forever, daemon=True
        )
        slow_mutation_thread.start()
        slow_mutation_env = env.copy()
        slow_mutation_env["MONOLITH_URL"] = (
            f"http://127.0.0.1:{slow_mutation_server.server_port}/mcp"
        )
        slow_mutation_env["MONOLITH_TOOL_LOG_DIR"] = str(
            temp_root / "slow-mutation-logs"
        )
        slow_mutation_env["MONOLITH_CATALOG_SNAPSHOT"] = str(
            temp_root / "missing-catalog.json"
        )
        slow_mutation_session = launch_proxy(proxy, slow_mutation_env)
        mutation_arguments = {
            "namespace": "asset",
            "action": "save_asset",
            "params": {"asset_path": "/Game/Probe"},
        }
        try:
            slow_mutation_session.request(
                630,
                "initialize",
                {
                    "protocolVersion": "2025-11-25",
                    "capabilities": {},
                    "clientInfo": {
                        "name": "completed-time-dedup-smoke",
                        "version": "1",
                    },
                },
            )
            wait_for_list_changed(slow_mutation_session)
            first_mutation = call_tool(
                slow_mutation_session, 631, "monolith_query", mutation_arguments
            )
            assert result_text(first_mutation) == "MUTATION_DONE"
            repeated_mutation = call_tool(
                slow_mutation_session, 632, "monolith_query", mutation_arguments
            )
            assert repeated_mutation["result"]["isError"] is True
            assert "same arguments was just called" in result_text(
                repeated_mutation
            )
            assert SlowMutationHandler.mutation_calls == 1
        finally:
            slow_mutation_session.close()
            slow_mutation_server.shutdown()
            slow_mutation_server.server_close()
            slow_mutation_thread.join(timeout=2)

        SlowSchemaDiscoveryHandler.schema_calls = 0
        slow_schema_server = ThreadingHTTPServer(
            ("127.0.0.1", 0), SlowSchemaDiscoveryHandler
        )
        slow_schema_thread = threading.Thread(
            target=slow_schema_server.serve_forever, daemon=True
        )
        slow_schema_thread.start()
        slow_schema_env = env.copy()
        slow_schema_env["MONOLITH_URL"] = (
            f"http://127.0.0.1:{slow_schema_server.server_port}/mcp"
        )
        slow_schema_env["MONOLITH_TOOL_LOG_DIR"] = str(
            temp_root / "slow-schema-logs"
        )
        slow_schema_session = launch_proxy(proxy, slow_schema_env)
        try:
            slow_schema_session.request(
                640,
                "initialize",
                {
                    "protocolVersion": "2025-11-25",
                    "capabilities": {},
                    "clientInfo": {
                        "name": "authoritative-schema-budget-smoke",
                        "version": "1",
                    },
                },
            )
            wait_for_list_changed(slow_schema_session)
            schema_start = time.monotonic()
            slow_schema_result = call_tool(
                slow_schema_session,
                641,
                "monolith_discover",
                {
                    "namespace": "editor",
                    "action": "run_automation_tests",
                    "mode": "schema",
                    "schema_detail": "full",
                },
            )
            schema_seconds = time.monotonic() - schema_start
            assert result_text(slow_schema_result) == "LIVE_SCHEMA_SENTINEL"
            assert "offline_fallback" not in json.dumps(slow_schema_result)
            assert 3.5 <= schema_seconds < 10.0, schema_seconds
            assert SlowSchemaDiscoveryHandler.schema_calls == 1
        finally:
            slow_schema_session.close()
            slow_schema_server.shutdown()
            slow_schema_server.server_close()
            slow_schema_thread.join(timeout=2)

        print(
            json.dumps(
                {
                    "status": "passed",
                    "proxy": str(proxy),
                    "offline_tools": len(expected_offline_tools),
                    "checks": [
                        "initialize",
                        "reproducible_build_contract",
                        "binary_source_hash_freshness",
                        "direct_fixed_query_matches_current_generation",
                        "direct_fixed_query_invalid_manifest_fails_closed",
                        "immutable_query_catalog_bundle_route",
                        "running_proxy_keeps_pinned_query_catalog_generation",
                        "immutable_query_catalog_sha_tamper_fails_closed",
                        "immutable_query_executable_sha_tamper_fails_closed",
                        "immutable_query_selected_file_missing_fails_closed",
                        "immutable_query_catalog_leaf_tamper_fails_closed",
                        "invalid_bundle_does_not_intercept_healthy_live_route",
                        "cold_tools_list",
                        "stale_cache_ignored_for_four_tool_control_plane",
                        "cold_tools_list_does_not_wait_for_hanging_editor",
                        "tools_list_total_deadline_does_not_open_action_circuit",
                        "oversized_live_response_fails_closed",
                        "risky_repeat_window_starts_at_completion",
                        "source_health",
                        "offline_response_shaping_query_and_meta",
                        "offline_response_shaping_type_validation",
                        "offline_response_shaping_unknown_field",
                        "offline_response_shaping_text_parity",
                        "indexed_source_read_survives",
                        "attacker_database_override_rejected",
                        "unindexed_system_file_rejected",
                        "tool_results_omitted_from_daily_logs",
                        "caller_database_path_override_rejected",
                        "project_health",
                        "source_plain_text",
                        "offline_query_top_level_param_merge",
                        "offline_query_nested_param_precedence",
                        "offline_query_string_params",
                        "offline_meta_nested_and_string_params",
                        "offline_malformed_params_errors",
                        "offline_catalog_dispatch_overlay",
                        "offline_discovery_mode_truthfulness",
                        "source_symbol_kind_alias",
                        "source_regex_mode_explicit_error",
                        "source_path_filter_and_exact_mode",
                        "source_unsupported_example_cursor_error",
                        "source_named_reference",
                        "source_string_array_csv",
                        "project_asset_type_alias",
                        "project_live_shape_parity",
                        "source_complex_range_json",
                        "project_error_exit_zero",
                        "source_review_error_envelope",
                        "check_result_status_is_not_transport_error",
                        "mcp_parameter_type_validation",
                        "mcp_action_specific_type_alias_range_validation",
                        "payload_error_object",
                        "monolith_status",
                        "monolith_status_headless_command",
                        "monolith_guide_text",
                        "unsupported_editor_error",
                        "readonly_execute_guard",
                        "comma_string_array_preservation",
                        "child_stderr_primary_error",
                        "nul_action_guard",
                        "strict_action_name_guard",
                        "reserved_readonly_guard",
                        "reserved_no_log_guard",
                        "offline_fallback_opt_out",
                        "empty_stdout_guard",
                        "remote_plain_http_rejected",
                        "incomplete_live_health_contract_rejected",
                        "health_total_deadline_rejects_trickle",
                        "spoofed_health_pid_rejected",
                        "status_editor_pid_mismatch_rejected",
                        "status_commandlet_identity_rejected",
                        "multi_owner_listener_pid_rejected",
                        "ipv6_loopback_listener_owner_live_route",
                        "delayed_health_routes_live",
                        "delayed_status_identity_routes_live",
                        "cold_live_only_generic_routes_live",
                        "slow_schema_discovery_preserves_live_authority",
                        "live_error_wins_without_fallback",
                        "invalid_live_tools_list_uses_stable_control_plane",
                        "invalid_live_tools_list_does_not_open_action_circuit",
                        "invalid_live_jsonrpc_falls_back_readonly",
                        "first_transport_failure_opens_circuit",
                        "health_poll_respects_transport_backoff",
                        "foreign_project_endpoint_rejected",
                        "accepted_endpoint_project_swap_rejected",
                        "optional_mcp_surface_is_local_and_bounded",
                        "malformed_request_does_not_kill_proxy",
                        "proxy_routing_log",
                    ],
                },
                indent=2,
            )
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
