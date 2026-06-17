#!/usr/bin/env python3
"""Shared helpers for Monolith benchmark runners.

The helpers are intentionally stdlib-only and do not require a live MCP server.
They keep benchmark input fingerprints stable across current working
directories by resolving relative paths from the Monolith plugin root.
"""

from __future__ import annotations

import hashlib
import json
import pathlib
from typing import Any, Iterable, Mapping


SCRIPT_DIR = pathlib.Path(__file__).resolve().parent
PLUGIN_ROOT = SCRIPT_DIR.parent

_PLUGIN_PREFIX = ("plugins", "monolith")
_DB_CANDIDATES = (
    "Saved/EngineSource.db",
    "Saved/graph.db",
    "Saved/ProjectIndex.db",
    "Logs/_engine_analysis.db",
    "Logs/_engine_analysis_v2.db",
    "Logs/_engine_analysis_v3.db",
    "Logs/_graph_analysis.db",
    "Logs/_graph_analysis_v2.db",
    "Logs/_graph_analysis_v3.db",
)
_MANIFEST_KEYS = (
    "benchmark",
    "generated_at",
    "task_count",
    "probe_set_task_count",
    "catalog_namespace_count",
    "catalog_action_count",
    "expected_namespace_count",
    "expected_action_count",
    "catalog_version_verified",
    "primary_score",
)
_MCP_STABLE_KEYS = (
    "name",
    "server",
    "server_name",
    "version",
    "plugin_version",
    "catalog_version",
    "schema_version",
    "status",
    "action_count",
    "tool_count",
    "namespace_count",
    "project",
    "engine_version",
)


def plugin_relative_path(value: str | pathlib.Path) -> pathlib.Path:
    """Normalize a user/config path to be relative to the Monolith plugin root."""
    text = str(value).replace("\\", "/")
    parts = [part for part in pathlib.PurePosixPath(text).parts if part not in ("", ".")]
    lowered = tuple(part.lower() for part in parts[:2])
    if lowered == _PLUGIN_PREFIX:
        parts = parts[2:]
    return pathlib.Path(*parts) if parts else pathlib.Path(".")


def resolve_plugin_path(
    value: str | pathlib.Path,
    plugin_root: pathlib.Path | None = None,
) -> pathlib.Path:
    """Resolve absolute, plugin-relative, or repo-prefixed paths deterministically."""
    path = pathlib.Path(value)
    if path.is_absolute():
        return path.resolve()
    root = (plugin_root or PLUGIN_ROOT).resolve()
    return (root / plugin_relative_path(value)).resolve()


def display_path(path: pathlib.Path, plugin_root: pathlib.Path | None = None) -> str:
    root = (plugin_root or PLUGIN_ROOT).resolve()
    try:
        return path.resolve().relative_to(root).as_posix()
    except ValueError:
        return path.as_posix()


def sha256_file(path: pathlib.Path) -> str | None:
    if not path.is_file():
        return None
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def text_line_counts(path: pathlib.Path) -> dict[str, int]:
    if not path.is_file():
        return {"line_count": 0, "non_empty_line_count": 0}
    line_count = 0
    non_empty = 0
    with path.open("r", encoding="utf-8", errors="replace") as handle:
        for line in handle:
            line_count += 1
            if line.strip():
                non_empty += 1
    return {"line_count": line_count, "non_empty_line_count": non_empty}


def file_signature(
    value: str | pathlib.Path,
    *,
    kind: str,
    plugin_root: pathlib.Path | None = None,
    include_sha256: bool = True,
    include_line_counts: bool = False,
) -> dict[str, Any]:
    root = (plugin_root or PLUGIN_ROOT).resolve()
    path = resolve_plugin_path(value, root)
    exists = path.exists()
    stat = path.stat() if exists else None
    result: dict[str, Any] = {
        "kind": kind,
        "path": display_path(path, root),
        "exists": exists,
    }
    if stat is not None:
        result.update({
            "size_bytes": stat.st_size,
            "mtime_ns": stat.st_mtime_ns,
            "signature": f"{stat.st_size}:{stat.st_mtime_ns}",
        })
    if include_sha256:
        result["sha256"] = sha256_file(path)
    if include_line_counts:
        result.update(text_line_counts(path))
    return result


def jsonl_input_signature(
    value: str | pathlib.Path,
    *,
    kind: str,
    plugin_root: pathlib.Path | None = None,
) -> dict[str, Any]:
    return file_signature(
        value,
        kind=kind,
        plugin_root=plugin_root,
        include_sha256=True,
        include_line_counts=True,
    )


def database_signatures(
    plugin_root: pathlib.Path | None = None,
    extra_paths: Iterable[str | pathlib.Path] | None = None,
) -> list[dict[str, Any]]:
    root = (plugin_root or PLUGIN_ROOT).resolve()
    candidates: list[str | pathlib.Path] = list(_DB_CANDIDATES)
    if extra_paths:
        candidates.extend(extra_paths)

    seen: set[pathlib.Path] = set()
    signatures: list[dict[str, Any]] = []
    for candidate in candidates:
        path = resolve_plugin_path(candidate, root)
        if path in seen:
            continue
        seen.add(path)
        if not path.exists():
            continue
        signatures.append(file_signature(
            path,
            kind="database",
            plugin_root=root,
            include_sha256=False,
            include_line_counts=False,
        ))
    return signatures


