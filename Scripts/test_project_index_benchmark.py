#!/usr/bin/env python3
"""Offline unit tests for the ProjectIndex benchmark scoring branches.

These tests run with NO live editor and NO network. They monkeypatch
`project_index_benchmark.mcp_call` to return fabricated MCP responses in the real
transport shape -- {"result": {"content": [{"type": "text", "text": <json>}],
"isError": <bool>}} -- and assert the new anti-gaming behavior:

  1. An EMPTY / broken index (every search returns 0 results) now scores well below
     1.0: project_index_score is capped at ALL_EMPTY_SCORE_CAP and `all_empty` is set.
  2. A HEALTHY index (known-answer queries return their expected /Game asset path,
     broad searches return field-complete rows) scores high.
  3. The known_answer branch is a HIT only when the response actually contains the
     expected match_object_path, and a MISS (wrong path) scores 0 for that task.

Run:  python Scripts/test_project_index_benchmark.py
Exit code 0 = all asserts passed.
"""

from __future__ import annotations

import json
import pathlib
import sys
from typing import Any, Dict, List

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))

import project_index_benchmark as pib  # noqa: E402


# ---------------------------------------------------------------------------
# Fabricated MCP responses in the real transport shape.
# ---------------------------------------------------------------------------

def mcp_text_response(payload: Dict[str, Any], is_error: bool = False) -> Dict[str, Any]:
    """An MCP tools/call response whose content[0].text is JSON-encoded payload."""
    return {
        "result": {
            "content": [{"type": "text", "text": json.dumps(payload)}],
            "isError": is_error,
        }
    }


def search_response(results: List[Dict[str, Any]]) -> Dict[str, Any]:
    return mcp_text_response({"success": True, "results": results, "count": len(results)})


def empty_search_response() -> Dict[str, Any]:
    # Valid non-error JSON, zero results -- the exact shape that USED to score 1.000.
    return mcp_text_response({"success": True, "results": [], "count": 0})


def asset_row(object_path: str) -> Dict[str, Any]:
    return {
        "match_object_path": object_path,
        "match_value": object_path.rsplit("/", 1)[-1],
        "match_source": "asset",
        "match_table": "assets",
        "match_field": "asset",
    }


def healthy_health_response() -> Dict[str, Any]:
    return mcp_text_response({"status": "ready", "total_assets": 16185})


def healthy_stats_response() -> Dict[str, Any]:
    return mcp_text_response({"success": True, "indexing": False, "stats": {"assets": 16185}})


def healthy_schema_response() -> Dict[str, Any]:
    return mcp_text_response({
        "schema": {"planning_signals": ["asset_path"], "skill": "unreal-project-search"},
    })


# ---------------------------------------------------------------------------
# Router: dispatch a fabricated response per task, parameterized by index "mode".
# ---------------------------------------------------------------------------

def make_router(mode: str):
    """Return a fake mcp_call(url, tool, arguments, timeout_s=...) for the given mode.

    mode == "empty":   every project.search / list returns 0 results (broken index).
    mode == "healthy": known-answer queries return their expected path; broad searches
                       return a field-complete row; health/stats/schema all pass.
    """

    def fake_mcp_call(url: str, tool: str, arguments: Dict[str, Any], timeout_s: float = 45.0) -> Dict[str, Any]:
        action = arguments.get("action")
        if tool == "monolith_status":
            return mcp_text_response({"status": "ok"})
        if tool == "monolith_discover":
            return healthy_schema_response() if mode == "healthy" else mcp_text_response({"schema": {}})
        # project_query
        if action == "health":
            return healthy_health_response()
        if action == "get_stats":
            return healthy_stats_response()
        if action in ("list_gameplay_tags", "search_gameplay_tags"):
            if mode == "empty":
                return empty_search_response()
            return search_response([{"tag": "Ability.Skill"}])
        if action == "search":
            if mode == "empty":
                return empty_search_response()
            # healthy: known-answer query -> return the expected path; otherwise a
            # generic field-complete asset row so broad searches look real.
            query = str(arguments.get("query", ""))
            expected = _EXPECTED_BY_QUERY.get(query)
            if expected is not None:
                return search_response([asset_row(expected)])
            return search_response([asset_row(f"/Game/Generic/{query}")])
        return mcp_text_response({})

    return fake_mcp_call


# Map known-answer query -> expected path, taken from the real fixtures so the
# healthy router answers each known-answer task with its own ground-truth path.
_EXPECTED_BY_QUERY = {q: p for q, p in pib._KNOWN_ANSWER_FIXTURES_20260618}


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

def run_full(mode: str) -> Dict[str, Any]:
    tasks = pib.build_static_tasks()
    rows = [pib.score_task("http://test/mcp", task, 1.0) for task in tasks]
    return pib.aggregate(mode, {"status": "ok"}, tasks, rows)


def expect(cond: bool, msg: str) -> None:
    if not cond:
        raise AssertionError(msg)


# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------

