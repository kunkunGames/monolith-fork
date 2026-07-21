#!/usr/bin/env python3
"""Offline unit tests for the SourceIndex benchmark scoring branches.

No live editor / MCP server is touched.  Each test fabricates an MCP ``tools/call``
response in one of the TWO real envelope shapes and asserts the SAME verdict from both,
so the scorer stays envelope-invariant:

  legacy (``bEnableStructuredToolResults=False``) -- payload serialized into the text::

      {"result": {"content": [{"type": "text", "text": <payload json>}], "isError": <bool>}}

  structured (the live contract, checked in at ``Config/DefaultMonolith.ini``) -- payload
  ONLY in ``structuredContent``; ``content[0].text`` is the constant stub
  "OK; see structuredContent."::

      {"result": {"isError": false, "structuredContent": {<payload>},
                  "_meta": {"result_kind": "structured"},
                  "content": [{"type": "text", "text": "OK; see structuredContent."}]}}

and asserts the scoring behaviour:

  * an empty ``find_callers`` response on a require_results task scores LOW,
  * a correct multi-result response scores HIGH,
  * a not-found symbol that returns a structured, identifier-naming error passes
    the negative_recovery category, while a transport crash / silent empty does not,
  * ergonomics grades the action's answer text, never the transport stub.

Run::

    python Plugins/Monolith/Scripts/test_source_index_benchmark.py
"""

from __future__ import annotations

import importlib.util
import json
import pathlib
import sys
import tempfile
from typing import Any, Dict, List, Optional

_SCRIPTS_DIR = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(_SCRIPTS_DIR))

_SPEC = importlib.util.spec_from_file_location("sib_under_test", _SCRIPTS_DIR / "source_index_benchmark.py")
sib = importlib.util.module_from_spec(_SPEC)
assert _SPEC and _SPEC.loader
_SPEC.loader.exec_module(sib)


# ---------------------------------------------------------------------------
# Fabricated MCP responses (mirror the real source-handler envelope shapes)
# ---------------------------------------------------------------------------

def _ok_text(text: str) -> Dict[str, Any]:
    return {"result": {"content": [{"type": "text", "text": text}], "isError": False}, "request": {}}


def _error_text(text: str, hints: Optional[List[str]] = None,
                related: Optional[List[str]] = None) -> Dict[str, Any]:
    result: Dict[str, Any] = {"content": [{"type": "text", "text": text}], "isError": True}
    if hints:
        result["hints"] = hints
    if related:
        result["related_actions"] = related
    return {"result": result, "request": {}}


def _transport_crash() -> Dict[str, Any]:
    return {"transport_error": True, "status": None, "raw": "timeout", "request": {}}


# --- Structured tool results (bEnableStructuredToolResults=True, the live contract) ---
#
# On SUCCESS the payload lives ONLY in ``structuredContent`` and ``content[0].text`` is
# a constant stub.  On ERROR the compact envelope keeps a human message in
# ``content[0].text`` but moves hints/related_actions into ``structuredContent``.
# Contract: Docs/specs/SPEC_MonolithStructuredToolResults.md.

_STRUCTURED_STUB = "OK; see structuredContent."


def _structured_ok(payload: Dict[str, Any]) -> Dict[str, Any]:
    return {
        "result": {
            "isError": False,
            "structuredContent": payload,
            "_meta": {"result_kind": "structured", "content_text_mode": "compact_status"},
            "content": [{"type": "text", "text": _STRUCTURED_STUB}],
        },
        "request": {},
    }


def _structured_ok_text(text: str) -> Dict[str, Any]:
    """Structured success whose action payload nests the handler's own text blob.

    The source handlers put ``{"content":[{"type":"text","text": <blob>}]}`` inside the
    action payload, so under structured results that object IS ``structuredContent``.
    """
    return _structured_ok({"content": [{"type": "text", "text": text}]})


def _structured_error(text: str, hints: Optional[List[str]] = None,
                      related: Optional[List[str]] = None,
                      error_code: str = "not_found") -> Dict[str, Any]:
    payload: Dict[str, Any] = {"ok": False, "error": text, "error_code": error_code}
    if hints:
        payload["hints"] = list(hints)
    if related:
        payload["related_actions"] = list(related)
    return {
        "result": {
            "isError": True,
            "structuredContent": payload,
            "_meta": {"result_kind": "structured", "content_text_mode": "compact_status"},
            "content": [{"type": "text", "text": f"{text}; see structuredContent."}],
        },
        "request": {},
    }


def _legacy_json(payload: Dict[str, Any], *, is_error: bool = False) -> Dict[str, Any]:
    """Legacy envelope (flag off): the action payload is SERIALIZED into content[0].text."""
    return {
        "result": {"content": [{"type": "text", "text": json.dumps(payload)}], "isError": is_error},
        "request": {},
    }


# A populated find_callers response: lines are "FromName — path:line" (em-dash blob).
_POPULATED_CALLERS = (
    "AMyActor::Start — Source/MyActor.cpp:42\n"
    "UMyComp::Init — Source/MyComp.cpp:88\n"
    "AGameMode::Begin — Source/GameMode.cpp:13"
)
# A populated search_source response carries structured rows in the parseable
# "  [kind] name (path:line)" blob shape.
_POPULATED_SEARCH = (
    "  [class] AActor (D:/UE/Engine/Actor.h:256)\n"
    "  [class] AActorComponent (D:/UE/Engine/ActorComponent.h:30)"
)


def _make_runner(response: Dict[str, Any]):
    """Patch sib.mcp_call to return a fixed fabricated response and run score_task."""
    def _call(url, tool, arguments, timeout_s=45.0):  # noqa: ANN001
        return dict(response)
    return _call


def _score(task: Dict[str, Any], response: Dict[str, Any]) -> Dict[str, Any]:
    original = sib.mcp_call
    sib.mcp_call = _make_runner(response)
    try:
        return sib.score_task("http://unused", task, timeout_s=1.0)
    finally:
        sib.mcp_call = original


def _status_response(catalog_version: str = "sha256:test") -> Dict[str, Any]:
    return _ok_text(json.dumps({
        "server_running": True,
        "catalog_version": catalog_version,
        "project_name": "Speed",
        "plugin_version": "unit-test",
        "engine_version": "unit-test",
    }))


def _curated_source_actions() -> List[str]:
    return sorted({str(task["action"]) for task in sib.build_static_tasks()})


