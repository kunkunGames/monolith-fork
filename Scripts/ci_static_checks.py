#!/usr/bin/env python3
"""Config-driven hosted static CI checks for the Monolith plugin.

The checker intentionally stays stdlib-only. Repository-specific policy lives in
`.github/monolith-static-ci.json`; this file is the reusable static-check engine.
"""

from __future__ import annotations

import argparse
import fnmatch
import json
import os
import queue
import re
import subprocess
import sys
import tempfile
import threading
from dataclasses import dataclass
from pathlib import Path
from typing import Any


@dataclass(frozen=True)
class Finding:
    severity: str
    check: str
    message: str
    path: str = ""


class CheckContext:
    def __init__(self, root: Path, config: dict[str, Any], config_path: Path) -> None:
        self.root = root.resolve()
        self.config = config
        self.config_path = config_path
        self.findings: list[Finding] = []
        self._tracked_files: list[Path] | None = None

    def rel(self, path: Path) -> str:
        try:
            return path.resolve().relative_to(self.root).as_posix()
        except ValueError:
            return path.as_posix()

    def path(self, value: str) -> Path:
        return (self.root / value).resolve()

    def add(self, severity: str, check: str, message: str, path: Path | str | None = None) -> None:
        rel_path = ""
        if isinstance(path, Path):
            rel_path = self.rel(path)
        elif isinstance(path, str):
            rel_path = path
        self.findings.append(Finding(severity, check, message, rel_path))

    def block(self, check: str, message: str, path: Path | str | None = None) -> None:
        self.add("blocker", check, message, path)

    def advisory(self, check: str, message: str, path: Path | str | None = None) -> None:
        self.add("advisory", check, message, path)

    def tracked_files(self) -> list[Path]:
        if self._tracked_files is not None:
            return self._tracked_files

        try:
            out = subprocess.check_output(
                ["git", "ls-files", "--cached", "--others", "--exclude-standard"],
                cwd=self.root,
                text=True,
                stderr=subprocess.DEVNULL,
            )
            files = [self.root / line for line in out.splitlines() if line]
        except (OSError, subprocess.CalledProcessError):
            files = [
                path
                for path in self.root.rglob("*")
                if path.is_file() and ".git" not in path.relative_to(self.root).parts
            ]

        self._tracked_files = sorted(files, key=lambda p: self.rel(p).lower())
        return self._tracked_files


def load_config(config_arg: str) -> tuple[Path, dict[str, Any], Path]:
    config_path = Path(config_arg)
    if not config_path.is_absolute():
        config_path = Path.cwd() / config_path
    config_path = config_path.resolve()
    with config_path.open(encoding="utf-8") as handle:
        config = json.load(handle)

    root = discover_repo_root(config_path)
    return root.resolve(), config, config_path


def discover_repo_root(config_path: Path) -> Path:
    try:
        out = subprocess.check_output(
            ["git", "-C", str(config_path.parent), "rev-parse", "--show-toplevel"],
            text=True,
            stderr=subprocess.DEVNULL,
        )
        return Path(out.strip())
    except (OSError, subprocess.CalledProcessError):
        pass

    for parent in [config_path.parent, *config_path.parents]:
        if (parent / ".git").exists():
            return parent

    if config_path.parent.name == ".github":
        return config_path.parent.parent
    return config_path.parent


def read_text(path: Path) -> str:
    return path.read_text(encoding="utf-8", errors="replace")


def matches_any(value: str, patterns: list[str]) -> bool:
    return any(fnmatch.fnmatchcase(value, pattern) for pattern in patterns)


def discover_uplugin(ctx: CheckContext) -> tuple[Path | None, dict[str, Any] | None]:
    descriptor = ctx.config.get("plugin_descriptor", "auto")
    if descriptor == "auto":
        candidates = sorted(ctx.root.glob("*.uplugin"))
        if not candidates:
            ctx.block("uplugin", "No .uplugin descriptor found at repository root")
            return None, None
        if len(candidates) > 1:
            ctx.block(
                "uplugin",
                "Expected exactly one root .uplugin descriptor, found "
                + ", ".join(ctx.rel(path) for path in candidates),
            )
            return None, None
        descriptor_path = candidates[0]
    else:
        descriptor_path = ctx.path(str(descriptor))

    try:
        with descriptor_path.open(encoding="utf-8") as handle:
            data = json.load(handle)
    except Exception as exc:  # noqa: BLE001 - report parser failure as CI finding.
        ctx.block("uplugin", f"Failed to parse plugin descriptor: {exc}", descriptor_path)
        return descriptor_path, None

    return descriptor_path, data


