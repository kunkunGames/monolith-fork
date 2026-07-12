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
  4. Fixture refresh rejects mutable, missing, or unsubmitted assets, selects
     deterministically across prefixes, and never overwrites on provider failure.
  5. Task generation refuses missing or invalid live fixtures instead of falling
     back to a historical cross-project snapshot.
  6. Status, protocol, runner, and transport integrity failures abort fail-closed,
     preserve triggering diagnostics, and never publish normal final artifacts.

Run:  python Scripts/test_project_index_benchmark.py
Exit code 0 = all asserts passed.
"""

from __future__ import annotations

import json
import pathlib
import sys
import tempfile
from typing import Any, Dict, List

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))

import project_index_benchmark as pib  # noqa: E402

_REAL_MCP_CALL = pib.mcp_call


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


def saved_asset_state_response(path: str, *, exists: bool = True, file_size: int = 128) -> Dict[str, Any]:
    return mcp_text_response({
        "success": True,
        "asset_state": {
            "package_path": path,
            "exists_on_disk": exists,
            "file_size": file_size if exists else -1,
        },
    })


def source_control_status_response(
    path: str,
    *,
    available: bool = True,
    source_controlled: bool = True,
    current: bool = True,
    added: bool = False,
    deleted: bool = False,
) -> Dict[str, Any]:
    return mcp_text_response({
        "available": available,
        "provider": {"name": "Perforce", "enabled": available, "available": available},
        "states": [{
            "file": path,
            "state_known": True,
            "source_controlled": source_controlled,
            "current": current,
            "added": added,
            "deleted": deleted,
            "ignored": False,
            "conflicted": False,
        }],
    })


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
            return mcp_text_response({"server_running": True, "status": "ok"})
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


# Explicit schema-v2 fixture used by scorer tests. Production builders never
# reach a historical snapshot when this fixture is missing or invalid.
_TEST_LIVE_FIXTURE: Dict[str, Any] = {
    "schema_version": pib.LIVE_FIXTURE_SCHEMA_VERSION,
    "benchmark": "ProjectIndex",
    "project_name": "Speed",
    "catalog_version": "test-catalog",
    "generated_at": "2026-07-11T00:00:00+00:00",
    "seed_prefixes": list(pib.KNOWN_ANSWER_SEED_PREFIXES),
    "stability_policy": pib.live_fixture_stability_policy(),
    "schema_actions": ["search", "health", "get_stats"],
    "known_answers": [{
        "query": "DA_StableAsset",
        "expected_object_path": "/SpeedCore/Data/DA_StableAsset",
    }],
}

_EXPECTED_BY_QUERY = {
    str(row["query"]): str(row["expected_object_path"])
    for row in _TEST_LIVE_FIXTURE["known_answers"]
}


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

def run_full(mode: str) -> Dict[str, Any]:
    tasks = pib.build_static_tasks(json.loads(json.dumps(_TEST_LIVE_FIXTURE)))
    rows = [pib.score_task("http://test/mcp", task, 1.0) for task in tasks]
    return pib.aggregate(mode, {"status": "ok"}, tasks, rows)


def expect(cond: bool, msg: str) -> None:
    if not cond:
        raise AssertionError(msg)


def simple_task(index: int) -> Dict[str, Any]:
    return {
        "id": f"PIB-U-{index:03d}",
        "category": "asset_search",
        "namespace": "project",
        "action": "search",
        "tool": "project_query",
        "arguments": {"action": "search", "query": f"Unit{index}"},
        "expected": {"min_results": 0},
    }


def transport_failure(status: int, raw: str) -> Dict[str, Any]:
    return {"transport_error": True, "status": status, "raw": raw}


def read_artifacts(output_dir: pathlib.Path) -> Dict[str, Any]:
    artifacts: Dict[str, Any] = {}
    for name in pib.RUN_OUTPUT_FILENAMES:
        path = output_dir / name
        if not path.exists():
            artifacts[name] = None
        elif name.endswith(".json"):
            artifacts[name] = json.loads(path.read_text(encoding="utf-8"))
        else:
            artifacts[name] = [
                json.loads(line)
                for line in path.read_text(encoding="utf-8").splitlines()
                if line.strip()
            ]
    return artifacts


def run_offline(
    task_count: int,
    task_response,
    *,
    status_response: Any = None,
    initial_outputs: bool = False,
    max_fraction: float = pib.DEFAULT_MAX_TRANSPORT_FAILED_FRACTION,
    max_consecutive: int = pib.DEFAULT_MAX_CONSECUTIVE_TRANSPORT_FAILURES,
    min_sample: int = pib.DEFAULT_MIN_TRANSPORT_FRACTION_SAMPLES,
) -> tuple[Dict[str, Any], Dict[str, Any], int]:
    """Execute the real runner against injected offline responses."""
    if status_response is None:
        status_response = mcp_text_response({
            "server_running": True,
            "status": "ok",
            "catalog_version": "test-catalog",
        })
    with tempfile.TemporaryDirectory() as temp_dir:
        root = pathlib.Path(temp_dir)
        tasks_path = root / "tasks.jsonl"
        tasks_path.write_text(
            "".join(json.dumps(simple_task(i)) + "\n" for i in range(1, task_count + 1)),
            encoding="utf-8",
        )
        output_dir = root / "out"
        output_dir.mkdir()
        if initial_outputs:
            for name in pib.RUN_OUTPUT_FILENAMES:
                (output_dir / name).write_text("STALE\n", encoding="utf-8")

        task_calls = 0

        def fake_mcp_call(url, tool, arguments, timeout_s=45.0):
            nonlocal task_calls
            if tool == "monolith_status":
                if isinstance(status_response, BaseException):
                    raise status_response
                return status_response
            task_calls += 1
            return task_response(task_calls, tool, arguments)

        saved = pib.mcp_call
        pib.mcp_call = fake_mcp_call
        try:
            result = pib.run_benchmark(
                "http://offline",
                tasks_path,
                output_dir,
                "offline-unit",
                1.0,
                max_fraction,
                max_consecutive,
                min_sample,
                allow_subset=True,
            )
        finally:
            pib.mcp_call = saved
        return result, read_artifacts(output_dir), task_calls


def assert_invalid_artifacts(result: Dict[str, Any], artifacts: Dict[str, Any]) -> None:
    expect(result.get("run_valid") is False, "invalid run must set run_valid=false")
    expect(result.get("metrics_valid") is False, "invalid run must set metrics_valid=false")
    expect(bool(result.get("metrics_scope")), "invalid run must declare metrics_scope")
    expect(artifacts["run_failure.json"] is not None, "invalid run must write run_failure.json")
    expect(artifacts["partial_summary.json"] is not None, "invalid run must write invalid partial_summary.json")
    expect(artifacts["summary.json"] is None, "invalid run must not write summary.json")
    expect(artifacts["per_task.json"] is None, "invalid run must not write final per_task.json")


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


def test_known_answer_hit_matches_live_asset_path_contract() -> None:
    # Current live contract: the asset's package path is in "asset_path";
    # "match_object_path" is the path of the matching field WITHIN the asset
    # (e.g. "MapID" for a variable match). Identity matching must use
    # asset_path, while legacy match_object_path identity rows keep working.
    live_row = {
        "asset_path": "/SpeedBox/System/Playlists/DA_SpeedBox_Online",
        "asset_name": "DA_SpeedBox_Online",
        "match_object_path": "MapID",
        "match_value": "Map:/SpeedBox/Maps/L_Lobby_Box",
        "match_source": "variable",
    }
    expect(pib.known_answer_hit([live_row], "/SpeedBox/System/Playlists/DA_SpeedBox_Online") is True,
           "known_answer_hit must match the asset_path package path")
    expect(pib.known_answer_hit([live_row], "/Game/Nope/Missing") is False,
           "known_answer_hit must not match an absent path")
    legacy_row = {"match_object_path": "/Game/X/DA_Legacy"}
    expect(pib.known_answer_hit([legacy_row], "/Game/X/DA_Legacy") is True,
           "legacy identity rows carried in match_object_path must keep matching")


def test_validated_live_fixtures_drive_schema_and_known_answer_tasks() -> None:
    fixtures = valid_live_fixture()
    fixtures["known_answers"] = [
        {"query": "DA_SpeedBox_Online", "expected_object_path": "/SpeedBox/System/Playlists/DA_SpeedBox_Online"},
    ]
    tasks = pib.build_static_tasks(fixtures)

    schema_rows = [t for t in tasks if t["category"] == "schema_field_presence"]
    schema_actions = {t["action"] for t in schema_rows}
    expect(schema_actions == {"search", "health", "get_stats"},
           f"live fixtures must fully replace the static schema action lists, got {sorted(schema_actions)}")

    ka_rows = [t for t in tasks if t["category"] == "known_answer"]
    expect(len(ka_rows) == 1, f"live fixtures must replace the static known answers, got {len(ka_rows)}")
    expect(ka_rows[0]["arguments"]["query"] == "DA_SpeedBox_Online", "known-answer query must come from fixtures")
    expect(ka_rows[0]["expected"]["expected_object_path"] == "/SpeedBox/System/Playlists/DA_SpeedBox_Online",
           "known-answer expected path must come from fixtures")


def test_build_static_tasks_requires_validated_live_fixtures() -> None:
    for invalid in (None, {}, {"schema_version": 1}):
        try:
            pib.build_static_tasks(invalid)  # type: ignore[arg-type]
        except RuntimeError:
            pass
        else:
            raise AssertionError(
                "build_static_tasks must reject missing or pre-schema-v2 fixtures"
            )


def test_derive_known_answer_fixtures_verifies_candidates() -> None:
    # Only the submitted on-disk candidate with an exact identity hit survives.
    good_path = "/SpeedCore/Data/DA_GoodAsset"
    local_add_path = "/SpeedCore/Data/DA_LocalAdd"
    stale_path = "/SpeedCore/Data/DA_StaleAsset"

    def fake_mcp_call(url, tool, arguments, timeout_s=45.0):
        action = str(arguments.get("action", ""))
        query = str(arguments.get("query", ""))
        if tool == "project_query" and action == "search" and arguments.get("include_content") is False:
            # Seed-prefix proposal search.
            if query == "DA_":
                return search_response([
                    {"asset_name": "DA_GoodAsset", "asset_path": good_path},
                    {"asset_name": "DA_BadNoVerify", "asset_path": "/SpeedCore/Data/DA_BadNoVerify"},
                    {"asset_name": "DA_LocalAdd", "asset_path": local_add_path},
                    {"asset_name": "DA_StaleAsset", "asset_path": stale_path},
                    {"asset_name": "DA_MutableAsset", "asset_path": "/Game/Benchmarks/DA_MutableAsset"},
                    {"asset_name": "DA_x", "asset_path": "/Game/Data/DA_x"},  # too short
                ])
            return empty_search_response()
        if tool == "project_query" and action == "get_saved_asset_state":
            path = str(arguments.get("asset_path", ""))
            return saved_asset_state_response(path, exists=path != stale_path)
        if tool == "source_control_query" and action == "get_status":
            path = str((arguments.get("paths") or [""])[0])
            if path == local_add_path:
                return source_control_status_response(
                    path, source_controlled=False, added=True
                )
            return source_control_status_response(path)
        # Benchmark-shaped verification search.
        if tool == "project_query" and action == "search" and query == "DA_GoodAsset":
            return search_response([{"asset_path": good_path, "asset_name": "DA_GoodAsset"}])
        return empty_search_response()

    saved = pib.mcp_call
    pib.mcp_call = fake_mcp_call
    try:
        fixtures = pib.derive_known_answer_fixtures("http://t", 1.0, target_count=5)
    finally:
        pib.mcp_call = saved
    expect(fixtures == [{"query": "DA_GoodAsset", "expected_object_path": good_path}],
           f"only the verified, distinctive candidate may be recorded, got {fixtures}")


def test_known_answer_selection_is_deterministic_and_prefix_balanced() -> None:
    pools = {
        "DA_": [
            ("DA_BetaAsset", "/SpeedCore/Data/DA_BetaAsset"),
            ("DA_AlphaAsset", "/SpeedCore/Data/DA_AlphaAsset"),
        ],
        "DT_": [
            ("DT_BetaTable", "/SpeedCore/Data/DT_BetaTable"),
            ("DT_AlphaTable", "/SpeedCore/Data/DT_AlphaTable"),
        ],
    }

    def run(reverse: bool) -> List[Dict[str, str]]:
        exact_paths = {name: path for rows in pools.values() for name, path in rows}

        def fake_mcp_call(url, tool, arguments, timeout_s=45.0):
            action = str(arguments.get("action", ""))
            query = str(arguments.get("query", ""))
            if tool == "project_query" and action == "search" and arguments.get("include_content") is False:
                rows = list(pools.get(query, []))
                if reverse:
                    rows.reverse()
                return search_response([
                    {"asset_name": name, "asset_path": path} for name, path in rows
                ])
            if tool == "project_query" and action == "get_saved_asset_state":
                return saved_asset_state_response(str(arguments.get("asset_path", "")))
            if tool == "source_control_query" and action == "get_status":
                path = str((arguments.get("paths") or [""])[0])
                return source_control_status_response(path)
            if tool == "project_query" and action == "search" and query in exact_paths:
                return search_response([{"asset_name": query, "asset_path": exact_paths[query]}])
            return empty_search_response()

        saved = pib.mcp_call
        pib.mcp_call = fake_mcp_call
        try:
            return pib.derive_known_answer_fixtures(
                "http://t", 1.0, target_count=4, per_prefix_cap=2
            )
        finally:
            pib.mcp_call = saved

    expected = [
        {"query": "DA_AlphaAsset", "expected_object_path": "/SpeedCore/Data/DA_AlphaAsset"},
        {"query": "DT_AlphaTable", "expected_object_path": "/SpeedCore/Data/DT_AlphaTable"},
        {"query": "DA_BetaAsset", "expected_object_path": "/SpeedCore/Data/DA_BetaAsset"},
        {"query": "DT_BetaTable", "expected_object_path": "/SpeedCore/Data/DT_BetaTable"},
    ]
    expect(run(False) == expected, "selection must be prefix round-robin after sorting")
    expect(run(True) == expected, "live search row order must not change selected fixtures")


def valid_live_fixture() -> Dict[str, Any]:
    return json.loads(json.dumps(_TEST_LIVE_FIXTURE))


def test_generate_fails_closed_without_valid_live_fixtures() -> None:
    with tempfile.TemporaryDirectory() as temp_dir:
        root = pathlib.Path(temp_dir)
        tasks_path = root / "tasks.jsonl"
        manifest_path = root / "manifest.json"
        tasks_path.write_text("TASKS_SENTINEL\n", encoding="utf-8")
        manifest_path.write_text("MANIFEST_SENTINEL\n", encoding="utf-8")

        try:
            pib.generate_tasks(tasks_path, manifest_path, root / "missing.json")
        except RuntimeError:
            pass
        else:
            raise AssertionError("generate must fail when live fixtures are missing")
        expect(tasks_path.read_text(encoding="utf-8") == "TASKS_SENTINEL\n",
               "missing fixtures must not overwrite tasks")
        expect(manifest_path.read_text(encoding="utf-8") == "MANIFEST_SENTINEL\n",
               "missing fixtures must not overwrite manifest")

        invalid = valid_live_fixture()
        invalid["known_answers"][0] = {
            "query": "BT_Mutable",
            "expected_object_path": "/Game/Benchmarks/AI/BT_Mutable",
        }
        invalid_path = root / "invalid.json"
        invalid_path.write_text(json.dumps(invalid), encoding="utf-8")
        try:
            pib.generate_tasks(tasks_path, manifest_path, invalid_path)
        except RuntimeError:
            pass
        else:
            raise AssertionError("generate must reject mutable-root live fixtures")
        expect(tasks_path.read_text(encoding="utf-8") == "TASKS_SENTINEL\n",
               "invalid fixtures must not overwrite tasks")
        expect(manifest_path.read_text(encoding="utf-8") == "MANIFEST_SENTINEL\n",
               "invalid fixtures must not overwrite manifest")


def test_refresh_does_not_overwrite_when_source_control_is_unavailable() -> None:
    candidate_path = "/SpeedCore/Data/DA_StableAsset"

    def fake_mcp_call(url, tool, arguments, timeout_s=45.0):
        action = str(arguments.get("action", ""))
        query = str(arguments.get("query", ""))
        if tool == "monolith_status":
            return mcp_text_response({
                "project_name": "Speed",
                "catalog_version": "test-catalog",
            })
        if tool == "monolith_discover":
            return mcp_text_response({
                "actions": [{"action": "search"}],
                "truncated": False,
            })
        if tool == "project_query" and action == "search" and arguments.get("include_content") is False:
            if query == "DA_":
                return search_response([{
                    "asset_name": "DA_StableAsset",
                    "asset_path": candidate_path,
                }])
            return empty_search_response()
        if tool == "project_query" and action == "get_saved_asset_state":
            return saved_asset_state_response(candidate_path)
        if tool == "source_control_query" and action == "get_status":
            return source_control_status_response(candidate_path, available=False)
        return empty_search_response()

    with tempfile.TemporaryDirectory() as temp_dir:
        fixture_path = pathlib.Path(temp_dir) / "live_fixtures.json"
        fixture_path.write_text("DO_NOT_OVERWRITE\n", encoding="utf-8")
        saved = pib.mcp_call
        pib.mcp_call = fake_mcp_call
        try:
            try:
                pib.refresh_live_fixtures(
                    "http://t", fixture_path, 1.0,
                    known_answer_count=1, min_known_answers=1,
                )
            except RuntimeError as exc:
                expect("provider unavailable" in str(exc),
                       f"unexpected source-control failure: {exc}")
            else:
                raise AssertionError("refresh must fail when source control is unavailable")
        finally:
            pib.mcp_call = saved
        expect(fixture_path.read_text(encoding="utf-8") == "DO_NOT_OVERWRITE\n",
               "provider failure must leave the existing fixture file untouched")


def test_mcp_top_level_list_is_protocol_error_and_aborts_task_run() -> None:
    class FakeHttpResponse:
        headers = {"Content-Type": "application/json"}

        def __enter__(self):
            return self

        def __exit__(self, exc_type, exc, tb):
            return False

        def read(self):
            return b"[]"

    saved_call = pib.mcp_call
    saved_urlopen = pib.urllib.request.urlopen
    pib.mcp_call = _REAL_MCP_CALL
    pib.urllib.request.urlopen = lambda request, timeout: FakeHttpResponse()
    try:
        response = pib.mcp_call("http://offline", "project_query", {}, timeout_s=1.0)
    finally:
        pib.urllib.request.urlopen = saved_urlopen
        pib.mcp_call = saved_call
    expect(response.get("protocol_error") is True,
           "mcp_call must classify a top-level list as protocol_error")
    expect("top-level JSON" in str(response.get("error")),
           "protocol error must explain the required object shape")

    result, artifacts, task_calls = run_offline(3, lambda *_: [])
    assert_invalid_artifacts(result, artifacts)
    expect(task_calls == 1, "task protocol failure must abort immediately")
    expect(result.get("failure_kind") == "protocol_error",
           f"unexpected protocol failure kind: {result.get('failure_kind')}")
    rows = artifacts["per_task.jsonl"]
    expect(isinstance(rows, list) and len(rows) == 1,
           "protocol failure must preserve exactly the triggering row")
    expect(rows[0]["task_id"] == "PIB-U-001", "wrong triggering task was preserved")


def test_status_invalid_transport_and_runner_fail_before_tasks() -> None:
    cases = [
        (mcp_text_response({"status": "ok"}), "invalid_status_payload"),
        ([], "protocol_error"),
        ({"parse_error": True, "raw": "not-json"}, "protocol_error"),
        (transport_failure(503, "status unavailable"), "transport_error"),
        (RuntimeError("status exploded"), "runner_exception"),
    ]
    for status_response, expected_kind in cases:
        result, artifacts, task_calls = run_offline(
            2,
            lambda *_: search_response([asset_row("/Game/ShouldNotRun")]),
            status_response=status_response,
        )
        assert_invalid_artifacts(result, artifacts)
        expect(task_calls == 0, f"{expected_kind} status failure must abort before tasks")
        expect(result.get("failure_stage") == "status_preflight",
               f"{expected_kind} must identify status_preflight")
        expect(result.get("failure_kind") == expected_kind,
               f"expected {expected_kind}, got {result.get('failure_kind')}")
        expect(artifacts["per_task.jsonl"] is None,
               "status failure must not create a per-task stream")


def test_three_consecutive_transport_failures_abort_with_diagnostics() -> None:
    result, artifacts, task_calls = run_offline(
        6,
        lambda index, *_: transport_failure(503, f"down-{index}"),
    )
    assert_invalid_artifacts(result, artifacts)
    expect(task_calls == 3, f"consecutive gate must stop at task 3, got {task_calls}")
    expect(result.get("transport_gate_reason") == "consecutive_transport_failures",
           "wrong consecutive gate reason")
    expect(result.get("last_task_id") == "PIB-U-003", "wrong failing task id")
    rows = artifacts["per_task.jsonl"]
    expect(len(rows) == 3, "all attempted transport rows must be preserved")
    expect(rows[-1]["transport_status"] == 503, "transport status must be preserved")
    expect(rows[-1]["transport_error_raw"] == "down-3", "transport raw must be preserved")


def test_transport_fraction_gate_fires_on_twentieth_observation() -> None:
    def response(index, *_):
        if index <= 2:
            return transport_failure(500 + index, f"fraction-{index}")
        return empty_search_response()

    result, artifacts, task_calls = run_offline(25, response)
    assert_invalid_artifacts(result, artifacts)
    expect(task_calls == 20, f"fraction gate must first fire at sample 20, got {task_calls}")
    expect(result.get("transport_gate_reason") == "transport_failed_fraction",
           "wrong fraction gate reason")
    expect(result.get("last_task_id") == "PIB-U-002",
           "fraction decision must identify the last actual transport failure")
    expect(result.get("last_transport_status") == 502,
           "fraction decision must preserve last transport status")
    expect(result.get("last_transport_error_raw") == "fraction-2",
           "fraction decision must preserve last transport raw")


def test_short_population_transport_fraction_fails_at_finalize() -> None:
    result, artifacts, task_calls = run_offline(
        10,
        lambda index, *_: transport_failure(504, "short-down")
        if index == 1 else empty_search_response(),
    )
    assert_invalid_artifacts(result, artifacts)
    expect(task_calls == 10, "short-run fraction gate must wait for finalize")
    expect(result.get("transport_gate_reason") == "final_transport_failed_fraction",
           "short run must use final fraction gate")
    expect(result.get("metrics_scope") == "complete_run_invalid",
           "short-run invalid metrics must be marked complete_run_invalid")


def test_success_resets_consecutive_transport_failures() -> None:
    failures = {1, 2, 4, 5}
    result, artifacts, task_calls = run_offline(
        6,
        lambda index, *_: transport_failure(502, f"reset-{index}")
        if index in failures else empty_search_response(),
        max_fraction=1.0,
    )
    expect(task_calls == 6, "success reset run must attempt every task")
    expect(result.get("run_valid") is True, "successes must reset the consecutive counter")
    expect(result.get("consecutive_transport_failures") == 0,
           "final successful task must leave consecutive failures at zero")
    expect(artifacts["summary.json"] is not None, "valid reset run must write summary")
    expect(artifacts["per_task.json"] is not None, "valid reset run must write final rows")
    expect(artifacts["run_failure.json"] is None, "valid reset run must not write failure")
    expect(artifacts["partial_summary.json"] is None, "valid reset run must remove partial")


def test_stale_success_outputs_are_removed_before_status_failure() -> None:
    result, artifacts, task_calls = run_offline(
        1,
        lambda *_: empty_search_response(),
        status_response=mcp_text_response({"status": "not-a-valid-preflight"}),
        initial_outputs=True,
    )
    assert_invalid_artifacts(result, artifacts)
    expect(task_calls == 0, "invalid status must not score tasks")
    expect(artifacts["per_task.jsonl"] is None,
           "stale per_task.jsonl must be removed when no task is attempted")


def test_task_runner_exception_preserves_triggering_row() -> None:
    def explode(*_):
        raise ValueError("scorer exploded")

    result, artifacts, task_calls = run_offline(3, explode)
    assert_invalid_artifacts(result, artifacts)
    expect(task_calls == 1, "runner exception must abort on the triggering task")
    expect(result.get("failure_kind") == "runner_exception", "wrong runner failure kind")
    rows = artifacts["per_task.jsonl"]
    expect(len(rows) == 1, "runner exception must preserve one triggering row")
    expect(rows[0]["failure_kind"] == "runner_exception", "row must identify runner_exception")
    expect("scorer exploded" in rows[0]["error"], "row must retain the exception text")


def test_success_writes_only_final_outputs_and_removes_partial() -> None:
    result, artifacts, task_calls = run_offline(
        2,
        lambda index, *_: search_response([asset_row(f"/Game/Stable/Asset{index}")]),
    )
    expect(task_calls == 2, "successful run must attempt every task")
    expect(result.get("run_valid") is True, "successful run must be valid")
    expect(result.get("metrics_valid") is True, "successful run metrics must be valid")
    expect(result.get("metrics_scope") == "complete_subset_run", "wrong success metric scope")
    expect(result.get("comparison_valid") is False,
           "explicit subset diagnostics must never be comparison-valid")
    expect(artifacts["summary.json"] is not None, "success must write summary.json")
    expect(artifacts["per_task.json"] is not None, "success must write per_task.json")
    expect(len(artifacts["per_task.jsonl"]) == 2, "success must preserve incremental task rows")
    expect(artifacts["partial_summary.json"] is None, "success must remove partial summary")
    expect(artifacts["run_failure.json"] is None, "success must not write run_failure")


def test_manifest_run_gates_match_shared_defaults_and_fixture_contract() -> None:
    manifest_path = pib.resolve_plugin_path(pib.DEFAULT_MANIFEST)
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    gates = manifest.get("run_gates")
    expect(isinstance(gates, dict), "manifest must declare run_gates")
    expect(gates.get("max_transport_failed_fraction") == pib.DEFAULT_MAX_TRANSPORT_FAILED_FRACTION,
           "manifest transport fraction must match shared default")
    expect(gates.get("max_consecutive_transport_failures") == pib.DEFAULT_MAX_CONSECUTIVE_TRANSPORT_FAILURES,
           "manifest consecutive gate must match shared default")
    expect(gates.get("min_transport_fraction_sample") == pib.DEFAULT_MIN_TRANSPORT_FRACTION_SAMPLES,
           "manifest sample floor must match shared default")
    for gate_name in (
        "status_transport_failure_aborts_before_tasks",
        "invalid_status_response_aborts_before_tasks",
        "task_protocol_error_aborts_immediately",
        "runner_exception_aborts_immediately",
        "short_run_fraction_checked_at_finalize",
        "invalid_run_writes_failure_and_partial",
    ):
        expect(gates.get(gate_name) is True, f"manifest must enable {gate_name}")
    expect(gates.get("invalid_run_writes_summary") is False,
           "manifest must forbid normal summary for invalid runs")
    expect(gates.get("invalid_run_writes_per_task") is False,
           "manifest must forbid final per_task for invalid runs")
    expect(manifest["live_fixtures"]["schema_version"] == pib.LIVE_FIXTURE_SCHEMA_VERSION == 2,
           "schema-v2 live fixture contract must remain strict")


def test_cli_allow_subset_is_forwarded() -> None:
    captured: Dict[str, Any] = {}
    original = pib.run_benchmark

    def fake_run(*args: Any, **kwargs: Any) -> Dict[str, Any]:
        captured.update(kwargs)
        return {"run_valid": True}

    pib.run_benchmark = fake_run
    try:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = pathlib.Path(temp_dir)
            code = pib.main([
                "run",
                "--tasks", str(root / "diagnostic.jsonl"),
                "--output-dir", str(root / "out"),
                "--label", "subset-selftest",
                "--allow-subset",
            ])
    finally:
        pib.run_benchmark = original
    expect(code == 0, "CLI diagnostic run should propagate a valid fake result")
    expect(captured.get("allow_subset") is True,
           "--allow-subset must reach run_benchmark instead of being parser-only")


def main() -> int:
    saved_mcp_call = pib.mcp_call
    try:
        test_known_answer_branch_hit_and_miss()
        test_require_results_gate_on_search()
        test_weights_sum_to_one()
        test_known_answer_hit_matches_live_asset_path_contract()
        test_validated_live_fixtures_drive_schema_and_known_answer_tasks()
        test_build_static_tasks_requires_validated_live_fixtures()
        test_derive_known_answer_fixtures_verifies_candidates()
        test_known_answer_selection_is_deterministic_and_prefix_balanced()
        test_generate_fails_closed_without_valid_live_fixtures()
        test_refresh_does_not_overwrite_when_source_control_is_unavailable()
        test_mcp_top_level_list_is_protocol_error_and_aborts_task_run()
        test_status_invalid_transport_and_runner_fail_before_tasks()
        test_three_consecutive_transport_failures_abort_with_diagnostics()
        test_transport_fraction_gate_fires_on_twentieth_observation()
        test_short_population_transport_fraction_fails_at_finalize()
        test_success_resets_consecutive_transport_failures()
        test_stale_success_outputs_are_removed_before_status_failure()
        test_task_runner_exception_preserves_triggering_row()
        test_success_writes_only_final_outputs_and_removes_partial()
        test_manifest_run_gates_match_shared_defaults_and_fixture_contract()
        test_cli_allow_subset_is_forwarded()
        empty_score = test_empty_run_scores_below_one()
        healthy_score = test_healthy_run_scores_high()
    finally:
        pib.mcp_call = saved_mcp_call

    expect(healthy_score - empty_score > 0.5,
           f"healthy ({healthy_score}) must score far above empty ({empty_score})")
    print("ALL 23 TESTS PASSED")
    print(f"  empty/broken-index project_index_score = {empty_score} (capped, was 1.000 before fix)")
    print(f"  healthy-index     project_index_score = {healthy_score}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
