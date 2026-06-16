#!/usr/bin/env python3
"""
Monolith MCP SourceIndex Quality benchmark.

Measures whether the source namespace (C++ symbol index) returns data that is
rich enough for agents to get useful code context.  The benchmark is
deterministic and does not call an LLM.

Six task categories:
  symbol_lookup          - search_source / find_callers / find_callees / risk_score for engine symbols.
  review_context_lookup  - review_context for engine symbols (scored like symbol_lookup).
  impact_radius_lookup   - impact_radius for engine symbols (scored like symbol_lookup).
  ergonomics_text        - get_include_path / get_signature / check_deprecations / verify_symbols /
                           find_example_usage - plain-text ergonomic tools.
  health_check           - source health, presence of status + symbol_count.
  schema_field_presence  - monolith_discover schema check for planning signals.
"""

from __future__ import annotations

import argparse
import datetime as _dt
import json
import math
import pathlib
import re
import socket
import sys
import time
import urllib.error
import urllib.request
from typing import Any, Dict, Iterable, List, Optional, Tuple


DEFAULT_MCP_URL = "http://localhost:9316/mcp"
DEFAULT_TASKS = pathlib.Path("Plugins/Monolith/Benchmarks/SourceIndex/tasks.jsonl")
DEFAULT_MANIFEST = pathlib.Path("Plugins/Monolith/Benchmarks/SourceIndex/manifest.json")
DEFAULT_RESULTS_ROOT = pathlib.Path("Saved/Monolith/Benchmarks/SourceIndex")

EXTENDED_SYMBOLS = [
    "AActor", "UObject", "UActorComponent", "UGameplayStatics", "FVector", "FString",
    "ACharacter", "APlayerController", "UStaticMeshComponent", "UPrimitiveComponent",
    "AGameModeBase", "UGameInstance", "UWorld", "ULevel", "UBlueprintFunctionLibrary",
    "FHitResult", "FTransform", "FRotator", "TArray", "TMap", "USceneComponent",
    "USkeletalMeshComponent", "UAnimInstance", "AController", "APawn",
]
GOLDEN_SYMBOLS = EXTENDED_SYMBOLS[:25]
SCHEMA_ACTIONS = [
    "search_source", "find_callers", "find_callees", "risk_score",
    "review_context", "impact_radius", "get_include_path", "get_signature",
    "verify_symbols", "find_example_usage", "find_overrides", "review_hotspots",
]
REQUIRED_RESULT_FIELDS = {"name", "kind", "file_path", "location"}


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


def count_by(rows: Iterable[Dict[str, Any]], field: str) -> Dict[str, int]:
    out: Dict[str, int] = {}
    for row in rows:
        key = str(row.get(field, ""))
        out[key] = out.get(key, 0) + 1
    return dict(sorted(out.items()))


def avg(values: List[float]) -> float:
    return sum(values) / len(values) if values else 0.0


# ---------------------------------------------------------------------------
# Result-field helpers
# ---------------------------------------------------------------------------

_SYMBOL_MATCH_RE = re.compile(r"^\s*\[(?P<kind>[^\]]+)\]\s+(?P<name>.+?)\s*\((?P<path>.+):(?P<line>\d+)\)\s*$")


def _parse_symbol_text_blob(text: str) -> List[Dict[str, Any]]:
    """Parse the plain-text symbol blob emitted by search_source / find_callers / find_callees.

    Lines look like ``  [class] AActor (D:/path/Actor.h:256)``.  An explicit
    "No direct ... found" / "No matches" sentence yields an empty list (a truthful
    zero result, not a parse failure).
    """
    results: List[Dict[str, Any]] = []
    for raw in text.splitlines():
        match = _SYMBOL_MATCH_RE.match(raw)
        if not match:
            continue
        path = match.group("path").strip()
        line = match.group("line").strip()
        results.append({
            "name": match.group("name").strip(),
            "kind": match.group("kind").strip(),
            "file_path": path,
            "location": f"{path}:{line}",
        })
    return results


