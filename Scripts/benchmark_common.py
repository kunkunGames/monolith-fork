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
from dataclasses import dataclass
from typing import Any, Callable, Iterable, Mapping, Sequence


SCRIPT_DIR = pathlib.Path(__file__).resolve().parent
PLUGIN_ROOT = SCRIPT_DIR.parent

DEFAULT_MAX_TRANSPORT_FAILED_FRACTION = 0.05
DEFAULT_MAX_CONSECUTIVE_TRANSPORT_FAILURES = 3
DEFAULT_MIN_TRANSPORT_FRACTION_SAMPLES = 20

# Benchmark traffic must be distinguishable from real agent traffic in the action log.
#
# The suites deliberately send hallucinated action names, transposed-letter typos and
# malformed params as NEGATIVE fixtures. The action logger's only synthetic signal is
# `environment.is_automation_test`, stamped from `GIsAutomationTesting` — which covers
# in-process C++ automation but NOT an out-of-process HTTP client like these runners.
# So every negative fixture was landing in Logs/<date>/action.jsonl as genuine, unmet
# demand: the 2026-07-11 analyzer run reported `worldgen.get_blockout_volumse`,
# `ui.get_widget_tree_typo`, `mesh.get_mesh_inof` and 100 similar rows as
# `needed_action` findings — the very report an agent reads to decide which actions to
# build next.
#
# An out-of-process client therefore has to declare itself. Monolith already forwards a
# client-supplied `_monolith_routing_context` object verbatim into the log record
# (MonolithHttpServer.cpp -> FScopedTrace -> record.routing_context), so the identity
# rides that existing extension point rather than a new one. The analyzer treats
# `routing_context.client_kind == "benchmark"` as a primary synthetic signal.
# See Docs/specs/SPEC_MonolithToolInvocationLogs.md and
# Docs/specs/SPEC_MonolithInvocationLogAnalyzer.md.
BENCHMARK_CLIENT_KIND = "benchmark"


def benchmark_routing_context(suite: str) -> dict[str, str]:
    """Routing context every benchmark MCP request must carry, so the analyzer can
    separate synthetic fixture traffic from real agent demand.

    ``suite`` names the benchmark (e.g. ``"AssetEditing"``) so a noisy suite can be
    attributed without re-reading the task corpora.
    """
    if not suite:
        raise ValueError("suite is required so benchmark log traffic stays attributable")
    return {"client_kind": BENCHMARK_CLIENT_KIND, "suite": suite}


@dataclass(frozen=True)
class TransportAbortDecision:
    """One immutable decision emitted when a transport budget is exceeded."""

    reason: str
    attempted_count: int
    failure_count: int
    failed_fraction: float
    consecutive_failures: int
    item_id: str = ""
    status: int | None = None
    raw: str = ""


