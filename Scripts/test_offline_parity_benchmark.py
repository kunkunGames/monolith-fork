#!/usr/bin/env python3
"""Offline unit tests for the OfflineParity benchmark.

No live editor / MCP server and no real subprocess is launched.  Each test
monkeypatches ``run_exe`` / ``run_py`` to return fabricated
``(returncode, stdout, stderr)`` tuples.  The exe/py offline tools print the same
JSON payload an MCP ``tools/call`` would carry, so the fabricated stdout is built
from the real envelope shape::

    {"result": {"content": [{"type": "text", "text": <json>}], "isError": <bool>}}

The tests cover three benchmark contracts:

  ITEM 1 -- the action table loads from Benchmarks/OfflineParity/actions.jsonl and
            the manifest action_count matches the data-file line count.
  ITEM 2 -- the ``offline_unsupported`` bucket: when BOTH offline tools agree on a
            failure the row is a MATCH(expected_offline) that does NOT drag the
            score (the reclassification); a genuine exe-vs-py disagreement
            (exactly one tool fails) is a DIFF(offline_parity_break) that scores
            LOW; and the same row scored as a plain row is a real ERROR.
  ITEM 3 -- rollback-journal safety is classified only by the run-level
            preflight. A one-sided Query refusal observed by ``run_action`` is a
            real ERROR, while a blocked preflight invalidates the entire run and
            forces a 0.0 score.

Run::

    python Plugins/Monolith/Scripts/test_offline_parity_benchmark.py
"""

from __future__ import annotations

import contextlib
import importlib.util
import io
import json
import pathlib
import sys
import tempfile
from types import SimpleNamespace
from typing import Any, Dict, List, Tuple

_SCRIPTS_DIR = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(_SCRIPTS_DIR))

_SPEC = importlib.util.spec_from_file_location("opb_under_test", _SCRIPTS_DIR / "offline_parity_benchmark.py")
opb = importlib.util.module_from_spec(_SPEC)
assert _SPEC and _SPEC.loader
_SPEC.loader.exec_module(opb)

# Unused dummy paths -- run_exe/run_py are patched so they are never read.
_DUMMY = pathlib.Path("unused")


# ---------------------------------------------------------------------------
# Fabricated offline-tool stdout (mirror the real MCP tools/call envelope)
# ---------------------------------------------------------------------------

def _envelope_text(payload: Any, is_error: bool) -> str:
    """Build the JSON stdout the offline tool prints for a JSON-compare action."""
    inner = json.dumps(payload, sort_keys=True)
    envelope = {"result": {"content": [{"type": "text", "text": inner}], "isError": is_error}}
    # The offline tools emit the inner payload directly on stdout; the envelope is
    # the transport shape.  We compare the inner payload, so emit that as stdout.
    assert envelope["result"]["content"][0]["text"] == inner
    return inner


_HEALTHY_PAYLOAD = {"success": True, "include_path": "Engine/Classes/GameFramework/Actor.h"}
_HEALTHY_STDOUT = _envelope_text(_HEALTHY_PAYLOAD, is_error=False)


def _fixed(exe_ret: int, exe_out: str, py_ret: int, py_out: str,
           exe_err: str = "", py_err: str = ""):
    """Return monkeypatch callables for run_exe / run_py with fixed outputs."""
    def _exe(ns, action, args, exe_path, mono_root):  # noqa: ANN001
        return exe_ret, exe_out, exe_err

    def _py(ns, action, args, py_path, mono_root):  # noqa: ANN001
        return py_ret, py_out, py_err

    return _exe, _py


def _run(label: str, *, offline_unsupported: bool, expected_error: bool = False,
         compare: str = "json", exe: Tuple[int, str, str] = (0, _HEALTHY_STDOUT, ""),
         py: Tuple[int, str, str] = (0, _HEALTHY_STDOUT, "")) -> Dict[str, Any]:
    """Run run_action with fabricated exe/py results."""
    orig_exe, orig_py = opb.run_exe, opb.run_py
    opb.run_exe, opb.run_py = _fixed(exe[0], exe[1], py[0], py[1], exe[2], py[2])
    try:
        return opb.run_action(
            label, "source", "get_include_path", ["AActor"],
            ignore_cursor_bytes=False, exe_path=_DUMMY, py_path=_DUMMY, mono_root=_DUMMY,
            compare=compare, expected_error=expected_error,
            offline_unsupported=offline_unsupported,
        )
    finally:
        opb.run_exe, opb.run_py = orig_exe, orig_py


# ---------------------------------------------------------------------------
# Test harness (mirrors the sibling benchmark tests)
# ---------------------------------------------------------------------------

