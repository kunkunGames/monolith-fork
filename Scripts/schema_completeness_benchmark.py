#!/usr/bin/env python3
"""
Monolith MCP schema-completeness benchmark.

Scans the entire live action catalog (all namespaces, all actions) and scores
each action's schema for six quality dimensions. Unlike ActionGuidance, which
uses a generated task sample, this benchmark enumerates the complete catalog.

The three param-gated dimensions (param_types_declared, required_params_marked,
value_domain) are scored only for actions that declare parameters; param-less
actions are N/A on them (excluded from the rate, never auto-passed).  value_domain
goes beyond "a type key exists" and checks that every declared param carries a
type, a non-empty description, and a correct required flag, and that constrained
params (enum / numeric range) actually document their allowed values.

Subcommands
-----------
scan    Connect to a live MCP endpoint, discover every action, score schema
        quality for each, and write result files to --output-dir. Full scans
        checkpoint every valid result and may continue with --resume after an
        MCP/editor restart when the input and catalog identity are unchanged.
probe   Load a probe_set.jsonl file, fetch the schema for each listed
        namespace.action, score it, and check expected_dimensions pass/fail.
        Writes summary.json, per_action.jsonl, partial_summary.json, and the
        additional probe_results.jsonl file with per-probe dimension results.
compare Diff two scan summary.json files and produce comparison.json + .md.
report  Print a human-readable namespace breakdown from a summary.json.
"""

from __future__ import annotations

import argparse
import datetime as _dt
import hashlib
import json
import math
import pathlib
import re
import socket
import sys
import time
import urllib.error
import urllib.request
from typing import Any, Dict, Iterable, List, NamedTuple, Optional, Set, Tuple

from benchmark_common import (
    benchmark_routing_context,
    DEFAULT_MAX_CONSECUTIVE_TRANSPORT_FAILURES,
    DEFAULT_MAX_TRANSPORT_FAILED_FRACTION,
    DEFAULT_MIN_TRANSPORT_FRACTION_SAMPLES,
    TransportFailureTracker,
    attach_benchmark_inputs,
    build_benchmark_inputs,
    local_project_name,
    refresh_benchmark_input_fingerprint,
    resolve_plugin_path,
)

DEFAULT_MCP_URL = "http://localhost:9316/mcp"
DEFAULT_PROBE_SET = "Benchmarks/SchemaCompleteness/probe_set.jsonl"
PARTIAL_FLUSH_EVERY = 25
MIN_TRANSPORT_FRACTION_SAMPLE = DEFAULT_MIN_TRANSPORT_FRACTION_SAMPLES
CANONICAL_ACTION_ID_RE = re.compile(r"^[a-z][a-z0-9_]*$")

# Score weights (must sum to 1.0)
W_PARAM_TYPES = 0.25
W_REQUIRED_PARAMS = 0.20
W_VALUE_DOMAIN = 0.20
W_PLANNING_SIGNALS = 0.15
W_SKILL_ROUTING = 0.10
W_OUTPUT_CONTRACT = 0.10

# Dimensions that are N/A (excluded from the rate denominator) for param-less
# actions. A param-less action neither passes nor fails these dimensions: it has
# no parameter contract to evaluate. Treating it as auto-pass let an action game
# the score by declaring nothing (ROI report A4 item 2/4); treating it as a fail
# would punish legitimately param-less reads. So they are scored None and dropped
# from the rate, never folded in as 1.0 or 0.0.
PARAM_GATED_DIMENSIONS = (
    "param_types_declared",
    "required_params_marked",
    "value_domain",
)

PROBE_AVAILABILITY_MODES = frozenset({"required", "optional", "feature_gated"})
SKIPPABLE_ABSENCE_MODES = frozenset({"optional", "feature_gated"})


class ProbeSetContractError(ValueError):
    """The checked-in probe contract is malformed or inconsistent."""


class SchemaFetchOutcome(NamedTuple):
    """Structured schema-fetch result; runner and server defects stay distinct."""

    schema: Optional[Dict[str, Any]]
    failure_kind: str
    transport_error: bool = False
    status: Optional[int] = None
    raw: str = ""
    error: str = ""


class StatusPreflightOutcome(NamedTuple):
    """Structured status result used before any benchmark scoring request."""

    status_data: Optional[Dict[str, Any]]
    failure_kind: str
    transport_error: bool = False
    transport_status: Optional[int] = None
    raw: str = ""
    error: str = ""


# ---------------------------------------------------------------------------
# Utilities shared with action_guidance_benchmark style
# ---------------------------------------------------------------------------

def utc_now() -> str:
    return _dt.datetime.now(_dt.timezone.utc).isoformat()


def write_json(path: pathlib.Path, payload: Any) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", encoding="utf-8", newline="\n") as handle:
        json.dump(payload, handle, indent=2, ensure_ascii=False)
        handle.write("\n")


def write_json_atomic(path: pathlib.Path, payload: Any) -> None:
    """Replace a JSON artifact atomically so interruption cannot truncate it."""
    temporary = path.with_name(f"{path.name}.tmp")
    write_json(temporary, payload)
    temporary.replace(path)