@dataclass
class TransportFailureTracker:
    """Shared in-run transport policy for benchmark runners.

    ``observe`` applies the consecutive gate immediately and the fraction gate
    once the configured sample floor is reached. ``finalize`` applies the
    fraction gate to a completed short run as well, preventing a 1/10 outage
    from becoming a valid baseline merely because the run had fewer than the
    normal 20 samples.
    """

    max_failed_fraction: float = DEFAULT_MAX_TRANSPORT_FAILED_FRACTION
    max_consecutive_failures: int = DEFAULT_MAX_CONSECUTIVE_TRANSPORT_FAILURES
    min_fraction_samples: int = DEFAULT_MIN_TRANSPORT_FRACTION_SAMPLES
    attempted_count: int = 0
    failure_count: int = 0
    consecutive_failures: int = 0
    # ``last_item_id`` is retained as the most recently attempted item for
    # callers that used the original tracker surface.  Transport diagnostics
    # must use ``last_transport_item_id`` so a fraction gate that fires on a
    # later successful observation does not pair that success with an older
    # transport error/status.
    last_item_id: str = ""
    last_transport_item_id: str = ""
    last_status: int | None = None
    last_raw: str = ""

    def __post_init__(self) -> None:
        if not 0.0 <= self.max_failed_fraction <= 1.0:
            raise ValueError("max_failed_fraction must be within [0.0, 1.0]")
        if self.max_consecutive_failures < 1:
            raise ValueError("max_consecutive_failures must be >= 1")
        if self.min_fraction_samples < 1:
            raise ValueError("min_fraction_samples must be >= 1")

    @property
    def failed_fraction(self) -> float:
        return self.failure_count / max(1, self.attempted_count)

    def _decision(self, reason: str) -> TransportAbortDecision:
        return TransportAbortDecision(
            reason=reason,
            attempted_count=self.attempted_count,
            failure_count=self.failure_count,
            failed_fraction=self.failed_fraction,
            consecutive_failures=self.consecutive_failures,
            item_id=self.last_transport_item_id,
            status=self.last_status,
            raw=self.last_raw,
        )

    def observe(
        self,
        *,
        transport_error: bool,
        item_id: str = "",
        status: int | None = None,
        raw: str = "",
    ) -> TransportAbortDecision | None:
        self.attempted_count += 1
        self.last_item_id = item_id
        if transport_error:
            self.failure_count += 1
            self.consecutive_failures += 1
            self.last_transport_item_id = item_id
            self.last_status = status
            self.last_raw = raw
        else:
            self.consecutive_failures = 0

        if self.consecutive_failures >= self.max_consecutive_failures:
            return self._decision("consecutive_transport_failures")
        if (
            self.attempted_count >= self.min_fraction_samples
            and self.failed_fraction > self.max_failed_fraction
        ):
            return self._decision("transport_failed_fraction")
        return None

    def finalize(self) -> TransportAbortDecision | None:
        if self.attempted_count and self.failed_fraction > self.max_failed_fraction:
            return self._decision("final_transport_failed_fraction")
        return None

    def snapshot(self) -> dict[str, Any]:
        return {
            "transport_failure_count": self.failure_count,
            "transport_failed_fraction": round(self.failed_fraction, 6),
            "consecutive_transport_failures": self.consecutive_failures,
            "max_transport_failed_fraction": self.max_failed_fraction,
            "max_consecutive_transport_failures": self.max_consecutive_failures,
            "min_transport_fraction_sample": self.min_fraction_samples,
            "last_transport_item_id": self.last_transport_item_id,
            "last_transport_status": self.last_status,
            "last_transport_error_raw": self.last_raw,
        }


class TaskCorpusContractError(RuntimeError):
    """Raised when a benchmark task corpus is absent, partial, or malformed."""


@dataclass(frozen=True)
class TaskCorpus:
    """One validated canonical corpus or an explicitly requested diagnostic subset."""

    tasks: list[dict[str, Any]]
    canonical: bool
    comparable: bool
    mode: str
    manifest: dict[str, Any]
    manifest_path: pathlib.Path | None


def _read_json_object_strict(path: pathlib.Path, description: str) -> dict[str, Any]:
    if not path.is_file():
        raise TaskCorpusContractError(f"{description} not found: {path}")
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise TaskCorpusContractError(f"{description} is not valid JSON: {path}: {exc}") from exc
    if not isinstance(value, dict):
        raise TaskCorpusContractError(f"{description} must be a JSON object: {path}")
    return value


def _category_counts(tasks: Sequence[Mapping[str, Any]]) -> dict[str, int]:
    counts: dict[str, int] = {}
    for task in tasks:
        category = str(task.get("category", ""))
        counts[category] = counts.get(category, 0) + 1
    return dict(sorted(counts.items()))


