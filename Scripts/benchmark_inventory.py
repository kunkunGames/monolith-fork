#!/usr/bin/env python3
"""Generate and validate the fixed Monolith benchmark completion inventory.

The inventory separates three facts that must not be conflated:

* ``items``: the manifest-declared benchmark contract;
* ``unwritten``: manifest rows that do not exist in the JSONL corpus;
* ``unverified``: written rows without a currently accepted full-run result.

Run from any directory:

    python Scripts/benchmark_inventory.py --write
    python Scripts/benchmark_inventory.py --portable-check
    python Scripts/benchmark_inventory.py --check
"""

from __future__ import annotations

import argparse
import collections
import difflib
import hashlib
import json
import pathlib
import sys
from dataclasses import dataclass
from typing import Any, Dict, Iterable, List, Mapping

from benchmark_common import (
    BENCHMARK_DATABASE_PATHS,
    BENCHMARK_DATABASE_SCOPE_MARKERS,
    benchmark_input_fingerprint,
    local_project_name,
)
from schema_completeness_benchmark import (
    CATALOG_ACTION_IDS_HASH_KIND,
    CHECKPOINT_DIMENSION_FIELDS,
    SCAN_CHECKPOINT_SCHEMA_VERSION,
    aggregate_metrics as aggregate_schema_metrics,
    build_namespace_breakdown as build_schema_namespace_breakdown,
    schema_quality_pass,
    validate_checkpoint_result_row,
    validate_schema_result_row,
)


MONOLITH_ROOT = pathlib.Path(__file__).resolve().parents[1]
BENCHMARK_ROOT = MONOLITH_ROOT / "Benchmarks"
STATUS_PATH = BENCHMARK_ROOT / "inventory_status.json"
OUTPUT_PATH = BENCHMARK_ROOT / "INVENTORY.md"
ACCEPTED_BUNDLE_FILENAME = "bundle_manifest.json"
ACCEPTED_BUNDLE_SCHEMA_VERSION = 2
PORTABLE_DATABASE_POLICY = "attest_when_absent_verify_when_present"
QUERY_BUNDLE_MANIFEST_RELATIVE = "Binaries/monolith_query.current.json"
QUERY_BUNDLE_MANIFEST_FIELDS = {
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
}


@dataclass(frozen=True)
class CorpusSpec:
    key: str
    label: str
    manifest: str
    jsonl: str
    count_field: str


CORPORA = (
    CorpusSpec(
        "OfflineParity",
        "OfflineParity",
        "OfflineParity/manifest.json",
        "OfflineParity/actions.jsonl",
        "action_count",
    ),
    CorpusSpec(
        "ActionGuidance",
        "ActionGuidance",
        "ActionGuidance/manifest.json",
        "ActionGuidance/tasks.jsonl",
        "task_count",
    ),
    CorpusSpec(
        "SourceIndex",
        "SourceIndex",
        "SourceIndex/manifest.json",
        "SourceIndex/tasks.jsonl",
        "task_count",
    ),
    CorpusSpec(
        "SchemaCompletenessProbe",
        "SchemaCompleteness probe contract",
        "SchemaCompleteness/manifest.json",
        "SchemaCompleteness/probe_set.jsonl",
        "probe_set_task_count",
    ),
    CorpusSpec(
        "ProjectIndex",
        "ProjectIndex",
        "ProjectIndex/manifest.json",
        "ProjectIndex/tasks.jsonl",
        "task_count",
    ),
    CorpusSpec(
        "AICapability",
        "AICapability",
        "AICapability/manifest.json",
        "AICapability/tasks.jsonl",
        "task_count",
    ),
    CorpusSpec(
        "AssetEditing",
        "AssetEditing",
        "AssetEditing/manifest.json",
        "AssetEditing/tasks.jsonl",
        "task_count",
    ),
)

FULL_CATALOG_KEY = "SchemaCompletenessFullCatalog"
FULL_CATALOG_LABEL = "SchemaCompleteness live full catalog"

SUITE_INPUT_PATHS: Dict[str, Dict[str, str]] = {
    "OfflineParity": {
        "tasks": "Benchmarks/OfflineParity/actions.jsonl",
        "manifest": "Benchmarks/OfflineParity/manifest.json",
        "runner": "Scripts/offline_parity_benchmark.py",
        "offline_python": "Scripts/monolith_offline.py",
        "query_manifest": QUERY_BUNDLE_MANIFEST_RELATIVE,
    },
    "ActionGuidance": {
        "tasks": "Benchmarks/ActionGuidance/tasks.jsonl",
        "manifest": "Benchmarks/ActionGuidance/manifest.json",
        "runner": "Scripts/action_guidance_benchmark.py",
    },
    "SourceIndex": {
        "tasks": "Benchmarks/SourceIndex/tasks.jsonl",
        "manifest": "Benchmarks/SourceIndex/manifest.json",
        "runner": "Scripts/source_index_benchmark.py",
    },
    "SchemaCompletenessProbe": {
        "probe_set": "Benchmarks/SchemaCompleteness/probe_set.jsonl",
        "manifest": "Benchmarks/SchemaCompleteness/manifest.json",
        "runner": "Scripts/schema_completeness_benchmark.py",
    },
    FULL_CATALOG_KEY: {
        "manifest": "Benchmarks/SchemaCompleteness/manifest.json",
        "runner": "Scripts/schema_completeness_benchmark.py",
    },
    "ProjectIndex": {
        "tasks": "Benchmarks/ProjectIndex/tasks.jsonl",
        "manifest": "Benchmarks/ProjectIndex/manifest.json",
        "runner": "Scripts/project_index_benchmark.py",
    },
    "AICapability": {
        "tasks": "Benchmarks/AICapability/tasks.jsonl",
        "manifest": "Benchmarks/AICapability/manifest.json",
        "runner": "Scripts/ai_capability_benchmark.py",
    },
    "AssetEditing": {
        "tasks": "Benchmarks/AssetEditing/tasks.jsonl",
        "manifest": "Benchmarks/AssetEditing/manifest.json",
        "runner": "Scripts/asset_editing_benchmark.py",
    },
}
for suite_input_paths in SUITE_INPUT_PATHS.values():
    suite_input_paths["benchmark_common"] = "Scripts/benchmark_common.py"

TASK_RESULT_FIELDS = {
    "ActionGuidance": "task_success",
    "SourceIndex": "direct_success",
    "ProjectIndex": "direct_success",
    "AICapability": "direct_success",
    "AssetEditing": "direct_success",
}
ACCEPTED_ARTIFACT_FILES = {
    "OfflineParity": ("per_action.jsonl",),
    "ActionGuidance": ("per_task.jsonl",),
    "SourceIndex": ("per_task.jsonl",),
    "SchemaCompletenessProbe": (
        "probe_results.jsonl",
        "per_action.jsonl",
        "probe_preflight.json",
    ),
    FULL_CATALOG_KEY: (
        "per_action.jsonl",
        "namespace_breakdown.json",
        "scan_checkpoint.json",
    ),
    "ProjectIndex": ("per_task.jsonl",),
    "AICapability": ("per_task.jsonl",),
    "AssetEditing": ("per_task.jsonl",),
}
CURRENT_CATALOG_MANIFEST_SUITES = frozenset({"SourceIndex", "AICapability"})
REQUIRED_PROTOCOL_RESULT_SUITES = {"SourceIndex", "AICapability"}
REQUIRED_FAILURE_KIND_RESULT_SUITES = {
    "SourceIndex",
    "ProjectIndex",
    "AICapability",
}

EVIDENCE_SUITE_DIRECTORIES = {
    "OfflineParity": "OfflineParity",
    "ActionGuidance": "ActionGuidance",
    "SourceIndex": "SourceIndex",
    "SchemaCompletenessProbe": "SchemaCompleteness",
    FULL_CATALOG_KEY: "SchemaCompleteness",
    "ProjectIndex": "ProjectIndex",
    "AICapability": "AICapability",
    "AssetEditing": "AssetEditing",
}

REQUIRED_EXECUTION_GATE_IDS = (
    "GATE-NOLINK",
    "GATE-FINAL-LINK",
    "GATE-CRG",
    "GATE-ANIMATION",
    "GATE-PRECOMMIT",
)
ALLOWED_EXECUTION_GATE_STATUSES = {
    "passed",
    "pending",
    "pending_after_final_link",
    "waiting_for_user_window",
    "failed",
    "blocked",
}
REQUIRED_LIVE_STATUS_IDENTITY_FIELDS = (
    "endpoint",
    "server_version",
    "catalog_version",
    "project",
    "engine_version",
)

SUITE_BENCHMARK_NAMES = {
    suite_key: "SchemaCompleteness"
    if suite_key in {"SchemaCompletenessProbe", FULL_CATALOG_KEY}
    else suite_key
    for suite_key in SUITE_INPUT_PATHS
}


class InventoryError(ValueError):
    """The checked-in benchmark inventory contract is internally inconsistent."""


def _reject_nonfinite_json(token: str) -> Any:
    raise InventoryError(f"non-finite JSON number is forbidden: {token}")


def _strict_json_object(pairs: List[tuple[str, Any]]) -> Dict[str, Any]:
    result: Dict[str, Any] = {}
    for key, value in pairs:
        if key in result:
            raise InventoryError(f"duplicate JSON object member: {key}")
        result[key] = value
    return result


def strict_json_loads(raw: str, *, context: str) -> Any:
    try:
        return json.loads(
            raw,
            object_pairs_hook=_strict_json_object,
            parse_constant=_reject_nonfinite_json,
        )
    except (json.JSONDecodeError, InventoryError) as exc:
        raise InventoryError(f"invalid strict JSON at {context}: {exc}") from exc


def exact_json_equal(left: Any, right: Any) -> bool:
    """Recursively compare JSON values without Python bool/number coercion."""
    if type(left) is not type(right):
        return False
    if isinstance(left, dict):
        return set(left) == set(right) and all(
            exact_json_equal(left[key], right[key]) for key in left
        )
    if isinstance(left, list):
        return len(left) == len(right) and all(
            exact_json_equal(a, b) for a, b in zip(left, right)
        )
    return left == right


def load_json(path: pathlib.Path) -> Dict[str, Any]:
    payload = strict_json_loads(
        path.read_text(encoding="utf-8"),
        context=str(path),
    )
    if not isinstance(payload, dict):
        raise InventoryError(f"JSON root must be an object: {path}")
    return payload


def expected_suite_input_paths(suite_key: str) -> Dict[str, str]:
    """Resolve accepted input paths, including the manifest-selected Query image."""
    expected = dict(SUITE_INPUT_PATHS[suite_key])
    if suite_key != "OfflineParity":
        return expected

    manifest_path = MONOLITH_ROOT / QUERY_BUNDLE_MANIFEST_RELATIVE
    manifest = load_json(manifest_path)
    if set(manifest) != QUERY_BUNDLE_MANIFEST_FIELDS:
        raise InventoryError(
            "current Query manifest fields are invalid for OfflineParity"
        )
    if (
        type(manifest.get("schema_version")) is not int
        or manifest.get("schema_version") != 1
        or manifest.get("tool") != "monolith_query"
        or manifest.get("runtime") != "native-cpp"
    ):
        raise InventoryError(
            "current Query manifest identity is invalid for OfflineParity"
        )
    source_hash = manifest.get("source_hash")
    filename = manifest.get("file")
    if (
        not isinstance(source_hash, str)
        or len(source_hash) != 16
        or any(character not in "0123456789abcdef" for character in source_hash)
        or not isinstance(filename, str)
        or filename != f"monolith_query-{source_hash}.exe"
        or pathlib.PurePath(filename).name != filename
        or "/" in filename
        or "\\" in filename
    ):
        raise InventoryError(
            "current Query manifest executable identity is invalid for OfflineParity"
        )
    expected["offline_exe"] = f"Binaries/{filename}"
    return expected