def _content_text(data: Dict[str, Any]) -> Optional[str]:
    """Return the inner content[0].text string if the payload nests a content blob."""
    content = data.get("content") if isinstance(data, dict) else None
    if isinstance(content, list) and content:
        first = content[0]
        if isinstance(first, dict) and isinstance(first.get("text"), str):
            return first["text"]
    return None


def symbol_results(data: Dict[str, Any]) -> List[Dict[str, Any]]:
    """Return list of symbol result dicts from any source lookup response.

    Covers the real source-namespace shapes:
      - search_source / find_callers / find_callees: nested ``content[0].text`` blob.
      - risk_score: top-level ``items`` list.
      - impact_radius: ``seed_symbols`` / ``impacted_symbols`` lists.
      - review_context: ``top_risks`` list plus the single ``risk`` seed dict.
    """
    if not isinstance(data, dict):
        return []
    for key in ("results", "symbols", "matches", "items", "callers", "callees",
                "impacted_symbols", "seed_symbols", "top_risks"):
        value = data.get(key)
        if isinstance(value, list) and value:
            dicts = [r for r in value if isinstance(r, dict)]
            if dicts:
                return dicts
    # review_context exposes the primary symbol under a single ``risk`` dict.
    risk = data.get("risk")
    if isinstance(risk, dict) and risk.get("name") is not None:
        return [risk]
    # search_source / find_callers / find_callees return a plain-text blob.
    text = _content_text(data)
    if text is not None:
        return _parse_symbol_text_blob(text)
    return []


def result_has_min_fields(result: Dict[str, Any]) -> bool:
    """True if at least 3 of the required fields are present.

    ``file`` is accepted as an alias for ``file_path`` and ``line`` as an alias for
    ``location`` because the source actions (risk_score, impact_radius, review_context)
    expose ``file``/``line`` rather than ``file_path``/``location``.
    """
    present = 0
    if result.get("name") is not None:
        present += 1
    if result.get("kind") is not None:
        present += 1
    if result.get("file_path") is not None or result.get("file") is not None:
        present += 1
    if result.get("location") is not None or result.get("line") is not None:
        present += 1
    return present >= 3


def health_symbol_count(data: Dict[str, Any]) -> Optional[int]:
    """Return the indexed symbol count from any of the real health shapes."""
    for key in ("symbol_count", "total_symbols"):
        value = data.get(key)
        if isinstance(value, int):
            return value
    row_counts = data.get("row_counts")
    if isinstance(row_counts, dict):
        symbols = row_counts.get("symbols")
        if isinstance(symbols, int):
            return symbols
    return None


def health_fields_present(data: Dict[str, Any], require_count: bool = True) -> bool:
    """True if health response contains status (and a symbol count when required).

    The source health action only emits counts (``row_counts.symbols``) when the
    request passes ``include_counts=true``; a plain or ``include_counts=false``
    request returns ``status`` without counts.  ``require_count`` lets the caller
    demand a count only for count-requesting tasks.  The count is read from
    ``row_counts.symbols`` (the real shape) as well as top-level
    ``symbol_count``/``total_symbols``.
    """
    has_status = "status" in data
    if not require_count:
        return has_status
    has_count = health_symbol_count(data) is not None
    return has_status and has_count


def health_is_stale_or_error(data: Dict[str, Any], require_count: bool = True) -> bool:
    """True if health response signals a stale index, error, or missing fields."""
    if not health_fields_present(data, require_count=require_count):
        return True
    status = str(data.get("status", "")).lower()
    if status in ("error", "stale", "unavailable", "unknown", ""):
        return True
    stale = data.get("stale") or data.get("is_stale")
    return bool(stale)


def schema_has_planning_signals(schema: Dict[str, Any]) -> bool:
    signals = schema.get("planning_signals")
    return isinstance(signals, list) and len(signals) > 0


def schema_has_skill(schema: Dict[str, Any]) -> bool:
    return isinstance(schema.get("skill"), str) and bool(schema.get("skill"))


