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
from typing import Any, Dict, Iterable, List, Optional, Tuple


DEFAULT_MCP_URL = "http://localhost:9316/mcp"
DEFAULT_TASKS = pathlib.Path("Plugins/Monolith/Benchmarks/ActionGuidance/tasks.jsonl")
DEFAULT_MANIFEST = pathlib.Path("Plugins/Monolith/Benchmarks/ActionGuidance/manifest.json")
DEFAULT_RESULTS_ROOT = pathlib.Path("Saved/Monolith/Benchmarks/ActionGuidance")

READ_ONLY_POLICY_IDS = {"", "read_only"}


def utc_now() -> str:
    return _dt.datetime.now(_dt.timezone.utc).isoformat()


def load_jsonl(path: pathlib.Path) -> List[Dict[str, Any]]:
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


def mcp_call(url: str, tool: str, arguments: Dict[str, Any], timeout_s: float = 45.0) -> Dict[str, Any]:
    body = {
        "jsonrpc": "2.0",
        "id": int(time.time() * 1000) % 1000000000,
        "method": "tools/call",
        "params": {
            "name": tool,
            "arguments": arguments,
        },
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

    raw = extract_sse_data(raw)
    try:
        parsed = json.loads(raw)
    except json.JSONDecodeError:
        parsed = {"parse_error": True, "raw": raw}
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


def discover_schema(url: str, namespace: str, action: str, timeout_s: float = 45.0) -> Optional[Dict[str, Any]]:
    response = mcp_call(url, "monolith_discover", {"namespace": namespace, "action": action, "mode": "schema"}, timeout_s=timeout_s)
    parsed = result_data(response)
    if not parsed:
        return None
    schema = parsed.get("schema")
    return schema if isinstance(schema, dict) else None


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


def generate_tasks(url: str, min_tasks: int, tasks_path: pathlib.Path, manifest_path: pathlib.Path) -> Dict[str, Any]:
    summary_response = mcp_call(url, "monolith_discover", {})
    summary = text_json(summary_response)
    if not summary or "namespaces" not in summary:
        raise RuntimeError("monolith_discover summary did not return namespaces")

    namespaces = [row for row in summary.get("namespaces", []) if isinstance(row, dict)]
    namespace_rows = []
    tasks: List[Dict[str, Any]] = []
    schema_cache: Dict[Tuple[str, str], Dict[str, Any]] = {}

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

    write_jsonl(tasks_path, tasks)
    manifest = {
        "generated_at": utc_now(),
        "mcp_url": url,
        "task_count": len(tasks),
        "min_tasks_requested": min_tasks,
        "catalog_namespace_count": len(namespace_rows),
        "catalog_action_count": sum(row["action_count"] for row in namespace_rows),
        "namespace_coverage": namespace_rows,
        "category_counts": count_by(tasks, "category"),
        "task_file": str(tasks_path).replace("\\", "/"),
        "scoring": {
            "effectiveness_score": "0.30*task_success_rate + 0.20*first_recovery_success_rate + 0.15*action_selection_accuracy + 0.15*param_correction_accuracy + 0.10*(1-normalized_tool_calls) + 0.10*(1-hallucinated_workflow_rate)",
            "normalized_tool_calls": "clamp((mean_tool_calls_to_success - 1) / 3, 0, 1)",
        },
    }
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


def call_discover_actions(url: str, namespace: str, timeout_s: float = 45.0) -> List[str]:
    response = mcp_call(url, "monolith_discover", {"namespace": namespace, "mode": "actions"}, timeout_s=timeout_s)
    parsed = result_data(response)
    if not parsed:
        return []
    actions = parsed.get("actions")
    out: List[str] = []
    if isinstance(actions, list):
        for row in actions:
            if isinstance(row, dict) and isinstance(row.get("action"), str):
                out.append(row["action"])
            elif isinstance(row, str):
                out.append(row)
    return out


def candidate_contains(payload: Dict[str, Any], expected_action_id: str) -> bool:
    candidates = array_field(payload, "candidate_actions")
    for candidate in candidates:
        if isinstance(candidate, dict) and candidate.get("action_id") == expected_action_id:
            return True
        if isinstance(candidate, str) and candidate == expected_action_id:
            return True
    return False


def score_task(url: str, task: Dict[str, Any], max_recovery_calls: int, timeout_s: float) -> Dict[str, Any]:
    response = mcp_call(url, str(task["tool"]), dict(task.get("arguments", {})), timeout_s=timeout_s)
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
            direct_success = bool(has_signals and has_skill and status_explicit)
            recovered = direct_success
            hallucinated_workflow_risk = 0.0 if status_explicit else 1.0
            evidence = {
                "has_planning_signals": has_signals,
                "has_skill": has_skill,
                "output_contract_status": output_status,
                "next_actions_status": next_status,
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
            actions = call_discover_actions(url, str(task.get("namespace", "")), timeout_s=timeout_s)
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
            schema = discover_schema(url, str(task.get("namespace", "")), str(task.get("action", "")), timeout_s=timeout_s)
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
            schema = discover_schema(url, str(task.get("namespace", "")), str(task.get("action", "")), timeout_s=timeout_s)
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

    return {
        "task_id": task.get("id"),
        "category": category,
        "namespace": task.get("namespace"),
        "action": task.get("action"),
        "direct_success": direct_success,
        "task_success": recovered,
        "tool_calls_to_success": tool_calls,
        "action_selection_score": action_selection_score,
        "param_correction_score": param_correction_score,
        "hallucinated_workflow_risk": hallucinated_workflow_risk,
        "evidence": evidence,
        "transport_error": bool(response.get("transport_error")),
        "transport_error_raw": str(response.get("raw", ""))[:300] if response.get("transport_error") else "",
        "response_is_error": bool(payload.get("isError")),
        "response_text": result_text(response)[:1000],
    }


def avg(values: List[float]) -> float:
    return sum(values) / len(values) if values else 0.0


def aggregate(label: str, status: Dict[str, Any], tasks: List[Dict[str, Any]], rows: List[Dict[str, Any]], max_recovery_calls: int) -> Dict[str, Any]:
    total = len(rows)
    failure_rows = [r for r in rows if r["category"] in ("unknown_action_recovery", "missing_required_param", "invalid_param_type")]
    action_rows = [r for r in rows if r["category"] in ("unknown_action_recovery", "discovery_planning")]
    param_rows = [r for r in rows if r["category"] in ("missing_required_param", "invalid_param_type")]
    workflow_rows = [r for r in rows if r["hallucinated_workflow_risk"] is not None]

    task_success_rate = avg([1.0 if r["task_success"] else 0.0 for r in rows])
    first_recovery_success_rate = avg([1.0 if r["direct_success"] else 0.0 for r in failure_rows])
    action_selection_accuracy = avg([float(r["action_selection_score"]) for r in action_rows if r["action_selection_score"] is not None])
    param_correction_accuracy = avg([float(r["param_correction_score"]) for r in param_rows if r["param_correction_score"] is not None])
    mean_calls = avg([float(r["tool_calls_to_success"]) for r in rows])
    normalized_tool_calls = max(0.0, min(1.0, (mean_calls - 1.0) / max(1.0, float(max_recovery_calls - 1))))
    invalid_retry_rate = avg([0.0 if r["direct_success"] else 1.0 for r in failure_rows])
    hallucinated_workflow_rate = avg([float(r["hallucinated_workflow_risk"]) for r in workflow_rows])
    effectiveness_score = (
        0.30 * task_success_rate
        + 0.20 * first_recovery_success_rate
        + 0.15 * action_selection_accuracy
        + 0.15 * param_correction_accuracy
        + 0.10 * (1.0 - normalized_tool_calls)
        + 0.10 * (1.0 - hallucinated_workflow_rate)
    )

    namespace_count = len({str(t.get("namespace")) for t in tasks})
    return {
        "label": label,
        "created_at": utc_now(),
        "mcp_status": status,
        "task_count": total,
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


def run_benchmark(
    url: str,
    tasks_path: pathlib.Path,
    output_dir: pathlib.Path,
    label: str,
    max_recovery_calls: int,
    timeout_s: float,
) -> Dict[str, Any]:
    tasks = load_jsonl(tasks_path)
    output_dir.mkdir(parents=True, exist_ok=True)
    status_response = mcp_call(url, "monolith_status", {}, timeout_s=timeout_s)
    status = result_data(status_response)

    rows: List[Dict[str, Any]] = []
    per_task_jsonl = output_dir / "per_task.jsonl"
    if per_task_jsonl.exists():
        per_task_jsonl.unlink()
    for index, task in enumerate(tasks, 1):
        row = score_task(url, task, max_recovery_calls, timeout_s)
        rows.append(row)
        with per_task_jsonl.open("a", encoding="utf-8", newline="\n") as handle:
            handle.write(json.dumps(row, ensure_ascii=False, sort_keys=True))
            handle.write("\n")
        if index == 1 or index == len(tasks) or index % 10 == 0:
            partial = aggregate(label, status, tasks[:index], rows, max_recovery_calls)
            partial["completed_task_count"] = index
            partial["total_task_count"] = len(tasks)
            write_json(output_dir / "partial_summary.json", partial)
            print(f"[{index}/{len(tasks)}] {row['task_id']} success={row['task_success']} direct={row['direct_success']}", flush=True)
    summary = aggregate(label, status, tasks, rows, max_recovery_calls)
    write_json(output_dir / "summary.json", summary)
    write_json(output_dir / "per_task.json", rows)
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
    run.add_argument("--max-recovery-calls", type=int, default=3)
    run.add_argument("--request-timeout-s", type=float, default=12.0)

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
        summary = run_benchmark(args.mcp_url, args.tasks, args.output_dir, args.label, args.max_recovery_calls, args.request_timeout_s)
        print(json.dumps(summary, indent=2, ensure_ascii=False))
        return 0
    if args.cmd == "compare":
        comparison = compare_runs(args.baseline, args.current, args.output_dir)
        print(json.dumps({"output_dir": str(args.output_dir), "deltas": comparison["deltas"]}, indent=2))
        return 0
    return 2


if __name__ == "__main__":
    raise SystemExit(main())