def _generation_router(
    actions: List[str],
    *,
    start_catalog_version: str = "sha256:test",
    end_catalog_version: Optional[str] = None,
    calls: Optional[List[str]] = None,
):
    status_calls = 0

    def fake_mcp_call(url, tool, arguments, timeout_s=45.0):  # noqa: ANN001
        nonlocal status_calls
        if calls is not None:
            calls.append(tool)
        if tool == "monolith_status":
            status_calls += 1
            version = (
                end_catalog_version
                if status_calls > 1 and end_catalog_version is not None
                else start_catalog_version
            )
            return _status_response(version)
        if tool == "monolith_discover":
            return _legacy_json({
                "catalog_version": start_catalog_version,
                "actions": [{"action": action} for action in actions],
                "truncated": False,
            })
        raise AssertionError(f"unexpected generation tool call: {tool}")

    return fake_mcp_call


def _run_task(index: int) -> Dict[str, Any]:
    return {
        "id": f"SIB-R-{index}",
        "category": "symbol_lookup",
        "namespace": "source",
        "action": "search_source",
        "tool": "source_query",
        "arguments": {"action": "search_source", "query": "AActor"},
        "expected": {"min_results": 1, "require_results": True},
    }


def _run_row(
    task: Dict[str, Any],
    *,
    transport: bool = False,
    status: Any = None,
    raw: str = "",
    protocol: bool = False,
) -> Dict[str, Any]:
    return {
        "task_id": task["id"],
        "category": task["category"],
        "namespace": task["namespace"],
        "action": task["action"],
        "direct_success": not transport and not protocol,
        "action_selection_score": 0.0 if transport or protocol else 1.0,
        "param_correction_score": None,
        "hallucinated_workflow_risk": None,
        "negative_quality_score": None,
        "expected_nonempty": True,
        "results_count": 0 if transport or protocol else 1,
        "field_complete_count": 0 if transport or protocol else 1,
        "evidence": {},
        "transport_error": transport,
        "transport_status": status,
        "transport_error_raw": raw if transport else "",
        "protocol_error": protocol,
        "protocol_error_raw": raw if protocol else "",
        "failure_kind": "protocol_error" if protocol else "",
        "response_is_error": False,
        "response_text": "",
    }


def _run_with_fake_rows(
    tasks: List[Dict[str, Any]],
    fake_score: Any,
    *,
    output_dir: pathlib.Path,
    status_response: Any = None,
    **kwargs: Any,
) -> Dict[str, Any]:
    tasks_path = output_dir.parent / "tasks.jsonl"
    tasks_path.write_text(
        "".join(json.dumps(task) + "\n" for task in tasks),
        encoding="utf-8",
    )
    unit_database_path = output_dir.parent / "EngineSource.unit.db"
    unit_database_path.write_bytes(b"source-index-unit-database")
    original_call = sib.mcp_call
    original_score = sib.score_task
    original_build_inputs = sib.build_benchmark_inputs
    sib.mcp_call = lambda url, tool, arguments, timeout_s=45.0: (
        _status_response() if status_response is None else status_response
    )
    sib.score_task = fake_score
    sib.build_benchmark_inputs = lambda *args, **kwargs: original_build_inputs(
        *args,
        **{
            **kwargs,
            "database_paths": (unit_database_path,),
        },
    )
    try:
        return sib.run_benchmark(
            "http://unused",
            tasks_path,
            output_dir,
            "selftest",
            1.0,
            allow_subset=True,
            **kwargs,
        )
    finally:
        sib.mcp_call = original_call
        sib.score_task = original_score
        sib.build_benchmark_inputs = original_build_inputs


# ---------------------------------------------------------------------------
# Tasks under test
# ---------------------------------------------------------------------------

def _require_results_callers_task() -> Dict[str, Any]:
    return {
        "id": "T-callers",
        "category": "symbol_lookup",
        "namespace": "source",
        "action": "find_callers",
        "tool": "source_query",
        "arguments": {"action": "find_callers", "symbol": "AActor::BeginPlay"},
        "expected": {"min_results": 1, "require_results": True,
                     "required_fields": ["name", "kind", "file_path"]},
    }


def _require_results_search_task() -> Dict[str, Any]:
    return {
        "id": "T-search",
        "category": "symbol_lookup",
        "namespace": "source",
        "action": "search_source",
        "tool": "source_query",
        "arguments": {"action": "search_source", "query": "AActor"},
        "expected": {"min_results": 1, "require_results": True,
                     "required_fields": ["name", "kind", "file_path"]},
    }


def _legacy_min0_callers_task() -> Dict[str, Any]:
    return {
        "id": "T-callers-legacy",
        "category": "symbol_lookup",
        "namespace": "source",
        "action": "find_callers",
        "tool": "source_query",
        "arguments": {"action": "find_callers", "symbol": "UGameplayStatics"},
        "expected": {"min_results": 0, "required_fields": ["name", "kind", "file_path"]},
    }


def _negative_notfound_task() -> Dict[str, Any]:
    return {
        "id": "T-neg",
        "category": "negative_recovery",
        "namespace": "source",
        "action": "find_callers",
        "tool": "source_query",
        "arguments": {"action": "find_callers", "symbol": "UNonExistentClass999::DoesNotExist"},
        "expected": {"expect_error": True, "require_identifier": True,
                     "offending_identifier": "UNonExistentClass999::DoesNotExist",
                     "pass_threshold": 0.7},
    }


def _negative_resolve_task() -> Dict[str, Any]:
    return {
        "id": "T-neg-resolve",
        "category": "negative_recovery",
        "namespace": "source",
        "action": "find_callers",
        "tool": "source_query",
        "arguments": {"action": "find_callers", "symbol": "BeginPlay"},
        "expected": {"expect_error": False, "offending_identifier": "BeginPlay",
                     "pass_threshold": 0.7},
    }


def _negative_search_task() -> Dict[str, Any]:
    return {
        "id": "T-neg-search",
        "category": "negative_recovery",
        "namespace": "source",
        "action": "search_source",
        "tool": "source_query",
        "arguments": {"action": "search_source", "query": "UTotallyMadeUpClass_ZZZ999"},
        "expected": {"expect_error": True, "require_identifier": True,
                     "offending_identifier": "UTotallyMadeUpClass_ZZZ999",
                     "pass_threshold": 0.7},
    }


def _ergonomics_task() -> Dict[str, Any]:
    return {
        "id": "T-ergo",
        "category": "ergonomics_text",
        "namespace": "source",
        "action": "get_signature",
        "tool": "source_query",
        "arguments": {"action": "get_signature", "symbol": "FString::Printf"},
        "expected": {},
    }