def schema_statuses_declared(schema: Dict[str, Any]) -> bool:
    output_status = schema.get("output_contract_status")
    next_status = schema.get("next_actions_status")
    return (
        output_status in ("declared", "not_declared")
        and next_status in ("declared", "not_declared")
    )


# ---------------------------------------------------------------------------
# Task scoring
# ---------------------------------------------------------------------------

def score_task(url: str, task: Dict[str, Any], timeout_s: float) -> Dict[str, Any]:
    response = mcp_call(url, str(task["tool"]), dict(task.get("arguments", {})), timeout_s=timeout_s)
    data = result_data(response)
    category = task.get("category")
    is_error_response = bool(result_payload(response).get("isError")) or bool(response.get("transport_error"))
    expected = task.get("expected") if isinstance(task.get("expected"), dict) else {}
    min_results = expected.get("min_results")
    allows_empty = isinstance(min_results, int) and min_results == 0

    direct_success = False
    action_selection_score: Optional[float] = None
    param_correction_score: Optional[float] = None
    hallucinated_workflow_risk: Optional[float] = None
    results_count = 0
    field_complete_count = 0
    evidence: Dict[str, Any] = {}

    if category == "symbol_lookup":
        results = symbol_results(data)
        results_count = len(results)
        has_named = results_count > 0 and any(r.get("name") is not None for r in results)
        # A successfully executed query whose truthful answer is "no results"
        # (e.g. no direct C++ callers for an engine base class) is a hit when the
        # task authored min_results == 0.
        direct_success = has_named or (not is_error_response and allows_empty)
        action_selection_score = 1.0 if (results_count > 0 or (not is_error_response and allows_empty)) else 0.0
        field_complete_count = sum(1 for r in results if result_has_min_fields(r))
        evidence = {
            "results_count": results_count,
            "field_complete_count": field_complete_count,
            "first_result_keys": list(results[0].keys())[:8] if results else [],
        }

    elif category == "health_check":
        # Counts (row_counts.symbols) are only emitted when include_counts=true;
        # a plain or include_counts=false health request returns status only.
        task_args = task.get("arguments", {})
        require_count = bool(task_args.get("include_counts")) is True
        fields_ok = health_fields_present(data, require_count=require_count)
        is_stale = health_is_stale_or_error(data, require_count=require_count)
        direct_success = fields_ok and not is_stale
        param_correction_score = 1.0 if fields_ok else 0.0
        evidence = {
            "health_fields_present": fields_ok,
            "is_stale_or_error": is_stale,
            "require_count": require_count,
            "status": data.get("status"),
            "symbol_count": health_symbol_count(data),
        }

    elif category == "schema_field_presence":
        schema = data.get("schema") if isinstance(data, dict) else None
        if not isinstance(schema, dict):
            schema = data
        has_signals = schema_has_planning_signals(schema)
        has_skill = schema_has_skill(schema)
        statuses_ok = schema_statuses_declared(schema)
        direct_success = bool(has_signals and has_skill and statuses_ok)
        hallucinated_workflow_risk = 0.0 if statuses_ok else 1.0
        evidence = {
            "has_planning_signals": has_signals,
            "has_skill": has_skill,
            "output_contract_status": schema.get("output_contract_status"),
            "next_actions_status": schema.get("next_actions_status"),
            "statuses_declared": statuses_ok,
        }

    elif category == "ergonomics_text":
        response_text = result_text(response)
        direct_success = bool(
            response_text
            and len(response_text.strip()) > 0
            and not response_text.startswith("Error")
        )
        results_count = 1 if direct_success else 0
        field_complete_count = results_count
        evidence = {
            "response_len": len(response_text),
            "response_preview": response_text[:80],
        }

    elif category in ("review_context_lookup", "impact_radius_lookup"):
        results = symbol_results(data)
        results_count = len(results)
        has_named = results_count > 0 and any(r.get("name") is not None for r in results)
        direct_success = has_named or (not is_error_response and allows_empty)
        action_selection_score = 1.0 if (results_count > 0 or (not is_error_response and allows_empty)) else 0.0
        field_complete_count = sum(1 for r in results if result_has_min_fields(r))
        evidence = {
            "results_count": results_count,
            "field_complete_count": field_complete_count,
            "first_result_keys": list(results[0].keys())[:8] if results else [],
        }

    else:
        evidence = {"unsupported_category": category}

    return {
        "task_id": task.get("id"),
        "category": category,
        "namespace": task.get("namespace"),
        "action": task.get("action"),
        "direct_success": direct_success,
        "action_selection_score": action_selection_score,
        "param_correction_score": param_correction_score,
        "hallucinated_workflow_risk": hallucinated_workflow_risk,
        "results_count": results_count,
        "field_complete_count": field_complete_count,
        "evidence": evidence,
        "transport_error": bool(response.get("transport_error")),
        "transport_error_raw": str(response.get("raw", ""))[:300] if response.get("transport_error") else "",
        "response_is_error": bool(result_payload(response).get("isError")),
        "response_text": result_text(response)[:1000],
    }


