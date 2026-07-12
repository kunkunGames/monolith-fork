#!/usr/bin/env python3
"""Publish and validate the immutable native Query + catalog bundle."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import stat
import subprocess
import sys
import tempfile
import uuid


SCHEMA_VERSION = 1
MANIFEST_NAME = "monolith_query.current.json"
MANIFEST_FIELDS = (
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
)
VERSION_FIELDS = (
    "tool",
    "runtime",
    "plugin_version",
    "parity_spec_rev",
    "source_hash",
)
SOURCE_HASH_RE = re.compile(r"^[0-9a-f]{16}$")
CATALOG_HASH_RE = re.compile(r"^[0-9a-f]{64}$")
SHA256_RE = re.compile(r"^[0-9a-f]{64}$")
QUERY_FILE_RE = re.compile(r"^monolith_query-([0-9a-f]{16})\.exe$")
CATALOG_FILE_RE = re.compile(r"^monolith_catalog-([0-9a-f]{64})\.json$")
MAX_VERSION_OUTPUT = 64 * 1024
MAX_MANIFEST_BYTES = 64 * 1024
MAX_CATALOG_BYTES = 64 * 1024 * 1024


class BundleError(RuntimeError):
    """Raised when publication or validation would violate the bundle contract."""


def _reject_duplicate_keys(pairs: list[tuple[str, object]]) -> dict[str, object]:
    result: dict[str, object] = {}
    for key, value in pairs:
        if key in result:
            raise BundleError(f"duplicate JSON key: {key}")
        result[key] = value
    return result


def _reject_non_finite(value: str) -> object:
    raise BundleError(f"non-finite JSON number is forbidden: {value}")


def _load_strict_json_bytes(data: bytes, label: str) -> dict[str, object]:
    try:
        text = data.decode("utf-8", errors="strict")
        value = json.loads(
            text,
            object_pairs_hook=_reject_duplicate_keys,
            parse_constant=_reject_non_finite,
        )
    except (UnicodeDecodeError, json.JSONDecodeError, BundleError) as error:
        raise BundleError(f"{label} is not strict UTF-8 JSON: {error}") from error
    if not isinstance(value, dict):
        raise BundleError(f"{label} must contain one JSON object")
    return value


def _regular_non_reparse_file(path: Path, label: str) -> None:
    try:
        info = path.lstat()
    except OSError as error:
        raise BundleError(f"{label} is not readable: {path}: {error}") from error
    if not stat.S_ISREG(info.st_mode):
        raise BundleError(f"{label} must be a regular file: {path}")
    if path.is_symlink() or (
        getattr(info, "st_file_attributes", 0)
        & getattr(stat, "FILE_ATTRIBUTE_REPARSE_POINT", 0)
    ):
        raise BundleError(f"{label} must not be a reparse point: {path}")


def _regular_non_reparse_directory(path: Path, label: str) -> Path:
    try:
        info = path.lstat()
    except OSError as error:
        raise BundleError(f"{label} is not readable: {path}: {error}") from error
    if not stat.S_ISDIR(info.st_mode):
        raise BundleError(f"{label} must be a directory: {path}")
    if path.is_symlink() or (
        getattr(info, "st_file_attributes", 0)
        & getattr(stat, "FILE_ATTRIBUTE_REPARSE_POINT", 0)
    ):
        raise BundleError(f"{label} must not be a reparse point: {path}")
    return path.resolve(strict=True)


def _read_bounded(path: Path, maximum: int, label: str) -> bytes:
    _regular_non_reparse_file(path, label)
    size = path.stat().st_size
    if size <= 0 or size > maximum:
        raise BundleError(f"{label} has invalid size {size}: {path}")
    return path.read_bytes()


def _sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def _sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _exact_string(value: object, name: str) -> str:
    if not isinstance(value, str) or not value:
        raise BundleError(f"{name} must be a non-empty string")
    return value


def _run_bounded_version(executable: Path) -> dict[str, object]:
    _regular_non_reparse_file(executable, "Query executable")
    creation_flags = getattr(subprocess, "CREATE_NO_WINDOW", 0) if os.name == "nt" else 0
    with tempfile.TemporaryFile() as stdout_file, tempfile.TemporaryFile() as stderr_file:
        try:
            process = subprocess.Popen(
                [str(executable), "--version"],
                stdin=subprocess.DEVNULL,
                stdout=stdout_file,
                stderr=stderr_file,
                creationflags=creation_flags,
            )
        except OSError as error:
            raise BundleError(f"Query --version could not start: {error}") from error
        try:
            return_code = process.wait(timeout=10)
        except subprocess.TimeoutExpired as error:
            process.kill()
            process.wait()
            raise BundleError("Query --version exceeded the 10 second timeout") from error

        stdout_size = stdout_file.tell()
        stderr_size = stderr_file.tell()
        if stdout_size <= 0 or stdout_size > MAX_VERSION_OUTPUT:
            raise BundleError(f"Query --version stdout has invalid size {stdout_size}")
        if stderr_size > MAX_VERSION_OUTPUT:
            raise BundleError(f"Query --version stderr is oversized ({stderr_size} bytes)")
        stdout_file.seek(0)
        stderr_file.seek(0)
        stdout = stdout_file.read()
        stderr = stderr_file.read()
        if return_code != 0:
            detail = stderr.decode("utf-8", errors="replace").strip()
            raise BundleError(f"Query --version exited {return_code}: {detail}")
        if stderr:
            raise BundleError("Query --version wrote unexpected stderr output")

    version = _load_strict_json_bytes(stdout, "Query --version output")
    if tuple(version.keys()) != VERSION_FIELDS and set(version) != set(VERSION_FIELDS):
        raise BundleError(
            "Query --version must contain exactly tool, runtime, plugin_version, "
            "parity_spec_rev, and source_hash"
        )
    if version.get("tool") != "monolith_query" or version.get("runtime") != "native-cpp":
        raise BundleError("Query --version has the wrong tool/runtime identity")
    source_hash = _exact_string(version.get("source_hash"), "version.source_hash")
    if not SOURCE_HASH_RE.fullmatch(source_hash):
        raise BundleError("version.source_hash must be 16 lowercase hexadecimal characters")
    _exact_string(version.get("plugin_version"), "version.plugin_version")
    _exact_string(version.get("parity_spec_rev"), "version.parity_spec_rev")
    return version


def _catalog_semantic_hash(catalog: dict[str, object]) -> str:
    actions = catalog.get("actions")
    if not isinstance(actions, list):
        raise BundleError("catalog.actions must be an array")
    if catalog.get("action_count") != len(actions):
        raise BundleError("catalog.action_count must equal the actions array length")
    normalized: list[dict[str, object]] = []
    for index, action in enumerate(actions):
        if not isinstance(action, dict):
            raise BundleError(f"catalog.actions[{index}] must be an object")
        if not isinstance(action.get("namespace"), str) or not isinstance(action.get("action"), str):
            raise BundleError(f"catalog.actions[{index}] is missing namespace/action identity")
        normalized.append({key: value for key, value in action.items() if key != "source_line"})
    normalized.sort(key=lambda row: (str(row["namespace"]), str(row["action"])))
    digest = hashlib.sha256()
    for action in normalized:
        try:
            encoded = json.dumps(
                action,
                ensure_ascii=False,
                sort_keys=True,
                separators=(",", ":"),
                allow_nan=False,
            ).encode("utf-8")
        except (TypeError, ValueError) as error:
            raise BundleError(f"catalog action is not strict JSON: {error}") from error
        digest.update(encoded)
        digest.update(b"\0")
    return digest.hexdigest()


def _catalog_contract(catalog: dict[str, object]) -> dict[str, object]:
    """Mirror the generator's stable drift contract for nonsemantic provenance churn."""
    contract_keys = (
        "schema_version",
        "source",
        "source_root",
        "source_hash",
        "source_hash_kind",
        "action_count",
        "proof_anchors",
    )
    actions = catalog.get("actions")
    assert isinstance(actions, list)
    contract = {key: catalog.get(key) for key in contract_keys}
    contract["actions"] = [
        {key: value for key, value in action.items() if key != "source_line"}
        for action in actions
        if isinstance(action, dict)
    ]
    return contract