def load_task_corpus(
    tasks_path: str | pathlib.Path,
    *,
    suite: str,
    canonical_tasks_path: str | pathlib.Path,
    canonical_manifest_path: str | pathlib.Path,
    allow_subset: bool = False,
    required_fields: Sequence[str] = ("category", "tool", "namespace", "action", "expected"),
    allowed_categories: Iterable[str] | None = None,
    require_arguments: bool = False,
    task_validator: Callable[[Mapping[str, Any], int], None] | None = None,
    plugin_root: pathlib.Path | None = None,
) -> TaskCorpus:
    """Load a benchmark JSONL corpus through one fail-closed contract.

    The checked-in canonical task path is bound to its checked-in manifest.  A
    different path is a diagnostic subset and is rejected unless the caller
    explicitly opts in with ``allow_subset``; this prevents a truncated or
    accidentally redirected corpus from publishing a comparable baseline.
    """

    root = (plugin_root or PLUGIN_ROOT).resolve()
    resolved_tasks = resolve_plugin_path(tasks_path, root)
    canonical_tasks = resolve_plugin_path(canonical_tasks_path, root)
    canonical_manifest = resolve_plugin_path(canonical_manifest_path, root)
    is_canonical = resolved_tasks == canonical_tasks
    if not is_canonical and not allow_subset:
        raise TaskCorpusContractError(
            f"{suite} non-canonical task corpus requires explicit --allow-subset: "
            f"{display_path(resolved_tasks, root)}"
        )
    if not resolved_tasks.is_file():
        raise TaskCorpusContractError(f"{suite} task corpus not found: {resolved_tasks}")

    allowed = set(allowed_categories or ())
    tasks: list[dict[str, Any]] = []
    seen_ids: dict[str, int] = {}
    try:
        handle = resolved_tasks.open("r", encoding="utf-8")
    except OSError as exc:
        raise TaskCorpusContractError(
            f"{suite} task corpus could not be opened: {resolved_tasks}: {exc}"
        ) from exc
    with handle:
        for line_no, line in enumerate(handle, 1):
            stripped = line.strip()
            if not stripped:
                continue
            try:
                value = json.loads(stripped)
            except json.JSONDecodeError as exc:
                raise TaskCorpusContractError(
                    f"{resolved_tasks}:{line_no}: invalid JSONL row: {exc}"
                ) from exc
            if not isinstance(value, dict):
                raise TaskCorpusContractError(
                    f"{resolved_tasks}:{line_no}: task row must be a JSON object"
                )

            task_id_value = value.get("id")
            if not isinstance(task_id_value, str) or not task_id_value.strip():
                raise TaskCorpusContractError(
                    f"{resolved_tasks}:{line_no}: task id must be a non-empty string"
                )
            task_id = task_id_value.strip()
            if task_id in seen_ids:
                raise TaskCorpusContractError(
                    f"{resolved_tasks}:{line_no}: duplicate task id {task_id!r}; "
                    f"first declared on line {seen_ids[task_id]}"
                )
            seen_ids[task_id] = line_no

            for field in required_fields:
                if field not in value:
                    raise TaskCorpusContractError(
                        f"{resolved_tasks}:{line_no}: {task_id} is missing required field {field!r}"
                    )
            for field in ("category", "tool", "namespace", "action"):
                if field in required_fields and (
                    not isinstance(value.get(field), str) or not str(value.get(field)).strip()
                ):
                    raise TaskCorpusContractError(
                        f"{resolved_tasks}:{line_no}: {task_id}.{field} must be a non-empty string"
                    )
            if "expected" in required_fields and not isinstance(value.get("expected"), dict):
                raise TaskCorpusContractError(
                    f"{resolved_tasks}:{line_no}: {task_id}.expected must be an object"
                )
            if require_arguments and not isinstance(value.get("arguments"), dict):
                raise TaskCorpusContractError(
                    f"{resolved_tasks}:{line_no}: {task_id}.arguments must be an object"
                )
            if "arguments" in value and not isinstance(value.get("arguments"), dict):
                raise TaskCorpusContractError(
                    f"{resolved_tasks}:{line_no}: {task_id}.arguments must be an object when present"
                )
            category = str(value.get("category", ""))
            if allowed and category not in allowed:
                raise TaskCorpusContractError(
                    f"{resolved_tasks}:{line_no}: {task_id} has unsupported category {category!r}"
                )
            if task_validator is not None:
                try:
                    task_validator(value, line_no)
                except TaskCorpusContractError:
                    raise
                except Exception as exc:
                    raise TaskCorpusContractError(
                        f"{resolved_tasks}:{line_no}: {task_id} failed {suite} task validation: {exc}"
                    ) from exc
            tasks.append(value)

    if not tasks:
        raise TaskCorpusContractError(f"{suite} task corpus is empty: {resolved_tasks}")

    manifest: dict[str, Any] = {}
    manifest_path: pathlib.Path | None = None
    if is_canonical:
        manifest_path = canonical_manifest
        manifest = _read_json_object_strict(canonical_manifest, f"{suite} canonical manifest")
        expected_count = manifest.get("task_count")
        if not isinstance(expected_count, int) or isinstance(expected_count, bool):
            raise TaskCorpusContractError(
                f"{suite} canonical manifest task_count must be an integer: {canonical_manifest}"
            )
        if expected_count != len(tasks):
            raise TaskCorpusContractError(
                f"{suite} canonical manifest task_count={expected_count}, parsed_task_count={len(tasks)}"
            )
        expected_categories = manifest.get("category_counts")
        observed_categories = _category_counts(tasks)
        if not isinstance(expected_categories, dict):
            raise TaskCorpusContractError(
                f"{suite} canonical manifest category_counts must be an object: {canonical_manifest}"
            )
        normalized_expected: dict[str, int] = {}
        for key, count in expected_categories.items():
            if not isinstance(key, str) or not isinstance(count, int) or isinstance(count, bool):
                raise TaskCorpusContractError(
                    f"{suite} canonical manifest category_counts must map strings to integers"
                )
            normalized_expected[key] = count
        if dict(sorted(normalized_expected.items())) != observed_categories:
            raise TaskCorpusContractError(
                f"{suite} canonical manifest category_counts={dict(sorted(normalized_expected.items()))}, "
                f"observed={observed_categories}"
            )

    return TaskCorpus(
        tasks=tasks,
        canonical=is_canonical,
        comparable=is_canonical,
        mode="canonical" if is_canonical else "explicit_subset",
        manifest=manifest,
        manifest_path=manifest_path,
    )