# ---------------------------------------------------------------------------
# Aggregate
# ---------------------------------------------------------------------------

def aggregate(label: str, status: Dict[str, Any], tasks: List[Dict[str, Any]], rows: List[Dict[str, Any]]) -> Dict[str, Any]:
    lookup_rows = [
        r for r in rows
        if r["category"] in ("symbol_lookup", "review_context_lookup", "impact_radius_lookup")
    ]
    health_rows = [r for r in rows if r["category"] == "health_check"]
    schema_rows = [r for r in rows if r["category"] == "schema_field_presence"]
    ergonomics_rows = [r for r in rows if r["category"] == "ergonomics_text"]

    # symbol_hit_rate: fraction of symbol_lookup + review_context_lookup + impact_radius_lookup with direct_success
    symbol_hit_rate = avg([1.0 if r["direct_success"] else 0.0 for r in lookup_rows])

    # field_completeness_rate: across all symbol results, fraction that have >=3 required fields
    total_results = sum(r["results_count"] for r in lookup_rows)
    total_complete = sum(r["field_complete_count"] for r in lookup_rows)
    field_completeness_rate = total_complete / total_results if total_results > 0 else 0.0

    # schema_adherence_rate: fraction of schema_field_presence tasks that pass
    schema_adherence_rate = avg([1.0 if r["direct_success"] else 0.0 for r in schema_rows])

    # stale_rate: fraction of health_check tasks with stale/error flags (direct_success=False)
    stale_rate = avg([0.0 if r["direct_success"] else 1.0 for r in health_rows])

    # mean_results_per_lookup: average result count for lookup tasks
    mean_results_per_lookup = avg([float(r["results_count"]) for r in lookup_rows])

    # ergonomics_success_rate: fraction of ergonomics_text tasks with non-empty, non-error response
    ergonomics_success_rate = avg([1.0 if r["direct_success"] else 0.0 for r in ergonomics_rows])

    source_index_score = (
        0.35 * symbol_hit_rate
        + 0.25 * field_completeness_rate
        + 0.20 * schema_adherence_rate
        + 0.10 * (1.0 - stale_rate)
        + 0.10 * ergonomics_success_rate
    )

    return {
        "label": label,
        "created_at": utc_now(),
        "mcp_status": status,
        "task_count": len(rows),
        "category_counts": count_by(tasks, "category"),
        "metrics": {
            "source_index_score": round(source_index_score, 6),
            "symbol_hit_rate": round(symbol_hit_rate, 6),
            "field_completeness_rate": round(field_completeness_rate, 6),
            "schema_adherence_rate": round(schema_adherence_rate, 6),
            "stale_rate": round(stale_rate, 6),
            "ergonomics_success_rate": round(ergonomics_success_rate, 6),
            "mean_results_per_lookup": round(mean_results_per_lookup, 6),
        },
    }


# ---------------------------------------------------------------------------
# Generate
# ---------------------------------------------------------------------------