def _validate_catalog_bytes(data: bytes) -> tuple[dict[str, object], str]:
    if not data or len(data) > MAX_CATALOG_BYTES:
        raise BundleError(f"catalog has invalid size {len(data)}")
    catalog = _load_strict_json_bytes(data, "catalog")
    source_hash = _exact_string(catalog.get("source_hash"), "catalog.source_hash")
    if not CATALOG_HASH_RE.fullmatch(source_hash):
        raise BundleError("catalog.source_hash must be 64 lowercase hexadecimal characters")
    if catalog.get("source_hash_kind") != "action_semantics_v1":
        raise BundleError("catalog.source_hash_kind must be action_semantics_v1")
    actual_semantic_hash = _catalog_semantic_hash(catalog)
    if source_hash != actual_semantic_hash:
        raise BundleError(
            f"catalog semantic hash mismatch: declared={source_hash} actual={actual_semantic_hash}"
        )
    return catalog, source_hash


def _manifest_bytes(manifest: dict[str, object]) -> bytes:
    return (
        json.dumps(manifest, indent=2, ensure_ascii=False, allow_nan=False) + "\n"
    ).encode("utf-8")


def _validate_manifest_shape(manifest: dict[str, object]) -> None:
    if set(manifest) != set(MANIFEST_FIELDS) or len(manifest) != len(MANIFEST_FIELDS):
        raise BundleError(f"manifest must contain exactly: {', '.join(MANIFEST_FIELDS)}")
    schema = manifest.get("schema_version")
    if isinstance(schema, bool) or not isinstance(schema, int) or schema != SCHEMA_VERSION:
        raise BundleError(f"manifest.schema_version must be integer {SCHEMA_VERSION}")
    if manifest.get("tool") != "monolith_query" or manifest.get("runtime") != "native-cpp":
        raise BundleError("manifest has the wrong tool/runtime identity")

    source_hash = _exact_string(manifest.get("source_hash"), "manifest.source_hash")
    catalog_hash = _exact_string(
        manifest.get("catalog_source_hash"), "manifest.catalog_source_hash"
    )
    if not SOURCE_HASH_RE.fullmatch(source_hash):
        raise BundleError("manifest.source_hash must be 16 lowercase hexadecimal characters")
    if not CATALOG_HASH_RE.fullmatch(catalog_hash):
        raise BundleError("manifest.catalog_source_hash must be 64 lowercase hexadecimal characters")
    if manifest.get("file") != f"monolith_query-{source_hash}.exe":
        raise BundleError("manifest.file is not bound to manifest.source_hash")
    if manifest.get("catalog_file") != f"monolith_catalog-{catalog_hash}.json":
        raise BundleError("manifest.catalog_file is not bound to manifest.catalog_source_hash")
    if not QUERY_FILE_RE.fullmatch(str(manifest.get("file", ""))):
        raise BundleError("manifest.file is not a valid immutable Query leaf name")
    if not CATALOG_FILE_RE.fullmatch(str(manifest.get("catalog_file", ""))):
        raise BundleError("manifest.catalog_file is not a valid immutable catalog leaf name")
    for field in ("sha256", "catalog_sha256"):
        value = _exact_string(manifest.get(field), f"manifest.{field}")
        if not SHA256_RE.fullmatch(value):
            raise BundleError(f"manifest.{field} must be lowercase SHA-256")
    _exact_string(manifest.get("plugin_version"), "manifest.plugin_version")
    _exact_string(manifest.get("parity_spec_rev"), "manifest.parity_spec_rev")


