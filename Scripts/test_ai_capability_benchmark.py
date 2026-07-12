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
                                  does PASSES (a silent no-op edit cannot pass). Create chains must
                                  also delete every generated package and prove public-read absence.
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
import pathlib
import tempfile
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


def status_env() -> Dict[str, Any]:
    return env({"server_running": True, "catalog_version": "sha256:test"})


def run_task(index: int) -> Dict[str, Any]:
    return {
        "id": f"T-run-{index}",
        "category": "discovery",
        "namespace": "ai",
        "action": "list_ai_assets",
        "tool": B.AI_TOOL,
        "arguments": {"action": "list_ai_assets"},
        "expected": {},
        "subsystem": "behavior_tree",
        "description": f"offline run-integrity task {index}",
    }


def run_row(
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
        "subsystem": task.get("subsystem", ""),
        "direct_success": not transport and not protocol,
        "planning_signals": False,
        "evidence": {},
        "transport_error": transport,
        "transport_status": status,
        "transport_error_raw": raw,
        "transport_failure_call_count": 1 if transport else 0,
        "last_transport_tool": B.AI_TOOL if transport else "",
        "last_transport_action": task["action"] if transport else "",
        "protocol_error": protocol,
        "protocol_error_raw": raw if protocol else "",
        "protocol_failure_call_count": 1 if protocol else 0,
        "last_protocol_tool": B.AI_TOOL if protocol else "",
        "last_protocol_action": task["action"] if protocol else "",
        "failure_kind": "protocol_error" if protocol else "",
        "response_is_error": False,
        "response_text": "",
    }