def append_jsonl_row(path: pathlib.Path, row: Dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("a", encoding="utf-8", newline="\n") as handle:
        handle.write(json.dumps(row, ensure_ascii=False, sort_keys=True))
        handle.write("\n")


def write_jsonl(path: pathlib.Path, rows: Iterable[Dict[str, Any]]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", encoding="utf-8", newline="\n") as handle:
        for row in rows:
            handle.write(json.dumps(row, ensure_ascii=False, sort_keys=True))
            handle.write("\n")


def write_jsonl_atomic(path: pathlib.Path, rows: Iterable[Dict[str, Any]]) -> None:
    """Replace a JSONL artifact atomically after validating rows in memory."""
    temporary = path.with_name(f"{path.name}.tmp")
    write_jsonl(temporary, rows)
    temporary.replace(path)


def _strict_json_object(pairs: List[Tuple[str, Any]]) -> Dict[str, Any]:
    """Build one JSON object while rejecting duplicate member names."""
    result: Dict[str, Any] = {}
    for key, value in pairs:
        if key in result:
            raise ValueError(f"duplicate JSON member name: {key}")
        result[key] = value
    return result


def strict_json_loads(text: str) -> Any:
    """Parse JSON without Python's default last-value-wins duplicate behavior."""
    return json.loads(text, object_pairs_hook=_strict_json_object)


RUN_OUTPUT_FILENAMES = (
    "summary.json",
    "partial_summary.json",
    "namespace_breakdown.json",
    "per_action.jsonl",
    "scan_checkpoint.json",
    "probe_results.jsonl",
    "probe_preflight.json",
    "run_failure.json",
)

SCAN_CHECKPOINT_FILENAME = "scan_checkpoint.json"
SCAN_CHECKPOINT_SCHEMA_VERSION = 2
SCAN_CONTRACT_VERSION = "schema-completeness-full-catalog-v2"
CATALOG_ACTION_IDS_HASH_KIND = "sorted_json_namespace_dot_action_v1"
RAW_SCHEMA_HASH_KIND = "sha256_utf8_canonical_json_sorted_keys_compact_ascii_type_exact_v1"
CHECKPOINT_DIMENSION_FIELDS = (
    "param_types_declared",
    "required_params_marked",
    "value_domain",
    "planning_signals_present",
    "skill_routing_present",
    "output_contract_declared",
)


def schema_quality_pass(row: Dict[str, Any]) -> bool:
    """True only when every applicable schema dimension explicitly passes."""
    if any(field not in row for field in CHECKPOINT_DIMENSION_FIELDS):
        return False
    applicable = [
        row[field]
        for field in CHECKPOINT_DIMENSION_FIELDS
        if field in row and row[field] is not None
    ]
    return bool(applicable) and all(value is True for value in applicable)


def clear_run_outputs(output_dir: pathlib.Path) -> None:
    """Remove only known outputs so a failed rerun cannot expose stale success."""
    for filename in RUN_OUTPUT_FILENAMES:
        path = output_dir / filename
        if path.exists():
            path.unlink()


def write_run_failure(output_dir: pathlib.Path, payload: Dict[str, Any]) -> None:
    """Persist a machine-readable invalid-run record; never write summary.json."""
    failure = dict(payload)
    failure.setdefault("created_at", utc_now())
    failure["run_valid"] = False
    write_json(output_dir / "run_failure.json", failure)


def _validate_canonical_json_value(payload: Any, *, path: str = "$") -> None:
    """Reject values whose Python type cannot be preserved by canonical JSON."""
    if payload is None or type(payload) in {bool, int, str}:
        return
    if type(payload) is float:
        if not math.isfinite(payload):
            raise ValueError(f"canonical JSON contains a non-finite number at {path}")
        return
    if type(payload) is list:
        for index, value in enumerate(payload):
            _validate_canonical_json_value(value, path=f"{path}[{index}]")
        return
    if type(payload) is dict:
        for key, value in payload.items():
            if type(key) is not str:
                raise ValueError(
                    f"canonical JSON object key at {path} must be a string, "
                    f"got {type(key).__name__}"
                )
            _validate_canonical_json_value(value, path=f"{path}.{key}")
        return
    raise ValueError(
        f"canonical JSON contains unsupported type at {path}: {type(payload).__name__}"
    )


def _canonical_json_bytes(payload: Any) -> bytes:
    """Serialize JSON with one type-preserving, byte-exact hashing contract."""
    _validate_canonical_json_value(payload)
    return json.dumps(
        payload,
        sort_keys=True,
        separators=(",", ":"),
        ensure_ascii=True,
        allow_nan=False,
    ).encode("utf-8")


def _sha256_json(payload: Any) -> str:
    return hashlib.sha256(_canonical_json_bytes(payload)).hexdigest()


def canonical_raw_schema(schema: Dict[str, Any]) -> Dict[str, Any]:
    """Detach a live schema as canonical JSON while preserving JSON scalar types."""
    if type(schema) is not dict:
        raise ValueError("raw schema must be a JSON object")
    canonical = strict_json_loads(_canonical_json_bytes(schema).decode("utf-8"))
    if type(canonical) is not dict:  # Defensive: the input check guarantees this.
        raise ValueError("canonical raw schema must be a JSON object")
    return canonical


def _exact_json_equal(left: Any, right: Any) -> bool:
    """Recursively compare JSON values without bool/number coercion."""
    if type(left) is not type(right):
        return False
    if isinstance(left, dict):
        return set(left) == set(right) and all(
            _exact_json_equal(left[key], right[key]) for key in left
        )
    if isinstance(left, list):
        return len(left) == len(right) and all(
            _exact_json_equal(a, b) for a, b in zip(left, right)
        )
    return left == right


def catalog_action_ids(pairs: Iterable[Tuple[str, str]]) -> List[str]:
    """Return the canonical sorted action-id set used by resume identity checks."""
    action_ids = sorted(f"{namespace}.{action}" for namespace, action in pairs)
    if len(action_ids) != len(set(action_ids)):
        raise ValueError("catalog action identity contains duplicate namespace.action ids")
    return action_ids


def catalog_action_ids_sha256(pairs: Iterable[Tuple[str, str]]) -> str:
    return _sha256_json(catalog_action_ids(pairs))


def build_schema_registry_inputs(
    *,
    status: Dict[str, Any],
    namespaces: List[Dict[str, Any]],
    catalog_pairs: Iterable[Tuple[str, str]],
    probe_set_path: Optional[pathlib.Path] = None,
) -> Dict[str, Any]:
    """Build DB-independent identity for live schema scans and probes.

    SchemaCompleteness reads only the live registry. Project/source database mtimes
    are unrelated to a schema score and commonly change while an editor restarts,
    so they are excluded before the shared builder enumerates them. The checked-in
    manifest, this runner, an optional probe set, and stable MCP/catalog metadata
    remain hashed.
    """
    inputs = build_benchmark_inputs(
        "SchemaCompleteness",
        probe_set_path=probe_set_path,
        mcp_status=status,
        catalog={"namespaces": namespaces},
        extra_files={"runner": pathlib.Path(__file__)},
        database_paths=(),
    )
    action_ids = catalog_action_ids(catalog_pairs)
    inputs["database_files_scope"] = "not_applicable_to_live_schema_registry_scan"
    inputs["schema_registry"] = {
        "catalog_action_count": len(action_ids),
        "catalog_action_ids_hash_kind": CATALOG_ACTION_IDS_HASH_KIND,
        "catalog_action_ids_sha256": _sha256_json(action_ids),
    }
    return refresh_benchmark_input_fingerprint(inputs)


def scan_resume_identity(
    *,
    args: argparse.Namespace,
    catalog_version: str,
    pairs: List[Tuple[str, str]],
    benchmark_inputs: Dict[str, Any],
) -> Dict[str, Any]:
    """Return every immutable input required to accept a checkpoint resume."""
    if not catalog_version:
        raise ValueError(
            "full-scan checkpointing requires a non-empty live catalog_version"
        )
    identity = {
        "contract_version": SCAN_CONTRACT_VERSION,
        "label": str(args.label),
        "mcp_url": str(args.mcp_url),
        "catalog_version": catalog_version,
        "catalog_action_count": len(pairs),
        "catalog_action_ids_hash_kind": CATALOG_ACTION_IDS_HASH_KIND,
        "catalog_action_ids_sha256": catalog_action_ids_sha256(pairs),
        "benchmark_input_fingerprint": str(
            benchmark_inputs.get("fingerprint_sha256", "")
        ),
        "request_timeout_s": float(args.request_timeout_s),
        "max_failed_fraction": float(args.max_failed_fraction),
        "max_transport_failed_fraction": float(
            args.max_transport_failed_fraction
        ),
        "max_consecutive_transport_failures": int(
            args.max_consecutive_transport_failures
        ),
        "min_transport_fraction_sample": int(
            args.min_transport_fraction_sample
        ),
    }
    identity["fingerprint_sha256"] = _sha256_json(identity)
    return identity


def _checkpoint_action_id(row: Dict[str, Any]) -> str:
    namespace = str(row.get("namespace", "")).strip()
    action = str(row.get("action", "")).strip()
    return f"{namespace}.{action}" if namespace and action else ""


def validate_checkpoint_result_row(
    row: Dict[str, Any],
    *,
    expected_action_ids: Set[str],
) -> str:
    """Validate one reusable result and return its unique action id.

    Only successful schema fetches are reusable. Transport/protocol/runner failures
    remain attempt provenance in the checkpoint and must be retried after restart.
    """
    action_id = _checkpoint_action_id(row)
    if action_id not in expected_action_ids:
        raise ValueError(
            f"checkpoint result action is not in the current catalog: {action_id or '<missing>'}"
        )
    if row.get("failure_kind") != "ok":
        raise ValueError(
            f"checkpoint result {action_id} is not reusable: failure_kind={row.get('failure_kind')!r}"
        )
    if row.get("error") not in (None, ""):
        raise ValueError(f"checkpoint result {action_id} carries an error")
    if row.get("transport_error") is not False:
        raise ValueError(f"checkpoint result {action_id} carries a transport failure")
    if row.get("transport_status") is not None or row.get("transport_error_raw") not in (
        None,
        "",
    ):
        raise ValueError(
            f"checkpoint result {action_id} carries stale transport diagnostics"
        )
    diagnostics = row.get("value_domain_diagnostics")
    if not isinstance(diagnostics, list):
        raise ValueError(
            f"checkpoint result {action_id} has invalid value_domain_diagnostics"
        )
    for field in CHECKPOINT_DIMENSION_FIELDS:
        if field not in row:
            raise ValueError(
                f"checkpoint result {action_id} is missing dimension {field}"
            )
        value = row.get(field)
        valid_value = isinstance(value, bool) or (
            field in PARAM_GATED_DIMENSIONS and value is None
        )
        if not valid_value:
            raise ValueError(
                f"checkpoint result {action_id} has invalid {field}={value!r}"
            )
    diagnostic_params: Set[str] = set()
    for index, diagnostic in enumerate(diagnostics, start=1):
        if not isinstance(diagnostic, dict):
            raise ValueError(
                f"checkpoint result {action_id} has non-object value-domain diagnostic {index}"
            )
        param = diagnostic.get("param")
        if not isinstance(param, str) or not param:
            raise ValueError(
                f"checkpoint result {action_id} has unnamed value-domain diagnostic {index}"
            )
        if param in diagnostic_params:
            raise ValueError(
                f"checkpoint result {action_id} has duplicate value-domain diagnostic {param}"
            )
        diagnostic_params.add(param)
        if type(diagnostic.get("ok")) is not bool or not _is_nonempty_string(
            diagnostic.get("reason")
        ):
            raise ValueError(
                f"checkpoint result {action_id} has malformed value-domain diagnostic {param}"
            )
    value_domain = row["value_domain"]
    if value_domain is None and diagnostics:
        raise ValueError(
            f"checkpoint result {action_id} has diagnostics for an N/A value_domain"
        )
    if value_domain is not None and (
        not diagnostics
        or value_domain is not all(diagnostic["ok"] for diagnostic in diagnostics)
    ):
        raise ValueError(
            f"checkpoint result {action_id} value_domain is not derived from diagnostics"
        )
    score = row.get("schema_score")
    if (
        type(score) is not float
        or not math.isfinite(float(score))
        or not 0.0 <= float(score) <= 1.0
    ):
        raise ValueError(
            f"checkpoint result {action_id} has invalid schema_score={score!r}"
        )
    if row.get("raw_schema_hash_kind") != RAW_SCHEMA_HASH_KIND:
        raise ValueError(
            f"checkpoint result {action_id} raw_schema_hash_kind is invalid"
        )
    if "raw_schema" not in row or type(row.get("raw_schema")) is not dict:
        raise ValueError(
            f"checkpoint result {action_id} is missing canonical raw_schema"
        )
    raw_schema = canonical_raw_schema(row["raw_schema"])
    raw_schema_sha256 = row.get("raw_schema_sha256")
    if (
        type(raw_schema_sha256) is not str
        or re.fullmatch(r"[0-9a-f]{64}", raw_schema_sha256) is None
    ):
        raise ValueError(
            f"checkpoint result {action_id} raw_schema_sha256 is invalid"
        )
    expected_raw_schema_sha256 = _sha256_json(raw_schema)
    if raw_schema_sha256 != expected_raw_schema_sha256:
        raise ValueError(
            f"checkpoint result {action_id} raw_schema_sha256 does not match raw_schema"
        )

    rescored = score_schema_quality(raw_schema)
    for field in CHECKPOINT_DIMENSION_FIELDS:
        if not _exact_json_equal(row[field], rescored[field]):
            raise ValueError(
                f"checkpoint result {action_id} {field} is not derived from raw_schema: "
                f"observed={row[field]!r} expected={rescored[field]!r}"
            )
    if not _exact_json_equal(score, rescored["schema_score"]):
        raise ValueError(
            f"checkpoint result {action_id} schema_score is not derived from raw_schema: "
            f"observed={score!r} expected={rescored['schema_score']!r}"
        )
    if not _exact_json_equal(diagnostics, rescored["value_domain_diagnostics"]):
        raise ValueError(
            f"checkpoint result {action_id} value_domain_diagnostics are not derived "
            "from raw_schema"
        )
    return action_id


def validate_schema_result_row(
    row: Dict[str, Any],
    *,
    expected_action_ids: Set[str],
) -> str:
    """Validate the shared scan/probe row contract, including failed fetches."""
    if row.get("failure_kind") == "ok":
        return validate_checkpoint_result_row(
            row,
            expected_action_ids=expected_action_ids,
        )

    action_id = _checkpoint_action_id(row)
    if action_id not in expected_action_ids:
        raise ValueError(
            f"schema result action is not in the expected set: {action_id or '<missing>'}"
        )
    failure_kind = row.get("failure_kind")
    if type(failure_kind) is not str or not failure_kind.strip():
        raise ValueError(f"schema result {action_id} has invalid failure_kind")
    if type(row.get("error")) is not str or not row["error"]:
        raise ValueError(f"schema result {action_id} fetch failure lacks an error")
    if type(row.get("transport_error")) is not bool:
        raise ValueError(f"schema result {action_id} has invalid transport_error")
    if row.get("raw_schema_hash_kind") != RAW_SCHEMA_HASH_KIND:
        raise ValueError(f"schema result {action_id} raw_schema_hash_kind is invalid")
    if "raw_schema" not in row or row["raw_schema"] is not None:
        raise ValueError(f"schema result {action_id} failed fetch must have raw_schema=null")
    if "raw_schema_sha256" not in row or row["raw_schema_sha256"] is not None:
        raise ValueError(
            f"schema result {action_id} failed fetch must have raw_schema_sha256=null"
        )
    expected_quality = failed_schema_quality()
    for field in (*CHECKPOINT_DIMENSION_FIELDS, "schema_score", "value_domain_diagnostics"):
        if field not in row or not _exact_json_equal(row[field], expected_quality[field]):
            raise ValueError(
                f"schema result {action_id} {field} is not the canonical failed-fetch value"
            )
    return action_id


def load_checkpoint_results(
    path: pathlib.Path,
    *,
    expected_action_ids: Set[str],
) -> Dict[str, Dict[str, Any]]:
    """Strictly load reusable rows, rejecting duplicate keys and action ids."""
    if not path.is_file():
        raise ValueError(f"checkpoint results file is missing: {path}")
    rows: Dict[str, Dict[str, Any]] = {}
    with path.open("r", encoding="utf-8") as handle:
        for line_no, line in enumerate(handle, 1):
            text = line.strip()
            if not text:
                raise ValueError(
                    f"checkpoint results contain a blank JSONL row at line {line_no}"
                )
            try:
                value = strict_json_loads(text)
            except (json.JSONDecodeError, ValueError) as exc:
                raise ValueError(
                    f"checkpoint results line {line_no} is invalid JSON: {exc}"
                ) from exc
            if not isinstance(value, dict):
                raise ValueError(
                    f"checkpoint results line {line_no} must be a JSON object"
                )
            action_id = validate_checkpoint_result_row(
                value,
                expected_action_ids=expected_action_ids,
            )
            if action_id in rows:
                raise ValueError(
                    f"checkpoint results contain duplicate action id: {action_id}"
                )
            rows[action_id] = value
    return rows


def new_scan_checkpoint(
    *,
    resume_identity: Dict[str, Any],
    benchmark_inputs: Dict[str, Any],
) -> Dict[str, Any]:
    now = utc_now()
    return {
        "schema_version": SCAN_CHECKPOINT_SCHEMA_VERSION,
        "benchmark": "SchemaCompleteness",
        "scan_scope": "full_catalog",
        "state": "in_progress",
        "created_at": now,
        "updated_at": now,
        "resume_identity": resume_identity,
        "benchmark_inputs": benchmark_inputs,
        "results_file": "per_action.jsonl",
        "completed_valid_action_count": 0,
        "segments": [],
        "outages": [],
        "invalid_attempts": [],
        "recovery_events": [],
    }


def load_scan_checkpoint(path: pathlib.Path) -> Dict[str, Any]:
    if not path.is_file():
        raise ValueError(f"scan checkpoint is missing: {path}")
    try:
        checkpoint = strict_json_loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError, ValueError) as exc:
        raise ValueError(f"scan checkpoint is invalid JSON: {path}: {exc}") from exc
    if not isinstance(checkpoint, dict):
        raise ValueError("scan checkpoint must be a JSON object")
    if checkpoint.get("schema_version") != SCAN_CHECKPOINT_SCHEMA_VERSION:
        raise ValueError(
            "scan checkpoint schema_version mismatch: "
            f"{checkpoint.get('schema_version')!r} != {SCAN_CHECKPOINT_SCHEMA_VERSION}"
        )
    if checkpoint.get("benchmark") != "SchemaCompleteness":
        raise ValueError("scan checkpoint benchmark identity mismatch")
    if checkpoint.get("scan_scope") != "full_catalog":
        raise ValueError("scan checkpoint is not a full_catalog checkpoint")
    if checkpoint.get("state") not in {
        "in_progress",
        "interrupted",
        "publishing",
        "completed",
    }:
        raise ValueError(f"scan checkpoint state is invalid: {checkpoint.get('state')!r}")
    for field in ("segments", "outages", "invalid_attempts", "recovery_events"):
        if not isinstance(checkpoint.get(field), list):
            raise ValueError(f"scan checkpoint {field} must be an array")
    if not isinstance(checkpoint.get("resume_identity"), dict):
        raise ValueError("scan checkpoint resume_identity must be an object")
    return checkpoint


def validate_checkpoint_identity(
    checkpoint: Dict[str, Any],
    current_identity: Dict[str, Any],
) -> None:
    previous_identity = checkpoint["resume_identity"]
    identity_without_fingerprint = dict(previous_identity)
    recorded_identity_fingerprint = identity_without_fingerprint.pop(
        "fingerprint_sha256",
        None,
    )
    if recorded_identity_fingerprint != _sha256_json(identity_without_fingerprint):
        raise ValueError("scan checkpoint resume_identity fingerprint is invalid")
    stored_inputs = checkpoint.get("benchmark_inputs")
    if not isinstance(stored_inputs, dict):
        raise ValueError("scan checkpoint benchmark_inputs must be an object")
    if stored_inputs.get("fingerprint_sha256") != previous_identity.get(
        "benchmark_input_fingerprint"
    ):
        raise ValueError("scan checkpoint benchmark input fingerprint is inconsistent")
    if previous_identity != current_identity:
        changed = sorted(
            key
            for key in set(previous_identity) | set(current_identity)
            if previous_identity.get(key) != current_identity.get(key)
        )
        raise ValueError(
            "scan checkpoint input/catalog identity mismatch: "
            + ", ".join(changed)
        )


def persist_scan_checkpoint(
    output_dir: pathlib.Path,
    checkpoint: Dict[str, Any],
    completed_valid_action_count: int,
) -> None:
    checkpoint["completed_valid_action_count"] = completed_valid_action_count
    checkpoint["updated_at"] = utc_now()
    write_json_atomic(output_dir / SCAN_CHECKPOINT_FILENAME, checkpoint)


# ---------------------------------------------------------------------------
# MCP transport
# ---------------------------------------------------------------------------

def read_http_body(response: Any, timeout_s: float) -> str:
    content_type = str(response.headers.get("Content-Type", "")).lower()
    if "text/event-stream" not in content_type:
        return response.read().decode("utf-8", errors="replace")

    data_lines: List[str] = []
    deadline = time.monotonic() + timeout_s
    while time.monotonic() < deadline:
        line = response.readline()
        if not line:
            break
        text = line.decode("utf-8", errors="replace").rstrip("\r\n")
        if text.startswith("data:"):
            data_lines.append(text[5:].lstrip())
            continue
        if text == "" and data_lines:
            return "\n".join(data_lines)
    return "\n".join(data_lines)


def extract_sse_data(raw: str) -> str:
    lines = raw.splitlines()
    data_lines = [line[5:].lstrip() for line in lines if line.startswith("data:")]
    return "\n".join(data_lines) if data_lines else raw


# Declares this traffic as synthetic benchmark fixtures so the invocation-log
# analyzer does not report deliberate negative probes as real, unmet demand.
_BENCHMARK_ROUTING_CONTEXT = benchmark_routing_context("SchemaCompleteness")


def mcp_call(url: str, tool: str, arguments: Dict[str, Any], timeout_s: float = 8.0) -> Dict[str, Any]:
    body = {
        "jsonrpc": "2.0",
        "id": int(time.time() * 1000) % 1_000_000_000,
        "method": "tools/call",
        "params": {
            "name": tool,
            "arguments": arguments,
        },
        "_monolith_routing_context": _BENCHMARK_ROUTING_CONTEXT,
    }
    data = json.dumps(body, separators=(",", ":")).encode("utf-8")
    request = urllib.request.Request(
        url,
        data=data,
        headers={
            "Content-Type": "application/json",
            "Accept": "application/json, text/event-stream",
        },
        method="POST",
    )
    try:
        with urllib.request.urlopen(request, timeout=timeout_s) as response:
            raw = read_http_body(response, timeout_s)
    except urllib.error.HTTPError as exc:
        raw = exc.read().decode("utf-8", errors="replace")
        return {"transport_error": True, "status": exc.code, "raw": raw}
    except (TimeoutError, socket.timeout) as exc:
        return {"transport_error": True, "status": None, "raw": f"timeout: {exc}"}
    except urllib.error.URLError as exc:
        return {"transport_error": True, "status": None, "raw": str(exc)}
    except OSError as exc:
        return {"transport_error": True, "status": None, "raw": str(exc)}

    raw = extract_sse_data(raw)
    try:
        parsed = json.loads(raw)
    except json.JSONDecodeError:
        parsed = {"parse_error": True, "raw": raw}
    if not isinstance(parsed, dict):
        return {
            "protocol_error": True,
            "raw": raw,
            "error": "MCP response top-level JSON must be an object",
        }
    return parsed


def result_text(response: Dict[str, Any]) -> str:
    result = response.get("result") if isinstance(response, dict) else None
    if isinstance(result, dict):
        content = result.get("content")
        if isinstance(content, list) and content:
            first = content[0]
            if isinstance(first, dict):
                return str(first.get("text", ""))
    error = response.get("error") if isinstance(response, dict) else None
    if error is not None:
        return json.dumps(error, ensure_ascii=False)
    return ""


def result_payload(response: Dict[str, Any]) -> Dict[str, Any]:
    result = response.get("result") if isinstance(response, dict) else None
    return result if isinstance(result, dict) else {}


def structured_content(payload: Dict[str, Any]) -> Dict[str, Any]:
    structured = payload.get("structuredContent") if isinstance(payload, dict) else None
    return structured if isinstance(structured, dict) else {}


def text_json(response: Dict[str, Any]) -> Optional[Dict[str, Any]]:
    text = result_text(response)
    if not text:
        return None
    try:
        parsed = json.loads(text)
    except json.JSONDecodeError:
        return None
    return parsed if isinstance(parsed, dict) else None


def result_data(response: Dict[str, Any]) -> Dict[str, Any]:
    parsed = text_json(response)
    if parsed:
        return parsed
    payload = result_payload(response)
    sc = structured_content(payload)
    return sc if sc else payload


def fetch_status_preflight(url: str, timeout_s: float) -> StatusPreflightOutcome:
    """Return one strict live-status outcome; malformed status invalidates the run."""
    try:
        response = mcp_call(url, "monolith_status", {}, timeout_s=timeout_s)
    except Exception as exc:  # noqa: BLE001 - convert runner defects into invalid artifacts.
        error = f"{type(exc).__name__}: {exc}"
        return StatusPreflightOutcome(None, "runner_exception", raw=error, error=error)
    if not isinstance(response, dict):
        raw = str(response)[:500]
        return StatusPreflightOutcome(
            None,
            "protocol_error",
            raw=raw,
            error="status response top-level JSON was not an object",
        )
    if response.get("transport_error"):
        status = response.get("status")
        raw = str(response.get("raw", ""))[:500]
        return StatusPreflightOutcome(
            None,
            "transport_error",
            transport_error=True,
            transport_status=(
                status if isinstance(status, int) and not isinstance(status, bool) else None
            ),
            raw=raw,
            error="status transport failure",
        )
    if (
        response.get("parse_error")
        or response.get("protocol_error")
        or response.get("error") is not None
    ):
        raw = str(response.get("raw", response))[:500]
        return StatusPreflightOutcome(
            None,
            "protocol_error",
            raw=raw,
            error="status response was not a valid MCP result object",
        )
    status = result_data(response)
    if not status or status.get("server_running") is not True:
        raw = str(status)[:500]
        return StatusPreflightOutcome(
            None,
            "invalid_status_payload",
            raw=raw,
            error="status payload did not declare server_running=true",
        )
    expected_project = local_project_name()
    observed_project = str(
        status.get("project_name") or status.get("project") or ""
    ).strip()
    if observed_project != expected_project:
        error = (
            f"status project identity mismatch: observed={observed_project or '<missing>'} "
            f"expected={expected_project}"
        )
        return StatusPreflightOutcome(
            None,
            "invalid_status_identity",
            raw=str(status)[:500],
            error=error,
        )
    return StatusPreflightOutcome(status, "ok")


def status_failure_fields(outcome: StatusPreflightOutcome) -> Dict[str, Any]:
    """Stable diagnostic fields shared by scan/probe status aborts."""
    return {
        "failure_kind": outcome.failure_kind,
        "error": outcome.error,
        "transport_failure_count": 1 if outcome.transport_error else 0,
        "last_transport_status": outcome.transport_status,
        "last_transport_error_raw": outcome.raw if outcome.transport_error else "",
        "protocol_error_raw": outcome.raw if not outcome.transport_error else "",
        "metrics_valid": False,
        "metrics_scope": "not_started",
    }


# ---------------------------------------------------------------------------
# Catalog discovery
# ---------------------------------------------------------------------------

# Upper bound on mode=actions pages per namespace. The server caps limit at
# 1000, so 64 pages = 64k actions — far above any real namespace. Hitting the
# cap means the pagination contract is broken, which must fail fast.
MAX_ACTION_PAGES_PER_NAMESPACE = 64
# Server-side maximum page size for mode=actions listings (MonolithCoreTools
# DefaultLimit=50 / MaxLimit=1000). One page per namespace in practice.
ACTION_PAGE_LIMIT = 1000


class CatalogTransportError(RuntimeError):
    """Live catalog enumeration could not continue because MCP transport failed."""


def discover_catalog_summary(
    url: str,
    timeout_s: float,
) -> Tuple[List[Dict[str, Any]], str]:
    """Return namespace summary rows and the catalog version when declared.

    Under the compact discover contract (mode="summary" default) each row is
    {namespace, action_count, projection, actions_hint} — action names are NOT
    inlined and must be enumerated per namespace with mode="actions".
    """
    response = mcp_call(
        url,
        "monolith_discover",
        {"mode": "summary", "limit": ACTION_PAGE_LIMIT},
        timeout_s=timeout_s,
    )
    if not isinstance(response, dict):
        raise RuntimeError(
            "monolith_discover({}) response top-level JSON was not an object: "
            f"{type(response).__name__}"
        )
    if response.get("transport_error"):
        raise RuntimeError(
            "monolith_discover({}) transport error: "
            f"{str(response.get('raw', ''))[:200]}"
        )
    if (
        response.get("parse_error")
        or response.get("protocol_error")
        or response.get("error") is not None
    ):
        raise RuntimeError(
            "monolith_discover({}) protocol error: "
            f"{str(response.get('raw', response))[:200]}"
        )
    data = result_data(response)
    if not data or "namespaces" not in data:
        raise RuntimeError(
            "monolith_discover({}) did not return namespaces. "
            f"raw response snippet: {str(response)[:400]}"
        )
    namespaces = [row for row in data["namespaces"] if isinstance(row, dict)]
    return namespaces, str(data.get("catalog_version", "")).strip()


def discover_namespaces(url: str, timeout_s: float) -> List[Dict[str, Any]]:
    """Backward-compatible namespace-only view of discover_catalog_summary."""
    namespaces, _catalog_version = discover_catalog_summary(url, timeout_s)
    return namespaces


def verify_catalog_version(url: str, catalog_version: str, timeout_s: float) -> None:
    """Require the enumerated catalog to remain unchanged until preflight ends."""
    if not catalog_version:
        # Legacy/fake servers may not expose a version. Count and duplicate
        # checks still apply, but production Monolith always supplies one.
        return
    response = mcp_call(
        url,
        "monolith_discover",
        {
            "mode": "summary",
            "limit": ACTION_PAGE_LIMIT,
            "if_version": catalog_version,
        },
        timeout_s=timeout_s,
    )
    if not isinstance(response, dict):
        raise RuntimeError(
            "catalog version recheck response top-level JSON was not an object: "
            f"{type(response).__name__}"
        )
    if response.get("transport_error"):
        raise RuntimeError(
            "catalog version recheck transport error: "
            f"{str(response.get('raw', ''))[:200]}"
        )
    if (
        response.get("parse_error")
        or response.get("protocol_error")
        or response.get("error") is not None
    ):
        raise RuntimeError(
            "catalog version recheck protocol error: "
            f"{str(response.get('raw', response))[:200]}"
        )
    data = result_data(response)
    observed_version = str(data.get("catalog_version", "")).strip()
    if observed_version != catalog_version:
        raise RuntimeError(
            "catalog version changed during enumeration "
            f"({catalog_version} -> {observed_version or '<missing>'})"
        )


def discover_namespace_actions(url: str, namespace: str, timeout_s: float) -> List[str]:
    """Enumerate one namespace's action names via paginated mode="actions".

    Follows next_offset while the listing reports truncated=true. Raises
    RuntimeError on transport failure, a page without an "actions" list, or a
    pagination loop that does not terminate — enumeration drift must fail fast
    instead of being silently scored as an empty catalog (N5, 2026-07-10).
    """
    actions: List[str] = []
    offset = 0
    for _page in range(MAX_ACTION_PAGES_PER_NAMESPACE):
        response = mcp_call(
            url,
            "monolith_discover",
            {"namespace": namespace, "mode": "actions", "limit": ACTION_PAGE_LIMIT, "offset": offset},
            timeout_s=timeout_s,
        )
        if not isinstance(response, dict):
            raise RuntimeError(
                f"monolith_discover(namespace={namespace}, mode=actions, offset={offset}) "
                f"response top-level JSON was not an object: {type(response).__name__}"
            )
        if response.get("transport_error"):
            raise CatalogTransportError(
                f"monolith_discover(namespace={namespace}, mode=actions, offset={offset}) "
                f"transport error: {str(response.get('raw', ''))[:200]}"
            )
        if (
            response.get("parse_error")
            or response.get("protocol_error")
            or response.get("error") is not None
        ):
            raise RuntimeError(
                f"monolith_discover(namespace={namespace}, mode=actions, offset={offset}) "
                f"protocol error: {str(response.get('raw', response))[:200]}"
            )
        data = result_data(response)
        rows = data.get("actions") if isinstance(data, dict) else None
        if not isinstance(rows, list):
            raise RuntimeError(
                f"monolith_discover(namespace={namespace}, mode=actions) returned no 'actions' "
                f"list (keys={sorted(data.keys())[:12] if isinstance(data, dict) else type(data).__name__}) "
                "— discover contract drift"
            )
        for row in rows:
            name = row.get("action") if isinstance(row, dict) else row
            name = str(name or "").strip()
            if name:
                actions.append(name)
        next_offset = data.get("next_offset")
        if not bool(data.get("truncated")) or not isinstance(next_offset, (int, float)):
            return actions
        if int(next_offset) <= offset:
            raise RuntimeError(
                f"monolith_discover(namespace={namespace}, mode=actions) pagination did not "
                f"advance (offset={offset}, next_offset={next_offset})"
            )
        offset = int(next_offset)
    raise RuntimeError(
        f"monolith_discover(namespace={namespace}, mode=actions) pagination did not terminate "
        f"after {MAX_ACTION_PAGES_PER_NAMESPACE} pages"
    )


def enumerate_catalog_actions(
    url: str,
    namespaces: List[Dict[str, Any]],
    timeout_s: float,
) -> Tuple[List[Tuple[str, str]], List[str]]:
    """Build the flat (namespace, action) scan list from summary rows.

    Legacy summary rows that still inline an "actions" list are honored;
    compact rows are enumerated with paginated mode="actions" calls. Rows with
    action_count == 0 (optional modules that are disabled or not installed)
    are skipped. Returns (pairs, enumeration_errors); any enumeration error
    means the scan list is incomplete and the caller must abort rather than
    score a partial catalog.
    """
    pairs: List[Tuple[str, str]] = []
    errors: List[str] = []
    for ns_row in namespaces:
        ns = str(ns_row.get("namespace", "")).strip()
        if not ns:
            continue

        names: List[str] = []
        inline = ns_row.get("actions")
        if isinstance(inline, list) and inline:
            for act in inline:
                if isinstance(act, dict):
                    act = act.get("action", "")
                act = str(act).strip()
                if act:
                    names.append(act)
        else:
            raw_count = ns_row.get("action_count")
            expected_count = int(raw_count) if isinstance(raw_count, (int, float)) else None
            if expected_count == 0:
                continue
            try:
                names = discover_namespace_actions(url, ns, timeout_s)
            except CatalogTransportError as exc:
                # A complete catalog is mandatory, so no later namespace can make
                # this run valid. Abort enumeration immediately instead of paying
                # one request timeout for every remaining namespace while the
                # endpoint is unavailable or the editor dispatch queue is blocked.
                errors.append(str(exc))
                return pairs, errors
            except RuntimeError as exc:
                errors.append(str(exc))
                continue
            if expected_count is not None and len(names) != expected_count:
                errors.append(
                    f"namespace {ns}: summary action_count={expected_count} but mode=actions "
                    f"enumerated {len(names)} actions"
                )
        for act in names:
            pairs.append((ns, act))
    return pairs, errors


def discover_complete_catalog_state(
    url: str,
    timeout_s: float,
) -> Tuple[List[Dict[str, Any]], List[Tuple[str, str]], str]:
    """Enumerate one coherent non-empty catalog and preserve its version."""
    namespaces, catalog_version = discover_catalog_summary(url, timeout_s)
    pairs, errors = enumerate_catalog_actions(url, namespaces, timeout_s)
    if errors:
        raise RuntimeError("; ".join(errors))
    if not pairs:
        raise RuntimeError(
            "catalog enumeration returned 0 actions — discover contract drift or dead endpoint"
        )
    if len(pairs) != len(set(pairs)):
        raise RuntimeError("catalog enumeration returned duplicate namespace.action pairs")
    verify_catalog_version(url, catalog_version, timeout_s)
    return namespaces, pairs, catalog_version


def discover_complete_catalog(
    url: str,
    timeout_s: float,
) -> Tuple[List[Dict[str, Any]], List[Tuple[str, str]]]:
    """Backward-compatible catalog view without the preserved version."""
    namespaces, pairs, _catalog_version = discover_complete_catalog_state(url, timeout_s)
    return namespaces, pairs


def validate_status_catalog_version(
    status: Dict[str, Any],
    enumerated_catalog_version: str,
) -> Optional[str]:
    """Return a mismatch reason when status no longer describes the enumerated catalog."""
    if not enumerated_catalog_version:
        return None
    status_catalog_version = str(status.get("catalog_version", "")).strip()
    if status_catalog_version != enumerated_catalog_version:
        return (
            "catalog version changed between enumeration and status preflight "
            f"({enumerated_catalog_version} -> {status_catalog_version or '<missing>'})"
        )
    return None


def build_catalog_index(
    namespaces: List[Dict[str, Any]],
    pairs: List[Tuple[str, str]],
) -> Dict[str, set[str]]:
    index: Dict[str, set[str]] = {
        str(row.get("namespace", "")).strip(): set()
        for row in namespaces
        if str(row.get("namespace", "")).strip()
    }
    for namespace, action in pairs:
        index.setdefault(namespace, set()).add(action)
    return index


def fetch_schema_for_action(
    url: str,
    namespace: str,
    action: str,
    timeout_s: float,
) -> SchemaFetchOutcome:
    """Fetch one action schema without flattening transport/protocol failures."""
    try:
        response = mcp_call(
            url,
            "monolith_discover",
            {"namespace": namespace, "action": action, "mode": "schema"},
            timeout_s=timeout_s,
        )
    except Exception as exc:  # noqa: BLE001 - classify benchmark implementation failures.
        error = f"{type(exc).__name__}: {exc}"
        return SchemaFetchOutcome(None, "runner_exception", error=error, raw=error)

    if not isinstance(response, dict):
        raw = str(response)[:500]
        return SchemaFetchOutcome(
            None,
            "protocol_error",
            raw=raw,
            error="schema response top-level JSON was not an object",
        )
    if response.get("transport_error"):
        status = response.get("status")
        return SchemaFetchOutcome(
            None,
            "transport_error",
            transport_error=True,
            status=status if isinstance(status, int) and not isinstance(status, bool) else None,
            raw=str(response.get("raw", ""))[:500],
            error="schema transport failure",
        )
    if response.get("parse_error") or response.get("protocol_error"):
        raw = str(response.get("raw", ""))[:500]
        return SchemaFetchOutcome(
            None,
            "protocol_error",
            raw=raw,
            error="schema response was not valid JSON",
        )
    if response.get("error") is not None:
        raw = json.dumps(response.get("error"), ensure_ascii=False)[:500]
        return SchemaFetchOutcome(
            None,
            "protocol_error",
            raw=raw,
            error="schema request returned a JSON-RPC error",
        )

    data = result_data(response)
    schema = data.get("schema") if isinstance(data, dict) else None
    if not isinstance(schema, dict):
        return SchemaFetchOutcome(
            None,
            "schema_not_returned",
            raw=str(response)[:500],
            error="schema_not_returned",
        )
    return SchemaFetchOutcome(schema, "ok")


def discover_schema_for_action(
    url: str,
    namespace: str,
    action: str,
    timeout_s: float,
) -> Optional[Dict[str, Any]]:
    """Compatibility wrapper returning only the schema on a successful fetch."""
    return fetch_schema_for_action(url, namespace, action, timeout_s).schema


def failed_schema_quality() -> Dict[str, Any]:
    """Return the stable hard-failure score without invoking scoring code."""
    return {
        "param_types_declared": False,
        "required_params_marked": False,
        "value_domain": False,
        "planning_signals_present": False,
        "skill_routing_present": False,
        "output_contract_declared": False,
        "schema_score": 0.0,
        "value_domain_diagnostics": [],
    }


def fetch_and_score_schema_target(
    url: str,
    namespace: str,
    action: str,
    timeout_s: float,
) -> Tuple[SchemaFetchOutcome, Dict[str, Any]]:
    """Create the common per-action row consumed by scan and probe."""
    try:
        outcome = fetch_schema_for_action(url, namespace, action, timeout_s)
        raw_schema = (
            canonical_raw_schema(outcome.schema)
            if outcome.failure_kind == "ok" and outcome.schema is not None
            else None
        )
        raw_schema_sha256 = _sha256_json(raw_schema) if raw_schema is not None else None
        quality = score_schema_quality(raw_schema)
    except Exception as exc:  # noqa: BLE001 - preserve one triggering row and abort upstream.
        error = f"{type(exc).__name__}: {exc}"
        outcome = SchemaFetchOutcome(
            None,
            "runner_exception",
            raw=error,
            error=error,
        )
        raw_schema = None
        raw_schema_sha256 = None
        quality = failed_schema_quality()
    error_msg = "" if outcome.failure_kind == "ok" else (
        f"{outcome.failure_kind}: {outcome.error or outcome.raw}"
    )
    row: Dict[str, Any] = {
        "namespace": namespace,
        "action": action,
        "param_types_declared": quality["param_types_declared"],
        "required_params_marked": quality["required_params_marked"],
        "value_domain": quality["value_domain"],
        "planning_signals_present": quality["planning_signals_present"],
        "skill_routing_present": quality["skill_routing_present"],
        "output_contract_declared": quality["output_contract_declared"],
        "schema_score": quality["schema_score"],
        "value_domain_diagnostics": quality["value_domain_diagnostics"],
        "raw_schema_hash_kind": RAW_SCHEMA_HASH_KIND,
        "raw_schema": raw_schema,
        "raw_schema_sha256": raw_schema_sha256,
        "error": error_msg,
        "failure_kind": outcome.failure_kind,
        "transport_error": outcome.transport_error,
        "transport_status": outcome.status,
        "transport_error_raw": outcome.raw if outcome.transport_error else "",
    }
    return outcome, row


# ---------------------------------------------------------------------------
# Schema quality scoring
# ---------------------------------------------------------------------------

_MISSING_PARAMS = object()


def _raw_user_param_entries(schema: Dict[str, Any]) -> Tuple[bool, Dict[str, Any]]:
    """Return whether a param contract is applicable and every declared entry.

    Only an absent ``params`` key or an object with no user-facing entries is
    genuinely param-less. A non-object container and non-object entry values are
    malformed parameter contracts, not an excuse to score the dimensions N/A.
    """
    params = schema.get("params", _MISSING_PARAMS)
    if params is _MISSING_PARAMS:
        return False, {}
    if not isinstance(params, dict):
        return True, {"<params>": params}
    entries = {
        str(name): value
        for name, value in params.items()
        if not str(name).startswith("_")
    }
    return bool(entries), entries


def _type_atoms(type_text: str) -> Set[str]:
    """Return the normalized top-level union members of a wire type string.

    Monolith schemas use compact unions such as ``array|string|object|number``.
    Substring matching makes that example look like a numeric-only parameter
    even though the numeric form is just one of four accepted representations.
    Keep the parsing deliberately small and exact: these are wire-type atoms,
    not free-form descriptions.
    """
    return {atom.strip().lower() for atom in type_text.split("|") if atom.strip()}


def _is_numeric_domain_type(type_text: str) -> bool:
    """True when every non-null union member is a numeric wire type."""
    atoms = _type_atoms(type_text)
    non_null_atoms = atoms - {"null"}
    return bool(non_null_atoms) and non_null_atoms <= {"integer", "number"}


def _is_constrained_param(meta: Dict[str, Any]) -> bool:
    """
    A param is "constrained" when its accepted values are not free-form: it
    declares an enum, a numeric range, or a numeric/bool/path type/kind that has
    a documentable value domain. These are exactly the params whose undocumented
    value domain causes the wrong-param-contract failures the structural booleans
    cannot see (ROI report A4 item 1).
    """
    if "enum" in meta:
        return True
    if "minimum" in meta or "maximum" in meta:
        return True
    type_text = str(meta.get("type", ""))
    type_atoms = _type_atoms(type_text)
    if _is_numeric_domain_type(type_text) or type_atoms == {"bool"}:
        return True
    kind_text = str(meta.get("kind", "")).lower()
    if kind_text and kind_text != "other":
        return True
    return False


KNOWN_DOMAIN_KINDS = frozenset(
    {"unbounded", "dynamic", "cross_field", "composite", "normalized"}
)


def _is_nonempty_string(value: Any) -> bool:
    return isinstance(value, str) and bool(value.strip())


def _is_finite_number(value: Any) -> bool:
    return (
        isinstance(value, (int, float))
        and not isinstance(value, bool)
        and math.isfinite(float(value))
    )


def _is_nonempty_string_list(value: Any) -> bool:
    return (
        isinstance(value, list)
        and bool(value)
        and all(_is_nonempty_string(item) for item in value)
    )


def _validate_sentinels(domain: Dict[str, Any]) -> Optional[str]:
    """Return a stable failure reason for malformed optional sentinels."""
    if "sentinels" not in domain:
        return None
    sentinels = domain.get("sentinels")
    if not isinstance(sentinels, list) or not sentinels:
        return "domain_sentinels_must_be_nonempty_array"
    for sentinel in sentinels:
        if not isinstance(sentinel, dict):
            return "domain_sentinel_must_be_object"
        if not _is_finite_number(sentinel.get("value")):
            return "domain_sentinel_value_must_be_finite_number"
        if not _is_nonempty_string(sentinel.get("meaning")):
            return "domain_sentinel_meaning_required"
    return None


def _validate_declared_domain(domain: Any) -> Tuple[bool, str, Optional[str]]:
    """Validate the non-enforcing nested per-param ``domain`` contract."""
    if not isinstance(domain, dict):
        return False, "domain_must_be_object", None

    raw_kind = domain.get("kind")
    if not _is_nonempty_string(raw_kind):
        return False, "domain_kind_required", None
    kind = str(raw_kind).strip().lower()
    if kind not in KNOWN_DOMAIN_KINDS:
        return False, "domain_kind_unknown", kind

    sentinel_error = _validate_sentinels(domain)
    if sentinel_error:
        return False, sentinel_error, kind

    if kind == "unbounded":
        if not _is_nonempty_string(domain.get("rationale")):
            return False, "unbounded_domain_rationale_required", kind
    elif kind == "dynamic":
        if not _is_nonempty_string(domain.get("source")):
            return False, "dynamic_domain_source_required", kind
        if not _is_nonempty_string(domain.get("rationale")):
            return False, "dynamic_domain_rationale_required", kind
    elif kind == "cross_field":
        if not _is_nonempty_string(domain.get("rule")):
            return False, "cross_field_domain_rule_required", kind
        if not _is_nonempty_string_list(domain.get("depends_on")):
            return False, "cross_field_domain_depends_on_required", kind
    elif kind == "composite":
        if not _is_nonempty_string(domain.get("rule")):
            return False, "composite_domain_rule_required", kind
        if not _is_nonempty_string_list(domain.get("variants")):
            return False, "composite_domain_variants_required", kind
    elif kind == "normalized":
        if domain.get("mode") != "clamp":
            return False, "normalized_domain_mode_must_be_clamp", kind
        minimum = domain.get("minimum")
        maximum = domain.get("maximum")
        if not _is_finite_number(minimum) or not _is_finite_number(maximum):
            return False, "normalized_domain_bounds_required", kind
        if float(minimum) > float(maximum):
            return False, "normalized_domain_bounds_inverted", kind
        if not _is_nonempty_string(domain.get("rationale")):
            return False, "normalized_domain_rationale_required", kind

    return True, f"ok_explicit_{kind}", kind


def _param_value_domain_diagnostic(param: str, meta: Dict[str, Any]) -> Dict[str, Any]:
    """Return param-level value-domain evidence without changing action scoring."""
    type_text = meta.get("type")
    type_variants = sorted(_type_atoms(type_text)) if isinstance(type_text, str) else []
    diagnostic: Dict[str, Any] = {
        "param": param,
        "ok": False,
        "reason": "",
        "type_variants": type_variants,
        "derived_domain_kind": None,
        "declared_domain_kind": None,
    }

    if not isinstance(type_text, str) or not type_text.strip():
        diagnostic["reason"] = "type_required"
        return diagnostic
    if not _is_nonempty_string(meta.get("description")):
        diagnostic["reason"] = "description_required"
        return diagnostic
    if not isinstance(meta.get("required"), bool):
        diagnostic["reason"] = "required_flag_must_be_boolean"
        return diagnostic

    enum_values = meta.get("enum")
    if "enum" in meta and (not isinstance(enum_values, list) or not enum_values):
        diagnostic["reason"] = "enum_must_be_nonempty_array"
        return diagnostic

    minimum = meta.get("minimum")
    maximum = meta.get("maximum")
    if "minimum" in meta and not _is_finite_number(minimum):
        diagnostic["reason"] = "minimum_must_be_finite_number"
        return diagnostic
    if "maximum" in meta and not _is_finite_number(maximum):
        diagnostic["reason"] = "maximum_must_be_finite_number"
        return diagnostic
    if "minimum" in meta and "maximum" in meta and float(minimum) > float(maximum):
        diagnostic["reason"] = "accepted_bounds_inverted"
        return diagnostic
    if ("minimum" in meta or "maximum" in meta) and not _is_numeric_domain_type(type_text):
        diagnostic["reason"] = "accepted_bounds_require_numeric_domain_type"
        return diagnostic

    domain_ok = False
    domain_reason = ""
    declared_kind: Optional[str] = None
    if "domain" in meta:
        domain_ok, domain_reason, declared_kind = _validate_declared_domain(meta.get("domain"))
        diagnostic["declared_domain_kind"] = declared_kind
        if not domain_ok:
            diagnostic["reason"] = domain_reason
            return diagnostic

    has_enum = isinstance(enum_values, list) and bool(enum_values)
    has_minimum = "minimum" in meta
    has_maximum = "maximum" in meta
    if has_enum:
        diagnostic.update(ok=True, reason="ok_enum", derived_domain_kind="enum")
        return diagnostic
    if has_minimum and has_maximum:
        diagnostic.update(ok=True, reason="ok_bounded", derived_domain_kind="bounded")
        return diagnostic
    if has_minimum:
        diagnostic.update(ok=True, reason="ok_lower_bounded", derived_domain_kind="lower_bounded")
        return diagnostic
    if has_maximum:
        diagnostic.update(ok=True, reason="ok_upper_bounded", derived_domain_kind="upper_bounded")
        return diagnostic
    if domain_ok and declared_kind:
        diagnostic.update(
            ok=True,
            reason=domain_reason,
            derived_domain_kind=declared_kind,
        )
        return diagnostic

    if _is_numeric_domain_type(type_text):
        diagnostic["reason"] = "numeric_domain_missing"
        diagnostic["derived_domain_kind"] = "missing"
        return diagnostic

    non_null_variants = set(type_variants) - {"null"}
    if non_null_variants & {"integer", "number"}:
        diagnostic["derived_domain_kind"] = "mixed_representation"
        diagnostic["reason"] = "ok_mixed_representation"
    else:
        diagnostic["derived_domain_kind"] = "free_form"
        diagnostic["reason"] = "ok_free_form"
    diagnostic["ok"] = True
    return diagnostic


def param_value_domain_diagnostics(schema: Dict[str, Any]) -> List[Dict[str, Any]]:
    """Return stable diagnostics for every user-facing param in declaration order."""
    has_contract, entries = _raw_user_param_entries(schema)
    if not has_contract:
        return []
    diagnostics: List[Dict[str, Any]] = []
    for param, meta in entries.items():
        if isinstance(meta, dict):
            diagnostics.append(_param_value_domain_diagnostic(param, meta))
        else:
            diagnostics.append({
                "param": param,
                "ok": False,
                "reason": (
                    "params_must_be_object"
                    if param == "<params>"
                    else "param_schema_must_be_object"
                ),
                "type_variants": [],
                "derived_domain_kind": None,
                "declared_domain_kind": None,
            })
    return diagnostics


def _param_value_domain_ok(meta: Dict[str, Any]) -> bool:
    """Compatibility boolean used by callers that do not need diagnostics."""
    return bool(_param_value_domain_diagnostic("", meta)["ok"])


def score_schema_quality(schema: Optional[Dict[str, Any]]) -> Dict[str, Any]:
    """
    Score one action's schema for six quality dimensions.

    Returns a dict with the per-dimension results and a float schema_score.

    Dimension result encoding:
      * True  — dimension satisfied
      * False — dimension violated
      * None  — dimension Not Applicable (param-gated dimensions on a param-less
                action). N/A dimensions are excluded from rate denominators and
                from the per-action schema_score, never auto-passed.

    If schema is None (transport/parse failure), every param-gated dimension is
    False (a fetch failure is a hard failure, not an N/A) and the non-param
    dimensions are False too, yielding schema_score 0.0.
    """
    if schema is None:
        return failed_schema_quality()

    has_params, user_param_entries = _raw_user_param_entries(schema)

    value_domain_diagnostics = param_value_domain_diagnostics(schema)

    if has_params:
        # 1. param_types_declared: EVERY declared param carries a "type".
        param_types_declared: Optional[bool] = all(
            isinstance(meta, dict)
            and isinstance(meta.get("type"), str)
            and bool(str(meta.get("type")).strip())
            for meta in user_param_entries.values()
        )

        # 2. required_params_marked: EVERY declared param carries a boolean
        #    "required" flag (so the required set is fully, correctly specified —
        #    not merely "at least one is required").
        required_params_marked: Optional[bool] = all(
            isinstance(meta, dict) and isinstance(meta.get("required"), bool)
            for meta in user_param_entries.values()
        )

        # 3. value_domain: every param is typed + described with a correct
        #    required flag, and constrained params document their allowed values
        #    (enum non-empty / numeric range present).
        value_domain: Optional[bool] = all(
            diagnostic["ok"] for diagnostic in value_domain_diagnostics
        )
    else:
        # Param-less action: the param contract is N/A, not a free pass.
        param_types_declared = None
        required_params_marked = None
        value_domain = None

    # 4. planning_signals_present: "planning_signals" key exists and is a non-empty list.
    planning_signals = schema.get("planning_signals")
    planning_signals_present = isinstance(planning_signals, list) and len(planning_signals) > 0

    # 5. skill_routing_present: "skill" key exists and is a non-empty string.
    skill = schema.get("skill")
    skill_routing_present = isinstance(skill, str) and bool(skill.strip())

    # 6. output_contract_declared: "output_contract_status" is explicitly set to
    #    "declared" or "not_declared" (not absent).
    output_contract_status = schema.get("output_contract_status")
    output_contract_declared = output_contract_status in ("declared", "not_declared")

    # schema_score is the mean of the APPLICABLE dimensions only (N/A excluded).
    applicable = [
        flag
        for flag in (
            param_types_declared,
            required_params_marked,
            value_domain,
            planning_signals_present,
            skill_routing_present,
            output_contract_declared,
        )
        if flag is not None
    ]
    schema_score = round(sum(1.0 for f in applicable if f) / len(applicable), 6) if applicable else 0.0

    return {
        "param_types_declared": param_types_declared,
        "required_params_marked": required_params_marked,
        "value_domain": value_domain,
        "planning_signals_present": planning_signals_present,
        "skill_routing_present": skill_routing_present,
        "output_contract_declared": output_contract_declared,
        "schema_score": schema_score,
        "value_domain_diagnostics": value_domain_diagnostics,
    }


# ---------------------------------------------------------------------------
# Aggregate metrics
# ---------------------------------------------------------------------------

# Default ceiling on the fraction of schema fetches that may fail before the
# run itself is treated as failed. An editor dying mid-run produces a burst of
# schema_not_returned rows that would otherwise be recorded as a legitimate
# (depressed) baseline with exit 0 — observed 2026-07-11 as an 18/100 blueprint
# failure burst during an editor crash loop.
DEFAULT_MAX_FAILED_FRACTION = 0.05


def check_fetch_failure_budget(
    failed_count: int,
    row_count: int,
    max_failed_fraction: float,
    unit_label: str,
) -> int:
    """Return the process exit code for a completed run's fetch-failure rate."""
    if row_count <= 0:
        return 0
    failed_fraction = failed_count / row_count
    if failed_fraction > max_failed_fraction:
        print(
            f"[schema_completeness] ERROR: {failed_count}/{row_count} {unit_label} failed "
            f"schema fetch ({failed_fraction:.1%} > --max-failed-fraction {max_failed_fraction:.1%}) — "
            "treating the run as failed (endpoint instability or contract drift), not as a baseline.",
            file=sys.stderr,
        )
        return 1
    return 0


def avg(values: List[float]) -> float:
    return sum(values) / len(values) if values else 0.0


def dimension_rate(rows: List[Dict[str, Any]], field: str) -> float:
    """
    Fraction of rows that satisfy a dimension, counting only APPLICABLE rows.

    A None value means N/A (param-gated dimension on a param-less action) and is
    excluded from both numerator and denominator — it is never folded in as 1.0.
    Returns 0.0 when the dimension is N/A for every row.
    """
    applicable = [r.get(field) for r in rows if r.get(field) is not None]
    if not applicable:
        return 0.0
    return avg([1.0 if v else 0.0 for v in applicable])


def param_domain_aggregate(rows: List[Dict[str, Any]]) -> Tuple[int, int, float]:
    """Return total/pass/coverage over emitted per-param domain diagnostics."""
    total = 0
    passed = 0
    for row in rows:
        diagnostics = row.get("value_domain_diagnostics")
        if not isinstance(diagnostics, list):
            continue
        for diagnostic in diagnostics:
            if not isinstance(diagnostic, dict):
                continue
            total += 1
            if diagnostic.get("ok") is True:
                passed += 1
    coverage = passed / total if total else 0.0
    return total, passed, coverage


def aggregate_metrics(label: str, rows: List[Dict[str, Any]], total_expected: int, ns_breakdown: Dict[str, Any]) -> Dict[str, Any]:
    """
    Compute aggregate schema_completeness_score and per-dimension rates.

    schema_completeness_score =
        0.25 * param_types_declared_rate
      + 0.20 * required_params_marked_rate
      + 0.20 * value_domain_rate
      + 0.15 * planning_signals_present_rate
      + 0.10 * skill_routing_present_rate
      + 0.10 * output_contract_declared_rate

    Param-gated rates (param_types/required/value_domain) are computed over only
    the actions that declare params; param-less actions are N/A, not auto-1.0.
    """
    if not rows:
        zero = {
            "schema_completeness_score": 0.0,
            "param_types_declared_rate": 0.0,
            "required_params_marked_rate": 0.0,
            "value_domain_rate": 0.0,
            "planning_signals_present_rate": 0.0,
            "skill_routing_present_rate": 0.0,
            "output_contract_declared_rate": 0.0,
            "mean_schema_score": 0.0,
            "quality_pass_action_count": 0,
            "quality_fail_action_count": 0,
            "failed_action_count": 0,
            "scanned_action_count": 0,
            "param_bearing_action_count": 0,
            "param_less_action_count": 0,
            "param_domain_total": 0,
            "param_domain_pass": 0,
            "param_domain_coverage": 0.0,
        }
        return {
            "label": label,
            "created_at": utc_now(),
            "scanned_action_count": 0,
            "total_expected_action_count": total_expected,
            "failed_action_count": 0,
            "namespace_count": 0,
            "metrics": zero,
            "namespace_breakdown": ns_breakdown,
        }

    failed = [r for r in rows if r.get("error")]

    ptd = dimension_rate(rows, "param_types_declared")
    rpm = dimension_rate(rows, "required_params_marked")
    vd = dimension_rate(rows, "value_domain")
    psp = dimension_rate(rows, "planning_signals_present")
    srp = dimension_rate(rows, "skill_routing_present")
    ocd = dimension_rate(rows, "output_contract_declared")

    schema_completeness_score = (
        W_PARAM_TYPES * ptd
        + W_REQUIRED_PARAMS * rpm
        + W_VALUE_DOMAIN * vd
        + W_PLANNING_SIGNALS * psp
        + W_SKILL_ROUTING * srp
        + W_OUTPUT_CONTRACT * ocd
    )

    mean_schema_score = avg([float(r.get("schema_score", 0.0)) for r in rows])
    quality_pass_action_count = sum(1 for row in rows if schema_quality_pass(row))
    namespaces = {r.get("namespace") for r in rows if r.get("namespace")}
    # An action is param-bearing when value_domain is applicable (not None).
    param_bearing = sum(1 for r in rows if r.get("value_domain") is not None)
    param_domain_total, param_domain_pass, param_domain_coverage = param_domain_aggregate(rows)

    return {
        "label": label,
        "created_at": utc_now(),
        "scanned_action_count": len(rows),
        "total_expected_action_count": total_expected,
        "failed_action_count": len(failed),
        "namespace_count": len(namespaces),
        "metrics": {
            "schema_completeness_score": round(schema_completeness_score, 6),
            "param_types_declared_rate": round(ptd, 6),
            "required_params_marked_rate": round(rpm, 6),
            "value_domain_rate": round(vd, 6),
            "planning_signals_present_rate": round(psp, 6),
            "skill_routing_present_rate": round(srp, 6),
            "output_contract_declared_rate": round(ocd, 6),
            "mean_schema_score": round(mean_schema_score, 6),
            "quality_pass_action_count": quality_pass_action_count,
            "quality_fail_action_count": len(rows) - quality_pass_action_count,
            "failed_action_count": len(failed),
            "scanned_action_count": len(rows),
            "param_bearing_action_count": param_bearing,
            "param_less_action_count": len(rows) - param_bearing,
            "param_domain_total": param_domain_total,
            "param_domain_pass": param_domain_pass,
            "param_domain_coverage": round(param_domain_coverage, 6),
        },
        "namespace_breakdown": ns_breakdown,
    }


def _dimension_applicable(rows: List[Dict[str, Any]], field: str) -> bool:
    """True when at least one row scores this dimension (non-None)."""
    return any(r.get(field) is not None for r in rows)


def _renormalized_score(rows: List[Dict[str, Any]]) -> float:
    """Weighted schema score with N/A dimensions excluded and the remaining
    weights renormalized.

    A namespace whose actions are all param-less has no parameter contract to
    evaluate; folding the three param-gated dimensions in as 0.0 capped such a
    namespace at 0.35 even when every applicable dimension passed (observed on
    `slate`/`reflect` in baseline-20260711). Renormalizing over the applicable
    dimensions scores it by what it can actually be measured on.
    """
    weighted = [
        (W_PARAM_TYPES, "param_types_declared"),
        (W_REQUIRED_PARAMS, "required_params_marked"),
        (W_VALUE_DOMAIN, "value_domain"),
        (W_PLANNING_SIGNALS, "planning_signals_present"),
        (W_SKILL_ROUTING, "skill_routing_present"),
        (W_OUTPUT_CONTRACT, "output_contract_declared"),
    ]
    total_weight = 0.0
    score = 0.0
    for weight, field in weighted:
        if not _dimension_applicable(rows, field):
            continue
        total_weight += weight
        score += weight * dimension_rate(rows, field)
    return score / total_weight if total_weight > 0 else 0.0


def build_namespace_breakdown(rows: List[Dict[str, Any]]) -> Dict[str, Any]:
    """Return per-namespace aggregated quality metrics."""
    by_ns: Dict[str, List[Dict[str, Any]]] = {}
    for row in rows:
        ns = str(row.get("namespace", "unknown"))
        by_ns.setdefault(ns, []).append(row)

    breakdown: Dict[str, Any] = {}
    for ns, ns_rows in sorted(by_ns.items()):
        failed_count = sum(1 for r in ns_rows if r.get("error"))

        ptd = round(dimension_rate(ns_rows, "param_types_declared"), 6)
        rpm = round(dimension_rate(ns_rows, "required_params_marked"), 6)
        vd = round(dimension_rate(ns_rows, "value_domain"), 6)
        psp = round(dimension_rate(ns_rows, "planning_signals_present"), 6)
        srp = round(dimension_rate(ns_rows, "skill_routing_present"), 6)
        ocd = round(dimension_rate(ns_rows, "output_contract_declared"), 6)
        ns_score = round(_renormalized_score(ns_rows), 6)
        quality_pass_action_count = sum(1 for row in ns_rows if schema_quality_pass(row))
        param_bearing_count = sum(1 for r in ns_rows if r.get("value_domain") is not None)
        param_domain_total, param_domain_pass, param_domain_coverage = param_domain_aggregate(ns_rows)
        breakdown[ns] = {
            "action_count": len(ns_rows),
            "failed_count": failed_count,
            "param_bearing_count": param_bearing_count,
            # False when the namespace is entirely param-less: the three
            # param-gated rate fields below are then vacuous 0.0 placeholders
            # and are EXCLUDED from schema_completeness_score (renormalized).
            "param_gated_applicable": param_bearing_count > 0,
            "param_domain_total": param_domain_total,
            "param_domain_pass": param_domain_pass,
            "param_domain_coverage": round(param_domain_coverage, 6),
            "schema_completeness_score": ns_score,
            "quality_pass_action_count": quality_pass_action_count,
            "quality_fail_action_count": len(ns_rows) - quality_pass_action_count,
            "param_types_declared_rate": ptd,
            "required_params_marked_rate": rpm,
            "value_domain_rate": vd,
            "planning_signals_present_rate": psp,
            "skill_routing_present_rate": srp,
            "output_contract_declared_rate": ocd,
        }
    return breakdown


# ---------------------------------------------------------------------------
# Scan subcommand
# ---------------------------------------------------------------------------

def cmd_scan(args: argparse.Namespace) -> int:
    url: str = args.mcp_url
    output_dir: pathlib.Path = args.output_dir
    label: str = args.label
    timeout_s: float = args.request_timeout_s
    max_actions: Optional[int] = args.max_actions
    resume_requested = bool(getattr(args, "resume", False))

    output_dir.mkdir(parents=True, exist_ok=True)
    if resume_requested and max_actions is not None:
        write_run_failure(output_dir, {
            "label": label,
            "completion_status": "aborted_invalid_resume_configuration",
            "failure_stage": "configuration",
            "error": "--resume cannot be combined with --max-actions; only a full-catalog scan is resumable",
            "metrics_valid": False,
            "metrics_scope": "not_started",
        })
        return 1
    if not resume_requested:
        clear_run_outputs(output_dir)

    print(
        f"[schema_completeness] scan started  label={label}  url={url}"
        f"  resume={str(resume_requested).lower()}",
        flush=True,
    )

    # Step 1: discover all namespaces, then enumerate their actions through the
    # compact discover contract (summary rows carry only action_count; action
    # names come from paginated mode="actions" calls per namespace).
    print("[schema_completeness] calling monolith_discover({}) ...", flush=True)
    try:
        namespaces, all_pairs, catalog_version = discover_complete_catalog_state(
            url, timeout_s=max(timeout_s, 20.0)
        )
    except Exception as exc:  # noqa: BLE001 - preflight defects must write invalid artifacts.
        print(f"[schema_completeness] ENUMERATION ERROR: {exc}", file=sys.stderr)
        write_run_failure(output_dir, {
            "label": label,
            "completion_status": "aborted_catalog_preflight",
            "failure_stage": "catalog_preflight",
            "error": str(exc),
            "completed_action_count": 0,
            "total_action_count": 0,
        })
        return 1

    full_catalog_total = len(all_pairs)
    if max_actions is not None and max_actions > 0:
        all_pairs = all_pairs[:max_actions]
        print(
            f"[schema_completeness] --max-actions={max_actions}: "
            f"scanning {len(all_pairs)}/{full_catalog_total} actions",
            flush=True,
        )
    else:
        print(
            f"[schema_completeness] discovered {full_catalog_total} actions "
            f"across {len(namespaces)} namespaces",
            flush=True,
        )
    total = len(all_pairs)
    is_full_catalog = total == full_catalog_total
    status_outcome = fetch_status_preflight(url, timeout_s)
    status = status_outcome.status_data
    if status is None:
        failure = {
            "label": label,
            "completion_status": "aborted_status_preflight",
            "failure_stage": "status_preflight",
            "completed_action_count": 0,
            "total_action_count": total,
            "max_transport_failed_fraction": args.max_transport_failed_fraction,
            "max_consecutive_transport_failures": args.max_consecutive_transport_failures,
            "min_transport_fraction_sample": args.min_transport_fraction_sample,
        }
        failure.update(status_failure_fields(status_outcome))
        write_run_failure(output_dir, failure)
        print(
            f"[schema_completeness] STATUS PREFLIGHT ERROR: {status_outcome.error}",
            file=sys.stderr,
        )
        return 1
    catalog_status_error = validate_status_catalog_version(status, catalog_version)
    if catalog_status_error:
        write_run_failure(output_dir, {
            "label": label,
            "completion_status": "aborted_catalog_status_mismatch",
            "failure_stage": "catalog_status_recheck",
            "error": catalog_status_error,
            "enumerated_catalog_version": catalog_version,
            "status_catalog_version": str(status.get("catalog_version", "")).strip(),
            "completed_action_count": 0,
            "total_action_count": total,
            "metrics_valid": False,
            "metrics_scope": "not_started",
        })
        print(
            f"[schema_completeness] CATALOG STATUS ERROR: {catalog_status_error}",
            file=sys.stderr,
        )
        return 1
    benchmark_inputs = build_schema_registry_inputs(
        status=status,
        namespaces=namespaces,
        catalog_pairs=all_pairs,
    )

    # Step 2: initialize or strictly load the full-scan checkpoint. A resumed
    # segment may reuse only successful rows from the exact same catalog/input
    # identity. Failed attempts remain provenance and are retried.
    per_action_path = output_dir / "per_action.jsonl"
    try:
        transport_tracker = TransportFailureTracker(
            max_failed_fraction=args.max_transport_failed_fraction,
            max_consecutive_failures=args.max_consecutive_transport_failures,
            min_fraction_samples=args.min_transport_fraction_sample,
        )
    except ValueError as exc:
        write_run_failure(output_dir, {
            "label": label,
            "completion_status": "aborted_invalid_configuration",
            "failure_stage": "configuration",
            "error": str(exc),
            "completed_action_count": 0,
            "total_action_count": total,
            "metrics_valid": False,
            "metrics_scope": "not_started",
        })
        return 1

    checkpoint: Optional[Dict[str, Any]] = None
    resume_identity: Optional[Dict[str, Any]] = None
    rows_by_id: Dict[str, Dict[str, Any]] = {}
    expected_action_ids = set(catalog_action_ids(all_pairs))

    if is_full_catalog:
        try:
            resume_identity = scan_resume_identity(
                args=args,
                catalog_version=catalog_version,
                pairs=all_pairs,
                benchmark_inputs=benchmark_inputs,
            )
            if resume_requested:
                checkpoint = load_scan_checkpoint(
                    output_dir / SCAN_CHECKPOINT_FILENAME
                )
                if checkpoint.get("state") == "completed":
                    if (output_dir / "summary.json").is_file():
                        print(
                            "[schema_completeness] completed scan already has summary.json; "
                            "nothing to resume",
                            file=sys.stderr,
                        )
                        return 1
                    raise ValueError(
                        "completed scan checkpoint is missing summary.json"
                    )
                validate_checkpoint_identity(checkpoint, resume_identity)
                if (output_dir / "summary.json").exists():
                    if checkpoint.get("state") != "publishing":
                        raise ValueError(
                            "summary.json exists for a checkpoint that is not in publishing state"
                        )
                    # A process may stop after atomically publishing the summary
                    # but before the final checkpoint-state flip. Re-run the
                    # verified publication after resume instead of treating that
                    # crash window as a completed or mixed baseline.
                    (output_dir / "summary.json").unlink()
                    namespace_path = output_dir / "namespace_breakdown.json"
                    if namespace_path.exists():
                        namespace_path.unlink()
                if checkpoint.get("results_file") != "per_action.jsonl":
                    raise ValueError(
                        "scan checkpoint results_file must be exactly per_action.jsonl"
                    )
                rows_by_id = load_checkpoint_results(
                    per_action_path,
                    expected_action_ids=expected_action_ids,
                )
                recorded_count = checkpoint.get("completed_valid_action_count")
                if (
                    isinstance(recorded_count, bool)
                    or not isinstance(recorded_count, int)
                    or recorded_count < 0
                ):
                    raise ValueError(
                        "scan checkpoint completed_valid_action_count must be a non-negative integer"
                    )
                if recorded_count > len(rows_by_id):
                    raise ValueError(
                        "scan checkpoint claims more completed results than per_action.jsonl contains"
                    )
                if recorded_count < len(rows_by_id):
                    checkpoint["recovery_events"].append({
                        "detected_at": utc_now(),
                        "kind": "result_appended_before_checkpoint_flush",
                        "recorded_count": recorded_count,
                        "validated_result_count": len(rows_by_id),
                    })
            else:
                checkpoint = new_scan_checkpoint(
                    resume_identity=resume_identity,
                    benchmark_inputs=benchmark_inputs,
                )
                write_jsonl_atomic(per_action_path, [])
            persist_scan_checkpoint(output_dir, checkpoint, len(rows_by_id))
        except (OSError, ValueError) as exc:
            failure = {
                "label": label,
                "completion_status": "aborted_checkpoint_mismatch",
                "failure_stage": "checkpoint_preflight",
                "error": str(exc),
                "run_valid": False,
                "metrics_valid": False,
                "metrics_scope": "not_started",
                "completed_action_count": 0,
                "total_action_count": total,
            }
            attach_benchmark_inputs(failure, benchmark_inputs)
            write_run_failure(output_dir, failure)
            print(
                f"[schema_completeness] CHECKPOINT ERROR: {exc}",
                file=sys.stderr,
            )
            return 1
    elif resume_requested:
        # Defensive; --resume + --max-actions was rejected before enumeration.
        raise AssertionError("diagnostic subset unexpectedly entered resume path")

    segment: Optional[Dict[str, Any]] = None
    if checkpoint is not None:
        segment_index = len(checkpoint["segments"]) + 1
        segment = {
            "segment_id": f"segment-{segment_index:04d}",
            "started_at": utc_now(),
            "resume_requested": resume_requested,
            "starting_valid_action_count": len(rows_by_id),
            "attempted_action_count": 0,
            "valid_result_count": 0,
            "invalid_result_count": 0,
            "transport_failure_count": 0,
            "server_identity": {
                key: status[key]
                for key in (
                    "server_name",
                    "plugin_version",
                    "catalog_version",
                    "project_name",
                    "engine_version",
                    "editor_pid",
                )
                if key in status
            },
        }
        checkpoint["segments"].append(segment)
        checkpoint["state"] = "in_progress"
        persist_scan_checkpoint(output_dir, checkpoint, len(rows_by_id))

    def ordered_valid_rows() -> List[Dict[str, Any]]:
        return [
            rows_by_id[f"{namespace}.{action}"]
            for namespace, action in all_pairs
            if f"{namespace}.{action}" in rows_by_id
        ]

    def update_segment_running(last_action_id: str = "") -> None:
        if checkpoint is None or segment is None:
            return
        segment["attempted_action_count"] = transport_tracker.attempted_count
        segment["valid_result_count"] = (
            len(rows_by_id) - int(segment["starting_valid_action_count"])
        )
        segment["invalid_result_count"] = sum(
            1
            for event in checkpoint["invalid_attempts"]
            if event.get("segment_id") == segment["segment_id"]
        )
        segment["transport_failure_count"] = transport_tracker.failure_count
        if last_action_id:
            segment["last_action_id"] = last_action_id
        persist_scan_checkpoint(output_dir, checkpoint, len(rows_by_id))

    def emit_partial(completion_status: str = "in_progress") -> Dict[str, Any]:
        rows = ordered_valid_rows()
        ns_breakdown = build_namespace_breakdown(rows)
        partial = aggregate_metrics(label, rows, total, ns_breakdown)
        partial.update({
            "completed_action_count": len(rows),
            "completed_valid_action_count": len(rows),
            "segment_attempted_action_count": transport_tracker.attempted_count,
            "remaining_action_count": total - len(rows),
            "total_action_count": total,
            "full_catalog_action_count": full_catalog_total,
            "run_valid": None,
            "completion_status": completion_status,
            "metrics_valid": False,
            "metrics_scope": (
                "checkpoint_valid_subset" if checkpoint is not None else "diagnostic_subset"
            ),
            "comparable": False,
        })
        partial.update(transport_tracker.snapshot())
        if checkpoint is not None:
            partial["attempted_invalid_action_count"] = len(
                checkpoint["invalid_attempts"]
            )
            partial["checkpoint_provenance"] = {
                "checkpoint_file": SCAN_CHECKPOINT_FILENAME,
                "result_file": "per_action.jsonl",
                "segment_count": len(checkpoint["segments"]),
                "outage_count": len(checkpoint["outages"]),
                "invalid_attempt_count": len(checkpoint["invalid_attempts"]),
            }
        attach_benchmark_inputs(partial, benchmark_inputs)
        write_json(output_dir / "partial_summary.json", partial)
        return partial

    def abort_segment(
        *,
        completion_status: str,
        failure_stage: str,
        last_action_id: str = "",
        extra: Optional[Dict[str, Any]] = None,
    ) -> int:
        if checkpoint is not None and segment is not None:
            update_segment_running(last_action_id)
            segment["ended_at"] = utc_now()
            segment["completion_status"] = completion_status
            checkpoint["state"] = "interrupted"
            persist_scan_checkpoint(output_dir, checkpoint, len(rows_by_id))
        failure = emit_partial(completion_status)
        failure.update({
            "run_valid": False,
            "failure_stage": failure_stage,
            "metrics_valid": False,
        })
        if last_action_id:
            failure["last_action_id"] = last_action_id
        if extra:
            failure.update(extra)
        write_run_failure(output_dir, failure)
        write_jsonl_atomic(per_action_path, ordered_valid_rows())
        return 1

    pending_pairs = [
        (namespace, action)
        for namespace, action in all_pairs
        if f"{namespace}.{action}" not in rows_by_id
    ]
    starting_valid_count = len(rows_by_id)
    print(
        f"[schema_completeness] checkpoint valid={starting_valid_count}/{total} "
        f"pending={len(pending_pairs)}",
        flush=True,
    )

    for segment_index, (ns, act) in enumerate(pending_pairs, 1):
        action_id = f"{ns}.{act}"
        progress = len(rows_by_id) + 1
        print(
            f"[{action_id} pending={segment_index}/{len(pending_pairs)} "
            f"valid_target={progress}/{total}]",
            flush=True,
        )

        outcome, row = fetch_and_score_schema_target(url, ns, act, timeout_s)
        transport_decision = transport_tracker.observe(
            transport_error=outcome.transport_error,
            item_id=action_id,
            status=outcome.status,
            raw=outcome.raw,
        )

        try:
            validate_schema_result_row(
                row,
                expected_action_ids=expected_action_ids,
            )
        except ValueError as exc:
            if checkpoint is not None and segment is not None:
                checkpoint["invalid_attempts"].append({
                    "detected_at": utc_now(),
                    "segment_id": segment["segment_id"],
                    "action_id": action_id,
                    "failure_kind": "invalid_result_contract",
                    "error": str(exc),
                    "transport_error": False,
                    "transport_status": None,
                    "transport_error_raw": "",
                })
            update_segment_running(action_id)
            return abort_segment(
                completion_status="aborted_invalid_result_contract",
                failure_stage="schema_scoring",
                last_action_id=action_id,
                extra={"error": str(exc)},
            )

        if outcome.failure_kind == "ok":
            if action_id in rows_by_id:
                if checkpoint is not None and segment is not None:
                    checkpoint["invalid_attempts"].append({
                        "detected_at": utc_now(),
                        "segment_id": segment["segment_id"],
                        "action_id": action_id,
                        "failure_kind": "duplicate_action_result",
                        "error": f"duplicate action result produced: {action_id}",
                        "transport_error": False,
                        "transport_status": None,
                        "transport_error_raw": "",
                    })
                update_segment_running(action_id)
                return abort_segment(
                    completion_status="aborted_duplicate_action_result",
                    failure_stage="schema_fetch",
                    last_action_id=action_id,
                    extra={"error": f"duplicate action result produced: {action_id}"},
                )
            rows_by_id[action_id] = row
            append_jsonl_row(per_action_path, row)
        elif checkpoint is not None and segment is not None:
            event = {
                "detected_at": utc_now(),
                "segment_id": segment["segment_id"],
                "action_id": action_id,
                "failure_kind": outcome.failure_kind,
                "error": outcome.error,
                "transport_error": outcome.transport_error,
                "transport_status": outcome.status,
                "transport_error_raw": outcome.raw if outcome.transport_error else "",
            }
            checkpoint["invalid_attempts"].append(event)
            if outcome.transport_error:
                checkpoint["outages"].append({
                    "detected_at": event["detected_at"],
                    "segment_id": segment["segment_id"],
                    "action_id": action_id,
                    "transport_status": outcome.status,
                    "transport_error_raw": outcome.raw,
                })

        update_segment_running(action_id)

        if outcome.failure_kind == "runner_exception":
            print(
                "[schema_completeness] ERROR: runner exception while fetching "
                f"{action_id}: {outcome.error}",
                file=sys.stderr,
            )
            return abort_segment(
                completion_status="aborted_runner_exception",
                failure_stage="schema_fetch",
                last_action_id=action_id,
                extra={"exception": outcome.error},
            )

        if transport_decision:
            if checkpoint is not None and checkpoint["outages"]:
                checkpoint["outages"][-1]["gate_reason"] = transport_decision.reason
                persist_scan_checkpoint(output_dir, checkpoint, len(rows_by_id))
            print(
                "[schema_completeness] ERROR: aborting scan segment after "
                f"{transport_tracker.attempted_count} attempts "
                f"({transport_decision.reason}; transport failures "
                f"{transport_tracker.failure_count}, consecutive "
                f"{transport_tracker.consecutive_failures}).",
                file=sys.stderr,
            )
            return abort_segment(
                completion_status="aborted_transport_failure_budget",
                failure_stage="schema_fetch",
                last_action_id=transport_decision.item_id,
                extra={"transport_gate_reason": transport_decision.reason},
            )

        if (
            transport_tracker.attempted_count % PARTIAL_FLUSH_EVERY == 0
            or segment_index == len(pending_pairs)
        ):
            emit_partial()

    final_transport_decision = transport_tracker.finalize()
    if final_transport_decision:
        return abort_segment(
            completion_status="completed_transport_failure_budget_exceeded",
            failure_stage="schema_fetch",
            last_action_id=final_transport_decision.item_id,
            extra={"transport_gate_reason": final_transport_decision.reason},
        )

    rows = ordered_valid_rows()
    missing_action_ids = sorted(expected_action_ids - set(rows_by_id))
    if missing_action_ids:
        return abort_segment(
            completion_status="incomplete_valid_results",
            failure_stage="schema_fetch",
            last_action_id=missing_action_ids[0],
            extra={
                "error": (
                    f"{len(missing_action_ids)} catalog actions do not have a valid schema result"
                ),
                "missing_valid_action_count": len(missing_action_ids),
                "missing_valid_action_ids": missing_action_ids,
            },
        )
    if len(rows) != total or len(rows_by_id) != total:
        return abort_segment(
            completion_status="aborted_result_set_mismatch",
            failure_stage="completion_contract",
            extra={
                "error": "valid result set does not exactly match the enumerated catalog",
            },
        )

    # Step 3: re-check the live identity after the final schema. A segment that
    # completed just as the editor disappeared retains all valid rows but does
    # not publish a summary until a later --resume can verify the same catalog.
    try:
        verify_catalog_version(url, catalog_version, timeout_s=max(timeout_s, 20.0))
        final_status_outcome = fetch_status_preflight(url, timeout_s)
        final_status = final_status_outcome.status_data
        if final_status is None:
            raise RuntimeError(
                f"completion status preflight failed: {final_status_outcome.failure_kind}: "
                f"{final_status_outcome.error or final_status_outcome.raw}"
            )
        final_catalog_error = validate_status_catalog_version(
            final_status,
            catalog_version,
        )
        if final_catalog_error:
            raise RuntimeError(final_catalog_error)
        final_inputs = build_schema_registry_inputs(
            status=final_status,
            namespaces=namespaces,
            catalog_pairs=all_pairs,
        )
        if is_full_catalog:
            final_identity = scan_resume_identity(
                args=args,
                catalog_version=catalog_version,
                pairs=all_pairs,
                benchmark_inputs=final_inputs,
            )
            if final_identity != resume_identity:
                raise RuntimeError(
                    "benchmark inputs changed between segment preflight and completion"
                )
        benchmark_inputs = final_inputs
    except Exception as exc:  # noqa: BLE001 - completion identity is a hard gate.
        completion_error = str(exc)
        if (
            checkpoint is not None
            and segment is not None
            and any(
                marker in completion_error.lower()
                for marker in ("transport", "timeout", "connection")
            )
        ):
            checkpoint["outages"].append({
                "detected_at": utc_now(),
                "segment_id": segment["segment_id"],
                "action_id": "<completion_identity_recheck>",
                "transport_status": None,
                "transport_error_raw": completion_error[:500],
                "gate_reason": "completion_identity_recheck",
            })
            persist_scan_checkpoint(output_dir, checkpoint, len(rows_by_id))
        return abort_segment(
            completion_status="aborted_completion_identity_recheck",
            failure_stage="completion_identity_recheck",
            extra={"error": completion_error},
        )

    ns_breakdown = build_namespace_breakdown(rows)
    summary = aggregate_metrics(label, rows, total, ns_breakdown)
    summary.update({
        "run_valid": True,
        "completion_status": "completed",
        "metrics_valid": True,
        "metrics_scope": "complete_run" if is_full_catalog else "diagnostic_subset",
        "comparable": is_full_catalog,
        "completed_action_count": total,
        "completed_valid_action_count": total,
        "remaining_action_count": 0,
        "total_action_count": total,
        "full_catalog_action_count": full_catalog_total,
    })
    summary.update(transport_tracker.snapshot())
    attach_benchmark_inputs(summary, benchmark_inputs)

    if checkpoint is not None and segment is not None:
        update_segment_running()
        segment["ended_at"] = utc_now()
        segment["completion_status"] = "publishing"
        checkpoint["state"] = "publishing"
        persist_scan_checkpoint(output_dir, checkpoint, len(rows_by_id))
        summary["checkpoint_provenance"] = {
            "checkpoint_file": SCAN_CHECKPOINT_FILENAME,
            "result_file": "per_action.jsonl",
            "segment_count": len(checkpoint["segments"]),
            "resumed": len(checkpoint["segments"]) > 1,
            "outage_count": len(checkpoint["outages"]),
            "invalid_attempt_count": len(checkpoint["invalid_attempts"]),
            "recovery_event_count": len(checkpoint["recovery_events"]),
            "catalog_version": catalog_version,
            "catalog_action_ids_hash_kind": CATALOG_ACTION_IDS_HASH_KIND,
            "catalog_action_ids_sha256": resume_identity[
                "catalog_action_ids_sha256"
            ],
        }

    write_jsonl_atomic(per_action_path, rows)
    write_json_atomic(output_dir / "summary.json", summary)
    write_json_atomic(output_dir / "namespace_breakdown.json", ns_breakdown)
    if checkpoint is not None and segment is not None:
        segment["completion_status"] = "completed"
        checkpoint["state"] = "completed"
        checkpoint["completed_at"] = segment["ended_at"]
        persist_scan_checkpoint(output_dir, checkpoint, len(rows_by_id))
    for stale_name in ("partial_summary.json", "run_failure.json"):
        stale_path = output_dir / stale_name
        if stale_path.exists():
            stale_path.unlink()

    score = summary["metrics"]["schema_completeness_score"]
    print(
        f"[schema_completeness] scan complete  actions={len(rows)}  failed=0"
        f"  comparable={str(is_full_catalog).lower()}"
        f"  schema_completeness_score={score:.4f}",
        flush=True,
    )
    print(json.dumps(summary, indent=2, ensure_ascii=False))
    return 0


# ---------------------------------------------------------------------------
# Probe subcommand
# ---------------------------------------------------------------------------

ALL_DIMENSIONS = list(CHECKPOINT_DIMENSION_FIELDS)


def load_probe_set(
    probe_set_path: pathlib.Path,
    *,
    manifest_path: Optional[pathlib.Path] = None,
) -> List[Dict[str, Any]]:
    """Strictly load the checked-in probe contract and its manifest count."""
    probes: List[Dict[str, Any]] = []
    errors: List[str] = []
    seen: Dict[Tuple[str, str], int] = {}
    with probe_set_path.open(encoding="utf-8") as fh:
        for lineno, line in enumerate(fh, 1):
            line = line.strip()
            if not line:
                continue
            try:
                entry = strict_json_loads(line)
            except (json.JSONDecodeError, ValueError) as exc:
                errors.append(f"line {lineno}: invalid JSON ({exc})")
                continue
            if not isinstance(entry, dict):
                errors.append(f"line {lineno}: row must be a JSON object")
                continue
            ns = str(entry.get("namespace", "")).strip()
            act = str(entry.get("action", "")).strip()
            if not ns or not act:
                errors.append(f"line {lineno}: namespace and action are required")
                continue
            if not CANONICAL_ACTION_ID_RE.fullmatch(ns) or not CANONICAL_ACTION_ID_RE.fullmatch(act):
                errors.append(
                    f"line {lineno}: namespace/action must use canonical lower_snake_case IDs"
                )
                continue
            key = (ns, act)
            if key in seen:
                errors.append(
                    f"line {lineno}: duplicate probe {ns}.{act} (first declared on line {seen[key]})"
                )
                continue
            seen[key] = lineno

            dimensions = entry.get("expected_dimensions")
            if not isinstance(dimensions, list) or not dimensions:
                errors.append(f"line {lineno}: expected_dimensions must be a non-empty array")
                continue
            if any(not isinstance(value, str) for value in dimensions):
                errors.append(f"line {lineno}: expected_dimensions entries must be strings")
                continue
            unknown_dimensions = [str(value) for value in dimensions if value not in ALL_DIMENSIONS]
            if unknown_dimensions:
                errors.append(
                    f"line {lineno}: unknown expected_dimensions={unknown_dimensions}"
                )
                continue
            duplicate_dimensions = sorted({
                value for value in dimensions if dimensions.count(value) > 1
            })
            if duplicate_dimensions:
                errors.append(
                    f"line {lineno}: duplicate expected_dimensions={duplicate_dimensions}"
                )
                continue

            raw_availability = entry.get("availability")
            if raw_availability is None:
                availability = {"mode": "required"}
            elif isinstance(raw_availability, dict):
                availability = dict(raw_availability)
            else:
                errors.append(f"line {lineno}: availability must be an object when present")
                continue
            mode = str(availability.get("mode", "")).strip()
            if mode not in PROBE_AVAILABILITY_MODES:
                errors.append(
                    f"line {lineno}: availability.mode must be one of {sorted(PROBE_AVAILABILITY_MODES)}"
                )
                continue
            gate = availability.get("gate")
            if mode in SKIPPABLE_ABSENCE_MODES:
                if not isinstance(gate, dict) or not str(gate.get("kind", "")).strip() or not str(gate.get("id", "")).strip():
                    errors.append(
                        f"line {lineno}: {mode} availability requires gate.kind and gate.id"
                    )
                    continue
            elif gate is not None:
                errors.append(f"line {lineno}: required availability cannot declare a gate")
                continue

            normalized = dict(entry)
            normalized["namespace"] = ns
            normalized["action"] = act
            normalized["expected_dimensions"] = list(dimensions)
            normalized["availability"] = availability
            normalized["probe_index"] = len(probes) + 1
            probes.append(normalized)

    manifest_path = manifest_path or probe_set_path.parent / "manifest.json"
    if not manifest_path.exists():
        errors.append(f"manifest not found: {manifest_path}")
    else:
        try:
            manifest = strict_json_loads(manifest_path.read_text(encoding="utf-8"))
        except (OSError, json.JSONDecodeError, ValueError) as exc:
            errors.append(f"manifest is not valid JSON: {exc}")
        else:
            expected_count = manifest.get("probe_set_task_count") if isinstance(manifest, dict) else None
            if isinstance(expected_count, bool) or not isinstance(expected_count, int):
                errors.append("manifest probe_set_task_count must be an integer")
            elif expected_count != len(probes):
                errors.append(
                    f"manifest probe_set_task_count={expected_count}, parsed_probe_count={len(probes)}"
                )
            expected_availability_counts = manifest.get("probe_set_availability_counts") if isinstance(manifest, dict) else None
            if expected_availability_counts is not None:
                actual_availability_counts = {
                    mode: sum(1 for probe in probes if probe["availability"]["mode"] == mode)
                    for mode in sorted(PROBE_AVAILABILITY_MODES)
                }
                if not isinstance(expected_availability_counts, dict):
                    errors.append("manifest probe_set_availability_counts must be an object")
                else:
                    normalized_expected_counts = {
                        mode: expected_availability_counts.get(mode)
                        for mode in sorted(PROBE_AVAILABILITY_MODES)
                    }
                    if normalized_expected_counts != actual_availability_counts:
                        errors.append(
                            "manifest probe_set_availability_counts="
                            f"{normalized_expected_counts}, actual={actual_availability_counts}"
                        )

    if errors:
        raise ProbeSetContractError("; ".join(errors))
    if not probes:
        raise ProbeSetContractError("probe set is empty")
    return probes


def _probe_absence_result(probe: Dict[str, Any], result_status: str) -> Dict[str, Any]:
    availability = probe["availability"]
    mode = str(availability["mode"])
    stale = result_status == "stale"
    return {
        "probe_index": int(probe["probe_index"]),
        "namespace": str(probe["namespace"]),
        "action": str(probe["action"]),
        "priority": str(probe.get("priority", "medium")),
        "rationale": str(probe.get("rationale", "")),
        "availability": availability,
        "catalog_presence": "absent",
        "result_status": result_status,
        "diagnostic_code": (
            "required_probe_absent_from_live_catalog"
            if stale
            else f"{mode}_probe_absent"
        ),
        "expected_dimensions": list(probe["expected_dimensions"]),
        "dimension_results": None,
        "expected_passed": None,
        "expected_failed": None,
        "expected_na": None,
        "probe_pass": None,
        "error": "stale_probe" if stale else "",
    }


def classify_probes_against_catalog(
    probes: List[Dict[str, Any]],
    catalog_index: Dict[str, set[str]],
) -> Tuple[List[Dict[str, Any]], List[Dict[str, Any]], List[Dict[str, Any]]]:
    runnable: List[Dict[str, Any]] = []
    skipped: List[Dict[str, Any]] = []
    stale: List[Dict[str, Any]] = []
    for probe in probes:
        namespace = str(probe["namespace"])
        action = str(probe["action"])
        if action in catalog_index.get(namespace, set()):
            runnable.append(probe)
            continue
        mode = str(probe["availability"]["mode"])
        if mode in SKIPPABLE_ABSENCE_MODES:
            skipped.append(_probe_absence_result(probe, "skipped"))
        else:
            stale.append(_probe_absence_result(probe, "stale"))
    return runnable, skipped, stale


def _probe_pass_rates(
    probe_results: List[Dict[str, Any]],
    priority_filter: Optional[str] = None,
) -> Tuple[float, int, int]:
    """
    Compute pass rate, passed count, and total expected-dimension checks.

    Returns (pass_rate, passed_checks, total_checks).
    pass_rate = passed_checks / total_checks, or 0.0 when total_checks == 0.
    """
    passed = 0
    total = 0
    for r in probe_results:
        if r.get("result_status") not in {"scored", "fetch_failed"}:
            continue
        if priority_filter is not None and r.get("priority") != priority_filter:
            continue
        dim_results = r.get("dimension_results") or {}
        for dim in r.get("expected_dimensions", []):
            result = dim_results.get(dim)
            if result is None:
                # N/A (param-gated dimension on a param-less action) — excluded
                # from the denominator, never counted as a failed check.
                continue
            total += 1
            if result:
                passed += 1
    rate = round(passed / total, 6) if total > 0 else 0.0
    return rate, passed, total


def cmd_probe(args: argparse.Namespace) -> int:
    probe_set_path: pathlib.Path = resolve_plugin_path(args.probe_set)
    url: str = args.mcp_url
    output_dir: pathlib.Path = args.output_dir
    label: str = args.label
    timeout_s: float = args.request_timeout_s

    output_dir.mkdir(parents=True, exist_ok=True)
    per_action_path = output_dir / "per_action.jsonl"
    probe_results_path = output_dir / "probe_results.jsonl"
    clear_run_outputs(output_dir)

    print(
        f"[schema_completeness] probe started  label={label}  url={url}  probe_set={probe_set_path}",
        flush=True,
    )

    if not probe_set_path.exists():
        print(f"[schema_completeness] ERROR: probe_set not found: {probe_set_path}", file=sys.stderr)
        write_run_failure(output_dir, {
            "label": label,
            "completion_status": "aborted_probe_contract",
            "failure_stage": "probe_contract",
            "error": f"probe_set not found: {probe_set_path}",
            "completed_action_count": 0,
            "total_action_count": 0,
        })
        return 1

    try:
        probes = load_probe_set(probe_set_path)
    except ProbeSetContractError as exc:
        print(f"[schema_completeness] PROBE CONTRACT ERROR: {exc}", file=sys.stderr)
        write_run_failure(output_dir, {
            "label": label,
            "completion_status": "aborted_probe_contract",
            "failure_stage": "probe_contract",
            "error": str(exc),
            "completed_action_count": 0,
            "total_action_count": 0,
        })
        return 1

    declared_total = len(probes)
    print(f"[schema_completeness] loaded {declared_total} probes from {probe_set_path}", flush=True)
    try:
        namespaces, catalog_pairs, catalog_version = discover_complete_catalog_state(
            url, timeout_s=max(timeout_s, 20.0)
        )
    except Exception as exc:  # noqa: BLE001 - preflight defects must write invalid artifacts.
        print(f"[schema_completeness] CATALOG PREFLIGHT ERROR: {exc}", file=sys.stderr)
        write_run_failure(output_dir, {
            "label": label,
            "completion_status": "aborted_catalog_preflight",
            "failure_stage": "catalog_preflight",
            "error": str(exc),
            "declared_probe_count": declared_total,
            "completed_action_count": 0,
            "total_action_count": 0,
        })
        return 1

    catalog_index = build_catalog_index(namespaces, catalog_pairs)
    runnable, skipped_results, stale_results = classify_probes_against_catalog(
        probes, catalog_index
    )
    preflight = {
        "declared_probe_count": declared_total,
        "catalog_namespace_count": len(namespaces),
        "catalog_action_count": len(catalog_pairs),
        "catalog_action_ids_hash_kind": CATALOG_ACTION_IDS_HASH_KIND,
        "catalog_action_ids_sha256": catalog_action_ids_sha256(catalog_pairs),
        "catalog_version": catalog_version,
        "runnable_probe_count": len(runnable),
        "skipped_probe_count": len(skipped_results),
        "stale_probe_count": len(stale_results),
        "skipped_action_ids": [f"{row['namespace']}.{row['action']}" for row in skipped_results],
        "stale_action_ids": [f"{row['namespace']}.{row['action']}" for row in stale_results],
    }
    write_json(output_dir / "probe_preflight.json", preflight)
    if stale_results:
        diagnostic_rows = sorted(
            skipped_results + stale_results,
            key=lambda row: int(row["probe_index"]),
        )
        write_jsonl(probe_results_path, diagnostic_rows)
        print(
            f"[schema_completeness] ERROR: {len(stale_results)} required probes are absent "
            "from the complete live catalog; aborting before schema fetch.",
            file=sys.stderr,
        )
        for row in stale_results:
            print(
                f"[schema_completeness] STALE PROBE: {row['namespace']}.{row['action']}",
                file=sys.stderr,
            )
        write_run_failure(output_dir, {
            "label": label,
            "completion_status": "aborted_required_probe_absence",
            "failure_stage": "catalog_preflight",
            "declared_probe_count": declared_total,
            "catalog_present_probe_count": len(runnable),
            "skipped_probe_count": len(skipped_results),
            "stale_probe_count": len(stale_results),
            "stale_action_ids": preflight["stale_action_ids"],
            "completed_action_count": 0,
            "total_action_count": len(runnable),
        })
        return 1
    if not runnable:
        write_jsonl(probe_results_path, skipped_results)
        print(
            "[schema_completeness] ERROR: probe contract has no catalog-present actions to score",
            file=sys.stderr,
        )
        write_run_failure(output_dir, {
            "label": label,
            "completion_status": "aborted_no_runnable_probes",
            "failure_stage": "catalog_preflight",
            "declared_probe_count": declared_total,
            "skipped_probe_count": len(skipped_results),
            "completed_action_count": 0,
            "total_action_count": 0,
        })
        return 1

    runnable_total = len(runnable)
    status_outcome = fetch_status_preflight(url, timeout_s)
    status = status_outcome.status_data
    if status is None:
        failure = {
            "label": label,
            "completion_status": "aborted_status_preflight",
            "failure_stage": "status_preflight",
            "declared_probe_count": declared_total,
            "catalog_present_probe_count": runnable_total,
            "skipped_probe_count": len(skipped_results),
            "completed_action_count": 0,
            "total_action_count": runnable_total,
            "max_transport_failed_fraction": args.max_transport_failed_fraction,
            "max_consecutive_transport_failures": args.max_consecutive_transport_failures,
            "min_transport_fraction_sample": args.min_transport_fraction_sample,
        }
        failure.update(status_failure_fields(status_outcome))
        write_run_failure(output_dir, failure)
        write_jsonl(probe_results_path, skipped_results)
        print(
            f"[schema_completeness] STATUS PREFLIGHT ERROR: {status_outcome.error}",
            file=sys.stderr,
        )
        return 1
    catalog_status_error = validate_status_catalog_version(status, catalog_version)
    if catalog_status_error:
        write_run_failure(output_dir, {
            "label": label,
            "completion_status": "aborted_catalog_status_mismatch",
            "failure_stage": "catalog_status_recheck",
            "error": catalog_status_error,
            "enumerated_catalog_version": catalog_version,
            "status_catalog_version": str(status.get("catalog_version", "")).strip(),
            "declared_probe_count": declared_total,
            "catalog_present_probe_count": runnable_total,
            "skipped_probe_count": len(skipped_results),
            "completed_action_count": 0,
            "total_action_count": runnable_total,
            "metrics_valid": False,
            "metrics_scope": "not_started",
        })
        write_jsonl(probe_results_path, skipped_results)
        print(
            f"[schema_completeness] CATALOG STATUS ERROR: {catalog_status_error}",
            file=sys.stderr,
        )
        return 1
    benchmark_inputs = build_schema_registry_inputs(
        status=status,
        namespaces=namespaces,
        catalog_pairs=catalog_pairs,
        probe_set_path=probe_set_path,
    )

    rows: List[Dict[str, Any]] = []          # same schema as scan rows
    probe_results_by_index: Dict[int, Dict[str, Any]] = {
        int(row["probe_index"]): row for row in skipped_results
    }
    try:
        transport_tracker = TransportFailureTracker(
            max_failed_fraction=args.max_transport_failed_fraction,
            max_consecutive_failures=args.max_consecutive_transport_failures,
            min_fraction_samples=args.min_transport_fraction_sample,
        )
    except ValueError as exc:
        write_run_failure(output_dir, {
            "label": label,
            "completion_status": "aborted_invalid_configuration",
            "failure_stage": "configuration",
            "error": str(exc),
            "declared_probe_count": declared_total,
            "catalog_present_probe_count": runnable_total,
            "skipped_probe_count": len(skipped_results),
            "completed_action_count": 0,
            "total_action_count": runnable_total,
            "metrics_valid": False,
            "metrics_scope": "not_started",
        })
        write_jsonl(probe_results_path, skipped_results)
        return 1

    def build_probe_progress(completed_count: int) -> Dict[str, Any]:
        current_probe_results = list(probe_results_by_index.values())
        ns_breakdown = build_namespace_breakdown(rows)
        partial = aggregate_metrics(label, rows, runnable_total, ns_breakdown)
        partial["completed_action_count"] = completed_count
        partial["total_action_count"] = runnable_total
        ppr, _, _ = _probe_pass_rates(current_probe_results)
        cpr, _, _ = _probe_pass_rates(current_probe_results, priority_filter="critical")
        hpr, _, _ = _probe_pass_rates(current_probe_results, priority_filter="high")
        failed_probe_count = sum(
            1 for row in current_probe_results if row.get("expected_failed")
        )
        partial["probe_metrics"] = {
            "declared_probe_count": declared_total,
            "catalog_present_probe_count": runnable_total,
            "scored_probe_count": completed_count,
            "skipped_probe_count": len(skipped_results),
            "skipped_optional_count": sum(
                1 for row in skipped_results
                if row["availability"]["mode"] == "optional"
            ),
            "skipped_feature_gated_count": sum(
                1 for row in skipped_results
                if row["availability"]["mode"] == "feature_gated"
            ),
            "stale_probe_count": 0,
            "probe_pass_rate": ppr,
            "critical_probe_pass_rate": cpr,
            "high_probe_pass_rate": hpr,
            "failed_probe_count": failed_probe_count,
        }
        partial.update(transport_tracker.snapshot())
        attach_benchmark_inputs(partial, benchmark_inputs)
        return partial

    for completed_index, probe in enumerate(runnable, 1):
        probe_index = int(probe["probe_index"])
        ns = str(probe.get("namespace", ""))
        act = str(probe.get("action", ""))
        priority = str(probe.get("priority", "medium"))
        expected_dims: List[str] = list(probe["expected_dimensions"])
        rationale = str(probe.get("rationale", ""))

        print(f"[{ns}.{act} {completed_index}/{runnable_total}]", flush=True)

        outcome, row = fetch_and_score_schema_target(url, ns, act, timeout_s)
        try:
            validate_schema_result_row(
                row,
                expected_action_ids={f"{ns}.{act}"},
            )
        except ValueError as exc:
            failure = build_probe_progress(completed_index - 1)
            failure.update({
                "run_valid": False,
                "completion_status": "aborted_invalid_result_contract",
                "failure_stage": "schema_scoring",
                "last_action_id": f"{ns}.{act}",
                "error": str(exc),
                "metrics_valid": False,
                "metrics_scope": "attempted_prefix_invalid_result_contract",
            })
            write_run_failure(output_dir, failure)
            write_json(output_dir / "partial_summary.json", failure)
            write_jsonl(per_action_path, rows)
            write_jsonl(
                probe_results_path,
                [probe_results_by_index[index] for index in sorted(probe_results_by_index)],
            )
            print(
                "[schema_completeness] ERROR: invalid schema evidence row for "
                f"{ns}.{act}: {exc}",
                file=sys.stderr,
            )
            return 1
        rows.append(row)
        append_jsonl_row(per_action_path, row)

        # Probe-specific result. dimension_results preserves the tri-state
        # (True / False / None), so a param-gated dimension on a param-less
        # action is reported as N/A rather than silently coerced to a fail.
        dim_results: Dict[str, Optional[bool]] = {
            dim: row.get(dim) for dim in ALL_DIMENSIONS
        }
        expected_passed = [d for d in expected_dims if dim_results.get(d) is True]
        expected_failed = [d for d in expected_dims if dim_results.get(d) is False]
        expected_na = [d for d in expected_dims if dim_results.get(d) is None]
        # A probe passes when no expected dimension fails AND at least one
        # expected dimension was applicable and satisfied (an all-N/A probe is
        # inconclusive, not a pass).
        probe_pass = len(expected_failed) == 0 and len(expected_passed) > 0

        probe_result: Dict[str, Any] = {
            "probe_index": probe_index,
            "namespace": ns,
            "action": act,
            "priority": priority,
            "rationale": rationale,
            "availability": probe["availability"],
            "catalog_presence": "present",
            "result_status": "fetch_failed" if outcome.failure_kind != "ok" else "scored",
            "diagnostic_code": (
                "schema_transport_error"
                if outcome.transport_error
                else "schema_fetch_failed" if outcome.failure_kind != "ok" else None
            ),
            "expected_dimensions": expected_dims,
            "dimension_results": dim_results,
            "value_domain_diagnostics": row["value_domain_diagnostics"],
            "expected_passed": expected_passed,
            "expected_failed": expected_failed,
            "expected_na": expected_na,
            "probe_pass": probe_pass,
            "error": row["error"],
            "failure_kind": outcome.failure_kind,
            "transport_error": outcome.transport_error,
            "transport_status": outcome.status,
            "transport_error_raw": outcome.raw if outcome.transport_error else "",
        }
        probe_results_by_index[probe_index] = probe_result

        transport_decision = transport_tracker.observe(
            transport_error=outcome.transport_error,
            item_id=f"{ns}.{act}",
            status=outcome.status,
            raw=outcome.raw,
        )

        if outcome.failure_kind == "runner_exception":
            failure = build_probe_progress(completed_index)
            failure.update({
                "run_valid": False,
                "completion_status": "aborted_runner_exception",
                "failure_stage": "schema_fetch",
                "last_action_id": f"{ns}.{act}",
                "exception": outcome.error,
                "metrics_valid": False,
                "metrics_scope": "attempted_prefix_runner_exception",
            })
            write_run_failure(output_dir, failure)
            write_json(output_dir / "partial_summary.json", failure)
            write_jsonl(per_action_path, rows)
            write_jsonl(
                probe_results_path,
                [probe_results_by_index[index] for index in sorted(probe_results_by_index)],
            )
            print(
                "[schema_completeness] ERROR: runner exception while fetching "
                f"{ns}.{act}: {outcome.error}",
                file=sys.stderr,
            )
            return 1

        if transport_decision:
            failure = build_probe_progress(completed_index)
            failure.update({
                "completion_status": "aborted_transport_failure_budget",
                "failure_stage": "schema_fetch",
                "transport_gate_reason": transport_decision.reason,
                "run_valid": False,
                "last_action_id": transport_decision.item_id,
                "metrics_valid": False,
                "metrics_scope": "attempted_prefix_including_transport_failures",
            })
            write_run_failure(output_dir, failure)
            write_json(output_dir / "partial_summary.json", failure)
            write_jsonl(per_action_path, rows)
            write_jsonl(
                probe_results_path,
                [probe_results_by_index[index] for index in sorted(probe_results_by_index)],
            )
            print(
                "[schema_completeness] ERROR: aborting probe after "
                f"{completed_index}/{runnable_total} catalog-present probes "
                f"({transport_decision.reason}; transport failures "
                f"{transport_tracker.failure_count}, consecutive "
                f"{transport_tracker.consecutive_failures}).",
                file=sys.stderr,
            )
            return 1

        # Flush partial summary every PARTIAL_FLUSH_EVERY actions
        if completed_index % PARTIAL_FLUSH_EVERY == 0 or completed_index == runnable_total:
            partial = build_probe_progress(completed_index)
            partial["run_valid"] = None
            partial["completion_status"] = "in_progress"
            partial["metrics_valid"] = False
            partial["metrics_scope"] = "attempted_prefix"
            write_json(output_dir / "partial_summary.json", partial)

    # Write final output files
    probe_results = [
        probe_results_by_index[index]
        for index in sorted(probe_results_by_index)
    ]
    ns_breakdown = build_namespace_breakdown(rows)
    summary = aggregate_metrics(label, rows, runnable_total, ns_breakdown)

    # Compute final probe metrics
    probe_pass_rate, passed_checks, total_checks = _probe_pass_rates(probe_results)
    critical_probe_pass_rate, _, _ = _probe_pass_rates(probe_results, priority_filter="critical")
    high_probe_pass_rate, _, _ = _probe_pass_rates(probe_results, priority_filter="high")
    scored_probe_count = sum(
        1 for row in probe_results if row.get("result_status") == "scored"
    )
    fetch_failed_probe_count = sum(
        1 for row in probe_results if row.get("result_status") == "fetch_failed"
    )
    failed_probe_count = sum(
        1
        for row in probe_results
        if row.get("result_status") in {"scored", "fetch_failed"}
        and row.get("probe_pass") is not True
    )

    summary["probe_metrics"] = {
        "probe_set_file": str(probe_set_path),
        "probe_count": declared_total,
        "declared_probe_count": declared_total,
        "catalog_present_probe_count": runnable_total,
        "scored_probe_count": scored_probe_count,
        "fetch_failed_probe_count": fetch_failed_probe_count,
        "skipped_probe_count": len(skipped_results),
        "skipped_optional_count": sum(
            1 for row in skipped_results if row["availability"]["mode"] == "optional"
        ),
        "skipped_feature_gated_count": sum(
            1 for row in skipped_results if row["availability"]["mode"] == "feature_gated"
        ),
        "stale_probe_count": 0,
        "probe_pass_rate": probe_pass_rate,
        "critical_probe_pass_rate": critical_probe_pass_rate,
        "high_probe_pass_rate": high_probe_pass_rate,
        "failed_probe_count": failed_probe_count,
        "passed_dimension_checks": passed_checks,
        "total_dimension_checks": total_checks,
    }
    final_transport_decision = transport_tracker.finalize()
    summary.update({
        "run_valid": True,
        "completion_status": "completed",
        "metrics_valid": True,
        "metrics_scope": "complete_run",
    })
    summary.update(transport_tracker.snapshot())
    attach_benchmark_inputs(summary, benchmark_inputs)

    write_jsonl(per_action_path, rows)
    write_jsonl(probe_results_path, probe_results)

    if final_transport_decision:
        summary["run_valid"] = False
        summary["metrics_valid"] = False
        summary["completion_status"] = "completed_transport_failure_budget_exceeded"
        summary["transport_gate_reason"] = final_transport_decision.reason
        summary["last_action_id"] = final_transport_decision.item_id
        write_run_failure(output_dir, summary)
        write_json(output_dir / "partial_summary.json", summary)
        return 1

    fetch_budget_rc = check_fetch_failure_budget(
        summary["failed_action_count"], len(rows), args.max_failed_fraction, "probes"
    )
    if fetch_budget_rc:
        summary["run_valid"] = False
        summary["metrics_valid"] = False
        summary["metrics_scope"] = "complete_run_invalid"
        summary["completion_status"] = "completed_fetch_failure_budget_exceeded"
        summary["max_failed_fraction"] = args.max_failed_fraction
        write_run_failure(output_dir, summary)
        write_json(output_dir / "partial_summary.json", summary)
        return fetch_budget_rc

    write_json(output_dir / "summary.json", summary)
    write_json(output_dir / "namespace_breakdown.json", ns_breakdown)
    partial_path = output_dir / "partial_summary.json"
    if partial_path.exists():
        partial_path.unlink()

    score = summary["metrics"]["schema_completeness_score"]
    print(
        f"[schema_completeness] probe complete"
        f"  declared={declared_total}"
        f"  scored={runnable_total}"
        f"  skipped={len(skipped_results)}"
        f"  failed_fetches={summary['failed_action_count']}"
        f"  schema_completeness_score={score:.4f}"
        f"  probe_pass_rate={probe_pass_rate:.4f}"
        f"  critical_probe_pass_rate={critical_probe_pass_rate:.4f}"
        f"  failed_probe_count={failed_probe_count}",
        flush=True,
    )
    print(json.dumps(summary, indent=2, ensure_ascii=False))
    return 0


# ---------------------------------------------------------------------------
# Compare subcommand
# ---------------------------------------------------------------------------

COMPARE_METRICS = [
    "schema_completeness_score",
    "param_types_declared_rate",
    "required_params_marked_rate",
    "value_domain_rate",
    "param_domain_coverage",
    "param_domain_total",
    "param_domain_pass",
    "planning_signals_present_rate",
    "skill_routing_present_rate",
    "output_contract_declared_rate",
    "mean_schema_score",
    "failed_action_count",
    "scanned_action_count",
]


def cmd_compare(args: argparse.Namespace) -> int:
    baseline_path: pathlib.Path = args.baseline
    current_path: pathlib.Path = args.current
    output_dir: pathlib.Path = args.output_dir

    baseline = json.loads(baseline_path.read_text(encoding="utf-8"))
    current = json.loads(current_path.read_text(encoding="utf-8"))

    base_metrics: Dict[str, Any] = baseline.get("metrics", {})
    cur_metrics: Dict[str, Any] = current.get("metrics", {})

    deltas: Dict[str, Any] = {}
    for key in COMPARE_METRICS:
        cur_value = cur_metrics.get(key)
        base_value = base_metrics.get(key)
        if isinstance(cur_value, (int, float)) and isinstance(base_value, (int, float)):
            deltas[key] = round(float(cur_value) - float(base_value), 6)

    comparison = {
        "created_at": utc_now(),
        "baseline": baseline,
        "current": current,
        "deltas": deltas,
    }
    output_dir.mkdir(parents=True, exist_ok=True)
    write_json(output_dir / "comparison.json", comparison)
    _write_comparison_markdown(output_dir / "comparison.md", comparison)

    print(json.dumps({"output_dir": str(output_dir), "deltas": deltas}, indent=2, ensure_ascii=False))
    return 0


def _write_comparison_markdown(path: pathlib.Path, comparison: Dict[str, Any]) -> None:
    baseline = comparison["baseline"]
    current = comparison["current"]
    deltas = comparison["deltas"]

    lines = [
        "# Monolith Schema Completeness Benchmark Comparison",
        "",
        f"- Created: `{comparison['created_at']}`",
        f"- Baseline: `{baseline.get('label', 'unknown')}`",
        f"- Current: `{current.get('label', 'unknown')}`",
        f"- Scanned actions (current): `{current.get('scanned_action_count', current.get('metrics', {}).get('scanned_action_count', '?'))}`",
        "",
        "| Metric | Baseline | Current | Delta |",
        "| --- | ---: | ---: | ---: |",
    ]
    base_metrics = baseline.get("metrics", {})
    cur_metrics = current.get("metrics", {})
    for metric in COMPARE_METRICS:
        base_value = base_metrics.get(metric, "N/A")
        cur_value = cur_metrics.get(metric, "N/A")
        delta = deltas.get(metric, "N/A")
        lines.append(f"| `{metric}` | {base_value} | {cur_value} | {delta} |")

    lines.append("")
    lines.append(
        "Higher is better for all rate/score metrics. "
        "`failed_action_count` should be minimised."
    )
    path.write_text("\n".join(lines) + "\n", encoding="utf-8", newline="\n")


# ---------------------------------------------------------------------------
# Report subcommand
# ---------------------------------------------------------------------------

def cmd_report(args: argparse.Namespace) -> int:
    summary_path: pathlib.Path = args.summary
    summary = json.loads(summary_path.read_text(encoding="utf-8"))

    label = summary.get("label", "unknown")
    created_at = summary.get("created_at", "unknown")
    metrics = summary.get("metrics", {})
    ns_breakdown = summary.get("namespace_breakdown", {})

    print(f"Schema Completeness Benchmark Report")
    print(f"  Label      : {label}")
    print(f"  Created    : {created_at}")
    print(f"  Actions    : {summary.get('scanned_action_count', '?')} scanned / {summary.get('total_expected_action_count', '?')} expected")
    print(f"  Failed     : {summary.get('failed_action_count', '?')}")
    print(f"  Namespaces : {summary.get('namespace_count', '?')}")
    print()
    print(f"  schema_completeness_score  : {metrics.get('schema_completeness_score', 'N/A'):.4f}" if isinstance(metrics.get('schema_completeness_score'), float) else f"  schema_completeness_score  : {metrics.get('schema_completeness_score', 'N/A')}")
    print(f"  param_types_declared_rate  : {metrics.get('param_types_declared_rate', 'N/A')}")
    print(f"  required_params_marked_rate: {metrics.get('required_params_marked_rate', 'N/A')}")
    print(f"  value_domain_rate          : {metrics.get('value_domain_rate', 'N/A')}")
    print(f"  param_domain_coverage      : {metrics.get('param_domain_coverage', 'N/A')}")
    print(f"  param_domain_pass/total    : {metrics.get('param_domain_pass', '?')} / {metrics.get('param_domain_total', '?')}")
    print(f"  planning_signals_rate      : {metrics.get('planning_signals_present_rate', 'N/A')}")
    print(f"  skill_routing_rate         : {metrics.get('skill_routing_present_rate', 'N/A')}")
    print(f"  output_contract_rate       : {metrics.get('output_contract_declared_rate', 'N/A')}")
    print(f"  mean_schema_score          : {metrics.get('mean_schema_score', 'N/A')}")
    print(f"  param_bearing/param_less   : {metrics.get('param_bearing_action_count', '?')} / {metrics.get('param_less_action_count', '?')}")
    print()

    if ns_breakdown:
        col_w = max((len(ns) for ns in ns_breakdown), default=10)
        header = f"  {'Namespace':<{col_w}}  {'Actions':>7}  {'Failed':>6}  {'Score':>6}  {'PTypes':>6}  {'ReqPrm':>6}  {'ValDom':>6}  {'Signals':>7}  {'Skill':>6}  {'OutCtx':>6}"
        print(header)
        print("  " + "-" * (len(header) - 2))
        for ns, ns_data in sorted(ns_breakdown.items(), key=lambda x: -x[1].get("schema_completeness_score", 0)):
            print(
                f"  {ns:<{col_w}}"
                f"  {ns_data.get('action_count', 0):>7}"
                f"  {ns_data.get('failed_count', 0):>6}"
                f"  {ns_data.get('schema_completeness_score', 0.0):>6.3f}"
                f"  {ns_data.get('param_types_declared_rate', 0.0):>6.3f}"
                f"  {ns_data.get('required_params_marked_rate', 0.0):>6.3f}"
                f"  {ns_data.get('value_domain_rate', 0.0):>6.3f}"
                f"  {ns_data.get('planning_signals_present_rate', 0.0):>7.3f}"
                f"  {ns_data.get('skill_routing_present_rate', 0.0):>6.3f}"
                f"  {ns_data.get('output_contract_declared_rate', 0.0):>6.3f}"
            )
    return 0


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------

def main(argv: Optional[List[str]] = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="cmd", required=True)

    # scan
    scan_cmd = sub.add_parser("scan", help="Scan all actions and score schema quality")
    scan_cmd.add_argument("--mcp-url", default=DEFAULT_MCP_URL, help="MCP endpoint URL")
    scan_cmd.add_argument("--output-dir", type=pathlib.Path, required=True, help="Directory for result files")
    scan_cmd.add_argument("--label", required=True, help="Human-readable label for this scan run")
    scan_cmd.add_argument("--request-timeout-s", type=float, default=8.0, help="Per-request timeout in seconds")
    scan_cmd.add_argument("--max-actions", type=int, default=None, help="Limit scan to first N actions (testing)")
    scan_cmd.add_argument(
        "--resume",
        action="store_true",
        help="Resume a full-catalog scan from --output-dir. Rejects changed inputs, "
             "catalog identity, duplicate result ids, and --max-actions.",
    )
    scan_cmd.add_argument(
        "--max-failed-fraction",
        type=float,
        default=DEFAULT_MAX_FAILED_FRACTION,
        help="Legacy fetch-failure diagnostic threshold (default: "
             f"{DEFAULT_MAX_FAILED_FRACTION}). Full scans are stricter and require "
             "one valid result for every catalog action regardless of this value.",
    )
    scan_cmd.add_argument(
        "--max-transport-failed-fraction",
        type=float,
        default=DEFAULT_MAX_TRANSPORT_FAILED_FRACTION,
        help="Abort without summary when transport failures exceed this fraction "
             f"after {MIN_TRANSPORT_FRACTION_SAMPLE} actions (default: "
             f"{DEFAULT_MAX_TRANSPORT_FAILED_FRACTION})",
    )
    scan_cmd.add_argument(
        "--max-consecutive-transport-failures",
        type=int,
        default=DEFAULT_MAX_CONSECUTIVE_TRANSPORT_FAILURES,
        help="Abort without summary after this many consecutive transport failures "
             f"(default: {DEFAULT_MAX_CONSECUTIVE_TRANSPORT_FAILURES})",
    )
    scan_cmd.add_argument(
        "--min-transport-fraction-sample",
        type=int,
        default=MIN_TRANSPORT_FRACTION_SAMPLE,
        help="Minimum completed requests before applying the transport-fraction gate "
             f"(default: {MIN_TRANSPORT_FRACTION_SAMPLE})",
    )

    # probe
    probe_cmd = sub.add_parser("probe", help="Score expected-dimension pass rates for a probe_set.jsonl")
    probe_cmd.add_argument(
        "--probe-set",
        type=pathlib.Path,
        default=pathlib.Path(DEFAULT_PROBE_SET),
        help=f"Path to probe_set.jsonl (default: {DEFAULT_PROBE_SET})",
    )
    probe_cmd.add_argument("--mcp-url", default=DEFAULT_MCP_URL, help="MCP endpoint URL")
    probe_cmd.add_argument("--output-dir", type=pathlib.Path, required=True, help="Directory for result files")
    probe_cmd.add_argument("--label", required=True, help="Human-readable label for this probe run")
    probe_cmd.add_argument("--request-timeout-s", type=float, default=8.0, help="Per-request timeout in seconds")
    probe_cmd.add_argument(
        "--max-failed-fraction",
        type=float,
        default=DEFAULT_MAX_FAILED_FRACTION,
        help="Maximum fraction of failed probe fetches before the run exits non-zero "
             f"(default: {DEFAULT_MAX_FAILED_FRACTION})",
    )
    probe_cmd.add_argument(
        "--max-transport-failed-fraction",
        type=float,
        default=DEFAULT_MAX_TRANSPORT_FAILED_FRACTION,
        help="Abort without summary when transport failures exceed this fraction "
             f"after {MIN_TRANSPORT_FRACTION_SAMPLE} probes (default: "
             f"{DEFAULT_MAX_TRANSPORT_FAILED_FRACTION})",
    )
    probe_cmd.add_argument(
        "--max-consecutive-transport-failures",
        type=int,
        default=DEFAULT_MAX_CONSECUTIVE_TRANSPORT_FAILURES,
        help="Abort without summary after this many consecutive transport failures "
             f"(default: {DEFAULT_MAX_CONSECUTIVE_TRANSPORT_FAILURES})",
    )
    probe_cmd.add_argument(
        "--min-transport-fraction-sample",
        type=int,
        default=MIN_TRANSPORT_FRACTION_SAMPLE,
        help="Minimum completed requests before applying the transport-fraction gate "
             f"(default: {MIN_TRANSPORT_FRACTION_SAMPLE})",
    )

    # compare
    cmp_cmd = sub.add_parser("compare", help="Compare two scan summary.json files")
    cmp_cmd.add_argument("--baseline", type=pathlib.Path, required=True, help="Baseline summary.json")
    cmp_cmd.add_argument("--current", type=pathlib.Path, required=True, help="Current summary.json")
    cmp_cmd.add_argument("--output-dir", type=pathlib.Path, required=True, help="Directory for comparison output")

    # report
    rep_cmd = sub.add_parser("report", help="Print namespace breakdown from a summary.json")
    rep_cmd.add_argument("--summary", type=pathlib.Path, required=True, help="Path to summary.json")

    args = parser.parse_args(argv)

    if args.cmd == "scan":
        return cmd_scan(args)
    if args.cmd == "probe":
        return cmd_probe(args)
    if args.cmd == "compare":
        return cmd_compare(args)
    if args.cmd == "report":
        return cmd_report(args)
    return 2


if __name__ == "__main__":
    raise SystemExit(main())