def current_catalog_identity() -> Dict[str, Any]:
    """Return the checked-in catalog-contract snapshot used by accepted suites."""
    manifest = load_json(BENCHMARK_ROOT / "ActionGuidance/manifest.json")
    version = str(manifest.get("catalog_version", "")).strip()
    action_count = manifest.get("catalog_action_count")
    namespace_count = manifest.get("catalog_namespace_count")
    if not version:
        raise InventoryError("ActionGuidance manifest has no catalog_version")
    if type(action_count) is not int or action_count < 1:
        raise InventoryError("ActionGuidance manifest has invalid catalog_action_count")
    if type(namespace_count) is not int or namespace_count < 1:
        raise InventoryError("ActionGuidance manifest has invalid catalog_namespace_count")
    return {
        "catalog_version": version,
        "catalog_action_count": action_count,
        "catalog_namespace_count": namespace_count,
    }


def load_jsonl(path: pathlib.Path) -> List[Dict[str, Any]]:
    rows: List[Dict[str, Any]] = []
    for line_no, raw in enumerate(path.read_text(encoding="utf-8").splitlines(), start=1):
        if not raw.strip():
            continue
        try:
            row = strict_json_loads(raw, context=f"{path}:{line_no}")
        except json.JSONDecodeError as exc:
            raise InventoryError(f"invalid JSONL at {path}:{line_no}: {exc}") from exc
        if not isinstance(row, dict):
            raise InventoryError(f"JSONL row must be an object at {path}:{line_no}")
        namespace = str(row.get("namespace", "")).strip()
        if not namespace:
            raise InventoryError(f"JSONL row has no namespace at {path}:{line_no}")
        rows.append(row)
    return rows


_FILE_HASH_CACHE: Dict[tuple[str, int, int, str], str] = {}


def file_sha256(path: pathlib.Path) -> str:
    stat = path.stat()
    cache_key = (str(path.resolve()), stat.st_size, stat.st_mtime_ns, "raw")
    cached = _FILE_HASH_CACHE.get(cache_key)
    if cached is not None:
        return cached
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    result = digest.hexdigest()
    _FILE_HASH_CACHE[cache_key] = result
    return result


def tracked_text_sha256(path: pathlib.Path) -> str:
    """Hash tracked text evidence without platform newline drift.

    Perforce and Git may materialize the same source-controlled text as LF or
    CRLF depending on the client platform. Accepted benchmark bundles are
    portable evidence, so their JSON/JSONL content is hashed after normalizing
    CRLF and lone CR bytes to LF. Binary inputs, especially databases and
    executables, continue to use ``file_sha256`` and remain byte-exact.
    """
    stat = path.stat()
    cache_key = (str(path.resolve()), stat.st_size, stat.st_mtime_ns, "text-lf")
    cached = _FILE_HASH_CACHE.get(cache_key)
    if cached is not None:
        return cached
    content = path.read_bytes().replace(b"\r\n", b"\n").replace(b"\r", b"\n")
    result = hashlib.sha256(content).hexdigest()
    _FILE_HASH_CACHE[cache_key] = result
    return result


def strict_non_negative_int(value: Any, context: str) -> int:
    if type(value) is not int or value < 0:
        raise InventoryError(f"{context} must be a non-negative integer")
    return value


def load_evidence_jsonl(path: pathlib.Path) -> List[Dict[str, Any]]:
    if not path.is_file():
        raise InventoryError(f"accepted evidence result file is missing: {path}")
    rows: List[Dict[str, Any]] = []
    for line_no, raw in enumerate(path.read_text(encoding="utf-8").splitlines(), start=1):
        if not raw.strip():
            continue
        try:
            row = strict_json_loads(raw, context=f"{path}:{line_no}")
        except json.JSONDecodeError as exc:
            raise InventoryError(f"invalid evidence JSONL at {path}:{line_no}: {exc}") from exc
        if not isinstance(row, dict):
            raise InventoryError(f"evidence JSONL row must be an object at {path}:{line_no}")
        rows.append(row)
    return rows


def resolve_monolith_relative(relative: str, context: str) -> pathlib.Path:
    normalized = relative.strip().replace("\\", "/")
    pure = pathlib.PurePosixPath(normalized)
    if not normalized or pure.is_absolute() or ".." in pure.parts:
        raise InventoryError(f"{context} must be a Monolith-relative path: {relative!r}")
    root = MONOLITH_ROOT.resolve()
    resolved = (root / pure).resolve()
    if resolved != root and root not in resolved.parents:
        raise InventoryError(f"{context} escapes the Monolith root: {relative!r}")
    return resolved


def require_sha256(value: Any, context: str) -> str:
    digest = str(value or "").strip().lower()
    if len(digest) != 64 or any(char not in "0123456789abcdef" for char in digest):
        raise InventoryError(f"{context} must be a lowercase SHA-256 digest")
    return digest


def load_accepted_bundle(
    suite_key: str,
    suite_status: Mapping[str, Any],
    summary_path: pathlib.Path,
    *,
    portable: bool,
) -> Dict[str, Any] | None:
    """Validate a tracked accepted-evidence bundle and return its manifest.

    Legacy ``Saved/...`` evidence remains valid for full local checks only. A
    portable check requires a source-controlled bundle so a clean checkout can
    rederive results without pretending the multi-gigabyte source database was
    checked in. The bundle manifest is pinned from inventory_status.json and in
    turn pins the summary, every suite-specific artifact consumed by evidence
    derivation, the input fingerprint, and the complete database attestation.
    """
    evidence = str(suite_status.get("evidence", "")).strip().replace("\\", "/")
    parts = pathlib.PurePosixPath(evidence).parts
    saved_prefix = (
        "Saved",
        "Monolith",
        "Benchmarks",
        EVIDENCE_SUITE_DIRECTORIES[suite_key],
    )
    if parts[:4] == saved_prefix and len(parts) >= 6 and parts[-1] == "summary.json":
        if portable:
            raise InventoryError(
                f"portable accepted evidence for {suite_key} must use a tracked "
                "Benchmarks/<suite>/accepted/<snapshot> bundle"
            )
        if suite_status.get("evidence_bundle_sha256") not in (None, ""):
            raise InventoryError(
                f"legacy accepted evidence for {suite_key} must not claim a tracked bundle"
            )
        return None

    expected_prefix = (
        "Benchmarks",
        EVIDENCE_SUITE_DIRECTORIES[suite_key],
        "accepted",
    )
    if (
        parts[:3] != expected_prefix
        or len(parts) != 5
        or not parts[3]
        or parts[4] != "summary.json"
    ):
        raise InventoryError(
            f"accepted {suite_key} evidence must be either its legacy Saved summary "
            "or Benchmarks/<suite>/accepted/<snapshot>/summary.json"
        )

    snapshot_id = parts[3]
    manifest_path = summary_path.parent / ACCEPTED_BUNDLE_FILENAME
    if not manifest_path.is_file():
        raise InventoryError(
            f"accepted evidence bundle manifest is missing for {suite_key}: {manifest_path}"
        )
    pinned_manifest_sha = require_sha256(
        suite_status.get("evidence_bundle_sha256"),
        f"accepted {suite_key} evidence_bundle_sha256",
    )
    observed_manifest_sha = tracked_text_sha256(manifest_path)
    if observed_manifest_sha != pinned_manifest_sha:
        raise InventoryError(
            f"accepted {suite_key} bundle manifest drifted: "
            f"status={pinned_manifest_sha} current={observed_manifest_sha}"
        )

    manifest = load_json(manifest_path)
    required_keys = {
        "schema_version",
        "benchmark",
        "suite",
        "snapshot_id",
        "summary_file",
        "summary_sha256",
        "artifact_files",
        "input_fingerprint",
        "database_inputs",
        "portable_database_policy",
    }
    if set(manifest) != required_keys:
        raise InventoryError(
            f"accepted {suite_key} bundle manifest keys are invalid: "
            f"observed={sorted(manifest)} expected={sorted(required_keys)}"
        )
    if strict_non_negative_int(
        manifest.get("schema_version"),
        f"accepted {suite_key} bundle schema_version",
    ) != ACCEPTED_BUNDLE_SCHEMA_VERSION:
        raise InventoryError(f"accepted {suite_key} bundle schema version is unsupported")
    if manifest.get("benchmark") != SUITE_BENCHMARK_NAMES[suite_key]:
        raise InventoryError(f"accepted {suite_key} bundle benchmark identity drifted")
    if manifest.get("suite") != suite_key or manifest.get("snapshot_id") != snapshot_id:
        raise InventoryError(f"accepted {suite_key} bundle suite/snapshot identity drifted")
    if manifest.get("portable_database_policy") != PORTABLE_DATABASE_POLICY:
        raise InventoryError(f"accepted {suite_key} bundle database policy drifted")
    if manifest.get("summary_file") != "summary.json":
        raise InventoryError(f"accepted {suite_key} bundle summary_file is invalid")
    summary_sha = require_sha256(
        manifest.get("summary_sha256"),
        f"accepted {suite_key} bundle summary_sha256",
    )
    if tracked_text_sha256(summary_path) != summary_sha:
        raise InventoryError(f"accepted {suite_key} bundle summary content drifted")

    artifact_files = manifest.get("artifact_files")
    expected_artifacts = ACCEPTED_ARTIFACT_FILES[suite_key]
    if not isinstance(artifact_files, dict) or set(artifact_files) != set(expected_artifacts):
        raise InventoryError(
            f"accepted {suite_key} bundle artifact set is invalid: "
            f"observed={sorted(artifact_files) if isinstance(artifact_files, dict) else '<invalid>'} "
            f"expected={sorted(expected_artifacts)}"
        )
    for artifact_name in expected_artifacts:
        metadata = artifact_files[artifact_name]
        expected_metadata_keys = (
            {"sha256", "non_empty_line_count"}
            if artifact_name.endswith(".jsonl")
            else {"sha256"}
        )
        if not isinstance(metadata, dict) or set(metadata) != expected_metadata_keys:
            raise InventoryError(
                f"accepted {suite_key} bundle metadata is invalid for {artifact_name}"
            )
        artifact_path = summary_path.parent / artifact_name
        if not artifact_path.is_file():
            raise InventoryError(
                f"accepted {suite_key} bundle artifact is missing: {artifact_path}"
            )
        expected_artifact_sha = require_sha256(
            metadata.get("sha256"),
            f"accepted {suite_key} bundle artifact SHA-256 for {artifact_name}",
        )
        if tracked_text_sha256(artifact_path) != expected_artifact_sha:
            raise InventoryError(
                f"accepted {suite_key} bundle artifact content drifted: {artifact_name}"
            )
        if artifact_name.endswith(".jsonl"):
            non_empty_line_count = sum(
                1
                for line in artifact_path.read_text(encoding="utf-8").splitlines()
                if line.strip()
            )
            if strict_non_negative_int(
                metadata.get("non_empty_line_count"),
                f"accepted {suite_key} bundle {artifact_name} non_empty_line_count",
            ) != non_empty_line_count:
                raise InventoryError(
                    f"accepted {suite_key} bundle artifact row count drifted: {artifact_name}"
                )
    require_sha256(
        manifest.get("input_fingerprint"),
        f"accepted {suite_key} bundle input_fingerprint",
    )
    if not isinstance(manifest.get("database_inputs"), list):
        raise InventoryError(f"accepted {suite_key} bundle database_inputs must be an array")
    return manifest


def normalize_namespace_results(
    suite_key: str,
    namespace_results: Mapping[str, Any],
) -> Dict[str, Dict[str, int]]:
    normalized: Dict[str, Dict[str, int]] = {}
    for namespace, raw_result in namespace_results.items():
        if not isinstance(raw_result, dict):
            raise InventoryError(f"namespace result {suite_key}.{namespace} must be an object")
        normalized[str(namespace)] = {
            "pass": strict_non_negative_int(
                raw_result.get("pass", 0), f"{suite_key}.{namespace}.pass"
            ),
            "fail": strict_non_negative_int(
                raw_result.get("fail", 0), f"{suite_key}.{namespace}.fail"
            ),
            "expected_skip": strict_non_negative_int(
                raw_result.get("expected_skip", 0),
                f"{suite_key}.{namespace}.expected_skip",
            ),
        }
    return normalized


