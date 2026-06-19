#!/usr/bin/env python3
"""Offline unit-test for ai_capability_benchmark scoring branches.

No live editor. Fabricates MCP responses in the canonical envelope shape
  {"result": {"content": [{"type": "text", "text": <json|message>}], "isError": <bool>}}
and monkeypatches mcp_call so every scorer runs against scripted responses. Asserts the NEW
adversarial failure modes score LOW while a healthy response scores high:

  compile_gate (validate_invalid): an EMPTY BT must make validate_behavior_tree report valid==false
                                  WITH an error-severity issue -> a stub that always reports
                                  valid==true FAILS; the real empty-BT error verdict PASSES; an
                                  isError validate (asset failed to LOAD) FAILS (not a validate
                                  verdict); valid==false with NO error issue FAILS.
  compile_gate (validate_valid) : a clean BT must make validate_behavior_tree report valid==true ->
                                  a stub that always reports valid==false FAILS; the real valid==true
                                  PASSES.
  edit_execute                  : a read-back that does NOT contain the added entity FAILS; one that
                                  does PASSES (a silent no-op edit cannot pass).
  error_path                    : an isError that does NOT name the offending identifier FAILS; one
                                  that names it PASSES (a reject-everything canned message cannot pass).
  duplicate_reject              : a server that silently succeeds on the 2nd create FAILS; one that
                                  returns a duplicate isError PASSES.

The compile_gate is built entirely from REAL Behavior Tree validate actions
(validate_behavior_tree); StateTree is compiled out on this build (WITH_STATETREE=0), so its
create/lint actions are runtime stubs and are NOT used by any execute/gate task.

Run: python Scripts/test_ai_capability_benchmark.py   (exit 0 = all asserts held)
"""

from __future__ import annotations

import json
from typing import Any, Dict, List

import ai_capability_benchmark as B


def env(text: Any, is_error: bool = False) -> Dict[str, Any]:
    """Build the canonical MCP result envelope. text may be a dict (json-encoded) or a string."""
    payload = json.dumps(text) if isinstance(text, (dict, list)) else str(text)
    return {"result": {"content": [{"type": "text", "text": payload}], "isError": is_error}}


class ScriptedMCP:
    """Replays a list of (predicate, response) rules; first matching rule wins. Predicate takes the
    (tool, arguments) of each mcp_call. Falls back to a benign empty-success envelope."""

    def __init__(self, rules: List[Any]):
        self.rules = rules
        self.calls: List[Dict[str, Any]] = []

    def __call__(self, url: str, tool: str, arguments: Dict[str, Any], timeout_s: float = 45.0) -> Dict[str, Any]:
        self.calls.append({"tool": tool, "args": dict(arguments)})
        for predicate, response in self.rules:
            if predicate(tool, arguments):
                return response
        return env({"ok": True})


def with_mcp(rules: List[Any], fn):
    original = B.mcp_call
    scripted = ScriptedMCP(rules)
    B.mcp_call = scripted
    try:
        return fn(scripted)
    finally:
        B.mcp_call = original


def action_is(name: str):
    return lambda tool, args: str(args.get("action", "")) == name


# --------------------------------------------------------------------------
# compile_gate — validate_invalid (negative) + validate_valid (positive), all REAL BT validate
# --------------------------------------------------------------------------

NEG_GATE_TASK = {
    "id": "T-neg", "category": "compile_gate", "polarity": "negative", "subsystem": "behavior_tree",
    "gate": "validate_invalid",
    "asset_path": "/Game/Benchmarks/AI/BT_BenchEmptyScratch",
    "setup_chain": [],
    "gate_args": {"action": "validate_behavior_tree",
                  "asset_path": "/Game/Benchmarks/AI/BT_BenchEmptyScratch"},
    "cleanup_chain": [], "expect_valid": False,
}