_CALLERS_CALLEES_SYMBOLS = [
    "AActor", "UObject", "UActorComponent", "UGameplayStatics", "FVector",
    "ACharacter", "APlayerController", "UStaticMeshComponent", "AGameModeBase",
    "UGameInstance", "UWorld", "FHitResult", "FTransform", "USceneComponent",
    "USkeletalMeshComponent",
]

_RISK_SCORE_SYMBOLS = [
    "AActor", "UObject", "UActorComponent", "UGameplayStatics", "FVector",
    "ACharacter", "AGameModeBase", "UGameInstance", "UWorld", "FHitResult",
    "USceneComponent", "AController",
]

_REVIEW_CONTEXT_SYMBOLS = [
    "AActor", "UObject", "UGameplayStatics", "ACharacter",
    "AGameModeBase", "UGameInstance", "UWorld", "USceneComponent",
]

_IMPACT_RADIUS_SYMBOLS = [
    "AActor", "UObject", "UGameplayStatics", "ACharacter",
    "UStaticMeshComponent", "USkeletalMeshComponent", "AController", "APawn",
]

_ERGONOMICS_INCLUDE_SYMBOLS = [
    "AActor", "UObject", "ACharacter", "AGameModeBase", "UGameInstance", "UWorld",
]

_ERGONOMICS_SIGNATURE_METHODS = [
    "UGameplayStatics::ApplyDamage",
    "AActor::BeginPlay",
    "AActor::EndPlay",
    "UObject::GetClass",
    "ACharacter::Jump",
]

_ERGONOMICS_DEPRECATION_LISTS = [
    ["AActor"],
    ["PreparePathfinding", "AActor"],
    ["UGameplayStatics"],
    ["UObject", "ACharacter"],
]

_ERGONOMICS_VERIFY_LISTS = [
    ["AActor", "UObject", "UNonExistentClass999"],
    ["UGameplayStatics::ApplyDamage", "ACharacter::Jump"],
    ["UStaticMeshComponent", "USkeletalMeshComponent", "UPrimitiveComponent"],
]

_ERGONOMICS_EXAMPLE_USAGE = [
    ("UGameplayStatics::ApplyDamage", 5),
    ("AActor::BeginPlay", 3),
]