def validate_complete_summary(suite_key: str, summary: Dict[str, Any]) -> None:
    if summary.get("run_valid") is not True:
        raise InventoryError(f"accepted {suite_key} run_valid must be true")
    if summary.get("completion_status") != "completed":
        raise InventoryError(f"accepted {suite_key} completion_status must be completed")
    if summary.get("metrics_valid") is not True:
        raise InventoryError(f"accepted {suite_key} metrics_valid must be true")
    if summary.get("metrics_scope") != "complete_run":
        raise InventoryError(f"accepted {suite_key} metrics_scope must be complete_run")
    if not isinstance(summary.get("metrics"), dict):
        raise InventoryError(f"accepted {suite_key} summary has no metrics object")


def validate_input_evidence(
    suite_key: str,
    suite_status: Dict[str, Any],
    summary: Dict[str, Any],
    *,
    portable: bool = False,
    bundle_manifest: Mapping[str, Any] | None = None,
    validation_report: Dict[str, List[str]] | None = None,
) -> Dict[str, pathlib.Path]:
    pinned_fingerprint = str(suite_status.get("input_fingerprint", "")).strip()
    observed_fingerprint = str(summary.get("input_fingerprint", "")).strip()
    inputs = summary.get("benchmark_inputs")
    if not isinstance(inputs, dict):
        raise InventoryError(f"accepted {suite_key} summary has no benchmark_inputs")
    if strict_non_negative_int(
        inputs.get("schema_version"), f"{suite_key}.benchmark_inputs.schema_version"
    ) != 1:
        raise InventoryError(f"accepted {suite_key} benchmark input schema_version must be 1")
    expected_benchmark = SUITE_BENCHMARK_NAMES[suite_key]
    if inputs.get("benchmark") != expected_benchmark:
        raise InventoryError(
            f"accepted {suite_key} benchmark input identity mismatch: "
            f"observed={inputs.get('benchmark')!r} expected={expected_benchmark!r}"
        )

    nested_fingerprint = str(inputs.get("fingerprint_sha256", "")).strip()
    recomputed_fingerprint = benchmark_input_fingerprint(inputs)
    if (
        not pinned_fingerprint
        or pinned_fingerprint != observed_fingerprint
        or pinned_fingerprint != nested_fingerprint
        or pinned_fingerprint != recomputed_fingerprint
    ):
        raise InventoryError(
            f"accepted {suite_key} input fingerprint mismatch: "
            f"status={pinned_fingerprint or '<missing>'} "
            f"summary={observed_fingerprint or '<missing>'} "
            f"nested={nested_fingerprint or '<missing>'} "
            f"recomputed={recomputed_fingerprint}"
        )
    if bundle_manifest is not None and bundle_manifest.get("input_fingerprint") != pinned_fingerprint:
        raise InventoryError(f"accepted {suite_key} bundle input fingerprint drifted")

    files = inputs.get("files")
    if not isinstance(files, dict):
        raise InventoryError(f"accepted {suite_key} benchmark input files are invalid")
    expected_paths = expected_suite_input_paths(suite_key)
    if set(files) != set(expected_paths):
        raise InventoryError(
            f"accepted {suite_key} input files differ from the suite contract: "
            f"observed={sorted(files)} expected={sorted(expected_paths)}"
        )

    resolved_files: Dict[str, pathlib.Path] = {}
    for input_name, expected_relative in expected_paths.items():
        entry = files[input_name]
        if not isinstance(entry, dict):
            raise InventoryError(f"accepted {suite_key} input {input_name} is invalid")
        expected_input_keys = {
            "kind",
            "path",
            "exists",
            "size_bytes",
            "mtime_ns",
            "signature",
            "sha256",
        }
        if input_name in {"tasks", "probe_set"}:
            expected_input_keys.update({"line_count", "non_empty_line_count"})
        if set(entry) != expected_input_keys:
            raise InventoryError(
                f"accepted {suite_key} input {input_name} structure is invalid: "
                f"observed={sorted(entry)} expected={sorted(expected_input_keys)}"
            )
        if entry.get("kind") != input_name:
            raise InventoryError(f"accepted {suite_key} input {input_name} kind is invalid")
        relative = str(entry.get("path", "")).strip().replace("\\", "/")
        if relative != expected_relative:
            raise InventoryError(
                f"accepted {suite_key} input {input_name} path mismatch: "
                f"observed={relative or '<missing>'} expected={expected_relative}"
            )
        if entry.get("exists") is not True:
            raise InventoryError(f"accepted {suite_key} input {input_name} must exist")
        expected_sha = str(entry.get("sha256", "")).strip().lower()
        if len(expected_sha) != 64 or any(char not in "0123456789abcdef" for char in expected_sha):
            raise InventoryError(f"accepted {suite_key} input {input_name} has invalid sha256")
        current_path = resolve_monolith_relative(relative, f"{suite_key}.{input_name}")
        if not current_path.is_file():
            raise InventoryError(f"accepted {suite_key} input is missing: {current_path}")
        stat = current_path.stat()
        expected_size = strict_non_negative_int(
            entry.get("size_bytes"), f"{suite_key}.{input_name}.size_bytes"
        )
        expected_mtime = strict_non_negative_int(
            entry.get("mtime_ns"), f"{suite_key}.{input_name}.mtime_ns"
        )
        if stat.st_size != expected_size:
            raise InventoryError(f"accepted {suite_key} input size drifted: {relative}")
        if not portable and stat.st_mtime_ns != expected_mtime:
            raise InventoryError(f"accepted {suite_key} input mtime drifted: {relative}")
        if entry.get("signature") != f"{expected_size}:{expected_mtime}":
            raise InventoryError(f"accepted {suite_key} input signature is invalid: {relative}")
        current_sha = file_sha256(current_path)
        if current_sha != expected_sha:
            raise InventoryError(
                f"accepted {suite_key} input drifted: {relative} "
                f"summary={expected_sha} current={current_sha}"
            )
        if input_name in {"tasks", "probe_set"}:
            lines = current_path.read_text(encoding="utf-8").splitlines()
            if strict_non_negative_int(
                entry.get("line_count"), f"{suite_key}.{input_name}.line_count"
            ) != len(lines):
                raise InventoryError(f"accepted {suite_key} input line count drifted: {relative}")
            if strict_non_negative_int(
                entry.get("non_empty_line_count"),
                f"{suite_key}.{input_name}.non_empty_line_count",
            ) != sum(1 for line in lines if line.strip()):
                raise InventoryError(
                    f"accepted {suite_key} input non-empty line count drifted: {relative}"
                )
        resolved_files[input_name] = current_path

    databases = inputs.get("database_files")
    if not isinstance(databases, list):
        raise InventoryError(f"accepted {suite_key} database_files must be an array")
    benchmark_key = (
        "SchemaCompleteness"
        if suite_key in {"SchemaCompletenessProbe", FULL_CATALOG_KEY}
        else suite_key
    )
    if benchmark_key not in BENCHMARK_DATABASE_PATHS:
        raise InventoryError(f"accepted {suite_key} has no database dependency contract")
    expected_database_paths = list(BENCHMARK_DATABASE_PATHS[benchmark_key])
    if not expected_database_paths:
        if databases:
            raise InventoryError(
                f"accepted {suite_key} must not fingerprint unrelated databases"
            )
        expected_scope = BENCHMARK_DATABASE_SCOPE_MARKERS.get(benchmark_key)
        if inputs.get("database_files_scope") != expected_scope:
            raise InventoryError(
                f"accepted {suite_key} lacks its exact DB-scope marker"
            )
    elif not databases:
        raise InventoryError(f"accepted {suite_key} requires explicit database input signatures")
    if bundle_manifest is not None and not exact_json_equal(
        bundle_manifest.get("database_inputs"), databases
    ):
        raise InventoryError(f"accepted {suite_key} bundle database attestation drifted")

    database_paths: List[str] = []
    for index, entry in enumerate(databases):
        if not isinstance(entry, dict):
            raise InventoryError(f"accepted {suite_key} database input {index} is invalid")
        expected_database_keys = {
            "kind",
            "path",
            "exists",
            "size_bytes",
            "mtime_ns",
            "signature",
            "sha256",
        }
        if set(entry) != expected_database_keys:
            raise InventoryError(
                f"accepted {suite_key} database input {index} structure is invalid: "
                f"observed={sorted(entry)} expected={sorted(expected_database_keys)}"
            )
        if entry.get("kind") != "database":
            raise InventoryError(f"accepted {suite_key} database input kind is invalid")
        relative = str(entry.get("path", "")).strip().replace("\\", "/")
        if not relative:
            raise InventoryError(f"accepted {suite_key} database input {index} has no path")
        database_paths.append(relative)
        current_path = resolve_monolith_relative(relative, f"{suite_key}.database_files[{index}]")
        expected_exists = entry.get("exists")
        if expected_exists is not True:
            raise InventoryError(f"accepted {suite_key} database input must exist: {relative}")
        expected_size = strict_non_negative_int(
            entry.get("size_bytes"), f"{suite_key}.{relative}.size_bytes"
        )
        expected_mtime = strict_non_negative_int(
            entry.get("mtime_ns"), f"{suite_key}.{relative}.mtime_ns"
        )
        if entry.get("signature") != f"{expected_size}:{expected_mtime}":
            raise InventoryError(f"accepted {suite_key} database signature is invalid: {relative}")
        expected_sha = require_sha256(
            entry.get("sha256"), f"accepted {suite_key} database SHA-256 for {relative}"
        )
        if not current_path.is_file():
            if not portable or bundle_manifest is None:
                raise InventoryError(f"accepted {suite_key} database input drifted: {relative}")
            if validation_report is not None:
                validation_report.setdefault("attested_databases", []).append(relative)
            continue
        stat = current_path.stat()
        if stat.st_size != expected_size:
            raise InventoryError(f"accepted {suite_key} database size drifted: {relative}")
        if stat.st_mtime_ns != expected_mtime:
            raise InventoryError(f"accepted {suite_key} database mtime drifted: {relative}")
        if file_sha256(current_path) != expected_sha:
            raise InventoryError(f"accepted {suite_key} database content drifted: {relative}")
    if len(database_paths) != len(set(database_paths)):
        raise InventoryError(f"accepted {suite_key} database input paths must be unique")
    if database_paths != expected_database_paths:
        raise InventoryError(
            f"accepted {suite_key} database inputs differ from the runner contract: "
            f"observed={database_paths} expected={expected_database_paths}"
        )
    if suite_key in {"SchemaCompletenessProbe", FULL_CATALOG_KEY}:
        registry = inputs.get("schema_registry")
        if not isinstance(registry, dict):
            raise InventoryError(f"accepted {suite_key} lacks exact schema registry identity")
        if strict_non_negative_int(
            registry.get("catalog_action_count"),
            f"{suite_key}.schema_registry.catalog_action_count",
        ) < 1:
            raise InventoryError(f"accepted {suite_key} schema registry must not be empty")
        registry_sha = str(registry.get("catalog_action_ids_sha256", "")).strip().lower()
        if registry.get("catalog_action_ids_hash_kind") != CATALOG_ACTION_IDS_HASH_KIND:
            raise InventoryError(f"accepted {suite_key} schema registry hash kind is invalid")
        if len(registry_sha) != 64 or any(
            char not in "0123456789abcdef" for char in registry_sha
        ):
            raise InventoryError(f"accepted {suite_key} schema registry hash is invalid")
        catalog_meta = inputs.get("mcp_catalog", {}).get("catalog", {})
        if not isinstance(catalog_meta, dict) or strict_non_negative_int(
            catalog_meta.get("action_count"),
            f"{suite_key}.mcp_catalog.catalog.action_count",
        ) != registry["catalog_action_count"]:
            raise InventoryError(
                f"accepted {suite_key} catalog count disagrees with registry identity"
            )
    if suite_key != "OfflineParity":
        status_meta = inputs.get("mcp_catalog", {}).get("status", {})
        observed_project = (
            str(status_meta.get("project_name") or status_meta.get("project") or "").strip()
            if isinstance(status_meta, dict)
            else ""
        )
        expected_project = local_project_name()
        if observed_project != expected_project:
            raise InventoryError(
                f"accepted {suite_key} status project identity mismatch: "
                f"observed={observed_project or '<missing>'} expected={expected_project}"
            )
        expected_catalog_version = current_catalog_identity()["catalog_version"]
        observed_catalog_version = (
            str(status_meta.get("catalog_version", "")).strip()
            if isinstance(status_meta, dict)
            else ""
        )
        if observed_catalog_version != expected_catalog_version:
            raise InventoryError(
                f"accepted {suite_key} catalog identity is not current: "
                f"observed={observed_catalog_version or '<missing>'} "
                f"expected={expected_catalog_version}"
            )
    return resolved_files