def run_with_fake_rows(
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
    original_call = B.mcp_call
    original_score = B.score_task
    B.mcp_call = lambda url, tool, arguments, timeout_s=45.0: (
        status_env() if status_response is None else status_response
    )
    B.score_task = fake_score
    try:
        return B.run_benchmark(
            "http://unused",
            tasks_path,
            output_dir,
            "selftest",
            1.0,
            require_fixtures=False,
            allow_subset=True,
            **kwargs,
        )
    finally:
        B.mcp_call = original_call
        B.score_task = original_score


def action_is(name: str):
    return lambda tool, args: str(args.get("action", "")) == name


# --------------------------------------------------------------------------
# compile_gate — validate_invalid (negative) + validate_valid (positive), all REAL BT validate
# --------------------------------------------------------------------------

NEG_GATE_TASK = {
    "id": "T-neg", "category": "compile_gate", "polarity": "negative", "subsystem": "behavior_tree",
    "gate": "validate_invalid",
    "asset_path": "/Game/Benchmarks/AI/BT_BenchNegativeGateScratch",
    "setup_chain": [
        {"op": "delete_behavior_tree", "allow_absent": True,
         "args": {"action": "delete_behavior_tree",
                  "asset_path": "/Game/Benchmarks/AI/BT_BenchNegativeGateScratch"}},
        {"op": "create_behavior_tree",
         "args": {"action": "create_behavior_tree",
                  "save_path": "/Game/Benchmarks/AI/BT_BenchNegativeGateScratch",
                  "name": "BT_BenchNegativeGateScratch"}},
    ],
    "gate_args": {"action": "validate_behavior_tree",
                  "asset_path": "/Game/Benchmarks/AI/BT_BenchNegativeGateScratch"},
    "cleanup_chain": [{"op": "delete_behavior_tree",
                       "args": {"action": "delete_behavior_tree",
                                "asset_path": "/Game/Benchmarks/AI/BT_BenchNegativeGateScratch"}}],
    "cleanup_verify": {"action": "get_behavior_tree",
                       "asset_path": "/Game/Benchmarks/AI/BT_BenchNegativeGateScratch",
                       "absent": ["NeverPresent"]},
    "expect_valid": False,
}

POS_GATE_TASK = {
    "id": "T-pos", "category": "compile_gate", "polarity": "positive", "subsystem": "behavior_tree",
    "gate": "validate_valid",
    "asset_path": "/Game/Benchmarks/AI/BT_BenchPositiveGateScratch",
    "setup_chain": [
        {"op": "delete_behavior_tree", "allow_absent": True,
         "args": {"action": "delete_behavior_tree",
                  "asset_path": "/Game/Benchmarks/AI/BT_BenchPositiveGateScratch"}},
        {"op": "create_behavior_tree",
         "args": {"action": "create_behavior_tree",
                  "save_path": "/Game/Benchmarks/AI/BT_BenchPositiveGateScratch",
                  "name": "BT_BenchPositiveGateScratch"}},
        {"op": "add_bt_node",
         "args": {"action": "add_bt_node",
                  "asset_path": "/Game/Benchmarks/AI/BT_BenchPositiveGateScratch",
                  "node_class": "BTComposite_Selector"}},
    ],
    "gate_args": {"action": "validate_behavior_tree",
                  "asset_path": "/Game/Benchmarks/AI/BT_BenchPositiveGateScratch"},
    "cleanup_chain": [{"op": "delete_behavior_tree",
                       "args": {"action": "delete_behavior_tree",
                                "asset_path": "/Game/Benchmarks/AI/BT_BenchPositiveGateScratch"}}],
    "cleanup_verify": {"action": "get_behavior_tree",
                       "asset_path": "/Game/Benchmarks/AI/BT_BenchPositiveGateScratch",
                       "absent": ["NeverPresent"]},
    "expect_valid": True,
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


def _template_lifecycle_task() -> Dict[str, Any]:
    return next(
        task for task in B.build_static_tasks()
        if task.get("category") == "edit_execute"
        and task.get("edit_action") == "create_bt_from_template"
    )


class TemplateLifecycleMCP:
    def __init__(self, *, residual_after_cleanup: bool = False, primary_readback_ok: bool = True):
        self.residual_after_cleanup = residual_after_cleanup
        self.primary_readback_ok = primary_readback_ok
        self.calls: List[Dict[str, Any]] = []
        self.delete_counts = {"delete_behavior_tree": 0, "delete_blackboard": 0}
        self.bt_reads = 0

    def __call__(self, url: str, tool: str, arguments: Dict[str, Any], timeout_s: float = 45.0) -> Dict[str, Any]:
        self.calls.append({"tool": tool, "args": dict(arguments)})
        action = str(arguments.get("action", ""))
        if action in self.delete_counts:
            self.delete_counts[action] += 1
            if self.delete_counts[action] == 1:
                path = str(arguments.get("asset_path", ""))
                return env(f"Asset not found: {path}", is_error=True)
            return env({"asset_path": arguments.get("asset_path"), "message": "deleted"})
        if action == "create_bt_from_template":
            return env({"asset_path": B.BT_TEMPLATE_SCRATCH, "blackboard": B.BB_TEMPLATE_SCRATCH})
        if action == "get_behavior_tree":
            self.bt_reads += 1
            if self.bt_reads == 1:
                blackboard = "BB_BenchTemplateScratch" if self.primary_readback_ok else "WrongBlackboard"
                return env({"asset_path": B.BT_TEMPLATE_SCRATCH, "blackboard": blackboard})
            if self.residual_after_cleanup:
                return env({"asset_path": B.BT_TEMPLATE_SCRATCH, "blackboard": "BB_BenchTemplateScratch"})
            return env(f"BehaviorTree not found: {B.BT_TEMPLATE_SCRATCH}", is_error=True)
        if action == "get_blackboard":
            return env(f"Asset not found: {B.BB_TEMPLATE_SCRATCH}", is_error=True)
        return env({"ok": True})


def test_create_edit_execute_cleans_all_generated_packages_and_verifies_absence():
    scripted = TemplateLifecycleMCP()
    row = B._score_edit_execute_chain("u", _template_lifecycle_task(), 1.0, scripted)
    assert row["direct_success"] is True, row["evidence"]
    assert scripted.delete_counts == {"delete_behavior_tree": 2, "delete_blackboard": 2}
    assert row["evidence"]["cleanup_ok"] is True
    assert row["evidence"]["cleanup_verify_ok"] is True
    assert len(row["evidence"]["cleanup_verify"]["checks"]) == 2


def test_create_edit_execute_fails_if_cleanup_readback_finds_residual_package():
    scripted = TemplateLifecycleMCP(residual_after_cleanup=True)
    row = B._score_edit_execute_chain("u", _template_lifecycle_task(), 1.0, scripted)
    assert row["evidence"]["primary_ok"] is True
    assert row["evidence"]["cleanup_ok"] is True
    assert row["evidence"]["cleanup_verify_ok"] is False
    assert row["direct_success"] is False


def test_create_edit_execute_cleans_up_even_when_primary_readback_fails():
    scripted = TemplateLifecycleMCP(primary_readback_ok=False)
    row = B._score_edit_execute_chain("u", _template_lifecycle_task(), 1.0, scripted)
    assert row["evidence"]["primary_ok"] is False
    assert row["evidence"]["cleanup_attempted"] is True
    assert row["evidence"]["cleanup_ok"] is True
    assert row["evidence"]["cleanup_verify_ok"] is True
    assert row["direct_success"] is False


def test_create_edit_execute_integrity_rejects_missing_cleanup_contract():
    task = _template_lifecycle_task()
    task.pop("cleanup_chain", None)
    try:
        B.validate_task_integrity([task])
    except RuntimeError as exc:
        assert "must declare cleanup actions" in str(exc)
    else:
        raise AssertionError("create edit_execute without cleanup must fail integrity validation")


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
    "cleanup_arguments": [{"action": "remove_bb_key", "asset_path": "/x", "key_name": "BenchDupKey"}],
    "cleanup_verify": {"action": "get_blackboard", "asset_path": "/x",
                       "absent": ["BenchDupKey"]},
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
    assert row["evidence"]["cleanup_ok"] is True


def test_duplicate_reject_accepts_only_explicit_absence_during_setup():
    state = {"adds": 0, "removes": 0}
    first_ok = env({"message": "Key 'BenchDupKey' (Bool) added"})
    dup_err = env("Key 'BenchDupKey' already exists in this blackboard", is_error=True)

    class StatefulMCP(ScriptedMCP):
        def __call__(self, url, tool, arguments, timeout_s=45.0):
            self.calls.append({"tool": tool, "args": dict(arguments)})
            action = str(arguments.get("action"))
            if action == "remove_bb_key":
                state["removes"] += 1
                if state["removes"] == 1:
                    return env("Key 'BenchDupKey' not found in blackboard /x", is_error=True)
                return env({"removed": "BenchDupKey"})
            if action == "add_bb_key":
                state["adds"] += 1
                return first_ok if state["adds"] == 1 else dup_err
            return env({"ok": True})

    original = B.mcp_call
    B.mcp_call = StatefulMCP([])
    try:
        row = B.score_task("u", DUP_TASK, 1.0)
    finally:
        B.mcp_call = original
    assert row["direct_success"] is True, row["evidence"]
    assert row["evidence"]["setup_steps"][0]["expected_absence"] is True
    assert state == {"adds": 2, "removes": 2}


def test_duplicate_reject_fails_closed_when_scratch_reset_fails():
    calls: List[str] = []

    class ResetFailureMCP(ScriptedMCP):
        def __call__(self, url, tool, arguments, timeout_s=45.0):
            action = str(arguments.get("action"))
            calls.append(action)
            if action == "remove_bb_key":
                return env("Failed to delete /x because the package is source controlled", is_error=True)
            return env({"ok": True})

    original = B.mcp_call
    B.mcp_call = ResetFailureMCP([])
    try:
        row = B.score_task("u", DUP_TASK, 1.0)
    finally:
        B.mcp_call = original
    assert row["direct_success"] is False
    assert row["evidence"]["first_call_attempted"] is False
    assert calls == ["remove_bb_key"], "failed reset must skip both create calls and cleanup"


def test_duplicate_reject_requires_cleanup_success():
    state = {"adds": 0, "removes": 0}

    class CleanupFailureMCP(ScriptedMCP):
        def __call__(self, url, tool, arguments, timeout_s=45.0):
            action = str(arguments.get("action"))
            if action == "remove_bb_key":
                state["removes"] += 1
                if state["removes"] == 1:
                    return env("Key 'BenchDupKey' not found in blackboard /x", is_error=True)
                return env("Failed to delete /x because the package is locked", is_error=True)
            if action == "add_bb_key":
                state["adds"] += 1
                if state["adds"] == 1:
                    return env({"message": "Key 'BenchDupKey' (Bool) added"})
                return env("Key 'BenchDupKey' already exists in this blackboard", is_error=True)
            return env({"ok": True})

    original = B.mcp_call
    B.mcp_call = CleanupFailureMCP([])
    try:
        row = B.score_task("u", DUP_TASK, 1.0)
    finally:
        B.mcp_call = original
    assert row["direct_success"] is False
    assert row["evidence"]["second_is_duplicate"] is True
    assert row["evidence"]["cleanup_ok"] is False


def test_duplicate_reject_fails_when_cleanup_readback_finds_residual_state():
    state = {"adds": 0, "removes": 0}

    class ResidualMCP(ScriptedMCP):
        def __call__(self, url, tool, arguments, timeout_s=45.0):
            action = str(arguments.get("action"))
            if action == "remove_bb_key":
                state["removes"] += 1
                if state["removes"] == 1:
                    return env("Key 'BenchDupKey' not found in blackboard /x", is_error=True)
                return env({"removed": "BenchDupKey"})
            if action == "add_bb_key":
                state["adds"] += 1
                if state["adds"] == 1:
                    return env({"message": "Key 'BenchDupKey' (Bool) added"})
                return env("Key 'BenchDupKey' already exists in this blackboard", is_error=True)
            if action == "get_blackboard":
                return env({"keys": [{"name": "BenchDupKey"}]})
            return env({"ok": True})

    original = B.mcp_call
    B.mcp_call = ResidualMCP([])
    try:
        row = B.score_task("u", DUP_TASK, 1.0)
    finally:
        B.mcp_call = original
    assert row["evidence"]["cleanup_ok"] is True
    assert row["evidence"]["cleanup_verify_ok"] is False
    assert row["direct_success"] is False


# --------------------------------------------------------------------------
# run-integrity contract
# --------------------------------------------------------------------------

def test_mcp_call_non_object_json_is_protocol_error():
    class FakeResponse:
        headers: Dict[str, str] = {}

        def __enter__(self):
            return self

        def __exit__(self, exc_type, exc, tb):
            return False

        def read(self):
            return b"[]"

    original = B.urllib.request.urlopen
    B.urllib.request.urlopen = lambda request, timeout=45.0: FakeResponse()
    try:
        response = B.mcp_call("http://unused", "monolith_status", {}, 1.0)
    finally:
        B.urllib.request.urlopen = original
    assert response["protocol_error"] is True
    assert "top-level JSON" in response["error"]


def test_edit_execute_attributes_chain_transport():
    down = {"transport_error": True, "status": 503, "raw": "chain down"}
    row = with_mcp(
        [(action_is("add_bb_key"), down)],
        lambda _: B.score_task("u", EDIT_TASK, 1.0),
    )
    assert row["direct_success"] is False
    assert row["transport_error"] is True
    assert row["transport_status"] == 503
    assert row["transport_error_raw"] == "chain down"
    assert row["last_transport_action"] == "add_bb_key"


def test_edit_execute_attributes_readback_transport():
    add_ok = env({"asset_path": "/x", "message": "added"})
    down = {"transport_error": True, "status": None, "raw": "readback down"}
    row = with_mcp(
        [(action_is("add_bb_key"), add_ok), (action_is("get_blackboard"), down)],
        lambda _: B.score_task("u", EDIT_TASK, 1.0),
    )
    assert row["direct_success"] is False
    assert row["transport_error"] is True
    assert row["transport_error_raw"] == "readback down"
    assert row["last_transport_action"] == "get_blackboard"


def test_duplicate_attributes_setup_transport_and_skips_probe():
    state = {"adds": 0}

    class StatefulMCP(ScriptedMCP):
        def __call__(self, url, tool, arguments, timeout_s=45.0):
            action = str(arguments.get("action", ""))
            if action == "remove_bb_key":
                return {"transport_error": True, "status": 502, "raw": "setup down"}
            if action == "add_bb_key":
                state["adds"] += 1
                if state["adds"] == 1:
                    return {"transport_error": True, "status": 503, "raw": "first down"}
                return env("already exists", is_error=True)
            return env({"ok": True})

    original = B.mcp_call
    B.mcp_call = StatefulMCP([])
    try:
        row = B.score_task("u", DUP_TASK, 1.0)
    finally:
        B.mcp_call = original
    assert row["direct_success"] is False
    assert row["transport_failure_call_count"] == 1
    assert row["transport_status"] == 502
    assert row["transport_error_raw"] == "setup down"
    assert row["evidence"]["first_call_attempted"] is False
    assert state["adds"] == 0


def test_duplicate_attributes_first_call_transport_skips_second_and_cleans_up():
    state = {"adds": 0, "removes": 0}

    class StatefulMCP(ScriptedMCP):
        def __call__(self, url, tool, arguments, timeout_s=45.0):
            action = str(arguments.get("action", ""))
            if action == "remove_bb_key":
                state["removes"] += 1
                if state["removes"] == 1:
                    return env("Key 'BenchDupKey' not found in blackboard /x", is_error=True)
                return env({"removed": "BenchDupKey"})
            if action == "add_bb_key":
                state["adds"] += 1
                return {"transport_error": True, "status": 503, "raw": "first down"}
            return env({"ok": True})

    original = B.mcp_call
    B.mcp_call = StatefulMCP([])
    try:
        row = B.score_task("u", DUP_TASK, 1.0)
    finally:
        B.mcp_call = original
    assert row["direct_success"] is False
    assert row["transport_failure_call_count"] == 1
    assert row["transport_status"] == 503
    assert row["transport_error_raw"] == "first down"
    assert row["evidence"]["second_call_attempted"] is False
    assert row["evidence"]["cleanup_ok"] is True
    assert state == {"adds": 1, "removes": 2}


def test_compile_gate_attributes_setup_and_cleanup_transport():
    task = json.loads(json.dumps(POS_GATE_TASK))
    task["cleanup_chain"] = [{"args": {"action": "remove_bt_node", "asset_path": "/x"}}]
    setup_down = {"transport_error": True, "status": 502, "raw": "setup down"}
    cleanup_down = {"transport_error": True, "status": 503, "raw": "cleanup down"}
    row = with_mcp(
        [
            (action_is("add_bt_node"), setup_down),
            (action_is("validate_behavior_tree"), env({"valid": True, "issues": []})),
            (action_is("remove_bt_node"), cleanup_down),
        ],
        lambda _: B.score_task("u", task, 1.0),
    )
    assert row["direct_success"] is False
    assert row["transport_failure_call_count"] == 2
    assert row["transport_status"] == 503
    assert row["last_transport_action"] == "remove_bt_node"


def test_status_protocol_failure_clears_stale_outputs_and_writes_no_summary():
    with tempfile.TemporaryDirectory() as tmp:
        root = pathlib.Path(tmp)
        output = root / "run"
        output.mkdir()
        for name in B.RUN_OUTPUT_FILENAMES:
            (output / name).write_text("stale", encoding="utf-8")
        result = run_with_fake_rows(
            [run_task(1)],
            lambda url, task, timeout_s: run_row(task),
            output_dir=output,
            status_response={"parse_error": True, "raw": "not-json"},
        )
        assert result["failure_stage"] == "status_preflight"
        assert result["failure_kind"] == "protocol_error"
        assert (output / "run_failure.json").exists()
        assert not (output / "summary.json").exists()
        assert not (output / "partial_summary.json").exists()
        assert not (output / "per_task.json").exists()


def test_status_transport_rpc_error_and_invalid_payload_abort_before_tasks():
    calls = 0

    def fake_score(url, task, timeout_s):
        nonlocal calls
        calls += 1
        return run_row(task)

    invalid_responses = [
        ({"transport_error": True, "status": 503, "raw": "status down"}, "transport_error"),
        ({"error": {"code": -32603, "message": "gateway failed"}}, "protocol_error"),
        (env({}), "invalid_status_payload"),
    ]
    with tempfile.TemporaryDirectory() as tmp:
        root = pathlib.Path(tmp)
        for index, (response, expected_kind) in enumerate(invalid_responses, 1):
            output = root / f"run-{index}"
            result = run_with_fake_rows(
                [run_task(1)],
                fake_score,
                output_dir=output,
                status_response=response,
            )
            assert result["failure_kind"] == expected_kind, result
            assert not (output / "summary.json").exists()
        assert calls == 0


def test_compile_cleanup_exception_invalidates_run_and_preserves_trigger_row():
    task = json.loads(json.dumps(NEG_GATE_TASK))
    task.update({
        "namespace": "ai",
        "action": "validate_behavior_tree",
        "tool": B.AI_TOOL,
        "arguments": dict(task["gate_args"]),
        "expected": {},
        "description": "offline cleanup exception contract",
    })
    task["cleanup_chain"] = [{"args": {"action": "delete_behavior_tree",
                                        "asset_path": task["asset_path"]}}]
    delete_calls = 0

    def fake_call(url, tool, arguments, timeout_s=45.0):
        nonlocal delete_calls
        if tool == "monolith_status":
            return status_env()
        action = str(arguments.get("action", ""))
        if action == "validate_behavior_tree":
            return env({
                "valid": False,
                "issue_count": 1,
                "issues": [{"severity": "error", "message": "empty"}],
            })
        if action == "delete_behavior_tree":
            delete_calls += 1
            if delete_calls == 1:
                return env("Behavior Tree not found", is_error=True)
            raise RuntimeError("cleanup exploded")
        return env({"ok": True})

    with tempfile.TemporaryDirectory() as tmp:
        root = pathlib.Path(tmp)
        tasks_path = root / "tasks.jsonl"
        tasks_path.write_text(json.dumps(task) + "\n", encoding="utf-8")
        output = root / "run"
        original = B.mcp_call
        B.mcp_call = fake_call
        try:
            result = B.run_benchmark(
                "http://unused",
                tasks_path,
                output,
                "cleanup-exception",
                1.0,
                require_fixtures=False,
                allow_subset=True,
            )
        finally:
            B.mcp_call = original
        row = json.loads((output / "per_task.jsonl").read_text(encoding="utf-8"))
        assert result["completion_status"] == "aborted_runner_exception"
        assert row["failure_kind"] == "runner_exception"
        assert "cleanup exploded" in row["error"]
        assert not (output / "summary.json").exists()


def test_three_consecutive_transport_failures_abort_on_third_task():
    tasks = [run_task(index) for index in range(1, 7)]
    calls: List[str] = []

    def fake_score(url, task, timeout_s):
        calls.append(task["id"])
        return run_row(task, transport=True, status=503, raw="down")

    with tempfile.TemporaryDirectory() as tmp:
        output = pathlib.Path(tmp) / "run"
        result = run_with_fake_rows(tasks, fake_score, output_dir=output)
        assert result["completion_status"] == "aborted_transport_failure_budget"
        assert result["transport_gate_reason"] == "consecutive_transport_failures"
        assert result["completed_task_count"] == 3
        assert calls == [task["id"] for task in tasks[:3]]
        assert not (output / "summary.json").exists()


def test_fraction_gate_aborts_at_twentieth_and_keeps_last_transport_identity():
    tasks = [run_task(index) for index in range(1, 23)]
    calls = 0

    def fake_score(url, task, timeout_s):
        nonlocal calls
        calls += 1
        failed = calls in {1, 6}
        return run_row(
            task,
            transport=failed,
            status=503 if failed else None,
            raw=f"down-{calls}" if failed else "",
        )

    with tempfile.TemporaryDirectory() as tmp:
        result = run_with_fake_rows(tasks, fake_score, output_dir=pathlib.Path(tmp) / "run")
        assert result["transport_gate_reason"] == "transport_failed_fraction"
        assert result["completed_task_count"] == 20
        assert result["last_task_id"] == "T-run-6"
        assert result["last_transport_status"] == 503
        assert result["last_transport_error_raw"] == "down-6"


def test_exact_five_percent_succeeds_and_removes_stale_failure_and_partial():
    tasks = [run_task(index) for index in range(1, 21)]
    calls = 0

    def fake_score(url, task, timeout_s):
        nonlocal calls
        calls += 1
        return run_row(task, transport=calls == 1, status=503, raw="one-down")

    with tempfile.TemporaryDirectory() as tmp:
        output = pathlib.Path(tmp) / "run"
        output.mkdir()
        (output / "run_failure.json").write_text("stale", encoding="utf-8")
        (output / "partial_summary.json").write_text("stale", encoding="utf-8")
        result = run_with_fake_rows(tasks, fake_score, output_dir=output)
        assert result["run_valid"] is True
        assert result["transport_failure_count"] == 1
        assert (output / "summary.json").exists()
        assert (output / "per_task.json").exists()
        assert not (output / "partial_summary.json").exists()
        assert not (output / "run_failure.json").exists()


def test_short_population_transport_fraction_fails_at_finalize():
    tasks = [run_task(index) for index in range(1, 11)]
    calls = 0

    def fake_score(url, task, timeout_s):
        nonlocal calls
        calls += 1
        return run_row(task, transport=calls == 1, status=503, raw="one-down")

    with tempfile.TemporaryDirectory() as tmp:
        output = pathlib.Path(tmp) / "run"
        result = run_with_fake_rows(
            tasks,
            fake_score,
            output_dir=output,
            max_consecutive_transport_failures=20,
        )
        assert result["completion_status"] == "completed_transport_failure_budget_exceeded"
        assert result["transport_gate_reason"] == "final_transport_failed_fraction"
        assert result["last_task_id"] == "T-run-1"
        assert not (output / "summary.json").exists()


def test_nontransport_response_resets_transport_streak():
    tasks = [run_task(index) for index in range(1, 6)]
    failures = {1, 2, 4, 5}
    calls = 0

    def fake_score(url, task, timeout_s):
        nonlocal calls
        calls += 1
        row = run_row(task, transport=calls in failures, raw="down")
        if calls == 3:
            row["response_is_error"] = True
            row["direct_success"] = True
        return row

    with tempfile.TemporaryDirectory() as tmp:
        result = run_with_fake_rows(
            tasks,
            fake_score,
            output_dir=pathlib.Path(tmp) / "run",
            max_transport_failed_fraction=1.0,
        )
        assert result["run_valid"] is True
        assert result["consecutive_transport_failures"] == 2


def test_task_protocol_error_and_runner_exception_write_invalid_artifacts():
    with tempfile.TemporaryDirectory() as tmp:
        root = pathlib.Path(tmp)
        protocol_output = root / "protocol"
        protocol = run_with_fake_rows(
            [run_task(1)],
            lambda url, task, timeout_s: run_row(task, protocol=True, raw="bad envelope"),
            output_dir=protocol_output,
        )
        assert protocol["completion_status"] == "aborted_protocol_error"
        assert not (protocol_output / "summary.json").exists()

        exception_output = root / "exception"

        def explode(url, task, timeout_s):
            raise RuntimeError("score exploded")

        exception = run_with_fake_rows(
            [run_task(1)], explode, output_dir=exception_output
        )
        row = json.loads((exception_output / "per_task.jsonl").read_text(encoding="utf-8"))
        assert exception["completion_status"] == "aborted_runner_exception"
        assert row["failure_kind"] == "runner_exception"
        assert "score exploded" in row["error"]
        assert not (exception_output / "summary.json").exists()


def test_manifest_run_gates_match_runner_defaults():
    with tempfile.TemporaryDirectory() as tmp:
        root = pathlib.Path(tmp)
        manifest = B.generate_tasks(root / "tasks.jsonl", root / "manifest.json")
        gates = manifest["run_gates"]
        assert gates["max_transport_failed_fraction"] == B.DEFAULT_MAX_TRANSPORT_FAILED_FRACTION
        assert gates["max_consecutive_transport_failures"] == B.DEFAULT_MAX_CONSECUTIVE_TRANSPORT_FAILURES
        assert gates["min_transport_fraction_sample"] == B.DEFAULT_MIN_TRANSPORT_FRACTION_SAMPLES
        assert gates["invalid_run_writes_summary"] is False


def test_main_returns_nonzero_for_invalid_run():
    original = B.run_benchmark
    B.run_benchmark = lambda *args, **kwargs: {"run_valid": False}
    try:
        rc = B.main(["run", "--output-dir", "unused", "--label", "invalid", "--skip-preflight"])
    finally:
        B.run_benchmark = original
    assert rc == 1


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
