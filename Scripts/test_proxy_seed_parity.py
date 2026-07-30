#!/usr/bin/env python3
"""Verify Python/native offline proxy seed parity.

The proxies must advertise the same dispatcher tools when the editor is down
and no cache exists. This parser reads source rather than importing the proxy,
so the test has no network, logging, or process-start side effects.
"""

from __future__ import annotations

import ast
import re
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
PYTHON_PROXY = ROOT / "Scripts" / "monolith_proxy.py"
NATIVE_PROXY = ROOT / "Tools" / "MonolithProxy" / "monolith_proxy.cpp"
REQUIRED_DISPATCHERS = {"dataflow_query"}


def read_python_seed() -> list[str]:
    tree = ast.parse(PYTHON_PROXY.read_text(encoding="utf-8"), PYTHON_PROXY.name)
    for node in tree.body:
        if not isinstance(node, ast.Assign):
            continue
        if any(
            isinstance(target, ast.Name) and target.id == "CORE_QUERY_TOOLS"
            for target in node.targets
        ):
            value = ast.literal_eval(node.value)
            if not isinstance(value, list) or not all(
                isinstance(item, str) for item in value
            ):
                raise AssertionError("Python CORE_QUERY_TOOLS must be a string list")
            return value
    raise AssertionError("Python CORE_QUERY_TOOLS assignment not found")


def read_native_seed() -> list[str]:
    source = NATIVE_PROXY.read_text(encoding="utf-8")
    match = re.search(
        r"CORE_QUERY_TOOLS\s*=\s*\{(?P<body>.*?)\};",
        source,
        flags=re.DOTALL,
    )
    if not match:
        raise AssertionError("Native CORE_QUERY_TOOLS initializer not found")
    return re.findall(r'"([^"]+)"', match.group("body"))


def validate_unique(label: str, values: list[str]) -> None:
    duplicates = sorted({value for value in values if values.count(value) > 1})
    if duplicates:
        raise AssertionError(f"{label} seed has duplicates: {duplicates}")


def main() -> int:
    python_seed = read_python_seed()
    native_seed = read_native_seed()
    validate_unique("Python", python_seed)
    validate_unique("Native", native_seed)

    if python_seed != native_seed:
        raise AssertionError(
            "Proxy seed order/content differs:\n"
            f"Python: {python_seed}\n"
            f"Native: {native_seed}"
        )

    missing = sorted(REQUIRED_DISPATCHERS.difference(python_seed))
    if missing:
        raise AssertionError(f"Required offline dispatchers are missing: {missing}")

    print(
        "PASS: Python/native offline proxy seeds match "
        f"({len(python_seed)} dispatchers), including dataflow_query"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
