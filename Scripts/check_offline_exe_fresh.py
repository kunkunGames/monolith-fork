#!/usr/bin/env python3
"""
check_offline_exe_fresh.py -- staleness guard for the offline Monolith CLI.

The native Query image is built from Tools/MonolithQuery/monolith_query.cpp,
header-only helpers, SQLite, and nlohmann/json by Tools/MonolithQuery/build.bat.
That build injects the SHA256-first-16 of its build-contract bytes plus ordered text
source inputs after CRLF/lone-CR canonicalization to LF as a /DSOURCE_HASH define,
which the exe echoes back under --version as "source_hash".
When present, monolith_query.current.json selects the authoritative immutable image;
the fixed monolith_query.exe is only the legacy compatibility fallback.

This script recomputes that same hash from the on-disk source, asks the exe what it was
built from, and exits non-zero if they disagree -- i.e. the shipped exe is stale relative
to the tracked source and must be rebuilt.

An exe built before the hash-injection landed reports source_hash="dev"; that will never
match a real hex hash, so such an exe is correctly reported STALE.

Usage (run from the Monolith plugin root):
    python Scripts/check_offline_exe_fresh.py

Exit codes:
    0  exe source_hash matches the current source (fresh)
    1  mismatch (STALE -- rebuild via Tools/MonolithQuery/build.bat)
    4  preflight failure (source or exe missing, or --version unparseable)

stdlib-only. Do not add third-party deps.
"""

import json
import subprocess
import sys
from pathlib import Path

from source_generation_hash import (
    HASH_PREFIX_LENGTH,
    SOURCE_GENERATION_SPECS,
    compute_source_generation_hash,
    source_paths_for_tool,
)

# Script lives in <MonolithRoot>/Scripts/, so the plugin root is parent.parent.
SCRIPT_DIR = Path(__file__).resolve().parent
MONO_ROOT = SCRIPT_DIR.parent
SRC_PATHS = list(source_paths_for_tool(MONO_ROOT, "query"))
BINARY_ROOT = MONO_ROOT / "Binaries"
MANIFEST_PATH = BINARY_ROOT / "monolith_query.current.json"
BUILD_CONTRACT = SOURCE_GENERATION_SPECS["query"].build_contract


def resolve_authoritative_exe_path():
    """Resolve the manifest-selected image, with fixed-name legacy fallback."""
    if not MANIFEST_PATH.is_file():
        return BINARY_ROOT / "monolith_query.exe"
    try:
        manifest = json.loads(MANIFEST_PATH.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as exc:
        raise ValueError("invalid Query current manifest: {0}".format(exc))
    source_hash = manifest.get("source_hash")
    file_name = manifest.get("file")
    expected = "monolith_query-{0}.exe".format(source_hash)
    if (
        manifest.get("tool") != "monolith_query"
        or manifest.get("runtime") != "native-cpp"
        or not isinstance(source_hash, str)
        or len(source_hash) != 16
        or any(ch not in "0123456789abcdef" for ch in source_hash)
        or file_name != expected
        or Path(file_name).name != file_name
    ):
        raise ValueError("Query current manifest has an invalid identity or file binding")
    return BINARY_ROOT / file_name


try:
    EXE_PATH = resolve_authoritative_exe_path()
except ValueError:
    # Preserve importability for ci_static_checks.py so it can report the manifest
    # problem through the existing missing/unrunnable finding path.
    EXE_PATH = MANIFEST_PATH

# Must match the shared build/check/release source-generation contract.
HASH_PREFIX_LEN = HASH_PREFIX_LENGTH


def compute_source_hash(paths):
    """Mirror the canonical build/check/release source generation exactly."""
    return compute_source_generation_hash(BUILD_CONTRACT, paths)


def read_exe_source_hash(exe_path):
    """Run `<exe> --version`, parse JSON, return its source_hash string (or None)."""
    # Bound the call so a hung/corrupt exe fails fast instead of wedging CI. Convert
    # the TimeoutExpired (not an OSError/ValueError) into ValueError so every caller
    # — this module's main() and ci_static_checks' (ValueError, OSError) catch —
    # degrades to a graceful advisory rather than crashing the run.
    try:
        proc = subprocess.run(
            [str(exe_path), "--version"],
            capture_output=True,
            encoding="utf-8",
            errors="replace",
            timeout=30,
        )
    except subprocess.TimeoutExpired as exc:
        raise ValueError("exe --version timed out after 30s: {0!r}".format(exc))
    if proc.returncode != 0:
        raise ValueError(
            "exe --version exited {0}: {1!r}".format(
                proc.returncode, (proc.stderr or "").strip()[:300]
            )
        )
    try:
        data = json.loads(proc.stdout)
    except Exception as exc:  # noqa: BLE001 - surface any parse failure
        raise ValueError(
            "could not parse --version JSON: {0}; raw[:300]={1!r}".format(
                exc, (proc.stdout or "")[:300]
            )
        )
    if data.get("tool") != "monolith_query" or data.get("runtime") != "native-cpp":
        raise ValueError("exe --version has the wrong tool/runtime identity")
    return data.get("source_hash")


def main():
    for src_path in SRC_PATHS:
        if not src_path.exists():
            print("FATAL: source not found at {0}".format(src_path))
            return 4
    if not EXE_PATH.exists():
        print("FATAL: exe not found at {0}".format(EXE_PATH))
        return 4

    src_hash = compute_source_hash(SRC_PATHS)

    try:
        exe_hash = read_exe_source_hash(EXE_PATH)
    except ValueError as exc:
        print("FATAL: {0}".format(exc))
        return 4

    print("Offline exe freshness check")
    print("  sources =")
    for src_path in SRC_PATHS:
        print("    - {0}".format(src_path))
    print("  exe    = {0}".format(EXE_PATH))
    print("  sources SHA256[:{0}] = {1}".format(HASH_PREFIX_LEN, src_hash))
    print("  exe   source_hash  = {0}".format(exe_hash))

    if exe_hash == src_hash:
        print("\nRESULT: FRESH -- exe matches current source.")
        return 0

    print("\nRESULT: STALE -- exe was built from different (or pre-hash) source.")
    print("  exe is STALE - rebuild via Tools/MonolithQuery/build.bat")
    return 1


if __name__ == "__main__":
    sys.exit(main())