def _child_leaf(root: Path, name: str, label: str) -> Path:
    if Path(name).name != name or "/" in name or "\\" in name:
        raise BundleError(f"{label} must be one leaf name")
    candidate = root / name
    if os.path.normcase(str(candidate.resolve(strict=False).parent)) != os.path.normcase(str(root)):
        raise BundleError(f"{label} escapes the bundle root")
    return candidate


def _validate_manifest_and_files(
    manifest: dict[str, object],
    root: Path,
    expected_source_hash: str | None = None,
) -> dict[str, object]:
    _validate_manifest_shape(manifest)
    source_hash = str(manifest["source_hash"])
    if expected_source_hash is not None and source_hash != expected_source_hash:
        raise BundleError(
            f"Query source hash mismatch: expected={expected_source_hash} actual={source_hash}"
        )
    executable = _child_leaf(root, str(manifest["file"]), "manifest.file")
    catalog_path = _child_leaf(root, str(manifest["catalog_file"]), "manifest.catalog_file")
    _regular_non_reparse_file(executable, "immutable Query executable")
    catalog_bytes = _read_bounded(catalog_path, MAX_CATALOG_BYTES, "immutable Query catalog")

    exe_sha_before = _sha256_file(executable)
    catalog_sha_before = _sha256_bytes(catalog_bytes)
    if exe_sha_before != manifest["sha256"]:
        raise BundleError("immutable Query executable SHA-256 does not match the manifest")
    if catalog_sha_before != manifest["catalog_sha256"]:
        raise BundleError("immutable Query catalog SHA-256 does not match the manifest")
    catalog, catalog_source_hash = _validate_catalog_bytes(catalog_bytes)
    if catalog_source_hash != manifest["catalog_source_hash"]:
        raise BundleError("immutable Query catalog semantic hash does not match the manifest")

    version = _run_bounded_version(executable)
    if version["source_hash"] != source_hash:
        raise BundleError("immutable Query --version source_hash does not match the manifest")
    for field in ("tool", "runtime", "plugin_version", "parity_spec_rev"):
        if version[field] != manifest[field]:
            raise BundleError(f"immutable Query --version {field} does not match the manifest")

    if _sha256_file(executable) != exe_sha_before:
        raise BundleError("immutable Query executable changed during validation")
    if _sha256_file(catalog_path) != catalog_sha_before:
        raise BundleError("immutable Query catalog changed during validation")
    return {
        "manifest": manifest,
        "executable": executable,
        "catalog": catalog_path,
        "version": version,
        "catalog_action_count": catalog["action_count"],
    }