def add_derived_result(
    results: Dict[str, Dict[str, int]],
    namespace: str,
    classification: str,
) -> None:
    if classification not in {"pass", "fail", "expected_skip"}:
        raise InventoryError(f"unsupported derived classification: {classification}")
    result = results.setdefault(
        namespace,
        {"pass": 0, "fail": 0, "expected_skip": 0},
    )
    result[classification] += 1


def derive_offline_parity_results(
    summary_path: pathlib.Path,
    summary: Dict[str, Any],
    task_path: pathlib.Path,
) -> Dict[str, Dict[str, int]]:
    run_environment = summary.get("run_environment")
    if not isinstance(run_environment, dict) or run_environment.get("valid") is not True:
        raise InventoryError("accepted OfflineParity run environment is not valid")
    version = summary.get("version")
    if not isinstance(version, dict) or version.get("version_parity_ok") is not True:
        raise InventoryError("accepted OfflineParity executable/Python versions do not match")
    exe_revision = version.get("exe_parity_spec_rev")
    python_revision = version.get("py_parity_spec_rev")
    if (
        type(exe_revision) is not str
        or not exe_revision.strip()
        or type(python_revision) is not str
        or not python_revision.strip()
        or exe_revision != python_revision
    ):
        raise InventoryError(
            "accepted OfflineParity executable/Python revisions must be non-empty and equal"
        )

    tasks = load_evidence_jsonl(task_path)
    result_rows = load_evidence_jsonl(summary_path.parent / "per_action.jsonl")
    if len(result_rows) != len(tasks):
        raise InventoryError(
            f"accepted OfflineParity result count {len(result_rows)} != corpus {len(tasks)}"
        )

    derived: Dict[str, Dict[str, int]] = {}
    status_counts = {"MATCH": 0, "DIFF": 0, "ERROR": 0, "SKIP": 0}
    chain_inputs = summary.get("chain_inputs")
    if not isinstance(chain_inputs, dict):
        raise InventoryError("accepted OfflineParity summary lacks chain_inputs")
    decision_id = chain_inputs.get("decision_id")
    if decision_id is not None and (
        type(decision_id) is not str or not decision_id.strip()
    ):
        raise InventoryError("accepted OfflineParity decision_id must be null or non-empty string")
    seen_labels: set[str] = set()
    for index, (task, row) in enumerate(zip(tasks, result_rows), start=1):
        label = str(task.get("label", "")).strip()
        namespace = str(task.get("namespace", "")).strip()
        if not label or not namespace:
            raise InventoryError(f"OfflineParity corpus row {index} lacks label/namespace")
        if label in seen_labels:
            raise InventoryError(f"OfflineParity corpus contains duplicate label: {label}")
        seen_labels.add(label)
        if str(row.get("label", "")).strip() != label:
            raise InventoryError(f"OfflineParity result identity mismatch at row {index}: {label}")
        raw_status = str(row.get("status", "")).strip()
        status = raw_status.upper()
        if status not in status_counts:
            raise InventoryError(f"OfflineParity result {label} has invalid status: {status!r}")
        if raw_status != status:
            raise InventoryError(f"OfflineParity result {label} status must use canonical uppercase")
        diff_count = strict_non_negative_int(
            row.get("diff_count"), f"OfflineParity.{label}.diff_count"
        )
        expected_error = row.get("expected_error")
        if type(expected_error) is not bool:
            raise InventoryError(f"OfflineParity result {label}.expected_error must be boolean")
        task_expected_error = task.get("expected_error", False)
        task_offline_unsupported = task.get("offline_unsupported", False)
        requires_decision_id = task.get("requires") == "decision_id"
        if type(task_expected_error) is not bool or type(task_offline_unsupported) is not bool:
            raise InventoryError(f"OfflineParity corpus flags must be boolean: {label}")
        if expected_error is not task_expected_error:
            raise InventoryError(f"OfflineParity expected_error drifted from corpus: {label}")
        row_offline_unsupported = row.get("offline_unsupported")
        if (
            type(row_offline_unsupported) is not bool
            or row_offline_unsupported is not task_offline_unsupported
        ):
            raise InventoryError(f"OfflineParity offline_unsupported drifted from corpus: {label}")
        if status == "MATCH":
            if diff_count != 0:
                raise InventoryError(f"OfflineParity MATCH result has diffs: {label}")
            exe_exit = row.get("exe_exit_code")
            py_exit = row.get("py_exit_code")
            if type(exe_exit) is not int or type(py_exit) is not int:
                raise InventoryError(f"OfflineParity MATCH exit codes must be integers: {label}")
            expected_kind = (
                "expected"
                if expected_error
                else "expected_offline"
                if task_offline_unsupported and exe_exit != 0 and py_exit != 0
                else "none"
            )
            if row.get("error_kind") != expected_kind:
                raise InventoryError(f"OfflineParity MATCH error_kind is invalid: {label}")
            if expected_kind in {"expected", "expected_offline"}:
                if exe_exit == 0 or py_exit == 0:
                    raise InventoryError(f"OfflineParity expected MATCH must fail in both tools: {label}")
            elif exe_exit != 0 or py_exit != 0:
                raise InventoryError(f"OfflineParity normal MATCH must exit zero in both tools: {label}")
        if status == "SKIP":
            if not requires_decision_id:
                raise InventoryError(
                    f"OfflineParity result skipped a task without the decision_id prerequisite: {label}"
                )
            if decision_id is not None:
                raise InventoryError(
                    f"OfflineParity result skipped despite an available decision_id: {label}"
                )
            if (
                row.get("error_kind") != "skip"
                or row.get("error") != "current DB corpus has no decision_id input"
                or row.get("exe_exit_code") is not None
                or row.get("py_exit_code") is not None
                or diff_count != 0
            ):
                raise InventoryError(f"OfflineParity skipped result contract is invalid: {label}")
        elif requires_decision_id and decision_id is None:
            raise InventoryError(
                f"OfflineParity decision-dependent result must skip without decision_id: {label}"
            )
        status_counts[status] += 1
        add_derived_result(
            derived,
            namespace,
            "pass" if status == "MATCH" else "expected_skip" if status == "SKIP" else "fail",
        )

    counts = summary.get("counts")
    if not isinstance(counts, dict):
        raise InventoryError("accepted OfflineParity summary lacks counts")
    expected_counts = {
        "match": status_counts["MATCH"],
        "diff": status_counts["DIFF"],
        "error": status_counts["ERROR"],
        "skip": status_counts["SKIP"],
        "total": len(result_rows),
    }
    for field, expected in expected_counts.items():
        if strict_non_negative_int(counts.get(field), f"OfflineParity.counts.{field}") != expected:
            raise InventoryError(f"OfflineParity summary count {field} does not match results")

    breakdown = summary.get("metrics", {}).get("category_breakdown", {})
    if not isinstance(breakdown, dict) or set(breakdown) != set(derived):
        raise InventoryError("accepted OfflineParity summary category_breakdown is incomplete")
    for namespace, result in derived.items():
        raw = breakdown.get(namespace)
        if not isinstance(raw, dict):
            raise InventoryError(f"invalid OfflineParity summary result for {namespace}")
        expected = {
            "match": result["pass"],
            "diff": sum(
                1
                for task, row in zip(tasks, result_rows)
                if task.get("namespace") == namespace and row.get("status") == "DIFF"
            ),
            "error": sum(
                1
                for task, row in zip(tasks, result_rows)
                if task.get("namespace") == namespace and row.get("status") == "ERROR"
            ),
            "skip": result["expected_skip"],
        }
        for field, expected_value in expected.items():
            if strict_non_negative_int(
                raw.get(field), f"OfflineParity.{namespace}.{field}"
            ) != expected_value:
                raise InventoryError(
                    f"OfflineParity summary {namespace}.{field} does not match per_action evidence"
                )
    return derived