def task_corpus_metadata(corpus: TaskCorpus) -> dict[str, Any]:
    return {
        "mode": corpus.mode,
        "canonical": corpus.canonical,
        "comparable": corpus.comparable,
        "validated_task_count": len(corpus.tasks),
    }


def classify_mcp_protocol_failure(response: Any) -> str:
    """Classify malformed JSON-RPC/MCP envelopes while preserving valid ``isError`` results."""
    if not isinstance(response, dict):
        return "protocol_error"
    if response.get("transport_error"):
        return ""
    if response.get("parse_error") or response.get("protocol_error"):
        return "protocol_error"
    if response.get("error") is not None:
        return "protocol_error"
    if not isinstance(response.get("result"), dict):
        return "protocol_error"
    return ""


def validate_mcp_status_response(
    response: Any,
    *,
    result_payload: Callable[[Mapping[str, Any]], Mapping[str, Any]],
    result_data: Callable[[Mapping[str, Any]], Mapping[str, Any]],
) -> dict[str, Any]:
    """Validate the mandatory status boundary through one shared fail-closed contract."""
    if not isinstance(response, dict):
        return {
            "ok": False,
            "failure_kind": "protocol_error",
            "raw": str(response)[:500],
            "transport_status": None,
        }
    if response.get("transport_error"):
        status = response.get("status")
        return {
            "ok": False,
            "failure_kind": "transport_error",
            "raw": str(response.get("raw", ""))[:500],
            "transport_status": (
                status if isinstance(status, int) and not isinstance(status, bool) else None
            ),
        }
    protocol_failure = classify_mcp_protocol_failure(response)
    if protocol_failure:
        return {
            "ok": False,
            "failure_kind": protocol_failure,
            "raw": str(response.get("raw", response))[:500],
            "transport_status": None,
        }
    try:
        payload = result_payload(response)
        status_data = result_data(response)
    except Exception as exc:
        return {
            "ok": False,
            "failure_kind": "protocol_error",
            "raw": f"status response decoding failed: {type(exc).__name__}: {exc}"[:500],
            "transport_status": None,
        }
    if bool(payload.get("isError")):
        return {
            "ok": False,
            "failure_kind": "server_error",
            "raw": str(response.get("raw", response))[:500],
            "transport_status": None,
        }
    if not isinstance(status_data, Mapping) or status_data.get("server_running") is not True:
        return {
            "ok": False,
            "failure_kind": "invalid_status_payload",
            "raw": str(status_data)[:500],
            "transport_status": None,
        }
    expected_project = local_project_name()
    observed_project = str(
        status_data.get("project_name") or status_data.get("project") or ""
    ).strip()
    if observed_project != expected_project:
        return {
            "ok": False,
            "failure_kind": "invalid_status_identity",
            "raw": (
                f"status project identity mismatch: observed={observed_project or '<missing>'} "
                f"expected={expected_project}"
            ),
            "transport_status": None,
        }
    return {
        "ok": True,
        "failure_kind": "",
        "raw": "",
        "transport_status": None,
        "status": dict(status_data),
    }


_STATUS_IDENTITY_ALIASES: tuple[tuple[str, tuple[str, ...]], ...] = (
    ("server", ("server_name", "server", "name")),
    ("server_version", ("plugin_version", "version")),
    ("catalog_version", ("catalog_version",)),
    ("project", ("project_name", "project")),
    ("engine_version", ("engine_version",)),
    ("process_id", ("editor_pid", "process_id", "pid")),
)


