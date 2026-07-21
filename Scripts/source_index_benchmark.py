#!/usr/bin/env python3
"""
Monolith MCP SourceIndex Quality benchmark.

Measures whether the source namespace (C++ symbol index) returns data that is
rich enough for agents to get useful code context.  The benchmark is
deterministic and does not call an LLM.

Seven task categories:
  symbol_lookup          - search_source / find_callers / find_callees / risk_score for engine symbols.
  review_context_lookup  - review_context for engine symbols (scored like symbol_lookup).
  impact_radius_lookup   - impact_radius for engine symbols (scored like symbol_lookup).
  ergonomics_text        - get_include_path / get_signature / check_deprecations / verify_symbols /
                           find_example_usage - plain-text ergonomic tools.
  negative_recovery      - deliberately bad input must return identifier-specific recovery guidance.
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
DEFAULT_TASKS = pathlib.Path("Benchmarks/SourceIndex/tasks.jsonl")
DEFAULT_MANIFEST = pathlib.Path("Benchmarks/SourceIndex/manifest.json")
DEFAULT_RESULTS_ROOT = pathlib.Path("Saved/Monolith/Benchmarks/SourceIndex")

RUN_OUTPUT_FILENAMES = (
    "summary.json",
    "partial_summary.json",
    "per_task.json",
    "per_task.jsonl",
    "run_failure.json",
)

SOURCE_INDEX_TASK_CATEGORIES = {
    "ergonomics_text",
    "health_check",
    "impact_radius_lookup",
    "negative_recovery",
    "review_context_lookup",
    "schema_field_presence",
    "symbol_lookup",
}

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
    "search_crg_graph",
]
REQUIRED_RESULT_FIELDS = {"name", "kind", "file_path", "location"}

# Sentinel substrings the source handlers emit for a *successfully executed* lookup
# whose truthful answer is "no rows" (e.g. find_callers/find_callees/search_source
# return isError=false with one of these phrases).  A require_results task must NOT
# match any of these — an empty-but-non-error response is the exact loophole this
# benchmark closes, so it counts as a miss, not a hit.
_EMPTY_RESULT_SENTINELS = (
    "no direct c++ callers found",
    "no callees found",
    "no results found",
    "no function found matching",
    "no symbol found matching",
    "no matches",
    "no impacted symbols",
    "no overrides found",
    "not found in the source index",
)

# Symbols whose definition is GUARANTEED indexed in EngineSource.db: search_source /
# risk_score / review_context / impact_radius must return >=1 row for these, so an
# empty response is a real index defect (require_results gate, not min_results:0).
_KNOWN_DEFINED_SYMBOLS = [
    "AActor", "UObject", "UActorComponent", "UGameplayStatics", "ACharacter",
    "UWorld", "USceneComponent", "APlayerController",
]

# Qualified method symbols KNOWN to have C++ callers AND callees inside the engine
# tree (every gameplay/component lifecycle hook is invoked by the framework).  An
# empty find_callers/find_callees here is the 72%-live-failure the headline must
# reflect, so these carry require_results.
# Qualified Class::Method names with REAL direct C++ callers (verified live against EngineSource).
# The previous set (AActor::BeginPlay/Tick/EndPlay, UActorComponent::*, UWorld::SpawnActor) were all
# virtual overrides dispatched by the engine framework via vtable/reflection, so find_callers
# (direct C++ callers) legitimately returns none — the benchmark was asserting callers that cannot
# exist. These are concrete functions reached by direct calls, and also exercise the 2026-06-18
# find_callers/find_callees qualified-name fix (before it, every Class::Method input returned
# "No function found matching ...").
_KNOWN_CALLED_METHODS = [
    "FString::Printf",            # ~34 direct callers
    "UObject::GetName",           # ~33
    "UObject::GetClass",          # ~13
    "UWorld::GetTimerManager",    # ~4
    "UWorld::GetGameInstance",    # ~2
    "UObject::StaticClass",       # >0
]

# Negative / error-recovery inputs.  Each is deliberately malformed; the benchmark
# scores RESPONSE QUALITY (structured error that names the offending identifier +
# a did-you-mean/qualified-symbol hint), not transport success.  `offending_identifier`
# is the token the response must echo back so the agent can correct the call.
_NEGATIVE_NONEXISTENT_SYMBOLS = [
    ("search_source", "query", "UTotallyMadeUpClass_ZZZ999"),
    ("find_callers", "symbol", "UNonExistentClass999::DoesNotExist"),
    ("find_callees", "symbol", "UNonExistentClass999::DoesNotExist"),
    ("get_include_path", "symbol", "UTotallyMadeUpClass_ZZZ999"),
    ("get_signature", "symbol", "UTotallyMadeUpClass_ZZZ999::Method"),
    ("risk_score", "symbol", "UTotallyMadeUpClass_ZZZ999"),
    ("review_context", "symbol", "UTotallyMadeUpClass_ZZZ999"),
    ("impact_radius", "symbol", "UTotallyMadeUpClass_ZZZ999"),
]

# Unqualified method names that resolution should still handle (or reject with a
# qualified-symbol hint).  `expect_error: false` — a populated answer is the pass.
_NEGATIVE_UNQUALIFIED_SYMBOLS = [
    ("find_callers", "symbol", "BeginPlay"),
    ("find_callees", "symbol", "Tick"),
]


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


def clear_run_outputs(output_dir: pathlib.Path) -> None:
    """Remove only known run outputs so a failed rerun cannot expose stale success."""
    for filename in RUN_OUTPUT_FILENAMES:
        path = output_dir / filename
        if path.exists():
            path.unlink()


def write_run_failure(output_dir: pathlib.Path, payload: Dict[str, Any]) -> None:
    """Persist one machine-readable invalid-run record; never write summary.json."""
    payload.setdefault("created_at", utc_now())
    payload["run_valid"] = False
    payload["metrics_valid"] = False
    write_json(output_dir / "run_failure.json", payload)


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
_BENCHMARK_ROUTING_CONTEXT = benchmark_routing_context("SourceIndex")


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


def canonical_payload(response: Dict[str, Any]) -> Dict[str, Any]:
    """Return the ACTION's own payload, never the transport envelope.

    With ``bEnableStructuredToolResults`` the payload lives ONLY in
    ``result.structuredContent`` while ``content[0].text`` is a constant stub
    ("OK; see structuredContent.").  An *empty* ``structuredContent`` therefore means
    an empty payload and must NOT fall back to the envelope -- that fallback is how
    the stub would leak into content assertions and let a data-less response pass.
    In legacy mode the same payload object is serialized into ``content[0].text`` and
    ``result_data`` parses it back, so both modes resolve to the same object.
    """
    payload = result_payload(response)
    if "structuredContent" in payload:
        return structured_content(payload)
    return result_data(response)


def response_scan_text(response: Dict[str, Any]) -> str:
    """Scannable blob over the CANONICAL payload: human text + structuredContent.

    Every token assertion (empty-result sentinels, the offending identifier, recovery
    hints) must scan the canonical payload rather than the transport stub.  Both
    surfaces are unioned, so the scan is strictly stronger than the legacy text-only
    scan: an error message keeps its human text AND gains the structured error object.
    """
    text = result_text(response)
    structured = structured_content(result_payload(response))
    if structured:
        return text + "\n" + json.dumps(structured, ensure_ascii=False, sort_keys=True)
    return text


def classify_protocol_failure(response: Any) -> str:
    """Classify malformed JSON-RPC/MCP envelopes without conflating valid isError results."""
    return classify_mcp_protocol_failure(response)


def validate_status_response(response: Any) -> Dict[str, Any]:
    """Validate the mandatory status boundary before any scored task executes."""
    return validate_mcp_status_response(
        response,
        result_payload=result_payload,
        result_data=result_data,
    )


def count_by(rows: Iterable[Dict[str, Any]], field: str) -> Dict[str, int]:
    out: Dict[str, int] = {}
    for row in rows:
        key = str(row.get(field, ""))
        out[key] = out.get(key, 0) + 1
    return dict(sorted(out.items()))


def avg(values: List[float]) -> float:
    return sum(values) / len(values) if values else 0.0


def task_fingerprint(task: Dict[str, Any]) -> str:
    payload = {
        "category": task.get("category"),
        "namespace": task.get("namespace"),
        "action": task.get("action"),
        "tool": task.get("tool"),
        "arguments": task.get("arguments"),
        "expected": task.get("expected"),
    }
    return json.dumps(payload, ensure_ascii=False, sort_keys=True, separators=(",", ":"))


def dedupe_tasks(tasks: Iterable[Dict[str, Any]], id_prefix: str) -> List[Dict[str, Any]]:
    unique: List[Dict[str, Any]] = []
    seen: set[str] = set()
    for task in tasks:
        fingerprint = task_fingerprint(task)
        if fingerprint in seen:
            continue
        seen.add(fingerprint)
        unique.append(dict(task))
    for index, task in enumerate(unique, 1):
        task["id"] = f"{id_prefix}-{index:03d}"
    return unique


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


def response_answer_text(response: Dict[str, Any]) -> str:
    """Return the plain-text answer an ergonomics action produced (never the stub).

    The source ergonomics actions (get_include_path / get_signature / verify_symbols /
    check_deprecations / find_example_usage) nest their answer at
    ``<payload>.content[0].text``.  Reading it from the canonical payload keeps the
    length / "Error" prefix assertions pointed at real content in both envelope modes;
    an empty or unreadable payload yields "" so the assertion FAILS instead of passing
    on the transport stub.
    """
    data = canonical_payload(response)
    text = _content_text(data)
    if text is not None:
        return text
    return json.dumps(data, ensure_ascii=False, sort_keys=True) if data else ""


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


def is_empty_result_text(text: str) -> bool:
    """True if a non-error lookup response is an explicit "no rows" sentinel.

    Closes the empty-response loophole: find_callers / find_callees / search_source
    return ``isError=false`` with a "No direct C++ callers found ..." / "No results
    found ..." sentence when the query resolved but produced zero edges.  A
    require_results task that lands on one of these has *not* produced the data an
    agent needs, so it must not score as a hit.
    """
    lowered = text.lower()
    return any(sentinel in lowered for sentinel in _EMPTY_RESULT_SENTINELS)


def lookup_has_data(
    data: Dict[str, Any],
    response_text: str,
    is_error_response: bool,
) -> bool:
    """True if a lookup response carries at least one real symbol row.

    ``data`` must be the CANONICAL payload and ``response_text`` the canonical scan
    text (``response_scan_text``), never the transport ``content[0].text`` -- under
    structured tool results that text is a constant stub, so scanning it would make
    every non-error lookup look like a hit.

    A response counts as carrying data when it is not a transport/handler error, is
    not an explicit empty-result sentinel, and either parses to >=1 structured symbol
    row or carries non-empty payload text (the find_callers/find_callees blob does not
    parse to structured rows, so non-empty non-sentinel payload text is the truthful
    positive signal for those actions).  An empty payload is never data.
    """
    if is_error_response:
        return False
    if is_empty_result_text(response_text):
        return False
    if symbol_results(data):
        return True
    payload_text = _content_text(data)
    if payload_text is not None:
        return bool(payload_text.strip())
    return bool(data)


def references_offending_identifier(text: str, identifier: str) -> bool:
    """True if the response text mentions the offending identifier from bad input.

    A self-correcting error names what the caller asked for so the agent can fix
    the call; a generic "internal error" that drops the identifier does not.
    """
    if not identifier or not text:
        return False
    return identifier.lower() in text.lower()


def has_recovery_hint(text: str, hints: List[str], offending_identifier: str = "") -> bool:
    """True if the response offers a did-you-mean / qualified-symbol / retry hint.

    The offending identifier is stripped from the text first so that ``::`` echoed
    back inside the caller's own ``Class::Method`` token is NOT mistaken for a
    qualified-symbol *suggestion* — only a Class::Method appearing outside the echoed
    input (i.e. an actual alternative the handler offers) counts as a hint.
    """
    cleaned_text = text
    if offending_identifier:
        cleaned_text = cleaned_text.replace(offending_identifier, "")
    blob = (cleaned_text + "\n" + "\n".join(hints)).lower()
    needles = (
        "did you mean",
        "did_you_mean",
        "qualified",
        "::",  # a Class::Method qualified-symbol suggestion (outside the echoed input)
        "search_source first",
        "run source.",
        "trigger_reindex",
        "discover",
        "try ",
        "instead",
        "coverage gap",
    )
    return any(needle in blob for needle in needles)


def score_negative_response(
    *,
    transport_error: bool,
    is_error_response: bool,
    response_text: str,
    hints: List[str],
    data: Dict[str, Any],
    identifier: str,
    expect: Dict[str, Any],
) -> Tuple[float, Dict[str, Any]]:
    """Grade RESPONSE QUALITY on deliberately bad input (0.0 .. 1.0).

    ``response_text`` must be the canonical scan text (``response_scan_text``) and
    ``data`` the canonical payload (``canonical_payload``); the raw transport
    ``content[0].text`` is a constant stub under structured tool results and carries
    neither the empty-result sentinel nor the echoed identifier.

    A bad call should fail loudly and helpfully:
      * transport crash / parse failure        -> 0.0 (worst: agent gets nothing actionable)
      * silent empty-but-success / data row     -> 0.0 (masks the bad input)
      * structured error that drops the symbol  -> 0.4 (loud but not self-correcting)
      * structured error naming the symbol       -> 0.7
      * + a did-you-mean / qualified hint         -> 1.0 (self-correcting)

    When the task declares ``expect_error: false`` (e.g. an unqualified symbol that
    SHOULD still resolve) the contract inverts: a populated answer is the pass and a
    not-found error is the failure.
    """
    expect_error = bool(expect.get("expect_error", True))
    require_identifier = bool(expect.get("require_identifier", True))
    require_hint = bool(expect.get("require_hint", False))

    if transport_error:
        return 0.0, {"reason": "transport_crash"}

    if not expect_error:
        # Resolution task: a structured non-error answer with data is the pass.
        if not is_error_response and lookup_has_data(data, response_text, is_error_response):
            return 1.0, {"reason": "resolved_as_expected"}
        if is_error_response:
            return 0.0, {"reason": "rejected_resolvable_input"}
        return 0.0, {"reason": "empty_on_resolvable_input"}

    if not is_error_response:
        # Bad input must not pass silently.  A non-error response that still surfaces
        # the problem in text earns partial credit; a clean "success" earns nothing.
        if references_offending_identifier(response_text, identifier) and is_empty_result_text(response_text):
            return 0.5, {"reason": "non_error_but_names_problem"}
        return 0.0, {"reason": "silent_non_error_on_bad_input"}

    names_it = references_offending_identifier(response_text, identifier)
    hint = has_recovery_hint(response_text, hints, offending_identifier=identifier)
    score = 0.4
    if names_it or not require_identifier:
        score = 0.7
    if hint:
        score = 1.0
    if require_hint and not hint:
        score = min(score, 0.7)
    return score, {
        "reason": "structured_error",
        "names_identifier": names_it,
        "has_hint": hint,
    }


# ---------------------------------------------------------------------------
# Task scoring
# ---------------------------------------------------------------------------

def score_task(url: str, task: Dict[str, Any], timeout_s: float) -> Dict[str, Any]:
    response = mcp_call(url, str(task["tool"]), dict(task.get("arguments", {})), timeout_s=timeout_s)
    if not isinstance(response, dict):
        response = {
            "protocol_error": True,
            "raw": str(response)[:500],
            "error": "MCP response top-level JSON must be an object",
        }
    data = canonical_payload(response)
    category = task.get("category")
    transport_error = bool(response.get("transport_error"))
    protocol_error = bool(classify_protocol_failure(response))
    is_error_response = (
        bool(result_payload(response).get("isError"))
        or transport_error
        or protocol_error
    )
    expected = task.get("expected") if isinstance(task.get("expected"), dict) else {}
    min_results = expected.get("min_results")
    allows_empty = isinstance(min_results, int) and min_results == 0
    # require_results closes the empty-response loophole: for symbols KNOWN to have
    # callers/callees/a definition the truthful answer is NOT empty, so an empty or
    # sentinel ("No direct C++ callers found ...") response is a real miss, not a hit.
    require_results = bool(expected.get("require_results")) or (isinstance(min_results, int) and min_results >= 1)
    # Human-readable text: evidence/preview fields only.  Token assertions scan the
    # canonical payload (response_scan_full) because under structured tool results
    # result_text() is the constant "OK; see structuredContent." stub.
    response_text_full = result_text(response)
    response_scan_full = response_scan_text(response)

    direct_success = False
    action_selection_score: Optional[float] = None
    param_correction_score: Optional[float] = None
    hallucinated_workflow_risk: Optional[float] = None
    negative_quality_score: Optional[float] = None
    results_count = 0
    field_complete_count = 0
    # expected_nonempty drives field_completeness: only require_results lookups
    # (those whose truthful answer must carry rows) feed the completeness ratio, so
    # an all-empty run can no longer earn a vacuous 1.0.
    expected_nonempty = False
    evidence: Dict[str, Any] = {}

    if category == "symbol_lookup":
        results = symbol_results(data)
        results_count = len(results)
        has_named = results_count > 0 and any(r.get("name") is not None for r in results)
        has_data = lookup_has_data(data, response_scan_full, is_error_response)
        if require_results:
            # Empty / sentinel / error responses are misses for known-nonempty symbols.
            expected_nonempty = True
            direct_success = has_data
            action_selection_score = 1.0 if has_data else 0.0
        else:
            # A successfully executed query whose truthful answer is "no results" is a hit when
            # the task allows empty (min_results == 0) OR the action is find_callers/find_callees:
            # a framework-dispatched method (AActor::BeginPlay, ACharacter::Jump, ...) legitimately
            # has NO direct C++ callers, so a structured non-error "No direct callers/callees found"
            # is a CORRECT definitive answer, not a miss. The require_results subset (curated
            # caller-rich symbols) still forces real rows, so a broken find_callers can't pass.
            action_name = task.get("action", "")
            answers_empty_ok = allows_empty or action_name in ("find_callers", "find_callees")
            direct_success = has_named or (not is_error_response and answers_empty_ok)
            action_selection_score = 1.0 if (results_count > 0 or (not is_error_response and answers_empty_ok)) else 0.0
        field_complete_count = sum(1 for r in results if result_has_min_fields(r))
        evidence = {
            "results_count": results_count,
            "field_complete_count": field_complete_count,
            "require_results": require_results,
            "has_data": has_data,
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
        # Assert on the ACTION's answer text, not the transport stub: under structured
        # tool results content[0].text is always "OK; see structuredContent.", which is
        # never empty and never starts with "Error", so scanning it would degenerate
        # this dimension into "1 - isError rate".
        answer_text = response_answer_text(response)
        direct_success = bool(
            not is_error_response
            and answer_text.strip()
            and not answer_text.lstrip().startswith("Error")
        )
        results_count = 1 if direct_success else 0
        field_complete_count = results_count
        evidence = {
            "response_len": len(answer_text),
            "response_preview": answer_text[:80],
        }

    elif category in ("review_context_lookup", "impact_radius_lookup"):
        results = symbol_results(data)
        results_count = len(results)
        has_named = results_count > 0 and any(r.get("name") is not None for r in results)
        has_data = lookup_has_data(data, response_scan_full, is_error_response)
        if require_results:
            expected_nonempty = True
            direct_success = has_data
            action_selection_score = 1.0 if has_data else 0.0
        else:
            direct_success = has_named or (not is_error_response and allows_empty)
            action_selection_score = 1.0 if (results_count > 0 or (not is_error_response and allows_empty)) else 0.0
        field_complete_count = sum(1 for r in results if result_has_min_fields(r))
        evidence = {
            "results_count": results_count,
            "field_complete_count": field_complete_count,
            "require_results": require_results,
            "has_data": has_data,
            "first_result_keys": list(results[0].keys())[:8] if results else [],
        }

    elif category == "negative_recovery":
        # Deliberately bad input: score RESPONSE QUALITY, not transport success.
        # hints / related_actions live at the result top level in the legacy envelope
        # and inside the structured error object under structured tool results; union
        # both surfaces so neither envelope can hide a self-correcting hint.
        payload = result_payload(response)
        hints: List[str] = []
        for source in (structured_content(payload), payload):
            hints_arr = source.get("hints")
            if isinstance(hints_arr, list):
                hints.extend(str(h) for h in hints_arr)
            related = source.get("related_actions")
            if isinstance(related, list):
                hints.extend(str(r) for r in related)
        identifier = str(expected.get("offending_identifier", ""))
        negative_quality_score, neg_evidence = score_negative_response(
            transport_error=transport_error or protocol_error,
            is_error_response=is_error_response,
            response_text=response_scan_full,
            hints=hints,
            data=data,
            identifier=identifier,
            expect=expected,
        )
        # A negative task "passes" only when the response is meaningfully self-correcting.
        direct_success = negative_quality_score >= float(expected.get("pass_threshold", 0.7))
        evidence = {
            "negative_quality_score": round(negative_quality_score, 4),
            "offending_identifier": identifier,
            "response_is_error": is_error_response,
            "transport_error": transport_error,
            **neg_evidence,
            "response_preview": response_text_full[:120],
        }

    else:
        evidence = {"unsupported_category": category}

    if transport_error or protocol_error:
        direct_success = False

    transport_status = response.get("status")
    return {
        "task_id": task.get("id"),
        "category": category,
        "namespace": task.get("namespace"),
        "action": task.get("action"),
        "direct_success": direct_success,
        "action_selection_score": action_selection_score,
        "param_correction_score": param_correction_score,
        "hallucinated_workflow_risk": hallucinated_workflow_risk,
        "negative_quality_score": negative_quality_score,
        "expected_nonempty": expected_nonempty,
        "results_count": results_count,
        "field_complete_count": field_complete_count,
        "evidence": evidence,
        "transport_error": transport_error,
        "transport_status": (
            transport_status
            if isinstance(transport_status, int) and not isinstance(transport_status, bool)
            else None
        ),
        "transport_error_raw": str(response.get("raw", ""))[:300] if transport_error else "",
        "protocol_error": protocol_error,
        "protocol_error_raw": str(response.get("raw", response))[:500] if protocol_error else "",
        "failure_kind": "protocol_error" if protocol_error else "",
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
    negative_rows = [r for r in rows if r["category"] == "negative_recovery"]

    # symbol_hit_rate: fraction of symbol_lookup + review_context_lookup + impact_radius_lookup with direct_success
    symbol_hit_rate = avg([1.0 if r["direct_success"] else 0.0 for r in lookup_rows])

    # field_completeness_rate: computed ONLY over expected-nonempty (require_results)
    # lookups so an all-empty run cannot earn a vacuous 1.0 — closes the
    # divide-by-returned-results loophole.  Each such task contributes its own
    # completeness ratio (field-complete rows / returned rows; 0 when it returned
    # nothing), and the metric is the mean of those per-task ratios.
    nonempty_lookup_rows = [r for r in lookup_rows if r.get("expected_nonempty")]
    per_task_completeness: List[float] = []
    for r in nonempty_lookup_rows:
        rc = r["results_count"]
        per_task_completeness.append((r["field_complete_count"] / rc) if rc > 0 else 0.0)
    field_completeness_rate = avg(per_task_completeness)

    # schema_adherence_rate: fraction of schema_field_presence tasks that pass
    schema_adherence_rate = avg([1.0 if r["direct_success"] else 0.0 for r in schema_rows])

    # stale_rate: fraction of health_check tasks with stale/error flags (direct_success=False)
    stale_rate = avg([0.0 if r["direct_success"] else 1.0 for r in health_rows])

    # mean_results_per_lookup: average result count for lookup tasks
    mean_results_per_lookup = avg([float(r["results_count"]) for r in lookup_rows])

    # ergonomics_success_rate: fraction of ergonomics_text tasks with non-empty, non-error response
    ergonomics_success_rate = avg([1.0 if r["direct_success"] else 0.0 for r in ergonomics_rows])

    # negative_recovery_rate: mean RESPONSE-QUALITY score on deliberately bad input.
    # A transport crash or silent empty success scores 0; a structured, self-correcting
    # error (names the offending identifier + did-you-mean/qualified hint) scores 1.
    negative_recovery_rate = avg(
        [float(r["negative_quality_score"]) for r in negative_rows if r.get("negative_quality_score") is not None]
    )

    source_index_score = (
        0.30 * symbol_hit_rate
        + 0.20 * field_completeness_rate
        + 0.15 * schema_adherence_rate
        + 0.10 * (1.0 - stale_rate)
        + 0.10 * ergonomics_success_rate
        + 0.15 * negative_recovery_rate
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
            "negative_recovery_rate": round(negative_recovery_rate, 6),
            "mean_results_per_lookup": round(mean_results_per_lookup, 6),
            "expected_nonempty_lookup_count": len(nonempty_lookup_rows),
        },
    }


# ---------------------------------------------------------------------------
# Generate
# ---------------------------------------------------------------------------

# find_callers/find_callees are FUNCTION-call actions: they take a function (bare name or
# qualified Class::Method), NOT a class. The previous list was 15 CLASS names (AActor, UObject,
# UGameplayStatics, ...) for which find_callers correctly returns "No function found" — the
# benchmark was testing find_callers with semantically invalid input. These are real functions
# that resolve (after the 2026-06-18 qualified-name fix) and return either direct callers or a
# truthful "no direct callers" (both valid; min_results:0 + the find_callers/find_callees
# empty-answer-ok scorer pass them, while the curated require_results set still forces real rows).
_CALLERS_CALLEES_SYMBOLS = [
    "FString::Printf", "UObject::GetName", "UObject::GetClass", "UObject::StaticClass",
    "UWorld::GetTimerManager", "UWorld::GetGameInstance", "UActorComponent::GetOwner",
    "AActor::BeginPlay", "AActor::Tick", "AActor::EndPlay", "UActorComponent::BeginPlay",
    "UActorComponent::TickComponent", "FName::ToString", "FVector::Size", "UObject::IsA",
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

_PRESERVED_PRACTICAL_CALLER_METHODS_20260617 = [
    "AActor::BeginPlay",
    "AActor::Tick",
    "AActor::EndPlay",
    "AActor::GetLifetimeReplicatedProps",
    "UActorComponent::BeginPlay",
    "UActorComponent::TickComponent",
    "UActorComponent::EndPlay",
    "UGameplayAbility::ActivateAbility",
    "UGameplayAbility::CanActivateAbility",
    "UGameplayAbility::EndAbility",
    "UGameplayAbility::CommitAbility",
    "UAbilitySystemComponent::GiveAbility",
    "UAbilitySystemComponent::TryActivateAbility",
    "UAbilitySystemComponent::ApplyGameplayEffectToSelf",
    "UAttributeSet::PreAttributeChange",
    "UAttributeSet::PostGameplayEffectExecute",
    "UUserWidget::NativeConstruct",
    "UUserWidget::NativeTick",
    "UUserWidget::NativeOnInitialized",
    "UAnimInstance::NativeUpdateAnimation",
    "UAnimInstance::NativeInitializeAnimation",
    "UCharacterMovementComponent::CalcVelocity",
    "UCharacterMovementComponent::PhysWalking",
    "APlayerController::SetupInputComponent",
    "APlayerController::PlayerTick",
    "AController::Possess",
    "APawn::SetupPlayerInputComponent",
    "APawn::PossessedBy",
    "ACharacter::Landed",
    "ACharacter::Jump",
]

_PRESERVED_PRACTICAL_CALLEE_METHODS_20260617 = [
    "AActor::BeginPlay",
    "AActor::Tick",
    "AActor::EndPlay",
    "AActor::GetLifetimeReplicatedProps",
    "UActorComponent::BeginPlay",
    "UActorComponent::TickComponent",
    "UActorComponent::EndPlay",
    "UGameplayAbility::ActivateAbility",
    "UGameplayAbility::CanActivateAbility",
    "UGameplayAbility::EndAbility",
    "UGameplayAbility::CommitAbility",
    "UAbilitySystemComponent::GiveAbility",
    "UAbilitySystemComponent::TryActivateAbility",
    "UAbilitySystemComponent::ApplyGameplayEffectToSelf",
    "UAttributeSet::PreAttributeChange",
    "UAttributeSet::PostGameplayEffectExecute",
    "UUserWidget::NativeConstruct",
    "UUserWidget::NativeTick",
    "UUserWidget::NativeOnInitialized",
    "UAnimInstance::NativeUpdateAnimation",
]

_PRESERVED_PRACTICAL_RISK_SYMBOLS_20260617 = [
    "UGameplayAbility",
    "UAbilitySystemComponent",
    "UAttributeSet",
    "UGameplayEffect",
    "FGameplayTag",
    "FGameplayTagContainer",
    "FGameplayEffectSpec",
    "UEnhancedInputComponent",
    "UInputAction",
    "UInputMappingContext",
    "UEnhancedPlayerInput",
    "UAnimMontage",
    "UBlendSpace",
    "UAnimBlueprintGeneratedClass",
    "UAnimSequence",
]

_PRESERVED_PRACTICAL_REVIEW_SYMBOLS_20260617 = [
    "UGameplayAbility",
    "UAbilitySystemComponent",
    "UAttributeSet",
    "UGameplayEffect",
    "UCharacterMovementComponent",
    "UUserWidget",
    "UEnhancedInputComponent",
    "UAnimInstance",
    "APlayerController",
    "UActorComponent",
]

_PRESERVED_PRACTICAL_IMPACT_SYMBOLS_20260617 = [
    "FGameplayTag",
    "UAbilitySystemComponent",
    "UGameplayEffect",
    "FHitResult",
    "FVector",
    "FTransform",
    "USkeletalMeshComponent",
    "UDataTable",
    "UActorComponent",
    "UPrimaryDataAsset",
]

_PRESERVED_PRACTICAL_INCLUDE_SYMBOLS_20260617 = [
    "UGameplayAbility",
    "UEnhancedInputComponent",
    "UUserWidget",
    "UAnimMontage",
    "UCharacterMovementComponent",
]

_PRESERVED_PRACTICAL_SIGNATURE_METHODS_20260617 = [
    "UGameplayAbility::ActivateAbility",
    "UAbilitySystemComponent::GiveAbility",
    "UUserWidget::NativeConstruct",
    "UAnimInstance::NativeUpdateAnimation",
    "UCharacterMovementComponent::CalcVelocity",
]

_PRESERVED_PRACTICAL_HEALTH_VARIANTS_20260617 = [
    {"action": "health"},
    {"action": "health", "include_counts": True},
    {"action": "health", "include_counts": False},
    {"action": "health", "detail": "summary"},
    {"action": "health", "mode": "smoke"},
]

_ADDED_PRACTICAL_CALLER_METHODS_20260617 = [
    "UNiagaraComponent::Activate",
    "UNiagaraSystem::GetExposedParameters",
    "UMaterialInstanceDynamic::SetScalarParameterValue",
    "UMaterialInstanceDynamic::SetVectorParameterValue",
    "UUserWidget::NativeDestruct",
    "UWidgetBlueprintLibrary::Create",
    "UAudioComponent::Play",
    "UAudioComponent::Stop",
    "USoundBase::GetDuration",
    "UEnhancedInputLocalPlayerSubsystem::AddMappingContext",
    "UInputComponent::BindAction",
    "UGameplayStatics::OpenLevel",
    "UGameplayStatics::SpawnEmitterAtLocation",
    "UGameplayStatics::PlaySoundAtLocation",
    "ULevelStreamingDynamic::LoadLevelInstance",
    "UDataTable::FindRow",
    "UPrimaryDataAsset::GetPrimaryAssetId",
    "UAssetManager::GetPrimaryAssetObject",
    "USkeletalMeshComponent::PlayAnimation",
    "UPrimitiveComponent::SetCollisionProfileName",
    "UActorComponent::Activate",
    "UActorComponent::Deactivate",
    "APlayerController::ClientTravel",
    "AController::SetPawn",
]

_ADDED_PRACTICAL_CALLEE_METHODS_20260617 = list(_ADDED_PRACTICAL_CALLER_METHODS_20260617)

_ADDED_PRACTICAL_RISK_SYMBOLS_20260617 = [
    "UNiagaraSystem",
    "UNiagaraComponent",
    "UMaterialInterface",
    "UMaterialInstanceDynamic",
    "UTexture2D",
    "UUserWidget",
    "UWidgetAnimation",
    "UAudioComponent",
    "USoundCue",
    "UInputAction",
    "UInputMappingContext",
    "UEnhancedInputLocalPlayerSubsystem",
    "ULevelSequence",
    "UMovieSceneSequence",
    "UDataTable",
    "UPrimaryDataAsset",
    "UAssetManager",
    "UWorldSubsystem",
    "UGameInstanceSubsystem",
    "FSoftObjectPath",
    "UGameplayStatics",
    "APlayerCameraManager",
]

_ADDED_PRACTICAL_REVIEW_SYMBOLS_20260617 = [
    "UNiagaraSystem",
    "UNiagaraComponent",
    "UMaterialInstanceDynamic",
    "UUserWidget",
    "UAudioComponent",
    "UInputMappingContext",
    "UEnhancedInputLocalPlayerSubsystem",
    "ULevelSequence",
    "UDataTable",
    "UPrimaryDataAsset",
    "UAssetManager",
    "UWorldSubsystem",
    "USceneComponent",
    "UPrimitiveComponent",
    "USkeletalMeshComponent",
    "UGameInstance",
    "ULocalPlayer",
]

_ADDED_PRACTICAL_IMPACT_SYMBOLS_20260617 = [
    "UNiagaraComponent",
    "UNiagaraSystem",
    "UMaterialInterface",
    "UTexture2D",
    "UWidgetAnimation",
    "UAudioComponent",
    "USoundCue",
    "UInputAction",
    "UInputMappingContext",
    "ULevelSequence",
    "UMovieSceneSequence",
    "UDataTable",
    "UPrimaryDataAsset",
    "UAssetManager",
    "FSoftObjectPath",
    "UNetDriver",
    "UGameViewportClient",
]

_ADDED_PRACTICAL_INCLUDE_SYMBOLS_20260617 = [
    "UNiagaraComponent",
    "UMaterialInstanceDynamic",
    "UWidgetBlueprintLibrary",
    "UEnhancedInputLocalPlayerSubsystem",
    "ULevelSequence",
]

_ADDED_PRACTICAL_SIGNATURE_METHODS_20260617 = [
    "UNiagaraComponent::Activate",
    "UMaterialInstanceDynamic::SetScalarParameterValue",
    "UWidgetBlueprintLibrary::Create",
    "UEnhancedInputLocalPlayerSubsystem::AddMappingContext",
    "UGameplayStatics::OpenLevel",
]


def append_source_domain_extension(
    tasks: List[Dict[str, Any]],
    next_id: Any,
    *,
    caller_methods: List[str],
    callee_methods: List[str],
    risk_symbols: List[str],
    review_symbols: List[str],
    impact_symbols: List[str],
    include_symbols: List[str],
    signature_methods: List[str],
    health_variants: List[Dict[str, Any]],
) -> None:
    for symbol in caller_methods:
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
    for symbol in callee_methods:
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
    for symbol in risk_symbols:
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
    for symbol in review_symbols:
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
    for symbol in impact_symbols:
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
    for symbol in include_symbols:
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
    for method in signature_methods:
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
    for args in health_variants:
        tasks.append({
            "id": next_id(),
            "category": "health_check",
            "namespace": "source",
            "action": "health",
            "tool": "source_query",
            "arguments": dict(args),
            "expected": {"fields": ["status", "row_counts"]},
            "safety": "read_only",
        })


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

    # Exercise the retained public graph-node search through its EngineSource
    # backend; schema-only discovery cannot catch storage/backend regressions.
    tasks.append({
        "id": next_id(),
        "category": "symbol_lookup",
        "namespace": "source",
        "action": "search_crg_graph",
        "tool": "source_query",
        "arguments": {"action": "search_crg_graph", "query": "UObject", "limit": 5},
        "expected": {
            "min_results": 1,
            "require_results": True,
            "required_fields": ["name", "kind", "file_path"],
        },
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

    append_source_domain_extension(
        tasks,
        next_id,
        caller_methods=_PRESERVED_PRACTICAL_CALLER_METHODS_20260617,
        callee_methods=_PRESERVED_PRACTICAL_CALLEE_METHODS_20260617,
        risk_symbols=_PRESERVED_PRACTICAL_RISK_SYMBOLS_20260617,
        review_symbols=_PRESERVED_PRACTICAL_REVIEW_SYMBOLS_20260617,
        impact_symbols=_PRESERVED_PRACTICAL_IMPACT_SYMBOLS_20260617,
        include_symbols=_PRESERVED_PRACTICAL_INCLUDE_SYMBOLS_20260617,
        signature_methods=_PRESERVED_PRACTICAL_SIGNATURE_METHODS_20260617,
        health_variants=_PRESERVED_PRACTICAL_HEALTH_VARIANTS_20260617,
    )
    append_source_domain_extension(
        tasks,
        next_id,
        caller_methods=_ADDED_PRACTICAL_CALLER_METHODS_20260617,
        callee_methods=_ADDED_PRACTICAL_CALLEE_METHODS_20260617,
        risk_symbols=_ADDED_PRACTICAL_RISK_SYMBOLS_20260617,
        review_symbols=_ADDED_PRACTICAL_REVIEW_SYMBOLS_20260617,
        impact_symbols=_ADDED_PRACTICAL_IMPACT_SYMBOLS_20260617,
        include_symbols=_ADDED_PRACTICAL_INCLUDE_SYMBOLS_20260617,
        signature_methods=_ADDED_PRACTICAL_SIGNATURE_METHODS_20260617,
        health_variants=[],
    )

    append_require_results_tasks(tasks, next_id)
    append_negative_recovery_tasks(tasks, next_id)

    return dedupe_tasks(tasks, "SIB")


def append_require_results_tasks(tasks: List[Dict[str, Any]], next_id: Any) -> None:
    """Curated known-nonempty lookups that MUST return >=1 row (require_results gate).

    These close the min_results:0 loophole: an empty / sentinel response for a symbol
    whose definition or call edges are guaranteed indexed is a real index miss, scored 0.
    """
    for symbol in _KNOWN_DEFINED_SYMBOLS:
        tasks.append({
            "id": next_id(),
            "category": "symbol_lookup",
            "namespace": "source",
            "action": "search_source",
            "tool": "source_query",
            "arguments": {"action": "search_source", "query": symbol},
            "expected": {"min_results": 1, "require_results": True,
                         "required_fields": ["name", "kind", "file_path"]},
            "safety": "read_only",
        })
    for symbol in _KNOWN_DEFINED_SYMBOLS:
        tasks.append({
            "id": next_id(),
            "category": "symbol_lookup",
            "namespace": "source",
            "action": "risk_score",
            "tool": "source_query",
            "arguments": {"action": "risk_score", "symbol": symbol},
            "expected": {"min_results": 1, "require_results": True,
                         "required_fields": ["name", "kind", "file_path"]},
            "safety": "read_only",
        })
    for symbol in _KNOWN_CALLED_METHODS:
        tasks.append({
            "id": next_id(),
            "category": "symbol_lookup",
            "namespace": "source",
            "action": "find_callers",
            "tool": "source_query",
            "arguments": {"action": "find_callers", "symbol": symbol},
            "expected": {"min_results": 1, "require_results": True,
                         "required_fields": ["name", "kind", "file_path"]},
            "safety": "read_only",
        })
    for symbol in _KNOWN_CALLED_METHODS:
        tasks.append({
            "id": next_id(),
            "category": "symbol_lookup",
            "namespace": "source",
            "action": "find_callees",
            "tool": "source_query",
            "arguments": {"action": "find_callees", "symbol": symbol},
            "expected": {"min_results": 1, "require_results": True,
                         "required_fields": ["name", "kind", "file_path"]},
            "safety": "read_only",
        })


def append_negative_recovery_tasks(tasks: List[Dict[str, Any]], next_id: Any) -> None:
    """Adversarial bad-input tasks scored on response quality, not transport success."""
    for action, arg_key, bad_value in _NEGATIVE_NONEXISTENT_SYMBOLS:
        tasks.append({
            "id": next_id(),
            "category": "negative_recovery",
            "namespace": "source",
            "action": action,
            "tool": "source_query",
            "arguments": {"action": action, arg_key: bad_value},
            "expected": {
                "expect_error": True,
                "require_identifier": True,
                "offending_identifier": bad_value,
                "pass_threshold": 0.7,
            },
            "safety": "read_only",
        })
    # Missing required param: the handler must reject with a structured error naming
    # the missing field, not crash or silently succeed.
    for action, arg_key in (("find_callers", "symbol"), ("get_signature", "symbol"),
                            ("get_include_path", "symbol")):
        tasks.append({
            "id": next_id(),
            "category": "negative_recovery",
            "namespace": "source",
            "action": action,
            "tool": "source_query",
            "arguments": {"action": action},
            "expected": {
                "expect_error": True,
                "require_identifier": True,
                "offending_identifier": arg_key,
                "pass_threshold": 0.7,
            },
            "safety": "read_only",
        })
    # Unqualified-vs-qualified resolution: an unqualified method name should still
    # resolve (or be rejected with a qualified-symbol hint).  expect_error: false.
    for action, arg_key, value in _NEGATIVE_UNQUALIFIED_SYMBOLS:
        tasks.append({
            "id": next_id(),
            "category": "negative_recovery",
            "namespace": "source",
            "action": action,
            "tool": "source_query",
            "arguments": {"action": action, arg_key: value},
            "expected": {
                "expect_error": False,
                "offending_identifier": value,
                "pass_threshold": 0.7,
            },
            "safety": "read_only",
        })


def require_generation_status(url: str, timeout_s: float, phase: str) -> Dict[str, Any]:
    """Return one validated MCP status snapshot for catalog-bound generation."""
    response = mcp_call(url, "monolith_status", {}, timeout_s=timeout_s)
    validation = validate_status_response(response)
    if not validation.get("ok"):
        failure_kind = str(validation.get("failure_kind", "protocol_error"))
        raw = str(validation.get("raw", ""))[:300]
        raise RuntimeError(
            f"SourceIndex {phase} status failed ({failure_kind}): {raw or '<no diagnostic>'}"
        )
    status = dict(validation["status"])
    catalog_version = str(status.get("catalog_version", "")).strip()
    if not catalog_version:
        raise RuntimeError(f"SourceIndex {phase} status omitted catalog_version")
    return status


def discover_source_catalog(
    url: str,
    timeout_s: float = 45.0,
) -> Tuple[List[str], Dict[str, Any]]:
    """Enumerate the complete live source catalog under one stable identity.

    Generation is intentionally catalog-bound even when the curated static corpus
    already satisfies ``--min-tasks``.  This catches removed/renamed actions before
    canonical files are written and prevents a transient editor restart from mixing
    pages from different registries.
    """
    start_status = require_generation_status(url, timeout_s, "pre-discovery")
    start_catalog_version = str(start_status["catalog_version"]).strip()
    page_catalog_versions: set[str] = set()

    def fetch_page(arguments: Dict[str, Any]) -> Dict[str, Any]:
        response = mcp_call(url, "monolith_discover", arguments, timeout_s=timeout_s)
        if response.get("transport_error"):
            raise RuntimeError(
                "source catalog discovery transport failure: "
                f"{str(response.get('raw', ''))[:300]}"
            )
        protocol_failure = classify_protocol_failure(response)
        if protocol_failure:
            raise RuntimeError(
                f"source catalog discovery {protocol_failure}: "
                f"{str(response.get('raw', result_text(response)))[:300]}"
            )
        payload = result_payload(response)
        if payload.get("isError"):
            raise RuntimeError(
                "source catalog discovery returned isError: "
                f"{result_text(response)[:300]}"
            )
        data = result_data(response)
        page_catalog_version = str(data.get("catalog_version", "")).strip()
        if page_catalog_version:
            page_catalog_versions.add(page_catalog_version)
        return data

    action_names = paginate_discover_action_names(fetch_page, "source")
    if not action_names:
        raise RuntimeError("live source catalog contains no actions")
    normalized_actions = [str(name).strip() for name in action_names if str(name).strip()]
    unique_actions = sorted(set(normalized_actions))
    if len(unique_actions) != len(normalized_actions):
        raise RuntimeError("live source catalog contains duplicate action names")

    end_status = require_generation_status(url, timeout_s, "post-discovery")
    end_catalog_version = str(end_status["catalog_version"]).strip()
    if end_catalog_version != start_catalog_version:
        raise RuntimeError(
            "source catalog changed during generation "
            f"({start_catalog_version} -> {end_catalog_version})"
        )
    unexpected_page_versions = sorted(
        version for version in page_catalog_versions if version != start_catalog_version
    )
    if unexpected_page_versions:
        raise RuntimeError(
            "source discover page catalog_version disagrees with monolith_status: "
            f"expected {start_catalog_version}, observed {unexpected_page_versions}"
        )

    return unique_actions, start_status


def validate_source_catalog_contract(
    tasks: List[Dict[str, Any]],
    live_actions: Iterable[str],
) -> Dict[str, Any]:
    """Validate curated tasks against live action identities without guessing schemas."""
    live_action_set = {str(action).strip() for action in live_actions if str(action).strip()}
    referenced_actions: set[str] = set()
    for task in tasks:
        task_id = str(task.get("id", "<missing-id>"))
        namespace = str(task.get("namespace", "")).strip()
        action = str(task.get("action", "")).strip()
        arguments = task.get("arguments")
        if namespace != "source":
            raise RuntimeError(f"{task_id} must target namespace 'source', got {namespace!r}")
        if not action:
            raise RuntimeError(f"{task_id} has no source action")
        if not isinstance(arguments, dict) or str(arguments.get("action", "")).strip() != action:
            raise RuntimeError(
                f"{task_id} arguments.action must exactly match task action {action!r}"
            )
        referenced_actions.add(action)

    missing_actions = sorted(referenced_actions - live_action_set)
    if missing_actions:
        raise RuntimeError(
            "SourceIndex curated tasks reference actions absent from the live source catalog: "
            + ", ".join(missing_actions)
        )
    uncovered_actions = sorted(live_action_set - referenced_actions)
    return {
        "live_action_count": len(live_action_set),
        "referenced_action_count": len(referenced_actions),
        "uncovered_action_count": len(uncovered_actions),
        "uncovered_actions": uncovered_actions,
    }


def append_live_schema_coverage_tasks(
    tasks: List[Dict[str, Any]],
    live_actions: Iterable[str],
) -> List[str]:
    """Add deterministic discovery-only coverage for otherwise-unreferenced actions."""
    referenced_actions = {
        str(task.get("action", "")).strip()
        for task in tasks
        if str(task.get("action", "")).strip()
    }
    uncovered_actions = sorted(
        {str(action).strip() for action in live_actions if str(action).strip()}
        - referenced_actions
    )
    for action in uncovered_actions:
        tasks.append({
            "id": f"SIB-{len(tasks) + 1:03d}",
            "category": "schema_field_presence",
            "namespace": "source",
            "action": action,
            "tool": "monolith_discover",
            "arguments": {
                "namespace": "source",
                "action": action,
                "mode": "schema",
            },
            "expected": {
                "requires_planning_signals": True,
                "requires_skill": True,
                "requires_status_declared": True,
            },
            "safety": "read_only_discovery",
        })
    return uncovered_actions


def generate_tasks(url: str, min_tasks: int, tasks_path: pathlib.Path, manifest_path: pathlib.Path) -> Dict[str, Any]:
    """Generate curated fixtures after validating the complete live source catalog."""
    tasks_path = resolve_plugin_path(tasks_path)
    manifest_path = resolve_plugin_path(manifest_path)
    if min_tasks < 0:
        raise RuntimeError("--min-tasks must be >= 0")
    tasks = build_static_tasks()
    live_actions, generation_status = discover_source_catalog(url)
    initial_catalog_validation = validate_source_catalog_contract(tasks, live_actions)
    generated_schema_actions = append_live_schema_coverage_tasks(tasks, live_actions)
    final_catalog_validation = validate_source_catalog_contract(tasks, live_actions)
    catalog_validation = {
        "live_action_count": final_catalog_validation["live_action_count"],
        "covered_action_count": final_catalog_validation["referenced_action_count"],
        "preexisting_covered_action_count": initial_catalog_validation["referenced_action_count"],
        "generated_schema_coverage_count": len(generated_schema_actions),
        "generated_schema_coverage_actions": generated_schema_actions,
        "uncovered_action_count": final_catalog_validation["uncovered_action_count"],
        "uncovered_actions": final_catalog_validation["uncovered_actions"],
    }
    if len(tasks) < min_tasks:
        raise RuntimeError(
            f"SourceIndex curated corpus has {len(tasks)} tasks, below --min-tasks={min_tasks}. "
            "Add schema-verified curated tasks; generic live-action/query top-ups are forbidden."
        )

    # Re-assign IDs to be monotonic after any additions.
    for index, task in enumerate(tasks, 1):
        task["id"] = f"SIB-{index:03d}"

    schema_actions = sorted({
        str(task["action"])
        for task in tasks
        if task.get("category") == "schema_field_presence"
    })

    write_jsonl(tasks_path, tasks)

    manifest = {
        "generated_at": utc_now(),
        "mcp_url": url,
        "catalog_version": str(generation_status.get("catalog_version", "")),
        "catalog_validation": catalog_validation,
        "task_count": len(tasks),
        "min_tasks_requested": min_tasks,
        "golden_symbols": GOLDEN_SYMBOLS,
        "schema_actions": schema_actions,
        "curated_schema_actions": SCHEMA_ACTIONS,
        "category_counts": count_by(tasks, "category"),
        "task_file": display_path(tasks_path),
        "scoring": {
            "source_index_score": (
                "0.30 * symbol_hit_rate"
                " + 0.20 * field_completeness_rate"
                " + 0.15 * schema_adherence_rate"
                " + 0.10 * (1 - stale_rate)"
                " + 0.10 * ergonomics_success_rate"
                " + 0.15 * negative_recovery_rate"
            ),
            "symbol_hit_rate": "fraction of symbol_lookup + review_context_lookup + impact_radius_lookup tasks with direct_success (require_results tasks miss on empty/sentinel responses)",
            "field_completeness_rate": "mean per-task field-completeness over expected-nonempty (require_results) lookups only; an empty required lookup contributes 0 (closes the divide-by-returned loophole)",
            "schema_adherence_rate": "fraction of schema_field_presence tasks with planning_signals + skill + status declared",
            "stale_rate": "fraction of health_check tasks with stale/error/missing-fields response",
            "ergonomics_success_rate": "fraction of ergonomics_text tasks with non-empty, non-error response",
            "negative_recovery_rate": "mean response-quality score (0..1) on deliberately bad input: transport crash / silent empty = 0, structured error naming the offending identifier = 0.7, + did-you-mean/qualified hint = 1.0",
            "mean_results_per_lookup": "average result count across symbol_lookup + review_context_lookup + impact_radius_lookup tasks",
        },
        "run_gates": {
            "max_transport_failed_fraction": DEFAULT_MAX_TRANSPORT_FAILED_FRACTION,
            "max_consecutive_transport_failures": DEFAULT_MAX_CONSECUTIVE_TRANSPORT_FAILURES,
            "min_transport_fraction_sample": DEFAULT_MIN_TRANSPORT_FRACTION_SAMPLES,
            "status_transport_failure_aborts_before_tasks": True,
            "invalid_status_response_aborts_before_tasks": True,
            "canonical_catalog_version_mismatch_aborts_before_tasks": True,
            "invalid_run_writes_summary": False,
        },
    }
    write_json(manifest_path, manifest)
    return manifest


# ---------------------------------------------------------------------------
# Run
# ---------------------------------------------------------------------------

def runner_exception_task_row(task: Dict[str, Any], error: str) -> Dict[str, Any]:
    """Preserve the task that exposed a benchmark implementation failure."""
    return {
        "task_id": task.get("id"),
        "category": task.get("category"),
        "namespace": task.get("namespace"),
        "action": task.get("action"),
        "direct_success": False,
        "action_selection_score": None,
        "param_correction_score": None,
        "hallucinated_workflow_risk": None,
        "negative_quality_score": None,
        "expected_nonempty": False,
        "results_count": 0,
        "field_complete_count": 0,
        "evidence": {"runner_exception": error},
        "transport_error": False,
        "transport_status": None,
        "transport_error_raw": "",
        "protocol_error": False,
        "protocol_error_raw": "",
        "response_is_error": False,
        "response_text": "",
        "failure_kind": "runner_exception",
        "error": error,
    }


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
    tracker: TransportFailureTracker,
    benchmark_inputs: Dict[str, Any],
    corpus: TaskCorpus,
    start_identity: Dict[str, str],
    fields: Dict[str, Any],
) -> Dict[str, Any]:
    """Build partial diagnostics without letting aggregate defects hide the invalid run."""
    try:
        failure = aggregate(label, status, tasks[:len(rows)], rows)
    except Exception as exc:  # noqa: BLE001 - retain a minimal invalid artifact.
        failure = {
            "label": label,
            "created_at": utc_now(),
            "task_count": len(rows),
            "aggregate_error": f"{type(exc).__name__}: {exc}",
        }
    failure.update(fields)
    failure["run_valid"] = False
    failure["metrics_valid"] = False
    failure.update(tracker.snapshot())
    attach_benchmark_inputs(failure, benchmark_inputs)
    attach_run_context(failure, corpus, start_identity)
    return failure


def run_benchmark(
    url: str,
    tasks_path: pathlib.Path,
    output_dir: pathlib.Path,
    label: str,
    timeout_s: float,
    *,
    allow_subset: bool = False,
    max_transport_failed_fraction: float = DEFAULT_MAX_TRANSPORT_FAILED_FRACTION,
    max_consecutive_transport_failures: int = DEFAULT_MAX_CONSECUTIVE_TRANSPORT_FAILURES,
    min_transport_fraction_sample: int = DEFAULT_MIN_TRANSPORT_FRACTION_SAMPLES,
) -> Dict[str, Any]:
    output_dir.mkdir(parents=True, exist_ok=True)
    clear_run_outputs(output_dir)

    try:
        corpus = load_task_corpus(
            tasks_path,
            suite="SourceIndex",
            canonical_tasks_path=DEFAULT_TASKS,
            canonical_manifest_path=DEFAULT_MANIFEST,
            allow_subset=allow_subset,
            allowed_categories=SOURCE_INDEX_TASK_CATEGORIES,
            require_arguments=True,
        )
        tasks_path = resolve_plugin_path(tasks_path)
        tasks = corpus.tasks
    except Exception as exc:  # noqa: BLE001 - invalid inputs must invalidate stale baselines.
        failure = {
            "label": label,
            "completion_status": "aborted_input_preflight",
            "failure_stage": "input_preflight",
            "failure_kind": "runner_exception",
            "metrics_scope": "not_started",
            "completed_task_count": 0,
            "total_task_count": 0,
            "error": f"{type(exc).__name__}: {exc}",
        }
        write_run_failure(output_dir, failure)
        return failure

    try:
        transport_tracker = TransportFailureTracker(
            max_failed_fraction=max_transport_failed_fraction,
            max_consecutive_failures=max_consecutive_transport_failures,
            min_fraction_samples=min_transport_fraction_sample,
        )
    except ValueError as exc:
        failure = {
            "label": label,
            "completion_status": "aborted_invalid_configuration",
            "failure_stage": "configuration",
            "failure_kind": "invalid_configuration",
            "metrics_scope": "not_started",
            "completed_task_count": 0,
            "total_task_count": len(tasks),
            "error": str(exc),
        }
        write_run_failure(output_dir, failure)
        return failure

    try:
        status_response: Any = mcp_call(url, "monolith_status", {}, timeout_s=timeout_s)
        status_validation = validate_status_response(status_response)
    except Exception as exc:  # noqa: BLE001 - status defects must create invalid artifacts.
        status_validation = {
            "ok": False,
            "failure_kind": "runner_exception",
            "raw": f"{type(exc).__name__}: {exc}",
            "transport_status": None,
        }
    if not status_validation.get("ok"):
        failure_kind = str(status_validation.get("failure_kind", "protocol_error"))
        raw = str(status_validation.get("raw", ""))[:500]
        failure = {
            "label": label,
            "completion_status": (
                "aborted_status_transport_failure"
                if failure_kind == "transport_error"
                else "aborted_status_preflight"
            ),
            "failure_stage": "status_preflight",
            "failure_kind": failure_kind,
            "metrics_scope": "not_started",
            "completed_task_count": 0,
            "total_task_count": len(tasks),
            "transport_failure_count": 1 if failure_kind == "transport_error" else 0,
            "last_transport_status": status_validation.get("transport_status"),
            "last_transport_error_raw": raw if failure_kind == "transport_error" else "",
            "protocol_error_raw": raw if failure_kind != "transport_error" else "",
            "max_transport_failed_fraction": max_transport_failed_fraction,
            "max_consecutive_transport_failures": max_consecutive_transport_failures,
            "min_transport_fraction_sample": min_transport_fraction_sample,
        }
        write_run_failure(output_dir, failure)
        return failure

    status = dict(status_validation["status"])
    start_identity = status_identity(status, endpoint=url)
    expected_catalog = str(corpus.manifest.get("catalog_version", "")).strip()
    observed_catalog = str(status.get("catalog_version", "")).strip()
    if corpus.canonical and (
        not expected_catalog or observed_catalog != expected_catalog
    ):
        failure = {
            "label": label,
            "completion_status": "aborted_catalog_identity_mismatch",
            "failure_stage": "status_preflight",
            "failure_kind": "catalog_identity_mismatch",
            "metrics_scope": "not_started",
            "completed_task_count": 0,
            "total_task_count": len(tasks),
            "expected_catalog_version": expected_catalog,
            "observed_catalog_version": observed_catalog,
            "error": (
                "canonical SourceIndex manifest catalog_version must exactly match "
                "the live status catalog_version before task calls"
            ),
        }
        attach_run_context(failure, corpus, start_identity)
        write_run_failure(output_dir, failure)
        return failure

    benchmark_inputs = build_benchmark_inputs(
        "SourceIndex",
        tasks_path=tasks_path,
        mcp_status=status,
        extra_files={"runner": pathlib.Path(__file__)},
    )
    rows: List[Dict[str, Any]] = []
    per_task_jsonl = output_dir / "per_task.jsonl"

    for index, task in enumerate(tasks, 1):
        runner_exception = ""
        try:
            row = score_task(url, task, timeout_s)
        except Exception as exc:  # noqa: BLE001 - preserve the triggering task and abort.
            runner_exception = f"{type(exc).__name__}: {exc}"
            row = runner_exception_task_row(task, runner_exception)
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
        print(
            f"[{index}/{len(tasks)}] {row['task_id']} success={row['direct_success']} "
            f"direct={row['direct_success']}",
            flush=True,
        )

        if runner_exception:
            failure = build_attempt_failure(
                label,
                status,
                tasks,
                rows,
                transport_tracker,
                benchmark_inputs,
                corpus,
                start_identity,
                {
                    "completion_status": "aborted_runner_exception",
                    "failure_stage": "task_scoring",
                    "failure_kind": "runner_exception",
                    "metrics_scope": "attempted_prefix_runner_exception",
                    "completed_task_count": index,
                    "total_task_count": len(tasks),
                    "last_task_id": str(task.get("id", "")),
                    "exception": runner_exception,
                },
            )
            write_run_failure(output_dir, failure)
            write_json(output_dir / "partial_summary.json", failure)
            return failure

        if row.get("failure_kind") == "protocol_error":
            failure = build_attempt_failure(
                label,
                status,
                tasks,
                rows,
                transport_tracker,
                benchmark_inputs,
                corpus,
                start_identity,
                {
                    "completion_status": "aborted_protocol_error",
                    "failure_stage": "task_response",
                    "failure_kind": "protocol_error",
                    "metrics_scope": "attempted_prefix_protocol_error",
                    "completed_task_count": index,
                    "total_task_count": len(tasks),
                    "last_task_id": str(task.get("id", "")),
                    "protocol_error_raw": str(row.get("protocol_error_raw", "")),
                },
            )
            write_run_failure(output_dir, failure)
            write_json(output_dir / "partial_summary.json", failure)
            return failure

        if transport_decision:
            failure = build_attempt_failure(
                label,
                status,
                tasks,
                rows,
                transport_tracker,
                benchmark_inputs,
                corpus,
                start_identity,
                {
                    "completion_status": "aborted_transport_failure_budget",
                    "failure_stage": "task_scoring",
                    "failure_kind": "transport_error",
                    "metrics_scope": "attempted_prefix_including_transport_failures",
                    "completed_task_count": index,
                    "total_task_count": len(tasks),
                    "transport_gate_reason": transport_decision.reason,
                    "last_task_id": transport_decision.item_id,
                },
            )
            write_run_failure(output_dir, failure)
            write_json(output_dir / "partial_summary.json", failure)
            return failure

        if index == 1 or index == len(tasks) or index % 10 == 0:
            partial = aggregate(label, status, tasks[:index], rows)
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

    try:
        summary = aggregate(label, status, tasks, rows)
    except Exception as exc:  # noqa: BLE001 - aggregate defects invalidate the run.
        error = f"{type(exc).__name__}: {exc}"
        failure = build_attempt_failure(
            label,
            status,
            tasks,
            rows,
            transport_tracker,
            benchmark_inputs,
            corpus,
            start_identity,
            {
                "completion_status": "aborted_runner_exception",
                "failure_stage": "final_aggregate",
                "failure_kind": "runner_exception",
                "metrics_scope": "complete_run_invalid",
                "completed_task_count": len(rows),
                "total_task_count": len(tasks),
                "exception": error,
            },
        )
        write_run_failure(output_dir, failure)
        write_json(output_dir / "partial_summary.json", failure)
        return failure

    final_transport_decision = transport_tracker.finalize()
    summary.update({
        "run_valid": True,
        "metrics_valid": True,
        "metrics_scope": "complete_run" if corpus.comparable else "complete_subset_run",
        "completion_status": "completed",
    })
    summary.update(transport_tracker.snapshot())
    attach_benchmark_inputs(summary, benchmark_inputs)
    attach_run_context(summary, corpus, start_identity)
    if final_transport_decision:
        summary.update({
            "run_valid": False,
            "metrics_valid": False,
            "metrics_scope": "complete_run_invalid",
            "completion_status": "completed_transport_failure_budget_exceeded",
            "failure_kind": "transport_error",
            "transport_gate_reason": final_transport_decision.reason,
            "last_task_id": final_transport_decision.item_id,
        })
        write_run_failure(output_dir, summary)
        write_json(output_dir / "partial_summary.json", summary)
        return summary

    try:
        end_status_response: Any = mcp_call(url, "monolith_status", {}, timeout_s=timeout_s)
        end_status_validation = validate_status_response(end_status_response)
    except Exception as exc:  # noqa: BLE001 - postflight must invalidate the run.
        end_status_validation = {
            "ok": False,
            "failure_kind": "runner_exception",
            "raw": f"{type(exc).__name__}: {exc}",
            "transport_status": None,
        }
    if not end_status_validation.get("ok"):
        failure_kind = str(end_status_validation.get("failure_kind", "protocol_error"))
        summary.update({
            "run_valid": False,
            "metrics_valid": False,
            "metrics_scope": "complete_run_invalid",
            "completion_status": "aborted_status_postflight",
            "failure_stage": "status_postflight",
            "failure_kind": failure_kind,
            "postflight_status_raw": str(end_status_validation.get("raw", ""))[:500],
            "postflight_transport_status": end_status_validation.get("transport_status"),
        })
        write_run_failure(output_dir, summary)
        write_json(output_dir / "partial_summary.json", summary)
        return summary

    end_status = dict(end_status_validation["status"])
    end_identity = status_identity(end_status, endpoint=url)
    identity_drift = status_identity_mismatches(start_identity, end_identity)
    attach_run_context(summary, corpus, start_identity, end_identity)
    if identity_drift:
        summary.update({
            "run_valid": False,
            "metrics_valid": False,
            "metrics_scope": "complete_run_invalid",
            "completion_status": "aborted_status_identity_drift",
            "failure_stage": "status_postflight",
            "failure_kind": "status_identity_drift",
            "status_identity_mismatches": identity_drift,
        })
        write_run_failure(output_dir, summary)
        write_json(output_dir / "partial_summary.json", summary)
        return summary

    write_json(output_dir / "summary.json", summary)
    write_json(output_dir / "per_task.json", rows)
    partial_path = output_dir / "partial_summary.json"
    if partial_path.exists():
        partial_path.unlink()
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
        "ergonomics_success_rate",
        "negative_recovery_rate",
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
    gen.add_argument("--min-tasks", type=int, default=363)

    run_cmd = sub.add_parser("run", help="Run tasks against a live MCP endpoint and score results")
    run_cmd.add_argument("--mcp-url", default=DEFAULT_MCP_URL)
    run_cmd.add_argument("--tasks", type=pathlib.Path, default=DEFAULT_TASKS)
    run_cmd.add_argument("--output-dir", type=pathlib.Path, required=True)
    run_cmd.add_argument("--label", required=True)
    run_cmd.add_argument("--request-timeout-s", type=float, default=12.0)
    run_cmd.add_argument(
        "--allow-subset",
        action="store_true",
        help="Permit an explicit non-canonical diagnostic corpus; output is marked non-comparable.",
    )
    run_cmd.add_argument(
        "--max-transport-failed-fraction",
        type=float,
        default=DEFAULT_MAX_TRANSPORT_FAILED_FRACTION,
        help="Abort without summary when transport failures exceed this fraction after 20 tasks.",
    )
    run_cmd.add_argument(
        "--max-consecutive-transport-failures",
        type=int,
        default=DEFAULT_MAX_CONSECUTIVE_TRANSPORT_FAILURES,
        help="Abort without summary after this many consecutive transport-failed tasks.",
    )
    run_cmd.add_argument(
        "--min-transport-fraction-sample",
        type=int,
        default=DEFAULT_MIN_TRANSPORT_FRACTION_SAMPLES,
        help="Minimum attempted tasks before applying the transport-fraction gate.",
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
            args.request_timeout_s,
            allow_subset=args.allow_subset,
            max_transport_failed_fraction=args.max_transport_failed_fraction,
            max_consecutive_transport_failures=args.max_consecutive_transport_failures,
            min_transport_fraction_sample=args.min_transport_fraction_sample,
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