def build_static_tasks() -> List[Dict[str, Any]]:
    """Build a deterministic task list from extended symbols and schema actions."""
    tasks: List[Dict[str, Any]] = []

    def next_id() -> str:
        return f"SIB-{len(tasks) + 1:03d}"

    # --- symbol_lookup: search_source for all 25 extended symbols ---
    for symbol in GOLDEN_SYMBOLS:
        tasks.append({
            "id": next_id(),
            "category": "symbol_lookup",
            "namespace": "source",
            "action": "search_source",
            "tool": "source_query",
            "arguments": {"action": "search_source", "query": symbol},
            "expected": {"min_results": 1, "required_fields": ["name", "kind", "file_path"]},
            "safety": "read_only",
        })

    # --- symbol_lookup: find_callers for 15 symbols ---
    for symbol in _CALLERS_CALLEES_SYMBOLS:
        tasks.append({
            "id": next_id(),
            "category": "symbol_lookup",
            "namespace": "source",
            "action": "find_callers",
            "tool": "source_query",
            "arguments": {"action": "find_callers", "symbol": symbol},
            "expected": {"min_results": 0, "required_fields": ["name", "kind", "file_path"]},
            "safety": "read_only",
        })

    # --- symbol_lookup: find_callees for 15 symbols ---
    for symbol in _CALLERS_CALLEES_SYMBOLS:
        tasks.append({
            "id": next_id(),
            "category": "symbol_lookup",
            "namespace": "source",
            "action": "find_callees",
            "tool": "source_query",
            "arguments": {"action": "find_callees", "symbol": symbol},
            "expected": {"min_results": 0, "required_fields": ["name", "kind", "file_path"]},
            "safety": "read_only",
        })

    # --- symbol_lookup: risk_score for 12 symbols ---
    for symbol in _RISK_SCORE_SYMBOLS:
        tasks.append({
            "id": next_id(),
            "category": "symbol_lookup",
            "namespace": "source",
            "action": "risk_score",
            "tool": "source_query",
            "arguments": {"action": "risk_score", "symbol": symbol},
            "expected": {"min_results": 0, "required_fields": ["name", "kind", "file_path"]},
            "safety": "read_only",
        })

    # --- review_context_lookup for 8 symbols ---
    for symbol in _REVIEW_CONTEXT_SYMBOLS:
        tasks.append({
            "id": next_id(),
            "category": "review_context_lookup",
            "namespace": "source",
            "action": "review_context",
            "tool": "source_query",
            "arguments": {"action": "review_context", "symbol": symbol},
            "expected": {"min_results": 0, "required_fields": ["name", "kind", "file_path"]},
            "safety": "read_only",
        })

    # --- impact_radius_lookup for 8 symbols ---
    for symbol in _IMPACT_RADIUS_SYMBOLS:
        tasks.append({
            "id": next_id(),
            "category": "impact_radius_lookup",
            "namespace": "source",
            "action": "impact_radius",
            "tool": "source_query",
            "arguments": {"action": "impact_radius", "symbol": symbol},
            "expected": {"min_results": 0, "required_fields": ["name", "kind", "file_path"]},
            "safety": "read_only",
        })

    # --- ergonomics_text: get_include_path ---
    for symbol in _ERGONOMICS_INCLUDE_SYMBOLS:
        tasks.append({
            "id": next_id(),
            "category": "ergonomics_text",
            "namespace": "source",
            "action": "get_include_path",
            "tool": "source_query",
            "arguments": {"action": "get_include_path", "symbol": symbol},
            "expected": {"non_empty_response": True},
            "safety": "read_only",
        })

    # --- ergonomics_text: get_signature ---
    for method in _ERGONOMICS_SIGNATURE_METHODS:
        tasks.append({
            "id": next_id(),
            "category": "ergonomics_text",
            "namespace": "source",
            "action": "get_signature",
            "tool": "source_query",
            "arguments": {"action": "get_signature", "symbol": method},
            "expected": {"non_empty_response": True},
            "safety": "read_only",
        })

    # --- ergonomics_text: check_deprecations ---
    for symbols_list in _ERGONOMICS_DEPRECATION_LISTS:
        tasks.append({
            "id": next_id(),
            "category": "ergonomics_text",
            "namespace": "source",
            "action": "check_deprecations",
            "tool": "source_query",
            "arguments": {"action": "check_deprecations", "symbols": symbols_list},
            "expected": {"non_empty_response": True},
            "safety": "read_only",
        })

    # --- ergonomics_text: verify_symbols ---
    for symbols_list in _ERGONOMICS_VERIFY_LISTS:
        tasks.append({
            "id": next_id(),
            "category": "ergonomics_text",
            "namespace": "source",
            "action": "verify_symbols",
            "tool": "source_query",
            "arguments": {"action": "verify_symbols", "symbols": symbols_list},
            "expected": {"non_empty_response": True},
            "safety": "read_only",
        })

    # --- ergonomics_text: find_example_usage ---
    for method, limit in _ERGONOMICS_EXAMPLE_USAGE:
        tasks.append({
            "id": next_id(),
            "category": "ergonomics_text",
            "namespace": "source",
            "action": "find_example_usage",
            "tool": "source_query",
            "arguments": {"action": "find_example_usage", "symbol": method, "limit": limit},
            "expected": {"non_empty_response": True},
            "safety": "read_only",
        })

    # --- health_check: plain health ---
    tasks.append({
        "id": next_id(),
        "category": "health_check",
        "namespace": "source",
        "action": "health",
        "tool": "source_query",
        "arguments": {"action": "health"},
        "expected": {"fields": ["status", "row_counts"]},
        "safety": "read_only",
    })

    # --- health_check: health with include_counts ---
    tasks.append({
        "id": next_id(),
        "category": "health_check",
        "namespace": "source",
        "action": "health",
        "tool": "source_query",
        "arguments": {"action": "health", "include_counts": True},
        "expected": {"fields": ["status", "row_counts"]},
        "safety": "read_only",
    })

    # --- health_check: health with detail=summary ---
    tasks.append({
        "id": next_id(),
        "category": "health_check",
        "namespace": "source",
        "action": "health",
        "tool": "source_query",
        "arguments": {"action": "health", "detail": "summary"},
        "expected": {"fields": ["status"]},
        "safety": "read_only",
    })

    # --- health_check: health with include_counts=False ---
    tasks.append({
        "id": next_id(),
        "category": "health_check",
        "namespace": "source",
        "action": "health",
        "tool": "source_query",
        "arguments": {"action": "health", "include_counts": False},
        "expected": {"fields": ["status"]},
        "safety": "read_only",
    })

    # --- schema_field_presence: discover schema for all SCHEMA_ACTIONS ---
    for action in SCHEMA_ACTIONS:
        tasks.append({
            "id": next_id(),
            "category": "schema_field_presence",
            "namespace": "source",
            "action": action,
            "tool": "monolith_discover",
            "arguments": {"namespace": "source", "action": action, "mode": "schema"},
            "expected": {"requires_planning_signals": True, "requires_skill": True, "requires_status_declared": True},
            "safety": "read_only_discovery",
        })

    return tasks