_FAILURES: List[str] = []


def check(name: str, condition: bool, detail: str = "") -> None:
    status = "PASS" if condition else "FAIL"
    print(f"[{status}] {name}" + (f" -- {detail}" if detail else ""))
    if not condition:
        _FAILURES.append(name)


def _metrics(rows: List[Dict[str, Any]]) -> Dict[str, Any]:
    return opb.compute_metrics(rows, version_parity_ok=True)


# ---------------------------------------------------------------------------
# ITEM 1 -- externalized table + manifest line-count parity
# ---------------------------------------------------------------------------

def test_actions_jsonl_loads_and_matches_manifest() -> None:
    specs = opb.load_action_specs()
    check("actions.jsonl loads EXPECTED_ACTION_COUNT rows",
          len(specs) == opb.EXPECTED_ACTION_COUNT, f"n={len(specs)}")
    manifest = json.loads(opb.DEFAULT_MANIFEST.read_text(encoding="utf-8"))
    check("manifest action_count == loaded row count",
          manifest["action_count"] == len(specs),
          f"manifest={manifest['action_count']} rows={len(specs)}")
    # Non-empty line count of the data file must equal the manifest count (the
    # exact contract hosted-static-CI enforces).
    line_count = sum(1 for ln in opb.DEFAULT_ACTIONS.read_text(encoding="utf-8").splitlines() if ln.strip())
    check("actions.jsonl non-empty line count == manifest action_count",
          line_count == manifest["action_count"], f"lines={line_count}")


def test_build_actions_substitutes_tokens_and_skips_missing_chain() -> None:
    chain = {"uclass": "ACharacter", "decision_id": "D-1", "risk_path": "Docs/SPEC_CORE.md"}
    actions = opb.build_actions(chain)
    by_label = {a[0]: a for a in actions}
    check("uclass token substituted", by_label["cppreflect.get_uclass"][3] == ["ACharacter"])
    check("risk_path token substituted",
          by_label["risk.get_hotspot_score"][3] == ["Docs/SPEC_CORE.md"])
    # decision_id absent -> the 6 did-dependent rows get None args (SKIP path).
    actions_no_did = opb.build_actions(dict(chain, decision_id=None))
    none_rows = [a[0] for a in actions_no_did if a[3] is None]
    check("missing decision_id yields 6 None-arg (SKIP) rows",
          len(none_rows) == 6, f"none_rows={none_rows}")
    # offline_unsupported metadata is carried on the source rows.
    ou = [a for a in actions if a[5].get("offline_unsupported")]
    check("offline_unsupported flag carried from jsonl", len(ou) >= 1, f"n={len(ou)}")


def test_authoritative_query_bundle_resolution_is_manifest_selected() -> None:
    with tempfile.TemporaryDirectory() as temp_dir:
        root = pathlib.Path(temp_dir)
        binaries = root / "Binaries"
        validator = (
            root
            / "Tools"
            / "MonolithQuery"
            / "publish_query_bundle.py"
        )
        binaries.mkdir(parents=True)
        validator.parent.mkdir(parents=True)
        validator.write_text("# unit validator\n", encoding="utf-8")
        manifest = binaries / "monolith_query.current.json"
        manifest.write_text("{}\n", encoding="utf-8")
        immutable = binaries / "monolith_query-0123456789abcdef.exe"
        immutable.write_bytes(b"immutable-query")
        mutable_alias = binaries / "monolith_query.exe"
        mutable_alias.write_bytes(b"stale-alias")

        original_run = opb._run
        opb._run = lambda command, cwd: (
            0,
            json.dumps({"file": immutable.name}),
            "",
        )
        try:
            resolved_exe, resolved_manifest = (
                opb.resolve_authoritative_query_bundle(root)
            )
        finally:
            opb._run = original_run

        check(
            "Query bundle resolver selects immutable executable",
            resolved_exe == immutable.resolve(),
            f"resolved={resolved_exe}",
        )
        check(
            "Query bundle resolver retains manifest identity",
            resolved_manifest == manifest.resolve(),
            f"manifest={resolved_manifest}",
        )
        check(
            "Query bundle resolver does not select compatibility alias",
            resolved_exe != mutable_alias.resolve(),
        )


def test_authoritative_query_bundle_resolution_rejects_invalid_leaf() -> None:
    with tempfile.TemporaryDirectory() as temp_dir:
        root = pathlib.Path(temp_dir)
        validator = (
            root
            / "Tools"
            / "MonolithQuery"
            / "publish_query_bundle.py"
        )
        validator.parent.mkdir(parents=True)
        validator.write_text("# unit validator\n", encoding="utf-8")

        original_run = opb._run
        opb._run = lambda command, cwd: (
            0,
            json.dumps({"file": "../outside.exe"}),
            "",
        )
        rejected = False
        try:
            opb.resolve_authoritative_query_bundle(root)
        except RuntimeError:
            rejected = True
        finally:
            opb._run = original_run
        check("Query bundle resolver rejects path traversal", rejected)


