#!/usr/bin/env python3
"""Lint agent .md files for frontmatter tool-allowlist drift.

Walks every agent file in `.claude/agents/` and verifies that any
`mcp__monolith__<name>` dispatcher referenced in the prompt body is
also declared in the YAML frontmatter `tools:` line. ToolSearch's
`select:` operates over the agent's surfaced deferred-tool universe,
so anything absent from `tools:` is invisible to `select:` -- which
is exactly the F10 drift this script prevents.

Pure stdlib, Python 3.10+. Exit 0 on clean, 1 on violations.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

# Project root is one parent up from Scripts/.
SCRIPT_DIR = Path(__file__).resolve().parent
PROJECT_ROOT = SCRIPT_DIR.parent
AGENTS_DIR = PROJECT_ROOT / ".claude" / "agents"

DISPATCHER_RE = re.compile(r"mcp__monolith__[A-Za-z_][A-Za-z0-9_]*")


def parse_tools_line(text: str) -> tuple[set[str], int] | None:
    """Extract the `tools:` frontmatter set and its line number.

    Returns (tool_set, line_number_1based) or None if no `tools:`
    line was found inside the leading `---` frontmatter block.
    """
    def clean_tool_value(raw: str) -> str:
        return raw.split("#", 1)[0].strip().strip("'\"")

    lines = text.splitlines()
    if not lines or lines[0].strip() != "---":
        return None

    tools: set[str] = set()
    found_tools = False
    tools_lineno = 0
    in_tools_list = False

    for idx in range(1, len(lines)):
        line = lines[idx]
        stripped = line.strip()

        if stripped == "---":
            return (tools, tools_lineno) if found_tools else None

        if in_tools_list:
            if stripped.startswith("- "):
                tools.add(clean_tool_value(stripped[2:]))
                continue
            if line and not line[0].isspace() and not stripped.startswith("#"):
                in_tools_list = False

        if stripped.startswith("tools:"):
            found_tools = True
            tools_lineno = idx + 1
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


def find_dispatcher_refs(text: str, frontmatter_end: int) -> list[tuple[str, int]]:
    """Return (dispatcher_name, line_number_1based) for each prompt-body match.

    `frontmatter_end` is the 1-based line number of the closing `---`;
    we only scan AFTER that to avoid double-counting the tools: line.
    """
    refs: list[tuple[str, int]] = []
    for lineno, line in enumerate(text.splitlines(), start=1):
        if lineno <= frontmatter_end:
            continue
        for match in DISPATCHER_RE.finditer(line):
            refs.append((match.group(0), lineno))
    return refs


def find_frontmatter_end(text: str) -> int:
    """Return 1-based line number of the closing `---` (or 0 if absent)."""
    lines = text.splitlines()
    if not lines or lines[0].strip() != "---":
        return 0
    for idx in range(1, len(lines)):
        if lines[idx].strip() == "---":
            return idx + 1
    return 0


def lint_agent(path: Path) -> list[tuple[Path, str, int]]:
    """Return list of (path, missing_tool, line_number) violations."""
    text = path.read_text(encoding="utf-8")
    parsed = parse_tools_line(text)
    if parsed is None:
        # No tools: line -- can't lint this agent. Skip silently;
        # missing-frontmatter is a separate concern.
        return []
    tools, _tools_lineno = parsed
    fm_end = find_frontmatter_end(text)
    refs = find_dispatcher_refs(text, fm_end)

    violations: list[tuple[Path, str, int]] = []
    seen: set[tuple[str, int]] = set()
    for name, lineno in refs:
        if name in tools:
            continue
        key = (name, lineno)
        if key in seen:
            continue
        seen.add(key)
        violations.append((path, name, lineno))
    return violations


def run_selftest() -> int:
    """Exercise parser cases that the repository may not contain today."""
    cases = [
        (
            "bracket list with inline comment",
            "---\ntools: [mcp__monolith__bracket, mcp__monolith__comment] # inline\n---\n",
            {"mcp__monolith__bracket", "mcp__monolith__comment"},
            2,
        ),
        (
            "csv list with inline comment",
            "---\ntools: mcp__monolith__alpha, mcp__monolith__beta # inline\n---\n",
            {"mcp__monolith__alpha", "mcp__monolith__beta"},
            2,
        ),
        (
            "multiline yaml list",
            "---\ntools:\n  - mcp__monolith__first\n  - mcp__monolith__second\n---\n",
            {"mcp__monolith__first", "mcp__monolith__second"},
            2,
        ),
    ]
    for label, text, expected_tools, expected_line in cases:
        parsed = parse_tools_line(text)
        if parsed != (expected_tools, expected_line):
            print(f"selftest failed: {label}: got {parsed!r}", file=sys.stderr)
            return 1

    print("selftest passed")
    return 0


def main() -> int:
    if not AGENTS_DIR.is_dir():
        print(f"ERROR: agents directory not found: {AGENTS_DIR}\nNote: This directory is an external prerequisite and may not be tracked in the repository.", file=sys.stderr)
        return 2

    agent_files = sorted(AGENTS_DIR.glob("*.md"))
    all_violations: list[tuple[Path, str, int]] = []
    for path in agent_files:
        all_violations.extend(lint_agent(path))

    n = len(agent_files)
    if not all_violations:
        print(f"OK -- {n} agents linted, 0 violations.")
        return 0

    print(f"FAIL -- {n} agents linted, {len(all_violations)} violation(s):")
    for path, name, lineno in all_violations:
        print(f"  {path.name}: missing '{name}' (referenced at line {lineno})")
    return 1


if __name__ == "__main__":
    if "--selftest" in sys.argv[1:]:
        sys.exit(run_selftest())
    sys.exit(main())
