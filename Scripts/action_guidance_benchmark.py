#!/usr/bin/env python3
"""
Reusable Monolith MCP action-guidance benchmark.

The benchmark intentionally avoids LLM calls. It measures whether MCP discovery
and failed-action responses contain enough machine-readable evidence for a
deterministic agent to select an action, correct arguments, and avoid inventing
workflow edges.
"""

from __future__ import annotations

import argparse
import datetime as _dt
import difflib
import json
import math
import pathlib
import socket
import sys
import time
import urllib.error
import urllib.request
from typing import Any, Callable, Dict, Iterable, List, Optional, Tuple

from benchmark_common import (
    benchmark_routing_context,
    DEFAULT_MAX_CONSECUTIVE_TRANSPORT_FAILURES,
    DEFAULT_MAX_TRANSPORT_FAILED_FRACTION,
    DEFAULT_MIN_TRANSPORT_FRACTION_SAMPLES,
    TaskCorpus,
    TransportFailureTracker,
    attach_benchmark_inputs,
    build_benchmark_inputs,
    classify_mcp_protocol_failure,
    display_path,
    load_task_corpus,
    paginate_discover_action_names,
    resolve_plugin_path,
    status_identity,
    status_identity_mismatches,
    task_corpus_metadata,
    validate_mcp_status_response,
)

DEFAULT_MCP_URL = "http://localhost:9316/mcp"
DEFAULT_TASKS = pathlib.Path("Benchmarks/ActionGuidance/tasks.jsonl")
DEFAULT_MANIFEST = pathlib.Path("Benchmarks/ActionGuidance/manifest.json")
DEFAULT_RESULTS_ROOT = pathlib.Path("Saved/Monolith/Benchmarks/ActionGuidance")
DEFAULT_MAX_RECOVERY_CALLS = 3

ACTION_GUIDANCE_TASK_CATEGORIES = {
    "discovery_planning",
    "needed_action_routing",
    "unknown_action_recovery",
    "missing_required_param",
    "invalid_param_type",
}

READ_ONLY_POLICY_IDS = {"", "read_only"}

# ---------------------------------------------------------------------------
# Demand weighting (ITEM 2)
# ---------------------------------------------------------------------------
# Live invocation volume x error cost, sourced from the invocation-log analyzer
# Action Stats table (Saved/Monolith/LogAnalysis/<run>/summary.md). Each entry is
# "<namespace>.<action>": (count, error_rate). The weight a task receives is
# 1.0 + log-scaled(count * error_rate) so that a 294-call/47%-error action moves
# the aggregate far more than a dead 10-call action, without letting a single
# heavy action dominate. Tasks for actions absent from this table keep weight 1.0.
#
# Keep this table in sync with the latest analyzer Action Stats when the demand
# profile shifts; it is intentionally a small, reviewable snapshot of the live
# high-volume/high-error rows rather than a full log re-read at benchmark time.
DEFAULT_WEIGHT = 1.0
_DEMAND_WEIGHT_SOURCE = "Saved/Monolith/LogAnalysis/20260618-205624/summary.md"
_ACTION_STATS_20260618 = {
    "monolith.discover": (5868, 0.088),
    "monolith.find": (5868, 0.088),  # routing twin of discover; shares live demand
    "cppreflect.get_uclass": (981, 0.000),
    "project.search": (601, 0.005),
    "risk.get_hotspot_score": (597, 0.002),
    "decision.list_stale": (467, 0.002),
    "decision.list_decisions": (465, 0.002),
    "source.get_include_path": (434, 0.601),
    "blueprint.get_variables": (390, 0.092),
    "source.get_signature": (380, 0.597),
    "network.list_replicated_classes": (370, 0.003),
    "cppreflect.find_class_specifier": (318, 0.003),
    "risk.get_file_churn": (308, 0.003),
    "risk.get_cochange_pairs": (298, 0.003),
    "blueprint.add_variable": (294, 0.466),
    "cppreflect.list_uproperties": (266, 0.000),
    "network.list_rpc_functions": (262, 0.004),
    "source.search_source": (249, 0.036),
    "source.verify_symbols": (244, 0.574),
    "blueprint.add_function": (241, 0.461),
    "blueprint.compile_blueprint": (235, 0.047),
    "cppreflect.list_ufunctions": (232, 0.000),
    "blueprint.get_functions": (223, 0.018),
    "blueprint.add_node": (177, 0.057),
    "source.find_example_usage": (167, 0.563),
    "source.find_callers": (166, 0.717),
    "source.check_deprecations": (166, 0.566),
    "blueprint.get_graph_data": (165, 0.194),
    "blueprint.get_blueprint_info": (163, 0.018),
    "source.find_callees": (134, 0.724),
    "source.generate_class_stub": (106, 0.660),
    "blueprint.list_graphs": (105, 0.314),
    "source.risk_score": (104, 0.115),
    "blueprint.create_blueprint": (101, 0.871),
    "blueprint.set_variable_defaults": (87, 0.103),
    "blueprint.add_event_dispatcher": (85, 0.212),
    "blueprint.get_graph_summary": (82, 0.207),
    "source.impact_radius": (74, 0.162),
}


def action_weight(namespace: str, action: str) -> float:
    """Live-demand weight for a task targeting ``namespace.action``.

    weight = 1.0 + log10(1 + count * error_rate). High-volume/high-error actions
    earn a larger weight; clean or low-volume actions stay near 1.0. The log keeps
    a single dominant action (e.g. create_blueprint 101*0.871) from swamping the
    aggregate while still clearly outweighing a dead 10-call action.
    """
    stats = _ACTION_STATS_20260618.get(f"{namespace}.{action}")
    if not stats:
        return DEFAULT_WEIGHT
    count, error_rate = stats
    error_cost = max(0.0, float(count) * float(error_rate))
    return round(DEFAULT_WEIGHT + math.log10(1.0 + error_cost), 6)


def task_weight(task: Dict[str, Any]) -> float:
    """Resolve the effective demand weight for a task, honoring an explicit field."""
    explicit = task.get("weight")
    if isinstance(explicit, (int, float)) and explicit > 0:
        return float(explicit)
    return action_weight(str(task.get("namespace", "")), str(task.get("action", "")))


def weighted_avg(pairs: List[Tuple[float, float]]) -> float:
    """Weighted mean of (value, weight) pairs; falls back to unweighted on zero mass."""
    total_weight = sum(weight for _, weight in pairs)
    if total_weight <= 0.0:
        return avg([value for value, _ in pairs])
    return sum(value * weight for value, weight in pairs) / total_weight



def utc_now() -> str:
    return _dt.datetime.now(_dt.timezone.utc).isoformat()


def load_jsonl(path: pathlib.Path) -> List[Dict[str, Any]]:
    path = resolve_plugin_path(path)
    rows: List[Dict[str, Any]] = []
    with path.open("r", encoding="utf-8") as handle:
        for line_no, line in enumerate(handle, 1):
            stripped = line.strip()
            if not stripped:
                continue
            try:
                rows.append(json.loads(stripped))
            except json.JSONDecodeError as exc:
                raise RuntimeError(f"{path}:{line_no}: invalid JSONL row: {exc}") from exc
    return rows


def write_json(path: pathlib.Path, payload: Any) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", encoding="utf-8", newline="\n") as handle:
        json.dump(payload, handle, indent=2, ensure_ascii=False)
        handle.write("\n")


