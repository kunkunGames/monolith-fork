#!/usr/bin/env python3
"""Offline unit tests for the OfflineParity benchmark.

No live editor / MCP server and no real subprocess is launched.  Each test
monkeypatches ``run_exe`` / ``run_py`` to return fabricated
``(returncode, stdout, stderr)`` tuples.  The exe/py offline tools print the same
JSON payload an MCP ``tools/call`` would carry, so the fabricated stdout is built
from the real envelope shape::

    {"result": {"content": [{"type": "text", "text": <json>}], "isError": <bool>}}

The tests cover two things the 2026-06-18 hardening added:

  ITEM 1 -- the action table loads from Benchmarks/OfflineParity/actions.jsonl and
            the manifest action_count matches the data-file line count.
  ITEM 2 -- the ``offline_unsupported`` bucket: when BOTH offline tools agree on a
            failure the row is a MATCH(expected_offline) that does NOT drag the
            score (the reclassification); a genuine exe-vs-py disagreement
            (exactly one tool fails) is a DIFF(offline_parity_break) that scores
            LOW; and the same row scored as a plain row is a real ERROR.

Run::

    python Plugins/Monolith/Scripts/test_offline_parity_benchmark.py
"""

from __future__ import annotations

import importlib.util
import json
import pathlib
import sys
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