def generate_tasks(url: str, min_tasks: int, tasks_path: pathlib.Path, manifest_path: pathlib.Path) -> Dict[str, Any]:
    """Generate task fixtures from golden symbols plus live discover for additional coverage."""
    tasks = build_static_tasks()

    # Top up from live catalog if needed.
    if len(tasks) < min_tasks:
        response = mcp_call(url, "monolith_discover", {"namespace": "source", "mode": "actions"}, timeout_s=45.0)
        data = result_data(response)
        live_actions: List[str] = []
        actions_list = data.get("actions")
        if isinstance(actions_list, list):
            for row in actions_list:
                if isinstance(row, dict) and isinstance(row.get("action"), str):
                    live_actions.append(row["action"])
                elif isinstance(row, str):
                    live_actions.append(row)

        existing_lookup_actions = {t["action"] for t in tasks if t["category"] == "symbol_lookup"}
        for action in live_actions:
            if action in existing_lookup_actions:
                continue
            for symbol in GOLDEN_SYMBOLS[:2]:
                if len(tasks) >= min_tasks:
                    break
                tasks.append({
                    "id": f"SIB-{len(tasks) + 1:03d}",
                    "category": "symbol_lookup",
                    "namespace": "source",
                    "action": action,
                    "tool": "source_query",
                    "arguments": {"action": action, "query": symbol},
                    "expected": {"min_results": 0, "required_fields": ["name", "kind", "file_path"]},
                    "safety": "read_only",
                })
            if len(tasks) >= min_tasks:
                break

    # Re-assign IDs to be monotonic after any additions.
    for index, task in enumerate(tasks, 1):
        task["id"] = f"SIB-{index:03d}"

    write_jsonl(tasks_path, tasks)

    manifest = {
        "generated_at": utc_now(),
        "mcp_url": url,
        "task_count": len(tasks),
        "min_tasks_requested": min_tasks,
        "golden_symbols": GOLDEN_SYMBOLS,
        "schema_actions": SCHEMA_ACTIONS,
        "category_counts": count_by(tasks, "category"),
        "task_file": str(tasks_path).replace("\\", "/"),
        "scoring": {
            "source_index_score": (
                "0.35 * symbol_hit_rate"
                " + 0.25 * field_completeness_rate"
                " + 0.20 * schema_adherence_rate"
                " + 0.10 * (1 - stale_rate)"
                " + 0.10 * ergonomics_success_rate"
            ),
            "symbol_hit_rate": "fraction of symbol_lookup + review_context_lookup + impact_radius_lookup tasks with direct_success",
            "field_completeness_rate": "fraction of all symbol results with >=3 required fields (name, kind, file_path, location)",
            "schema_adherence_rate": "fraction of schema_field_presence tasks with planning_signals + skill + status declared",
            "stale_rate": "fraction of health_check tasks with stale/error/missing-fields response",
            "ergonomics_success_rate": "fraction of ergonomics_text tasks with non-empty, non-error response",
            "mean_results_per_lookup": "average result count across symbol_lookup + review_context_lookup + impact_radius_lookup tasks",
        },
    }
    write_json(manifest_path, manifest)
    return manifest


