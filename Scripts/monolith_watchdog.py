#!/usr/bin/env python3
"""Console wrapper for the Monolith MCP watchdog.

Compile this script to Binaries/monolith_watchdog.exe when Windows Task Manager
needs a recognizable watchdog process name. The wrapper stays alive while
watch_mcp.ps1 runs and forwards the watchdog exit code.
"""

from __future__ import annotations

import argparse
import ctypes
import os
import subprocess
import sys
from pathlib import Path


APP_NAME = "monolith_watchdog"


def set_console_title(project_root: Path) -> None:
    if os.name != "nt":
        return
    title = f"Monolith Watchdog - {project_root.name}"
    try:
        ctypes.windll.kernel32.SetConsoleTitleW(title)
    except Exception:
        pass


def find_windows_powershell() -> str:
    system_root = os.environ.get("SystemRoot") or r"C:\Windows"
    powershell = Path(system_root) / "System32" / "WindowsPowerShell" / "v1.0" / "powershell.exe"
    if powershell.is_file():
        return str(powershell)
    return "powershell.exe"


def resolve_project_root(raw_path: str) -> Path:
    root = Path(raw_path).expanduser()
    if not root.is_absolute():
        raise ValueError(f"project root must be absolute: {raw_path}")
    root = root.resolve()
    if not root.is_dir():
        raise ValueError(f"project root does not exist: {root}")
    if not any(root.glob("*.uproject")):
        raise ValueError(f"project root has no .uproject file: {root}")
    return root


def build_watchdog_command(project_root: Path, extra_args: list[str]) -> list[str]:
    watch_script = project_root / "Plugins" / "Monolith" / "Scripts" / "watch_mcp.ps1"
    if not watch_script.is_file():
        raise ValueError(f"watch_mcp.ps1 not found: {watch_script}")
    if any(arg.lower() == "-projectroot" for arg in extra_args):
        raise ValueError("do not pass -ProjectRoot in extra arguments; it is derived from the project root argument")

    return [
        find_windows_powershell(),
        "-NoProfile",
        "-ExecutionPolicy",
        "Bypass",
        "-File",
        str(watch_script),
        "-ProjectRoot",
        str(project_root),
        *extra_args,
    ]


def parse_args(argv: list[str]) -> argparse.Namespace:
    dry_run = False
    filtered_argv: list[str] = []
    for arg in argv:
        if arg == "--dry-run":
            dry_run = True
        else:
            filtered_argv.append(arg)

    parser = argparse.ArgumentParser(
        prog=APP_NAME,
        description="Run Plugins/Monolith/Scripts/watch_mcp.ps1 behind a recognizable monolith_watchdog.exe process.",
    )
    parser.add_argument("project_root", help=r"Absolute project root path, for example D:\P4\speed")
    parser.add_argument(
        "extra_args",
        nargs=argparse.REMAINDER,
        help="Optional watch_mcp.ps1 args. Use -- before PowerShell-style switches, e.g. -- -ProbeOnly.",
    )
    parsed = parser.parse_args(filtered_argv)
    parsed.dry_run = dry_run
    return parsed


def main(argv: list[str] | None = None) -> int:
    args = parse_args(sys.argv[1:] if argv is None else argv)
    extra_args = list(args.extra_args)
    if extra_args and extra_args[0] == "--":
        extra_args = extra_args[1:]

    try:
        project_root = resolve_project_root(args.project_root)
        command = build_watchdog_command(project_root, extra_args)
    except ValueError as exc:
        print(f"{APP_NAME}: {exc}", file=sys.stderr, flush=True)
        return 2

    set_console_title(project_root)
    print(f"{APP_NAME}: projectRoot={project_root}", flush=True)
    print(f"{APP_NAME}: command={' '.join(command)}", flush=True)

    if args.dry_run:
        return 0

    try:
        return subprocess.call(command, cwd=str(project_root))
    except KeyboardInterrupt:
        return 130
    except OSError as exc:
        print(f"{APP_NAME}: failed to start PowerShell: {exc}", file=sys.stderr, flush=True)
        return 3


if __name__ == "__main__":
    raise SystemExit(main())