def status_identity(status: Mapping[str, Any], *, endpoint: str) -> dict[str, str]:
    """Extract stable endpoint/project/catalog/engine identity from a status payload."""
    identity: dict[str, str] = {"endpoint": str(endpoint)}
    for canonical, aliases in _STATUS_IDENTITY_ALIASES:
        for alias in aliases:
            value = status.get(alias)
            if value is not None and str(value).strip():
                identity[canonical] = str(value).strip()
                break
    return identity


def status_identity_mismatches(
    start: Mapping[str, str],
    end: Mapping[str, str],
) -> dict[str, dict[str, str]]:
    """Return every identity field that changed or disappeared during a run."""
    mismatches: dict[str, dict[str, str]] = {}
    for key in sorted(set(start) | set(end)):
        start_value = str(start.get(key, ""))
        end_value = str(end.get(key, ""))
        if start_value != end_value:
            mismatches[key] = {"start": start_value, "end": end_value}
    return mismatches

_PLUGIN_PREFIX = ("plugins", "monolith")
DEFAULT_DATABASE_PATHS = (
    "Saved/EngineSource.db",
    "Saved/ProjectIndex.db",
    "Logs/_engine_analysis.db",
    "Logs/_engine_analysis_v2.db",
    "Logs/_engine_analysis_v3.db",
    "Logs/_graph_analysis.db",
    "Logs/_graph_analysis_v2.db",
    "Logs/_graph_analysis_v3.db",
)

# Benchmark evidence must fingerprint only databases that can affect that
# suite's answers.  The former broad default made unrelated index churn stale
# every live suite and forced lightweight contract tests to hash multi-gigabyte
# databases they never queried.
BENCHMARK_DATABASE_PATHS: dict[str, tuple[str, ...]] = {
    "ActionGuidance": (),
    # Canonical SourceIndex execution, including graph-node search, reads the
    # source subsystem's authoritative EngineSource database.
    "SourceIndex": ("Saved/EngineSource.db",),
    "SchemaCompleteness": (),
    "OfflineParity": ("Saved/EngineSource.db",),
    "ProjectIndex": ("Saved/ProjectIndex.db",),
    "AICapability": (),
    "AssetEditing": ("Saved/ProjectIndex.db",),
}

BENCHMARK_DATABASE_SCOPE_MARKERS: dict[str, str] = {
    "ActionGuidance": "not_applicable_to_registry_routing",
    "SchemaCompleteness": "not_applicable_to_live_schema_registry_scan",
    "AICapability": "not_applicable_to_live_editor_ai_actions",
}
# Backward-compatible private alias for tests/callers that patched the original
# name. New contracts should use DEFAULT_DATABASE_PATHS.
_DB_CANDIDATES = DEFAULT_DATABASE_PATHS
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
    "project_name",
    "engine_version",
)


def local_project_name(plugin_root: pathlib.Path | None = None) -> str:
    """Resolve the owning Unreal project from the local Monolith checkout."""
    root = (plugin_root or PLUGIN_ROOT).resolve()
    project_root = root.parent.parent
    project_files = sorted(project_root.glob("*.uproject"))
    if len(project_files) == 1:
        return project_files[0].stem
    return "Monolith"


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
    *,
    candidate_paths: Iterable[str | pathlib.Path] | None = None,
) -> list[dict[str, Any]]:
    root = (plugin_root or PLUGIN_ROOT).resolve()
    candidates: list[str | pathlib.Path] = list(
        _DB_CANDIDATES if candidate_paths is None else candidate_paths
    )
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
            include_sha256=True,
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
    database_paths: Iterable[str | pathlib.Path] | None = None,
    extra_database_paths: Iterable[str | pathlib.Path] | None = None,
    plugin_root: pathlib.Path | None = None,
) -> dict[str, Any]:
    root = (plugin_root or PLUGIN_ROOT).resolve()
    resolved_database_paths = (
        BENCHMARK_DATABASE_PATHS.get(benchmark, DEFAULT_DATABASE_PATHS)
        if database_paths is None
        else tuple(database_paths)
    )
    files: dict[str, Any] = {
        "benchmark_common": file_signature(
            root / "Scripts" / "benchmark_common.py",
            kind="benchmark_common",
            plugin_root=root,
            include_sha256=True,
            include_line_counts=False,
        )
    }
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
        if name in files:
            raise ValueError(f"duplicate benchmark input file key: {name}")
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
        # ``None`` retains the shared broad diagnostic default. A benchmark
        # with a known DB contract supplies an exact dependency set so an
        # unrelated live index cannot invalidate an otherwise comparable run.
        "database_files": database_signatures(
            root,
            extra_database_paths,
            candidate_paths=resolved_database_paths,
        ),
        "mcp_catalog": compact_mcp_catalog_metadata(mcp_status, catalog, manifest),
    }
    database_scope = BENCHMARK_DATABASE_SCOPE_MARKERS.get(benchmark)
    if database_scope is not None:
        payload["database_files_scope"] = database_scope
    return refresh_benchmark_input_fingerprint(payload)