def write_jsonl(path: pathlib.Path, rows: Iterable[Dict[str, Any]]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", encoding="utf-8", newline="\n") as handle:
        for row in rows:
            handle.write(json.dumps(row, ensure_ascii=False, sort_keys=True))
            handle.write("\n")


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
_BENCHMARK_ROUTING_CONTEXT = benchmark_routing_context("ActionGuidance")


def mcp_call(url: str, tool: str, arguments: Dict[str, Any], timeout_s: float = 45.0) -> Dict[str, Any]:
    body = {
        "jsonrpc": "2.0",
        "id": int(time.time() * 1000) % 1000000000,
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
        return {"transport_error": True, "status": exc.code, "raw": raw, "request": body}
    except (TimeoutError, socket.timeout) as exc:
        return {"transport_error": True, "status": None, "raw": f"timeout: {exc}", "request": body}
    except urllib.error.URLError as exc:
        return {"transport_error": True, "status": None, "raw": str(exc), "request": body}
    except OSError as exc:
        return {"transport_error": True, "status": None, "raw": str(exc), "request": body}

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
            "request": body,
        }
    parsed["request"] = body
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
    structured = structured_content(payload)
    return structured if structured else payload


def action_tool(namespace: str, action: str, params: Dict[str, Any]) -> Tuple[str, Dict[str, Any]]:
    if namespace == "monolith":
        return f"monolith_{action}", dict(params)
    args = {"action": action}
    args.update(params)
    return f"{namespace}_query", args


def typo_action(action: str, existing: Iterable[str]) -> str:
    existing_set = set(existing)
    if len(action) >= 5:
        typo = action[:-2] + action[-1] + action[-2]
    else:
        typo = action + "_typo"
    if typo == action or typo in existing_set:
        typo = action + "_typo"
    return typo


def type_names(type_spec: str) -> List[str]:
    return [part.strip().lower() for part in str(type_spec or "").replace(",", "|").split("|") if part.strip()]


def dummy_value_for_type(type_spec: str) -> Any:
    types = type_names(type_spec)
    if "string" in types:
        return "BenchmarkValue"
    if "integer" in types:
        return 1
    if "number" in types:
        return 1.0
    if "boolean" in types or "bool" in types:
        return True
    if "array" in types:
        return []
    if "object" in types:
        return {}
    return "BenchmarkValue"


def wrong_value_for_type(type_spec: str) -> Any:
    types = type_names(type_spec)
    if "integer" in types or "number" in types:
        return "not_a_number"
    if "boolean" in types or "bool" in types:
        return "not_a_bool"
    if "string" in types:
        return 12345
    if "array" in types:
        return "not_an_array"
    if "object" in types:
        return "not_an_object"
    return {"wrong": True}


def schema_params(schema_row: Dict[str, Any]) -> Dict[str, Dict[str, Any]]:
    params = schema_row.get("params")
    if not isinstance(params, dict):
        return {}
    out: Dict[str, Dict[str, Any]] = {}
    for name, value in params.items():
        if name.startswith("_") or not isinstance(value, dict):
            continue
        out[name] = value
    return out


def is_read_only(schema_row: Dict[str, Any]) -> bool:
    policy = schema_row.get("execution_policy")
    if not isinstance(policy, dict):
        return True
    policy_id = str(policy.get("policy_id", "")).lower()
    can_mutate = bool(policy.get("dirty_package_tracking") or policy.get("transaction_wrapping") or policy.get("post_edit_validation"))
    destructive = bool(schema_row.get("destructive_hint", False))
    return (policy_id in READ_ONLY_POLICY_IDS) and not can_mutate and not destructive


def discover_schema(
    url: str,
    namespace: str,
    action: str,
    timeout_s: float = 45.0,
    call_fn: Optional[Callable[[str, str, Dict[str, Any], float], Dict[str, Any]]] = None,
) -> Optional[Dict[str, Any]]:
    caller = call_fn or mcp_call
    response = caller(
        url,
        "monolith_discover",
        {"namespace": namespace, "action": action, "mode": "schema"},
        timeout_s,
    )
    parsed = result_data(response)
    if not parsed:
        return None
    schema = parsed.get("schema")
    return schema if isinstance(schema, dict) else None


def discover_namespace_catalog_strict(
    url: str,
    namespace: str,
    timeout_s: float = 45.0,
) -> Tuple[List[str], Dict[str, Dict[str, Any]]]:
    """Enumerate one namespace and cache its inline action schemas.

    ``monolith_discover(detail=true)`` already returns the same action rows used
    by focused ``mode=schema`` calls.  Fetching those rows once per paginated
    namespace avoids hundreds of redundant round trips while retaining exact
    action-set, pagination, policy, and parameter-schema validation.
    """

    actions: List[str] = []
    schemas: Dict[str, Dict[str, Any]] = {}
    offset = 0
    page_limit = 50
    max_pages = 1000
    for _page in range(max_pages):
        arguments = {
            "namespace": namespace,
            "mode": "actions",
            "limit": page_limit,
            "offset": offset,
            "detail": True,
            "planning_detail": "compact",
            "schema_detail": "full",
        }
        response = mcp_call(url, "monolith_discover", arguments, timeout_s=timeout_s)
        payload = result_data(response) or {}
        rows = payload.get("actions") if isinstance(payload, dict) else None
        if not isinstance(rows, list):
            raise RuntimeError(
                f"monolith_discover(namespace={namespace}, mode=actions, offset={offset}, "
                "detail=true) returned no 'actions' list -- discover contract drift"
            )
        for row_index, row in enumerate(rows):
            if not isinstance(row, dict):
                raise RuntimeError(
                    f"monolith_discover(namespace={namespace}, detail=true) action row "
                    f"{offset + row_index} is not an object"
                )
            action = str(row.get("action", "")).strip()
            if not action:
                raise RuntimeError(
                    f"monolith_discover(namespace={namespace}, detail=true) action row "
                    f"{offset + row_index} has no action name"
                )
            if action in schemas:
                raise RuntimeError(
                    f"monolith_discover returned duplicate action '{namespace}.{action}'"
                )
            actions.append(action)
            schemas[action] = dict(row)

        truncated = payload.get("truncated")
        if not isinstance(truncated, bool):
            raise RuntimeError(
                f"monolith_discover(namespace={namespace}, mode=actions, detail=true) "
                f"returned non-boolean truncated={truncated!r} at offset={offset}"
            )
        next_offset = payload.get("next_offset")
        if not truncated:
            return actions, schemas
        if isinstance(next_offset, bool) or not isinstance(next_offset, int):
            raise RuntimeError(
                f"monolith_discover(namespace={namespace}, mode=actions, detail=true) "
                f"returned truncated=true without an integer next_offset at offset={offset}"
            )
        if next_offset <= offset:
            raise RuntimeError(
                f"monolith_discover(namespace={namespace}, mode=actions, detail=true) "
                f"pagination did not advance (offset={offset}, next_offset={next_offset})"
            )
        offset = next_offset

    raise RuntimeError(
        f"monolith_discover(namespace={namespace}, mode=actions, detail=true) pagination "
        f"did not terminate after {max_pages} pages"
    )


def discover_namespace_actions_strict(url: str, namespace: str, timeout_s: float = 45.0) -> List[str]:
    """Compatibility wrapper returning only the strict action-name list."""

    actions, _schemas = discover_namespace_catalog_strict(url, namespace, timeout_s)
    return actions


def discover_catalog_namespaces(
    url: str,
    timeout_s: float = 45.0,
    expected_catalog_version: str = "",
) -> List[Dict[str, Any]]:
    """Return summary rows enriched with the complete paginated action catalog.

    Compact discovery deliberately keeps action names out of summary rows.  A
    benchmark generator must enumerate ``mode=actions`` for every namespace and
    verify the summary counts; otherwise a response-shape or pagination change
    silently shrinks the generated corpus.
    """

    response = mcp_call(
        url,
        "monolith_discover",
        {"mode": "summary", "limit": 1000},
        timeout_s=timeout_s,
    )
    summary = result_data(response)
    rows = summary.get("namespaces") if isinstance(summary, dict) else None
    if not isinstance(rows, list) or not rows:
        raise RuntimeError("monolith_discover summary did not return namespaces")
    observed_catalog_version = str(summary.get("catalog_version", "")).strip()
    if (
        expected_catalog_version
        and observed_catalog_version != expected_catalog_version
    ):
        raise RuntimeError(
            "catalog version changed before namespace enumeration "
            f"({expected_catalog_version} -> {observed_catalog_version or '<missing>'})"
        )

    normalized: List[Dict[str, Any]] = []
    seen_namespaces = set()
    for index, row in enumerate(rows):
        if not isinstance(row, dict):
            raise RuntimeError(f"monolith_discover summary namespace row {index} is not an object")
        namespace = str(row.get("namespace", "")).strip()
        if not namespace:
            raise RuntimeError(f"monolith_discover summary namespace row {index} has no namespace")
        if namespace in seen_namespaces:
            raise RuntimeError(f"monolith_discover summary returned duplicate namespace '{namespace}'")
        seen_namespaces.add(namespace)

        expected_count = row.get("action_count")
        if isinstance(expected_count, bool) or not isinstance(expected_count, int) or expected_count < 1:
            raise RuntimeError(
                f"monolith_discover summary namespace '{namespace}' has invalid action_count={expected_count!r}"
            )

        actions, schemas = discover_namespace_catalog_strict(url, namespace, timeout_s=timeout_s)
        if len(actions) != len(set(actions)):
            raise RuntimeError(f"monolith_discover returned duplicate actions for namespace '{namespace}'")
        if len(actions) != expected_count:
            raise RuntimeError(
                f"monolith_discover action count mismatch for namespace '{namespace}': "
                f"summary={expected_count}, enumerated={len(actions)}"
            )

        normalized_row = dict(row)
        normalized_row["namespace"] = namespace
        normalized_row["actions"] = actions
        normalized_row["schemas"] = schemas
        normalized.append(normalized_row)

    total_actions = summary.get("total_actions")
    enumerated_total = sum(len(row["actions"]) for row in normalized)
    if isinstance(total_actions, bool) or not isinstance(total_actions, int):
        raise RuntimeError(f"monolith_discover summary has invalid total_actions={total_actions!r}")
    if total_actions != enumerated_total:
        raise RuntimeError(
            "monolith_discover total action count mismatch: "
            f"summary={total_actions}, enumerated={enumerated_total}"
        )

    return normalized


def read_generation_catalog_fingerprint(
    url: str,
    timeout_s: float = 45.0,
) -> Dict[str, Any]:
    """Read strict status+discover identity before/after corpus generation."""
    status_response = mcp_call(url, "monolith_status", {}, timeout_s=timeout_s)
    if not isinstance(status_response, dict):
        raise RuntimeError("monolith_status response top-level JSON was not an object")
    if (
        status_response.get("transport_error")
        or status_response.get("parse_error")
        or status_response.get("protocol_error")
        or status_response.get("error") is not None
    ):
        raise RuntimeError(
            f"monolith_status failed during corpus generation: {str(status_response)[:300]}"
        )
    status = result_data(status_response)
    if not status or status.get("server_running") is not True:
        raise RuntimeError(
            f"monolith_status returned an invalid generation payload: {str(status)[:300]}"
        )

    discover_response = mcp_call(
        url,
        "monolith_discover",
        {"mode": "summary", "limit": 1000},
        timeout_s=timeout_s,
    )
    if not isinstance(discover_response, dict):
        raise RuntimeError("monolith_discover response top-level JSON was not an object")
    if (
        discover_response.get("transport_error")
        or discover_response.get("parse_error")
        or discover_response.get("protocol_error")
        or discover_response.get("error") is not None
    ):
        raise RuntimeError(
            "monolith_discover failed during corpus generation: "
            f"{str(discover_response)[:300]}"
        )
    discover = result_data(discover_response)
    namespaces = discover.get("namespaces") if isinstance(discover, dict) else None
    if not isinstance(namespaces, list) or not namespaces:
        raise RuntimeError("monolith_discover generation fingerprint returned no namespaces")

    status_version = str(status.get("catalog_version", "")).strip()
    discover_version = str(discover.get("catalog_version", "")).strip()
    if not status_version or status_version != discover_version:
        raise RuntimeError(
            "status/discover catalog version mismatch during corpus generation "
            f"({status_version or '<missing>'} != {discover_version or '<missing>'})"
        )
    discover_action_count = discover.get("total_actions")
    if not isinstance(discover_action_count, int) or isinstance(discover_action_count, bool):
        discover_action_count = sum(
            int(row.get("action_count") or 0)
            for row in namespaces
            if isinstance(row, dict)
        )
    status_action_count = status.get("catalog_action_count", status.get("total_actions"))
    status_namespace_count = status.get("catalog_namespace_count", status.get("namespaces"))
    if isinstance(status_action_count, int) and status_action_count != discover_action_count:
        raise RuntimeError(
            "status/discover catalog action-count mismatch during corpus generation "
            f"({status_action_count} != {discover_action_count})"
        )
    if isinstance(status_namespace_count, int) and status_namespace_count != len(namespaces):
        raise RuntimeError(
            "status/discover catalog namespace-count mismatch during corpus generation "
            f"({status_namespace_count} != {len(namespaces)})"
        )
    return {
        "catalog_version": status_version,
        "catalog_action_count": discover_action_count,
        "catalog_namespace_count": len(namespaces),
    }


def representative_actions(actions: List[str], count: int = 3) -> List[str]:
    preferred_prefixes = (
        "get",
        "list",
        "find",
        "search",
        "read",
        "describe",
        "validate",
        "inspect",
        "status",
        "health",
    )
    preferred = [a for a in actions if a.startswith(preferred_prefixes)]
    merged: List[str] = []
    for action in preferred + actions:
        if action not in merged:
            merged.append(action)
        if len(merged) >= count:
            break
    return merged


def task_fingerprint(task: Dict[str, Any]) -> str:
    """Stable identity for de-duplicating generated benchmark tasks."""
    payload = {
        "category": task.get("category"),
        "namespace": task.get("namespace"),
        "action": task.get("action"),
        "tool": task.get("tool"),
        "arguments": task.get("arguments"),
        "expected": task.get("expected"),
    }
    return json.dumps(payload, ensure_ascii=False, sort_keys=True, separators=(",", ":"))


def append_unique_task(tasks: List[Dict[str, Any]], seen: set[str], task: Dict[str, Any]) -> bool:
    fingerprint = task_fingerprint(task)
    if fingerprint in seen:
        return False
    task["id"] = f"AGB-{len(tasks) + 1:03d}"
    seen.add(fingerprint)
    tasks.append(task)
    return True


_STATIC_DISCOVERY_TASKS_20260717 = [
    ("gas", "list_abilities"),
    ("gas", "get_ability_info"),
    ("gas", "list_gameplay_effects"),
    ("gas", "get_gameplay_effect"),
    ("gas", "find_abilities_by_tag"),
    ("niagara", "list_systems"),
    ("niagara", "get_system_summary"),
    ("niagara", "validate_system"),
    ("material", "get_material_properties"),
    ("material", "list_material_instances"),
    ("material", "validate_material"),
    ("ui", "list_widget_types"),
    ("ui", "get_widget_tree"),
    ("ui", "get_widget_bindings"),
    ("blueprint", "list_graphs"),
    ("blueprint", "get_blueprint_info"),
    ("blueprint", "validate_blueprint"),
    ("animation", "get_graphs"),
    ("animation", "get_abp_info"),
    ("animation", "get_montage_info"),
    ("audio", "list_audio_assets"),
    ("audio", "search_audio_assets"),
    ("asset", "inspect_asset"),
    ("scene", "get_level_actors"),
    ("source", "review_context"),
]

_STATIC_READ_ONLY_POLICY_TASKS_20260711 = [
    ("source_control", "list_opened"),
    ("source_control", "map_depot_paths"),
]

_STATIC_UNKNOWN_ACTION_TASKS_20260717 = [
    # Log-derived stale or imagined action names that frequently tempt agents.
    # Renamed legacy actions stay here as migration regressions: the old name
    # must remain absent and the response must route to the current contract.
    ("gas", "get_ability_info", "get_ability"),
    ("niagara", "get_system_summary", "get_system_info"),
    ("material", "get_material_properties", "get_material_info"),
    ("ui", "get_widget_tree", "list_widgets"),
    ("ui", "get_widget_tree", "get_widget_hierarchy"),
    ("animation", "get_montage_info", "list_montages"),
    ("animation", "get_graphs", "get_anim_graph"),
    ("audio", "list_audio_assets", "list_sound_cues"),
    ("scene", "get_level_actors", "list_actors"),
    ("source", "find_callees", "get_callee_tree"),
    ("source", "find_callers", "get_caller_tree"),
    ("source", "read_file", "read_flie"),
    ("blueprint", "get_dependencies", "get_class_hierarchy"),
    ("blueprint", "find_variable_references", "find_references"),
    ("blueprint", "get_components", "list_components"),
    ("blueprint", "get_execution_flow", "get_event_graph"),
    ("blueprint", "get_functions", "list_functions"),
    ("gas", "get_gameplay_effect", "get_gameplay_efefct"),
    ("gas", "find_abilities_by_tag", "find_abilities_by_tga"),
    ("niagara", "get_module_inputs", "get_module_inptus"),
    ("niagara", "list_renderers", "list_rendererz"),
    ("material", "get_expression_details", "get_expression_detials"),
    ("material", "check_tiling_quality", "check_tiling_quailty"),
    ("ui", "get_widget_bindings", "get_viewmodel_bindings"),
    ("ui", "configure_common_button", "configure_common_buton"),
    ("blueprint", "get_function_signature", "get_function_signatre"),
    ("blueprint", "search_nodes", "search_nodse"),
    ("animation", "get_state_machines", "get_state_machine"),
    ("animation", "get_montage_info", "get_montage_sections"),
    ("audio", "get_sound_cue_graph", "get_sound_cue"),
    ("audio", "validate_sound_cue", "validate_sound_ceu"),
    ("asset", "inspect_assets_batch", "inspect_assets_bacth"),
    ("scene", "get_actor_properties", "get_actor_propreties"),
    ("scene", "query_raycast", "query_raycats"),
    ("config", "resolve_setting", "get_config_value"),
    ("input", "get_input_mapping_context", "get_input_mapping_conetxt"),
    ("localization", "get_string_table", "get_localized_string"),
    ("source", "review_context", "review_conetxt"),
    ("cppreflect", "get_uclass", "get_uclas"),
    ("risk", "get_hotspot_score", "get_hotpsot_score"),
    ("project", "get_asset_details", "get_asset_detials"),
    ("mesh", "get_mesh_info", "get_mesh_ifno"),
]

_STATIC_LEGACY_RETIRED_ACTION_IDS_20260717 = frozenset(
    {
        "gas.get_ability",
        "niagara.get_system_info",
        "material.get_material_info",
        "ui.list_widgets",
        "ui.get_widget_hierarchy",
        "ui.get_viewmodel_bindings",
        "animation.list_montages",
        "animation.get_anim_graph",
        "animation.get_state_machine",
        "animation.get_montage_sections",
        "audio.list_sound_cues",
        "audio.get_sound_cue",
        "scene.list_actors",
        "config.get_config_value",
        "localization.get_localized_string",
    }
)
_STATIC_LEGACY_ACTION_MIGRATIONS_20260717 = [
    row
    for row in _STATIC_UNKNOWN_ACTION_TASKS_20260717
    if f"{row[0]}.{row[2]}".casefold() in _STATIC_LEGACY_RETIRED_ACTION_IDS_20260717
]

_STATIC_MISSING_PARAM_TASKS_20260717 = [
    ("gas", "get_ability_info", "asset_path"),
    ("gas", "get_gameplay_effect", "asset_path"),
    ("gas", "find_abilities_by_tag", "tag"),
    ("niagara", "get_ordered_modules", "asset_path"),
    ("niagara", "get_module_inputs", "asset_path"),
    ("material", "get_all_expressions", "asset_path"),
    ("material", "get_expression_details", "asset_path"),
    ("ui", "get_widget_tree", "asset_path"),
    ("ui", "get_animation_details", "asset_path"),
    ("blueprint", "list_graphs", "asset_path"),
    ("blueprint", "get_graph_data", "asset_path"),
    ("animation", "get_state_machines", "asset_path"),
    ("animation", "get_state_info", "asset_path"),
    ("audio", "get_sound_cue_graph", "asset_path"),
    ("audio", "search_audio_assets", "query"),
    ("asset", "inspect_asset", "asset_path"),
    ("scene", "get_actor_info", "actor_name"),
    ("scene", "get_actor_properties", "actor_name"),
    ("config", "resolve_setting", "key"),
    ("localization", "get_string_table", "asset_path"),
    ("source", "search_source", "query"),
    ("source", "review_context", "symbol"),
    ("source", "find_overrides", "symbol"),
    ("source", "impact_radius", "symbol"),
    ("material", "validate_material", "asset_path"),
    ("material", "get_material_properties", "asset_path"),
    ("paper2d", "get_asset", "asset_path"),
    ("mesh", "get_mesh_info", "asset_path"),
    ("project", "search", "query"),
    ("project", "get_asset_details", "asset_path"),
    ("collection", "get_collection", "name"),
]

_STATIC_INVALID_PARAM_TASKS_20260717 = [
    ("gas", "get_ability_info", {"asset_path": 12345}, "asset_path"),
    ("gas", "find_abilities_by_tag", {"tag": 12345}, "tag"),
    ("niagara", "get_ordered_modules", {"asset_path": 12345, "emitter": "BenchmarkValue"}, "asset_path"),
    (
        "niagara",
        "get_module_inputs",
        {"asset_path": 12345, "emitter": "BenchmarkValue", "module_node": "BenchmarkValue"},
        "asset_path",
    ),
    ("material", "get_all_expressions", {"asset_path": 12345}, "asset_path"),
    (
        "material",
        "get_expression_details",
        {"asset_path": 12345, "expression_name": "BenchmarkValue"},
        "asset_path",
    ),
    ("ui", "get_widget_tree", {"asset_path": 12345}, "asset_path"),
    (
        "ui",
        "list_widget_properties",
        {"asset_path": 12345, "widget_name": "BenchmarkValue"},
        "asset_path",
    ),
    ("blueprint", "list_graphs", {"asset_path": 12345}, "asset_path"),
    ("blueprint", "get_graph_data", {"asset_path": 12345}, "asset_path"),
    ("animation", "get_state_machines", {"asset_path": 12345}, "asset_path"),
    (
        "animation",
        "get_state_info",
        {"asset_path": 12345, "machine_name": "BenchmarkValue", "state_name": "BenchmarkValue"},
        "asset_path",
    ),
    ("audio", "get_sound_cue_graph", {"asset_path": 12345}, "asset_path"),
    ("audio", "search_audio_assets", {"query": 12345}, "query"),
    ("asset", "inspect_asset", {"asset_path": 12345}, "asset_path"),
    ("asset", "inspect_assets_batch", {"asset_paths": "not_an_array"}, "asset_paths"),
    ("scene", "list_layers", {"include_actors": "not_a_bool"}, "include_actors"),
    ("scene", "list_streaming_levels", {"limit": "not_a_number"}, "limit"),
    ("config", "search_config", {"query": 12345}, "query"),
    ("config", "resolve_setting", {"section": "BenchmarkSection", "key": 12345}, "key"),
    ("localization", "get_string_table", {"asset_path": 12345}, "asset_path"),
    ("input", "get_input_mapping_context", {"asset_path": 12345}, "asset_path"),
    ("source", "search_source", {"query": 12345}, "query"),
    ("mesh", "get_mesh_info", {"asset_path": 12345}, "asset_path"),
    ("project", "get_asset_details", {"asset_path": 12345}, "asset_path"),
]

_STATIC_ALIAS_PARAM_TASKS_20260617 = [
    ("source", "find_callers", {"query": "AActor::BeginPlay"}, "symbol"),
    ("source", "find_callees", {"query": "AActor::BeginPlay"}, "symbol"),
    ("source", "find_overrides", {"query": "UActorComponent::BeginPlay"}, "symbol"),
    ("source", "impact_radius", {"query": "UObject"}, "symbol"),
    ("source", "read_source", {"path": "Source/Monolith/Private/MonolithModule.cpp"}, "symbol"),
    ("material", "validate_material", {"path": "/Game/Benchmarks/M_Test"}, "asset_path"),
    ("material", "get_material_properties", {"material_path": "/Game/Benchmarks/M_Test"}, "asset_path"),
    ("paper2d", "get_asset", {"path": "/Game/Benchmarks/S_Test"}, "asset_path"),
    ("asset", "inspect_asset", {"path": "/Game/Benchmarks/BPB_TestActor"}, "asset_path"),
    ("project", "get_asset_details", {"path": "/Game/Benchmarks/BPB_TestActor"}, "asset_path"),
    (
        "blueprint",
        "add_variable",
        {"asset_path": "/Game/Benchmarks/BPB_TestActor", "variable_name": "Health", "variable_type": "int32"},
        "name",
    ),
    (
        "blueprint",
        "connect_pins",
        {"asset_path": "/Game/Benchmarks/BPB_TestActor", "source_node_id": "A", "target_node_id": "B"},
        "source_node",
    ),
    ("project", "search", {"path": "/Game/Benchmarks"}, "query"),
]

# ITEM 1: needed_action routing. The agent does not know the exact action name —
# it has a typo'd name, an invented/stale name, or only a vague task phrase. The
# routing tool (monolith_find) must name the REAL action_id as a candidate, or the
# monolith_discover error for a non-existent target must carry a useful hint. This
# is the discovery side of the #1 live pain (monolith.discover 5868 calls/516 err)
# that the happy-path discovery_planning tasks never probe. Each row is
# (subtype, tool, query_args, expected_action_id, expected_namespace, expected_action).
#
#   - "find" rows route through monolith_find(query=...); success requires the real
#     action_id to appear in matches[].action_id.
#   - "discover_unknown_action" rows call monolith_discover for an absent action;
#     a bare "Unknown action" error scores LOW — a routing hint naming the real
#     action (candidate_actions / did_you_mean / matches) scores high.
_STATIC_NEEDED_ACTION_ROUTING_TASKS_20260618 = [
    # Typo'd / mis-routed source lookups (source.get_signature mis-routes are a
    # documented live pain: get_signature 380 calls / 227 err).
    ("find", {"query": "get function signature for a C++ symbol"}, "source.get_signature", "source", "get_signature"),
    ("find", {"query": "get_signatuer"}, "source.get_signature", "source", "get_signature"),
    ("find", {"query": "who calls this function"}, "source.find_callers", "source", "find_callers"),
    ("find", {"query": "functions this symbol calls"}, "source.find_callees", "source", "find_callees"),
    ("find", {"query": "header include path for a class"}, "source.get_include_path", "source", "get_include_path"),
    ("find", {"query": "generate a c++ class stub", "namespace": "source"}, "source.generate_class_stub", "source", "generate_class_stub"),
    # Vague blueprint authoring intent -> the real high-volume write actions.
    ("find", {"query": "add a variable to a blueprint"}, "blueprint.add_variable", "blueprint", "add_variable"),
    ("find", {"query": "add a function to a blueprint"}, "blueprint.add_function", "blueprint", "add_function"),
    ("find", {"query": "create a new blueprint asset"}, "blueprint.create_blueprint", "blueprint", "create_blueprint"),
    ("find", {"query": "compile a blueprint"}, "blueprint.compile_blueprint", "blueprint", "compile_blueprint"),
    # Vague project / asset discovery.
    ("find", {"query": "search the project for assets"}, "project.search", "project", "search"),
    ("find", {"query": "details for an asset path", "namespace": "project"}, "project.get_asset_details", "project", "get_asset_details"),
    # Vague cross-namespace intent (no namespace hint at all).
    ("find", {"query": "list gameplay abilities"}, "gas.list_abilities", "gas", "list_abilities"),
    ("find", {"query": "risk hotspot score for a file"}, "risk.get_hotspot_score", "risk", "get_hotspot_score"),
    # Non-existent / invented action names routed through monolith_discover: the
    # error must point at the real action rather than dead-ending.
    ("discover_unknown_action", {"namespace": "source", "action": "get_function_signature"}, "source.get_signature", "source", "get_signature"),
    ("discover_unknown_action", {"namespace": "source", "action": "read_cpp_file"}, "source.read_file", "source", "read_file"),
    ("discover_unknown_action", {"namespace": "blueprint", "action": "find_references"}, "blueprint.find_variable_references", "blueprint", "find_variable_references"),
    ("discover_unknown_action", {"namespace": "project", "action": "get_asset_detials"}, "project.get_asset_details", "project", "get_asset_details"),
]


def static_unreal_practical_action_contracts() -> List[Dict[str, Any]]:
    """Return every live action/schema assumption embedded in static tasks.

    Generated catalog-driven rows cannot drift by construction, but the curated
    practical rows can outlive renamed or removed actions.  Keeping their
    assumptions in one normalized list lets generation fail closed before it
    overwrites the canonical corpus.
    """
    contracts: List[Dict[str, Any]] = []

    def add(source: str, namespace: str, action: str, **details: Any) -> None:
        contract = {"source": source, "namespace": namespace, "action": action}
        contract.update(details)
        contracts.append(contract)

    for namespace, action in _STATIC_DISCOVERY_TASKS_20260717:
        add("discovery", namespace, action)
    for namespace, action in _STATIC_READ_ONLY_POLICY_TASKS_20260711:
        add(
            "read_only_policy",
            namespace,
            action,
            expected_execution_policy_id="read_only",
            expected_execution_policy_defaulted=False,
            expected_mutates_assets=False,
        )
    for namespace, candidate_action, typo in _STATIC_UNKNOWN_ACTION_TASKS_20260717:
        add("unknown_action_candidate", namespace, candidate_action)
        add("unknown_action_probe", namespace, typo, expected_absent=True)
    for namespace, action, missing_param in _STATIC_MISSING_PARAM_TASKS_20260717:
        add("missing_param", namespace, action, required_param=missing_param)
    for namespace, action, params, invalid_param in _STATIC_INVALID_PARAM_TASKS_20260717:
        add(
            "invalid_param",
            namespace,
            action,
            invalid_param=invalid_param,
            provided_params=dict(params),
        )
    for namespace, action, params, missing_param in _STATIC_ALIAS_PARAM_TASKS_20260617:
        add(
            "alias_param",
            namespace,
            action,
            required_param=missing_param,
            provided_params=dict(params),
            expects_unknown_alias=True,
        )
    for subtype, query_args, expected_action_id, namespace, action in _STATIC_NEEDED_ACTION_ROUTING_TASKS_20260618:
        add(
            "needed_action_routing",
            namespace,
            action,
            expected_action_id=expected_action_id,
        )
        if subtype == "discover_unknown_action":
            add(
                "needed_action_unknown_probe",
                str(query_args.get("namespace", "")),
                str(query_args.get("action", "")),
                expected_absent=True,
            )
    for action_id in _ACTION_STATS_20260618:
        namespace, separator, action = action_id.partition(".")
        if separator:
            add("demand_weight", namespace, action, expected_action_id=action_id)

    return contracts


def _value_matches_declared_type(value: Any, type_spec: str) -> bool:
    """Return whether a static invalid-param fixture accidentally became valid."""
    for declared_type in type_names(type_spec):
        if declared_type == "string" and isinstance(value, str):
            return True
        if declared_type == "integer" and isinstance(value, int) and not isinstance(value, bool):
            return True
        if declared_type == "number" and isinstance(value, (int, float)) and not isinstance(value, bool):
            return True
        if declared_type in {"boolean", "bool"} and isinstance(value, bool):
            return True
        if declared_type == "array" and isinstance(value, list):
            return True
        if declared_type == "object" and isinstance(value, dict):
            return True
        if declared_type == "null" and value is None:
            return True
    return False


def validate_action_contracts(
    contracts: Iterable[Dict[str, Any]],
    namespaces: Iterable[Dict[str, Any]],
    schema_loader: Optional[Callable[[str, str], Optional[Dict[str, Any]]]] = None,
) -> None:
    """Fail closed when curated benchmark assumptions drift from a catalog.

    ``schema_loader`` is optional so offline corpus tests can still validate
    action identity against the bundled catalog.  Live generation supplies it
    and additionally verifies required/invalid parameter fixtures.
    """
    available: Dict[str, set[str]] = {}
    for row in namespaces:
        namespace = str(row.get("namespace", "")).strip()
        actions = {str(action).strip() for action in row.get("actions", []) if str(action).strip()}
        if namespace:
            available[namespace] = actions

    issues: set[str] = set()
    schema_cache: Dict[Tuple[str, str], Optional[Dict[str, Any]]] = {}
    for contract in contracts:
        source = str(contract.get("source", "static"))
        namespace = str(contract.get("namespace", "")).strip()
        action = str(contract.get("action", "")).strip()
        action_id = f"{namespace}.{action}"
        expected_action_id = str(contract.get("expected_action_id", "")).strip()
        if expected_action_id and expected_action_id != action_id:
            issues.add(
                f"{source}: expected_action_id={expected_action_id!r} does not match {action_id!r}"
            )
        if bool(contract.get("expected_absent")):
            if action in available.get(namespace, set()):
                issues.add(f"{source}: probe action {action_id!r} unexpectedly exists in the catalog")
            continue
        if action not in available.get(namespace, set()):
            issues.add(f"{source}: action {action_id!r} is absent from the catalog")
            continue

        policy_contract_fields = {
            "expected_execution_policy_id",
            "expected_execution_policy_defaulted",
            "expected_mutates_assets",
        }
        needs_schema = bool(
            contract.get("required_param")
            or contract.get("invalid_param")
            or policy_contract_fields.intersection(contract)
        )
        if not needs_schema or schema_loader is None:
            continue

        key = (namespace, action)
        if key not in schema_cache:
            try:
                schema_cache[key] = schema_loader(namespace, action)
            except Exception as exc:  # noqa: BLE001 - aggregate all drift evidence
                issues.add(f"{source}: schema load failed for {action_id!r}: {type(exc).__name__}: {exc}")
                schema_cache[key] = None
        schema = schema_cache[key]
        if not isinstance(schema, dict) or not schema:
            issues.add(f"{source}: schema for {action_id!r} is unavailable")
            continue

        params = schema_params(schema)
        required_params = {name for name, meta in params.items() if bool(meta.get("required"))}
        provided_params = contract.get("provided_params", {})
        if not isinstance(provided_params, dict):
            issues.add(f"{source}: provided_params for {action_id!r} is not an object")
            provided_params = {}

        required_param = str(contract.get("required_param", "")).strip()
        if required_param:
            if required_param not in required_params:
                issues.add(
                    f"{source}: {action_id!r} no longer requires {required_param!r}; "
                    f"live required params are {sorted(required_params)!r}"
                )
            if required_param in provided_params:
                issues.add(f"{source}: fixture unexpectedly supplies required param {required_param!r}")
            if contract.get("expects_unknown_alias"):
                unknown_aliases = sorted(set(provided_params) - set(params))
                if not unknown_aliases:
                    issues.add(f"{source}: {action_id!r} fixture no longer supplies an unknown alias")

        invalid_param = str(contract.get("invalid_param", "")).strip()
        if invalid_param:
            meta = params.get(invalid_param)
            if not isinstance(meta, dict):
                issues.add(f"{source}: invalid-param target {invalid_param!r} is absent from {action_id!r}")
                continue
            type_spec = str(meta.get("type", "")).strip()
            if not type_spec:
                issues.add(f"{source}: invalid-param target {action_id!r}.{invalid_param} has no declared type")
            if invalid_param not in provided_params:
                issues.add(f"{source}: fixture does not supply invalid-param target {invalid_param!r}")
            elif type_spec and _value_matches_declared_type(provided_params[invalid_param], type_spec):
                issues.add(
                    f"{source}: fixture value for {action_id!r}.{invalid_param} now matches type {type_spec!r}"
                )

            unknown_params = sorted(set(provided_params) - set(params))
            if unknown_params:
                issues.add(f"{source}: {action_id!r} fixture supplies unknown params {unknown_params!r}")
            omitted_required = sorted(required_params - set(provided_params))
            if omitted_required:
                issues.add(
                    f"{source}: {action_id!r} invalid-param fixture omits required params {omitted_required!r}"
                )

        if policy_contract_fields.intersection(contract):
            policy = schema.get("execution_policy")
            if not isinstance(policy, dict):
                issues.add(f"{source}: {action_id!r} has no execution_policy object")
                continue
            expected_policy_id = contract.get("expected_execution_policy_id")
            if expected_policy_id is not None and policy.get("policy_id") != expected_policy_id:
                issues.add(
                    f"{source}: {action_id!r} policy_id={policy.get('policy_id')!r}, "
                    f"expected {expected_policy_id!r}"
                )
            if "expected_execution_policy_defaulted" in contract:
                expected_defaulted = bool(contract["expected_execution_policy_defaulted"])
                if bool(policy.get("defaulted")) != expected_defaulted:
                    issues.add(
                        f"{source}: {action_id!r} execution_policy.defaulted={bool(policy.get('defaulted'))!r}, "
                        f"expected {expected_defaulted!r}"
                    )
            if "expected_mutates_assets" in contract:
                expected_mutates = bool(contract["expected_mutates_assets"])
                if bool(schema.get("mutates_assets")) != expected_mutates:
                    issues.add(
                        f"{source}: {action_id!r} mutates_assets={bool(schema.get('mutates_assets'))!r}, "
                        f"expected {expected_mutates!r}"
                    )

    if issues:
        details = "\n - ".join(sorted(issues))
        raise RuntimeError(
            "static ActionGuidance task contract drift; refusing to generate the canonical corpus:\n - "
            + details
        )


def validate_static_unreal_practical_action_contracts(
    namespaces: Iterable[Dict[str, Any]],
    schema_loader: Optional[Callable[[str, str], Optional[Dict[str, Any]]]] = None,
) -> None:
    validate_action_contracts(static_unreal_practical_action_contracts(), namespaces, schema_loader)


def append_static_unreal_practical_tasks(tasks: List[Dict[str, Any]]) -> None:
    seen = {task_fingerprint(task) for task in tasks}

    for namespace, action in _STATIC_DISCOVERY_TASKS_20260717:
        append_unique_task(tasks, seen, {
            "category": "discovery_planning",
            "namespace": namespace,
            "action": action,
            "tool": "monolith_discover",
            "arguments": {"namespace": namespace, "action": action, "mode": "schema"},
            "expected": {"action_id": f"{namespace}.{action}", "requires_planning_signals": True},
            "safety": "read_only_discovery",
        })

    for namespace, action in _STATIC_READ_ONLY_POLICY_TASKS_20260711:
        append_unique_task(tasks, seen, {
            "category": "discovery_planning",
            "namespace": namespace,
            "action": action,
            "tool": "monolith_discover",
            "arguments": {"namespace": namespace, "action": action, "mode": "schema"},
            "expected": {
                "action_id": f"{namespace}.{action}",
                "requires_planning_signals": True,
                "execution_policy_id": "read_only",
                "execution_policy_defaulted": False,
                "mutates_assets": False,
            },
            "safety": "read_only_discovery",
        })

    for namespace, candidate_action, typo in _STATIC_UNKNOWN_ACTION_TASKS_20260717:
        tool, args = action_tool(namespace, typo, {})
        append_unique_task(tasks, seen, {
            "category": "unknown_action_recovery",
            "namespace": namespace,
            "action": typo,
            "tool": tool,
            "arguments": args,
            "expected": {"candidate_action": f"{namespace}.{candidate_action}", "failure_cause": "unknown_action"},
            "safety": "lookup_failure_before_handler",
        })

    for namespace, action, missing_param in _STATIC_MISSING_PARAM_TASKS_20260717:
        tool, args = action_tool(namespace, action, {})
        append_unique_task(tasks, seen, {
            "category": "missing_required_param",
            "namespace": namespace,
            "action": action,
            "tool": tool,
            "arguments": args,
            "expected": {"missing_param": missing_param, "failure_cause": "missing_required_param"},
            "safety": "schema_failure_before_handler",
        })

    for namespace, action, params, missing_param in _STATIC_ALIAS_PARAM_TASKS_20260617:
        tool, args = action_tool(namespace, action, dict(params))
        append_unique_task(tasks, seen, {
            "category": "missing_required_param",
            "namespace": namespace,
            "action": action,
            "tool": tool,
            "arguments": args,
            "expected": {"missing_param": missing_param, "failure_cause": "missing_required_param"},
            "safety": "schema_failure_before_handler",
        })

    for namespace, action, params, invalid_param in _STATIC_INVALID_PARAM_TASKS_20260717:
        tool, args = action_tool(namespace, action, dict(params))
        append_unique_task(tasks, seen, {
            "category": "invalid_param_type",
            "namespace": namespace,
            "action": action,
            "tool": tool,
            "arguments": args,
            "expected": {"invalid_param": invalid_param, "failure_cause": "invalid_param"},
            "safety": "schema_failure_before_handler",
        })

    for subtype, query_args, expected_action_id, exp_ns, exp_action in _STATIC_NEEDED_ACTION_ROUTING_TASKS_20260618:
        if subtype == "find":
            tool = "monolith_find"
            args = dict(query_args)
            safety = "read_only_routing"
        elif subtype == "discover_unknown_action":
            tool = "monolith_discover"
            args = dict(query_args)
            safety = "lookup_failure_before_handler"
        else:
            raise RuntimeError(f"unknown needed_action_routing subtype: {subtype}")
        # namespace/action describe the REAL action the agent needs so demand
        # weighting reflects the routed-to action (e.g. source.get_signature).
        append_unique_task(tasks, seen, {
            "category": "needed_action_routing",
            "namespace": exp_ns,
            "action": exp_action,
            "tool": tool,
            "arguments": args,
            "expected": {
                "candidate_action": expected_action_id,
                "routing_subtype": subtype,
                "failure_cause": "needed_action",
            },
            "safety": safety,
        })


def generate_tasks(url: str, min_tasks: int, tasks_path: pathlib.Path, manifest_path: pathlib.Path) -> Dict[str, Any]:
    tasks_path = resolve_plugin_path(tasks_path)
    manifest_path = resolve_plugin_path(manifest_path)
    start_catalog = read_generation_catalog_fingerprint(url)
    namespaces = discover_catalog_namespaces(
        url,
        expected_catalog_version=str(start_catalog["catalog_version"]),
    )
    namespace_rows = []
    tasks: List[Dict[str, Any]] = []
    schema_cache: Dict[Tuple[str, str], Dict[str, Any]] = {}
    for ns_row in namespaces:
        namespace = str(ns_row.get("namespace", ""))
        inline_schemas = ns_row.get("schemas")
        if not namespace or not isinstance(inline_schemas, dict):
            continue
        for action, schema in inline_schemas.items():
            if isinstance(schema, dict):
                schema_cache[(namespace, str(action))] = schema

    def cached_schema(namespace: str, action: str) -> Optional[Dict[str, Any]]:
        key = (namespace, action)
        if key not in schema_cache:
            schema_cache[key] = discover_schema(url, namespace, action) or {}
        return schema_cache[key] or None

    validate_static_unreal_practical_action_contracts(namespaces, cached_schema)

    def next_id() -> str:
        return f"AGB-{len(tasks) + 1:03d}"

    for ns_row in namespaces:
        namespace = str(ns_row.get("namespace", ""))
        actions = [str(a) for a in ns_row.get("actions", []) if str(a)]
        if not namespace or not actions:
            continue
        namespace_rows.append({"namespace": namespace, "action_count": len(actions)})
        rep = representative_actions(actions, 1)[0]
        tasks.append({
            "id": next_id(),
            "category": "discovery_planning",
            "namespace": namespace,
            "action": rep,
            "tool": "monolith_discover",
            "arguments": {"namespace": namespace, "action": rep, "mode": "schema"},
            "expected": {"action_id": f"{namespace}.{rep}", "requires_planning_signals": True},
            "safety": "read_only_discovery",
        })
        typo = typo_action(rep, actions)
        tool, args = action_tool(namespace, typo, {})
        tasks.append({
            "id": next_id(),
            "category": "unknown_action_recovery",
            "namespace": namespace,
            "action": typo,
            "tool": tool,
            "arguments": args,
            "expected": {"candidate_action": f"{namespace}.{rep}", "failure_cause": "unknown_action"},
            "safety": "lookup_failure_before_handler",
        })

    # Add schema-driven missing-param and invalid-param tasks from read-only actions.
    for ns_row in namespaces:
        namespace = str(ns_row.get("namespace", ""))
        actions = [str(a) for a in ns_row.get("actions", []) if str(a)]
        if not namespace or not actions:
            continue
        for action in representative_actions(actions, 4):
            key = (namespace, action)
            schema = schema_cache.get(key)
            if schema is None:
                schema = discover_schema(url, namespace, action) or {}
                schema_cache[key] = schema
            if not schema or not is_read_only(schema):
                continue
            params = schema_params(schema)
            required = [name for name, meta in params.items() if bool(meta.get("required"))]
            if required:
                tool, args = action_tool(namespace, action, {})
                tasks.append({
                    "id": next_id(),
                    "category": "missing_required_param",
                    "namespace": namespace,
                    "action": action,
                    "tool": tool,
                    "arguments": args,
                    "expected": {"missing_param": required[0], "failure_cause": "missing_required_param"},
                    "safety": "schema_failure_before_handler",
                })
            invalid_target = None
            for name, meta in params.items():
                if "type" in meta:
                    invalid_target = name
                    break
            if invalid_target:
                call_params: Dict[str, Any] = {}
                for name, meta in params.items():
                    if bool(meta.get("required")):
                        call_params[name] = dummy_value_for_type(str(meta.get("type", "")))
                call_params[invalid_target] = wrong_value_for_type(str(params[invalid_target].get("type", "")))
                tool, args = action_tool(namespace, action, call_params)
                tasks.append({
                    "id": next_id(),
                    "category": "invalid_param_type",
                    "namespace": namespace,
                    "action": action,
                    "tool": tool,
                    "arguments": args,
                    "expected": {"invalid_param": invalid_target, "failure_cause": "invalid_param"},
                    "safety": "schema_failure_before_handler",
                })
            if len(tasks) >= min_tasks + 40:
                break
        if len(tasks) >= min_tasks + 40:
            break

    # If the catalog is sparse in schema-rich read-only actions, top up with more discovery tasks.
    if len(tasks) < min_tasks:
        for ns_row in namespaces:
            namespace = str(ns_row.get("namespace", ""))
            actions = [str(a) for a in ns_row.get("actions", []) if str(a)]
            for action in representative_actions(actions, 8):
                if any(t["category"] == "discovery_planning" and t["namespace"] == namespace and t["action"] == action for t in tasks):
                    continue
                tasks.append({
                    "id": next_id(),
                    "category": "discovery_planning",
                    "namespace": namespace,
                    "action": action,
                    "tool": "monolith_discover",
                    "arguments": {"namespace": namespace, "action": action, "mode": "schema"},
                    "expected": {"action_id": f"{namespace}.{action}", "requires_planning_signals": True},
                    "safety": "read_only_discovery",
                })
                if len(tasks) >= min_tasks:
                    break
            if len(tasks) >= min_tasks:
                break

    append_static_unreal_practical_tasks(tasks)

    # ITEM 2: stamp a live-demand weight on every task so the aggregate tracks
    # invocation volume x error cost rather than uniform per-namespace sampling.
    weighted_task_count = 0
    for task in tasks:
        weight = action_weight(str(task.get("namespace", "")), str(task.get("action", "")))
        task["weight"] = weight
        if weight > DEFAULT_WEIGHT:
            weighted_task_count += 1

    end_catalog = read_generation_catalog_fingerprint(url)
    if end_catalog != start_catalog:
        raise RuntimeError(
            "catalog drifted while generating ActionGuidance corpus; refusing to overwrite "
            f"tasks/manifest (start={start_catalog}, end={end_catalog})"
        )

    manifest = {
        "generated_at": utc_now(),
        "mcp_url": url,
        "task_count": len(tasks),
        "min_tasks_requested": min_tasks,
        "catalog_namespace_count": len(namespace_rows),
        "catalog_action_count": sum(row["action_count"] for row in namespace_rows),
        "catalog_version": start_catalog["catalog_version"],
        "namespace_coverage": namespace_rows,
        "category_counts": count_by(tasks, "category"),
        "task_file": display_path(tasks_path),
        "demand_weighting": {
            "source": _DEMAND_WEIGHT_SOURCE,
            "formula": "weight = 1.0 + log10(1 + count * error_rate) for documented high-volume/high-error actions; else 1.0",
            "weighted_action_count": len(_ACTION_STATS_20260618),
            "weighted_task_count": weighted_task_count,
        },
        "scoring": {
            "effectiveness_score": "0.30*task_success_rate + 0.20*first_recovery_success_rate + 0.15*action_selection_accuracy + 0.15*param_correction_accuracy + 0.10*(1-normalized_tool_calls) + 0.10*(1-hallucinated_workflow_rate)",
            "normalized_tool_calls": "clamp((mean_tool_calls_to_success - 1) / max(1, max_recovery_calls - 1), 0, 1)",
            "aggregation": "Per-task sub-metrics are combined with weighted_avg using each task's demand weight; component weights still sum to 1.0.",
        },
        "run_gates": {
            "default_max_recovery_calls": DEFAULT_MAX_RECOVERY_CALLS,
            "max_transport_failed_fraction": DEFAULT_MAX_TRANSPORT_FAILED_FRACTION,
            "max_consecutive_transport_failures": DEFAULT_MAX_CONSECUTIVE_TRANSPORT_FAILURES,
            "min_transport_fraction_sample": DEFAULT_MIN_TRANSPORT_FRACTION_SAMPLES,
            "status_transport_failure_aborts_before_tasks": True,
            "invalid_status_response_aborts_before_tasks": True,
            "task_protocol_error_aborts_immediately": True,
            "runner_exception_aborts_immediately": True,
            "short_run_fraction_checked_at_finalize": True,
            "canonical_task_corpus_required_for_comparison": True,
            "status_identity_must_match_postflight": True,
            "invalid_run_writes_summary": False,
        },
    }
    write_jsonl(tasks_path, tasks)
    write_json(manifest_path, manifest)
    return manifest


def count_by(rows: Iterable[Dict[str, Any]], field: str) -> Dict[str, int]:
    out: Dict[str, int] = {}
    for row in rows:
        key = str(row.get(field, ""))
        out[key] = out.get(key, 0) + 1
    return dict(sorted(out.items()))


def nested_error_data(payload: Dict[str, Any]) -> Dict[str, Any]:
    error_data = payload.get("error_data")
    if isinstance(error_data, dict):
        return error_data
    nested = structured_content(payload).get("error_data")
    if isinstance(nested, dict):
        return nested
    return {}


def field_or_nested(payload: Dict[str, Any], field: str) -> Any:
    if field in payload:
        return payload[field]
    structured = structured_content(payload)
    if field in structured:
        return structured[field]
    nested = nested_error_data(payload)
    return nested.get(field)


def array_field(payload: Dict[str, Any], field: str) -> List[Any]:
    value = field_or_nested(payload, field)
    return value if isinstance(value, list) else []


def call_discover_actions(
    url: str,
    namespace: str,
    timeout_s: float = 45.0,
    call_fn: Optional[Callable[[str, str, Dict[str, Any], float], Dict[str, Any]]] = None,
) -> List[str]:
    """Return every action name of a namespace via paginated mode="actions".

    The server pages namespace listings (default 50 rows); a single unpaged
    call hid everything past page 1 for namespaces like ai (182 actions) and
    silently failed the error-recovery scorer for any expected action beyond
    the first page. Pagination drift is reported loudly but scored as a failed
    recovery ([]), not a crashed run.
    """
    caller = call_fn or mcp_call

    def fetch_page(arguments: Dict[str, Any]) -> Dict[str, Any]:
        response = caller(url, "monolith_discover", arguments, timeout_s)
        return result_data(response) or {}

    try:
        return paginate_discover_action_names(fetch_page, namespace)
    except RuntimeError as exc:
        print(f"[action_guidance] WARNING: discover action enumeration failed: {exc}", file=sys.stderr)
        return []


def candidate_contains(payload: Dict[str, Any], expected_action_id: str) -> bool:
    candidates = array_field(payload, "candidate_actions")
    for candidate in candidates:
        if isinstance(candidate, dict) and candidate.get("action_id") == expected_action_id:
            return True
        if isinstance(candidate, str) and candidate == expected_action_id:
            return True
    return False


def _candidate_action_id(candidate: Any) -> Optional[str]:
    if isinstance(candidate, str):
        return candidate
    if isinstance(candidate, dict):
        for key in ("action_id", "action", "id"):
            value = candidate.get(key)
            if isinstance(value, str) and value:
                return value
    return None


def routing_candidates(payload: Dict[str, Any], parsed: Dict[str, Any]) -> List[str]:
    """Collect every action_id a routing response surfaces.

    Covers both the monolith_find shape (top-level ``matches[].action_id``) and any
    structured discover routing hint (``candidate_actions`` / ``did_you_mean`` /
    ``suggestions``) on the error/result payload. A bare ``Unknown action`` error
    with none of these fields yields an empty list and therefore scores LOW.
    """
    out: List[str] = []
    sources: List[Any] = []
    for container in (parsed, payload):
        if not isinstance(container, dict):
            continue
        for field in ("matches", "candidate_actions", "did_you_mean", "suggestions"):
            value = container.get(field)
            if isinstance(value, list):
                sources.extend(value)
        # candidate fields may also live in nested error_data on the payload.
    for field in ("candidate_actions", "did_you_mean", "suggestions"):
        sources.extend(array_field(payload, field))
    for candidate in sources:
        action_id = _candidate_action_id(candidate)
        if action_id and action_id not in out:
            out.append(action_id)
    return out


def routing_names_action(payload: Dict[str, Any], parsed: Dict[str, Any], expected_action_id: str, top_n: int = 5) -> bool:
    candidates = routing_candidates(payload, parsed)
    return expected_action_id in candidates[:top_n]


class TaskMcpProtocolError(RuntimeError):
    """A malformed task-call envelope; the run must stop at this task."""

    def __init__(self, tool: str, raw: str):
        super().__init__(f"{tool} returned an invalid MCP/JSON-RPC response")
        self.tool = tool
        self.raw = raw[:500]


def _score_task_impl(url: str, task: Dict[str, Any], max_recovery_calls: int, timeout_s: float) -> Dict[str, Any]:
    transport_events: List[Dict[str, Any]] = []

    def task_mcp_call(
        call_url: str,
        tool: str,
        arguments: Dict[str, Any],
        call_timeout_s: float,
    ) -> Dict[str, Any]:
        call_response = mcp_call(
            call_url,
            tool,
            arguments,
            timeout_s=call_timeout_s,
        )
        if not isinstance(call_response, dict):
            call_response = {
                "protocol_error": True,
                "raw": str(call_response)[:500],
                "error": "MCP response top-level JSON must be an object",
            }
        if call_response.get("transport_error"):
            status = call_response.get("status")
            transport_events.append({
                "tool": tool,
                "status": (
                    status if isinstance(status, int) and not isinstance(status, bool) else None
                ),
                "raw": str(call_response.get("raw", ""))[:300],
            })
        protocol_failure = classify_mcp_protocol_failure(call_response)
        if protocol_failure:
            raise TaskMcpProtocolError(
                tool,
                str(call_response.get("raw", call_response)),
            )
        return call_response

    response = task_mcp_call(
        url,
        str(task["tool"]),
        dict(task.get("arguments", {})),
        timeout_s,
    )
    payload = result_payload(response)
    parsed_text = result_data(response)
    category = task.get("category")
    expected = task.get("expected", {})

    direct_success = False
    recovered = False
    tool_calls = 1
    action_selection_score: Optional[float] = None
    param_correction_score: Optional[float] = None
    hallucinated_workflow_risk: Optional[float] = None
    evidence: Dict[str, Any] = {}

    if category == "discovery_planning":
        schema = parsed_text.get("schema") if isinstance(parsed_text, dict) else None
        if isinstance(schema, dict):
            action_selection_score = 1.0 if schema.get("action") == task.get("action") else 0.0
            signals = schema.get("planning_signals")
            has_signals = isinstance(signals, list) and len(signals) > 0
            has_skill = isinstance(schema.get("skill"), str) and bool(schema.get("skill"))
            output_status = schema.get("output_contract_status")
            next_status = schema.get("next_actions_status")
            status_explicit = output_status in ("declared", "not_declared") and next_status in ("declared", "not_declared")
            expected_policy_id = expected.get("execution_policy_id")
            expected_policy_defaulted = expected.get("execution_policy_defaulted")
            expected_mutates_assets = expected.get("mutates_assets")
            policy = schema.get("execution_policy")
            policy_ok = True
            if expected_policy_id is not None:
                policy_ok = isinstance(policy, dict) and policy.get("policy_id") == expected_policy_id
                policy_ok = policy_ok and not bool(
                    policy.get("dirty_package_tracking")
                    or policy.get("transaction_wrapping")
                    or policy.get("post_edit_validation")
                )
            if expected_policy_defaulted is not None:
                policy_ok = policy_ok and isinstance(policy, dict) and bool(policy.get("defaulted")) == bool(expected_policy_defaulted)
            if expected_mutates_assets is not None:
                policy_ok = policy_ok and bool(schema.get("mutates_assets")) == bool(expected_mutates_assets)
            direct_success = bool(has_signals and has_skill and status_explicit and policy_ok)
            recovered = direct_success
            hallucinated_workflow_risk = 0.0 if status_explicit and policy_ok else 1.0
            evidence = {
                "has_planning_signals": has_signals,
                "has_skill": has_skill,
                "output_contract_status": output_status,
                "next_actions_status": next_status,
                "execution_policy": policy,
                "mutates_assets": schema.get("mutates_assets"),
                "policy_ok": policy_ok,
            }
        else:
            action_selection_score = 0.0
            hallucinated_workflow_risk = 1.0
    elif category == "unknown_action_recovery":
        expected_candidate = str(expected.get("candidate_action", ""))
        direct_candidate = candidate_contains(payload, expected_candidate)
        failure_cause = field_or_nested(payload, "failure_cause")
        retryability = field_or_nested(payload, "retryability")
        direct_success = bool(direct_candidate and failure_cause == "unknown_action" and retryability)
        action_selection_score = 1.0 if direct_candidate else 0.0
        recovered = direct_success
        if not recovered and tool_calls < max_recovery_calls:
            actions = call_discover_actions(
                url,
                str(task.get("namespace", "")),
                timeout_s=timeout_s,
                call_fn=task_mcp_call,
            )
            tool_calls += 1
            expected_action = expected_candidate.split(".", 1)[1] if "." in expected_candidate else expected_candidate
            best = difflib.get_close_matches(str(task.get("action", "")), actions, n=1)
            recovered = bool(best and best[0] == expected_action)
            if recovered and action_selection_score == 0.0:
                action_selection_score = 0.5
        evidence = {
            "direct_candidate": direct_candidate,
            "failure_cause": failure_cause,
            "retryability": retryability,
        }
    elif category == "needed_action_routing":
        # The agent has an absent/typo'd/vague action name. The routing response
        # (monolith_find matches, or a structured monolith_discover hint) must name
        # the REAL action_id. A bare no-candidate response scores LOW.
        expected_candidate = str(expected.get("candidate_action", ""))
        candidates = routing_candidates(payload, parsed_text if isinstance(parsed_text, dict) else {})
        direct_named = expected_candidate in candidates[:5]
        direct_success = bool(direct_named)
        action_selection_score = 1.0 if direct_named else 0.0
        recovered = direct_success
        if not recovered and tool_calls < max_recovery_calls:
            # Deterministic fallback: re-query monolith_find scoped to the namespace.
            retry = task_mcp_call(
                url,
                "monolith_find",
                {"query": str(task.get("action", "")).replace("_", " "), "namespace": str(task.get("namespace", ""))},
                timeout_s,
            )
            tool_calls += 1
            retry_candidates = routing_candidates(result_payload(retry), result_data(retry))
            recovered = expected_candidate in retry_candidates[:5]
            if recovered and action_selection_score == 0.0:
                action_selection_score = 0.5
        evidence = {
            "routing_subtype": expected.get("routing_subtype"),
            "expected_action_id": expected_candidate,
            "direct_named": direct_named,
            "candidate_count": len(candidates),
            "top_candidates": candidates[:5],
        }
    elif category == "missing_required_param":
        missing = expected.get("missing_param")
        missing_direct = missing in array_field(payload, "missing_required_params")
        required_params = array_field(payload, "required_params")
        required_direct = any(isinstance(row, dict) and row.get("name") == missing for row in required_params)
        failure_cause = field_or_nested(payload, "failure_cause")
        direct_success = bool((missing_direct or required_direct) and failure_cause in ("missing_required_param", "schema_validation"))
        param_correction_score = 1.0 if direct_success else 0.0
        recovered = direct_success
        if not recovered and tool_calls < max_recovery_calls:
            schema = discover_schema(
                url,
                str(task.get("namespace", "")),
                str(task.get("action", "")),
                timeout_s=timeout_s,
                call_fn=task_mcp_call,
            )
            tool_calls += 1
            params = schema_params(schema or {})
            recovered = bool(missing in params and params[missing].get("required"))
            if recovered:
                param_correction_score = 0.5
        evidence = {
            "missing_direct": missing_direct,
            "required_direct": required_direct,
            "failure_cause": failure_cause,
        }
    elif category == "invalid_param_type":
        invalid_param = expected.get("invalid_param")
        validation_errors = array_field(payload, "validation_errors")
        failure_cause = field_or_nested(payload, "failure_cause")
        direct_validation = any(invalid_param and invalid_param in str(err) for err in validation_errors)
        direct_success = bool(direct_validation and failure_cause in ("invalid_param", "schema_validation"))
        param_correction_score = 1.0 if direct_success else 0.0
        recovered = direct_success
        if not recovered and tool_calls < max_recovery_calls:
            schema = discover_schema(
                url,
                str(task.get("namespace", "")),
                str(task.get("action", "")),
                timeout_s=timeout_s,
                call_fn=task_mcp_call,
            )
            tool_calls += 1
            params = schema_params(schema or {})
            recovered = bool(invalid_param in params and params[invalid_param].get("type"))
            if recovered:
                param_correction_score = 0.5
        evidence = {
            "direct_validation": direct_validation,
            "failure_cause": failure_cause,
            "validation_errors": validation_errors[:3],
        }
    else:
        evidence = {"unsupported_category": category}

    if not recovered:
        tool_calls = max_recovery_calls

    last_transport = transport_events[-1] if transport_events else {}
    return {
        "task_id": task.get("id"),
        "category": category,
        "namespace": task.get("namespace"),
        "action": task.get("action"),
        "weight": task_weight(task),
        "direct_success": direct_success,
        "task_success": recovered,
        "tool_calls_to_success": tool_calls,
        "action_selection_score": action_selection_score,
        "param_correction_score": param_correction_score,
        "hallucinated_workflow_risk": hallucinated_workflow_risk,
        "evidence": evidence,
        "transport_error": bool(transport_events),
        "transport_status": last_transport.get("status"),
        "transport_error_raw": str(last_transport.get("raw", "")),
        "transport_failure_call_count": len(transport_events),
        "last_transport_tool": str(last_transport.get("tool", "")),
        "response_is_error": bool(payload.get("isError")),
        "response_text": result_text(response)[:1000],
    }


def protocol_failure_task_row(
    task: Dict[str, Any],
    max_recovery_calls: int,
    failure: TaskMcpProtocolError,
) -> Dict[str, Any]:
    """Preserve the exact task and call that exposed a malformed MCP envelope."""
    return {
        "task_id": task.get("id"),
        "category": task.get("category"),
        "namespace": task.get("namespace"),
        "action": task.get("action"),
        "weight": task_weight(task),
        "direct_success": False,
        "task_success": False,
        "tool_calls_to_success": max_recovery_calls,
        "action_selection_score": 0.0,
        "param_correction_score": None,
        "hallucinated_workflow_risk": 1.0,
        "evidence": {"protocol_error": str(failure)},
        "transport_error": False,
        "transport_status": None,
        "transport_error_raw": "",
        "transport_failure_call_count": 0,
        "last_transport_tool": "",
        "protocol_error": True,
        "protocol_error_raw": failure.raw,
        "protocol_failure_call_count": 1,
        "last_protocol_tool": failure.tool,
        "response_is_error": False,
        "response_text": "",
        "failure_kind": "protocol_error",
        "error": str(failure),
    }


def score_task(url: str, task: Dict[str, Any], max_recovery_calls: int, timeout_s: float) -> Dict[str, Any]:
    try:
        return _score_task_impl(url, task, max_recovery_calls, timeout_s)
    except TaskMcpProtocolError as failure:
        return protocol_failure_task_row(task, max_recovery_calls, failure)


def avg(values: List[float]) -> float:
    return sum(values) / len(values) if values else 0.0


def _row_weight(row: Dict[str, Any]) -> float:
    weight = row.get("weight")
    if isinstance(weight, (int, float)) and weight > 0:
        return float(weight)
    return DEFAULT_WEIGHT


def aggregate(label: str, status: Dict[str, Any], tasks: List[Dict[str, Any]], rows: List[Dict[str, Any]], max_recovery_calls: int) -> Dict[str, Any]:
    total = len(rows)
    # needed_action_routing is a needed-action recovery (failure_rows) and a routing
    # selection (action_rows). It contributes to first_recovery and action_selection.
    failure_rows = [r for r in rows if r["category"] in ("unknown_action_recovery", "needed_action_routing", "missing_required_param", "invalid_param_type")]
    action_rows = [r for r in rows if r["category"] in ("unknown_action_recovery", "needed_action_routing", "discovery_planning")]
    param_rows = [r for r in rows if r["category"] in ("missing_required_param", "invalid_param_type")]
    workflow_rows = [r for r in rows if r["hallucinated_workflow_risk"] is not None]

    # ITEM 2: every sub-metric is a demand-weighted mean so high-volume/high-error
    # actions move the score far more than dead low-volume actions.
    task_success_rate = weighted_avg([(1.0 if r["task_success"] else 0.0, _row_weight(r)) for r in rows])
    first_recovery_success_rate = weighted_avg([(1.0 if r["direct_success"] else 0.0, _row_weight(r)) for r in failure_rows])
    action_selection_accuracy = weighted_avg([(float(r["action_selection_score"]), _row_weight(r)) for r in action_rows if r["action_selection_score"] is not None])
    param_correction_accuracy = weighted_avg([(float(r["param_correction_score"]), _row_weight(r)) for r in param_rows if r["param_correction_score"] is not None])
    mean_calls = weighted_avg([(float(r["tool_calls_to_success"]), _row_weight(r)) for r in rows])
    normalized_tool_calls = max(0.0, min(1.0, (mean_calls - 1.0) / max(1.0, float(max_recovery_calls - 1))))
    invalid_retry_rate = weighted_avg([(0.0 if r["direct_success"] else 1.0, _row_weight(r)) for r in failure_rows])
    hallucinated_workflow_rate = weighted_avg([(float(r["hallucinated_workflow_risk"]), _row_weight(r)) for r in workflow_rows])
    effectiveness_score = (
        0.30 * task_success_rate
        + 0.20 * first_recovery_success_rate
        + 0.15 * action_selection_accuracy
        + 0.15 * param_correction_accuracy
        + 0.10 * (1.0 - normalized_tool_calls)
        + 0.10 * (1.0 - hallucinated_workflow_rate)
    )

    namespace_count = len({str(t.get("namespace")) for t in tasks})
    total_weight = sum(_row_weight(r) for r in rows)
    return {
        "label": label,
        "created_at": utc_now(),
        "mcp_status": status,
        "task_count": total,
        "weighted_task_mass": round(total_weight, 6),
        "namespace_count_in_tasks": namespace_count,
        "category_counts": count_by(tasks, "category"),
        "metrics": {
            "effectiveness_score": round(effectiveness_score, 6),
            "task_success_rate": round(task_success_rate, 6),
            "first_recovery_success_rate": round(first_recovery_success_rate, 6),
            "action_selection_accuracy": round(action_selection_accuracy, 6),
            "param_correction_accuracy": round(param_correction_accuracy, 6),
            "mean_tool_calls_to_success": round(mean_calls, 6),
            "normalized_tool_calls": round(normalized_tool_calls, 6),
            "invalid_retry_rate": round(invalid_retry_rate, 6),
            "hallucinated_workflow_rate": round(hallucinated_workflow_rate, 6),
        },
    }


def runner_exception_task_row(
    task: Dict[str, Any],
    max_recovery_calls: int,
    error: str,
) -> Dict[str, Any]:
    """Preserve the triggering task without invoking benchmark scoring code."""
    return {
        "task_id": task.get("id"),
        "category": task.get("category"),
        "namespace": task.get("namespace"),
        "action": task.get("action"),
        "weight": task_weight(task),
        "direct_success": False,
        "task_success": False,
        "tool_calls_to_success": max_recovery_calls,
        "action_selection_score": 0.0,
        "param_correction_score": None,
        "hallucinated_workflow_risk": 1.0,
        "evidence": {"runner_exception": error},
        "transport_error": False,
        "transport_status": None,
        "transport_error_raw": "",
        "transport_failure_call_count": 0,
        "last_transport_tool": "",
        "response_is_error": False,
        "response_text": "",
        "failure_kind": "runner_exception",
        "error": error,
    }


RUN_OUTPUT_FILENAMES = (
    "summary.json",
    "partial_summary.json",
    "per_task.json",
    "per_task.jsonl",
    "run_failure.json",
)


def clear_known_run_outputs(output_dir: pathlib.Path) -> None:
    """Invalidate every stale success/failure artifact before any input is trusted."""
    output_dir.mkdir(parents=True, exist_ok=True)
    for name in RUN_OUTPUT_FILENAMES:
        path = output_dir / name
        if path.exists():
            path.unlink()


def write_invalid_run_artifacts(output_dir: pathlib.Path, failure: Dict[str, Any]) -> None:
    failure["run_valid"] = False
    failure["metrics_valid"] = False
    write_json(output_dir / "run_failure.json", failure)
    write_json(output_dir / "partial_summary.json", failure)


def attach_run_context(
    payload: Dict[str, Any],
    corpus: TaskCorpus,
    start_identity: Dict[str, str],
    end_identity: Optional[Dict[str, str]] = None,
) -> Dict[str, Any]:
    payload["task_corpus"] = task_corpus_metadata(corpus)
    payload["comparison_valid"] = corpus.comparable
    payload["status_identity_start"] = start_identity
    if end_identity is not None:
        payload["status_identity_end"] = end_identity
    return payload


def build_attempt_failure(
    label: str,
    status: Dict[str, Any],
    tasks: List[Dict[str, Any]],
    rows: List[Dict[str, Any]],
    max_recovery_calls: int,
    tracker: TransportFailureTracker,
    benchmark_inputs: Dict[str, Any],
    corpus: TaskCorpus,
    start_identity: Dict[str, str],
    fields: Dict[str, Any],
) -> Dict[str, Any]:
    try:
        failure = aggregate(label, status, tasks[:len(rows)], rows, max_recovery_calls)
    except Exception as exc:  # noqa: BLE001 - preserve the invalid run even if aggregate code fails.
        failure = {
            "label": label,
            "created_at": utc_now(),
            "task_count": len(rows),
            "aggregate_error": f"{type(exc).__name__}: {exc}",
        }
    failure.update(fields)
    failure.update(tracker.snapshot())
    attach_benchmark_inputs(failure, benchmark_inputs)
    attach_run_context(failure, corpus, start_identity)
    return failure


def run_benchmark(
    url: str,
    tasks_path: pathlib.Path,
    output_dir: pathlib.Path,
    label: str,
    max_recovery_calls: int,
    timeout_s: float,
    max_transport_failed_fraction: float = DEFAULT_MAX_TRANSPORT_FAILED_FRACTION,
    max_consecutive_transport_failures: int = DEFAULT_MAX_CONSECUTIVE_TRANSPORT_FAILURES,
    min_transport_fraction_sample: int = DEFAULT_MIN_TRANSPORT_FRACTION_SAMPLES,
    allow_subset: bool = False,
) -> Dict[str, Any]:
    clear_known_run_outputs(output_dir)
    try:
        corpus = load_task_corpus(
            tasks_path,
            suite="ActionGuidance",
            canonical_tasks_path=DEFAULT_TASKS,
            canonical_manifest_path=DEFAULT_MANIFEST,
            allow_subset=allow_subset,
            allowed_categories=ACTION_GUIDANCE_TASK_CATEGORIES,
            require_arguments=True,
        )
        tasks_path = resolve_plugin_path(tasks_path)
        tasks = corpus.tasks
    except Exception as exc:  # noqa: BLE001 - malformed corpora must invalidate stale baselines.
        failure = {
            "label": label,
            "created_at": utc_now(),
            "metrics_scope": "not_started",
            "completion_status": "aborted_input_preflight",
            "failure_stage": "input_preflight",
            "failure_kind": "runner_exception",
            "completed_task_count": 0,
            "total_task_count": 0,
            "error": f"{type(exc).__name__}: {exc}",
        }
        write_invalid_run_artifacts(output_dir, failure)
        return failure

    try:
        if isinstance(max_recovery_calls, bool) or not isinstance(max_recovery_calls, int):
            raise ValueError("max_recovery_calls must be an integer")
        if max_recovery_calls < 1:
            raise ValueError("max_recovery_calls must be >= 1")
        transport_tracker = TransportFailureTracker(
            max_failed_fraction=max_transport_failed_fraction,
            max_consecutive_failures=max_consecutive_transport_failures,
            min_fraction_samples=min_transport_fraction_sample,
        )
    except ValueError as exc:
        failure = {
            "label": label,
            "created_at": utc_now(),
            "metrics_scope": "not_started",
            "completion_status": "aborted_invalid_configuration",
            "failure_stage": "configuration",
            "failure_kind": "invalid_configuration",
            "completed_task_count": 0,
            "total_task_count": len(tasks),
            "error": str(exc),
        }
        write_invalid_run_artifacts(output_dir, failure)
        return failure

    try:
        status_response: Any = mcp_call(url, "monolith_status", {}, timeout_s=timeout_s)
        status_validation = validate_mcp_status_response(
            status_response,
            result_payload=result_payload,
            result_data=result_data,
        )
    except Exception as exc:  # noqa: BLE001 - status runner defects must leave invalid artifacts.
        status_validation = {
            "ok": False,
            "failure_kind": "runner_exception",
            "raw": f"{type(exc).__name__}: {exc}",
            "transport_status": None,
        }

    if not status_validation.get("ok"):
        status_failure_kind = str(status_validation.get("failure_kind", "protocol_error"))
        raw = str(status_validation.get("raw", ""))[:500]
        failure = {
            "label": label,
            "created_at": utc_now(),
            "metrics_scope": "not_started",
            "completion_status": (
                "aborted_status_transport_failure"
                if status_failure_kind == "transport_error"
                else "aborted_status_preflight"
            ),
            "failure_stage": "status_preflight",
            "failure_kind": status_failure_kind,
            "completed_task_count": 0,
            "total_task_count": len(tasks),
            "transport_failure_count": 1 if status_failure_kind == "transport_error" else 0,
            "transport_status": status_validation.get("transport_status"),
            "transport_error_raw": raw if status_failure_kind == "transport_error" else "",
            "protocol_error_raw": raw if status_failure_kind != "transport_error" else "",
        }
        write_invalid_run_artifacts(output_dir, failure)
        return failure

    status = dict(status_validation["status"])
    start_identity = status_identity(status, endpoint=url)
    expected_catalog = str(corpus.manifest.get("catalog_version", "")).strip()
    observed_catalog = str(status.get("catalog_version", "")).strip()
    if corpus.canonical and expected_catalog and observed_catalog != expected_catalog:
        failure = {
            "label": label,
            "created_at": utc_now(),
            "metrics_scope": "not_started",
            "completion_status": "aborted_catalog_identity_mismatch",
            "failure_stage": "status_preflight",
            "failure_kind": "catalog_identity_mismatch",
            "completed_task_count": 0,
            "total_task_count": len(tasks),
            "expected_catalog_version": expected_catalog,
            "observed_catalog_version": observed_catalog,
        }
        attach_run_context(failure, corpus, start_identity)
        write_invalid_run_artifacts(output_dir, failure)
        return failure

    try:
        benchmark_inputs = build_benchmark_inputs(
            "ActionGuidance",
            tasks_path=tasks_path,
            mcp_status=status,
            extra_files={"runner": pathlib.Path(__file__)},
        )
    except Exception as exc:  # noqa: BLE001 - provenance defects invalidate the run.
        failure = {
            "label": label,
            "created_at": utc_now(),
            "metrics_scope": "not_started",
            "completion_status": "aborted_runner_exception",
            "failure_stage": "benchmark_inputs",
            "failure_kind": "runner_exception",
            "completed_task_count": 0,
            "total_task_count": len(tasks),
            "exception": f"{type(exc).__name__}: {exc}",
        }
        attach_run_context(failure, corpus, start_identity)
        write_invalid_run_artifacts(output_dir, failure)
        return failure

    rows: List[Dict[str, Any]] = []
    per_task_jsonl = output_dir / "per_task.jsonl"
    for index, task in enumerate(tasks, 1):
        runner_exception = ""
        try:
            row = score_task(url, task, max_recovery_calls, timeout_s)
        except Exception as exc:  # noqa: BLE001 - preserve the triggering task and abort.
            runner_exception = f"{type(exc).__name__}: {exc}"
            row = runner_exception_task_row(task, max_recovery_calls, runner_exception)
        rows.append(row)
        transport_decision = transport_tracker.observe(
            transport_error=bool(row.get("transport_error")),
            item_id=str(row.get("task_id", "")),
            status=(
                row.get("transport_status")
                if isinstance(row.get("transport_status"), int)
                and not isinstance(row.get("transport_status"), bool)
                else None
            ),
            raw=str(row.get("transport_error_raw", "")),
        )
        with per_task_jsonl.open("a", encoding="utf-8", newline="\n") as handle:
            handle.write(json.dumps(row, ensure_ascii=False, sort_keys=True))
            handle.write("\n")

        failure_kind = str(row.get("failure_kind", ""))
        if runner_exception or failure_kind == "protocol_error":
            failure = build_attempt_failure(
                label, status, tasks, rows, max_recovery_calls, transport_tracker,
                benchmark_inputs, corpus, start_identity,
                {
                    "metrics_scope": (
                        "attempted_prefix_runner_exception"
                        if runner_exception
                        else "attempted_prefix_protocol_failure"
                    ),
                    "completion_status": (
                        "aborted_runner_exception"
                        if runner_exception
                        else "aborted_protocol_failure"
                    ),
                    "failure_stage": "task_scoring",
                    "failure_kind": "runner_exception" if runner_exception else "protocol_error",
                    "completed_task_count": index,
                    "total_task_count": len(tasks),
                    "last_task_id": str(task.get("id", "")),
                    "exception": runner_exception,
                    "protocol_error_raw": str(row.get("protocol_error_raw", "")),
                },
            )
            write_invalid_run_artifacts(output_dir, failure)
            return failure

        if transport_decision:
            failure = build_attempt_failure(
                label, status, tasks, rows, max_recovery_calls, transport_tracker,
                benchmark_inputs, corpus, start_identity,
                {
                    "metrics_scope": "attempted_prefix_including_transport_failures",
                    "completion_status": "aborted_transport_failure_budget",
                    "failure_stage": "task_transport",
                    "failure_kind": "transport_error",
                    "completed_task_count": index,
                    "total_task_count": len(tasks),
                    "transport_gate_reason": transport_decision.reason,
                    "last_task_id": transport_decision.item_id,
                },
            )
            write_invalid_run_artifacts(output_dir, failure)
            return failure

        if index == 1 or index == len(tasks) or index % 10 == 0:
            partial = aggregate(label, status, tasks[:index], rows, max_recovery_calls)
            partial.update({
                "completed_task_count": index,
                "total_task_count": len(tasks),
                "run_valid": None,
                "metrics_valid": False,
                "metrics_scope": "attempted_prefix",
                "completion_status": "in_progress",
            })
            partial.update(transport_tracker.snapshot())
            attach_benchmark_inputs(partial, benchmark_inputs)
            attach_run_context(partial, corpus, start_identity)
            write_json(output_dir / "partial_summary.json", partial)
            print(
                f"[{index}/{len(tasks)}] {row['task_id']} "
                f"success={row['task_success']} direct={row['direct_success']}",
                flush=True,
            )

    try:
        summary = aggregate(label, status, tasks, rows, max_recovery_calls)
    except Exception as exc:  # noqa: BLE001 - aggregate defects invalidate the run.
        failure = build_attempt_failure(
            label, status, tasks, rows, max_recovery_calls, transport_tracker,
            benchmark_inputs, corpus, start_identity,
            {
                "metrics_scope": "complete_run_invalid",
                "completion_status": "aborted_runner_exception",
                "failure_stage": "final_aggregate",
                "failure_kind": "runner_exception",
                "completed_task_count": len(rows),
                "total_task_count": len(tasks),
                "exception": f"{type(exc).__name__}: {exc}",
            },
        )
        write_invalid_run_artifacts(output_dir, failure)
        return failure

    summary.update({
        "run_valid": True,
        "completion_status": "completed",
        "metrics_valid": True,
        "metrics_scope": "complete_run" if corpus.comparable else "complete_subset_run",
        "max_recovery_calls": max_recovery_calls,
    })
    summary.update(transport_tracker.snapshot())
    attach_benchmark_inputs(summary, benchmark_inputs)
    attach_run_context(summary, corpus, start_identity)

    final_transport_decision = transport_tracker.finalize()
    if final_transport_decision:
        summary.update({
            "metrics_scope": "complete_run_invalid",
            "completion_status": "completed_transport_failure_budget_exceeded",
            "failure_stage": "task_transport_finalize",
            "failure_kind": "transport_error",
            "completed_task_count": len(rows),
            "total_task_count": len(tasks),
            "transport_gate_reason": final_transport_decision.reason,
            "last_task_id": final_transport_decision.item_id,
        })
        write_invalid_run_artifacts(output_dir, summary)
        return summary

    try:
        end_status_response: Any = mcp_call(url, "monolith_status", {}, timeout_s=timeout_s)
        end_status_validation = validate_mcp_status_response(
            end_status_response,
            result_payload=result_payload,
            result_data=result_data,
        )
    except Exception as exc:  # noqa: BLE001 - postflight must invalidate the run.
        end_status_validation = {
            "ok": False,
            "failure_kind": "runner_exception",
            "raw": f"{type(exc).__name__}: {exc}",
            "transport_status": None,
        }
    if not end_status_validation.get("ok"):
        summary.update({
            "metrics_scope": "complete_run_invalid",
            "completion_status": "aborted_status_postflight",
            "failure_stage": "status_postflight",
            "failure_kind": str(end_status_validation.get("failure_kind", "protocol_error")),
            "postflight_status_raw": str(end_status_validation.get("raw", ""))[:500],
            "postflight_transport_status": end_status_validation.get("transport_status"),
        })
        write_invalid_run_artifacts(output_dir, summary)
        return summary

    end_status = dict(end_status_validation["status"])
    end_identity = status_identity(end_status, endpoint=url)
    identity_drift = status_identity_mismatches(start_identity, end_identity)
    attach_run_context(summary, corpus, start_identity, end_identity)
    if identity_drift:
        summary.update({
            "metrics_scope": "complete_run_invalid",
            "completion_status": "aborted_status_identity_drift",
            "failure_stage": "status_postflight",
            "failure_kind": "status_identity_drift",
            "status_identity_mismatches": identity_drift,
        })
        write_invalid_run_artifacts(output_dir, summary)
        return summary

    write_json(output_dir / "summary.json", summary)
    write_json(output_dir / "per_task.json", rows)
    partial_path = output_dir / "partial_summary.json"
    if partial_path.exists():
        partial_path.unlink()
    return summary


def compare_runs(baseline_path: pathlib.Path, current_path: pathlib.Path, output_dir: pathlib.Path) -> Dict[str, Any]:
    baseline = json.loads(baseline_path.read_text(encoding="utf-8"))
    current = json.loads(current_path.read_text(encoding="utf-8"))
    base_metrics = baseline["metrics"]
    cur_metrics = current["metrics"]
    deltas = {}
    for key, cur_value in cur_metrics.items():
        base_value = base_metrics.get(key)
        if isinstance(cur_value, (int, float)) and isinstance(base_value, (int, float)):
            deltas[key] = round(cur_value - base_value, 6)
    comparison = {
        "created_at": utc_now(),
        "baseline": baseline,
        "current": current,
        "deltas": deltas,
    }
    output_dir.mkdir(parents=True, exist_ok=True)
    write_json(output_dir / "comparison.json", comparison)
    write_comparison_markdown(output_dir / "comparison.md", comparison)
    return comparison


def write_comparison_markdown(path: pathlib.Path, comparison: Dict[str, Any]) -> None:
    baseline = comparison["baseline"]
    current = comparison["current"]
    deltas = comparison["deltas"]
    metrics = [
        "effectiveness_score",
        "task_success_rate",
        "first_recovery_success_rate",
        "action_selection_accuracy",
        "param_correction_accuracy",
        "mean_tool_calls_to_success",
        "invalid_retry_rate",
        "hallucinated_workflow_rate",
    ]
    lines = [
        "# Monolith Action Guidance Benchmark Comparison",
        "",
        f"- Created: `{comparison['created_at']}`",
        f"- Baseline: `{baseline['label']}`",
        f"- Current: `{current['label']}`",
        f"- Task count: `{current['task_count']}`",
        "",
        "| Metric | Baseline | Current | Delta |",
        "| --- | ---: | ---: | ---: |",
    ]
    for metric in metrics:
        base_value = baseline["metrics"].get(metric)
        cur_value = current["metrics"].get(metric)
        delta = deltas.get(metric)
        lines.append(f"| `{metric}` | {base_value} | {cur_value} | {delta} |")
    lines.append("")
    lines.append("Higher is better for all metrics except `mean_tool_calls_to_success`, `invalid_retry_rate`, and `hallucinated_workflow_rate`.")
    path.write_text("\n".join(lines) + "\n", encoding="utf-8", newline="\n")


def main(argv: Optional[List[str]] = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="cmd", required=True)

    gen = sub.add_parser("generate", help="Generate a reusable task set from a live MCP catalog")
    gen.add_argument("--mcp-url", default=DEFAULT_MCP_URL)
    gen.add_argument("--tasks", type=pathlib.Path, default=DEFAULT_TASKS)
    gen.add_argument("--manifest", type=pathlib.Path, default=DEFAULT_MANIFEST)
    gen.add_argument("--min-tasks", type=int, default=120)

    run = sub.add_parser("run", help="Run a task set against one MCP endpoint")
    run.add_argument("--mcp-url", default=DEFAULT_MCP_URL)
    run.add_argument("--tasks", type=pathlib.Path, default=DEFAULT_TASKS)
    run.add_argument("--output-dir", type=pathlib.Path, required=True)
    run.add_argument("--label", required=True)
    run.add_argument("--max-recovery-calls", type=int, default=DEFAULT_MAX_RECOVERY_CALLS)
    run.add_argument(
        "--allow-subset",
        action="store_true",
        help="Run a non-canonical diagnostic task subset; results are marked non-comparable.",
    )
    run.add_argument("--request-timeout-s", type=float, default=12.0)
    run.add_argument(
        "--max-transport-failed-fraction",
        type=float,
        default=DEFAULT_MAX_TRANSPORT_FAILED_FRACTION,
        help="Abort without summary when transport failures exceed this fraction after 20 tasks.",
    )
    run.add_argument(
        "--max-consecutive-transport-failures",
        type=int,
        default=DEFAULT_MAX_CONSECUTIVE_TRANSPORT_FAILURES,
        help="Abort without summary after this many consecutive transport failures.",
    )
    run.add_argument(
        "--min-transport-fraction-sample",
        type=int,
        default=DEFAULT_MIN_TRANSPORT_FRACTION_SAMPLES,
        help="Minimum completed tasks before applying the transport-fraction gate.",
    )

    cmp_cmd = sub.add_parser("compare", help="Compare two run summary files")
    cmp_cmd.add_argument("--baseline", type=pathlib.Path, required=True)
    cmp_cmd.add_argument("--current", type=pathlib.Path, required=True)
    cmp_cmd.add_argument("--output-dir", type=pathlib.Path, required=True)

    args = parser.parse_args(argv)
    if args.cmd == "generate":
        manifest = generate_tasks(args.mcp_url, args.min_tasks, args.tasks, args.manifest)
        print(json.dumps(manifest, indent=2, ensure_ascii=False))
        return 0
    if args.cmd == "run":
        summary = run_benchmark(
            args.mcp_url,
            args.tasks,
            args.output_dir,
            args.label,
            args.max_recovery_calls,
            args.request_timeout_s,
            args.max_transport_failed_fraction,
            args.max_consecutive_transport_failures,
            args.min_transport_fraction_sample,
            allow_subset=args.allow_subset,
        )
        print(json.dumps(summary, indent=2, ensure_ascii=False))
        return 0 if summary.get("run_valid") else 1
    if args.cmd == "compare":
        comparison = compare_runs(args.baseline, args.current, args.output_dir)
        print(json.dumps({"output_dir": str(args.output_dir), "deltas": comparison["deltas"]}, indent=2))
        return 0
    return 2


if __name__ == "__main__":
    raise SystemExit(main())