def derive_task_suite_results(
    suite_key: str,
    summary_path: pathlib.Path,
    summary: Dict[str, Any],
    task_path: pathlib.Path,
) -> Dict[str, Dict[str, int]]:
    validate_complete_summary(suite_key, summary)
    if summary.get("comparison_valid") is not True:
        raise InventoryError(f"accepted {suite_key} must be a canonical full-corpus run")
    start_identity = summary.get("status_identity_start")
    end_identity = summary.get("status_identity_end")
    if not isinstance(start_identity, dict) or not isinstance(end_identity, dict):
        raise InventoryError(
            f"accepted {suite_key} requires identical local-project status identities"
        )
    missing_identity_fields = [
        field
        for field in REQUIRED_LIVE_STATUS_IDENTITY_FIELDS
        if not str(start_identity.get(field, "")).strip()
        or not str(end_identity.get(field, "")).strip()
    ]
    if missing_identity_fields or start_identity != end_identity:
        raise InventoryError(
            f"accepted {suite_key} requires complete identical status identities: "
            f"missing={missing_identity_fields}"
        )
    expected_catalog_version = current_catalog_identity()["catalog_version"]
    input_status = summary.get("benchmark_inputs", {}).get("mcp_catalog", {}).get("status", {})
    input_catalog_version = (
        str(input_status.get("catalog_version", "")).strip()
        if isinstance(input_status, dict)
        else ""
    )
    if (
        start_identity["project"] != local_project_name()
        or start_identity["catalog_version"] != expected_catalog_version
        or input_catalog_version != expected_catalog_version
    ):
        raise InventoryError(
            f"accepted {suite_key} status/input/catalog identities do not match the current catalog"
        )
    if suite_key == "AssetEditing":
        selection = summary.get("task_selection")
        if not isinstance(selection, dict) or selection.get("is_subset") is not False:
            raise InventoryError("accepted AssetEditing evidence must not be a selected subset")
        if selection.get("score_comparable_to_full_run") is not True:
            raise InventoryError("accepted AssetEditing selection must be full-run comparable")
        if summary.get("benchmark_inputs", {}).get("task_selection") != selection:
            raise InventoryError("accepted AssetEditing task selection is not fingerprinted exactly")
        execution = summary.get("execution")
        if (
            not isinstance(execution, dict)
            or execution.get("mode") != "sequential"
            or type(execution.get("jobs")) is not int
            or execution.get("jobs") != 1
        ):
            raise InventoryError("accepted AssetEditing evidence must use sequential jobs=1 execution")

    tasks = load_evidence_jsonl(task_path)
    result_rows = load_evidence_jsonl(summary_path.parent / "per_task.jsonl")
    if strict_non_negative_int(summary.get("task_count"), f"{suite_key}.task_count") != len(tasks):
        raise InventoryError(f"accepted {suite_key} summary task_count does not match corpus")
    if len(result_rows) != len(tasks):
        raise InventoryError(
            f"accepted {suite_key} result count {len(result_rows)} != corpus {len(tasks)}"
        )

    tasks_by_id: Dict[str, Dict[str, Any]] = {}
    for index, task in enumerate(tasks, start=1):
        task_id = str(task.get("id", "")).strip()
        namespace = str(task.get("namespace", "")).strip()
        action = str(task.get("action", "")).strip()
        if not task_id or not namespace or not action:
            raise InventoryError(f"{suite_key} corpus row {index} lacks id/namespace/action")
        if task_id in tasks_by_id:
            raise InventoryError(f"{suite_key} corpus contains duplicate task id: {task_id}")
        tasks_by_id[task_id] = task

    if suite_key == "AssetEditing":
        selection = summary["task_selection"]
        if strict_non_negative_int(
            selection.get("task_count"), "AssetEditing.task_selection.task_count"
        ) != len(tasks):
            raise InventoryError("accepted AssetEditing selection count does not match corpus")
        if selection.get("task_ids") != list(tasks_by_id):
            raise InventoryError("accepted AssetEditing selected task ids do not match corpus order")
    else:
        corpus = summary.get("task_corpus")
        if not isinstance(corpus, dict):
            raise InventoryError(f"accepted {suite_key} summary lacks task_corpus metadata")
        if (
            corpus.get("mode") != "canonical"
            or corpus.get("canonical") is not True
            or corpus.get("comparable") is not True
            or strict_non_negative_int(
                corpus.get("validated_task_count"),
                f"{suite_key}.task_corpus.validated_task_count",
            ) != len(tasks)
        ):
            raise InventoryError(f"accepted {suite_key} task_corpus is not canonical and complete")

    results_by_id: Dict[str, Dict[str, Any]] = {}
    for index, row in enumerate(result_rows, start=1):
        task_id = str(row.get("task_id", "")).strip()
        if not task_id:
            raise InventoryError(f"{suite_key} result row {index} lacks task_id")
        if task_id in results_by_id:
            raise InventoryError(f"{suite_key} results contain duplicate task id: {task_id}")
        results_by_id[task_id] = row
    if set(results_by_id) != set(tasks_by_id):
        raise InventoryError(f"accepted {suite_key} result ids do not match the canonical corpus")
    if list(results_by_id) != list(tasks_by_id):
        raise InventoryError(f"accepted {suite_key} result order does not match the canonical corpus")

    derived: Dict[str, Dict[str, int]] = {}
    success_field = TASK_RESULT_FIELDS[suite_key]
    transport_failure_count = 0
    for task_id, task in tasks_by_id.items():
        row = results_by_id[task_id]
        namespace = str(task["namespace"]).strip()
        if str(row.get("namespace", "")).strip() != namespace:
            raise InventoryError(f"{suite_key} result namespace mismatch for {task_id}")
        if str(row.get("action", "")).strip() != str(task.get("action", "")).strip():
            raise InventoryError(f"{suite_key} result action mismatch for {task_id}")
        task_category = task.get("category")
        if task_category is not None and row.get("category") != task_category:
            raise InventoryError(f"{suite_key} result category mismatch for {task_id}")
        success = row.get(success_field)
        if type(success) is not bool:
            raise InventoryError(f"{suite_key} result {task_id}.{success_field} must be boolean")
        transport_error = row.get("transport_error")
        if type(transport_error) is not bool:
            raise InventoryError(f"{suite_key} result {task_id}.transport_error must be boolean")
        transport_failure_count += int(transport_error)
        if success and transport_error:
            if suite_key != "ActionGuidance":
                raise InventoryError(
                    f"{suite_key} result {task_id} cannot pass with a transport failure"
                )
            if (
                row.get("direct_success") is not False
                or type(row.get("transport_failure_call_count")) is not int
                or row.get("transport_failure_call_count") < 1
            ):
                raise InventoryError(
                    f"ActionGuidance result {task_id} lacks explicit recovery provenance"
                )
        protocol_error = row.get("protocol_error")
        if (
            suite_key in REQUIRED_PROTOCOL_RESULT_SUITES
            and type(protocol_error) is not bool
        ):
            raise InventoryError(
                f"{suite_key} result {task_id}.protocol_error must be a required boolean"
            )
        if "protocol_error" in row and type(protocol_error) is not bool:
            raise InventoryError(
                f"{suite_key} result {task_id}.protocol_error must be boolean"
            )
        if success and protocol_error is True:
            raise InventoryError(
                f"{suite_key} result {task_id} cannot pass with a protocol failure"
            )
        failure_kind = row.get("failure_kind")
        if (
            suite_key in REQUIRED_FAILURE_KIND_RESULT_SUITES
            and type(failure_kind) is not str
        ):
            raise InventoryError(
                f"{suite_key} result {task_id}.failure_kind must be a required string"
            )
        if "failure_kind" in row and type(failure_kind) is not str:
            raise InventoryError(
                f"{suite_key} result {task_id}.failure_kind must be a string"
            )
        if success and isinstance(failure_kind, str) and failure_kind.strip():
            raise InventoryError(
                f"{suite_key} result {task_id} cannot pass with failure_kind={failure_kind!r}"
            )
        add_derived_result(derived, namespace, "pass" if success else "fail")
    transport_summary = summary.get("transport") if suite_key == "AssetEditing" else summary
    if not isinstance(transport_summary, dict) or strict_non_negative_int(
        transport_summary.get("transport_failure_count"),
        f"{suite_key}.transport_failure_count",
    ) != transport_failure_count:
        raise InventoryError(f"accepted {suite_key} transport count does not match per_task evidence")
    return derived


def validate_task_execution_contract(
    suite_key: str,
    summary: Dict[str, Any],
    manifest_path: pathlib.Path,
) -> None:
    """Bind result-affecting runner gates to the checked-in canonical manifest."""
    manifest = load_json(manifest_path)
    if suite_key in CURRENT_CATALOG_MANIFEST_SUITES:
        observed_catalog_version = str(manifest.get("catalog_version", "")).strip()
        expected_catalog_version = current_catalog_identity()["catalog_version"]
        if observed_catalog_version != expected_catalog_version:
            raise InventoryError(
                f"accepted {suite_key} canonical manifest catalog_version is not current: "
                f"observed={observed_catalog_version or '<missing>'} "
                f"expected={expected_catalog_version}"
            )
    run_gates = manifest.get("run_gates")
    if not isinstance(run_gates, dict):
        if suite_key == "AssetEditing":
            return
        raise InventoryError(f"accepted {suite_key} manifest lacks run_gates")
    for field in (
        "max_transport_failed_fraction",
        "max_consecutive_transport_failures",
        "min_transport_fraction_sample",
    ):
        if field not in run_gates:
            raise InventoryError(f"accepted {suite_key} manifest run_gates lacks {field}")
        if type(summary.get(field)) is not type(run_gates[field]) or (
            summary.get(field) != run_gates[field]
        ):
            raise InventoryError(
                f"accepted {suite_key} execution setting {field} drifted: "
                f"observed={summary.get(field)!r} expected={run_gates[field]!r}"
            )
    if suite_key == "ActionGuidance":
        expected_recovery_calls = run_gates.get("default_max_recovery_calls")
        if type(expected_recovery_calls) is not int or expected_recovery_calls < 1:
            raise InventoryError(
                "accepted ActionGuidance manifest has invalid default_max_recovery_calls"
            )
        if type(summary.get("max_recovery_calls")) is not int or (
            summary.get("max_recovery_calls") != expected_recovery_calls
        ):
            raise InventoryError(
                "accepted ActionGuidance max_recovery_calls differs from the canonical contract"
            )


def derive_schema_probe_results(
    summary_path: pathlib.Path,
    summary: Dict[str, Any],
    probe_set_path: pathlib.Path,
) -> Dict[str, Dict[str, int]]:
    suite_key = "SchemaCompletenessProbe"
    validate_complete_summary(suite_key, summary)
    probes = load_evidence_jsonl(probe_set_path)
    results = load_evidence_jsonl(summary_path.parent / "probe_results.jsonl")
    if len(results) != len(probes):
        raise InventoryError(
            f"accepted {suite_key} result count {len(results)} != probe contract {len(probes)}"
        )

    manifest = load_json(probe_set_path.parent / "manifest.json")
    raw_dimensions = manifest.get("scoring", {}).get("dimensions", [])
    schema_dimensions = [
        str(entry.get("name", "")).strip()
        for entry in raw_dimensions
        if isinstance(entry, dict)
    ]
    if (
        not schema_dimensions
        or any(not name for name in schema_dimensions)
        or len(schema_dimensions) != len(set(schema_dimensions))
    ):
        raise InventoryError("SchemaCompleteness manifest dimensions are invalid")
    if schema_dimensions != list(CHECKPOINT_DIMENSION_FIELDS):
        raise InventoryError(
            "SchemaCompleteness manifest dimensions drifted from the canonical runner contract"
        )

    action_rows = load_evidence_jsonl(summary_path.parent / "per_action.jsonl")
    action_rows_by_id: Dict[str, Dict[str, Any]] = {}
    for row in action_rows:
        namespace = str(row.get("namespace", "")).strip()
        action = str(row.get("action", "")).strip()
        action_id = f"{namespace}.{action}" if namespace and action else ""
        if not action_id:
            raise InventoryError("SchemaCompleteness per_action row lacks namespace/action")
        if action_id in action_rows_by_id:
            raise InventoryError(f"SchemaCompleteness per_action contains duplicate id: {action_id}")
        action_rows_by_id[action_id] = row

    derived: Dict[str, Dict[str, int]] = {}
    probe_ids: set[str] = set()
    present_ids: set[str] = set()
    scored_count = 0
    fetch_failed_count = 0
    skipped_count = 0
    failed_count = 0
    for index, (probe, result) in enumerate(zip(probes, results), start=1):
        namespace = str(probe.get("namespace", "")).strip()
        action = str(probe.get("action", "")).strip()
        action_id = f"{namespace}.{action}"
        if not namespace or not action:
            raise InventoryError(f"SchemaCompleteness probe row {index} lacks namespace/action")
        if action_id in probe_ids:
            raise InventoryError(f"SchemaCompleteness probe contract contains duplicate id: {action_id}")
        probe_ids.add(action_id)
        if strict_non_negative_int(
            result.get("probe_index"), f"SchemaCompletenessProbe.{action_id}.probe_index"
        ) != index:
            raise InventoryError(f"SchemaCompleteness probe index mismatch for {action_id}")
        if (
            str(result.get("namespace", "")).strip() != namespace
            or str(result.get("action", "")).strip() != action
        ):
            raise InventoryError(f"SchemaCompleteness probe identity mismatch at row {index}")
        availability = probe.get("availability", {"mode": "required"})
        if not isinstance(availability, dict):
            raise InventoryError(f"SchemaCompleteness probe availability is invalid: {action_id}")
        mode = availability.get("mode", "required")
        normalized_availability = dict(availability)
        normalized_availability["mode"] = mode
        if result.get("availability") != normalized_availability:
            raise InventoryError(f"SchemaCompleteness probe availability drifted: {action_id}")
        if result.get("expected_dimensions") != probe.get("expected_dimensions"):
            raise InventoryError(f"SchemaCompleteness expected dimensions drifted: {action_id}")
        expected_dimensions = probe.get("expected_dimensions")
        if (
            not isinstance(expected_dimensions, list)
            or not expected_dimensions
            or any(
                type(dimension) is not str or dimension not in schema_dimensions
                for dimension in expected_dimensions
            )
            or len(expected_dimensions) != len(set(expected_dimensions))
        ):
            raise InventoryError(f"SchemaCompleteness expected dimensions are invalid: {action_id}")

        result_status = result.get("result_status")
        if result_status in {"scored", "fetch_failed"}:
            action_row = action_rows_by_id.get(action_id)
            if action_row is None:
                raise InventoryError(f"SchemaCompleteness probe lacks per_action evidence: {action_id}")
            dimension_results = result.get("dimension_results")
            if not isinstance(dimension_results, dict) or set(dimension_results) != set(schema_dimensions):
                raise InventoryError(f"SchemaCompleteness dimension_results are incomplete: {action_id}")
            for dimension in schema_dimensions:
                value = dimension_results[dimension]
                if value is not None and type(value) is not bool:
                    raise InventoryError(
                        f"SchemaCompleteness dimension result must be boolean/null: "
                        f"{action_id}.{dimension}"
                    )
                if action_row.get(dimension) is not value:
                    raise InventoryError(
                        f"SchemaCompleteness probe/per_action dimension mismatch: "
                        f"{action_id}.{dimension}"
                    )
            expected_passed = [
                dimension for dimension in expected_dimensions
                if dimension_results[dimension] is True
            ]
            expected_failed = [
                dimension for dimension in expected_dimensions
                if dimension_results[dimension] is False
            ]
            expected_na = [
                dimension for dimension in expected_dimensions
                if dimension_results[dimension] is None
            ]
            if (
                result.get("expected_passed") != expected_passed
                or result.get("expected_failed") != expected_failed
                or result.get("expected_na") != expected_na
            ):
                raise InventoryError(
                    f"SchemaCompleteness derived dimension classifications drifted: {action_id}"
                )
            recomputed_probe_pass = not expected_failed and bool(expected_passed)
            if result.get("probe_pass") is not recomputed_probe_pass:
                raise InventoryError(f"SchemaCompleteness probe_pass is not derived: {action_id}")
            if result.get("catalog_presence") != "present":
                raise InventoryError(f"SchemaCompleteness probe must be catalog-present: {action_id}")
            if not exact_json_equal(
                result.get("value_domain_diagnostics"),
                action_row.get("value_domain_diagnostics"),
            ):
                raise InventoryError(
                    "SchemaCompleteness probe/per_action value-domain diagnostics mismatch: "
                    f"{action_id}"
                )
            present_ids.add(action_id)

        if result_status == "scored":
            action_row = action_rows_by_id[action_id]
            try:
                validate_checkpoint_result_row(
                    action_row,
                    expected_action_ids={action_id},
                )
            except ValueError as exc:
                raise InventoryError(
                    f"SchemaCompleteness scored row is not reusable: {exc}"
                ) from exc
            probe_pass = result["probe_pass"]
            scored_count += 1
            failed_count += int(not probe_pass)
            add_derived_result(derived, namespace, "pass" if probe_pass else "fail")
        elif result_status == "fetch_failed":
            action_row = action_rows_by_id[action_id]
            try:
                validate_schema_result_row(
                    action_row,
                    expected_action_ids={action_id},
                )
            except ValueError as exc:
                raise InventoryError(
                    f"SchemaCompleteness fetch-failed row violates evidence contract: {exc}"
                ) from exc
            if (
                result.get("failure_kind") in {None, "", "ok"}
                or result.get("failure_kind") != action_row.get("failure_kind")
                or result.get("transport_error") is not action_row.get("transport_error")
                or result.get("error") != action_row.get("error")
            ):
                raise InventoryError(f"SchemaCompleteness fetch failure lacks failure_kind: {action_id}")
            fetch_failed_count += 1
            failed_count += 1
            add_derived_result(derived, namespace, "fail")
        elif result_status == "skipped":
            if mode not in {"optional", "feature_gated"}:
                raise InventoryError(f"required SchemaCompleteness probe was skipped: {action_id}")
            if (
                result.get("catalog_presence") != "absent"
                or result.get("diagnostic_code") != f"{mode}_probe_absent"
                or result.get("probe_pass") is not None
                or result.get("dimension_results") is not None
                or result.get("expected_passed") is not None
                or result.get("expected_failed") is not None
                or result.get("expected_na") is not None
            ):
                raise InventoryError(f"SchemaCompleteness skipped probe contract is invalid: {action_id}")
            skipped_count += 1
            add_derived_result(derived, namespace, "expected_skip")
        else:
            raise InventoryError(
                f"SchemaCompleteness probe {action_id} has invalid result_status: {result_status!r}"
            )

    if set(action_rows_by_id) != present_ids:
        raise InventoryError("SchemaCompleteness per_action ids do not match catalog-present probes")

    preflight = load_json(summary_path.parent / "probe_preflight.json")
    registry = summary.get("benchmark_inputs", {}).get("schema_registry", {})
    if (
        strict_non_negative_int(
            preflight.get("catalog_action_count"),
            "SchemaCompletenessProbe.probe_preflight.catalog_action_count",
        ) != registry.get("catalog_action_count")
        or preflight.get("catalog_action_ids_sha256")
        != registry.get("catalog_action_ids_sha256")
        or preflight.get("catalog_action_ids_hash_kind")
        != CATALOG_ACTION_IDS_HASH_KIND
        or strict_non_negative_int(
            preflight.get("declared_probe_count"),
            "SchemaCompletenessProbe.probe_preflight.declared_probe_count",
        ) != len(probes)
        or strict_non_negative_int(
            preflight.get("runnable_probe_count"),
            "SchemaCompletenessProbe.probe_preflight.runnable_probe_count",
        ) != len(present_ids)
        or strict_non_negative_int(
            preflight.get("skipped_probe_count"),
            "SchemaCompletenessProbe.probe_preflight.skipped_probe_count",
        ) != skipped_count
        or strict_non_negative_int(
            preflight.get("stale_probe_count"),
            "SchemaCompletenessProbe.probe_preflight.stale_probe_count",
        ) != 0
    ):
        raise InventoryError("SchemaCompleteness probe preflight does not match accepted evidence")

    probe_metrics = summary.get("probe_metrics")
    if not isinstance(probe_metrics, dict):
        raise InventoryError("accepted SchemaCompletenessProbe summary lacks probe_metrics")
    expected_metrics = {
        "declared_probe_count": len(probes),
        "catalog_present_probe_count": len(present_ids),
        "scored_probe_count": scored_count,
        "fetch_failed_probe_count": fetch_failed_count,
        "skipped_probe_count": skipped_count,
        "failed_probe_count": failed_count,
    }
    for field, expected in expected_metrics.items():
        if strict_non_negative_int(
            probe_metrics.get(field), f"SchemaCompletenessProbe.probe_metrics.{field}"
        ) != expected:
            raise InventoryError(f"SchemaCompletenessProbe summary {field} does not match results")
    return derived