def _health_task() -> Dict[str, Any]:
    return {
        "id": "T-health",
        "category": "health_check",
        "namespace": "source",
        "action": "health",
        "tool": "source_query",
        "arguments": {"action": "health", "include_counts": True},
        "expected": {},
    }


_SIGNATURE_TEXT = "static FString FString::Printf(const FmtType& Fmt, Types... Args)"


# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------

_FAILURES: List[str] = []


def check(name: str, condition: bool, detail: str = "") -> None:
    status = "PASS" if condition else "FAIL"
    print(f"[{status}] {name}" + (f" -- {detail}" if detail else ""))
    if not condition:
        _FAILURES.append(name)


def test_empty_require_results_callers_scores_low() -> None:
    # The exact loophole: isError=false + "No direct C++ callers found ..." sentinel.
    resp = _ok_text("No direct C++ callers found for 'AActor::BeginPlay'. This function "
                    "may be called via delegates, Blueprints, input bindings, or reflection.")
    row = _score(_require_results_callers_task(), resp)
    check("empty find_callers (require_results) is NOT a hit",
          row["direct_success"] is False, f"direct_success={row['direct_success']}")
    check("empty find_callers contributes 0 action_selection",
          row["action_selection_score"] == 0.0, f"sel={row['action_selection_score']}")
    check("empty find_callers is flagged expected_nonempty",
          row["expected_nonempty"] is True)


def test_populated_require_results_callers_scores_high() -> None:
    row = _score(_require_results_callers_task(), _ok_text(_POPULATED_CALLERS))
    check("populated find_callers (require_results) is a hit",
          row["direct_success"] is True, f"direct_success={row['direct_success']}")


def test_populated_search_source_is_field_complete() -> None:
    row = _score(_require_results_search_task(), _ok_text(_POPULATED_SEARCH))
    check("populated search_source (require_results) is a hit", row["direct_success"] is True)
    check("populated search_source parses >=1 structured row", row["results_count"] >= 1,
          f"results_count={row['results_count']}")
    check("populated search_source rows are field-complete",
          row["field_complete_count"] >= 1, f"complete={row['field_complete_count']}")


def test_legacy_min0_empty_still_passes() -> None:
    # Backward-compat: a min_results:0 task whose truthful answer is empty stays a hit,
    # so the gate only bites the curated known-nonempty set.
    resp = _ok_text("No function found matching 'UGameplayStatics'.")
    # Note: that is an error string only if isError set; here simulate a non-error empty.
    row = _score(_legacy_min0_callers_task(), _ok_text("No callees found for 'UGameplayStatics'."))
    check("legacy min_results:0 empty find_callers still passes",
          row["direct_success"] is True, f"direct_success={row['direct_success']}")
    check("legacy min_results:0 task is NOT expected_nonempty",
          row["expected_nonempty"] is False)


def test_negative_structured_error_with_hint_passes() -> None:
    resp = _error_text("No function found matching 'UNonExistentClass999::DoesNotExist'.",
                       hints=["Run source.search_source first to discover the indexed symbol name."])
    row = _score(_negative_notfound_task(), resp)
    check("negative: structured error naming identifier + hint passes",
          row["direct_success"] is True, f"q={row['negative_quality_score']}")
    check("negative: identifier-naming error scores 1.0 with hint",
          row["negative_quality_score"] == 1.0, f"q={row['negative_quality_score']}")


def test_negative_structured_error_no_hint_partial() -> None:
    resp = _error_text("No function found matching 'UNonExistentClass999::DoesNotExist'.")
    row = _score(_negative_notfound_task(), resp)
    check("negative: error naming identifier but no hint scores 0.7",
          row["negative_quality_score"] == 0.7, f"q={row['negative_quality_score']}")
    check("negative: 0.7 meets default pass_threshold", row["direct_success"] is True)


def test_negative_transport_crash_scores_zero() -> None:
    row = _score(_negative_notfound_task(), _transport_crash())
    check("negative: transport crash scores 0.0",
          row["negative_quality_score"] == 0.0, f"q={row['negative_quality_score']}")
    check("negative: transport crash FAILS the negative task", row["direct_success"] is False)


def test_negative_silent_empty_success_scores_zero() -> None:
    # Bad input answered with a clean non-error success that drops the identifier.
    resp = _ok_text("OK")
    row = _score(_negative_notfound_task(), resp)
    check("negative: silent non-error success on bad input scores 0.0",
          row["negative_quality_score"] == 0.0, f"q={row['negative_quality_score']}")
    check("negative: silent success FAILS the negative task", row["direct_success"] is False)


def test_negative_resolve_populated_passes() -> None:
    row = _score(_negative_resolve_task(), _ok_text(_POPULATED_CALLERS))
    check("negative-resolve: unqualified name that resolves with data passes",
          row["direct_success"] is True, f"q={row['negative_quality_score']}")
    check("negative-resolve: resolved input scores 1.0",
          row["negative_quality_score"] == 1.0)


def test_negative_resolve_rejected_fails() -> None:
    resp = _error_text("No function found matching 'BeginPlay'.")
    row = _score(_negative_resolve_task(), resp)
    check("negative-resolve: rejecting a resolvable input scores 0.0",
          row["negative_quality_score"] == 0.0, f"q={row['negative_quality_score']}")


# ---------------------------------------------------------------------------
# Structured-tool-results regressions (payload only in structuredContent).
#
# Every one of these FAILS on the pre-fix scorer, which scanned the transport
# content[0].text stub: the empty-result gate went dead (silent inflation to 1.0),
# the negative-recovery partial-credit rungs became unreachable (silent 0.0), and
# ergonomics degenerated into "1 - isError rate".  Each case asserts the SAME verdict
# as its legacy-envelope twin above, so the scorer is envelope-invariant.
# ---------------------------------------------------------------------------

def test_structured_empty_require_results_callers_scores_low() -> None:
    resp = _structured_ok_text(
        "No direct C++ callers found for 'FString::Printf'. This function may be called "
        "via delegates, Blueprints, input bindings, or reflection."
    )
    row = _score(_require_results_callers_task(), resp)
    check("structured: empty find_callers (require_results) is NOT a hit",
          row["direct_success"] is False, f"direct_success={row['direct_success']}")
    check("structured: empty find_callers contributes 0 action_selection",
          row["action_selection_score"] == 0.0, f"sel={row['action_selection_score']}")
    check("structured: empty find_callers is flagged expected_nonempty",
          row["expected_nonempty"] is True)
    check("structured: empty find_callers has_data is False",
          row["evidence"]["has_data"] is False, f"evidence={row['evidence']}")