def test_input_fingerprint_tracks_only_declared_database_dependencies() -> None:
    with tempfile.TemporaryDirectory() as temp_dir:
        root = pathlib.Path(temp_dir)
        (root / "Saved").mkdir()
        (root / "Saved" / "EngineSource.db").write_bytes(b"source")
        (root / "Saved" / "ProjectIndex.db").write_bytes(b"project")
        exe = root / "query.exe"
        py = root / "offline.py"
        query_manifest = root / "Binaries" / "monolith_query.current.json"
        query_manifest.parent.mkdir()
        exe.write_bytes(b"query")
        py.write_text("# offline\n", encoding="utf-8")
        query_manifest.write_text("{}\n", encoding="utf-8")

        inputs = opb.build_offline_parity_inputs(
            exe,
            py,
            root,
            query_manifest,
        )

    database_paths = [row["path"] for row in inputs["database_files"]]
    check(
        "OfflineParity fingerprint includes only EngineSource.db",
        database_paths == ["Saved/EngineSource.db"],
        f"database_paths={database_paths}",
    )
    check(
        "OfflineParity fingerprint includes corpus and runner SHA inputs",
        set(inputs["files"]) == {
            "benchmark_common",
            "tasks",
            "manifest",
            "runner",
            "offline_exe",
            "offline_python",
            "query_manifest",
        },
        f"file_inputs={sorted(inputs['files'])}",
    )


# ---------------------------------------------------------------------------
# ITEM 2 -- offline_unsupported bucket scoring
# ---------------------------------------------------------------------------

def test_offline_unsupported_both_fail_is_expected_offline_match() -> None:
    # THE RECLASSIFICATION: both offline tools agree the surface errors -> MATCH.
    err = _envelope_text({"success": False, "error": "surface unavailable offline"}, is_error=True)
    row = _run("source.get_include_path", offline_unsupported=True, compare="text",
               exe=(1, err, "no include path"), py=(1, err, "no include path"))
    check("offline_unsupported both-fail -> MATCH", row["status"] == "MATCH",
          f"status={row['status']}")
    check("offline_unsupported both-fail -> error_kind=expected_offline",
          row["error_kind"] == "expected_offline", f"kind={row['error_kind']}")


def test_offline_unsupported_one_fail_is_offline_parity_break_diff() -> None:
    # THE NEW FAILURE MODE: exactly one tool fails -> genuine exe-vs-py disagreement.
    row = _run("source.get_include_path", offline_unsupported=True, compare="text",
               exe=(0, "Engine/Actor.h", ""), py=(1, "", "no include path"))
    check("offline_unsupported one-fail -> DIFF", row["status"] == "DIFF",
          f"status={row['status']}")
    check("offline_unsupported one-fail -> error_kind=offline_parity_break",
          row["error_kind"] == "offline_parity_break", f"kind={row['error_kind']}")


def test_offline_unsupported_both_succeed_compares_normally() -> None:
    # When the surface IS offline-served on both tools, outputs compare normally.
    row_match = _run("source.get_include_path", offline_unsupported=True, compare="text",
                     exe=(0, "Engine/Actor.h", ""), py=(0, "Engine/Actor.h", ""))
    check("offline_unsupported both-succeed equal -> MATCH none",
          row_match["status"] == "MATCH" and row_match["error_kind"] == "none",
          f"status={row_match['status']} kind={row_match['error_kind']}")
    row_diff = _run("source.get_include_path", offline_unsupported=True, compare="text",
                    exe=(0, "Engine/Actor.h", ""), py=(0, "Engine/OTHER.h", ""))
    check("offline_unsupported both-succeed unequal -> DIFF",
          row_diff["status"] == "DIFF", f"status={row_diff['status']}")


def test_plain_row_both_fail_is_real_error() -> None:
    # Control: WITHOUT the flag the identical both-fail row is a real ERROR that
    # drags the score -- this is the masking the reclassification removes.
    err = _envelope_text({"success": False, "error": "surface unavailable offline"}, is_error=True)
    row = _run("source.get_include_path", offline_unsupported=False, compare="text",
               exe=(1, err, "boom"), py=(1, err, "boom"))
    check("plain both-fail -> ERROR", row["status"] == "ERROR", f"status={row['status']}")
    check("plain both-fail -> error_kind=real", row["error_kind"] == "real",
          f"kind={row['error_kind']}")


