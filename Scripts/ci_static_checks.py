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
import shutil
import socket
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
            files = [path for path in files if path.is_file()]
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

    uplugin_contract = ctx.config.get("uplugin", {})
    if not isinstance(uplugin_contract, dict):
        ctx.block("uplugin-dependency", "uplugin config must be an object", ctx.config_path)
        uplugin_contract = {}

    plugin_references = data.get("Plugins", [])
    if not isinstance(plugin_references, list):
        ctx.block("uplugin-dependency", "Plugins must be a list", descriptor_path)
        plugin_references = []

    required_plugins = uplugin_contract.get("required_plugin_references", [])
    if not isinstance(required_plugins, list):
        ctx.block(
            "uplugin-dependency",
            "uplugin.required_plugin_references must be a list",
            ctx.config_path,
        )
    else:
        for required_name in sorted({str(name) for name in required_plugins}):
            matching_references = [
                reference
                for reference in plugin_references
                if isinstance(reference, dict)
                and str(reference.get("Name", "")).casefold() == required_name.casefold()
            ]
            if not matching_references:
                ctx.block(
                    "uplugin-dependency",
                    f"Required plugin reference is missing: {required_name}",
                    descriptor_path,
                )
            elif len(matching_references) != 1:
                ctx.block(
                    "uplugin-dependency",
                    f"Required plugin reference must appear exactly once: {required_name}",
                    descriptor_path,
                )
            elif not (
                matching_references[0].get("Enabled") is True
                and matching_references[0].get("Optional") is not True
            ):
                ctx.block(
                    "uplugin-dependency",
                    f"Required plugin reference must set Enabled=true and must not set Optional=true: {required_name}",
                    descriptor_path,
                )

    forbidden_optional = uplugin_contract.get("forbidden_optional_plugin_references", [])
    if not isinstance(forbidden_optional, list):
        ctx.block(
            "uplugin-dependency",
            "uplugin.forbidden_optional_plugin_references must be a list",
            ctx.config_path,
        )
    else:
        forbidden_optional_names = {str(name).casefold() for name in forbidden_optional}
        for reference in plugin_references:
            if not isinstance(reference, dict):
                continue
            name = str(reference.get("Name", ""))
            if (
                name.casefold() in forbidden_optional_names
                and reference.get("Enabled") is True
                and reference.get("Optional") is True
            ):
                ctx.block(
                    "uplugin-dependency",
                    f"Enabled optional plugin reference is forbidden by repository contract: {name}",
                    descriptor_path,
                )

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
    project_template_re = re.compile(r"RegisterProjectAction\s*<\s*([A-Za-z_][A-Za-z0-9_:]*)\s*>\s*\(")
    get_name_re = re.compile(
        r"(?:class|struct)\s+([A-Za-z_][A-Za-z0-9_]*)[\s\S]{0,3000}?"
        r"GetName\s*\(\s*\)\s*(?:const\s*)?\{\s*return\s+TEXT\(\"([^\"]+)\"\)\s*;",
        re.MULTILINE,
    )
    registrations: dict[tuple[str, str], list[tuple[Path, int]]] = {}
    action_names: dict[str, tuple[str, Path, int]] = {}

    for path in ctx.tracked_files():
        if path.suffix not in extensions:
            continue
        try:
            path.resolve().relative_to(source_dir)
        except ValueError:
            continue

        text = read_text(path)
        for match in get_name_re.finditer(text):
            class_name, action = match.group(1), match.group(2)
            line_number = text.count("\n", 0, match.start()) + 1
            action_names[class_name] = (action, path, line_number)

        for match in register_re.finditer(text):
            namespace, action = match.group(1), match.group(2)
            line_number = text.count("\n", 0, match.start()) + 1
            registrations.setdefault((namespace, action), []).append((path, line_number))

    for path in ctx.tracked_files():
        if path.suffix not in extensions:
            continue
        try:
            path.resolve().relative_to(source_dir)
        except ValueError:
            continue

        text = read_text(path)
        for match in project_template_re.finditer(text):
            class_name = match.group(1).split("::")[-1]
            line_number = text.count("\n", 0, match.start()) + 1
            action_info = action_names.get(class_name)
            if action_info is None:
                ctx.block(
                    "action-registry",
                    f"Could not resolve RegisterProjectAction<{class_name}> GetName()",
                    path,
                )
                continue

            action, _name_path, _name_line = action_info
            registrations.setdefault(("project", action), []).append((path, line_number))

    for (namespace, action), locations in sorted(registrations.items()):
        if len(locations) <= 1:
            continue
        location_text = ", ".join(f"{ctx.rel(path)}:{line}" for path, line in locations)
        ctx.block(
            "action-registry",
            f"Duplicate action registration {namespace}.{action}: {location_text}",
            locations[0][0],
        )


def find_matching_brace(text: str, open_index: int) -> int:
    depth = 0
    in_string = False
    escape = False
    index = open_index
    while index < len(text):
        char = text[index]
        if in_string:
            if escape:
                escape = False
            elif char == "\\":
                escape = True
            elif char == '"':
                in_string = False
        else:
            if char == '"':
                in_string = True
            elif char == "{":
                depth += 1
            elif char == "}":
                depth -= 1
                if depth == 0:
                    return index
        index += 1
    return -1


def extract_action_registrations(text: str) -> list[dict[str, Any]]:
    register_re = re.compile(
        r"RegisterAction\s*\(\s*TEXT\(\"([^\"]+)\"\)\s*,\s*TEXT\(\"([^\"]+)\"\)",
        re.MULTILINE | re.DOTALL,
    )
    registrations: list[dict[str, Any]] = []
    for match in register_re.finditer(text):
        open_index = text.find("(", match.start())
        if open_index == -1:
            continue
        # RegisterAction calls are parenthesized, not braced; scan parentheses
        # separately so nested FParamSchemaBuilder calls are included.
        depth = 0
        in_string = False
        escape = False
        end_index = -1
        index = open_index
        while index < len(text):
            char = text[index]
            if in_string:
                if escape:
                    escape = False
                elif char == "\\":
                    escape = True
                elif char == '"':
                    in_string = False
            else:
                if char == '"':
                    in_string = True
                elif char == "(":
                    depth += 1
                elif char == ")":
                    depth -= 1
                    if depth == 0:
                        end_index = index + 1
                        break
            index += 1
        if end_index == -1:
            continue
        block = text[match.start():end_index]
        handler_match = re.search(r"CreateStatic\s*\(\s*&([A-Za-z_][A-Za-z0-9_:]*)\s*\)", block)
        registrations.append({
            "namespace": match.group(1),
            "action": match.group(2),
            "block": block,
            "line": text.count("\n", 0, match.start()) + 1,
            "handler": handler_match.group(1).split("::")[-1] if handler_match else "",
        })
    return registrations