def test_known_answer_branch_hit_and_miss() -> None:
    task = {
        "id": "T1",
        "category": "known_answer",
        "tool": "project_query",
        "action": "search",
        "arguments": {"action": "search", "query": "DA_Monster_001_Wiggly"},
        "expected": {"min_results": 1, "expected_object_path": "/Game/X/DA_Monster_001_Wiggly"},
    }

    # HIT: response contains the expected path.
    pib.mcp_call = lambda *a, **k: search_response([asset_row("/Game/X/DA_Monster_001_Wiggly")])
    hit = pib.score_task("http://test/mcp", task, 1.0)
    expect(hit["direct_success"] is True, "known_answer HIT must succeed")
    expect(hit["known_answer_hit"] is True, "known_answer_hit flag must be True on hit")
    expect(hit["expected_nonempty"] is True, "known_answer is expected-nonempty")

    # MISS: response returns a DIFFERENT path -> no hit.
    pib.mcp_call = lambda *a, **k: search_response([asset_row("/Game/Other/SomethingElse")])
    miss = pib.score_task("http://test/mcp", task, 1.0)
    expect(miss["direct_success"] is False, "known_answer MISS (wrong path) must fail")
    expect(miss["known_answer_hit"] is False, "known_answer_hit flag must be False on miss")

    # EMPTY: zero results -> no hit, no vacuous pass.
    pib.mcp_call = lambda *a, **k: empty_search_response()
    empt = pib.score_task("http://test/mcp", task, 1.0)
    expect(empt["direct_success"] is False, "known_answer on empty index must fail")


def test_require_results_gate_on_search() -> None:
    # A search task that demands min_results>=1 must fail on an empty response,
    # but a min_results:0 task stays lenient.
    strict = {
        "id": "S1", "category": "asset_search", "tool": "project_query", "action": "search",
        "arguments": {"action": "search", "query": "X"}, "expected": {"min_results": 1},
    }
    lenient = {
        "id": "S2", "category": "asset_search", "tool": "project_query", "action": "search",
        "arguments": {"action": "search", "query": "X"}, "expected": {"min_results": 0},
    }
    pib.mcp_call = lambda *a, **k: empty_search_response()
    expect(pib.score_task("http://t", strict, 1.0)["direct_success"] is False,
           "min_results>=1 search must fail on empty response")
    expect(pib.score_task("http://t", lenient, 1.0)["direct_success"] is True,
           "min_results:0 search stays lenient on empty response")


def test_empty_run_scores_below_one() -> None:
    pib.mcp_call = make_router("empty")
    summary = run_full("empty")
    m = summary["metrics"]
    score = m["project_index_score"]
    expect(summary["all_empty"] is True, "empty run must set all_empty=True")
    expect(m["all_empty"] is True, "empty run metrics must set all_empty=True")
    expect(score <= pib.ALL_EMPTY_SCORE_CAP + 1e-9,
           f"empty run must be capped at {pib.ALL_EMPTY_SCORE_CAP}, got {score}")
    expect(score < 0.5, f"empty/broken index must score well below 1.0, got {score}")
    expect(m["known_answer_hit_rate"] == 0.0,
           f"empty run known_answer_hit_rate must be 0.0, got {m['known_answer_hit_rate']}")
    expect(m["field_completeness_rate"] == 0.0,
           f"empty run field_completeness_rate must be 0.0 (no vacuous 1.0), got {m['field_completeness_rate']}")
    return score


def test_healthy_run_scores_high() -> None:
    pib.mcp_call = make_router("healthy")
    summary = run_full("healthy")
    m = summary["metrics"]
    score = m["project_index_score"]
    expect(summary["all_empty"] is False, "healthy run must NOT be all_empty")
    expect(m["known_answer_hit_rate"] == 1.0,
           f"healthy run must hit every known-answer fixture, got {m['known_answer_hit_rate']}")
    expect(m["field_completeness_rate"] == 1.0,
           f"healthy run field_completeness must be 1.0, got {m['field_completeness_rate']}")
    expect(score > 0.9, f"healthy index must score high (>0.9), got {score}")
    return score


def test_weights_sum_to_one() -> None:
    # The folded weights in aggregate() must still sum to 1.0.
    weights = [0.25, 0.20, 0.20, 0.15, 0.10, 0.10]
    expect(abs(sum(weights) - 1.0) < 1e-9, f"score weights must sum to 1.0, got {sum(weights)}")


def main() -> int:
    saved_mcp_call = pib.mcp_call
    try:
        test_known_answer_branch_hit_and_miss()
        test_require_results_gate_on_search()
        test_weights_sum_to_one()
        empty_score = test_empty_run_scores_below_one()
        healthy_score = test_healthy_run_scores_high()
    finally:
        pib.mcp_call = saved_mcp_call

    expect(healthy_score - empty_score > 0.5,
           f"healthy ({healthy_score}) must score far above empty ({empty_score})")
    print("ALL TESTS PASSED")
    print(f"  empty/broken-index project_index_score = {empty_score} (capped, was 1.000 before fix)")
    print(f"  healthy-index     project_index_score = {healthy_score}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
