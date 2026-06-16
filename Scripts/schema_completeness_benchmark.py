#!/usr/bin/env python3
"""
Monolith MCP schema-completeness benchmark.

Scans the entire action catalog (all namespaces, all actions) and scores each
action's schema for five quality dimensions.  Unlike ActionGuidance which samples
161 tasks, this benchmark covers all 1766 actions across all 51 namespaces.

Subcommands
-----------
scan    Connect to a live MCP endpoint, discover every action, score schema
        quality for each, and write result files to --output-dir.
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
DEFAULT_PROBE_SET = "Plugins/Monolith/Benchmarks/SchemaCompleteness/probe_set.jsonl"
PARTIAL_FLUSH_EVERY = 25

# Score weights (must sum to 1.0)
W_PARAM_TYPES = 0.30
W_REQUIRED_PARAMS = 0.25
W_PLANNING_SIGNALS = 0.20
W_SKILL_ROUTING = 0.15
W_OUTPUT_CONTRACT = 0.10


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


def mcp_call(url: str, tool: str, arguments: Dict[str, Any], timeout_s: float = 8.0) -> Dict[str, Any]:
    body = {
        "jsonrpc": "2.0",
        "id": int(time.time() * 1000) % 1_000_000_000,
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
        return {"transport_error": True, "status": exc.code, "raw": raw}
    except (TimeoutError, socket.timeout) as exc:
        return {"transport_error": True, "status": None, "raw": f"timeout: {exc}"}
    except urllib.error.URLError as exc:
        return {"transport_error": True, "status": None, "raw": str(exc)}

    raw = extract_sse_data(raw)
    try:
        parsed = json.loads(raw)
    except json.JSONDecodeError:
        parsed = {"parse_error": True, "raw": raw}
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


# ---------------------------------------------------------------------------
# Catalog discovery
# ---------------------------------------------------------------------------

def discover_namespaces(url: str, timeout_s: float) -> List[Dict[str, Any]]:
    """Return list of {namespace, actions} dicts from monolith_discover({})."""
    response = mcp_call(url, "monolith_discover", {}, timeout_s=timeout_s)
    data = result_data(response)
    if not data or "namespaces" not in data:
        raise RuntimeError(
            "monolith_discover({}) did not return namespaces. "
            f"raw response snippet: {str(response)[:400]}"
        )
    namespaces = [row for row in data["namespaces"] if isinstance(row, dict)]
    return namespaces


def discover_schema_for_action(url: str, namespace: str, action: str, timeout_s: float) -> Optional[Dict[str, Any]]:
    """Return the schema dict for a single action, or None on failure."""
    response = mcp_call(
        url,
        "monolith_discover",
        {"namespace": namespace, "action": action, "mode": "schema"},
        timeout_s=timeout_s,
    )
    if response.get("transport_error"):
        return None
    data = result_data(response)
    if not data:
        return None
    schema = data.get("schema")
    return schema if isinstance(schema, dict) else None


# ---------------------------------------------------------------------------
# Schema quality scoring
# ---------------------------------------------------------------------------

def score_schema_quality(schema: Optional[Dict[str, Any]]) -> Dict[str, Any]:
    """
    Score one action's schema for five quality dimensions.

    Returns a dict with five boolean flags and a float schema_score (0.0-1.0).
    If schema is None (transport/parse failure), all flags are False.
    """
    if schema is None:
        return {
            "param_types_declared": False,
            "required_params_marked": False,
            "planning_signals_present": False,
            "skill_routing_present": False,
            "output_contract_declared": False,
            "schema_score": 0.0,
        }

    # 1. param_types_declared: all params that exist have a "type" field.
    params = schema.get("params")
    if isinstance(params, dict):
        user_params = {k: v for k, v in params.items() if not k.startswith("_") and isinstance(v, dict)}
        if user_params:
            param_types_declared = all("type" in meta for meta in user_params.values())
        else:
            # no params — vacuously true
            param_types_declared = True
    else:
        # missing params key — treat as vacuously true (action may have no params)
        param_types_declared = True

    # 2. required_params_marked: at least one param has "required": true OR no params.
    if isinstance(params, dict):
        user_params = {k: v for k, v in params.items() if not k.startswith("_") and isinstance(v, dict)}
        if user_params:
            required_params_marked = any(bool(meta.get("required")) for meta in user_params.values())
        else:
            required_params_marked = True
    else:
        required_params_marked = True

    # 3. planning_signals_present: "planning_signals" key exists and is a non-empty list.
    planning_signals = schema.get("planning_signals")
    planning_signals_present = isinstance(planning_signals, list) and len(planning_signals) > 0

    # 4. skill_routing_present: "skill" key exists and is a non-empty string.
    skill = schema.get("skill")
    skill_routing_present = isinstance(skill, str) and bool(skill.strip())

    # 5. output_contract_declared: "output_contract_status" is explicitly set to
    #    "declared" or "not_declared" (not absent).
    output_contract_status = schema.get("output_contract_status")
    output_contract_declared = output_contract_status in ("declared", "not_declared")

    flags = [
        param_types_declared,
        required_params_marked,
        planning_signals_present,
        skill_routing_present,
        output_contract_declared,
    ]
    schema_score = round(sum(1.0 for f in flags if f) / len(flags), 6)

    return {
        "param_types_declared": param_types_declared,
        "required_params_marked": required_params_marked,
        "planning_signals_present": planning_signals_present,
        "skill_routing_present": skill_routing_present,
        "output_contract_declared": output_contract_declared,
        "schema_score": schema_score,
    }


# ---------------------------------------------------------------------------
# Aggregate metrics
# ---------------------------------------------------------------------------

def avg(values: List[float]) -> float:
    return sum(values) / len(values) if values else 0.0


def aggregate_metrics(label: str, rows: List[Dict[str, Any]], total_expected: int, ns_breakdown: Dict[str, Any]) -> Dict[str, Any]:
    """
    Compute aggregate schema_completeness_score and per-dimension rates.

    schema_completeness_score =
        0.30 * param_types_declared_rate
      + 0.25 * required_params_marked_rate
      + 0.20 * planning_signals_present_rate
      + 0.15 * skill_routing_present_rate
      + 0.10 * output_contract_declared_rate
    """
    if not rows:
        zero = {
            "schema_completeness_score": 0.0,
            "param_types_declared_rate": 0.0,
            "required_params_marked_rate": 0.0,
            "planning_signals_present_rate": 0.0,
            "skill_routing_present_rate": 0.0,
            "output_contract_declared_rate": 0.0,
            "mean_schema_score": 0.0,
            "failed_action_count": 0,
            "scanned_action_count": 0,
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
    scored = [r for r in rows if not r.get("error")]

    def rate(field: str) -> float:
        vals = [1.0 if r.get(field) else 0.0 for r in rows]
        return avg(vals)

    ptd = rate("param_types_declared")
    rpm = rate("required_params_marked")
    psp = rate("planning_signals_present")
    srp = rate("skill_routing_present")
    ocd = rate("output_contract_declared")

    schema_completeness_score = (
        W_PARAM_TYPES * ptd
        + W_REQUIRED_PARAMS * rpm
        + W_PLANNING_SIGNALS * psp
        + W_SKILL_ROUTING * srp
        + W_OUTPUT_CONTRACT * ocd
    )

    mean_schema_score = avg([float(r.get("schema_score", 0.0)) for r in rows])
    namespaces = {r.get("namespace") for r in rows if r.get("namespace")}

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
            "planning_signals_present_rate": round(psp, 6),
            "skill_routing_present_rate": round(srp, 6),
            "output_contract_declared_rate": round(ocd, 6),
            "mean_schema_score": round(mean_schema_score, 6),
            "failed_action_count": len(failed),
            "scanned_action_count": len(rows),
        },
        "namespace_breakdown": ns_breakdown,
    }


def build_namespace_breakdown(rows: List[Dict[str, Any]]) -> Dict[str, Any]:
    """Return per-namespace aggregated quality metrics."""
    by_ns: Dict[str, List[Dict[str, Any]]] = {}
    for row in rows:
        ns = str(row.get("namespace", "unknown"))
        by_ns.setdefault(ns, []).append(row)

    breakdown: Dict[str, Any] = {}
    for ns, ns_rows in sorted(by_ns.items()):
        failed_count = sum(1 for r in ns_rows if r.get("error"))
        scored_count = len(ns_rows) - failed_count

        def ns_rate(field: str) -> float:
            vals = [1.0 if r.get(field) else 0.0 for r in ns_rows]
            return round(avg(vals), 6)

        ptd = ns_rate("param_types_declared")
        rpm = ns_rate("required_params_marked")
        psp = ns_rate("planning_signals_present")
        srp = ns_rate("skill_routing_present")
        ocd = ns_rate("output_contract_declared")
        ns_score = round(
            W_PARAM_TYPES * ptd
            + W_REQUIRED_PARAMS * rpm
            + W_PLANNING_SIGNALS * psp
            + W_SKILL_ROUTING * srp
            + W_OUTPUT_CONTRACT * ocd,
            6,
        )
        breakdown[ns] = {
            "action_count": len(ns_rows),
            "failed_count": failed_count,
            "schema_completeness_score": ns_score,
            "param_types_declared_rate": ptd,
            "required_params_marked_rate": rpm,
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

    output_dir.mkdir(parents=True, exist_ok=True)

    print(f"[schema_completeness] scan started  label={label}  url={url}", flush=True)

    # Step 1: discover all namespaces and their action lists
    print("[schema_completeness] calling monolith_discover({}) ...", flush=True)
    try:
        namespaces = discover_namespaces(url, timeout_s=max(timeout_s, 20.0))
    except RuntimeError as exc:
        print(f"[schema_completeness] ERROR: {exc}", file=sys.stderr)
        return 1

    # Build flat list of (namespace, action) pairs
    all_pairs: List[Tuple[str, str]] = []
    for ns_row in namespaces:
        ns = str(ns_row.get("namespace", ""))
        actions = ns_row.get("actions", [])
        if not ns:
            continue
        if isinstance(actions, list):
            for act in actions:
                if isinstance(act, dict):
                    act = act.get("action", "")
                act = str(act).strip()
                if act:
                    all_pairs.append((ns, act))

    total = len(all_pairs)
    if max_actions is not None and max_actions > 0:
        all_pairs = all_pairs[:max_actions]
        print(f"[schema_completeness] --max-actions={max_actions}: scanning {len(all_pairs)}/{total} actions", flush=True)
        total = len(all_pairs)
    else:
        print(f"[schema_completeness] discovered {total} actions across {len(namespaces)} namespaces", flush=True)

    # Step 2: scan each action
    per_action_path = output_dir / "per_action.jsonl"
    if per_action_path.exists():
        per_action_path.unlink()

    rows: List[Dict[str, Any]] = []
    for index, (ns, act) in enumerate(all_pairs, 1):
        print(f"[{ns}.{act} {index}/{total}]", flush=True)

        error_msg: str = ""
        quality: Dict[str, Any] = {}
        schema: Optional[Dict[str, Any]] = None

        try:
            schema = discover_schema_for_action(url, ns, act, timeout_s=timeout_s)
            if schema is None:
                error_msg = "schema_not_returned"
        except Exception as exc:  # noqa: BLE001
            error_msg = f"exception: {exc}"

        quality = score_schema_quality(schema if not error_msg else None)

        row: Dict[str, Any] = {
            "namespace": ns,
            "action": act,
            "param_types_declared": quality["param_types_declared"],
            "required_params_marked": quality["required_params_marked"],
            "planning_signals_present": quality["planning_signals_present"],
            "skill_routing_present": quality["skill_routing_present"],
            "output_contract_declared": quality["output_contract_declared"],
            "schema_score": quality["schema_score"],
            "error": error_msg,
        }
        rows.append(row)
        append_jsonl_row(per_action_path, row)

        # Flush partial summary every PARTIAL_FLUSH_EVERY actions
        if index % PARTIAL_FLUSH_EVERY == 0 or index == total:
            ns_breakdown = build_namespace_breakdown(rows)
            partial = aggregate_metrics(label, rows, total, ns_breakdown)
            partial["completed_action_count"] = index
            partial["total_action_count"] = total
            write_json(output_dir / "partial_summary.json", partial)

    # Step 3: write final output files
    ns_breakdown = build_namespace_breakdown(rows)
    summary = aggregate_metrics(label, rows, total, ns_breakdown)
    write_json(output_dir / "summary.json", summary)
    write_json(output_dir / "namespace_breakdown.json", ns_breakdown)

    # Re-write per_action.jsonl as a clean write (already appended above, but ensure order)
    write_jsonl(per_action_path, rows)

    score = summary["metrics"]["schema_completeness_score"]
    print(
        f"[schema_completeness] scan complete  actions={len(rows)}  failed={summary['failed_action_count']}"
        f"  schema_completeness_score={score:.4f}",
        flush=True,
    )
    print(json.dumps(summary, indent=2, ensure_ascii=False))
    return 0


# ---------------------------------------------------------------------------
# Probe subcommand
# ---------------------------------------------------------------------------

ALL_DIMENSIONS = [
    "param_types_declared",
    "required_params_marked",
    "planning_signals_present",
    "skill_routing_present",
    "output_contract_declared",
]


def load_probe_set(probe_set_path: pathlib.Path) -> List[Dict[str, Any]]:
    """Load probe entries from a JSONL file.  Skips blank lines and parse errors."""
    probes: List[Dict[str, Any]] = []
    with probe_set_path.open(encoding="utf-8") as fh:
        for lineno, line in enumerate(fh, 1):
            line = line.strip()
            if not line:
                continue
            try:
                entry = json.loads(line)
            except json.JSONDecodeError as exc:
                print(
                    f"[schema_completeness] WARNING: probe_set line {lineno} skipped (JSON error: {exc})",
                    file=sys.stderr,
                )
                continue
            if not isinstance(entry, dict):
                continue
            ns = str(entry.get("namespace", "")).strip()
            act = str(entry.get("action", "")).strip()
            if not ns or not act:
                print(
                    f"[schema_completeness] WARNING: probe_set line {lineno} skipped (missing namespace or action)",
                    file=sys.stderr,
                )
                continue
            probes.append(entry)
    return probes


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
        if priority_filter is not None and r.get("priority") != priority_filter:
            continue
        for dim in r.get("expected_dimensions", []):
            total += 1
            if r.get("dimension_results", {}).get(dim):
                passed += 1
    rate = round(passed / total, 6) if total > 0 else 0.0
    return rate, passed, total


def cmd_probe(args: argparse.Namespace) -> int:
    probe_set_path: pathlib.Path = args.probe_set
    url: str = args.mcp_url
    output_dir: pathlib.Path = args.output_dir
    label: str = args.label
    timeout_s: float = args.request_timeout_s

    output_dir.mkdir(parents=True, exist_ok=True)

    print(
        f"[schema_completeness] probe started  label={label}  url={url}  probe_set={probe_set_path}",
        flush=True,
    )

    if not probe_set_path.exists():
        print(f"[schema_completeness] ERROR: probe_set not found: {probe_set_path}", file=sys.stderr)
        return 1

    probes = load_probe_set(probe_set_path)
    if not probes:
        print("[schema_completeness] ERROR: probe_set is empty or all entries were invalid", file=sys.stderr)
        return 1

    total = len(probes)
    print(f"[schema_completeness] loaded {total} probes from {probe_set_path}", flush=True)

    # Output file paths
    per_action_path = output_dir / "per_action.jsonl"
    probe_results_path = output_dir / "probe_results.jsonl"
    if per_action_path.exists():
        per_action_path.unlink()
    if probe_results_path.exists():
        probe_results_path.unlink()

    rows: List[Dict[str, Any]] = []          # same schema as scan rows
    probe_results: List[Dict[str, Any]] = []  # probe-specific per-entry results

    for index, probe in enumerate(probes, 1):
        ns = str(probe.get("namespace", ""))
        act = str(probe.get("action", ""))
        priority = str(probe.get("priority", "medium"))
        expected_dims: List[str] = [
            d for d in probe.get("expected_dimensions", []) if d in ALL_DIMENSIONS
        ]
        rationale = str(probe.get("rationale", ""))

        print(f"[{ns}.{act} {index}/{total}]", flush=True)

        error_msg: str = ""
        schema: Optional[Dict[str, Any]] = None

        try:
            schema = discover_schema_for_action(url, ns, act, timeout_s=timeout_s)
            if schema is None:
                error_msg = "schema_not_returned"
        except Exception as exc:  # noqa: BLE001
            error_msg = f"exception: {exc}"

        quality = score_schema_quality(schema if not error_msg else None)

        # Per-action row (compatible with scan output)
        row: Dict[str, Any] = {
            "namespace": ns,
            "action": act,
            "param_types_declared": quality["param_types_declared"],
            "required_params_marked": quality["required_params_marked"],
            "planning_signals_present": quality["planning_signals_present"],
            "skill_routing_present": quality["skill_routing_present"],
            "output_contract_declared": quality["output_contract_declared"],
            "schema_score": quality["schema_score"],
            "error": error_msg,
        }
        rows.append(row)
        append_jsonl_row(per_action_path, row)

        # Probe-specific result
        dim_results: Dict[str, bool] = {
            dim: bool(quality.get(dim, False)) for dim in ALL_DIMENSIONS
        }
        expected_passed = [d for d in expected_dims if dim_results.get(d, False)]
        expected_failed = [d for d in expected_dims if not dim_results.get(d, False)]
        probe_pass = len(expected_failed) == 0 and bool(expected_dims)

        probe_result: Dict[str, Any] = {
            "namespace": ns,
            "action": act,
            "priority": priority,
            "rationale": rationale,
            "expected_dimensions": expected_dims,
            "dimension_results": dim_results,
            "expected_passed": expected_passed,
            "expected_failed": expected_failed,
            "probe_pass": probe_pass,
            "error": error_msg,
        }
        probe_results.append(probe_result)
        append_jsonl_row(probe_results_path, probe_result)

        # Flush partial summary every PARTIAL_FLUSH_EVERY actions
        if index % PARTIAL_FLUSH_EVERY == 0 or index == total:
            ns_breakdown = build_namespace_breakdown(rows)
            partial = aggregate_metrics(label, rows, total, ns_breakdown)
            partial["completed_action_count"] = index
            partial["total_action_count"] = total
            # Add probe rates to partial
            ppr, _, _ = _probe_pass_rates(probe_results)
            cpr, _, _ = _probe_pass_rates(probe_results, priority_filter="critical")
            hpr, _, _ = _probe_pass_rates(probe_results, priority_filter="high")
            failed_probe_count = sum(1 for r in probe_results if r.get("expected_failed"))
            partial["probe_metrics"] = {
                "probe_pass_rate": ppr,
                "critical_probe_pass_rate": cpr,
                "high_probe_pass_rate": hpr,
                "failed_probe_count": failed_probe_count,
            }
            write_json(output_dir / "partial_summary.json", partial)

    # Write final output files
    ns_breakdown = build_namespace_breakdown(rows)
    summary = aggregate_metrics(label, rows, total, ns_breakdown)

    # Compute final probe metrics
    probe_pass_rate, passed_checks, total_checks = _probe_pass_rates(probe_results)
    critical_probe_pass_rate, _, _ = _probe_pass_rates(probe_results, priority_filter="critical")
    high_probe_pass_rate, _, _ = _probe_pass_rates(probe_results, priority_filter="high")
    failed_probe_count = sum(1 for r in probe_results if r.get("expected_failed"))

    summary["probe_metrics"] = {
        "probe_set_file": str(probe_set_path),
        "probe_count": total,
        "probe_pass_rate": probe_pass_rate,
        "critical_probe_pass_rate": critical_probe_pass_rate,
        "high_probe_pass_rate": high_probe_pass_rate,
        "failed_probe_count": failed_probe_count,
        "passed_dimension_checks": passed_checks,
        "total_dimension_checks": total_checks,
    }

    write_json(output_dir / "summary.json", summary)
    write_json(output_dir / "namespace_breakdown.json", ns_breakdown)
    write_jsonl(per_action_path, rows)
    # probe_results.jsonl is already appended; rewrite for clean sort order
    write_jsonl(probe_results_path, probe_results)

    score = summary["metrics"]["schema_completeness_score"]
    print(
        f"[schema_completeness] probe complete"
        f"  probes={total}"
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
    print(f"  planning_signals_rate      : {metrics.get('planning_signals_present_rate', 'N/A')}")
    print(f"  skill_routing_rate         : {metrics.get('skill_routing_present_rate', 'N/A')}")
    print(f"  output_contract_rate       : {metrics.get('output_contract_declared_rate', 'N/A')}")
    print(f"  mean_schema_score          : {metrics.get('mean_schema_score', 'N/A')}")
    print()

    if ns_breakdown:
        col_w = max((len(ns) for ns in ns_breakdown), default=10)
        header = f"  {'Namespace':<{col_w}}  {'Actions':>7}  {'Failed':>6}  {'Score':>6}  {'PTypes':>6}  {'ReqPrm':>6}  {'Signals':>7}  {'Skill':>6}  {'OutCtx':>6}"
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