def test_one_sided_hot_journal_marker_is_real_error() -> None:
    # A marker appearing after the preflight means the environment changed
    # during the run. Query failed while Python succeeded, so the row must stay
    # in the score denominator as a real ERROR rather than an inflationary SKIP.
    blocked = (
        "ERROR: Rollback journal exists for database and could not be recovered "
        "safely: Saved/EngineSource.db-journal - database is locked"
    )
    row = _run(
        "source.get_include_path",
        offline_unsupported=True,
        exe=(1, "", blocked),
        py=(0, _HEALTHY_STDOUT, ""),
    )
    check("one-sided rollback journal -> ERROR", row["status"] == "ERROR",
          f"status={row['status']}")
    check("one-sided rollback journal -> real error",
          row["error_kind"] == "real",
          f"kind={row['error_kind']}")
    metrics = _metrics([row])
    check("one-sided rollback journal remains comparable",
          metrics["comparable_actions"] == 1,
          f"comparable={metrics['comparable_actions']}")
    check("one-sided rollback journal cannot inflate score",
          metrics["offline_parity_score"] < 1.0,
          f"score={metrics['offline_parity_score']}")
    check("one-sided rollback journal increments real errors",
          metrics["real_error_count"] == 1,
          f"real={metrics['real_error_count']}")


def test_blocked_environment_preflight_invalidates_entire_run() -> None:
    blocked = (
        "ERROR: Rollback journal exists for database and could not be recovered "
        "safely: Saved/EngineSource.db-journal - global --readonly forbids recovery"
    )
    orig_exe, orig_py = opb.run_exe, opb.run_py
    opb.run_exe, opb.run_py = _fixed(1, "", 0, _HEALTHY_STDOUT, blocked, "")
    try:
        preflight = opb.preflight_execution_environment(_DUMMY, _DUMMY, _DUMMY)
    finally:
        opb.run_exe, opb.run_py = orig_exe, orig_py

    check("blocked preflight is run-level invalid",
          preflight["status"] == "environment_blocked" and not preflight["valid"],
          f"preflight={preflight}")
    check("blocked preflight does not open Python reader",
          preflight["probe"]["py"].get("not_run") is True,
          f"py={preflight['probe']['py']}")
    rows = [
        opb.environment_blocked_action(label, preflight["reason"])
        for label in ("cppreflect.get_uclass", "network.list_rpc_functions")
    ]
    metrics = _metrics(rows)
    check("blocked run has zero comparable actions",
          metrics["comparable_actions"] == 0,
          f"comparable={metrics['comparable_actions']}")
    check("blocked run score is forced to zero",
          metrics["offline_parity_score"] == 0.0,
          f"score={metrics['offline_parity_score']}")
    check("blocked run reports every invalidated row",
          metrics["environment_blocked_count"] == len(rows),
          f"blocked={metrics['environment_blocked_count']}")


def test_cmd_run_blocked_preflight_writes_zero_score_summary() -> None:
    blocked = (
        "ERROR: Rollback journal exists for database and could not be recovered "
        "safely: Saved/EngineSource.db-journal - global --readonly forbids recovery"
    )
    orig_exe, orig_py = opb.run_exe, opb.run_py
    orig_version = opb.check_version_parity
    py_calls = 0

    def _blocked_exe(ns, action, args, exe_path, mono_root):  # noqa: ANN001
        return 1, "", blocked

    def _counted_py(ns, action, args, py_path, mono_root):  # noqa: ANN001
        nonlocal py_calls
        py_calls += 1
        return 0, _HEALTHY_STDOUT, ""

    with tempfile.TemporaryDirectory() as temp_dir:
        root = pathlib.Path(temp_dir)
        exe = root / "query.exe"
        py = root / "offline.py"
        exe.write_bytes(b"test")
        py.write_text("# test\n", encoding="utf-8")
        output = root / "result"
        output.mkdir(parents=True)
        (output / "summary.json").write_text('{"stale": true}', encoding="utf-8")
        (output / "partial_summary.json").write_text('{"stale": true}', encoding="utf-8")
        (output / "run_failure.json").write_text('{"stale": true}', encoding="utf-8")
        args = SimpleNamespace(
            exe_path=str(exe),
            py_path=str(py),
            output_dir=str(output),
            label="blocked-test",
            ignore_cursor_bytes=False,
        )
        opb.run_exe, opb.run_py = _blocked_exe, _counted_py
        opb.check_version_parity = lambda *unused: (True, "test-rev", "test-rev")
        try:
            with contextlib.redirect_stdout(io.StringIO()):
                exit_code = opb.cmd_run(args)
        finally:
            opb.run_exe, opb.run_py = orig_exe, orig_py
            opb.check_version_parity = orig_version

        summary = json.loads((output / "summary.json").read_text(encoding="utf-8"))
        check("blocked cmd_run completes with diagnostic output", exit_code == 0)
        check("blocked cmd_run never launches Python reader", py_calls == 0,
              f"py_calls={py_calls}")
        check("blocked cmd_run invalidates all manifest actions",
              summary["counts"]["skip"] == opb.EXPECTED_ACTION_COUNT,
              f"skip={summary['counts']['skip']}")
        check("blocked cmd_run records run environment",
              summary["run_environment"]["status"] == "environment_blocked",
              f"environment={summary['run_environment']}")
        check("blocked cmd_run cannot produce a positive score",
              summary["metrics"]["comparable_actions"] == 0
              and summary["metrics"]["offline_parity_score"] == 0.0,
              f"metrics={summary['metrics']}")
        check("completed cmd_run removes partial summary",
              not (output / "partial_summary.json").exists())
        check("completed cmd_run removes stale failure artifact",
              not (output / "run_failure.json").exists())