def validate_bundle(
    binaries_root: Path,
    expected_source_hash: str | None = None,
) -> dict[str, object]:
    root = _regular_non_reparse_directory(binaries_root, "bundle root")
    manifest_path = root / MANIFEST_NAME
    manifest_before = _read_bounded(manifest_path, MAX_MANIFEST_BYTES, "Query manifest")
    manifest = _load_strict_json_bytes(manifest_before, "Query manifest")
    result = _validate_manifest_and_files(manifest, root, expected_source_hash)
    manifest_after = _read_bounded(manifest_path, MAX_MANIFEST_BYTES, "Query manifest")
    if manifest_after != manifest_before:
        raise BundleError("Query manifest changed during validation")
    result["manifest_path"] = manifest_path
    return result


def _publish_immutable_bytes(destination: Path, data: bytes, label: str) -> None:
    if destination.exists():
        _regular_non_reparse_file(destination, label)
        if destination.read_bytes() != data:
            raise BundleError(
                f"immutable-name collision for {destination.name}; existing bytes differ"
            )
        return

    temporary = destination.parent / f".{destination.name}.tmp-{uuid.uuid4().hex}"
    try:
        with temporary.open("xb") as handle:
            handle.write(data)
            handle.flush()
            os.fsync(handle.fileno())
        try:
            os.link(temporary, destination)
        except FileExistsError:
            _regular_non_reparse_file(destination, label)
            if destination.read_bytes() != data:
                raise BundleError(
                    f"immutable-name collision for {destination.name}; concurrent bytes differ"
                )
    finally:
        try:
            temporary.unlink()
        except FileNotFoundError:
            pass


def _publish_immutable_catalog(
    destination: Path,
    generated_bytes: bytes,
    generated_catalog: dict[str, object],
) -> bytes:
    """Publish once, reusing old bytes only for generator-defined semantic equality."""
    if destination.exists():
        existing_bytes = _read_bounded(
            destination, MAX_CATALOG_BYTES, "immutable Query catalog"
        )
        existing_catalog, existing_source_hash = _validate_catalog_bytes(existing_bytes)
        if existing_source_hash != generated_catalog["source_hash"]:
            raise BundleError(f"immutable-name collision for {destination.name}; hash differs")
        if existing_bytes != generated_bytes:
            # generated_at and source_line are deliberately excluded from the
            # generator's semantic hash. Preserve the already-published bytes
            # when only that provenance changed, so an immutable name never
            # aliases two payloads and routine regeneration remains repeatable.
            if _catalog_contract(existing_catalog) != _catalog_contract(generated_catalog):
                raise BundleError(
                    f"immutable-name collision for {destination.name}; semantic contract differs"
                )
        return existing_bytes

    _publish_immutable_bytes(destination, generated_bytes, "immutable Query catalog")
    return generated_bytes


def _publish_manifest_atomically(root: Path, data: bytes) -> None:
    temporary = root / f".{MANIFEST_NAME}.tmp-{uuid.uuid4().hex}"
    try:
        with temporary.open("xb") as handle:
            handle.write(data)
            handle.flush()
            os.fsync(handle.fileno())
        os.replace(temporary, root / MANIFEST_NAME)
    finally:
        try:
            temporary.unlink()
        except FileNotFoundError:
            pass


def _publish_compatibility_copy(root: Path, immutable_executable: Path) -> str | None:
    compatibility = root / "monolith_query.exe"
    temporary = root / f".monolith_query.compat.tmp-{uuid.uuid4().hex}.exe"
    try:
        # Do not hard-link the mutable compatibility name to the immutable image:
        # a legacy in-place writer touching monolith_query.exe would otherwise
        # corrupt the authoritative generation through the shared file record.
        with immutable_executable.open("rb") as source, temporary.open("xb") as target:
            for chunk in iter(lambda: source.read(1024 * 1024), b""):
                target.write(chunk)
            target.flush()
            os.fsync(target.fileno())
        os.replace(temporary, compatibility)
        return None
    except OSError as error:
        return (
            "compatibility monolith_query.exe was not replaced; the immutable manifest bundle "
            f"is authoritative ({error})"
        )
    finally:
        try:
            temporary.unlink()
        except FileNotFoundError:
            pass


