#!/usr/bin/env python3
"""Offline unit tests for the SourceIndex benchmark scoring branches.

No live editor / MCP server is touched.  Each test fabricates an MCP
``tools/call`` response in the real envelope shape::

    {"result": {"content": [{"type": "text", "text": <text>}], "isError": <bool>},
     "request": {...}}

and asserts the NEW scoring behaviour:

  * an empty ``find_callers`` response on a require_results task scores LOW,
  * a correct multi-result response scores HIGH,
  * a not-found symbol that returns a structured, identifier-naming error passes
    the negative_recovery category, while a transport crash / silent empty does not.

Run::

    python Plugins/Monolith/Scripts/test_source_index_benchmark.py
"""

from __future__ import annotations

import importlib.util
import pathlib
import sys
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