def test_structured_empty_payload_is_never_a_hit() -> None:
    # An empty structuredContent ({}) carries no data at all and must not pass the
    # require_results gate on the strength of the non-empty transport stub.
    row = _score(_require_results_search_task(), _structured_ok({}))
    check("structured: empty {} payload is NOT a lookup hit",
          row["direct_success"] is False and row["results_count"] == 0,
          f"direct_success={row['direct_success']} count={row['results_count']}")


def test_structured_populated_lookups_score_high() -> None:
    callers = _score(_require_results_callers_task(), _structured_ok_text(_POPULATED_CALLERS))
    check("structured: populated find_callers (require_results) is a hit",
          callers["direct_success"] is True, f"direct_success={callers['direct_success']}")

    search = _score(_require_results_search_task(), _structured_ok_text(_POPULATED_SEARCH))
    check("structured: populated search_source is a hit", search["direct_success"] is True)
    check("structured: populated search_source parses >=1 structured row",
          search["results_count"] >= 1, f"results_count={search['results_count']}")
    check("structured: populated search_source rows are field-complete",
          search["field_complete_count"] >= 1, f"complete={search['field_complete_count']}")


def test_structured_review_context_empty_sentinel_is_a_miss() -> None:
    task = {
        "id": "T-review",
        "category": "review_context_lookup",
        "namespace": "source",
        "action": "review_context",
        "tool": "source_query",
        "arguments": {"action": "review_context", "symbol": "AActor"},
        "expected": {"min_results": 1, "require_results": True},
    }
    row = _score(task, _structured_ok_text("No results found for 'AActor'."))
    check("structured: empty review_context (require_results) is NOT a hit",
          row["direct_success"] is False and row["action_selection_score"] == 0.0,
          f"direct_success={row['direct_success']} sel={row['action_selection_score']}")

    populated = _score(task, _structured_ok({
        "risk": {"name": "AActor", "kind": "class", "file": "Engine/Actor.h", "line": 256},
        "top_risks": [{"name": "AActor::Tick", "kind": "function",
                       "file": "Engine/Actor.cpp", "line": 1200}],
    }))
    check("structured: populated review_context is a hit",
          populated["direct_success"] is True and populated["results_count"] >= 1,
          f"direct_success={populated['direct_success']} count={populated['results_count']}")


def test_structured_negative_non_error_sentinel_scores_half() -> None:
    # isError=false + "No results found for '<bad symbol>'" names the problem without
    # erroring: the 0.5 partial-credit rung, unreachable while only the stub was scanned.
    row = _score(_negative_search_task(),
                 _structured_ok_text("No results found for 'UTotallyMadeUpClass_ZZZ999'."))
    check("structured: non-error response naming the bad symbol scores 0.5",
          row["negative_quality_score"] == 0.5, f"q={row['negative_quality_score']}")
    check("structured: 0.5 still FAILS the 0.7 pass threshold",
          row["direct_success"] is False, f"direct_success={row['direct_success']}")


def test_structured_negative_silent_success_still_scores_zero() -> None:
    # A clean success that drops the identifier must stay at 0.0 (no rung inflation).
    row = _score(_negative_search_task(), _structured_ok({"results": [], "note": "done"}))
    check("structured: silent non-error success on bad input scores 0.0",
          row["negative_quality_score"] == 0.0, f"q={row['negative_quality_score']}")


def test_structured_negative_error_with_structured_hints_scores_one() -> None:
    # Compact+structured errors keep a human message but move hints/related_actions
    # into structuredContent; reading only the result top level capped this at 0.7.
    resp = _structured_error(
        "No results found for 'UTotallyMadeUpClass_ZZZ999'.",
        hints=["Run source.search_source first to discover the indexed symbol name."],
        related=["source.search_source"],
        error_code="coverage_miss",
    )
    row = _score(_negative_search_task(), resp)
    check("structured: error carrying structuredContent hints scores 1.0",
          row["negative_quality_score"] == 1.0, f"q={row['negative_quality_score']}")
    check("structured: self-correcting error PASSES the negative task",
          row["direct_success"] is True)
    check("structured: hint flag is set from the structured payload",
          row["evidence"]["has_hint"] is True and row["evidence"]["names_identifier"] is True,
          f"evidence={row['evidence']}")


def test_structured_negative_error_without_hint_stays_partial() -> None:
    # No hint anywhere -> still 0.7, i.e. the structured payload must not manufacture one.
    row = _score(_negative_search_task(),
                 _structured_error("No results found for 'UTotallyMadeUpClass_ZZZ999'."))
    check("structured: identifier-naming error without a hint scores 0.7",
          row["negative_quality_score"] == 0.7, f"q={row['negative_quality_score']}")


def test_structured_negative_resolve_empty_scores_zero() -> None:
    # expect_error=false: an unqualified name answered with the truthful empty sentinel
    # is a FAILURE, not a pass.  The stub-scanning scorer inflated this to 1.0.
    row = _score(_negative_resolve_task(),
                 _structured_ok_text("No direct C++ callers found for 'BeginPlay'."))
    check("structured: truthfully-empty unqualified resolve scores 0.0",
          row["negative_quality_score"] == 0.0, f"q={row['negative_quality_score']}")
    check("structured: empty unqualified resolve FAILS the negative task",
          row["direct_success"] is False)

    populated = _score(_negative_resolve_task(), _structured_ok_text(_POPULATED_CALLERS))
    check("structured: unqualified resolve WITH data still scores 1.0",
          populated["negative_quality_score"] == 1.0 and populated["direct_success"] is True,
          f"q={populated['negative_quality_score']}")


def test_structured_ergonomics_scores_the_answer_not_the_stub() -> None:
    ok = _score(_ergonomics_task(), _structured_ok_text(_SIGNATURE_TEXT))
    check("structured: ergonomics answer text passes",
          ok["direct_success"] is True, f"direct_success={ok['direct_success']}")
    check("structured: ergonomics evidence reports the ANSWER, not the 26-char stub",
          ok["evidence"]["response_len"] == len(_SIGNATURE_TEXT)
          and ok["evidence"]["response_preview"].startswith("static FString"),
          f"evidence={ok['evidence']}")

    empty = _score(_ergonomics_task(), _structured_ok({}))
    check("structured: ergonomics with an EMPTY payload fails",
          empty["direct_success"] is False and empty["results_count"] == 0,
          f"direct_success={empty['direct_success']}")

    errored = _score(_ergonomics_task(), _structured_ok_text("Error: symbol is not indexed."))
    check("structured: ergonomics answer starting with 'Error' fails",
          errored["direct_success"] is False, f"direct_success={errored['direct_success']}")