def publish_bundle(
    built_executable: Path,
    catalog_source: Path,
    binaries_root: Path,
    expected_source_hash: str,
) -> dict[str, object]:
    if not SOURCE_HASH_RE.fullmatch(expected_source_hash):
        raise BundleError("--expected-source-hash must be 16 lowercase hexadecimal characters")
    root = binaries_root
    root.mkdir(parents=True, exist_ok=True)
    root = _regular_non_reparse_directory(root, "bundle root")

    executable_bytes = _read_bounded(
        built_executable.resolve(strict=True), 256 * 1024 * 1024, "built Query executable"
    )
    version = _run_bounded_version(built_executable.resolve(strict=True))
    if version["source_hash"] != expected_source_hash:
        raise BundleError(
            "built Query source_hash does not match build.bat: "
            f"expected={expected_source_hash} actual={version['source_hash']}"
        )
    catalog_bytes = _read_bounded(
        catalog_source.resolve(strict=True), MAX_CATALOG_BYTES, "generated Query catalog"
    )
    generated_catalog, catalog_source_hash = _validate_catalog_bytes(catalog_bytes)

    executable_name = f"monolith_query-{expected_source_hash}.exe"
    catalog_name = f"monolith_catalog-{catalog_source_hash}.json"
    immutable_executable = root / executable_name
    immutable_catalog = root / catalog_name
    _publish_immutable_bytes(immutable_executable, executable_bytes, "immutable Query executable")
    published_catalog_bytes = _publish_immutable_catalog(
        immutable_catalog, catalog_bytes, generated_catalog
    )

    manifest: dict[str, object] = {
        "schema_version": SCHEMA_VERSION,
        "tool": "monolith_query",
        "runtime": "native-cpp",
        "file": executable_name,
        "plugin_version": version["plugin_version"],
        "parity_spec_rev": version["parity_spec_rev"],
        "source_hash": expected_source_hash,
        "sha256": _sha256_bytes(executable_bytes),
        "catalog_file": catalog_name,
        "catalog_source_hash": catalog_source_hash,
        "catalog_sha256": _sha256_bytes(published_catalog_bytes),
    }
    _validate_manifest_and_files(manifest, root, expected_source_hash)
    manifest_data = _manifest_bytes(manifest)
    _publish_manifest_atomically(root, manifest_data)
    result = validate_bundle(root, expected_source_hash)
    warning = _publish_compatibility_copy(root, immutable_executable)
    result["compatibility_warning"] = warning
    return result


def _summary(result: dict[str, object]) -> dict[str, object]:
    manifest = result["manifest"]
    assert isinstance(manifest, dict)
    return {
        "status": "ok",
        "manifest": MANIFEST_NAME,
        "file": manifest["file"],
        "sha256": manifest["sha256"],
        "source_hash": manifest["source_hash"],
        "catalog_file": manifest["catalog_file"],
        "catalog_sha256": manifest["catalog_sha256"],
        "catalog_source_hash": manifest["catalog_source_hash"],
        "catalog_action_count": result["catalog_action_count"],
        "compatibility_warning": result.get("compatibility_warning"),
    }


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(dest="command", required=True)

    publish_parser = subparsers.add_parser("publish", help="publish a validated immutable bundle")
    publish_parser.add_argument("--built-exe", type=Path, required=True)
    publish_parser.add_argument("--catalog", type=Path, required=True)
    publish_parser.add_argument("--binaries-root", type=Path, required=True)
    publish_parser.add_argument("--expected-source-hash", required=True)

    validate_parser = subparsers.add_parser("validate", help="validate the current bundle")
    validate_parser.add_argument("--binaries-root", type=Path, required=True)
    validate_parser.add_argument("--expected-source-hash")

    args = parser.parse_args(argv)
    try:
        if args.command == "publish":
            result = publish_bundle(
                args.built_exe,
                args.catalog,
                args.binaries_root,
                args.expected_source_hash,
            )
        else:
            result = validate_bundle(args.binaries_root, args.expected_source_hash)
        print(json.dumps(_summary(result), indent=2))
        return 0
    except (BundleError, OSError) as error:
        print(f"FAILED: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