def check_uplugin_and_modules(ctx: CheckContext) -> None:
    descriptor_path, data = discover_uplugin(ctx)
    if not descriptor_path or data is None:
        return

    modules = data.get("Modules", [])
    module_names = [str(module.get("Name", "")) for module in modules if module.get("Name")]
    duplicate_modules = sorted({name for name in module_names if module_names.count(name) > 1})
    for module in duplicate_modules:
        ctx.block("module-map", f"Duplicate module in descriptor: {module}", descriptor_path)

    source_dir = ctx.path(str(ctx.config.get("source_dir", "Source")))
    if not source_dir.is_dir():
        ctx.block("module-map", "Source directory is missing", source_dir)
        return

    source_modules = sorted(path.name for path in source_dir.iterdir() if path.is_dir())
    missing_source = sorted(set(module_names) - set(source_modules))
    extra_source = sorted(set(source_modules) - set(module_names))
    for module in missing_source:
        ctx.block("module-map", f"Descriptor module has no Source directory: {module}", descriptor_path)
    for module in extra_source:
        ctx.block("module-map", f"Source directory is not declared in descriptor: {module}", source_dir / module)

    check_buildcs(ctx, source_dir, module_names)
    check_implementation_modules(ctx, module_names)


def check_buildcs(ctx: CheckContext, source_dir: Path, module_names: list[str]) -> None:
    release_guard = str(ctx.config.get("buildcs", {}).get("release_guard_env", ""))
    optional_tokens = list(ctx.config.get("buildcs", {}).get("optional_dependency_tokens", []))

    for module in sorted(module_names):
        buildcs = source_dir / module / f"{module}.Build.cs"
        if not buildcs.is_file():
            ctx.block("buildcs", f"Missing {module}.Build.cs", buildcs)
            continue

        text = read_text(buildcs)
        class_re = re.compile(rf"public\s+class\s+{re.escape(module)}\s*:\s*ModuleRules")
        ctor_re = re.compile(
            rf"public\s+{re.escape(module)}\s*\(\s*ReadOnlyTargetRules\s+Target\s*\)"
            rf"\s*:\s*base\s*\(\s*Target\s*\)"
        )
        if not class_re.search(text):
            ctx.block("buildcs", f"Build.cs class name does not match module {module}", buildcs)
        if not ctor_re.search(text):
            ctx.block("buildcs", f"Build.cs constructor does not match module {module}", buildcs)

        if release_guard:
            used_optional = [token for token in optional_tokens if re.search(rf"\b{re.escape(token)}\b", text)]
            if used_optional and release_guard not in text:
                ctx.block(
                    "buildcs",
                    "Optional dependency token(s) lack release guard "
                    f"{release_guard}: {', '.join(sorted(set(used_optional)))}",
                    buildcs,
                )


def check_implementation_modules(ctx: CheckContext, descriptor_modules: list[str]) -> None:
    impl_re = re.compile(
        r"IMPLEMENT_(?:MODULE|GAME_MODULE|PRIMARY_GAME_MODULE)\s*"
        r"\(\s*[^,]+,\s*([A-Za-z_][A-Za-z0-9_]*)"
    )
    implemented: dict[str, list[str]] = {}
    for path in ctx.tracked_files():
        if path.suffix not in {".cpp", ".cc", ".cxx"}:
            continue
        text = read_text(path)
        for match in impl_re.finditer(text):
            implemented.setdefault(match.group(1), []).append(ctx.rel(path))

    expected = set(descriptor_modules)
    actual = set(implemented)
    for module in sorted(expected - actual):
        ctx.block("implement-module", f"Descriptor module has no IMPLEMENT_MODULE entry: {module}")
    for module in sorted(actual - expected):
        ctx.block(
            "implement-module",
            f"IMPLEMENT_MODULE entry is not declared in descriptor: {module}",
            implemented[module][0],
        )
    for module, paths in sorted(implemented.items()):
        if len(paths) > 1:
            ctx.block("implement-module", f"Duplicate IMPLEMENT_MODULE entries for {module}: {paths[0]}")