def test_structured_health_check_is_envelope_invariant() -> None:
    payload = {"status": "ok", "row_counts": {"symbols": 1234}}
    structured = _score(_health_task(), _structured_ok(payload))
    legacy = _score(_health_task(), _legacy_json(payload))
    check("structured: health with status + counts passes",
          structured["direct_success"] is True, f"row={structured['evidence']}")
    check("health: structured and legacy envelopes agree",
          structured["direct_success"] == legacy["direct_success"]
          and structured["evidence"]["symbol_count"] == legacy["evidence"]["symbol_count"] == 1234,
          f"structured={structured['evidence']} legacy={legacy['evidence']}")

    empty = _score(_health_task(), _structured_ok({}))
    check("structured: health with an EMPTY payload fails",
          empty["direct_success"] is False, f"evidence={empty['evidence']}")


def test_legacy_serialized_json_envelope_still_scores() -> None:
    # The flag can be off in other checkouts: the REAL legacy wire shape serializes the
    # action payload into content[0].text.  Both verdicts must match structured mode.
    empty = _score(
        _require_results_callers_task(),
        _legacy_json({"content": [{"type": "text",
                                   "text": "No direct C++ callers found for 'FString::Printf'."}]}),
    )
    check("legacy JSON: empty find_callers (require_results) is NOT a hit",
          empty["direct_success"] is False, f"direct_success={empty['direct_success']}")

    populated = _score(
        _require_results_search_task(),
        _legacy_json({"content": [{"type": "text", "text": _POPULATED_SEARCH}]}),
    )
    check("legacy JSON: populated search_source is a hit with structured rows",
          populated["direct_success"] is True and populated["results_count"] >= 1,
          f"direct_success={populated['direct_success']} count={populated['results_count']}")

    ergonomics = _score(
        _ergonomics_task(),
        _legacy_json({"content": [{"type": "text", "text": _SIGNATURE_TEXT}]}),
    )
    check("legacy JSON: ergonomics scores the answer text",
          ergonomics["direct_success"] is True
          and ergonomics["evidence"]["response_len"] == len(_SIGNATURE_TEXT),
          f"evidence={ergonomics['evidence']}")

    negative = _score(
        _negative_search_task(),
        _error_text("No results found for 'UTotallyMadeUpClass_ZZZ999'.",
                    hints=["Run source.search_source first."]),
    )
    check("legacy: top-level hints still reach the 1.0 rung",
          negative["negative_quality_score"] == 1.0, f"q={negative['negative_quality_score']}")


def test_mcp_call_non_object_json_is_protocol_error() -> None:
    class FakeResponse:
        headers: Dict[str, str] = {}

        def __enter__(self):
            return self

        def __exit__(self, exc_type, exc, tb):
            return False

        def read(self):
            return b"[]"

    original = sib.urllib.request.urlopen
    sib.urllib.request.urlopen = lambda request, timeout=45.0: FakeResponse()
    try:
        response = sib.mcp_call("http://unused", "monolith_status", {}, 1.0)
    finally:
        sib.urllib.request.urlopen = original
    check(
        "mcp_call: non-object JSON becomes protocol_error",
        response.get("protocol_error") is True and "top-level JSON" in response.get("error", ""),
        f"response={response}",
    )


def test_protocol_errors_cannot_false_pass_empty_or_ergonomics_tasks() -> None:
    rpc_error = {"error": {"code": -32603, "message": "gateway failed"}}
    lookup = _score(_legacy_min0_callers_task(), rpc_error)
    ergonomics_task = {
        "id": "SIB-PROTO",
        "category": "ergonomics_text",
        "namespace": "source",
        "action": "get_signature",
        "tool": "source_query",
        "arguments": {"action": "get_signature", "symbol": "AActor"},
        "expected": {},
    }
    ergonomics = _score(ergonomics_task, rpc_error)
    check(
        "protocol: min_results=0 empty lookup cannot pass",
        lookup["direct_success"] is False and lookup["failure_kind"] == "protocol_error",
        f"row={lookup}",
    )
    check(
        "protocol: JSON-RPC error text cannot pass ergonomics",
        ergonomics["direct_success"] is False and ergonomics["protocol_error"] is True,
        f"row={ergonomics}",
    )


def test_status_failures_clear_stale_outputs_and_write_no_summary() -> None:
    calls = 0

    def fake_score(url, task, timeout_s):
        nonlocal calls
        calls += 1
        return _run_row(task)

    with tempfile.TemporaryDirectory() as tmp:
        root = pathlib.Path(tmp)
        output = root / "run"
        output.mkdir()
        for name in sib.RUN_OUTPUT_FILENAMES:
            (output / name).write_text("stale", encoding="utf-8")
        result = _run_with_fake_rows(
            [_run_task(1)],
            fake_score,
            output_dir=output,
            status_response={"parse_error": True, "raw": "not-json"},
        )
        check(
            "status protocol failure aborts before tasks",
            result["failure_stage"] == "status_preflight"
            and result["failure_kind"] == "protocol_error",
            f"stage={result.get('failure_stage')} kind={result.get('failure_kind')}",
        )
        check("status failure writes run_failure", (output / "run_failure.json").exists())
        check("status failure writes no summary", not (output / "summary.json").exists())
        check("status failure removes stale partial", not (output / "partial_summary.json").exists())
        check("status failure removes stale per_task", not (output / "per_task.json").exists())

        invalid_output = root / "invalid-status"
        invalid = _run_with_fake_rows(
            [_run_task(1)],
            fake_score,
            output_dir=invalid_output,
            status_response=_ok_text("{}"),
        )
        check(
            "status payload requires server_running=true",
            invalid["failure_kind"] == "invalid_status_payload"
            and not (invalid_output / "summary.json").exists(),
            f"kind={invalid.get('failure_kind')}",
        )

        transport_output = root / "transport-status"
        transport = _run_with_fake_rows(
            [_run_task(1)],
            fake_score,
            output_dir=transport_output,
            status_response={"transport_error": True, "status": 503, "raw": "status down"},
        )
        check(
            "status transport failure aborts before tasks",
            transport["failure_kind"] == "transport_error"
            and transport["last_transport_status"] == 503
            and not (transport_output / "summary.json").exists(),
            f"kind={transport.get('failure_kind')} status={transport.get('last_transport_status')}",
        )

        rpc_output = root / "rpc-status"
        rpc = _run_with_fake_rows(
            [_run_task(1)],
            fake_score,
            output_dir=rpc_output,
            status_response={"error": {"code": -32603, "message": "gateway failed"}},
        )
        check(
            "status JSON-RPC error aborts before tasks",
            rpc["failure_kind"] == "protocol_error"
            and not (rpc_output / "summary.json").exists(),
            f"kind={rpc.get('failure_kind')}",
        )
        check("all invalid statuses execute zero tasks", calls == 0, f"calls={calls}")