def test_non_marker_preflight_failure_does_not_hide_action_errors() -> None:
    # A broad process/database error is not an environment exemption. The run
    # proceeds, allowing the ordinary action rules to record ERROR rows.
    orig_exe, orig_py = opb.run_exe, opb.run_py
    opb.run_exe, opb.run_py = _fixed(1, "", 0, _HEALTHY_STDOUT, "database is locked", "")
    try:
        preflight = opb.preflight_execution_environment(_DUMMY, _DUMMY, _DUMMY)
    finally:
        opb.run_exe, opb.run_py = orig_exe, orig_py
    check("generic preflight failure remains a valid scored run",
          preflight["status"] == "valid" and preflight["valid"],
          f"preflight={preflight}")


def test_reclassification_raises_score() -> None:
    # The score lift the task asks to demonstrate: take a small run with N rows that
    # both tools fail on, score them as plain (real ERROR) vs offline_unsupported
    # (expected_offline MATCH) and confirm the second scores strictly higher.
    err = _envelope_text({"success": False, "error": "surface unavailable offline"}, is_error=True)

    def _row(flag: bool) -> Dict[str, Any]:
        return _run("source.get_include_path", offline_unsupported=flag, compare="text",
                    exe=(1, err, "boom"), py=(1, err, "boom"))

    healthy = _run("source.get_include_path", offline_unsupported=False, compare="text",
                   exe=(0, "Engine/Actor.h", ""), py=(0, "Engine/Actor.h", ""))

    # 1 healthy MATCH + 5 both-fail rows.
    plain_rows = [healthy] + [_row(False) for _ in range(5)]
    reclassified_rows = [healthy] + [_row(True) for _ in range(5)]

    plain = _metrics(plain_rows)["offline_parity_score"]
    reclassified = _metrics(reclassified_rows)["offline_parity_score"]

    check("plain run (real errors) scores LOW", plain < 0.5, f"plain={plain}")
    check("reclassified run (expected_offline) scores HIGH", reclassified > 0.95,
          f"reclassified={reclassified}")
    check("reclassification strictly raises the score",
          reclassified > plain, f"plain={plain} reclassified={reclassified}")
    # The both-fail rows must NOT count as real errors after reclassification.
    rm = _metrics(reclassified_rows)
    check("reclassified run has 0 real errors", rm["real_error_count"] == 0,
          f"real={rm['real_error_count']}")
    check("reclassified both-fail rows counted as expected matches",
          rm["expected_error_count"] == 5, f"expected={rm['expected_error_count']}")
    # And the one-sided disagreement is still a problem (does not get masked).
    break_rows = [_run("source.get_include_path", offline_unsupported=True, compare="text",
                       exe=(0, "Engine/Actor.h", ""), py=(1, "", "no path"))]
    bm = _metrics(break_rows)
    check("offline_parity_break counts as an expected-error problem",
          bm["expected_error_problem_count"] == 1, f"problem={bm['expected_error_problem_count']}")


def test_expected_error_bucket_unchanged() -> None:
    # The pre-existing expected_error bucket still behaves the same.
    row = _run("source.read_file", offline_unsupported=False, expected_error=True,
               compare="text", exe=(1, "", "missing"), py=(1, "", "missing"))
    check("expected_error both-fail -> MATCH expected",
          row["status"] == "MATCH" and row["error_kind"] == "expected",
          f"status={row['status']} kind={row['error_kind']}")


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