def derive_schema_full_results(
    summary_path: pathlib.Path,
    summary: Dict[str, Any],
) -> Dict[str, Dict[str, int]]:
    validate_complete_summary(FULL_CATALOG_KEY, summary)
    if summary.get("comparable") is not True:
        raise InventoryError(f"accepted {FULL_CATALOG_KEY} comparable must be true")
    rows = load_evidence_jsonl(summary_path.parent / "per_action.jsonl")
    derived: Dict[str, Dict[str, int]] = {}
    action_ids: set[str] = set()
    quality_failed_count = 0
    for index, row in enumerate(rows, start=1):
        namespace = str(row.get("namespace", "")).strip()
        action = str(row.get("action", "")).strip()
        if not namespace or not action:
            raise InventoryError(f"SchemaCompleteness full result row {index} lacks namespace/action")
        action_id = f"{namespace}.{action}"
        if action_id in action_ids:
            raise InventoryError(f"SchemaCompleteness full results contain duplicate id: {action_id}")
        action_ids.add(action_id)
        try:
            validate_checkpoint_result_row(row, expected_action_ids={action_id})
        except ValueError as exc:
            raise InventoryError(
                f"SchemaCompleteness full result is not reusable: {exc}"
            ) from exc
        quality_passed = schema_quality_pass(row)
        quality_failed_count += int(not quality_passed)
        add_derived_result(derived, namespace, "pass" if quality_passed else "fail")

    expected_namespace_breakdown = build_schema_namespace_breakdown(rows)
    if not exact_json_equal(
        summary.get("namespace_breakdown"), expected_namespace_breakdown
    ):
        raise InventoryError(
            "SchemaCompleteness full namespace_breakdown is not derived from per_action evidence"
        )
    recomputed_summary = aggregate_schema_metrics(
        str(summary.get("label", "")),
        rows,
        len(rows),
        expected_namespace_breakdown,
    )
    if not exact_json_equal(summary.get("metrics"), recomputed_summary.get("metrics")):
        raise InventoryError(
            "SchemaCompleteness full metrics are not derived from per_action evidence"
        )
    namespace_breakdown_path = summary_path.parent / "namespace_breakdown.json"
    if not namespace_breakdown_path.is_file() or not exact_json_equal(
        load_json(namespace_breakdown_path), expected_namespace_breakdown
    ):
        raise InventoryError(
            "SchemaCompleteness full namespace_breakdown.json is not derived from per_action evidence"
        )

    if strict_non_negative_int(
        summary.get("scanned_action_count"), f"{FULL_CATALOG_KEY}.scanned_action_count"
    ) != len(rows):
        raise InventoryError("SchemaCompleteness full scanned_action_count does not match results")
    if strict_non_negative_int(
        summary.get("total_expected_action_count"),
        f"{FULL_CATALOG_KEY}.total_expected_action_count",
    ) != len(rows):
        raise InventoryError("SchemaCompleteness full result is not an exact-catalog scan")
    if strict_non_negative_int(
        summary.get("failed_action_count"), f"{FULL_CATALOG_KEY}.failed_action_count"
    ) != 0:
        raise InventoryError("SchemaCompleteness full failed_action_count does not match results")
    for field in (
        "completed_action_count",
        "completed_valid_action_count",
        "total_action_count",
        "full_catalog_action_count",
    ):
        if strict_non_negative_int(
            summary.get(field), f"{FULL_CATALOG_KEY}.{field}"
        ) != len(rows):
            raise InventoryError(f"SchemaCompleteness full {field} does not match results")
    if strict_non_negative_int(
        summary.get("remaining_action_count"),
        f"{FULL_CATALOG_KEY}.remaining_action_count",
    ) != 0:
        raise InventoryError("SchemaCompleteness full run still declares remaining actions")
    if strict_non_negative_int(
        summary.get("namespace_count"), f"{FULL_CATALOG_KEY}.namespace_count"
    ) != len(derived):
        raise InventoryError("SchemaCompleteness full namespace_count does not match results")
    registry = summary.get("benchmark_inputs", {}).get("schema_registry", {})
    if strict_non_negative_int(
        registry.get("catalog_action_count"),
        f"{FULL_CATALOG_KEY}.schema_registry.catalog_action_count",
    ) != len(action_ids):
        raise InventoryError("SchemaCompleteness full registry action count does not match results")
    observed_action_hash = hashlib.sha256(
        json.dumps(
            sorted(action_ids), sort_keys=True, separators=(",", ":"), ensure_ascii=True
        ).encode("utf-8")
    ).hexdigest()
    if registry.get("catalog_action_ids_sha256") != observed_action_hash:
        raise InventoryError("SchemaCompleteness full action identities do not match the input registry")
    if registry.get("catalog_action_ids_hash_kind") != CATALOG_ACTION_IDS_HASH_KIND:
        raise InventoryError("SchemaCompleteness full action identity hash kind drifted")
    provenance = summary.get("checkpoint_provenance")
    if (
        not isinstance(provenance, dict)
        or provenance.get("catalog_action_ids_hash_kind") != CATALOG_ACTION_IDS_HASH_KIND
        or provenance.get("catalog_action_ids_sha256") != observed_action_hash
        or not str(provenance.get("catalog_version", "")).strip()
    ):
        raise InventoryError("SchemaCompleteness full checkpoint provenance is incomplete")
    input_status = summary.get("benchmark_inputs", {}).get("mcp_catalog", {}).get("status", {})
    if (
        not isinstance(input_status, dict)
        or provenance.get("catalog_version") != input_status.get("catalog_version")
    ):
        raise InventoryError("SchemaCompleteness full catalog version provenance drifted")
    checkpoint_path = summary_path.parent / "scan_checkpoint.json"
    if not checkpoint_path.is_file():
        raise InventoryError("SchemaCompleteness full accepted evidence lacks scan_checkpoint.json")
    checkpoint = load_json(checkpoint_path)
    resume_identity = checkpoint.get("resume_identity")
    if (
        checkpoint.get("schema_version") != SCAN_CHECKPOINT_SCHEMA_VERSION
        or checkpoint.get("benchmark") != "SchemaCompleteness"
        or checkpoint.get("scan_scope") != "full_catalog"
        or checkpoint.get("state") != "completed"
        or checkpoint.get("results_file") != "per_action.jsonl"
        or not isinstance(resume_identity, dict)
    ):
        raise InventoryError("SchemaCompleteness full checkpoint is not completed")
    if strict_non_negative_int(
        checkpoint.get("completed_valid_action_count"),
        f"{FULL_CATALOG_KEY}.checkpoint.completed_valid_action_count",
    ) != len(rows):
        raise InventoryError("SchemaCompleteness full checkpoint result count drifted")
    if not exact_json_equal(
        checkpoint.get("benchmark_inputs"), summary.get("benchmark_inputs")
    ):
        raise InventoryError("SchemaCompleteness full checkpoint benchmark inputs drifted")
    if (
        resume_identity.get("catalog_version") != provenance.get("catalog_version")
        or strict_non_negative_int(
            resume_identity.get("catalog_action_count"),
            f"{FULL_CATALOG_KEY}.checkpoint.catalog_action_count",
        ) != len(rows)
        or resume_identity.get("catalog_action_ids_sha256") != observed_action_hash
        or resume_identity.get("catalog_action_ids_hash_kind")
        != CATALOG_ACTION_IDS_HASH_KIND
        or resume_identity.get("benchmark_input_fingerprint")
        != summary.get("input_fingerprint")
    ):
        raise InventoryError("SchemaCompleteness full checkpoint identity drifted")
    provenance_lists = {
        "segment_count": "segments",
        "outage_count": "outages",
        "invalid_attempt_count": "invalid_attempts",
        "recovery_event_count": "recovery_events",
    }
    for provenance_field, checkpoint_field in provenance_lists.items():
        rows_value = checkpoint.get(checkpoint_field)
        if not isinstance(rows_value, list) or strict_non_negative_int(
            provenance.get(provenance_field),
            f"{FULL_CATALOG_KEY}.checkpoint_provenance.{provenance_field}",
        ) != len(rows_value):
            raise InventoryError(
                f"SchemaCompleteness full checkpoint provenance drifted: {checkpoint_field}"
            )
    segments = checkpoint["segments"]
    if not segments or not isinstance(segments[-1], dict) or (
        segments[-1].get("completion_status") != "completed"
    ):
        raise InventoryError("SchemaCompleteness full checkpoint has no completed final segment")
    return derived