def test_canonical_stale_catalog_identity_aborts_before_task_calls() -> None:
    task = _run_task(1)
    status_calls = 0
    task_calls = 0

    def fake_call(url, tool, arguments, timeout_s=45.0):  # noqa: ANN001
        nonlocal status_calls
        if tool != "monolith_status":
            raise AssertionError(f"unexpected MCP call before catalog gate: {tool}")
        status_calls += 1
        return _status_response("sha256:live")

    def fake_score(url, scored_task, timeout_s):  # noqa: ANN001
        nonlocal task_calls
        task_calls += 1
        return _run_row(scored_task)

    with tempfile.TemporaryDirectory() as tmp:
        root = pathlib.Path(tmp)
        output = root / "run"
        corpus = sib.TaskCorpus(
            tasks=[task],
            canonical=True,
            comparable=True,
            mode="canonical",
            manifest={"catalog_version": "sha256:stale"},
            manifest_path=root / "manifest.json",
        )
        original_load = sib.load_task_corpus
        original_call = sib.mcp_call
        original_score = sib.score_task
        sib.load_task_corpus = lambda *args, **kwargs: corpus
        sib.mcp_call = fake_call
        sib.score_task = fake_score
        try:
            result = sib.run_benchmark(
                "http://unused",
                root / "tasks.jsonl",
                output,
                "stale-catalog",
                1.0,
            )
        finally:
            sib.load_task_corpus = original_load
            sib.mcp_call = original_call
            sib.score_task = original_score

        failure = json.loads((output / "run_failure.json").read_text(encoding="utf-8"))
        check(
            "stale canonical catalog aborts before task calls",
            result["completion_status"] == "aborted_catalog_identity_mismatch"
            and result["failure_stage"] == "status_preflight"
            and result["failure_kind"] == "catalog_identity_mismatch"
            and result["completed_task_count"] == 0
            and result["expected_catalog_version"] == "sha256:stale"
            and result["observed_catalog_version"] == "sha256:live"
            and failure["failure_kind"] == "catalog_identity_mismatch"
            and status_calls == 1
            and task_calls == 0,
            f"result={result} status_calls={status_calls} task_calls={task_calls}",
        )
        check("stale catalog writes no summary", not (output / "summary.json").exists())
        check("stale catalog writes no per_task", not (output / "per_task.json").exists())
        check("stale catalog writes no per_task jsonl", not (output / "per_task.jsonl").exists())


def test_three_consecutive_transport_failures_abort_on_third_task() -> None:
    tasks = [_run_task(index) for index in range(1, 7)]
    calls: List[str] = []

    def fake_score(url, task, timeout_s):
        calls.append(task["id"])
        return _run_row(task, transport=True, status=503, raw="down")

    with tempfile.TemporaryDirectory() as tmp:
        output = pathlib.Path(tmp) / "run"
        result = _run_with_fake_rows(tasks, fake_score, output_dir=output)
        check(
            "transport: three consecutive failures abort on third",
            result["transport_gate_reason"] == "consecutive_transport_failures"
            and result["completed_task_count"] == 3
            and len(calls) == 3,
            f"gate={result.get('transport_gate_reason')} completed={result.get('completed_task_count')} calls={len(calls)}",
        )
        check("transport abort writes no summary", not (output / "summary.json").exists())


def test_fraction_gate_aborts_at_twentieth_and_keeps_transport_identity() -> None:
    tasks = [_run_task(index) for index in range(1, 23)]
    calls = 0

    def fake_score(url, task, timeout_s):
        nonlocal calls
        calls += 1
        failed = calls in {1, 6}
        return _run_row(
            task,
            transport=failed,
            status=503 if failed else None,
            raw=f"down-{calls}" if failed else "",
        )

    with tempfile.TemporaryDirectory() as tmp:
        result = _run_with_fake_rows(
            tasks, fake_score, output_dir=pathlib.Path(tmp) / "run"
        )
        check(
            "transport: cumulative fraction aborts at sample 20",
            result["transport_gate_reason"] == "transport_failed_fraction"
            and result["completed_task_count"] == 20,
            f"gate={result.get('transport_gate_reason')} completed={result.get('completed_task_count')}",
        )
        check(
            "transport: success-triggered fraction gate keeps last failure identity",
            result["last_task_id"] == "SIB-R-6"
            and result["last_transport_status"] == 503
            and result["last_transport_error_raw"] == "down-6",
            f"last={result.get('last_task_id')} status={result.get('last_transport_status')}",
        )


def test_exact_five_percent_succeeds_and_cleans_partial_failure() -> None:
    tasks = [_run_task(index) for index in range(1, 21)]
    calls = 0

    def fake_score(url, task, timeout_s):
        nonlocal calls
        calls += 1
        return _run_row(task, transport=calls == 1, status=503, raw="one-down")

    with tempfile.TemporaryDirectory() as tmp:
        output = pathlib.Path(tmp) / "run"
        output.mkdir()
        (output / "run_failure.json").write_text("stale", encoding="utf-8")
        (output / "partial_summary.json").write_text("stale", encoding="utf-8")
        result = _run_with_fake_rows(tasks, fake_score, output_dir=output)
        check(
            "transport: exact 5 percent is allowed",
            result["run_valid"] is True and result["transport_failure_count"] == 1,
            f"valid={result.get('run_valid')} failures={result.get('transport_failure_count')}",
        )
        check("success writes summary", (output / "summary.json").exists())
        check("success removes partial", not (output / "partial_summary.json").exists())
        check("success removes stale failure", not (output / "run_failure.json").exists())


def test_short_population_transport_fraction_fails_at_finalize() -> None:
    tasks = [_run_task(index) for index in range(1, 11)]
    calls = 0

    def fake_score(url, task, timeout_s):
        nonlocal calls
        calls += 1
        return _run_row(task, transport=calls == 1, status=503, raw="one-down")

    with tempfile.TemporaryDirectory() as tmp:
        output = pathlib.Path(tmp) / "run"
        result = _run_with_fake_rows(
            tasks,
            fake_score,
            output_dir=output,
            max_consecutive_transport_failures=20,
        )
        check(
            "transport: short population is rejected at finalize",
            result["completion_status"] == "completed_transport_failure_budget_exceeded"
            and result["transport_gate_reason"] == "final_transport_failed_fraction"
            and result["last_task_id"] == "SIB-R-1",
            f"completion={result.get('completion_status')} gate={result.get('transport_gate_reason')}",
        )
        check("short invalid run writes no summary", not (output / "summary.json").exists())