def benchmark_input_fingerprint(payload: Mapping[str, Any]) -> str:
    """Return the canonical SHA-256 for a benchmark-input payload.

    The stored fingerprint is deliberately excluded from its own digest.  Keep
    this operation public so runners that add suite-specific identity fields can
    refresh the fingerprint instead of silently publishing unhashed metadata.
    """
    unsigned_payload = dict(payload)
    unsigned_payload.pop("fingerprint_sha256", None)
    digest_source = json.dumps(
        unsigned_payload,
        sort_keys=True,
        separators=(",", ":"),
        ensure_ascii=True,
    )
    return hashlib.sha256(digest_source.encode("utf-8")).hexdigest()


def refresh_benchmark_input_fingerprint(payload: dict[str, Any]) -> dict[str, Any]:
    """Refresh ``fingerprint_sha256`` after suite-specific identity changes."""
    payload["fingerprint_sha256"] = benchmark_input_fingerprint(payload)
    return payload


def attach_benchmark_inputs(summary: dict[str, Any], benchmark_inputs: dict[str, Any]) -> dict[str, Any]:
    summary["benchmark_inputs"] = benchmark_inputs
    summary["input_fingerprint"] = benchmark_inputs.get("fingerprint_sha256")
    return summary


# Server-side page bounds for monolith_discover mode="actions" listings
# (MonolithCoreTools: DefaultLimit=50, MaxLimit=1000). Runners that omit an
# explicit limit see only the first 50 actions of a namespace.
DISCOVER_ACTION_PAGE_LIMIT = 1000
DISCOVER_ACTION_MAX_PAGES = 64


def paginate_discover_action_names(
    fetch_page,
    namespace: str,
    *,
    page_limit: int = DISCOVER_ACTION_PAGE_LIMIT,
    max_pages: int = DISCOVER_ACTION_MAX_PAGES,
) -> list[str]:
    """Enumerate every action name of one namespace through compact discover.

    ``fetch_page(arguments)`` performs one ``monolith_discover`` call with the
    given arguments dict and returns the parsed payload dict (transport is
    injected so this module stays offline-safe). Follows ``next_offset`` while
    the listing reports ``truncated`` — the default 50-row page silently hides
    everything past the first page for namespaces like ``ai`` (182 actions).
    Raises RuntimeError on a page without an ``actions`` list or a pagination
    loop that does not advance, so contract drift fails fast instead of being
    scored as a shrunken catalog.
    """
    names: list[str] = []
    offset = 0
    for _page in range(max_pages):
        payload = fetch_page({
            "namespace": namespace,
            "mode": "actions",
            "limit": page_limit,
            "offset": offset,
        })
        rows = payload.get("actions") if isinstance(payload, dict) else None
        if not isinstance(rows, list):
            raise RuntimeError(
                f"monolith_discover(namespace={namespace}, mode=actions, offset={offset}) "
                "returned no 'actions' list — discover contract drift"
            )
        for row in rows:
            name = row.get("action") if isinstance(row, dict) else row
            name = str(name or "").strip()
            if name:
                names.append(name)
        next_offset = payload.get("next_offset")
        if not bool(payload.get("truncated")) or not isinstance(next_offset, (int, float)):
            return names
        if int(next_offset) <= offset:
            raise RuntimeError(
                f"monolith_discover(namespace={namespace}, mode=actions) pagination did not "
                f"advance (offset={offset}, next_offset={next_offset})"
            )
        offset = int(next_offset)
    raise RuntimeError(
        f"monolith_discover(namespace={namespace}, mode=actions) pagination did not terminate "
        f"after {max_pages} pages"
    )