def check_automation_test_names(ctx: CheckContext) -> None:
    config = ctx.config.get("automation_tests", {})
    extensions = set(config.get("scan_extensions", [".cpp", ".h", ".hpp"]))
    regexes = [re.compile(value) for value in config.get("name_regexes", [])]
    names: dict[str, list[str]] = {}

    for path in ctx.tracked_files():
        if path.suffix not in extensions:
            continue
        text = read_text(path)
        for regex in regexes:
            for match in regex.finditer(text):
                names.setdefault(match.group(1), []).append(ctx.rel(path))

    if not names and config.get("warn_if_none", False):
        ctx.advisory("automation-tests", "No UE Automation Test names found")
        return

    for name, paths in sorted(names.items()):
        if len(paths) > 1:
            ctx.block("automation-tests", f"Duplicate UE Automation Test name: {name}", paths[0])


def check_action_registry_duplicates(ctx: CheckContext) -> None:
    config = ctx.config.get("action_registry", {})
    if config.get("enabled", True) is False:
        return

    source_dir = ctx.path(str(ctx.config.get("source_dir", "Source")))
    if not source_dir.is_dir():
        return

    extensions = set(config.get("scan_extensions", [".cpp", ".h", ".hpp"]))
    register_re = re.compile(
        r"RegisterAction\s*\(\s*TEXT\(\"([^\"]+)\"\)\s*,\s*TEXT\(\"([^\"]+)\"\)",
        re.MULTILINE | re.DOTALL,
    )
    registrations: dict[tuple[str, str], list[tuple[Path, int]]] = {}

    for path in ctx.tracked_files():
        if path.suffix not in extensions:
            continue
        try:
            path.resolve().relative_to(source_dir)
        except ValueError:
            continue

        text = read_text(path)
        for match in register_re.finditer(text):
            namespace, action = match.group(1), match.group(2)
            line_number = text.count("\n", 0, match.start()) + 1
            registrations.setdefault((namespace, action), []).append((path, line_number))

    for (namespace, action), locations in sorted(registrations.items()):
        if len(locations) <= 1:
            continue
        location_text = ", ".join(f"{ctx.rel(path)}:{line}" for path, line in locations)
        ctx.block(
            "action-registry",
            f"Duplicate action registration {namespace}.{action}: {location_text}",
            locations[0][0],
        )


def check_generated_h_include_order(ctx: CheckContext) -> None:
    for path in ctx.tracked_files():
        if path.suffix not in {".h", ".hpp"}:
            continue
        lines = read_text(path).splitlines()
        generated_index = None
        for index, line in enumerate(lines):
            if ".generated.h" in line and line.strip().startswith("#include"):
                if generated_index is None:
                    generated_index = index

        if generated_index is None:
            continue

        for later_index, line in enumerate(lines[generated_index + 1 :], start=generated_index + 2):
            if line.strip().startswith("#include"):
                ctx.block(
                    "generated-h-order",
                    f".generated.h include is not the final include; later include at line {later_index}",
                    path,
                )
                break


def check_text_hygiene(ctx: CheckContext) -> None:
    text_config = ctx.config.get("text_hygiene", {})
    extensions = set(text_config.get("scan_extensions", []))
    line_ending_allowlist = list(text_config.get("line_ending_allowlist", []))
    line_ending_severity = ctx.config.get("severity", {}).get("line_endings", "advisory")
    for path in ctx.tracked_files():
        if extensions and path.suffix.lower() not in extensions:
            continue
        rel = ctx.rel(path)
        data = path.read_bytes()
        if b"\0" in data:
            ctx.block("text-hygiene", "NUL byte found in text-scanned file", path)
        try:
            data.decode("utf-8")
        except UnicodeDecodeError as exc:
            ctx.block("text-hygiene", f"File is not valid UTF-8: {exc}", path)
        if b"\r\n" in data and not matches_any(rel, line_ending_allowlist):
            if line_ending_severity == "blocker":
                ctx.block("text-hygiene", "CRLF line ending found", path)
            else:
                ctx.advisory("text-hygiene", "CRLF line ending found", path)


def check_repo_hygiene(ctx: CheckContext) -> None:
    patterns = [re.compile(pattern, re.IGNORECASE) for pattern in ctx.config.get("repo_hygiene", {}).get(
        "generated_or_binary_patterns",
        [],
    )]
    lower_seen: dict[str, str] = {}
    for path in ctx.tracked_files():
        rel = ctx.rel(path)
        for pattern in patterns:
            if pattern.search(rel):
                ctx.block("repo-hygiene", "Tracked generated or binary artifact path", rel)
                break

        lowered = rel.lower()
        existing = lower_seen.get(lowered)
        if existing and existing != rel:
            ctx.block("repo-hygiene", f"Case collision: {existing} vs {rel}", rel)
        lower_seen[lowered] = rel