def extract_function_body(text: str, function_name: str) -> str:
    if not function_name:
        return ""
    match = re.search(rf"\b{re.escape(function_name)}\s*\([^)]*\)\s*(?:const\s*)?\{{", text)
    if not match:
        return ""
    open_index = text.find("{", match.end() - 1)
    if open_index == -1:
        return ""
    close_index = find_matching_brace(text, open_index)
    if close_index == -1:
        return ""
    return text[open_index:close_index + 1]


def check_action_registry_hygiene(ctx: CheckContext) -> None:
    config = ctx.config.get("routing_hygiene", {})
    if config.get("enabled", True) is False:
        return

    high_risk_config = config.get("high_risk_actions", {})
    high_risk_actions: set[tuple[str, str]] = set()
    if isinstance(high_risk_config, dict):
        for namespace, actions in high_risk_config.items():
            if isinstance(actions, list):
                high_risk_actions.update((str(namespace), str(action)) for action in actions)

    if not high_risk_actions:
        return

    source_dir = ctx.path(str(ctx.config.get("source_dir", "Source")))
    extensions = set(config.get("scan_extensions", [".cpp", ".h", ".hpp"]))
    registrations: dict[tuple[str, str], tuple[Path, dict[str, Any], str]] = {}

    for path in ctx.tracked_files():
        if path.suffix not in extensions:
            continue
        try:
            path.resolve().relative_to(source_dir)
        except ValueError:
            continue
        text = read_text(path)
        for registration in extract_action_registrations(text):
            key = (registration["namespace"], registration["action"])
            if key in high_risk_actions:
                registrations[key] = (path, registration, text)

    for namespace, action in sorted(high_risk_actions):
        found = registrations.get((namespace, action))
        if not found:
            ctx.advisory(
                "action-registry-hygiene",
                f"Configured high-risk action registration not found: {namespace}.{action}",
            )
            continue

        path, registration, text = found
        block = registration["block"]
        action_id = f"{namespace}.{action}"
        if "FParamSchemaBuilder" not in block or ".Build()" not in block:
            ctx.block("action-registry-hygiene", f"High-risk action lacks param schema: {action_id}", path)
        elif "EnableValidation()" not in block:
            ctx.block("action-registry-hygiene", f"High-risk action schema lacks EnableValidation(): {action_id}", path)

        body = extract_function_body(text, str(registration.get("handler", "")))
        if body:
            if re.search(r"\bParams\s*->\s*Get(?:String|Number)Field\s*\(", body):
                ctx.advisory("action-registry-hygiene", f"High-risk handler uses raw Params getter: {action_id}", path)
            if "LoadObject<UNiagaraSystem>" in body:
                ctx.advisory("action-registry-hygiene", f"High-risk handler directly loads Niagara systems instead of shared helper: {action_id}", path)


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
        rel = ctx.rel(path)
        parts = rel.split("/")
        if "Binaries" in parts or "Intermediate" in parts or "Saved" in parts or "DerivedDataCache" in parts:
            continue
        if extensions and path.suffix.lower() not in extensions:
            continue
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

    dispatcher_re = re.compile(re.escape(dispatcher_prefix) + r"[a-zA-Z_-][a-zA-Z0-9_-]*")
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
            clean_val = clean_tool_value(value).strip("[]")
            if clean_val:
                tools.update(
                    clean_tool_value(token)
                    for token in clean_val.split(",")
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

    previous_position = -1
    for token in config.get("required_ordered_tokens", []):
        position = text.find(str(token))
        if position < 0:
            ctx.block("workflow", f"Hosted static CI workflow is missing required token: {token}", hosted)
            continue
        if position <= previous_position:
            ctx.block("workflow", f"Hosted static CI workflow token is out of order: {token}", hosted)
        previous_position = max(previous_position, position)

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
        required_tools = config.get("required_offline_tools", [])
        if isinstance(tools, list) and isinstance(required_tools, list):
            tool_names = {
                tool.get("name")
                for tool in tools
                if isinstance(tool, dict) and isinstance(tool.get("name"), str)
            }
            missing_tools = [
                name
                for name in required_tools
                if isinstance(name, str) and name not in tool_names
            ]
            if missing_tools:
                ctx.block(
                    "proxy-smoke",
                    "offline tools/list missing required tool(s): " + ", ".join(missing_tools),
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


def _endpoint_reachable(url: str, timeout: float = 1.5) -> bool:
    """Quick TCP probe of the host:port in an http(s) URL (no HTTP request)."""
    match = re.match(r"https?://([^/:]+)(?::(\d+))?", url)
    if not match:
        return False
    host = match.group(1)
    port = int(match.group(2) or (443 if url.startswith("https") else 80))
    try:
        with socket.create_connection((host, port), timeout=timeout):
            return True
    except OSError:
        return False


def check_skill_catalog_drift(ctx: CheckContext) -> None:
    """Gate skill action/param tables against the live catalog via check_skill_catalog_drift.ps1.

    Catalog source preference: committed offline dumps (-DumpDir) -> live editor at mcp_url ->
    skip. Only a hard-drift result (script exit 2) blocks; an unavailable catalog (no dumps and
    no editor, or operational exit 3) is a non-blocking advisory so headless CI never fails for
    lack of a running editor. Commit dumps (generated from a live editor) to make this a hard
    gate on GitHub. See Docs/specs/SPEC_MonolithSkillCatalogDrift.md.
    """
    config = ctx.config.get("skill_drift", {})
    if not config.get("enabled", False):
        return

    script = ctx.path(str(config.get("script", "Scripts/check_skill_catalog_drift.ps1")))
    if not script.is_file():
        ctx.block("skill-drift", "Skill catalog drift script is missing", script)
        return

    pwsh = shutil.which("pwsh") or shutil.which("powershell")
    if not pwsh:
        ctx.advisory("skill-drift", "skipped: no PowerShell (pwsh/powershell) available to run the drift guard")
        return

    mcp_url = str(config.get("mcp_url", "http://localhost:9316/mcp"))
    dump_dir_cfg = config.get("dump_dir")
    dump_dir = ctx.path(str(dump_dir_cfg)) if dump_dir_cfg else None
    args = [pwsh, "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", str(script)]

    if dump_dir and dump_dir.is_dir() and any(dump_dir.glob("*.json")):
        args += ["-Offline", "-DumpDir", str(dump_dir)]
        mode = f"offline:{ctx.rel(dump_dir)}"
    elif _endpoint_reachable(mcp_url):
        args += ["-McpUrl", mcp_url]
        mode = f"live:{mcp_url}"
    else:
        ctx.advisory(
            "skill-drift",
            "skipped: no committed catalog dumps and live editor unreachable at "
            f"{mcp_url}. Run with the editor up, or commit dumps to enable headless gating "
            "(Docs/specs/SPEC_MonolithSkillCatalogDrift.md).",
        )
        return

    try:
        proc = subprocess.run(
            args, cwd=ctx.root, capture_output=True, text=True,
            timeout=float(config.get("timeout_seconds", 180)),
        )
    except (OSError, subprocess.TimeoutExpired) as exc:
        ctx.advisory("skill-drift", f"skipped: drift guard did not complete ({mode}): {exc}")
        return

    stdout = proc.stdout or ""
    result_line = next((ln for ln in reversed(stdout.splitlines()) if ln.startswith("RESULT=")), "")
    if proc.returncode == 0:
        return  # clean (feature-gated actions are reported but do not fail)
    if proc.returncode == 2:
        drift_lines = [ln for ln in stdout.splitlines() if ln.startswith("DRIFT")][:6]
        detail = "; ".join(drift_lines) if drift_lines else (result_line or "hard drift")
        ctx.block("skill-drift", f"Skill docs drift from live catalog ({mode}). {detail}")
        return
    # exit 3 / other: catalog unavailable or usage error -> non-blocking (not a code defect)
    reason = result_line or (proc.stderr or "").strip().splitlines()[-1:] or ["operational error"]
    ctx.advisory("skill-drift", f"skipped: drift guard non-gating exit {proc.returncode} ({mode}): {reason if isinstance(reason, str) else reason[0]}")


def _load_sibling_module(script: Path, alias: str) -> Any | None:
    import importlib.util

    spec = importlib.util.spec_from_file_location(alias, script)
    if spec is None or spec.loader is None:
        return None
    module = importlib.util.module_from_spec(spec)
    script_dir = str(script.parent)
    inserted = False
    if script_dir not in sys.path:
        sys.path.insert(0, script_dir)
        inserted = True
    try:
        spec.loader.exec_module(module)
        return module
    finally:
        if inserted:
            try:
                sys.path.remove(script_dir)
            except ValueError:
                pass


def _load_offline_exe_freshness_module(
    ctx: CheckContext,
    finding_check: str,
) -> tuple[Path, Any] | None:
    """Load the single authority for selecting the current Query executable."""
    config = ctx.config.get("offline_exe_freshness", {})
    script = ctx.path(str(config.get("script", "Scripts/check_offline_exe_fresh.py")))
    if not script.is_file():
        ctx.block(finding_check, "Offline-exe freshness script is missing", script)
        return None

    try:
        fresh = _load_sibling_module(script, "monolith_check_offline_exe_fresh")
    except Exception as exc:  # noqa: BLE001 - report load failure as a finding.
        ctx.block(finding_check, f"Could not load offline-exe freshness script: {exc}", script)
        return None
    if fresh is None:
        ctx.block(finding_check, "Could not load offline-exe freshness script", script)
        return None
    return script, fresh


def _plugin_relative_path(value: str | Path) -> Path:
    text = str(value).replace("\\", "/")
    parts = [part for part in Path(text).as_posix().split("/") if part not in ("", ".")]
    if len(parts) >= 2 and parts[0].lower() == "plugins" and parts[1].lower() == "monolith":
        parts = parts[2:]
    return Path(*parts) if parts else Path(".")


def _resolve_plugin_path(ctx: CheckContext, value: str | Path) -> Path:
    path = Path(value)
    if path.is_absolute():
        return path.resolve()
    return (ctx.root / _plugin_relative_path(value)).resolve()


def _same_resolved_path(left: Path, right: Path) -> bool:
    return os.path.normcase(str(left.resolve())) == os.path.normcase(str(right.resolve()))


def _read_json_dict(path: Path) -> dict[str, Any] | None:
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        return None
    return data if isinstance(data, dict) else None


def _non_empty_line_count(path: Path) -> int:
    count = 0
    with path.open("r", encoding="utf-8", errors="replace") as handle:
        for line in handle:
            if line.strip():
                count += 1
    return count


def check_benchmark_definitions(ctx: CheckContext) -> None:
    """Validate benchmark fixture manifests without needing a live MCP server."""
    config = ctx.config.get("benchmark_definitions", {})
    if not config.get("enabled", False):
        return

    definitions = config.get("definitions", [])
    if not isinstance(definitions, list):
        ctx.block("benchmark-definitions", "benchmark_definitions.definitions must be a list", ctx.config_path)
        return

    for raw_entry in definitions:
        if not isinstance(raw_entry, dict):
            ctx.block("benchmark-definitions", "Benchmark definition entry must be an object", ctx.config_path)
            continue

        name = str(raw_entry.get("name") or "<unnamed>")
        script_cfg = raw_entry.get("script")
        manifest_cfg = raw_entry.get("manifest")
        data_key = "tasks" if raw_entry.get("tasks") is not None else "probe_set"
        data_cfg = raw_entry.get(data_key)
        count_field = str(raw_entry.get("manifest_count_field") or ("task_count" if data_key == "tasks" else "probe_set_task_count"))

        if not script_cfg or not manifest_cfg or not data_cfg:
            ctx.block("benchmark-definitions", f"{name}: script, manifest, and {data_key} are required", ctx.config_path)
            continue

        script = _resolve_plugin_path(ctx, str(script_cfg))
        manifest_path = _resolve_plugin_path(ctx, str(manifest_cfg))
        data_path = _resolve_plugin_path(ctx, str(data_cfg))

        if not script.is_file():
            ctx.block("benchmark-definitions", f"{name}: script is missing", script)
            continue
        if not manifest_path.is_file():
            ctx.block("benchmark-definitions", f"{name}: manifest is missing", manifest_path)
            continue
        if not data_path.is_file():
            ctx.block("benchmark-definitions", f"{name}: {data_key} file is missing", data_path)
            continue

        manifest = _read_json_dict(manifest_path)
        if manifest is None:
            ctx.block("benchmark-definitions", f"{name}: manifest is not a JSON object", manifest_path)
            continue

        expected_count = manifest.get(count_field)
        if not isinstance(expected_count, int):
            ctx.block("benchmark-definitions", f"{name}: manifest missing integer {count_field}", manifest_path)
        else:
            actual_count = _non_empty_line_count(data_path)
            if actual_count != expected_count:
                ctx.block(
                    "benchmark-definitions",
                    f"{name}: {ctx.rel(data_path)} has {actual_count} non-empty lines but "
                    f"{ctx.rel(manifest_path)} {count_field} is {expected_count}",
                    data_path,
                )

        manifest_path_field = raw_entry.get("manifest_path_field")
        if isinstance(manifest_path_field, str) and manifest.get(manifest_path_field):
            manifest_data_path = _resolve_plugin_path(ctx, str(manifest[manifest_path_field]))
            if not _same_resolved_path(manifest_data_path, data_path):
                ctx.block(
                    "benchmark-definitions",
                    f"{name}: manifest {manifest_path_field} points to {ctx.rel(manifest_data_path)}, "
                    f"expected {ctx.rel(data_path)}",
                    manifest_path,
                )

        default_path_attrs = raw_entry.get("default_path_attrs", {})
        if not isinstance(default_path_attrs, dict) or not default_path_attrs:
            continue

        try:
            module = _load_sibling_module(script, f"monolith_benchmark_definition_{re.sub(r'[^A-Za-z0-9_]', '_', name)}")
        except Exception as exc:  # noqa: BLE001 - import failures are actionable static drift.
            ctx.block("benchmark-definitions", f"{name}: could not import script defaults: {exc}", script)
            continue
        if module is None:
            ctx.block("benchmark-definitions", f"{name}: could not import script defaults", script)
            continue

        for attr, expected_key in sorted(default_path_attrs.items()):
            if not hasattr(module, attr):
                ctx.block("benchmark-definitions", f"{name}: script missing {attr}", script)
                continue
            expected_cfg = raw_entry.get(str(expected_key))
            if expected_cfg is None:
                ctx.block("benchmark-definitions", f"{name}: default attr {attr} maps to unknown config key {expected_key}", ctx.config_path)
                continue
            actual_path = _resolve_plugin_path(ctx, getattr(module, attr))
            expected_path = _resolve_plugin_path(ctx, str(expected_cfg))
            if not _same_resolved_path(actual_path, expected_path):
                ctx.block(
                    "benchmark-definitions",
                    f"{name}: {attr} resolves to {ctx.rel(actual_path)}, expected {ctx.rel(expected_path)}",
                    script,
                )
            elif not actual_path.exists():
                ctx.block(
                    "benchmark-definitions",
                    f"{name}: {attr} resolves to missing path {ctx.rel(actual_path)}",
                    script,
                )


def _subprocess_tail(stdout: str | bytes | None, stderr: str | bytes | None, max_lines: int = 8) -> str:
    parts = []
    for part in (stdout, stderr):
        if not part:
            continue
        if isinstance(part, bytes):
            parts.append(part.decode("utf-8", errors="replace"))
        else:
            parts.append(str(part))
    combined = "\n".join(parts)
    lines = [line.strip() for line in combined.splitlines() if line.strip()]
    if not lines:
        return "no output"
    return " | ".join(lines[-max_lines:])


def check_benchmark_contract_tests(ctx: CheckContext) -> None:
    """Run lightweight benchmark contract tests listed in static CI config."""
    config = ctx.config.get("benchmark_contract_tests", {})
    if not config.get("enabled", False):
        return

    tests = config.get("tests", [])
    if not isinstance(tests, list):
        ctx.block("benchmark-contract-tests", "benchmark_contract_tests.tests must be a list", ctx.config_path)
        return

    default_timeout = float(config.get("timeout_seconds", 60))
    for raw_entry in tests:
        if not isinstance(raw_entry, dict):
            ctx.block("benchmark-contract-tests", "Benchmark contract test entry must be an object", ctx.config_path)
            continue

        name = str(raw_entry.get("name") or "<unnamed>")
        script_cfg = raw_entry.get("script")
        if not script_cfg:
            ctx.block("benchmark-contract-tests", f"{name}: script is required", ctx.config_path)
            continue

        script = _resolve_plugin_path(ctx, str(script_cfg))
        if not script.is_file():
            ctx.block("benchmark-contract-tests", f"{name}: script is missing", script)
            continue

        args = raw_entry.get("args", [])
        if not isinstance(args, list):
            ctx.block("benchmark-contract-tests", f"{name}: args must be a list", ctx.config_path)
            continue

        timeout = float(raw_entry.get("timeout_seconds", default_timeout))
        try:
            proc = subprocess.run(
                [sys.executable, str(script), *[str(arg) for arg in args]],
                cwd=ctx.root,
                capture_output=True,
                text=True,
                timeout=timeout,
            )
        except subprocess.TimeoutExpired as exc:
            ctx.block(
                "benchmark-contract-tests",
                f"{name}: contract test timed out after {timeout:g}s: {_subprocess_tail(exc.stdout, exc.stderr)}",
                script,
            )
            continue
        except OSError as exc:
            ctx.block("benchmark-contract-tests", f"{name}: could not run contract test: {exc}", script)
            continue

        if proc.returncode != 0:
            ctx.block(
                "benchmark-contract-tests",
                f"{name}: contract test failed with exit {proc.returncode}: "
                f"{_subprocess_tail(proc.stdout, proc.stderr)}",
                script,
            )


def check_offline_catalog_snapshot(ctx: CheckContext) -> None:
    """Block when the generated source snapshot drifts from action registrations."""
    config = ctx.config.get("offline_catalog_snapshot", {})
    if not config.get("enabled", False):
        return

    generator_cfg = config.get(
        "generator", "Tools/MonolithQuery/generate_monolith_catalog_snapshot.py"
    )
    snapshot_cfg = config.get(
        "snapshot", "Tools/MonolithQuery/Generated/monolith_catalog_snapshot.json"
    )
    generator = ctx.path(str(generator_cfg))
    snapshot = ctx.path(str(snapshot_cfg))
    if not generator.is_file():
        ctx.block("offline-catalog-snapshot", "Offline catalog generator is missing", generator)
        return
    if not snapshot.is_file():
        ctx.block(
            "offline-catalog-snapshot",
            "Generated offline catalog snapshot is missing; run the configured generator before static checks",
            snapshot,
        )
        return

    timeout = float(config.get("timeout_seconds", 60))
    args = [
        sys.executable,
        str(generator),
        "--check",
        "--root",
        str(ctx.root),
        "--out",
        str(snapshot),
    ]
    try:
        proc = subprocess.run(
            args,
            cwd=ctx.root,
            capture_output=True,
            text=True,
            timeout=timeout,
        )
    except subprocess.TimeoutExpired as exc:
        ctx.block(
            "offline-catalog-snapshot",
            f"Offline catalog snapshot check timed out after {timeout:g}s: "
            f"{_subprocess_tail(exc.stdout, exc.stderr)}",
            generator,
        )
        return
    except OSError as exc:
        ctx.block(
            "offline-catalog-snapshot",
            f"Could not run offline catalog snapshot check: {exc}",
            generator,
        )
        return

    if proc.returncode != 0:
        ctx.block(
            "offline-catalog-snapshot",
            f"Generated source snapshot drifted from action registrations (exit {proc.returncode}): "
            f"{_subprocess_tail(proc.stdout, proc.stderr)}. Regenerate with "
            "python Tools/MonolithQuery/generate_monolith_catalog_snapshot.py",
            snapshot,
        )


def check_offline_exe_freshness(ctx: CheckContext) -> None:
    """Block when the shipped offline CLI is stale relative to its tracked source.

    Reuses Scripts/check_offline_exe_fresh.py so the hashed source inputs match
    build.bat exactly. Action-schema/alias fixes only help agents once the shipped
    monolith_query.exe carries them, so a stale exe must fail CI (6A binary-lag).

    Graceful skips keep headless CI green when the exe is a local/release artifact
    that is not present, or cannot be executed on the CI host (e.g. a Windows PE
    binary on a Linux runner): those are advisories, not blockers. Only a present,
    runnable, hash-mismatched exe blocks.
    """
    config = ctx.config.get("offline_exe_freshness", {})
    if not config.get("enabled", False):
        return

    loaded = _load_offline_exe_freshness_module(ctx, "offline-exe-fresh")
    if loaded is None:
        return
    script, fresh = loaded

    missing_sources = [path for path in fresh.SRC_PATHS if not path.exists()]
    if missing_sources:
        ctx.block(
            "offline-exe-fresh",
            "Offline CLI source missing: " + ", ".join(ctx.rel(path) for path in missing_sources),
        )
        return

    if not fresh.EXE_PATH.exists():
        ctx.advisory(
            "offline-exe-fresh",
            f"skipped: {ctx.rel(fresh.EXE_PATH)} not present in this checkout "
            "(offline CLI is a local/release artifact; build via Tools/MonolithQuery/build.bat)",
        )
        return

    src_hash = fresh.compute_source_hash(fresh.SRC_PATHS)
    try:
        exe_hash = fresh.read_exe_source_hash(fresh.EXE_PATH)
    except (ValueError, OSError) as exc:
        ctx.advisory(
            "offline-exe-fresh",
            f"skipped: could not run {ctx.rel(fresh.EXE_PATH)} --version on this host: {exc}",
        )
        return

    if exe_hash != src_hash:
        ctx.block(
            "offline-exe-fresh",
            f"Stale {ctx.rel(fresh.EXE_PATH)}: built from source_hash={exe_hash} but current "
            f"Tools/MonolithQuery source hashes to {src_hash}; rebuild via Tools/MonolithQuery/build.bat",
            fresh.EXE_PATH,
        )


def check_offline_parity_smoke(ctx: CheckContext) -> None:
    """Gate the offline parity score so parity regressions fail CI.

    Runs offline_parity_benchmark.py in a temp directory and checks:
    - offline_parity_score >= min_score (default 0.80)
    - error_rate <= max_error_rate (default 0.10)

    The executable is selected by the same freshness module used by
    check_offline_exe_freshness: monolith_query.current.json chooses the
    authoritative immutable image and the fixed monolith_query.exe name is only
    a legacy fallback when no manifest exists. Gracefully skips (advisory) when
    that selected executable is absent or cannot execute on this host (e.g. a
    Windows PE binary on Linux). Only a present, runnable executable with a
    sub-threshold score blocks.
    """
    config = ctx.config.get("offline_parity_smoke", {})
    if not config.get("enabled", False):
        return

    script = ctx.path(str(config.get("script", "Scripts/offline_parity_benchmark.py")))
    if not script.is_file():
        ctx.block("offline-parity-smoke", "Offline parity benchmark script is missing", script)
        return

    loaded = _load_offline_exe_freshness_module(ctx, "offline-parity-smoke")
    if loaded is None:
        return
    _, fresh = loaded
    exe_path = Path(fresh.EXE_PATH).resolve()
    if not exe_path.exists():
        ctx.advisory(
            "offline-parity-smoke",
            f"skipped: {ctx.rel(exe_path)} not present in this checkout "
            "(offline CLI is a local/release artifact; build via Tools/MonolithQuery/build.bat)",
        )
        return

    min_score = float(config.get("min_score", 0.80))
    max_error_rate = float(config.get("max_error_rate", 0.10))
    timeout = float(config.get("timeout_seconds", 120))

    with tempfile.TemporaryDirectory() as tmp_dir:
        try:
            proc = subprocess.run(
                [
                    sys.executable, str(script), "run",
                    "--label", "ci-smoke",
                    "--output-dir", tmp_dir,
                    "--exe-path", str(exe_path),
                ],
                cwd=ctx.root,
                capture_output=True,
                text=True,
                timeout=timeout,
            )
        except subprocess.TimeoutExpired:
            ctx.advisory(
                "offline-parity-smoke",
                f"skipped: offline parity smoke timed out after {timeout}s",
            )
            return
        except OSError as exc:
            ctx.advisory("offline-parity-smoke", f"skipped: could not launch benchmark: {exc}")
            return

        summary_path = Path(tmp_dir) / "summary.json"
        if not summary_path.exists():
            if proc.returncode != 0:
                stderr_tail = (proc.stderr or "").strip().splitlines()[-3:]
                detail = "; ".join(stderr_tail) if stderr_tail else "no output"
                ctx.advisory(
                    "offline-parity-smoke",
                    f"skipped: benchmark run did not produce summary.json (exit {proc.returncode}): {detail}",
                )
            else:
                ctx.block("offline-parity-smoke", "Benchmark ran successfully but produced no summary.json")
            return

        try:
            summary = json.loads(summary_path.read_text(encoding="utf-8"))
        except (OSError, json.JSONDecodeError) as exc:
            ctx.block("offline-parity-smoke", f"Could not parse benchmark summary.json: {exc}")
            return

        metrics = summary.get("metrics", {})
        score = metrics.get("offline_parity_score")
        error_rate = metrics.get("error_rate")

        if score is None:
            ctx.block("offline-parity-smoke", "Benchmark summary.json missing offline_parity_score")
            return

        if score < min_score:
            ctx.block(
                "offline-parity-smoke",
                f"offline_parity_score {score:.4f} < threshold {min_score:.2f}; "
                f"exe and Python reference have diverged. Run "
                f"'python Scripts/offline_parity_benchmark.py report --summary <path>' for details.",
            )

        if error_rate is not None and error_rate > max_error_rate:
            ctx.block(
                "offline-parity-smoke",
                f"offline error_rate {error_rate:.4f} > threshold {max_error_rate:.2f}; "
                f"too many actions erroring during exe/py comparison.",
            )


def check_analyzer_smoke(ctx: CheckContext) -> None:
    config = ctx.config.get("analyzer_smoke", {})
    if not config.get("enabled", False):
        return

    script = ctx.path(str(config.get("script", "Analyzer/analyze_invocation_logs.py")))
    if not script.is_file():
        ctx.block("analyzer-smoke", "Invocation log analyzer script is missing", script)
        return

    def record(record_id: str, date: str, payload_bytes: int) -> str:
        return json.dumps(
            {
                "format_version": 3,
                "surface": "action",
                "record_id": record_id,
                "trace_id": f"trace-{record_id}",
                "span_id": f"span-{record_id}",
                "start_time": f"{date[:4]}-{date[4:6]}-{date[6:]}T00:00:00+09:00",
                "duration_ms": 1.0,
                "status": "success",
                "call": {
                    "namespace": "console",
                    "action": "search_objects",
                    "arguments": {},
                },
                "agent_signal": {
                    "outcome": "success",
                    "result_bytes": payload_bytes,
                },
                "return": {"success": True},
            },
            separators=(",", ":"),
        )

    def retry_record(record_id: str, date: str, status: str, retry_signature: str) -> str:
        is_success = status == "success"
        agent_signal: dict[str, Any] = {
            "outcome": "success" if is_success else "invalid_params",
            "retry_signature": retry_signature,
            "result_bytes": 100,
        }
        if not is_success:
            agent_signal.update(
                {
                    "error_class": "validation",
                    "error_code": "invalid_params",
                    "improvement_tags": ["schema_confusion"],
                }
            )
        return json.dumps(
            {
                "format_version": 3,
                "surface": "action",
                "record_id": record_id,
                "trace_id": f"trace-{record_id}",
                "span_id": f"span-{record_id}",
                "start_time": f"{date[:4]}-{date[4:6]}-{date[6:]}T00:00:00+09:00",
                "duration_ms": 1.0,
                "status": status,
                "call": {
                    "namespace": "source",
                    "action": "search_source",
                    "arguments": {"query": "FixtureSymbol"},
                },
                "agent_signal": agent_signal,
                "return": {"success": is_success},
            },
            separators=(",", ":"),
        )

    with tempfile.TemporaryDirectory() as log_tmp, tempfile.TemporaryDirectory() as out_tmp:
        log_root = Path(log_tmp)
        old_dir = log_root / "20260101"
        recent_dir = log_root / "20260102"
        old_dir.mkdir()
        recent_dir.mkdir()
        successful_signature = "sha256:successful-repeat"
        failed_signature = "sha256:failed-repeat"
        old_rows = [record("old-large", "20260101", 250_000)]
        old_rows.extend(
            retry_record(f"successful-repeat-{index}", "20260101", "success", successful_signature)
            for index in range(3)
        )
        old_rows.extend(
            retry_record(f"failed-repeat-{index}", "20260101", "error", failed_signature)
            for index in range(2)
        )
        recent_rows = [record("recent-large", "20260102", 260_000)]
        recent_rows.extend(
            retry_record(f"successful-repeat-{index}", "20260102", "success", successful_signature)
            for index in range(3, 6)
        )
        recent_rows.extend(
            retry_record(f"failed-repeat-{index}", "20260102", "error", failed_signature)
            for index in range(2, 5)
        )
        recent_rows.extend(
            retry_record(f"recovered-repeat-{index}", "20260102", "success", failed_signature)
            for index in range(2)
        )
        (old_dir / "action.jsonl").write_text("\n".join(old_rows) + "\n", encoding="utf-8")
        (recent_dir / "action.jsonl").write_text("\n".join(recent_rows) + "\n", encoding="utf-8")

        proc = subprocess.run(
            [
                sys.executable,
                str(script),
                "--log-root",
                str(log_root),
                "--out",
                out_tmp,
                "--format",
                "json",
                "--category",
                "large_result",
                "--category",
                "duplicate_retry",
                "--rank-by-recency",
                "--fix-boundary",
                "20260102",
                "--top",
                "10",
            ],
            cwd=ctx.root,
            capture_output=True,
            text=True,
            timeout=float(config.get("timeout_seconds", 30)),
        )
        if proc.returncode != 0:
            detail = (proc.stderr or proc.stdout or "").strip().splitlines()[-3:]
            ctx.block("analyzer-smoke", f"Analyzer smoke failed: {'; '.join(detail)}", script)
            return

        findings_path = Path(out_tmp) / "findings.json"
        try:
            data = json.loads(findings_path.read_text(encoding="utf-8"))
        except (OSError, json.JSONDecodeError) as exc:
            ctx.block("analyzer-smoke", f"Analyzer did not produce parseable findings.json: {exc}", script)
            return

        findings = data.get("findings", [])
        large = next(
            (
                item
                for item in findings
                if item.get("category") == "large_result"
                and item.get("finding_id") == "large_result:action:console.search_objects"
            ),
            None,
        )
        if not large:
            ctx.block("analyzer-smoke", "Analyzer did not emit the expected large_result finding", script)
            return

        recency = large.get("recency") or {}
        if (
            recency.get("metric") != "calls"
            or recency.get("status") != "still_open"
            or int(recency.get("recent_calls") or 0) < 1
        ):
            ctx.block(
                "analyzer-smoke",
                "large_result finding did not include call-based still_open recency metadata",
                script,
            )

        retries = [item for item in findings if item.get("category") == "duplicate_retry"]
        successful_retry = next(
            (
                item
                for item in retries
                if (item.get("sample") or {}).get("retry_signature") == successful_signature
            ),
            None,
        )
        if successful_retry:
            ctx.block(
                "analyzer-smoke",
                "Analyzer mislabeled repeated successful calls as a duplicate retry",
                script,
            )

        failed_retries = [
            item
            for item in retries
            if (item.get("sample") or {}).get("retry_signature") == failed_signature
        ]
        if len(failed_retries) != 1:
            ctx.block(
                "analyzer-smoke",
                "Analyzer did not emit exactly one finding for repeated failed retry signatures",
                script,
            )
        else:
            failed_retry = failed_retries[0]
            sample = failed_retry.get("sample") or {}
            evidence = failed_retry.get("evidence") or []
            if int(sample.get("count") or 0) != 5:
                ctx.block(
                    "analyzer-smoke",
                    "Duplicate retry count included successful recovery calls",
                    script,
                )
            if not evidence or any(item.get("status") != "error" for item in evidence):
                ctx.block(
                    "analyzer-smoke",
                    "Duplicate retry evidence included a successful call",
                    script,
                )


def run_checks(ctx: CheckContext) -> list[Finding]:
    check_uplugin_and_modules(ctx)
    check_automation_test_names(ctx)
    check_action_registry_duplicates(ctx)
    check_action_registry_hygiene(ctx)
    check_generated_h_include_order(ctx)
    check_text_hygiene(ctx)
    check_repo_hygiene(ctx)
    check_agent_tools(ctx)
    check_secrets(ctx)
    check_workflow_scope(ctx)
    check_proxy_smoke(ctx)
    check_skill_catalog_drift(ctx)
    check_benchmark_definitions(ctx)
    check_benchmark_contract_tests(ctx)
    check_offline_catalog_snapshot(ctx)
    check_offline_exe_freshness(ctx)
    check_offline_parity_smoke(ctx)
    check_analyzer_smoke(ctx)
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
    (root / "Benchmarks/Foo").mkdir(parents=True)
    (root / "Scripts").mkdir(parents=True)
    (root / ".github/workflows/ci.yml").write_text(
        "name: CI\nrun: generate_catalog\nrun: static_check\n",
        encoding="utf-8",
    )
    config = {
        "plugin_descriptor": "auto",
        "uplugin": {
            "required_plugin_references": ["GameplayAbilities"],
            "forbidden_optional_plugin_references": ["Chooser"],
        },
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
        "routing_hygiene": {
            "enabled": True,
            "high_risk_actions": {
                "foo": ["high_risk"],
            },
        },
        "proxy_smoke": {"enabled": False},
        "workflow": {
            "hosted_static_workflow": ".github/workflows/ci.yml",
            "required_ordered_tokens": ["run: generate_catalog", "run: static_check"],
            "forbidden_tokens": ["RunUBT"],
            "duplicate_workflow_paths": [".github/workflows/static-ci.yml"],
        },
        "secrets": {
            "high_confidence_regexes": [r"AKIA[0-9A-Z]{16}"],
            "broad_advisory_regexes": [],
        },
        "benchmark_definitions": {
            "enabled": True,
            "definitions": [
                {
                    "name": "FooBench",
                    "script": "Scripts/foo_benchmark.py",
                    "manifest": "Benchmarks/Foo/manifest.json",
                    "tasks": "Benchmarks/Foo/tasks.jsonl",
                    "manifest_count_field": "task_count",
                    "manifest_path_field": "task_file",
                    "default_path_attrs": {
                        "DEFAULT_TASKS": "tasks",
                        "DEFAULT_MANIFEST": "manifest",
                    },
                },
            ],
        },
        "benchmark_contract_tests": {
            "enabled": True,
            "timeout_seconds": 5,
            "tests": [
                {
                    "name": "FooBench",
                    "script": "Scripts/foo_contract_test.py",
                },
            ],
        },
        "offline_catalog_snapshot": {
            "enabled": True,
            "generator": "Scripts/check_catalog_fixture.py",
            "snapshot": "Tools/MonolithQuery/Generated/catalog.json",
            "timeout_seconds": 5,
        },
    }
    config_path = root / ".github/monolith-static-ci.json"
    config_path.write_text(json.dumps(config), encoding="utf-8")
    (root / "Foo.uplugin").write_text(
        json.dumps(
            {
                "Modules": [{"Name": "Foo", "Type": "Editor"}],
                "Plugins": [{"Name": "GameplayAbilities", "Enabled": True}],
            }
        ),
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
    (root / "Source/Foo/Private/HygieneActions.cpp").write_text(
        "void Register(FRegistry& Registry) {\n"
        "    Registry.RegisterAction(TEXT(\"foo\"), TEXT(\"high_risk\"), TEXT(\"schema ok\"), "
        "FMonolithActionHandler::CreateStatic(&HandleHighRisk), "
        "FParamSchemaBuilder().EnableValidation().Required(TEXT(\"query\"), TEXT(\"string\"), TEXT(\"Query\")).Build());\n"
        "}\n"
        "FMonolithActionResult HandleHighRisk(const TSharedPtr<FJsonObject>& Params) {\n"
        "    return FMonolithActionResult::Success(MakeShared<FJsonObject>());\n"
        "}\n",
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
    (root / ".claude/agents/good_multiline_comment.md").write_text(
        "---\ntools: # multiline list follows\n  - mcp__monolith__multiline\n---\nUses mcp__monolith__multiline.\n",
        encoding="utf-8",
    )
    (root / "Benchmarks/Foo/tasks.jsonl").write_text(
        "{\"id\":\"one\"}\n\n{\"id\":\"two\"}\n",
        encoding="utf-8",
    )
    (root / "Benchmarks/Foo/manifest.json").write_text(
        json.dumps({"task_count": 2, "task_file": "Benchmarks/Foo/tasks.jsonl"}),
        encoding="utf-8",
    )
    (root / "Scripts/foo_benchmark.py").write_text(
        "import pathlib\n"
        "DEFAULT_TASKS = pathlib.Path('Benchmarks/Foo/tasks.jsonl')\n"
        "DEFAULT_MANIFEST = pathlib.Path('Benchmarks/Foo/manifest.json')\n",
        encoding="utf-8",
    )
    (root / "Scripts/foo_contract_test.py").write_text(
        "raise SystemExit(0)\n",
        encoding="utf-8",
    )
    (root / "Tools/MonolithQuery/Generated").mkdir(parents=True)
    (root / "Tools/MonolithQuery/Generated/catalog.json").write_text(
        "current\n",
        encoding="utf-8",
    )
    (root / "Scripts/check_catalog_fixture.py").write_text(
        "import argparse\n"
        "from pathlib import Path\n"
        "parser = argparse.ArgumentParser()\n"
        "parser.add_argument('--check', action='store_true')\n"
        "parser.add_argument('--root', type=Path, required=True)\n"
        "parser.add_argument('--out', type=Path, required=True)\n"
        "args = parser.parse_args()\n"
        "current = args.check and args.root.joinpath('Source').is_dir() "
        "and args.out.read_text(encoding='utf-8') == 'current\\n'\n"
        "if not current:\n"
        "    print('catalog snapshot drift')\n"
        "raise SystemExit(0 if current else 1)\n",
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
            "forbidden optional plugin reference",
            "uplugin-dependency",
            lambda root: (root / "Foo.uplugin").write_text(
                json.dumps(
                    {
                        "Modules": [{"Name": "Foo", "Type": "Editor"}],
                        "Plugins": [
                            {"Name": "GameplayAbilities", "Enabled": True},
                            {"Name": "chooser", "Enabled": True, "Optional": True},
                        ],
                    }
                ),
                encoding="utf-8",
            ),
        ),
        (
            "required plugin reference made optional",
            "uplugin-dependency",
            lambda root: (root / "Foo.uplugin").write_text(
                json.dumps(
                    {
                        "Modules": [{"Name": "Foo", "Type": "Editor"}],
                        "Plugins": [
                            {"Name": "GameplayAbilities", "Enabled": True, "Optional": True}
                        ],
                    }
                ),
                encoding="utf-8",
            ),
        ),
        (
            "duplicate required plugin references",
            "uplugin-dependency",
            lambda root: (root / "Foo.uplugin").write_text(
                json.dumps(
                    {
                        "Modules": [{"Name": "Foo", "Type": "Editor"}],
                        "Plugins": [
                            {"Name": "GameplayAbilities", "Enabled": True},
                            {
                                "Name": "gameplayabilities",
                                "Enabled": False,
                                "Optional": True,
                            },
                        ],
                    }
                ),
                encoding="utf-8",
            ),
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
            "duplicate templated project action registration",
            "action-registry",
            lambda root: (root / "Source/Foo/Private/DuplicateProjectActions.cpp").write_text(
                "class FProjectDuplicateAction {\n"
                "public:\n"
                "    static FString GetName() { return TEXT(\"bar\"); }\n"
                "};\n"
                "void Register(FRegistry& Registry) {\n"
                "    RegisterProjectAction<FProjectDuplicateAction>(Registry);\n"
                "    Registry.RegisterAction(TEXT(\"project\"), TEXT(\"bar\"), TEXT(\"two\"), Handler);\n"
                "}\n",
                encoding="utf-8",
            ),
        ),
        (
            "high-risk action missing schema validation",
            "action-registry-hygiene",
            lambda root: (root / "Source/Foo/Private/HygieneActions.cpp").write_text(
                "void Register(FRegistry& Registry) {\n"
                "    Registry.RegisterAction(TEXT(\"foo\"), TEXT(\"high_risk\"), TEXT(\"missing schema\"), Handler);\n"
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
        (
            "required hosted workflow order",
            "workflow",
            lambda root: (root / ".github/workflows/ci.yml").write_text(
                "name: CI\nrun: static_check\nrun: generate_catalog\n",
                encoding="utf-8",
            ),
        ),
        (
            "benchmark task count drift",
            "benchmark-definitions",
            lambda root: (root / "Benchmarks/Foo/manifest.json").write_text(
                json.dumps({"task_count": 99, "task_file": "Benchmarks/Foo/tasks.jsonl"}),
                encoding="utf-8",
            ),
        ),
        (
            "benchmark default path drift",
            "benchmark-definitions",
            lambda root: (root / "Scripts/foo_benchmark.py").write_text(
                "import pathlib\n"
                "DEFAULT_TASKS = pathlib.Path('Benchmarks/Other/tasks.jsonl')\n"
                "DEFAULT_MANIFEST = pathlib.Path('Benchmarks/Foo/manifest.json')\n",
                encoding="utf-8",
            ),
        ),
        (
            "benchmark contract failure",
            "benchmark-contract-tests",
            lambda root: (root / "Scripts/foo_contract_test.py").write_text(
                "print('contract failed')\nraise SystemExit(7)\n",
                encoding="utf-8",
            ),
        ),
        (
            "offline catalog snapshot drift",
            "offline-catalog-snapshot",
            lambda root: (root / "Tools/MonolithQuery/Generated/catalog.json").write_text(
                "stale\n",
                encoding="utf-8",
            ),
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