def test_nontransport_response_resets_transport_streak() -> None:
    tasks = [_run_task(index) for index in range(1, 6)]
    failures = {1, 2, 4, 5}
    calls = 0

    def fake_score(url, task, timeout_s):
        nonlocal calls
        calls += 1
        row = _run_row(task, transport=calls in failures, raw="down")
        if calls == 3:
            row["response_is_error"] = True
            row["direct_success"] = True
        return row

    with tempfile.TemporaryDirectory() as tmp:
        result = _run_with_fake_rows(
            tasks,
            fake_score,
            output_dir=pathlib.Path(tmp) / "run",
            max_transport_failed_fraction=1.0,
        )
        check(
            "transport: valid semantic error resets consecutive streak",
            result["run_valid"] is True and result["consecutive_transport_failures"] == 2,
            f"valid={result.get('run_valid')} consecutive={result.get('consecutive_transport_failures')}",
        )


def test_task_protocol_and_runner_exception_write_invalid_artifacts() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        root = pathlib.Path(tmp)
        protocol_output = root / "protocol"
        protocol = _run_with_fake_rows(
            [_run_task(1)],
            lambda url, task, timeout_s: _run_row(task, protocol=True, raw="bad envelope"),
            output_dir=protocol_output,
        )
        check(
            "task protocol error invalidates run",
            protocol["completion_status"] == "aborted_protocol_error"
            and not (protocol_output / "summary.json").exists(),
            f"completion={protocol.get('completion_status')}",
        )

        exception_output = root / "exception"

        def explode(url, task, timeout_s):
            raise RuntimeError("score exploded")

        exception = _run_with_fake_rows(
            [_run_task(1)], explode, output_dir=exception_output
        )
        row = json.loads((exception_output / "per_task.jsonl").read_text(encoding="utf-8"))
        check(
            "runner exception preserves trigger row and invalidates run",
            exception["completion_status"] == "aborted_runner_exception"
            and row["failure_kind"] == "runner_exception"
            and "score exploded" in row["error"]
            and not (exception_output / "summary.json").exists(),
            f"completion={exception.get('completion_status')} row_kind={row.get('failure_kind')}",
        )


def test_generate_always_enumerates_and_validates_live_catalog() -> None:
    curated_tasks = sib.build_static_tasks()
    curated_actions = _curated_source_actions()
    added_live_actions = ["get_module_info", "audit_module_dep_reality"]
    live_actions = curated_actions + added_live_actions
    calls: List[str] = []
    original = sib.mcp_call
    sib.mcp_call = _generation_router(live_actions, calls=calls)
    try:
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            manifest = sib.generate_tasks(
                "http://offline", 0, root / "tasks.jsonl", root / "manifest.json"
            )
            generated_tasks = [
                json.loads(line)
                for line in (root / "tasks.jsonl").read_text(encoding="utf-8").splitlines()
                if line.strip()
            ]
    finally:
        sib.mcp_call = original

    validation = manifest["catalog_validation"]
    check(
        "generate always performs status/discover/status catalog validation",
        calls == ["monolith_status", "monolith_discover", "monolith_status"],
        f"calls={calls}",
    )
    check(
        "generate preserves existing curated tasks before schema-only additions",
        generated_tasks[:len(curated_tasks)] == curated_tasks,
        f"generated={len(generated_tasks)} curated={len(curated_tasks)}",
    )
    check(
        "generate records stable catalog identity and closes action coverage",
        manifest["catalog_version"] == "sha256:test"
        and validation["live_action_count"] == len(set(live_actions))
        and validation["covered_action_count"] == len(set(live_actions))
        and validation["preexisting_covered_action_count"] == len(curated_actions)
        and validation["generated_schema_coverage_actions"] == sorted(added_live_actions)
        and validation["uncovered_action_count"] == 0,
        f"validation={validation}",
    )
    check(
        "manifest schema action list includes curated and generated coverage",
        manifest["schema_actions"] == sorted(set(sib.SCHEMA_ACTIONS) | set(added_live_actions)),
        f"schema_actions={manifest.get('schema_actions')}",
    )
    appended = generated_tasks[len(curated_tasks):]
    check(
        "unreferenced live action receives exact schema-only coverage",
        len(appended) == 2
        and [task["action"] for task in appended] == sorted(added_live_actions)
        and all(task["category"] == "schema_field_presence" for task in appended)
        and all(task["tool"] == "monolith_discover" for task in appended)
        and all(task["arguments"] == {
            "namespace": "source", "action": task["action"], "mode": "schema"
        } for task in appended),
        f"appended={appended}",
    )


def test_generate_rejects_missing_live_action_without_overwrite() -> None:
    curated_actions = _curated_source_actions()
    missing_action = "search_source"
    live_actions = [action for action in curated_actions if action != missing_action]
    original = sib.mcp_call
    sib.mcp_call = _generation_router(live_actions)
    try:
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            tasks_path = root / "tasks.jsonl"
            manifest_path = root / "manifest.json"
            tasks_path.write_text("TASKS_SENTINEL\n", encoding="utf-8")
            manifest_path.write_text("MANIFEST_SENTINEL\n", encoding="utf-8")
            try:
                sib.generate_tasks("http://offline", 0, tasks_path, manifest_path)
            except RuntimeError as exc:
                error = str(exc)
            else:
                raise AssertionError("missing live action must fail generation")
            check(
                "generate names missing curated action",
                missing_action in error,
                f"error={error}",
            )
            check(
                "missing catalog action leaves canonical outputs untouched",
                tasks_path.read_text(encoding="utf-8") == "TASKS_SENTINEL\n"
                and manifest_path.read_text(encoding="utf-8") == "MANIFEST_SENTINEL\n",
            )
    finally:
        sib.mcp_call = original


def test_generate_refuses_blind_generic_top_up() -> None:
    curated_count = len(sib.build_static_tasks())
    live_actions = _curated_source_actions() + ["future_schema_specific_action"]
    original = sib.mcp_call
    sib.mcp_call = _generation_router(live_actions)
    try:
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            try:
                sib.generate_tasks(
                    "http://offline",
                    curated_count + 2,
                    root / "tasks.jsonl",
                    root / "manifest.json",
                )
            except RuntimeError as exc:
                error = str(exc)
            else:
                raise AssertionError("min-task deficit must not invent generic query tasks")
            check(
                "top-up deficit requires schema-verified curated tasks",
                "generic live-action/query top-ups are forbidden" in error,
                f"error={error}",
            )
            check(
                "top-up failure publishes no task or manifest artifact",
                not (root / "tasks.jsonl").exists()
                and not (root / "manifest.json").exists(),
            )
    finally:
        sib.mcp_call = original