POS_GATE_TASK = {
    "id": "T-pos", "category": "compile_gate", "polarity": "positive", "subsystem": "behavior_tree",
    "gate": "validate_valid",
    "asset_path": "/Game/Benchmarks/AI/BT_BenchValidateScratch",
    "setup_chain": [{"op": "add_bt_node",
                     "args": {"action": "add_bt_node",
                              "asset_path": "/Game/Benchmarks/AI/BT_BenchValidateScratch",
                              "node_class": "BTComposite_Selector"}}],
    "gate_args": {"action": "validate_behavior_tree",
                  "asset_path": "/Game/Benchmarks/AI/BT_BenchValidateScratch"},
    "cleanup_chain": [], "expect_valid": True,
}


def test_compile_gate_negative_fails_on_valid_stub():
    # A stub validator that ALWAYS reports valid==true (everything clean) must FAIL the negative gate.
    always_valid = env({"asset_path": "x", "valid": True, "issue_count": 0, "issues": []})
    rules = [(action_is("validate_behavior_tree"), always_valid)]
    row = with_mcp(rules, lambda _: B.score_task("u", NEG_GATE_TASK, 1.0))
    assert row["direct_success"] is False, f"stub always-valid must FAIL negative gate: {row['evidence']}"


def test_compile_gate_negative_passes_on_empty_bt_error():
    # A real empty BT validates valid==false WITH an error-severity issue -> the negative gate PASSES.
    empty_err = env({"asset_path": "x", "valid": False, "issue_count": 1,
                     "issues": [{"severity": "error",
                                 "message": "Root has no children — empty Behavior Tree"}]})
    rules = [(action_is("validate_behavior_tree"), empty_err)]
    row = with_mcp(rules, lambda _: B.score_task("u", NEG_GATE_TASK, 1.0))
    assert row["direct_success"] is True, f"empty-BT valid==false + error issue must PASS negative gate: {row['evidence']}"


def test_compile_gate_negative_fails_without_error_issue():
    # valid==false but with NO error-severity issue (e.g. only warnings) must FAIL — the negative gate
    # requires the error issue, not just a false verdict (anti-reject-everything guard).
    false_no_error = env({"asset_path": "x", "valid": False, "issue_count": 1,
                          "issues": [{"severity": "warning", "message": "just a warning"}]})
    rules = [(action_is("validate_behavior_tree"), false_no_error)]
    row = with_mcp(rules, lambda _: B.score_task("u", NEG_GATE_TASK, 1.0))
    assert row["direct_success"] is False, "valid==false without an error-severity issue must FAIL negative gate"


def test_compile_gate_negative_fails_on_iserror():
    # If validate errors at the transport/isError level (asset failed to LOAD, not validate), the
    # negative gate must NOT pass — that's the anti-reject-everything guard (valid is None).
    err = env("BehaviorTree not found: x", is_error=True)
    rules = [(action_is("validate_behavior_tree"), err)]
    row = with_mcp(rules, lambda _: B.score_task("u", NEG_GATE_TASK, 1.0))
    assert row["direct_success"] is False, "isError on validate is not a validate verdict"


def test_compile_gate_positive_fails_on_invalid_stub():
    # A reject-everything stub that ALWAYS reports valid==false must FAIL the positive gate.
    add_node = env({"node_id": "GUID-1"})
    always_invalid = env({"asset_path": "x", "valid": False, "issue_count": 1, "issues": []})
    rules = [(action_is("add_bt_node"), add_node),
             (action_is("validate_behavior_tree"), always_invalid)]
    row = with_mcp(rules, lambda _: B.score_task("u", POS_GATE_TASK, 1.0))
    assert row["direct_success"] is False, f"stub always-invalid must FAIL positive gate: {row['evidence']}"


def test_compile_gate_positive_passes_on_real_valid():
    # A real well-formed BT validates valid==true -> the positive gate PASSES.
    add_node = env({"node_id": "GUID-1"})
    real_valid = env({"asset_path": "x", "valid": True, "issue_count": 0, "issues": []})
    rules = [(action_is("add_bt_node"), add_node),
             (action_is("validate_behavior_tree"), real_valid)]
    row = with_mcp(rules, lambda _: B.score_task("u", POS_GATE_TASK, 1.0))
    assert row["direct_success"] is True, f"real valid==true must PASS positive gate: {row['evidence']}"