def validate_accepted_evidence(
    suite_key: str,
    suite_status: Dict[str, Any],
    namespace_results: Dict[str, Any],
    *,
    portable: bool = False,
    validation_report: Dict[str, List[str]] | None = None,
) -> None:
    evidence = str(suite_status.get("evidence", "")).strip()
    if not evidence:
        raise InventoryError(f"accepted suite {suite_key} requires an evidence path")
    summary_path = resolve_monolith_relative(evidence, f"accepted {suite_key} evidence")
    if not summary_path.is_file():
        raise InventoryError(f"accepted evidence is missing for {suite_key}: {summary_path}")
    bundle_manifest = load_accepted_bundle(
        suite_key,
        suite_status,
        summary_path,
        portable=portable,
    )
    stale_invalid_artifacts = [
        name
        for name in ("run_failure.json", "partial_summary.json")
        if (summary_path.parent / name).exists()
    ]
    if stale_invalid_artifacts:
        raise InventoryError(
            f"accepted {suite_key} evidence coexists with invalid-run artifacts: "
            f"{stale_invalid_artifacts}"
        )
    summary = load_json(summary_path)
    input_files = validate_input_evidence(
        suite_key,
        suite_status,
        summary,
        portable=portable,
        bundle_manifest=bundle_manifest,
        validation_report=validation_report,
    )
    if suite_key == "OfflineParity":
        observed_results = derive_offline_parity_results(
            summary_path, summary, input_files["tasks"]
        )
    elif suite_key in TASK_RESULT_FIELDS:
        validate_task_execution_contract(
            suite_key,
            summary,
            input_files["manifest"],
        )
        observed_results = derive_task_suite_results(
            suite_key, summary_path, summary, input_files["tasks"]
        )
    elif suite_key == "SchemaCompletenessProbe":
        observed_results = derive_schema_probe_results(
            summary_path, summary, input_files["probe_set"]
        )
    elif suite_key == FULL_CATALOG_KEY:
        observed_results = derive_schema_full_results(summary_path, summary)
    else:
        raise InventoryError(f"accepted evidence adapter is missing for {suite_key}")

    normalized_declared = normalize_namespace_results(suite_key, namespace_results)
    if observed_results != normalized_declared:
        raise InventoryError(
            f"accepted {suite_key} namespace results do not match derived evidence"
        )


def corpus_rows(spec: CorpusSpec) -> List[Dict[str, Any]]:
    manifest = load_json(BENCHMARK_ROOT / spec.manifest)
    declared = manifest.get(spec.count_field)
    if not isinstance(declared, int) or isinstance(declared, bool) or declared < 0:
        raise InventoryError(
            f"{spec.manifest}:{spec.count_field} must be a non-negative integer"
        )
    source_rows = load_jsonl(BENCHMARK_ROOT / spec.jsonl)
    if len(source_rows) > declared:
        raise InventoryError(
            f"{spec.key} corpus has {len(source_rows)} rows but manifest declares {declared}"
        )

    counts = collections.Counter(str(row["namespace"]).strip() for row in source_rows)
    rows = [
        {
            "suite": spec.key,
            "label": spec.label,
            "namespace": namespace,
            "items": count,
            "unwritten": 0,
        }
        for namespace, count in sorted(counts.items())
    ]
    missing = declared - len(source_rows)
    if missing:
        rows.append(
            {
                "suite": spec.key,
                "label": spec.label,
                "namespace": "(unassigned missing rows)",
                "items": missing,
                "unwritten": missing,
            }
        )
    return rows


def full_catalog_rows() -> tuple[List[Dict[str, Any]], Dict[str, Any]]:
    manifest = load_json(BENCHMARK_ROOT / "ActionGuidance/manifest.json")
    coverage = manifest.get("namespace_coverage")
    if not isinstance(coverage, list) or not coverage:
        raise InventoryError("ActionGuidance manifest has no namespace_coverage snapshot")

    rows: List[Dict[str, Any]] = []
    seen = set()
    for index, entry in enumerate(coverage):
        if not isinstance(entry, dict):
            raise InventoryError(f"namespace_coverage[{index}] must be an object")
        namespace = str(entry.get("namespace", "")).strip()
        count = entry.get("action_count")
        if not namespace or namespace in seen:
            raise InventoryError(f"invalid/duplicate live namespace: {namespace!r}")
        if not isinstance(count, int) or isinstance(count, bool) or count <= 0:
            raise InventoryError(f"invalid action_count for live namespace {namespace!r}")
        seen.add(namespace)
        rows.append(
            {
                "suite": FULL_CATALOG_KEY,
                "label": FULL_CATALOG_LABEL,
                "namespace": namespace,
                "items": count,
                "unwritten": 0,
            }
        )

    expected_namespace_count = manifest.get("catalog_namespace_count")
    expected_action_count = manifest.get("catalog_action_count")
    if len(rows) != expected_namespace_count:
        raise InventoryError(
            f"live catalog namespace count {len(rows)} != manifest {expected_namespace_count}"
        )
    if sum(row["items"] for row in rows) != expected_action_count:
        raise InventoryError(
            "live catalog action total does not match ActionGuidance manifest"
        )
    return rows, {
        "version": str(manifest.get("catalog_version", "")),
        "namespace_count": expected_namespace_count,
        "action_count": expected_action_count,
        "generated_at": str(manifest.get("generated_at", "")),
    }


def apply_status(
    rows: List[Dict[str, Any]],
    status: Dict[str, Any],
    *,
    portable: bool = False,
    validation_report: Dict[str, List[str]] | None = None,
) -> None:
    suites = status.get("suites")
    if not isinstance(suites, dict):
        raise InventoryError("inventory_status.json requires a suites object")

    row_keys = {row["suite"] for row in rows}
    if set(suites) != row_keys:
        raise InventoryError(
            "inventory status suite keys differ from derived inventory: "
            f"status={sorted(suites)} derived={sorted(row_keys)}"
        )

    gaps = status.get("gaps")
    if not isinstance(gaps, list):
        raise InventoryError("inventory_status.json requires a gaps array")
    if any(not isinstance(gap, dict) for gap in gaps):
        raise InventoryError("every gap must be an object")
    gap_ids = [str(gap.get("id", "")).strip() for gap in gaps]
    if any(not gap_id for gap_id in gap_ids):
        raise InventoryError("every gap requires a non-empty id")
    if len(gap_ids) != len(set(gap_ids)):
        raise InventoryError("gap ids must be unique")
    gap_for_suite: Dict[str, str] = {}
    for gap, gap_id in zip(gaps, gap_ids):
        gap_suites = gap.get("suites")
        if (
            not isinstance(gap_suites, list)
            or not gap_suites
            or any(type(suite) is not str or not suite.strip() for suite in gap_suites)
        ):
            raise InventoryError(f"gap {gap_id} requires a non-empty suites array")
        normalized_suites = [suite.strip() for suite in gap_suites]
        if len(normalized_suites) != len(set(normalized_suites)):
            raise InventoryError(f"gap {gap_id} contains duplicate suites")
        unknown_suites = set(normalized_suites) - row_keys
        if unknown_suites:
            raise InventoryError(f"gap {gap_id} contains unknown suites: {sorted(unknown_suites)}")
        for suite_key in normalized_suites:
            if suite_key in gap_for_suite:
                raise InventoryError(
                    f"suite {suite_key} is assigned to multiple gaps: "
                    f"{gap_for_suite[suite_key]}, {gap_id}"
                )
            gap_for_suite[suite_key] = gap_id

    execution_gates = status.get("execution_gates")
    if not isinstance(execution_gates, list) or any(
        not isinstance(gate, dict) for gate in execution_gates
    ):
        raise InventoryError("inventory_status.json requires an execution_gates array")
    gate_ids = [str(gate.get("id", "")).strip() for gate in execution_gates]
    if len(gate_ids) != len(set(gate_ids)):
        raise InventoryError("execution gate ids must be unique")
    if set(gate_ids) != set(REQUIRED_EXECUTION_GATE_IDS):
        raise InventoryError(
            "execution gate ids differ from the fixed Done contract: "
            f"observed={sorted(gate_ids)} expected={sorted(REQUIRED_EXECUTION_GATE_IDS)}"
        )
    for gate, gate_id in zip(execution_gates, gate_ids):
        gate_status = str(gate.get("status", "")).strip()
        if gate_status not in ALLOWED_EXECUTION_GATE_STATUSES:
            raise InventoryError(f"execution gate {gate_id} has invalid status: {gate_status!r}")
        if not str(gate.get("contract", "")).strip():
            raise InventoryError(f"execution gate {gate_id} requires a contract")
        if gate_status == "passed" and not str(gate.get("evidence", "")).strip():
            raise InventoryError(f"passed execution gate {gate_id} requires evidence")

    for suite_key, suite_status in suites.items():
        if not isinstance(suite_status, dict):
            raise InventoryError(f"status for {suite_key} must be an object")
        state = str(suite_status.get("state", "")).strip()
        if state not in {"pending", "accepted"}:
            raise InventoryError(f"unsupported state for {suite_key}: {state!r}")
        namespace_results = suite_status.get("namespace_results", {})
        if not isinstance(namespace_results, dict):
            raise InventoryError(f"namespace_results for {suite_key} must be an object")
        if state == "pending" and namespace_results:
            raise InventoryError(
                f"pending suite {suite_key} cannot claim pass/fail/skip result credit"
            )
        evidence = str(suite_status.get("evidence", "")).strip()
        if state == "pending" and evidence:
            evidence_path = resolve_monolith_relative(
                evidence,
                f"pending {suite_key} evidence",
            )
            if not evidence_path.is_file():
                if portable:
                    if validation_report is not None:
                        validation_report.setdefault("omitted_pending_evidence", []).append(
                            evidence.replace("\\", "/")
                        )
                else:
                    raise InventoryError(
                        f"pending evidence is missing for {suite_key}: {evidence_path}"
                    )

        suite_rows = [row for row in rows if row["suite"] == suite_key]
        known_namespaces = {row["namespace"] for row in suite_rows}
        unknown_results = set(namespace_results) - known_namespaces
        if unknown_results:
            raise InventoryError(
                f"{suite_key} status contains unknown namespaces: {sorted(unknown_results)}"
            )
        if state == "accepted" and set(namespace_results) != known_namespaces:
            raise InventoryError(
                f"accepted {suite_key} result must classify every namespace"
            )
        if state == "accepted":
            validate_accepted_evidence(
                suite_key,
                suite_status,
                namespace_results,
                portable=portable,
                validation_report=validation_report,
            )

        for row in suite_rows:
            classified = namespace_results.get(row["namespace"], {})
            if not isinstance(classified, dict):
                raise InventoryError(
                    f"namespace result {suite_key}.{row['namespace']} must be an object"
                )
            row["pass"] = strict_non_negative_int(
                classified.get("pass", 0), f"{suite_key}.{row['namespace']}.pass"
            )
            row["fail"] = strict_non_negative_int(
                classified.get("fail", 0), f"{suite_key}.{row['namespace']}.fail"
            )
            row["expected_skip"] = strict_non_negative_int(
                classified.get("expected_skip", 0),
                f"{suite_key}.{row['namespace']}.expected_skip",
            )
            classified_total = (
                row["pass"] + row["fail"] + row["expected_skip"] + row["unwritten"]
            )
            row["unverified"] = row["items"] - classified_total
            if min(
                row["pass"],
                row["fail"],
                row["expected_skip"],
                row["unverified"],
                row["unwritten"],
            ) < 0:
                raise InventoryError(
                    f"classification exceeds items for {suite_key}.{row['namespace']}"
                )
            row["state"] = state
            row["evidence"] = str(suite_status.get("evidence", ""))
            row["diagnostic"] = str(suite_status.get("diagnostic", ""))
            row["gap_id"] = str(suite_status.get("gap_id", ""))

        unresolved = sum(
            row["fail"] + row["unverified"] + row["unwritten"] for row in suite_rows
        )
        gap_id = str(suite_status.get("gap_id", ""))
        if unresolved and gap_id not in gap_ids:
            raise InventoryError(
                f"unresolved suite {suite_key} requires a declared gap_id"
            )
        if not unresolved and gap_id:
            raise InventoryError(f"resolved suite {suite_key} must clear gap_id")
        declared_gap_id = gap_for_suite.get(suite_key, "")
        if gap_id != declared_gap_id:
            raise InventoryError(
                f"suite {suite_key} gap_id disagrees with gaps[].suites: "
                f"status={gap_id or '<none>'} declared={declared_gap_id or '<none>'}"
            )