# ---------------------------------------------------------------------------
# Run
# ---------------------------------------------------------------------------

def run_benchmark(
    url: str,
    tasks_path: pathlib.Path,
    output_dir: pathlib.Path,
    label: str,
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
        row = score_task(url, task, timeout_s)
        rows.append(row)
        with per_task_jsonl.open("a", encoding="utf-8", newline="\n") as handle:
            handle.write(json.dumps(row, ensure_ascii=False, sort_keys=True))
            handle.write("\n")
        print(
            f"[{index}/{len(tasks)}] {row['task_id']} success={row['direct_success']} direct={row['direct_success']}",
            flush=True,
        )
        if index == 1 or index == len(tasks) or index % 10 == 0:
            partial = aggregate(label, status, tasks[:index], rows)
            partial["completed_task_count"] = index
            partial["total_task_count"] = len(tasks)
            write_json(output_dir / "partial_summary.json", partial)

    summary = aggregate(label, status, tasks, rows)
    write_json(output_dir / "summary.json", summary)
    write_json(output_dir / "per_task.json", rows)
    return summary


# ---------------------------------------------------------------------------
# Compare
# ---------------------------------------------------------------------------

def compare_runs(baseline_path: pathlib.Path, current_path: pathlib.Path, output_dir: pathlib.Path) -> Dict[str, Any]:
    baseline = json.loads(baseline_path.read_text(encoding="utf-8"))
    current = json.loads(current_path.read_text(encoding="utf-8"))
    base_metrics = baseline["metrics"]
    cur_metrics = current["metrics"]
    deltas: Dict[str, float] = {}
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
        "source_index_score",
        "symbol_hit_rate",
        "field_completeness_rate",
        "schema_adherence_rate",
        "stale_rate",
        "mean_results_per_lookup",
    ]
    lines = [
        "# Monolith SourceIndex Benchmark Comparison",
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
    lines.append("Higher is better for all metrics except `stale_rate`.")
    path.write_text("\n".join(lines) + "\n", encoding="utf-8", newline="\n")


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------

def main(argv: Optional[List[str]] = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="cmd", required=True)

    gen = sub.add_parser("generate", help="Generate task fixtures from golden symbols and live catalog")
    gen.add_argument("--mcp-url", default=DEFAULT_MCP_URL)
    gen.add_argument("--tasks", type=pathlib.Path, default=DEFAULT_TASKS)
    gen.add_argument("--manifest", type=pathlib.Path, default=DEFAULT_MANIFEST)
    gen.add_argument("--min-tasks", type=int, default=30)

    run_cmd = sub.add_parser("run", help="Run tasks against a live MCP endpoint and score results")
    run_cmd.add_argument("--mcp-url", default=DEFAULT_MCP_URL)
    run_cmd.add_argument("--tasks", type=pathlib.Path, default=DEFAULT_TASKS)
    run_cmd.add_argument("--output-dir", type=pathlib.Path, required=True)
    run_cmd.add_argument("--label", required=True)
    run_cmd.add_argument("--request-timeout-s", type=float, default=12.0)

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
        summary = run_benchmark(args.mcp_url, args.tasks, args.output_dir, args.label, args.request_timeout_s)
        print(json.dumps(summary, indent=2, ensure_ascii=False))
        return 0

    if args.cmd == "compare":
        comparison = compare_runs(args.baseline, args.current, args.output_dir)
        print(json.dumps({"output_dir": str(args.output_dir), "deltas": comparison["deltas"]}, indent=2))
        return 0

    return 2


if __name__ == "__main__":
    raise SystemExit(main())