def _load_json(path: pathlib.Path) -> dict[str, Any]:
    if not path.is_file():
        return {}
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        return {}
    return data if isinstance(data, dict) else {}


def _stable_subset(data: Mapping[str, Any]) -> dict[str, Any]:
    return {key: data[key] for key in _MCP_STABLE_KEYS if key in data}


def compact_mcp_catalog_metadata(
    mcp_status: Mapping[str, Any] | None = None,
    catalog: Mapping[str, Any] | None = None,
    manifest: Mapping[str, Any] | None = None,
) -> dict[str, Any]:
    metadata: dict[str, Any] = {}
    status = mcp_status if isinstance(mcp_status, Mapping) else {}
    catalog_data = catalog if isinstance(catalog, Mapping) else {}
    manifest_data = manifest if isinstance(manifest, Mapping) else {}

    if status:
        metadata["status"] = _stable_subset(status)
        for nested_key in ("serverInfo", "server_info", "catalog", "indexing"):
            nested = status.get(nested_key)
            if isinstance(nested, Mapping):
                metadata.setdefault("status_nested", {})[nested_key] = _stable_subset(nested)

    namespaces = catalog_data.get("namespaces")
    if isinstance(namespaces, list):
        action_count = 0
        for namespace in namespaces:
            if isinstance(namespace, Mapping):
                actions = namespace.get("actions")
                if isinstance(actions, list):
                    action_count += len(actions)
                else:
                    action_count += int(namespace.get("action_count") or 0)
        metadata["catalog"] = {
            "namespace_count": len(namespaces),
            "action_count": action_count,
        }

    manifest_meta = {key: manifest_data[key] for key in _MANIFEST_KEYS if key in manifest_data}
    if manifest_meta:
        metadata["manifest"] = manifest_meta

    return metadata


def _manifest_for_input(
    benchmark: str,
    task_or_probe_path: pathlib.Path | None,
    plugin_root: pathlib.Path,
) -> pathlib.Path:
    if task_or_probe_path is not None:
        return task_or_probe_path.parent / "manifest.json"
    return resolve_plugin_path(f"Benchmarks/{benchmark}/manifest.json", plugin_root)


def build_benchmark_inputs(
    benchmark: str,
    *,
    tasks_path: str | pathlib.Path | None = None,
    probe_set_path: str | pathlib.Path | None = None,
    mcp_status: Mapping[str, Any] | None = None,
    catalog: Mapping[str, Any] | None = None,
    extra_files: Mapping[str, str | pathlib.Path] | None = None,
    extra_database_paths: Iterable[str | pathlib.Path] | None = None,
    plugin_root: pathlib.Path | None = None,
) -> dict[str, Any]:
    root = (plugin_root or PLUGIN_ROOT).resolve()
    files: dict[str, Any] = {}
    task_or_probe_path: pathlib.Path | None = None

    if tasks_path is not None:
        task_or_probe_path = resolve_plugin_path(tasks_path, root)
        files["tasks"] = jsonl_input_signature(task_or_probe_path, kind="tasks", plugin_root=root)
    if probe_set_path is not None:
        task_or_probe_path = resolve_plugin_path(probe_set_path, root)
        files["probe_set"] = jsonl_input_signature(task_or_probe_path, kind="probe_set", plugin_root=root)

    manifest_path = _manifest_for_input(benchmark, task_or_probe_path, root)
    manifest = _load_json(manifest_path)
    if manifest_path.exists():
        files["manifest"] = file_signature(
            manifest_path,
            kind="manifest",
            plugin_root=root,
            include_sha256=True,
            include_line_counts=False,
        )

    for name, value in (extra_files or {}).items():
        files[name] = file_signature(
            value,
            kind=name,
            plugin_root=root,
            include_sha256=True,
            include_line_counts=False,
        )

    payload: dict[str, Any] = {
        "schema_version": 1,
        "benchmark": benchmark,
        "files": files,
        "database_files": database_signatures(root, extra_database_paths),
        "mcp_catalog": compact_mcp_catalog_metadata(mcp_status, catalog, manifest),
    }
    digest_source = json.dumps(payload, sort_keys=True, separators=(",", ":"), ensure_ascii=True)
    payload["fingerprint_sha256"] = hashlib.sha256(digest_source.encode("utf-8")).hexdigest()
    return payload


def attach_benchmark_inputs(summary: dict[str, Any], benchmark_inputs: dict[str, Any]) -> dict[str, Any]:
    summary["benchmark_inputs"] = benchmark_inputs
    summary["input_fingerprint"] = benchmark_inputs.get("fingerprint_sha256")
    return summary