# --------------------------------------------------------------------------
# edit_execute read-back
# --------------------------------------------------------------------------

EDIT_TASK = B._build_edit_execute_task({
    "subsystem": "blackboard", "edit_action": "add_bb_key",
    "description": "selftest add bb key",
    "chain": [{"op": "add_bb_key", "args": {"action": "add_bb_key", "asset_path": "/x",
                                            "key_name": "BenchEditBoolKey", "key_type": "Bool"}}],
    "verify": {"read_action": "get_blackboard", "read_args": {"asset_path": "/x"},
               "contains": ["BenchEditBoolKey"]},
})


def test_edit_execute_fails_on_silent_noop():
    # The edit "succeeds" (non-error) but the read-back does NOT show the key -> FAIL.
    add_ok = env({"asset_path": "/x", "message": "Key 'BenchEditBoolKey' (Bool) added"})
    readback_empty = env({"keys": []})  # key not present
    rules = [(action_is("add_bb_key"), add_ok),
             (action_is("get_blackboard"), readback_empty)]
    row = with_mcp(rules, lambda _: B.score_task("u", EDIT_TASK, 1.0))
    assert row["direct_success"] is False, f"silent no-op edit must FAIL (read-back absent): {row['evidence']}"


def test_edit_execute_passes_on_observed_mutation():
    add_ok = env({"asset_path": "/x", "message": "Key 'BenchEditBoolKey' (Bool) added"})
    readback = env({"keys": [{"key_name": "BenchEditBoolKey", "type": "Bool"}]})
    rules = [(action_is("add_bb_key"), add_ok),
             (action_is("get_blackboard"), readback)]
    row = with_mcp(rules, lambda _: B.score_task("u", EDIT_TASK, 1.0))
    assert row["direct_success"] is True, f"observed mutation must PASS: {row['evidence']}"


# --------------------------------------------------------------------------
# error_path offending-identifier
# --------------------------------------------------------------------------

ERR_TASK = {
    "id": "T-err", "category": "error_path", "namespace": "ai", "tool": B.AI_TOOL,
    "action": "remove_bb_key", "subsystem": "blackboard",
    "arguments": {"action": "remove_bb_key", "asset_path": "/x", "key_name": "NONEXISTENT_BBKEY_ZZZZ"},
    "expected": {"is_error": True, "specific_tokens": ["NONEXISTENT_BBKEY_ZZZZ"],
                 "error_tokens": ["key", "not found"]},
    "description": "selftest error path",
}


def test_error_path_fails_on_generic_rejectall():
    # A reject-everything canned message contains generic words but never the offending identifier.
    generic = env("key not found", is_error=True)
    rules = [(action_is("remove_bb_key"), generic)]
    row = with_mcp(rules, lambda _: B.score_task("u", ERR_TASK, 1.0))
    assert row["direct_success"] is False, "generic isError without the offending identifier must FAIL"
    assert row["evidence"]["generic_only"] is True


def test_error_path_passes_when_naming_identifier():
    specific = env("Key 'NONEXISTENT_BBKEY_ZZZZ' not found in blackboard", is_error=True)
    rules = [(action_is("remove_bb_key"), specific)]
    row = with_mcp(rules, lambda _: B.score_task("u", ERR_TASK, 1.0))
    assert row["direct_success"] is True, "isError naming the offending key must PASS"


def test_error_path_fails_on_silent_success():
    # No isError at all (handler silently "succeeded" on a bad input) -> FAIL.
    ok = env({"ok": True})
    rules = [(action_is("remove_bb_key"), ok)]
    row = with_mcp(rules, lambda _: B.score_task("u", ERR_TASK, 1.0))
    assert row["direct_success"] is False, "silent success on invalid input must FAIL"