def check_agent_tools(ctx: CheckContext) -> None:
    config = ctx.config.get("agent_tools", {})
    agents_dir = ctx.path(str(config.get("agents_dir", ".claude/agents")))
    dispatcher_prefix = str(config.get("dispatcher_prefix", "mcp__monolith__"))
    missing_is_error = bool(config.get("missing_agents_dir_is_error", False))
    if not agents_dir.exists():
        message = f"Agents directory missing: {ctx.rel(agents_dir)} (Note: This directory is an external prerequisite and may not be tracked in the repository.)"
        if missing_is_error:
            ctx.block("agent-tools", message, agents_dir)
        else:
            ctx.advisory("agent-tools", message, agents_dir)
        return

    dispatcher_re = re.compile(re.escape(dispatcher_prefix) + r"[A-Za-z_][A-Za-z0-9_]*")
    for path in sorted(agents_dir.glob("*.md")):
        text = read_text(path)
        tools = parse_agent_tools_frontmatter(text)
        frontmatter_end = find_frontmatter_end(text)
        if tools is None or frontmatter_end == 0:
            ctx.advisory("agent-tools", "Agent file has no parseable tools frontmatter", path)
            continue
        for line_number, line in enumerate(text.splitlines(), start=1):
            if line_number <= frontmatter_end:
                continue
            for match in dispatcher_re.finditer(line):
                tool = match.group(0)
                if tool not in tools:
                    ctx.block("agent-tools", f"Dispatcher {tool} missing from tools frontmatter", path)


def parse_agent_tools_frontmatter(text: str) -> set[str] | None:
    def clean_tool_value(raw: str) -> str:
        return raw.split("#", 1)[0].strip().strip("'\"")

    lines = text.splitlines()
    if not lines or lines[0].strip() != "---":
        return None
    tools: set[str] = set()
    found_tools = False
    in_tools_list = False
    for line in lines[1:]:
        stripped = line.strip()
        if stripped == "---":
            return tools if found_tools else None
        if in_tools_list:
            if stripped.startswith("- "):
                tools.add(clean_tool_value(stripped[2:]))
                continue
            if line and not line[0].isspace() and not stripped.startswith("#"):
                in_tools_list = False
        if stripped.startswith("tools:"):
            found_tools = True
            value = stripped.split(":", 1)[1].strip()
            if value:
                value = clean_tool_value(value).strip("[]")
                tools.update(
                    clean_tool_value(token)
                    for token in value.split(",")
                    if clean_tool_value(token)
                )
            else:
                in_tools_list = True
    return None


def find_frontmatter_end(text: str) -> int:
    lines = text.splitlines()
    if not lines or lines[0].strip() != "---":
        return 0
    for index, line in enumerate(lines[1:], start=2):
        if line.strip() == "---":
            return index
    return 0


def check_secrets(ctx: CheckContext) -> None:
    extensions = set(ctx.config.get("text_hygiene", {}).get("scan_extensions", []))
    secret_config = ctx.config.get("secrets", {})
    high_conf = [re.compile(value) for value in secret_config.get("high_confidence_regexes", [])]
    broad = [re.compile(value) for value in secret_config.get("broad_advisory_regexes", [])]

    for path in ctx.tracked_files():
        if extensions and path.suffix.lower() not in extensions:
            continue
        text = read_text(path)
        for regex in high_conf:
            if regex.search(text):
                ctx.block("secrets", "High-confidence secret pattern found", path)
                break
        for regex in broad:
            if regex.search(text):
                ctx.advisory("secrets", "Broad secret-like assignment pattern found", path)
                break


def check_workflow_scope(ctx: CheckContext) -> None:
    config = ctx.config.get("workflow", {})
    hosted = ctx.path(str(config.get("hosted_static_workflow", ".github/workflows/ci.yml")))
    if not hosted.is_file():
        ctx.block("workflow", "Hosted static CI workflow is missing", hosted)
        return

    text = read_text(hosted)
    for token in config.get("forbidden_tokens", []):
        if token in text:
            ctx.block("workflow", f"Hosted static CI workflow contains forbidden token: {token}", hosted)

    for duplicate in config.get("duplicate_workflow_paths", []):
        duplicate_path = ctx.path(str(duplicate))
        if duplicate_path.exists():
            ctx.block("workflow", "Duplicate hosted static CI workflow path exists", duplicate_path)


