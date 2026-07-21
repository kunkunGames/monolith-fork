#!/usr/bin/env python3
"""Compute checkout-independent source-generation hashes for native tools.

The source-generation contract is intentionally different from an artifact SHA-256:

* the ASCII build-contract bytes are hashed first;
* each ordered text-source input is read as bytes;
* CRLF and lone CR are canonicalized to LF before those bytes are hashed; and
* the first 16 lowercase hexadecimal characters identify the build generation.

Only source-generation inputs use newline canonicalization. Executables, catalogs,
manifests, and other published artifacts must continue to use SHA-256 over their exact
raw bytes.
"""

from __future__ import annotations

import argparse
from dataclasses import dataclass
import hashlib
from pathlib import Path
import re
from typing import Iterable


HASH_PREFIX_LENGTH = 16
SOURCE_HASH_RE = re.compile(r"^[0-9a-f]{16}$")


@dataclass(frozen=True)
class SourceGenerationSpec:
    build_contract: bytes
    source_root: tuple[str, ...]
    inputs: tuple[str, ...]


SOURCE_GENERATION_SPECS = {
    "query": SourceGenerationSpec(
        build_contract=b"monolith-query-build-v2-mt-o2-fts5-brepro",
        source_root=("Tools", "MonolithQuery"),
        inputs=(
            "monolith_query.cpp",
            "monolith_query_tool_log.h",
            "monolith_query_crg.h",
            "monolith_query_review_ranges.h",
            "monolith_query_bridge.h",
            "monolith_query_help.h",
            "../../Source/MonolithSource/Public/MonolithSourceConsoleSchema.h",
            "../../Source/MonolithSource/Public/MonolithSourceGraphSearchSchema.h",
            "../../Source/MonolithSource/Public/MonolithSourceSymbolSearchSchema.h",
            "ThirdParty/sqlite3.c",
            "ThirdParty/sqlite3.h",
            "../MonolithProxy/ThirdParty/nlohmann/json.hpp",
        ),
    ),
    "proxy": SourceGenerationSpec(
        build_contract=(
            b"monolith-proxy-build-v3-mt-o2-brepro-msvc-deterministic-pathmap"
        ),
        source_root=("Tools", "MonolithProxy"),
        inputs=(
            "monolith_proxy.cpp",
            "monolith_proxy_offline.cpp",
            "monolith_proxy_offline.h",
            "monolith_proxy_help.h",
            "monolith_proxy_tool_log.h",
            "ThirdParty/nlohmann/json.hpp",
        ),
    ),
}


def canonicalize_text_source(data: bytes) -> bytes:
    """Return the canonical byte representation used only for text source inputs."""
    return data.replace(b"\r\n", b"\n").replace(b"\r", b"\n")


def compute_source_generation_hash(
    build_contract: bytes,
    source_paths: Iterable[Path],
) -> str:
    """Hash one ordered source set according to the source-generation contract."""
    digest = hashlib.sha256()
    digest.update(build_contract)
    for source_path in source_paths:
        digest.update(canonicalize_text_source(Path(source_path).read_bytes()))
    return digest.hexdigest()[:HASH_PREFIX_LENGTH]


def source_paths_for_tool(plugin_root: Path, tool: str) -> tuple[Path, ...]:
    """Resolve the ordered source paths declared for ``tool``."""
    try:
        spec = SOURCE_GENERATION_SPECS[tool]
    except KeyError as error:
        raise ValueError(f"unknown native tool: {tool}") from error
    source_root = Path(plugin_root).resolve().joinpath(*spec.source_root)
    return tuple((source_root / value).resolve() for value in spec.inputs)


def compute_tool_source_generation_hash(plugin_root: Path, tool: str) -> str:
    """Compute the canonical generation hash for a declared native tool."""
    try:
        spec = SOURCE_GENERATION_SPECS[tool]
    except KeyError as error:
        raise ValueError(f"unknown native tool: {tool}") from error
    return compute_source_generation_hash(
        spec.build_contract,
        source_paths_for_tool(plugin_root, tool),
    )


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--plugin-root", required=True, type=Path)
    parser.add_argument("--tool", required=True, choices=tuple(SOURCE_GENERATION_SPECS))
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    try:
        source_hash = compute_tool_source_generation_hash(args.plugin_root, args.tool)
    except OSError as error:
        raise SystemExit(f"could not read {args.tool} source input: {error}") from error
    if not SOURCE_HASH_RE.fullmatch(source_hash):
        raise SystemExit(f"invalid computed source-generation hash: {source_hash!r}")
    print(source_hash)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