# --------------------------------------------------------------------------
# duplicate_reject
# --------------------------------------------------------------------------

DUP_TASK = {
    "id": "T-dup", "category": "duplicate_reject", "namespace": "ai", "tool": B.AI_TOOL,
    "action": "add_bb_key", "subsystem": "blackboard",
    "setup_arguments": [{"action": "remove_bb_key", "asset_path": "/x", "key_name": "BenchDupKey"}],
    "arguments": {"action": "add_bb_key", "asset_path": "/x", "key_name": "BenchDupKey", "key_type": "Bool"},
    "description": "selftest dup",
}


def test_duplicate_reject_fails_on_silent_second_success():
    # Both creates "succeed" -> no duplicate guard -> FAIL.
    ok = env({"message": "Key 'BenchDupKey' (Bool) added"})
    rules = [(action_is("remove_bb_key"), env({"ok": True})),
             (action_is("add_bb_key"), ok)]
    row = with_mcp(rules, lambda _: B.score_task("u", DUP_TASK, 1.0))
    assert row["direct_success"] is False, "no duplicate guard (2nd create succeeds) must FAIL"


def test_duplicate_reject_passes_on_second_isError():
    # First create succeeds, second returns a duplicate-specific isError -> PASS. Use a stateful
    # rule so call #1 succeeds and call #2 errors.
    state = {"adds": 0}
    first_ok = env({"message": "Key 'BenchDupKey' (Bool) added"})
    dup_err = env("Key 'BenchDupKey' already exists in this blackboard", is_error=True)

    class StatefulMCP(ScriptedMCP):
        def __call__(self, url, tool, arguments, timeout_s=45.0):
            self.calls.append({"tool": tool, "args": dict(arguments)})
            if str(arguments.get("action")) == "remove_bb_key":
                return env({"ok": True})
            if str(arguments.get("action")) == "add_bb_key":
                state["adds"] += 1
                return first_ok if state["adds"] == 1 else dup_err
            return env({"ok": True})

    original = B.mcp_call
    B.mcp_call = StatefulMCP([])
    try:
        row = B.score_task("u", DUP_TASK, 1.0)
    finally:
        B.mcp_call = original
    assert row["direct_success"] is True, f"clean first + duplicate isError second must PASS: {row['evidence']}"


# --------------------------------------------------------------------------
# composite: a fully-healthy run scores ~1.0, a fully-broken run scores low
# --------------------------------------------------------------------------

def test_aggregate_weights_sum_and_composite_span():
    # Healthy rows for every weighted category -> composite ~1.0.
    healthy = [{"category": c, "direct_success": True, "transport_error": False,
                "response_is_error": False, "subsystem": "state_tree"} for c in B.WEIGHTS]
    summ = B.aggregate("healthy", {}, [{"category": c} for c in B.WEIGHTS], healthy)
    assert abs(summ["metrics"]["ai_capability_score"] - 1.0) < 1e-9, summ["metrics"]

    broken = [{"category": c, "direct_success": False, "transport_error": False,
               "response_is_error": True, "subsystem": "state_tree"} for c in B.WEIGHTS]
    summ_b = B.aggregate("broken", {}, [{"category": c} for c in B.WEIGHTS], broken)
    assert summ_b["metrics"]["ai_capability_score"] == 0.0, summ_b["metrics"]


def main() -> int:
    tests = [v for k, v in sorted(globals().items()) if k.startswith("test_") and callable(v)]
    failures = 0
    for t in tests:
        try:
            t()
            print(f"PASS {t.__name__}")
        except AssertionError as exc:
            failures += 1
            print(f"FAIL {t.__name__}: {exc}")
        except Exception as exc:  # noqa: BLE001
            failures += 1
            print(f"ERROR {t.__name__}: {type(exc).__name__}: {exc}")
    print(f"\n{len(tests) - failures}/{len(tests)} passed")
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