def aggregate(rows: Iterable[Dict[str, Any]]) -> Dict[str, int]:
    result = {
        "items": 0,
        "pass": 0,
        "fail": 0,
        "expected_skip": 0,
        "unverified": 0,
        "unwritten": 0,
    }
    for row in rows:
        for key in result:
            result[key] += int(row[key])
    return result


def md(value: Any) -> str:
    return str(value).replace("|", "\\|").replace("\n", " ")


def render_inventory(
    rows: List[Dict[str, Any]],
    status: Dict[str, Any],
    catalog: Dict[str, Any],
) -> str:
    totals = aggregate(rows)
    invariant = (
        totals["pass"]
        + totals["fail"]
        + totals["expected_skip"]
        + totals["unverified"]
        + totals["unwritten"]
    )
    if invariant != totals["items"]:
        raise InventoryError(f"global inventory invariant failed: {invariant} != {totals['items']}")

    suites = status["suites"]
    ordered_keys = [spec.key for spec in CORPORA]
    ordered_keys.insert(4, FULL_CATALOG_KEY)
    labels = {spec.key: spec.label for spec in CORPORA}
    labels[FULL_CATALOG_KEY] = FULL_CATALOG_LABEL
    benchmark_rows_classified = (
        totals["fail"] == totals["unverified"] == totals["unwritten"] == 0
    )
    accepted_suite_count = sum(
        1 for suite_status in suites.values() if suite_status.get("state") == "accepted"
    )
    suite_states_done = accepted_suite_count == len(suites)
    benchmark_rows_done = benchmark_rows_classified and suite_states_done
    execution_gates = status["execution_gates"]
    passed_gate_count = sum(1 for gate in execution_gates if gate["status"] == "passed")
    execution_gates_done = passed_gate_count == len(REQUIRED_EXECUTION_GATE_IDS)
    overall_done = benchmark_rows_done and execution_gates_done

    lines = [
        "# Monolith Benchmark Completion Inventory",
        "",
        f"Snapshot: `{md(status.get('snapshot_id', ''))}`",
        f"Catalog contract: `{md(catalog['version'])}` / {catalog['namespace_count']} namespaces / {catalog['action_count']} checked-in actions",
        "Source of truth: manifests and JSONL corpora under `Benchmarks`, plus `Benchmarks/inventory_status.json` for accepted-run evidence.",
        "Validation modes are explicit: `--portable-check` rederives tracked accepted bundles in a clean checkout and uses recorded DB attestation only when the DB is absent; `--check` additionally requires every live DB and pending Saved diagnostic and rejects mtime/content drift.",
        "",
        "## Done Contract",
        "",
        "For every row: `pass + expected skip + fail + unverified + unwritten = items`.",
        "Benchmark rows are done only when every suite is `accepted` and `fail = 0`, `unverified = 0`, and `unwritten = 0`; pending suites cannot claim numeric result credit. An expected skip counts only when the raw test row and prerequisite state prove that environment-dependent outcome.",
        "For SchemaCompleteness full-catalog rows, `pass` means every applicable schema dimension passed (`schema_score = 1.0`); a reusable fetched row with an incomplete schema is `fail`, not merely completed coverage.",
        "Overall Done additionally requires every fixed execution gate (`GATE-NOLINK`, `GATE-FINAL-LINK`, `GATE-CRG`, `GATE-ANIMATION`, `GATE-PRECOMMIT`) to be `passed` with evidence.",
        "A diagnostic subset or an interrupted prefix is evidence, but never reduces the accepted `unverified` count.",
        "",
        "## Fixed Totals",
        "",
        "| Items | Pass | Expected skip | Fail | Unverified | Unwritten | Rows classified | Suites accepted | Benchmark rows done | Gates passed | Overall Done |",
        "| ---: | ---: | ---: | ---: | ---: | ---: | :---: | ---: | :---: | ---: | :---: |",
        f"| {totals['items']} | {totals['pass']} | {totals['expected_skip']} | {totals['fail']} | {totals['unverified']} | {totals['unwritten']} | {'YES' if benchmark_rows_classified else 'NO'} | {accepted_suite_count}/{len(suites)} | {'YES' if benchmark_rows_done else 'NO'} | {passed_gate_count}/{len(REQUIRED_EXECUTION_GATE_IDS)} | {'YES' if overall_done else 'NO'} |",
        "",
        "## Suite Summary",
        "",
        "| Suite | Namespace rows | Items | Pass | Expected skip | Fail | Unverified | Unwritten | State | Gap | Evidence / diagnostic |",
        "| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | --- | --- | --- |",
    ]
    for key in ordered_keys:
        suite_rows = [row for row in rows if row["suite"] == key]
        summary = aggregate(suite_rows)
        suite_status = suites[key]
        evidence = str(suite_status.get("evidence", ""))
        diagnostic = str(suite_status.get("diagnostic", ""))
        note = "; ".join(part for part in (evidence, diagnostic) if part)
        lines.append(
            "| "
            + " | ".join(
                [
                    md(labels[key]),
                    str(len(suite_rows)),
                    str(summary["items"]),
                    str(summary["pass"]),
                    str(summary["expected_skip"]),
                    str(summary["fail"]),
                    str(summary["unverified"]),
                    str(summary["unwritten"]),
                    md(suite_status["state"]),
                    md(suite_status.get("gap_id", "")),
                    md(note),
                ]
            )
            + " |"
        )

    lines.extend(["", "## Namespace Inventory", ""])
    for key in ordered_keys:
        suite_rows = [row for row in rows if row["suite"] == key]
        lines.extend(
            [
                f"### {labels[key]}",
                "",
                "| Namespace | Items | Pass | Expected skip | Fail | Unverified | Unwritten |",
                "| --- | ---: | ---: | ---: | ---: | ---: | ---: |",
            ]
        )
        for row in suite_rows:
            lines.append(
                f"| {md(row['namespace'])} | {row['items']} | {row['pass']} | "
                f"{row['expected_skip']} | {row['fail']} | {row['unverified']} | "
                f"{row['unwritten']} |"
            )
        lines.append("")

    lines.extend(
        [
            "## Remaining Gap List",
            "",
            "Only these declared gaps and execution gates may expand the remaining work. New gaps require a concrete failed row or a manifest/catalog identity change.",
            "",
            "| ID | Scope | Remaining | Done when | Blocker / sequencing |",
            "| --- | --- | ---: | --- | --- |",
        ]
    )
    for gap in status["gaps"]:
        suite_keys = [str(key) for key in gap.get("suites", [])]
        remaining = sum(
            row["fail"] + row["unverified"] + row["unwritten"]
            for row in rows
            if row["suite"] in suite_keys
        )
        lines.append(
            f"| {md(gap['id'])} | {md(', '.join(suite_keys))} | {remaining} | "
            f"{md(gap.get('done_when', ''))} | {md(gap.get('blocker', ''))} |"
        )

    lines.extend(
        [
            "",
            "## Execution Gates",
            "",
            "| ID | Status | Contract | Evidence |",
            "| --- | --- | --- | --- |",
        ]
    )
    for gate in status.get("execution_gates", []):
        lines.append(
            f"| {md(gate.get('id', ''))} | {md(gate.get('status', ''))} | "
            f"{md(gate.get('contract', ''))} | {md(gate.get('evidence', ''))} |"
        )

    boundary = status.get("changelist_boundary", {})
    lines.extend(
        [
            "",
            "## Changelist Boundary",
            "",
            f"- CL 1100 (`bench`): {md(boundary.get('1100', ''))}",
            f"- CL 1200 (`Monolith`): {md(boundary.get('1200', ''))}",
            f"- Verified overlap: {md(boundary.get('overlap', ''))}",
            "",
            "Regenerate and validate:",
            "",
            "```powershell",
            "python Scripts\\benchmark_inventory.py --write",
            "python Scripts\\benchmark_inventory.py --portable-check",
            "python Scripts\\benchmark_inventory.py --check",
            "```",
            "",
        ]
    )
    return "\n".join(lines)


def build(
    *,
    portable: bool = False,
    validation_report: Dict[str, List[str]] | None = None,
) -> str:
    status = load_json(STATUS_PATH)
    rows: List[Dict[str, Any]] = []
    for spec in CORPORA:
        rows.extend(corpus_rows(spec))
    live_rows, catalog = full_catalog_rows()
    rows.extend(live_rows)
    apply_status(
        rows,
        status,
        portable=portable,
        validation_report=validation_report,
    )
    return render_inventory(rows, status, catalog)


def main(argv: List[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--write", action="store_true", help="Regenerate Benchmarks/INVENTORY.md")
    mode.add_argument(
        "--portable-check",
        action="store_true",
        help=(
            "Clean-checkout validation using tracked accepted bundles; an absent DB "
            "uses its pinned attestation, while a present DB must still match"
        ),
    )
    mode.add_argument(
        "--check",
        action="store_true",
        help=(
            "Full local validation requiring live DBs, exact mtimes/content, and "
            "every referenced pending Saved diagnostic"
        ),
    )
    args = parser.parse_args(argv)
    validation_report: Dict[str, List[str]] = {
        "attested_databases": [],
        "omitted_pending_evidence": [],
    }
    portable = bool(args.portable_check)

    try:
        rendered = build(
            portable=portable,
            validation_report=validation_report,
        )
    except (InventoryError, OSError, json.JSONDecodeError) as exc:
        print(f"benchmark inventory error: {exc}", file=sys.stderr)
        return 1

    if args.write:
        OUTPUT_PATH.write_text(rendered, encoding="utf-8", newline="\n")
        print(f"wrote {OUTPUT_PATH}")
        return 0

    current = OUTPUT_PATH.read_text(encoding="utf-8") if OUTPUT_PATH.exists() else ""
    if current == rendered:
        mode_label = "portable clean-checkout" if portable else "full local"
        print(
            f"benchmark inventory {mode_label} check is current: "
            f"{sum(1 for line in rendered.splitlines() if line.startswith('| '))} table rows"
        )
        if portable:
            attested = validation_report["attested_databases"]
            omitted = validation_report["omitted_pending_evidence"]
            print(
                "portable DB validation: "
                + (
                    "recorded bundle attestation used for absent " + ", ".join(attested)
                    if attested
                    else "all required databases were present and content-verified"
                )
            )
            print(
                "portable pending diagnostics: "
                + (
                    f"{len(omitted)} absent Saved path(s) omitted from completion credit"
                    if omitted
                    else "all referenced diagnostics were present"
                )
            )
        return 0
    print("benchmark inventory is stale; run Scripts/benchmark_inventory.py --write", file=sys.stderr)
    for line in difflib.unified_diff(
        current.splitlines(), rendered.splitlines(), fromfile=str(OUTPUT_PATH), tofile="generated"
    ):
        print(line, file=sys.stderr)
    return 1


if __name__ == "__main__":
    raise SystemExit(main())