def test_generate_rejects_catalog_change_during_discovery() -> None:
    original = sib.mcp_call
    sib.mcp_call = _generation_router(
        _curated_source_actions(),
        start_catalog_version="sha256:start",
        end_catalog_version="sha256:end",
    )
    try:
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            try:
                sib.generate_tasks(
                    "http://offline", 0, root / "tasks.jsonl", root / "manifest.json"
                )
            except RuntimeError as exc:
                error = str(exc)
            else:
                raise AssertionError("catalog identity change must fail generation")
            check(
                "generation fails closed on catalog identity change",
                "source catalog changed during generation" in error,
                f"error={error}",
            )
    finally:
        sib.mcp_call = original


def test_manifest_run_gates_match_runner_defaults() -> None:
    original = sib.mcp_call
    sib.mcp_call = _generation_router(_curated_source_actions())
    try:
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            manifest = sib.generate_tasks(
                "http://unused", 0, root / "tasks.jsonl", root / "manifest.json"
            )
            gates = manifest["run_gates"]
            check(
                "manifest run gates match runner defaults",
                gates["max_transport_failed_fraction"] == sib.DEFAULT_MAX_TRANSPORT_FAILED_FRACTION
                and gates["max_consecutive_transport_failures"] == sib.DEFAULT_MAX_CONSECUTIVE_TRANSPORT_FAILURES
                and gates["min_transport_fraction_sample"] == sib.DEFAULT_MIN_TRANSPORT_FRACTION_SAMPLES
                and gates["canonical_catalog_version_mismatch_aborts_before_tasks"] is True
                and gates["invalid_run_writes_summary"] is False,
                f"gates={gates}",
            )
    finally:
        sib.mcp_call = original


def test_input_fingerprint_uses_only_engine_source_database() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        root = pathlib.Path(tmp)
        saved = root / "Saved"
        saved.mkdir()
        (saved / "EngineSource.db").write_bytes(b"source-index-authoritative")

        inputs = sib.build_benchmark_inputs("SourceIndex", plugin_root=root)

    check(
        "SourceIndex fingerprints only the authoritative EngineSource database",
        [row["path"] for row in inputs["database_files"]]
        == ["Saved/EngineSource.db"],
        f"database_files={inputs['database_files']}",
    )


def test_main_returns_nonzero_for_invalid_run() -> None:
    original = sib.run_benchmark
    sib.run_benchmark = lambda *args, **kwargs: {"run_valid": False}
    try:
        rc = sib.main(["run", "--output-dir", "unused", "--label", "invalid"])
    finally:
        sib.run_benchmark = original
    check("main returns nonzero for invalid run", rc == 1, f"rc={rc}")


def test_aggregate_weights_sum_to_one_and_react() -> None:
    # Build a tiny run: one passing require_results lookup + one negative pass/fail and
    # confirm the score reacts (formula coefficients sum to 1.0).
    tasks = [_require_results_callers_task(), _negative_notfound_task()]
    good_rows = [
        _score(_require_results_callers_task(), _ok_text(_POPULATED_CALLERS)),
        _score(_negative_notfound_task(),
               _error_text("No function found matching 'UNonExistentClass999::DoesNotExist'.",
                           hints=["Run source.search_source first."])),
    ]
    bad_rows = [
        _score(_require_results_callers_task(),
               _ok_text("No direct C++ callers found for 'AActor::BeginPlay'.")),
        _score(_negative_notfound_task(), _transport_crash()),
    ]
    good_m = sib.aggregate("good", {}, tasks, good_rows)["metrics"]
    bad_m = sib.aggregate("bad", {}, tasks, bad_rows)["metrics"]
    good = good_m["source_index_score"]
    bad = bad_m["source_index_score"]
    check("aggregate: healthy run scores higher than broken run",
          good > bad, f"good={good} bad={bad}")
    # With no health tasks the stale term contributes its neutral 0.10; the meaningful
    # signal is that the two driven dimensions (symbol_hit + negative_recovery) collapsed.
    check("aggregate: broken run symbol_hit_rate collapses to 0",
          bad_m["symbol_hit_rate"] == 0.0, f"hit={bad_m['symbol_hit_rate']}")
    check("aggregate: broken run negative_recovery_rate collapses to 0",
          bad_m["negative_recovery_rate"] == 0.0, f"neg={bad_m['negative_recovery_rate']}")
    check("aggregate: broken run composite is well below healthy",
          bad <= 0.10 and good - bad >= 0.3, f"good={good} bad={bad}")


def test_formula_coefficients_sum_to_one() -> None:
    # Drive every dimension to 1.0 and assert the composite is exactly 1.0.
    class _Row(dict):
        pass

    def lk(success: bool, rc: int, fc: int, nonempty: bool) -> Dict[str, Any]:
        return {"category": "symbol_lookup", "direct_success": success,
                "results_count": rc, "field_complete_count": fc,
                "expected_nonempty": nonempty, "negative_quality_score": None}

    rows = [
        lk(True, 2, 2, True),
        {"category": "schema_field_presence", "direct_success": True, "results_count": 0,
         "field_complete_count": 0, "expected_nonempty": False, "negative_quality_score": None},
        {"category": "health_check", "direct_success": True, "results_count": 0,
         "field_complete_count": 0, "expected_nonempty": False, "negative_quality_score": None},
        {"category": "ergonomics_text", "direct_success": True, "results_count": 1,
         "field_complete_count": 1, "expected_nonempty": False, "negative_quality_score": None},
        {"category": "negative_recovery", "direct_success": True, "results_count": 0,
         "field_complete_count": 0, "expected_nonempty": False, "negative_quality_score": 1.0},
    ]
    score = sib.aggregate("perfect", {}, [], rows)["metrics"]["source_index_score"]
    check("formula: all-perfect dimensions sum to exactly 1.0", abs(score - 1.0) < 1e-9,
          f"score={score}")


def main() -> int:
    tests = [v for k, v in sorted(globals().items()) if k.startswith("test_") and callable(v)]
    for test in tests:
        test()
    print()
    if _FAILURES:
        print(f"{len(_FAILURES)} FAILED: {', '.join(_FAILURES)}")
        return 1
    print(f"All {len(tests)} test functions passed.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