def check_proxy_smoke(ctx: CheckContext) -> None:
    config = ctx.config.get("proxy_smoke", {})
    if not config.get("enabled", False):
        return

    script = ctx.path(str(config.get("script", "")))
    if not script.is_file():
        ctx.block("proxy-smoke", "Proxy smoke script is missing", script)
        return

    env = os.environ.copy()
    env[str(config.get("url_env", "MONOLITH_URL"))] = str(config.get("offline_url", "http://127.0.0.1:9/mcp"))
    timeout = float(config.get("timeout_seconds", 5))
    log_tmp = tempfile.TemporaryDirectory()
    env["MONOLITH_TOOL_LOG_DIR"] = log_tmp.name
    env.pop("MONOLITH_TOOL_LOG_ENABLED", None)
    proc = subprocess.Popen(
        [sys.executable, str(script)],
        cwd=ctx.root,
        env=env,
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    assert proc.stdout is not None
    stdout_lines: queue.Queue[str] = queue.Queue()

    def read_stdout() -> None:
        assert proc.stdout is not None
        for line in proc.stdout:
            stdout_lines.put(line)

    stdout_thread = threading.Thread(target=read_stdout, daemon=True)
    stdout_thread.start()

    def request(message: dict[str, Any]) -> dict[str, Any]:
        assert proc.stdin is not None
        proc.stdin.write(json.dumps(message) + "\n")
        proc.stdin.flush()
        try:
            line = stdout_lines.get(timeout=timeout)
        except queue.Empty as exc:
            raise TimeoutError(f"timed out waiting for {message.get('method')}") from exc
        return json.loads(line)

    try:
        responses = [
            request({
                "jsonrpc": "2.0",
                "id": 1,
                "method": "initialize",
                "params": {"protocolVersion": "2025-11-25"},
            }),
            request({"jsonrpc": "2.0", "id": 2, "method": "ping"}),
            request({"jsonrpc": "2.0", "id": 3, "method": "tools/list"}),
            request({
                "jsonrpc": "2.0",
                "id": 4,
                "method": "tools/call",
                "params": {"name": "ci_static_smoke", "arguments": {}},
            }),
        ]
        server_name = responses[0].get("result", {}).get("serverInfo", {}).get("name")
        if server_name != config.get("expected_server_name"):
            ctx.block("proxy-smoke", f"Unexpected proxy server name: {server_name}", script)
        if responses[1].get("result") != {}:
            ctx.block("proxy-smoke", "ping did not return an empty result", script)
        tools = responses[2].get("result", {}).get("tools")
        min_tools = int(config.get("expected_offline_min_tools", 0))
        if not isinstance(tools, list) or len(tools) < min_tools:
            ctx.block(
                "proxy-smoke",
                f"offline tools/list returned {0 if not isinstance(tools, list) else len(tools)} tools; "
                f"expected at least {min_tools}",
                script,
            )
        if responses[3].get("result", {}).get("isError") is not True:
            ctx.block("proxy-smoke", "offline tools/call did not return graceful tool error", script)
        log_files = list(Path(log_tmp.name).glob("*/proxy.jsonl"))
        if not log_files:
            ctx.block("proxy-smoke", "proxy daily log was not created with MONOLITH_TOOL_LOG_ENABLED unset", script)
        else:
            records = [json.loads(line) for line in log_files[0].read_text(encoding="utf-8").splitlines() if line.strip()]
            matching = [
                record for record in records
                if record.get("surface") == "proxy"
                and record.get("call", {}).get("tool_name_original") == "ci_static_smoke"
            ]
            if not matching:
                ctx.block("proxy-smoke", "proxy daily log did not include the smoke tools/call record", log_files[0])
            else:
                record = matching[-1]
                if record.get("client", {}).get("proxy_runtime") != "python":
                    ctx.block("proxy-smoke", "proxy daily log did not identify the Python proxy runtime", log_files[0])
                if record.get("format_version") != 3:
                    ctx.block("proxy-smoke", "proxy daily log did not use format_version 3", log_files[0])
                if not record.get("record_id") or not record.get("trace_id") or not record.get("span_id"):
                    ctx.block("proxy-smoke", "proxy daily log did not include record_id/trace_id/span_id", log_files[0])
                if not record.get("process_instance_id") or "call_index" not in record:
                    ctx.block("proxy-smoke", "proxy daily log did not include process timeline fields", log_files[0])
                if not isinstance(record.get("routing_context"), dict):
                    ctx.block("proxy-smoke", "proxy daily log did not include routing_context", log_files[0])
                if not isinstance(record.get("workflow"), dict):
                    ctx.block("proxy-smoke", "proxy daily log did not include workflow", log_files[0])
                if not isinstance(record.get("phase_timing"), dict):
                    ctx.block("proxy-smoke", "proxy daily log did not include phase_timing", log_files[0])
                if not isinstance(record.get("return_summary"), dict):
                    ctx.block("proxy-smoke", "proxy daily log did not include return_summary", log_files[0])
                elif not record.get("return_summary", {}).get("result_shape"):
                    ctx.block("proxy-smoke", "proxy daily log return_summary did not include result_shape", log_files[0])
                if record.get("agent_signal", {}).get("outcome") != "editor_unavailable":
                    ctx.block("proxy-smoke", "proxy daily log did not classify offline call as editor_unavailable", log_files[0])
                if "retry_signature" in record.get("agent_signal", {}):
                    ctx.block("proxy-smoke", "proxy daily log duplicated retry_signature in agent_signal", log_files[0])

        disabled_tmp = tempfile.TemporaryDirectory()
        disabled_env = env.copy()
        disabled_env["MONOLITH_TOOL_LOG_DIR"] = disabled_tmp.name
        disabled_env["MONOLITH_TOOL_LOG_ENABLED"] = "0"
        disabled_proc = subprocess.Popen(
            [sys.executable, str(script)],
            cwd=ctx.root,
            env=disabled_env,
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
        try:
            disabled_proc.communicate(input=json.dumps({
                "jsonrpc": "2.0",
                "id": 5,
                "method": "tools/call",
                "params": {"name": "ci_static_disabled_smoke", "arguments": {}},
            }) + "\n", timeout=timeout)
            if list(Path(disabled_tmp.name).glob("*/proxy.jsonl")):
                ctx.block("proxy-smoke", "proxy daily log was created despite MONOLITH_TOOL_LOG_ENABLED=0", script)
        except subprocess.TimeoutExpired:
            ctx.block("proxy-smoke", "disabled proxy daily log smoke timed out", script)
        finally:
            disabled_proc.kill()
            try:
                disabled_proc.wait(timeout=1)
            except subprocess.TimeoutExpired:
                disabled_proc.terminate()
            disabled_tmp.cleanup()
    except Exception as exc:  # noqa: BLE001 - convert smoke failure to CI finding.
        ctx.block("proxy-smoke", f"Offline JSON-RPC smoke failed: {exc}", script)
    finally:
        proc.kill()
        try:
            proc.wait(timeout=1)
        except subprocess.TimeoutExpired:
            proc.terminate()
        log_tmp.cleanup()


def run_checks(ctx: CheckContext) -> list[Finding]:
    check_uplugin_and_modules(ctx)
    check_automation_test_names(ctx)
    check_action_registry_duplicates(ctx)
    check_generated_h_include_order(ctx)
    check_text_hygiene(ctx)
    check_repo_hygiene(ctx)
    check_agent_tools(ctx)
    check_secrets(ctx)
    check_workflow_scope(ctx)
    check_proxy_smoke(ctx)
    return ctx.findings


def print_report(findings: list[Finding], github: bool) -> int:
    blockers = [finding for finding in findings if finding.severity == "blocker"]
    advisories = [finding for finding in findings if finding.severity == "advisory"]

    print("Monolith hosted static CI checks")
    print(f"Blocking findings: {len(blockers)}")
    print(f"Advisory findings: {len(advisories)}")

    for title, group in [("BLOCKER", blockers), ("ADVISORY", advisories)]:
        if not group:
            continue
        print(f"\n{title}:")
        for finding in group:
            location = f"{finding.path}: " if finding.path else ""
            print(f"- [{finding.check}] {location}{finding.message}")
            if github and finding.severity == "blocker":
                file_part = f" file={finding.path}," if finding.path else ""
                print(f"::error{file_part}title={finding.check}::{finding.message}")
            elif github and finding.severity == "advisory":
                file_part = f" file={finding.path}," if finding.path else ""
                print(f"::warning{file_part}title={finding.check}::{finding.message}")

    summary_path = os.environ.get("GITHUB_STEP_SUMMARY")
    if summary_path:
        with open(summary_path, "a", encoding="utf-8") as handle:
            handle.write("## Monolith hosted static CI checks\n\n")
            handle.write(f"- Blocking findings: `{len(blockers)}`\n")
            handle.write(f"- Advisory findings: `{len(advisories)}`\n")
            for title, group in [("Blocking", blockers), ("Advisory", advisories)]:
                if group:
                    handle.write(f"\n### {title}\n")
                    for finding in group:
                        location = f"`{finding.path}`: " if finding.path else ""
                        handle.write(f"- `{finding.check}` {location}{finding.message}\n")

    return 1 if blockers else 0


def write_selftest_fixture(root: Path) -> tuple[dict[str, Any], Path]:
    (root / ".github/workflows").mkdir(parents=True)
    (root / "Source/Foo/Private").mkdir(parents=True)
    (root / ".claude/agents").mkdir(parents=True)
    (root / ".github/workflows/ci.yml").write_text("name: CI\n", encoding="utf-8")
    config = {
        "plugin_descriptor": "auto",
        "source_dir": "Source",
        "buildcs": {"release_guard_env": "RELEASE", "optional_dependency_tokens": []},
        "repo_hygiene": {"generated_or_binary_patterns": [r"(^|/)Binaries/"]},
        "automation_tests": {
            "scan_extensions": [".cpp", ".h"],
            "name_regexes": [
                r"IMPLEMENT_(?:SIMPLE_|COMPLEX_)?AUTOMATION_TEST\s*\([^,]+,\s*\"([^\"]+)\""
            ],
            "warn_if_none": False,
        },
        "text_hygiene": {"scan_extensions": [".uplugin", ".cs", ".cpp", ".h", ".md", ".txt", ".yml"]},
        "agent_tools": {"agents_dir": ".claude/agents", "missing_agents_dir_is_error": False},
        "proxy_smoke": {"enabled": False},
        "workflow": {
            "hosted_static_workflow": ".github/workflows/ci.yml",
            "forbidden_tokens": ["RunUBT"],
            "duplicate_workflow_paths": [".github/workflows/static-ci.yml"],
        },
        "secrets": {
            "high_confidence_regexes": [r"AKIA[0-9A-Z]{16}"],
            "broad_advisory_regexes": [],
        },
    }
    config_path = root / ".github/monolith-static-ci.json"
    config_path.write_text(json.dumps(config), encoding="utf-8")
    (root / "Foo.uplugin").write_text(
        json.dumps({"Modules": [{"Name": "Foo", "Type": "Editor"}]}),
        encoding="utf-8",
    )
    (root / "Source/Foo/Foo.Build.cs").write_text(
        "using UnrealBuildTool;\n"
        "public class Foo : ModuleRules\n"
        "{\n"
        "    public Foo(ReadOnlyTargetRules Target) : base(Target) {}\n"
        "}\n",
        encoding="utf-8",
    )
    (root / "Source/Foo/Private/FooModule.cpp").write_text(
        "#include \"Modules/ModuleManager.h\"\nIMPLEMENT_MODULE(FFooModule, Foo)\n",
        encoding="utf-8",
    )
    (root / ".claude/agents/good.md").write_text(
        "---\ntools:\n  - mcp__monolith__allowed # preferred tool\n  - mcp__monolith__inline # inline comment in list value\n---\nUses mcp__monolith__allowed and mcp__monolith__inline.\n",
        encoding="utf-8",
    )
    (root / ".claude/agents/good_bracket.md").write_text(
        "---\ntools: [mcp__monolith__bracket, mcp__monolith__comment] # bracket + inline comment\n---\nUses mcp__monolith__bracket.\n",
        encoding="utf-8",
    )
    return config, config_path


def selftest_findings(mutator: Any | None = None) -> list[Finding]:
    with tempfile.TemporaryDirectory() as temp:
        root = Path(temp)
        config, config_path = write_selftest_fixture(root)
        if mutator:
            mutator(root)
        return run_checks(CheckContext(root, config, config_path))


def selftest_has_blocker(findings: list[Finding], check: str) -> bool:
    return any(finding.severity == "blocker" and finding.check == check for finding in findings)


def run_selftest() -> int:
    with tempfile.TemporaryDirectory() as temp:
        root = Path(temp)
        config, _ = write_selftest_fixture(root)
        root_config = root / "monolith-static-ci.json"
        root_config.write_text(json.dumps(config), encoding="utf-8")
        loaded_root, _, _ = load_config(str(root_config))
        if loaded_root != root.resolve():
            print(f"selftest root config loaded wrong root: {loaded_root}")
            return 1

    clean_blockers = [finding for finding in selftest_findings() if finding.severity == "blocker"]
    if clean_blockers:
        print("selftest clean fixture failed")
        for finding in clean_blockers:
            print(finding)
        return 1

    cases = [
        (
            "module map drift",
            "module-map",
            lambda root: (root / "Source/Bar").mkdir(parents=True),
        ),
        (
            "Build.cs class drift",
            "buildcs",
            lambda root: (root / "Source/Foo/Foo.Build.cs").write_text(
                "using UnrealBuildTool;\npublic class Drift : ModuleRules {}\n",
                encoding="utf-8",
            ),
        ),
        (
            "duplicate automation test names",
            "automation-tests",
            lambda root: (root / "Source/Foo/Private/DuplicateTests.cpp").write_text(
                "IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOne, \"Monolith.Duplicate\", Flags)\n"
                "IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTwo, \"Monolith.Duplicate\", Flags)\n",
                encoding="utf-8",
            ),
        ),
        (
            "duplicate action registration",
            "action-registry",
            lambda root: (root / "Source/Foo/Private/DuplicateActions.cpp").write_text(
                "void Register(FRegistry& Registry) {\n"
                "    Registry.RegisterAction(TEXT(\"foo\"), TEXT(\"bar\"), TEXT(\"one\"), Handler);\n"
                "    Registry.RegisterAction(TEXT(\"foo\"), TEXT(\"bar\"), TEXT(\"two\"), Handler);\n"
                "}\n",
                encoding="utf-8",
            ),
        ),
        (
            ".generated.h include order",
            "generated-h-order",
            lambda root: (root / "Source/Foo/Private/BadGeneratedOrder.h").write_text(
                "#include \"Foo.generated.h\"\n#include \"AfterGenerated.h\"\n",
                encoding="utf-8",
            ),
        ),
        (
            ".generated.h include order false-negative case",
            "generated-h-order",
            lambda root: (root / "Source/Foo/Private/GeneratedMiddle.h").write_text(
                "#include \"Foo.generated.h\"\n#include \"AfterGenerated.h\"\n#include \"Bar.generated.h\"\n",
                encoding="utf-8",
            ),
        ),
        (
            "text hygiene NUL",
            "text-hygiene",
            lambda root: (root / "Source/Foo/Private/Nul.txt").write_bytes(b"bad\0text\n"),
        ),
        (
            "agent allowlist drift",
            "agent-tools",
            lambda root: (root / ".claude/agents/bad.md").write_text(
                "---\ntools: mcp__monolith__allowed\n---\nUses mcp__monolith__missing.\n",
                encoding="utf-8",
            ),
        ),
        (
            "high-confidence secret",
            "secrets",
            lambda root: (root / "Source/Foo/Private/Secret.txt").write_text(
                "aws = \"" + "AKIA" + "1234567890ABCDEF" + "\"\n",
                encoding="utf-8",
            ),
        ),
        (
            "forbidden hosted workflow token",
            "workflow",
            lambda root: (root / ".github/workflows/ci.yml").write_text("run: RunUBT\n", encoding="utf-8"),
        ),
    ]

    for label, check, mutator in cases:
        findings = selftest_findings(mutator)
        if not selftest_has_blocker(findings, check):
            print(f"selftest {label} fixture did not fail as {check}")
            for finding in findings:
                print(finding)
            return 1

    print("selftest passed")
    return 0


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Run Monolith hosted static CI checks")
    parser.add_argument("--config", default=".github/monolith-static-ci.json", help="Path to static CI config")
    parser.add_argument("--github", action="store_true", help="Emit GitHub Actions annotations")
    subparsers = parser.add_subparsers(dest="command", required=True)
    subparsers.add_parser("check", help="Run checks against the repository")
    subparsers.add_parser("selftest", help="Run checker fixture selftest")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    if args.command == "selftest":
        return run_selftest()

    root, config, config_path = load_config(args.config)
    ctx = CheckContext(root, config, config_path)
    findings = run_checks(ctx)
    return print_report(findings, args.github)


if __name__ == "__main__":
    raise SystemExit(main())
