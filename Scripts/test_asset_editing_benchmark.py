#!/usr/bin/env python3
"""Offline contract tests for the AssetEditing benchmark corpus.

This script does not contact Unreal Editor or the MCP endpoint. It verifies
that the checked-in AssetEditing task set is internally consistent and still
exercises the high-error Blueprint recovery actions called out by
SPEC_MonolithPractitionerWorkflowROI P0.4.
"""

from __future__ import annotations

import collections
import json
import os
import pathlib
import re
import stat
import sys
import tempfile
from typing import Any, Dict, Iterable, List, Set

_SCRIPTS_DIR = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(_SCRIPTS_DIR))

import asset_editing_benchmark as aeb  # noqa: E402
import benchmark_common as benchmark_common  # noqa: E402

REQUIRED_BLUEPRINT_RECOVERY_ACTIONS = {
    "remove_event_dispatcher",
    "add_event_node",
    "rename_function",
    "override_parent_function",
    "duplicate_graph",
    "add_component",
    "create_blueprint",
}

READBACK_REQUIRED_CATEGORIES = {
    "asset_authoring",
    "edit_execute",
    "workflow_execute",
}

# The CONSTANT stub the MCP server puts in content[0].text for a successful tools/call when
# bEnableStructuredToolResults=True (Config/DefaultMonolith.ini). The payload is in
# structuredContent only — see Docs/specs/SPEC_MonolithStructuredToolResults.md.
STRUCTURED_STUB_TEXT = "OK; see structuredContent."

_FAILURES: List[str] = []


def check(name: str, condition: bool, detail: str = "") -> None:
    status = "PASS" if condition else "FAIL"
    print(f"[{status}] {name}" + (f" -- {detail}" if detail else ""))
    if not condition:
        _FAILURES.append(name)


def test_generator_preserves_expected_read_only_outputs() -> None:
    with tempfile.TemporaryDirectory(prefix="aeb-generator-") as temp_dir:
        root = pathlib.Path(temp_dir)
        expected = root / "testcases" / "expected.json"
        stale = root / "testcases" / "stale.json"
        payload = {"generated_by": "asset_editing_benchmark.py", "value": 1}
        aeb.write_json(expected, payload)
        aeb.write_json(stale, payload)
        expected.chmod(stat.S_IREAD)
        try:
            changed = aeb.write_json(expected, payload)
            aeb.remove_stale_generated_json(
                expected.parent,
                [expected],
                recursive=False,
            )
            check(
                "unchanged read-only generated output is not rewritten",
                changed is False and expected.exists(),
            )
            check(
                "stale generated output is removed without deleting expected output",
                not stale.exists() and expected.exists(),
            )
        finally:
            expected.chmod(stat.S_IWRITE | stat.S_IREAD)


def test_generator_uses_platform_native_line_endings() -> None:
    with tempfile.TemporaryDirectory(prefix="aeb-generator-newlines-") as temp_dir:
        output = pathlib.Path(temp_dir) / "generated.json"
        payload = {"generated_by": "asset_editing_benchmark.py", "values": [1, 2]}

        changed = aeb.write_json(output, payload)
        canonical_text = json.dumps(payload, indent=2, ensure_ascii=False) + "\n"
        expected_bytes = canonical_text.replace("\n", os.linesep).encode("utf-8")

        check(
            "generated output uses the platform checkout line ending",
            changed is True and output.read_bytes() == expected_bytes,
        )


def load_json(path: pathlib.Path) -> Dict[str, Any]:
    return json.loads(aeb.resolve_plugin_path(path).read_text(encoding="utf-8"))


def task_actions(task: Dict[str, Any]) -> Set[str]:
    actions: Set[str] = set()

    def visit(value: Any) -> None:
        if isinstance(value, dict):
            action = value.get("action") or value.get("op")
            if isinstance(action, str) and action:
                actions.add(action)
            for child in value.values():
                visit(child)
        elif isinstance(value, list):
            for child in value:
                visit(child)

    action = task.get("action")
    if isinstance(action, str) and action:
        actions.add(action)
    visit(task.get("arguments"))
    visit(task.get("chain"))
    visit(task.get("setup_chain"))
    visit(task.get("cleanup_chain"))
    visit(task.get("verify"))
    return actions


def has_readback(task: Dict[str, Any]) -> bool:
    verify = task.get("verify")
    capabilities = task.get("capabilities", {})
    if bool(verify):
        return True
    if isinstance(capabilities, dict):
        if capabilities.get("readback_verifies") is True:
            return True
        operations = capabilities.get("asset_operations")
        if isinstance(operations, list) and "readback_verify" in operations:
            return True
    return False


def count_rows_by_field(rows: Iterable[Dict[str, Any]], field: str) -> Dict[str, int]:
    counts: Dict[str, int] = {}
    for row in rows:
        key = str(row.get(field, ""))
        counts[key] = counts.get(key, 0) + 1
    return counts


def mcp_success_response(data: Dict[str, Any]) -> Dict[str, Any]:
    """Build the compact structured MCP success envelope used by offline scorer tests.

    This is the live shape when ``bEnableStructuredToolResults=True`` (Config/DefaultMonolith.ini):
    ``content[0].text`` is a CONSTANT stub and the payload lives only in ``structuredContent``.
    """
    return {
        "result": {
            "content": [{"type": "text", "text": STRUCTURED_STUB_TEXT}],
            "isError": False,
            "structuredContent": data,
            "_meta": {"result_kind": "structured", "content_text_mode": "compact_status"},
        }
    }


def mcp_legacy_text_response(data: Dict[str, Any]) -> Dict[str, Any]:
    """Build the LEGACY (structured results disabled) envelope: payload serialized into
    ``content[0].text`` with no ``structuredContent``. Other checkouts can still run with the flag
    off, so every scorer must keep working against this shape."""
    return {
        "result": {
            "content": [{"type": "text", "text": json.dumps(data, ensure_ascii=False)}],
            "isError": False,
        }
    }


def mcp_error_response(message: str, data: Dict[str, Any] | None = None) -> Dict[str, Any]:
    """Build an MCP error envelope. Errors keep a REAL human message in content[0].text."""
    payload: Dict[str, Any] = {
        "content": [{"type": "text", "text": message}],
        "isError": True,
    }
    if data is not None:
        payload["structuredContent"] = data
    return {"result": payload}


def scripted_mcp_call(responses: List[Dict[str, Any]]):
    """Return an mcp_call stand-in that replays ``responses`` in order.

    Raises RuntimeError rather than StopIteration if a scorer calls more times than scripted, so an
    unexpected extra call surfaces as a loud failure instead of a silent generator stop.
    """
    queue = iter(responses)

    def call(*_args: Any, **_kwargs: Any) -> Dict[str, Any]:
        try:
            return next(queue)
        except StopIteration:
            raise RuntimeError("scripted_mcp_call exhausted: scorer made an unscripted MCP call")

    return call


def evidence_row(scored: Dict[str, Any], key: str) -> Dict[str, Any]:
    """First evidence row for ``key`` ('steps' / 'verify'), or {} when the scorer never got there.

    A scorer that breaks its chain early produces an EMPTY list; returning {} keeps the assertion a
    reported FAIL instead of an IndexError that aborts the whole run.
    """
    rows = scored.get("evidence", {}).get(key)
    return rows[0] if isinstance(rows, list) and rows and isinstance(rows[0], dict) else {}


def test_structured_results_token_scanning() -> None:
    """Regression guard for the structured-tool-results contract.

    With ``bEnableStructuredToolResults=True`` a SUCCESSFUL tools/call returns a constant stub in
    ``content[0].text`` and puts the real payload in ``structuredContent``. Every content-asserting
    scorer must therefore scan the CANONICAL payload. Each check below fails against a scorer that
    greps ``result_text()``: the ``contains`` checks score 0, and the ``not_contains`` checks pass
    vacuously (the stub can never hold a project token).
    """
    structured = mcp_success_response({"variables": [{"name": "BenchHealth", "type": "float"}]})
    check("structured success envelope carries only the stub in content[0].text",
          aeb.result_text(structured) == STRUCTURED_STUB_TEXT
          and "BenchHealth" not in aeb.result_text(structured),
          f"text={aeb.result_text(structured)!r}")
    scan = aeb.response_scan_text(structured)
    check("response_scan_text exposes the structured payload to token scans",
          "BenchHealth" in scan and STRUCTURED_STUB_TEXT in scan, f"scan={scan!r}")

    legacy = mcp_legacy_text_response({"variables": [{"name": "BenchHealth"}]})
    check("response_scan_text still scans the legacy text-JSON payload",
          "BenchHealth" in aeb.response_scan_text(legacy)
          and aeb.response_scan_text(legacy) == aeb.result_text(legacy),
          f"scan={aeb.response_scan_text(legacy)!r}")

    check("response_scan_text on a transport error is empty rather than raising",
          aeb.response_scan_text({"transport_error": True, "raw": "boom"}) == "")

    # _response_contains_any — gates graph_read / variable_read content_ok and the fixture preflight.
    check("_response_contains_any finds a token in the structured payload",
          aeb._response_contains_any(structured, ["BenchHealth"]))
    check("_response_contains_any finds a token in a legacy text payload",
          aeb._response_contains_any(legacy, ["BenchHealth"]))
    check("_response_contains_any still rejects an absent token",
          not aeb._response_contains_any(structured, ["MissingVar"]))

    # score_task (variable_read): fixture_vars + expected.contains.
    original_mcp_call = aeb.mcp_call
    try:
        aeb.mcp_call = scripted_mcp_call([structured])
        var_read = aeb.score_task(
            "http://offline.invalid/mcp",
            {
                "id": "TEST-VARREAD-STRUCTURED",
                "category": "variable_read",
                "tool": "blueprint_query",
                "action": "get_variables",
                "arguments": {"action": "get_variables", "asset_path": "/Game/Bench/BP_Test"},
                "expected": {"fixture_vars": ["BenchHealth"], "contains": ["BenchHealth"]},
            },
            timeout_s=0.1,
        )
    finally:
        aeb.mcp_call = original_mcp_call
    check("variable_read scores fixture_vars + expected.contains from structuredContent",
          var_read.get("direct_success") is True
          and var_read.get("evidence", {}).get("content_ok") is True
          and var_read.get("evidence", {}).get("content_check_applied") is True,
          f"score={var_read}")

    original_mcp_call = aeb.mcp_call
    try:
        aeb.mcp_call = scripted_mcp_call([mcp_success_response({"variables": []})])
        var_read_absent = aeb.score_task(
            "http://offline.invalid/mcp",
            {
                "id": "TEST-VARREAD-ABSENT",
                "category": "variable_read",
                "tool": "blueprint_query",
                "action": "get_variables",
                "arguments": {"action": "get_variables", "asset_path": "/Game/Bench/BP_Test"},
                "expected": {"fixture_vars": ["BenchHealth"]},
            },
            timeout_s=0.1,
        )
    finally:
        aeb.mcp_call = original_mcp_call
    check("variable_read still fails when the fixture variable is genuinely absent",
          var_read_absent.get("direct_success") is False,
          f"score={var_read_absent}")

    # _verify_readback — the mutation-observability gate for edit_execute / workflow_execute.
    original_mcp_call = aeb.mcp_call
    try:
        aeb.mcp_call = scripted_mcp_call([structured])
        readback_ok, readback_detail = aeb._verify_readback(
            "http://offline.invalid/mcp", "/Game/Bench/BP_Test",
            {"read_action": "get_variables", "contains": ["BenchHealth"],
             "not_contains": ["DeletedVar"]},
            timeout_s=0.1,
        )
    finally:
        aeb.mcp_call = original_mcp_call
    check("_verify_readback proves contains against structuredContent",
          readback_ok is True and readback_detail.get("contains", {}).get("ok") is True,
          f"detail={readback_detail}")

    original_mcp_call = aeb.mcp_call
    try:
        aeb.mcp_call = scripted_mcp_call([
            mcp_success_response({"variables": [{"name": "DeletedVar"}]}),
        ])
        readback_not_ok, readback_not_detail = aeb._verify_readback(
            "http://offline.invalid/mcp", "/Game/Bench/BP_Test",
            {"read_action": "get_variables", "not_contains": ["DeletedVar"]},
            timeout_s=0.1,
        )
    finally:
        aeb.mcp_call = original_mcp_call
    check("_verify_readback not_contains catches a token that survives only in structuredContent",
          readback_not_ok is False
          and readback_not_detail.get("not_contains", {}).get("ok") is False,
          f"detail={readback_not_detail}")

    # _score_asset_authoring_task — per-step and verify-loop contains / not_contains.
    original_mcp_call = aeb.mcp_call
    try:
        aeb.mcp_call = scripted_mcp_call([
            mcp_success_response({"asset_path": "/Game/Bench/DT_Bench", "created": True}),
            mcp_success_response({"rows": [{"name": "Row_A"}]}),
        ])
        authoring = aeb._score_asset_authoring_task(
            "http://offline.invalid/mcp",
            {
                "id": "TEST-AUTHORING-STRUCTURED",
                "name": "structured_contains_chain",
                "chain": [{
                    "tool": "data_query",
                    "args": {"action": "create_datatable"},
                    "contains": ["/Game/Bench/DT_Bench"],
                    "not_contains": ["Row_Removed"],
                }],
                "verify": [{
                    "tool": "data_query",
                    "args": {"action": "get_datatable_rows"},
                    "contains": ["Row_A"],
                    "not_contains": ["Row_Removed"],
                }],
            },
            timeout_s=0.1,
        )
    finally:
        aeb.mcp_call = original_mcp_call
    authoring_step = evidence_row(authoring, "steps")
    authoring_verify = evidence_row(authoring, "verify")
    check("asset-authoring step contains scores against structuredContent",
          authoring.get("direct_success") is True
          and authoring_step.get("contains_ok") is True
          and authoring_verify.get("contains_ok") is True,
          f"score={authoring}")

    original_mcp_call = aeb.mcp_call
    try:
        aeb.mcp_call = scripted_mcp_call([
            mcp_success_response({"asset_path": "/Game/Bench/DT_Bench"}),
            mcp_success_response({"rows": [{"name": "Row_Removed"}]}),
        ])
        authoring_leak = aeb._score_asset_authoring_task(
            "http://offline.invalid/mcp",
            {
                "id": "TEST-AUTHORING-NOTCONTAINS",
                "name": "structured_not_contains_chain",
                "chain": [{
                    "tool": "data_query",
                    "args": {"action": "remove_datatable_row"},
                    "contains": ["/Game/Bench/DT_Bench"],
                }],
                "verify": [{
                    "tool": "data_query",
                    "args": {"action": "get_datatable_rows"},
                    "not_contains": ["Row_Removed"],
                }],
            },
            timeout_s=0.1,
        )
    finally:
        aeb.mcp_call = original_mcp_call
    leak_verify = evidence_row(authoring_leak, "verify")
    check("asset-authoring verify not_contains catches a row that only structuredContent reveals",
          authoring_leak.get("direct_success") is False
          and leak_verify.get("not_contains_ok") is False,
          f"score={authoring_leak}")

    # negative_compile — the compiler diagnostic lives in structuredContent.errors[].
    compile_broken = mcp_success_response({
        "error_count": 1,
        "errors": ["Bench_Var is bad or unknown type (Structure)"],
        "status": "Error",
    })
    original_mcp_call = aeb.mcp_call
    try:
        aeb.mcp_call = scripted_mcp_call([compile_broken])
        negative = aeb._score_negative_compile(
            "http://offline.invalid/mcp",
            {
                "id": "TEST-NEGATIVE-COMPILE",
                "category": "negative_compile",
                "tool": "blueprint_query",
                "asset_path": "/Game/Bench/BP_Test",
                "expected": {"error_tokens": ["bad or unknown type"]},
            },
            timeout_s=0.1,
        )
    finally:
        aeb.mcp_call = original_mcp_call
    check("negative_compile matches error_tokens against structuredContent.errors",
          negative.get("direct_success") is True
          and negative.get("evidence", {}).get("token_ok") is True,
          f"score={negative}")


def test_compile_signal_requires_evidence() -> None:
    """A compile whose payload carries NO structured signal is unprovable and must FAIL.

    The old code fell back to scanning content[0].text ("0 error" in text or "error" not in text),
    which evaluates True against the structured stub — i.e. a compile with no evidence scored as a
    clean PASS. The inverse must not become a free negative_compile pass either.
    """
    clean_ok, clean_detail = aeb._compile_is_clean(
        mcp_success_response({"status": "UpToDate", "errors": [], "error_count": 0}))
    check("clean compile payload still scores clean",
          clean_ok is True and clean_detail.get("signal_present") is True, f"detail={clean_detail}")

    broken_ok, broken_detail = aeb._compile_is_clean(
        mcp_success_response({"error_count": 2, "errors": ["a", "b"], "status": "Error"}))
    check("failed compile payload still scores dirty",
          broken_ok is False and broken_detail.get("signal_present") is True,
          f"detail={broken_detail}")

    validate_ok, validate_detail = aeb._compile_is_clean(mcp_success_response({
        "unused_variables": ["Unused"], "disconnected_nodes": [], "node_errors": [],
        "unimplemented_interface_functions": [], "duplicate_custom_events": [],
    }))
    check("validate_blueprint lint report still scores clean on warnings only",
          validate_ok is True and validate_detail.get("signal_present") is True,
          f"detail={validate_detail}")

    silent_ok, silent_detail = aeb._compile_is_clean(mcp_success_response({"success": True}))
    check("compile with no structured signal FAILS instead of defaulting to clean",
          silent_ok is False
          and silent_detail.get("signal_present") is False
          and silent_detail.get("reason") == "no_structured_compile_signal",
          f"detail={silent_detail}")

    silent_err, silent_err_detail = aeb._compile_has_errors(mcp_success_response({"success": True}))
    check("missing compile signal is not evidence of a compile error either",
          silent_err is False
          and silent_err_detail.get("reason") == "no_structured_compile_signal",
          f"detail={silent_err_detail}")

    real_err, real_err_detail = aeb._compile_has_errors(
        mcp_success_response({"error_count": 1, "errors": ["boom"]}))
    check("_compile_has_errors still detects a real compile failure",
          real_err is True and real_err_detail.get("signal_present") is True,
          f"detail={real_err_detail}")

    iserror_err, _ = aeb._compile_has_errors(mcp_error_response("asset not found"))
    check("_compile_has_errors still rejects an isError envelope as a compile signal",
          iserror_err is False)


def test_error_text_assertions_still_use_human_text() -> None:
    """Errors keep a REAL message in content[0].text; error-path scoring must keep reading it."""
    err = mcp_error_response("Variable 'NoSuchVar' does not exist on /Game/Bench/BP_Test",
                             {"ok": False, "error_code": "not_found"})
    check("error envelopes still expose a human-readable message via result_text",
          "NoSuchVar" in aeb.result_text(err))
    original_mcp_call = aeb.mcp_call
    try:
        aeb.mcp_call = scripted_mcp_call([err])
        scored = aeb.score_task(
            "http://offline.invalid/mcp",
            {
                "id": "TEST-ERROR-PATH",
                "category": "error_path",
                "tool": "blueprint_query",
                "action": "get_variable_details",
                "arguments": {"action": "get_variable_details", "variable_name": "NoSuchVar"},
                "expected": {"specific_tokens": ["NoSuchVar"], "error_tokens": ["does not exist"]},
            },
            timeout_s=0.1,
        )
    finally:
        aeb.mcp_call = original_mcp_call
    check("error_path scoring still matches the offending identifier in the error message",
          scored.get("direct_success") is True, f"score={scored}")


def test_expect_error_requires_a_rejection() -> None:
    """`expect_error` must REQUIRE an error, where `allow_error` only tolerates one.

    Without this, a negative case cannot be expressed in an asset_authoring chain, so a guard
    regressing from "clean rejection" to "silent success" would still score as a pass. The
    live case is BEB-429: create_blueprint_prefab must reject rootless actors by name rather
    than crash the editor (UE 5.8 HarvestBlueprintFromActors null-derefs GetRootComponent()).
    """
    original = aeb.mcp_call
    task = {
        "id": "TEST-EXPECT-ERROR",
        "category": "asset_authoring",
        "chain": [
            {"tool": "level_instance_query", "expect_error": True,
             "args": {"action": "create_blueprint_prefab"},
             "contains": ["AE_Rootless", "root component"]},
        ],
        "verify": [
            {"tool": "asset_query", "expect_error": True,
             "args": {"action": "inspect_asset"}},
        ],
    }

    rejection = mcp_error_response(
        "Cannot harvest a Blueprint prefab from actor(s) with no root component: AE_Rootless. "
        "A prefab needs a scene root per actor.")
    asset_gone = mcp_error_response("Asset not found: /Game/Bench/BP_Rootless")

    try:
        aeb.mcp_call = scripted_mcp_call([rejection, asset_gone])  # type: ignore[assignment]
        scored = aeb.score_task("http://x", task, 5.0)
        check("expect_error passes when the server rejects as required",
              scored["direct_success"] is True, json.dumps(scored.get("evidence", {}))[:200])

        # A silent SUCCESS where a rejection was required must FAIL — this is exactly the
        # regression the guard exists to catch.
        aeb.mcp_call = scripted_mcp_call([
            mcp_success_response({"blueprint_path": "/Game/Bench/BP_Rootless",
                                  "source_actor_count": 2,
                                  "note": "AE_Rootless root component"}),
        ])  # type: ignore[assignment]
        scored_silent = aeb.score_task("http://x", task, 5.0)
        check("expect_error FAILS when the server silently succeeds instead of rejecting",
              scored_silent["direct_success"] is False,
              json.dumps(scored_silent.get("evidence", {}).get("steps", []))[:220])

        # A rejection whose message does not name the offending actor must still fail:
        # expect_error does not weaken the token assertions.
        aeb.mcp_call = scripted_mcp_call([mcp_error_response("Internal error")])  # type: ignore[assignment]
        scored_vague = aeb.score_task("http://x", task, 5.0)
        check("expect_error still requires the error to name the offending input",
              scored_vague["direct_success"] is False,
              json.dumps(scored_vague.get("evidence", {}).get("steps", []))[:220])

        # A verify step that expects absence must fail if the asset exists anyway — a guard
        # that half-wrote the asset is a silent partial success.
        aeb.mcp_call = scripted_mcp_call([
            rejection,
            mcp_success_response({"asset_path": "/Game/Bench/BP_Rootless", "class": "Blueprint"}),
        ])  # type: ignore[assignment]
        scored_leftover = aeb.score_task("http://x", task, 5.0)
        check("expect_error verify FAILS when the rejected asset exists anyway",
              scored_leftover["direct_success"] is False,
              json.dumps(scored_leftover.get("evidence", {}).get("verify", []))[:220])
    finally:
        aeb.mcp_call = original  # type: ignore[assignment]


def test_expected_rejection_has_non_success_lifecycle(tasks: List[Dict[str, Any]]) -> None:
    """Expected mutation rejection must never advertise a successful asset lifecycle."""
    guard = next(
        (task for task in tasks if task.get("workflow") == "level_instance_rootless_prefab_rejected"),
        None,
    )
    check("BEB rootless-prefab rejection guard exists", isinstance(guard, dict))
    if not isinstance(guard, dict):
        return

    capabilities = aeb.task_capabilities(guard)
    success_operations = {"creation_or_import", "edit", "save"}
    actual_operations = set(capabilities.get("asset_operations") or [])
    check("expected mutation rejection routes to rejection_guard semantics",
          capabilities.get("operation_semantics") == aeb.REJECTION_GUARD_SEMANTIC,
          f"capabilities={capabilities}")
    check("expected mutation rejection has no successful persistence lifecycle",
          capabilities.get("lifecycle_phase") == "not_applicable",
          f"capabilities={capabilities}")
    check("expected mutation rejection is excluded from successful asset-operation roles",
          not success_operations.intersection(actual_operations),
          f"asset_operations={sorted(actual_operations)}")
    check("expected mutation rejection retains absence read-back coverage",
          actual_operations == {"readback_verify"},
          f"asset_operations={sorted(actual_operations)}")
    check("rejection guard records the guarded mutation action",
          capabilities.get("expected_rejection_actions") == ["create_blueprint_prefab"],
          f"actions={capabilities.get('expected_rejection_actions')}")
    check("rejection guard does not claim asset creation/edit/save success flags",
          capabilities.get("creates_or_imports_assets") is False
          and capabilities.get("asset_edits") is False
          and capabilities.get("saves_assets") is False,
          f"capabilities={capabilities}")

    stale_guard = dict(guard)
    stale_guard["operation_semantics"] = "create_save"
    stale_guard["lifecycle_phase"] = "create_save"
    stale_guard["capabilities"] = {
        "creates_or_imports_assets": True,
        "saves_assets": True,
        "asset_edits": True,
        "asset_operations": ["creation_or_import", "edit", "save", "readback_verify"],
        "operation_semantics": "create_save",
        "lifecycle_phase": "create_save",
    }
    recovered = aeb.task_capabilities(stale_guard)
    check("inference overrides stale generated rejection lifecycle metadata",
          recovered.get("operation_semantics") == aeb.REJECTION_GUARD_SEMANTIC
          and recovered.get("lifecycle_phase") == "not_applicable"
          and not success_operations.intersection(recovered.get("asset_operations") or []),
          f"capabilities={recovered}")

    modules = aeb.load_testset_modules(aeb.DEFAULT_TESTSETS)
    guard_id = str(guard.get("id"))
    rejection_module = modules.get("operation_semantics.rejection_guard", {})
    lifecycle_module = modules.get("lifecycle.not_applicable", {})
    check("persisted testset exposes operation_semantics.rejection_guard",
          guard_id in set(rejection_module.get("task_ids") or []),
          f"guard={guard_id} task_ids={rejection_module.get('task_ids')}")
    check("persisted testset routes rejection guard to lifecycle.not_applicable",
          guard_id in set(lifecycle_module.get("task_ids") or []),
          f"guard={guard_id}")
    wrongly_routed = sorted(
        module_id
        for module_id, module in modules.items()
        if module_id.startswith((
            "asset_operation.creation_or_import",
            "asset_operation.edit",
            "asset_operation.save",
        ))
        and guard_id in set(module.get("task_ids") or [])
    )
    check("persisted testset excludes rejection guard from successful asset-operation routes",
          not wrongly_routed, f"modules={wrongly_routed[:8]}")


def test_structured_expect_scoring() -> None:
    exact_ok, exact_evidence = aeb._evaluate_structured_expect(
        mcp_success_response({"status": "updated", "saved": True, "nested": {"value": None}}),
        {"$.status": "updated", "$.saved": True, "$.nested.value": None},
    )
    check("structured expect accepts exact JSONPath values", exact_ok,
          f"evidence={exact_evidence}")

    bool_ok, bool_evidence = aeb._evaluate_structured_expect(
        mcp_success_response({"saved": 1}),
        {"$.saved": True},
    )
    check("structured expect keeps booleans distinct from numeric values", not bool_ok,
          f"evidence={bool_evidence}")

    nested_value = {
        "groups": [
            {"enabled": True, "weights": [1, 2.0, None]},
            {"enabled": False, "metadata": {"label": "second"}},
        ]
    }
    nested_ok, nested_evidence = aeb._evaluate_structured_expect(
        mcp_success_response({"settings": nested_value}),
        {"$.settings": nested_value},
    )
    check("structured expect recursively accepts exact nested dict/list values", nested_ok,
          f"evidence={nested_evidence}")

    nested_bool_ok, nested_bool_evidence = aeb._evaluate_structured_expect(
        mcp_success_response({"settings": {"groups": [{"enabled": 1}]}}),
        {"$.settings": {"groups": [{"enabled": True}]}},
    )
    check("structured expect keeps nested booleans distinct from numbers", not nested_bool_ok,
          f"evidence={nested_bool_evidence}")

    nested_object_shape_ok, nested_object_shape_evidence = aeb._evaluate_structured_expect(
        mcp_success_response({"settings": {"values": [1, 2], "extra": None}}),
        {"$.settings": {"values": [1, 2]}},
    )
    check("structured expect requires exact nested object keys", not nested_object_shape_ok,
          f"evidence={nested_object_shape_evidence}")

    nested_list_shape_ok, nested_list_shape_evidence = aeb._evaluate_structured_expect(
        mcp_success_response({"settings": {"values": [1, 2, 3]}}),
        {"$.settings": {"values": [1, 2]}},
    )
    check("structured expect requires exact nested list length", not nested_list_shape_ok,
          f"evidence={nested_list_shape_evidence}")

    present_null_ok, present_null_evidence = aeb._evaluate_structured_expect(
        mcp_success_response({"settings": {"optional": None}}),
        {"$.settings.optional": None},
    )
    missing_null_ok, missing_null_evidence = aeb._evaluate_structured_expect(
        mcp_success_response({"settings": {}}),
        {"$.settings.optional": None},
    )
    check("structured expect distinguishes present JSON null from a missing path",
          present_null_ok and not missing_null_ok
          and present_null_evidence[0].get("found") is True
          and missing_null_evidence[0].get("found") is False,
          f"present={present_null_evidence} missing={missing_null_evidence}")

    original_mcp_call = aeb.mcp_call
    try:
        aeb.mcp_call = lambda *_args, **_kwargs: mcp_success_response({"success": False})
        scored = aeb._score_asset_authoring_task(
            "http://offline.invalid/mcp",
            {
                "id": "TEST-EXPECT-CHAIN",
                "name": "structured_expect_false_payload",
                "chain": [{
                    "tool": "asset_query",
                    "args": {"action": "synthetic_action"},
                    "expect": {"$.success": True},
                }],
                "verify": [],
            },
            timeout_s=0.1,
        )
    finally:
        aeb.mcp_call = original_mcp_call
    chain_evidence = scored.get("evidence", {}).get("steps", [{}])[0]
    check("asset-authoring scorer rejects success=false in a transport-success response",
          scored.get("direct_success") is False
          and chain_evidence.get("transport_error") is False
          and chain_evidence.get("expect_ok") is False,
          f"score={scored}")

    responses = iter([
        mcp_success_response({"success": True}),
        mcp_success_response({"class": "Texture2D"}),
    ])
    original_mcp_call = aeb.mcp_call
    try:
        aeb.mcp_call = lambda *_args, **_kwargs: next(responses)
        verify_scored = aeb._score_asset_authoring_task(
            "http://offline.invalid/mcp",
            {
                "id": "TEST-EXPECT-VERIFY",
                "name": "structured_expect_verify",
                "chain": [{
                    "tool": "asset_query",
                    "args": {"action": "synthetic_create"},
                    "expect": {"$.success": True},
                }],
                "verify": [{
                    "tool": "asset_query",
                    "args": {"action": "synthetic_read"},
                    "expect": {"$.class": "PCGGraph"},
                }],
            },
            timeout_s=0.1,
        )
    finally:
        aeb.mcp_call = original_mcp_call
    verify_evidence = verify_scored.get("evidence", {}).get("verify", [{}])[0]
    check("asset-authoring verify steps enforce structured expect assertions",
          verify_scored.get("direct_success") is False
          and verify_evidence.get("expect_ok") is False,
          f"score={verify_scored}")

    original_mcp_call = aeb.mcp_call
    try:
        aeb.mcp_call = lambda *_args, **_kwargs: {
            "transport_error": True,
            "raw": "synthetic chain outage",
        }
        chain_transport_scored = aeb._score_asset_authoring_task(
            "http://offline.invalid/mcp",
            {
                "id": "TEST-TRANSPORT-CHAIN",
                "name": "transport_error_chain",
                "chain": [{
                    "tool": "asset_query",
                    "args": {"action": "synthetic_create"},
                }],
                "verify": [],
            },
            timeout_s=0.1,
        )
    finally:
        aeb.mcp_call = original_mcp_call
    check("asset-authoring scorer propagates chain transport diagnostics",
          chain_transport_scored.get("direct_success") is False
          and chain_transport_scored.get("transport_error") is True
          and chain_transport_scored.get("transport_error_raw") == "synthetic chain outage",
          f"score={chain_transport_scored}")

    responses = iter([
        mcp_success_response({"success": True}),
        {"transport_error": True, "raw": "synthetic verify outage"},
    ])
    original_mcp_call = aeb.mcp_call
    try:
        aeb.mcp_call = lambda *_args, **_kwargs: next(responses)
        verify_transport_scored = aeb._score_asset_authoring_task(
            "http://offline.invalid/mcp",
            {
                "id": "TEST-TRANSPORT-VERIFY",
                "name": "transport_error_verify",
                "chain": [{
                    "tool": "asset_query",
                    "args": {"action": "synthetic_create"},
                    "expect": {"$.success": True},
                }],
                "verify": [{
                    "tool": "asset_query",
                    "args": {"action": "synthetic_read"},
                }],
            },
            timeout_s=0.1,
        )
    finally:
        aeb.mcp_call = original_mcp_call
    check("asset-authoring scorer propagates verify transport diagnostics",
          verify_transport_scored.get("direct_success") is False
          and verify_transport_scored.get("transport_error") is True
          and verify_transport_scored.get("transport_error_raw") == "synthetic verify outage",
          f"score={verify_transport_scored}")


def test_engine_font_resolver_discovery() -> None:
    with tempfile.TemporaryDirectory(prefix="aeb-font-resolver-") as temp_dir:
        temp_root = pathlib.Path(temp_dir)
        project = temp_root / "Speed"
        project.mkdir()
        speed_uproject = project / "Speed.uproject"
        speed_uproject.write_text("{}\n", encoding="utf-8")
        (project / "Other.uproject").write_text("{}\n", encoding="utf-8")

        resolver = project / "Build" / "BatchFiles" / "Script" / "ResolveUnrealEngine.ps1"
        resolver.parent.mkdir(parents=True)
        resolver.write_text("# synthetic resolver\n", encoding="utf-8")

        engine_root = temp_root / "UE_5.8"
        font = engine_root / "Engine" / "Content" / "Slate" / "Fonts" / "Roboto-Regular.ttf"
        font.parent.mkdir(parents=True)
        font.write_bytes(b"synthetic-font")

        observed: Dict[str, Any] = {}
        original_project_root = aeb.project_root
        original_check_output = aeb.subprocess.check_output
        original_cache = aeb._ENGINE_ROBOTO_CACHE

        def fake_check_output(args: List[str], **kwargs: Any) -> str:
            observed["args"] = args
            observed["cwd"] = kwargs.get("cwd")
            return f"resolver diagnostic\n{engine_root}\n"

        try:
            aeb.project_root = lambda: project
            aeb.subprocess.check_output = fake_check_output
            aeb._ENGINE_ROBOTO_CACHE = None
            resolved = aeb.resolve_engine_roboto_regular_ttf()
        finally:
            aeb.project_root = original_project_root
            aeb.subprocess.check_output = original_check_output
            aeb._ENGINE_ROBOTO_CACHE = original_cache

        args = observed.get("args", [])
        check("font resolver discovers the project-name .uproject",
              "-Project" in args and args[args.index("-Project") + 1] == str(speed_uproject),
              f"args={args}")
        check("font resolver discovers the Build/BatchFiles helper",
              "-File" in args and args[args.index("-File") + 1] == str(resolver),
              f"args={args}")
        check("font resolver returns Roboto from the resolved engine root",
              resolved == font and observed.get("cwd") == str(project),
              f"resolved={resolved} cwd={observed.get('cwd')}")


def test_pcg_asset_authoring_contract(tasks: List[Dict[str, Any]]) -> None:
    matches = [
        task for task in tasks
        if task.get("workflow") == "pcg_graph_idempotent_authoring_roundtrip"
    ]
    check("PCG idempotent asset-authoring task exists", len(matches) == 1,
          f"matches={[task.get('id') for task in matches]}")
    if len(matches) != 1:
        return

    task = matches[0]
    actions = task_actions(task)
    required_actions = {
        "create_pcg_graph",
        "add_pcg_node",
        "set_pcg_node_params",
        "connect_pcg_nodes",
        "save_asset",
        "get_pcg_graph_info",
        "validate_pcg_graph",
    }
    check("PCG task covers the complete authoring/read-back contract",
          required_actions.issubset(actions),
          f"missing={sorted(required_actions - actions)}")
    check("PCG task targets the stable benchmark graph path",
          task.get("domain") == "pcg"
          and task.get("asset_path") == aeb.ASSET_AUTHORING_ROOT
          and all(
              step.get("args", {}).get("asset_path") == aeb.ASSET_AUTHORING_PCG_GRAPH
              for step in task.get("chain", []) + task.get("verify", [])
          ),
          f"task={task.get('id')}")
    check("PCG task is rerunnable without delete or reset actions",
          not actions.intersection({"delete_asset", "delete_assets", "remove_pcg_node", "disconnect_pcg_nodes"}),
          f"actions={sorted(actions)}")
    save_reload_steps = [
        step for step in task.get("chain", [])
        if step.get("args", {}).get("action") == "save_asset"
    ]
    check("PCG task proves persistence through generic asset save/reload",
          len(save_reload_steps) == 1
          and save_reload_steps[0].get("args", {}).get("verify_reload") is True
          and save_reload_steps[0].get("expect", {}).get("$.reloaded") is True,
          f"save_reload_steps={save_reload_steps}")

    chain = [step for step in task.get("chain", []) if isinstance(step, dict)]
    create_steps = [step for step in chain if step.get("args", {}).get("action") == "create_pcg_graph"]
    add_steps = [step for step in chain if step.get("args", {}).get("action") == "add_pcg_node"]
    connect_steps = [step for step in chain if step.get("args", {}).get("action") == "connect_pcg_nodes"]
    set_steps = [step for step in chain if step.get("args", {}).get("action") == "set_pcg_node_params"]
    check("PCG create/add steps use return_existing policy",
          bool(create_steps) and bool(add_steps)
          and all(step.get("args", {}).get("existing_policy") == "return_existing"
                  for step in create_steps + add_steps))
    check("PCG Add Tags node uses a stable authored title and native setting",
          all(step.get("args", {}).get("node_title") == "Bench_AddTag" for step in add_steps)
          and len(set_steps) == 1
          and set_steps[0].get("args", {}).get("properties") == {"TagsToAdd": "Monolith.Benchmark"})

    edge_keys = [
        (
            step.get("args", {}).get("source_node"),
            step.get("args", {}).get("source_pin"),
            step.get("args", {}).get("target_node"),
            step.get("args", {}).get("target_pin"),
        )
        for step in connect_steps
    ]
    edge_counts = collections.Counter(edge_keys)
    check("PCG connections are repeated to prove idempotent edge authoring",
          len(edge_counts) == 2 and all(count == 2 for count in edge_counts.values()),
          f"edges={edge_counts}")

    exact_paths = {
        path
        for step in chain + [step for step in task.get("verify", []) if isinstance(step, dict)]
        for path in (step.get("expect") or {})
    }
    check("PCG task exact expectations cover status/saved/valid/class",
          {"$.status", "$.saved", "$.valid", "$.class",
           "$.nodes[2].settings.TagsToAdd"}.issubset(exact_paths),
          f"paths={sorted(exact_paths)}")

    selected, selection = aeb.select_tasks(
        aeb.DEFAULT_TASKS,
        aeb.DEFAULT_TESTSETS,
        module_ids=["asset_authoring.pcg.graph_authoring"],
    )
    check("PCG asset-authoring module selects exactly the PCG task",
          [row.get("id") for row in selected] == [task.get("id")]
          and selection.get("selection_filters", {}).get("module_ids")
          == ["asset_authoring.pcg.graph_authoring"],
          f"selected={[row.get('id') for row in selected]} selection={selection}")

    expected_workflows = {
        "pcg_graph_idempotent_authoring_roundtrip",
        "pcg_graph_mutation_cleanup_roundtrip",
        "pcg_graph_user_parameter_roundtrip",
        "pcg_subgraph_assignment_roundtrip",
        "pcg_graph_replacement_roundtrip",
        "pcg_surface_discovery_roundtrip",
    }
    pcg_tasks = [row for row in tasks if row.get("domain") == "pcg"]
    pcg_workflow_counts = collections.Counter(row.get("workflow") for row in pcg_tasks)
    pcg_by_workflow = {row.get("workflow"): row for row in pcg_tasks}
    check("PCG asset-authoring suite contains every executable graph workflow once",
          expected_workflows.issubset(pcg_by_workflow)
          and all(pcg_workflow_counts[workflow] == 1 for workflow in expected_workflows),
          f"counts={dict(sorted(pcg_workflow_counts.items(), key=lambda item: str(item[0])))}")

    suite_actions: Set[str] = set()
    for pcg_task in pcg_tasks:
        suite_actions.update(task_actions(pcg_task))
    expanded_actions = {
        "get_status",
        "list_graph_assets",
        "get_graph_asset",
        "list_pcg_node_types",
        "disconnect_pcg_nodes",
        "remove_pcg_node",
        "set_pcg_graph_user_parameters",
        "set_pcg_subgraph",
        "replace_pcg_graph_contents",
    }
    check("PCG suite executes the expanded mutation and discovery surface",
          expanded_actions.issubset(suite_actions),
          f"missing={sorted(expanded_actions - suite_actions)}")

    expected_scratch_paths = {
        "pcg_graph_idempotent_authoring_roundtrip": {
            aeb.ASSET_AUTHORING_PCG_GRAPH,
        },
        "pcg_graph_mutation_cleanup_roundtrip": {
            aeb.ASSET_AUTHORING_PCG_MUTATION_GRAPH,
        },
        "pcg_graph_user_parameter_roundtrip": {
            aeb.ASSET_AUTHORING_PCG_USER_PARAMS_GRAPH,
        },
        "pcg_subgraph_assignment_roundtrip": {
            aeb.ASSET_AUTHORING_PCG_SUBGRAPH_PARENT,
            aeb.ASSET_AUTHORING_PCG_SUBGRAPH_CHILD_A,
            aeb.ASSET_AUTHORING_PCG_SUBGRAPH_CHILD_B,
        },
        "pcg_graph_replacement_roundtrip": {
            aeb.ASSET_AUTHORING_PCG_REPLACE_SOURCE,
            aeb.ASSET_AUTHORING_PCG_REPLACE_BASELINE,
            aeb.ASSET_AUTHORING_PCG_REPLACE_TARGET,
        },
        "pcg_surface_discovery_roundtrip": {
            aeb.ASSET_AUTHORING_PCG_DISCOVERY_GRAPH,
        },
    }
    observed_scratch_paths: Dict[str, Set[str]] = {}
    for workflow in expected_workflows:
        pcg_task = pcg_by_workflow.get(workflow, {})
        paths: Set[str] = set()
        for step in pcg_task.get("chain", []) + pcg_task.get("verify", []):
            args = step.get("args", {}) if isinstance(step, dict) else {}
            for field in ("asset_path", "source_asset_path", "target_asset_path", "subgraph_asset_path"):
                path = args.get(field)
                if isinstance(path, str) and path.startswith(aeb.ASSET_AUTHORING_PCG_ROOT):
                    paths.add(path.split(".", 1)[0])
        observed_scratch_paths[workflow] = paths
    check("PCG workflows own exact non-overlapping scratch graph sets",
          observed_scratch_paths == expected_scratch_paths
          and sum(len(paths) for paths in observed_scratch_paths.values())
          == len(set().union(*observed_scratch_paths.values())),
          f"observed={observed_scratch_paths}")

    for workflow in sorted(expected_workflows - {"pcg_graph_idempotent_authoring_roundtrip"}):
        pcg_task = pcg_by_workflow.get(workflow, {})
        save_reload_steps = [
            step for step in pcg_task.get("chain", [])
            if isinstance(step, dict)
            and step.get("args", {}).get("action") == "save_asset"
            and step.get("args", {}).get("verify_reload") is True
            and step.get("expect", {}).get("$.reloaded") is True
        ]
        check(f"PCG {workflow} crosses a verified package reload boundary",
              bool(save_reload_steps),
              f"save_reload_steps={save_reload_steps}")

    root_selected, root_selection = aeb.select_tasks(
        aeb.DEFAULT_TASKS,
        aeb.DEFAULT_TESTSETS,
        module_ids=["asset_authoring.pcg"],
    )
    check("PCG root module selects every generated PCG task",
          [row.get("id") for row in root_selected] == [row.get("id") for row in pcg_tasks]
          and root_selection.get("selection_filters", {}).get("module_ids")
          == ["asset_authoring.pcg"],
          f"selected={[row.get('id') for row in root_selected]} "
          f"expected={[row.get('id') for row in pcg_tasks]} selection={root_selection}")

    for workflow in sorted(expected_workflows):
        pcg_task = pcg_by_workflow.get(workflow)
        if not pcg_task:
            continue
        module_id = f"asset_authoring.pcg.{pcg_task.get('edit_domain')}"
        leaf_selected, leaf_selection = aeb.select_tasks(
            aeb.DEFAULT_TASKS,
            aeb.DEFAULT_TESTSETS,
            module_ids=[module_id],
        )
        check(f"PCG leaf module {module_id} selects only {workflow}",
              [row.get("id") for row in leaf_selected] == [pcg_task.get("id")]
              and leaf_selection.get("selection_filters", {}).get("module_ids") == [module_id],
              f"selected={[row.get('id') for row in leaf_selected]} selection={leaf_selection}")


def test_ui_high_usage_asset_authoring_contract(tasks: List[Dict[str, Any]]) -> None:
    expected_actions_by_workflow = {
        "ui_registry_layout_accessibility_roundtrip": {
            "list_widget_types",
            "dump_property_allowlist",
            "describe_widget_type_schema",
            "list_widget_property_enums",
            "list_widget_events",
            "get_widget_bindings",
            "audit_accessibility",
            "audit_widget_layout",
            "measure_widget_layout",
            "compute_widget_uispec_fingerprint",
        },
        "ui_commonui_content_framework_roundtrip": {
            "configure_numeric_text",
            "configure_rotator",
            "configure_animated_switcher",
            "setup_common_list_view",
            "create_widget_carousel",
            "create_hardware_visibility_border",
            "create_lazy_image",
            "create_load_guard",
            "get_common_framework_status",
            "describe_common_widget_blueprint",
            "describe_common_messaging_flow",
            "validate_common_dialog_contract",
            "list_platform_input_tables",
        },
        "ui_uispec_diff_patch_roundtrip": {
            "build_ui_from_spec",
            "apply_ui_spec_patch",
            "dump_ui_spec_schema",
            "dump_ui_spec",
            "diff_ui_spec",
            "compute_widget_uispec_fingerprint",
        },
        "ui_animation_inspection_delta_binding_roundtrip": {
            "create_animation_v2",
            "apply_animation_delta",
            "remap_animation_binding",
            "get_animation_overview",
            "get_animation_timeline",
            "get_animation_time_slice",
            "get_animation_details",
        },
    }
    ui_tasks = [row for row in tasks if row.get("domain") == "ui"]
    workflow_counts = collections.Counter(row.get("workflow") for row in ui_tasks)
    ui_by_workflow = {row.get("workflow"): row for row in ui_tasks}
    check(
        "UI suite contains every high-usage UMG/CommonUI workflow exactly once",
        set(expected_actions_by_workflow).issubset(ui_by_workflow)
        and all(workflow_counts[workflow] == 1 for workflow in expected_actions_by_workflow),
        f"counts={dict(sorted(workflow_counts.items(), key=lambda item: str(item[0])))}",
    )

    for workflow, required_actions in expected_actions_by_workflow.items():
        task = ui_by_workflow.get(workflow, {})
        actions = task_actions(task)
        check(
            f"UI workflow {workflow} executes its required action contract",
            required_actions.issubset(actions),
            f"missing={sorted(required_actions - actions)}",
        )
        check(
            f"UI workflow {workflow} records source and local skill references",
            any(
                isinstance(ref, str) and ref.startswith("source:")
                for ref in task.get("reference_context", [])
            )
            and any(
                isinstance(ref, str)
                and ref.startswith("skill_ref: Plugins/Monolith/Skills/unreal-ui/")
                for ref in task.get("reference_context", [])
            ),
            f"reference_context={task.get('reference_context')}",
        )

        save_reload_steps = [
            step
            for step in task.get("chain", [])
            if isinstance(step, dict)
            and step.get("args", {}).get("action") == "save_asset"
            and step.get("args", {}).get("verify_reload") is True
            and step.get("expect", {}).get("$.reloaded") is True
        ]
        check(
            f"UI workflow {workflow} crosses a verified package reload boundary",
            bool(save_reload_steps),
            f"save_reload_steps={save_reload_steps}",
        )

        module_id = f"asset_authoring.ui.{task.get('edit_domain')}"
        leaf_selected, leaf_selection = aeb.select_tasks(
            aeb.DEFAULT_TASKS,
            aeb.DEFAULT_TESTSETS,
            module_ids=[module_id],
        )
        check(
            f"UI leaf module {module_id} selects only {workflow}",
            [row.get("id") for row in leaf_selected] == [task.get("id")]
            and leaf_selection.get("selection_filters", {}).get("module_ids") == [module_id],
            f"selected={[row.get('id') for row in leaf_selected]} selection={leaf_selection}",
        )

    expected_scratch_paths = {
        "ui_registry_layout_accessibility_roundtrip": {
            aeb.ASSET_AUTHORING_UI_DISCOVERY,
        },
        "ui_commonui_content_framework_roundtrip": {
            aeb.ASSET_AUTHORING_UI_COMMON_CONTENT,
            aeb.ASSET_AUTHORING_UI_COMMON_ENTRY,
        },
        "ui_uispec_diff_patch_roundtrip": {
            aeb.ASSET_AUTHORING_UI_SPEC_PATCH,
        },
        "ui_animation_inspection_delta_binding_roundtrip": {
            aeb.ASSET_AUTHORING_UI_ANIMATION,
        },
    }
    observed_scratch_paths: Dict[str, Set[str]] = {}
    path_fields = ("asset_path", "save_path", "wbp_path")
    for workflow in expected_actions_by_workflow:
        task = ui_by_workflow.get(workflow, {})
        paths: Set[str] = set()
        for step in task.get("chain", []) + task.get("verify", []):
            args = step.get("args", {}) if isinstance(step, dict) else {}
            for field in path_fields:
                path = args.get(field)
                if isinstance(path, str) and path.startswith(aeb.ASSET_AUTHORING_UI_ROOT):
                    paths.add(path.split(".", 1)[0])
            for path in args.get("asset_paths", []):
                if isinstance(path, str) and path.startswith(aeb.ASSET_AUTHORING_UI_ROOT):
                    paths.add(path.split(".", 1)[0])
            entry_class = args.get("entry_class")
            if isinstance(entry_class, str) and entry_class.startswith(aeb.ASSET_AUTHORING_UI_ROOT):
                paths.add(entry_class.split(".", 1)[0])
        observed_scratch_paths[workflow] = paths
    check(
        "UI high-usage workflows own exact non-overlapping scratch asset sets",
        observed_scratch_paths == expected_scratch_paths
        and sum(len(paths) for paths in observed_scratch_paths.values())
        == len(set().union(*observed_scratch_paths.values())),
        f"observed={observed_scratch_paths}",
    )

    discovery_task = ui_by_workflow.get("ui_registry_layout_accessibility_roundtrip", {})
    registry_steps = [
        step
        for step in discovery_task.get("verify", [])
        if step.get("args", {}).get("action") == "list_widget_types"
    ]
    check(
        "UI discovery benchmark proves reflection-backed CommonUI type filtering",
        len(registry_steps) == 1
        and registry_steps[0].get("args", {}).get("module_filter") == "CommonUI"
        and registry_steps[0].get("args", {}).get("filter") == "display"
        and "CommonNumericTextBlock" in registry_steps[0].get("contains", [])
        and "total_registered" in registry_steps[0].get("contains", []),
        f"steps={registry_steps}",
    )

    guarded_workflows = {
        "ui_uispec_diff_patch_roundtrip": "apply_ui_spec_patch",
        "ui_animation_inspection_delta_binding_roundtrip": "apply_animation_delta",
    }
    for workflow, action in guarded_workflows.items():
        task = ui_by_workflow.get(workflow, {})
        action_steps = [
            step for step in task.get("chain", [])
            if step.get("args", {}).get("action") == action
        ]
        modes = {
            (
                step.get("args", {}).get("dry_run"),
                step.get("args", {}).get("confirm"),
            )
            for step in action_steps
        }
        check(
            f"UI {action} benchmark covers dry-run and explicitly confirmed apply",
            modes == {(True, False), (False, True)},
            f"modes={modes}",
        )

    animation_task = ui_by_workflow.get(
        "ui_animation_inspection_delta_binding_roundtrip", {}
    )
    remap_steps = [
        step
        for step in animation_task.get("chain", [])
        if step.get("args", {}).get("action") == "remap_animation_binding"
    ]
    remap_modes = {
        (
            step.get("args", {}).get("dry_run"),
            step.get("args", {}).get("confirm"),
        )
        for step in remap_steps
    }
    check(
        "UI animation binding remap covers dry-run and explicitly confirmed apply",
        remap_modes == {(True, False), (False, True)}
        and all(
            step.get("args", {}).get("from_widget_name") == "BenchAnimSource"
            and step.get("args", {}).get("to_widget_name") == "BenchAnimTarget"
            for step in remap_steps
        ),
        f"steps={remap_steps}",
    )

    common_task = ui_by_workflow.get("ui_commonui_content_framework_roundtrip", {})
    advisory_validators = {
        step.get("args", {}).get("action"): step
        for step in common_task.get("verify", [])
        if step.get("args", {}).get("action")
        in {"describe_common_messaging_flow", "validate_common_dialog_contract"}
    }
    check(
        "project-dependent Common framework validators are explicit advisory evidence",
        set(advisory_validators)
        == {"describe_common_messaging_flow", "validate_common_dialog_contract"}
        and all(step.get("server_ok_only") is True for step in advisory_validators.values())
        and all(
            isinstance(step.get("server_ok_reason"), str)
            and len(step.get("server_ok_reason", "").strip()) >= 20
            for step in advisory_validators.values()
        ),
        f"validators={advisory_validators}",
    )

    new_suite_actions: Set[str] = set()
    for workflow in expected_actions_by_workflow:
        new_suite_actions.update(task_actions(ui_by_workflow.get(workflow, {})))
    runtime_only_actions = {
        "set_colorblind_mode",
        "set_text_scale",
        "register_tab",
        "create_button_group",
        "dump_action_router_state",
    }
    check(
        "asset-authoring UI expansion excludes runtime-only PIE actions",
        not new_suite_actions.intersection(runtime_only_actions),
        f"unexpected={sorted(new_suite_actions.intersection(runtime_only_actions))}",
    )


def test_manifest_matches_tasks(tasks: List[Dict[str, Any]], manifest: Dict[str, Any]) -> None:
    check("task_count matches JSONL rows", manifest.get("task_count") == len(tasks),
          f"manifest={manifest.get('task_count')} rows={len(tasks)}")
    check("weights match runner", manifest.get("weights") == aeb.WEIGHTS)
    check("score_formula matches runner", manifest.get("score_formula") == aeb.score_formula_string())
    check("score_dimensions match runner",
          manifest.get("score_dimensions") == aeb.SCORE_DIMENSIONS,
          f"manifest={manifest.get('score_dimensions')} runner={aeb.SCORE_DIMENSIONS}")

    category_counts = count_rows_by_field(tasks, "category")
    check("all scored categories are present",
          set(aeb.WEIGHTS).issubset(category_counts),
          f"missing={sorted(set(aeb.WEIGHTS) - set(category_counts))}")
    for category in sorted(aeb.WEIGHTS):
        check(f"manifest category count matches {category}",
              manifest.get("category_counts", {}).get(category) == category_counts.get(category),
              f"manifest={manifest.get('category_counts', {}).get(category)} rows={category_counts.get(category)}")


def test_root_readme_generated_summary(tasks: List[Dict[str, Any]], manifest: Dict[str, Any]) -> None:
    asset_index = load_json(pathlib.Path(str(manifest.get("asset_type_index", ""))))
    testset_index = load_json(pathlib.Path(str(manifest.get("testset_index", ""))))
    counts = aeb.asset_editing_global_counts(tasks, asset_index, testset_index)
    readme_path = aeb.resolve_plugin_path(aeb.DEFAULT_TASKS).parent / "README.md"
    readme_text = readme_path.read_text(encoding="utf-8") if readme_path.exists() else ""
    expected_summary = aeb.render_root_readme_summary(counts)

    check("root README has one generated corpus summary marker pair",
          readme_text.count(aeb.ROOT_README_SUMMARY_START) == 1
          and readme_text.count(aeb.ROOT_README_SUMMARY_END) == 1,
          str(readme_path))
    check("root README global corpus counts match generated artifacts",
          expected_summary in readme_text,
          f"expected={expected_summary!r}")
    expected_task_file_row = (
        f"| `tasks.jsonl` | {counts['canonical_tasks']} benchmark tasks across "
        f"{len(aeb.WEIGHTS)} categories |"
    )
    check("root README file table matches canonical task and category counts",
          expected_task_file_row in readme_text,
          f"expected={expected_task_file_row!r}")
    expected_asset_authoring_row = (
        f"| `asset_authoring` | {counts['asset_authoring_tasks']} | mixed owner namespaces |"
    )
    check("root README category table matches generated asset-authoring count",
          expected_asset_authoring_row in readme_text,
          f"expected_prefix={expected_asset_authoring_row!r}")
    check("manifest global corpus counts match README source artifacts",
          manifest.get("task_count") == counts["canonical_tasks"]
          and manifest.get("asset_authoring_tasks") == counts["asset_authoring_tasks"]
          and manifest.get("testset_module_count") == counts["testset_modules"]
          and manifest.get("testset_module_shard_count") == counts["module_shards"],
          f"manifest={{'task_count': {manifest.get('task_count')}, "
          f"'asset_authoring_tasks': {manifest.get('asset_authoring_tasks')}, "
          f"'testset_module_count': {manifest.get('testset_module_count')}, "
          f"'testset_module_shard_count': {manifest.get('testset_module_shard_count')}}} "
          f"counts={counts}")


def test_task_shape(tasks: List[Dict[str, Any]]) -> None:
    seen_ids: Set[str] = set()
    duplicate_ids: List[str] = []
    missing_fields: List[str] = []
    bad_shapes: List[str] = []
    bad_readbacks: List[str] = []

    for index, task in enumerate(tasks, 1):
        task_id = str(task.get("id", "")) or f"row {index}"
        if task_id in seen_ids:
            duplicate_ids.append(task_id)
        seen_ids.add(task_id)

        for field in ("id", "category", "namespace", "action", "tool", "expected", "safety"):
            if field not in task:
                missing_fields.append(f"{task_id}:{field}")

        category = str(task.get("category", ""))
        if category not in aeb.WEIGHTS:
            bad_shapes.append(f"{task_id}:unknown category {category}")
        if not isinstance(task.get("expected"), dict):
            bad_shapes.append(f"{task_id}:expected")
        if "arguments" in task and not isinstance(task.get("arguments"), dict):
            bad_shapes.append(f"{task_id}:arguments")
        if "chain" in task and not isinstance(task.get("chain"), list):
            bad_shapes.append(f"{task_id}:chain")
        if category in READBACK_REQUIRED_CATEGORIES and not has_readback(task):
            bad_readbacks.append(task_id)

    check("all tasks have unique ids", not duplicate_ids, f"duplicates={duplicate_ids[:8]}")
    check("all tasks have required fields", not missing_fields, f"missing={missing_fields[:8]}")
    check("all task payload fields have valid shape", not bad_shapes, f"bad={bad_shapes[:8]}")
    check("mutating workflow categories declare read-back", not bad_readbacks,
          f"missing={bad_readbacks[:8]}")


def test_high_error_recovery_coverage(tasks: List[Dict[str, Any]]) -> None:
    action_categories: Dict[str, Set[str]] = collections.defaultdict(set)
    action_ids: Dict[str, List[str]] = collections.defaultdict(list)
    for task in tasks:
        category = str(task.get("category", ""))
        task_id = str(task.get("id", ""))
        for action in task_actions(task):
            if action in REQUIRED_BLUEPRINT_RECOVERY_ACTIONS:
                action_categories[action].add(category)
                action_ids[action].append(task_id)

    missing = sorted(REQUIRED_BLUEPRINT_RECOVERY_ACTIONS - set(action_categories))
    check("all P0.4 high-error Blueprint actions are covered", not missing, f"missing={missing}")

    weak = []
    for action in sorted(REQUIRED_BLUEPRINT_RECOVERY_ACTIONS):
        categories = action_categories.get(action, set())
        if not categories.intersection({"edit_execute", "workflow_execute", "asset_authoring", "error_path", "duplicate_reject"}):
            weak.append(f"{action}:{sorted(categories)}")
    check("P0.4 actions have executable or recovery-path coverage", not weak, f"weak={weak}")
    for action in sorted(REQUIRED_BLUEPRINT_RECOVERY_ACTIONS):
        check(f"{action} has at least one task id", bool(action_ids.get(action)),
              f"ids={action_ids.get(action, [])[:5]}")


def test_asset_type_and_testset_indexes(tasks: List[Dict[str, Any]], manifest: Dict[str, Any]) -> None:
    asset_index_path = aeb.resolve_plugin_path(pathlib.Path(manifest.get("asset_type_index", "")))
    testset_index_path = aeb.resolve_plugin_path(pathlib.Path(manifest.get("testset_index", "")))
    testset_modules_path = aeb.resolve_plugin_path(pathlib.Path(manifest.get("testset_modules", "")))

    check("asset type index exists", asset_index_path.exists(), str(asset_index_path))
    check("testset index exists", testset_index_path.exists(), str(testset_index_path))
    check("testset modules manifest exists", testset_modules_path.exists(), str(testset_modules_path))

    if asset_index_path.exists():
        asset_index = load_json(asset_index_path)
        asset_types = asset_index.get("asset_types", [])
        check("asset_type_count matches index",
              manifest.get("asset_type_count") == len(asset_types),
              f"manifest={manifest.get('asset_type_count')} index={len(asset_types)}")
        root_readme = asset_index_path.parent / "README.md"
        root_readme_text = root_readme.read_text(encoding="utf-8") if root_readme.exists() else ""
        stale_root_rows: List[str] = []
        repeated_asset_type_cases: List[str] = []
        for item in asset_types:
            asset_type_name = str(item.get("asset_type") or "")
            operation_counts = item.get("operation_counts") if isinstance(item.get("operation_counts"), dict) else {}
            expected_root_row = (
                f"| `{asset_type_name}` | {int(item.get('task_count', 0))} | "
                f"{int(item.get('edit_domain_count', 0))} | "
                f"{int(operation_counts.get('creation_or_import', 0))} | "
                f"{int(operation_counts.get('edit', 0))} | "
                f"{int(operation_counts.get('save', 0))} | "
                f"{int(operation_counts.get('readback_verify', 0))} | "
                f"`{aeb.markdown_cell(str(item.get('directory') or ''))}` |"
            )
            if expected_root_row not in root_readme_text:
                stale_root_rows.append(expected_root_row)
            task_file = aeb.resolve_plugin_path(pathlib.Path(str(item.get("tasks_file", ""))))
            rows = aeb.load_jsonl(task_file) if task_file.exists() else []
            check(f"asset type task count matches {item.get('asset_type')}",
                  task_file.exists() and item.get("task_count") == len(rows),
                  f"file={task_file} count={item.get('task_count')} rows={len(rows)}")
            type_index_file = aeb.resolve_plugin_path(pathlib.Path(str(item.get("index_file", ""))))
            if type_index_file.exists():
                type_index = load_json(type_index_file)
                domain_slug = aeb.slugify_route(type_index.get("asset_type"))
                for case in type_index.get("testcases", []):
                    route_slug = aeb.slugify_route(case.get("route_edit_domain"))
                    if route_slug == domain_slug or route_slug.startswith(f"{domain_slug}_"):
                        repeated_asset_type_cases.append(
                            f"{type_index.get('asset_type')}:{case.get('route_edit_domain')}"
                        )
        check("asset type testcase route names do not repeat the AssetType token",
              not repeated_asset_type_cases,
              f"repeated={repeated_asset_type_cases[:8]}")
        check("root README AssetType support table matches generated index",
              root_readme.exists() and not stale_root_rows,
              f"missing_or_stale={stale_root_rows[:3]}")

    if testset_index_path.exists() and testset_modules_path.exists():
        testset_index = load_json(testset_index_path)
        modules_payload = load_json(testset_modules_path)
        module_refs = modules_payload.get("module_refs", [])
        module_lookup = modules_payload.get("module_lookup", {})
        all_task_ids = {str(task.get("id")) for task in tasks}
        check("testset task_count matches tasks",
              testset_index.get("task_count") == len(tasks),
              f"index={testset_index.get('task_count')} rows={len(tasks)}")
        check("testset module_count matches modules",
              testset_index.get("module_count") == modules_payload.get("module_count") == len(module_refs) == len(module_lookup),
              f"index={testset_index.get('module_count')} manifest={modules_payload.get('module_count')} refs={len(module_refs)} lookup={len(module_lookup)}")
        missing_shards = []
        bad_shards: List[str] = []
        bad_shard_links: List[str] = []
        for shard_info in modules_payload.get("module_shards", {}).values():
            shard = pathlib.Path(str(shard_info.get("file", ""))) if isinstance(shard_info, dict) else pathlib.Path(str(shard_info))
            shard = aeb.resolve_plugin_path(shard)
            if not shard.exists():
                missing_shards.append(str(shard))
                continue
            shard_payload = load_json(shard)
            shard_ids = [str(module_id) for module_id in (shard_info.get("module_ids", []) if isinstance(shard_info, dict) else [])]
            shard_refs = shard_payload.get("module_refs", [])
            shard_modules = shard_payload.get("modules", {})
            if (
                shard_payload.get("module_count") != len(shard_refs)
                or len(shard_refs) != len(shard_modules)
                or (shard_ids and sorted(shard_ids) != sorted(str(ref.get("module_id")) for ref in shard_refs))
                or sorted(str(module_id) for module_id in shard_modules) != sorted(str(ref.get("module_id")) for ref in shard_refs)
            ):
                bad_shards.append(str(shard))
            for module_id, module in shard_modules.items():
                if str(module_id) not in module_lookup:
                    bad_shard_links.append(f"{module_id}:missing_lookup")
                task_ids = {str(task_id) for task_id in module.get("task_ids", [])}
                if not task_ids.issubset(all_task_ids):
                    bad_shard_links.append(f"{module_id}:unknown_task")
                parent_id = str(module.get("parent_module_id") or "")
                if parent_id and parent_id not in module_lookup:
                    bad_shard_links.append(f"{module_id}:parent={parent_id}")
                for child_id in module.get("child_module_ids", []):
                    if str(child_id) not in module_lookup:
                        bad_shard_links.append(f"{module_id}:child={child_id}")
        check("all split testset module shards exist", not missing_shards,
              f"missing={missing_shards[:5]}")
        check("all split testset module shards have internally matching refs/modules",
              not bad_shards,
              f"bad={bad_shards[:5]}")
        check("all split testset module shard links resolve",
              not bad_shard_links,
              f"bad={bad_shard_links[:8]}")

        repeated_leaf_modules: List[str] = []
        for module_id in module_lookup:
            parts = [part for part in str(module_id).split(".") if part]
            if len(parts) >= 3 and parts[0] == "asset_authoring":
                domain_slug, edit_slug = parts[1], parts[2]
            elif len(parts) >= 4 and parts[0] == "asset_operation":
                domain_slug, edit_slug = parts[2], parts[3]
            else:
                continue
            if edit_slug == domain_slug or edit_slug.startswith(f"{domain_slug}_"):
                repeated_leaf_modules.append(str(module_id))
        check("asset route module ids do not repeat the AssetType token",
              not repeated_leaf_modules,
              f"repeated={repeated_leaf_modules[:8]}")
        repeated_tree_keys: List[str] = []

        def collect_repeated_edit_domain_tree_keys(payload: Dict[str, Any], label: str) -> None:
            tree = payload.get("tree")
            if not isinstance(tree, dict):
                return
            asset_authoring = tree.get("asset_authoring")
            if isinstance(asset_authoring, dict):
                domains = asset_authoring.get("domains")
                if isinstance(domains, dict):
                    for domain, domain_payload in domains.items():
                        domain_slug = aeb.slugify_route(domain)
                        edit_domains = domain_payload.get("edit_domains") if isinstance(domain_payload, dict) else None
                        if not isinstance(edit_domains, dict):
                            continue
                        for edit_key in edit_domains:
                            edit_slug = aeb.slugify_route(edit_key)
                            if edit_slug == domain_slug or edit_slug.startswith(f"{domain_slug}_"):
                                repeated_tree_keys.append(f"{label}:asset_authoring.{domain}.{edit_key}")
            asset_operation = tree.get("asset_operation")
            if isinstance(asset_operation, dict):
                role_domains = asset_operation.get("role_domains")
                if isinstance(role_domains, dict):
                    for role, domains in role_domains.items():
                        if not isinstance(domains, dict):
                            continue
                        for domain, domain_payload in domains.items():
                            domain_slug = aeb.slugify_route(domain)
                            edit_domains = domain_payload.get("edit_domains") if isinstance(domain_payload, dict) else None
                            if not isinstance(edit_domains, dict):
                                continue
                            for edit_key in edit_domains:
                                edit_slug = aeb.slugify_route(edit_key)
                                if edit_slug == domain_slug or edit_slug.startswith(f"{domain_slug}_"):
                                    repeated_tree_keys.append(
                                        f"{label}:asset_operation.{role}.{domain}.{edit_key}"
                                    )

        collect_repeated_edit_domain_tree_keys(testset_index, "index")
        collect_repeated_edit_domain_tree_keys(modules_payload, "modules")
        check("asset route tree edit-domain keys do not repeat the AssetType token",
              not repeated_tree_keys,
              f"repeated={repeated_tree_keys[:8]}")
        legacy_asset_authoring_selectors: List[str] = []
        legacy_asset_operation_selectors: List[str] = []
        for module_id in module_lookup:
            parts = [part for part in str(module_id).split(".") if part]
            if len(parts) >= 3 and parts[0] == "asset_authoring":
                domain_slug, edit_slug = parts[1], parts[2]
                if edit_slug != "base":
                    legacy_asset_authoring_selectors.append(
                        ".".join(["asset_authoring", domain_slug, f"{domain_slug}_{edit_slug}"])
                    )
            elif len(parts) >= 4 and parts[0] == "asset_operation":
                role_slug, domain_slug, edit_slug = parts[1], parts[2], parts[3]
                if edit_slug != "base":
                    legacy_asset_operation_selectors.append(
                        ".".join(["asset_operation", role_slug, domain_slug, f"{domain_slug}_{edit_slug}"])
                    )

        bad_legacy_authoring = [
            selector
            for selector in legacy_asset_authoring_selectors
            if aeb.normalize_testset_module_id(selector) not in module_lookup
        ]
        bad_legacy_operation = [
            selector
            for selector in legacy_asset_operation_selectors
            if aeb.normalize_testset_module_id(selector) not in module_lookup
        ]
        check("legacy asset_authoring duplicate leaf selectors normalize",
              not bad_legacy_authoring,
              f"bad={bad_legacy_authoring[:8]}")
        check("legacy asset_operation duplicate leaf selectors normalize",
              not bad_legacy_operation,
              f"bad={bad_legacy_operation[:8]}")
        check("canonical asset batch_delete route exists",
              "asset_authoring.asset.batch_delete" in module_lookup)
        check("canonical asset-operation batch_delete route exists",
              "asset_operation.edit.asset.batch_delete" in module_lookup)
        canonical_authoring_tasks, canonical_authoring_selection = aeb.select_tasks(
            aeb.DEFAULT_TASKS,
            aeb.DEFAULT_TESTSETS,
            module_ids=["asset_authoring.asset.batch_delete"],
        )
        legacy_authoring_tasks, legacy_authoring_selection = aeb.select_tasks(
            aeb.DEFAULT_TASKS,
            aeb.DEFAULT_TESTSETS,
            module_ids=["asset_authoring.asset." + "asset_batch_delete"],
        )
        canonical_operation_tasks, canonical_operation_selection = aeb.select_tasks(
            aeb.DEFAULT_TASKS,
            aeb.DEFAULT_TESTSETS,
            module_ids=["asset_operation.edit.asset.batch_delete"],
        )
        legacy_operation_tasks, legacy_operation_selection = aeb.select_tasks(
            aeb.DEFAULT_TASKS,
            aeb.DEFAULT_TESTSETS,
            module_ids=["asset_operation.edit.asset." + "asset_batch_delete"],
        )
        check("legacy authoring duplicate selector selects same tasks as canonical",
              [task.get("id") for task in legacy_authoring_tasks] == [task.get("id") for task in canonical_authoring_tasks]
              and legacy_authoring_selection.get("selection_filters", {}).get("module_ids") == ["asset_authoring.asset.batch_delete"],
              f"legacy={legacy_authoring_selection.get('selection_filters', {}).get('module_ids')}")
        check("legacy asset-operation duplicate selector selects same tasks as canonical",
              [task.get("id") for task in legacy_operation_tasks] == [task.get("id") for task in canonical_operation_tasks]
              and legacy_operation_selection.get("selection_filters", {}).get("module_ids") == ["asset_operation.edit.asset.batch_delete"],
              f"legacy={legacy_operation_selection.get('selection_filters', {}).get('module_ids')}")

    docs_root = aeb.resolve_plugin_path(aeb.DEFAULT_TASKS).parent
    repeated_doc_selectors: List[str] = []
    repeated_selector_patterns = [
        re.compile(r"asset_authoring\.([A-Za-z0-9_]+)\.\1_[A-Za-z0-9_]+"),
        re.compile(r"asset_operation\.(?:creation_or_import|edit|save|readback_verify|delete)\.([A-Za-z0-9_]+)\.\1_[A-Za-z0-9_]+"),
    ]
    for doc_path in docs_root.rglob("*.md"):
        text = doc_path.read_text(encoding="utf-8")
        for pattern in repeated_selector_patterns:
            for match in pattern.finditer(text):
                repeated_doc_selectors.append(
                    f"{doc_path.relative_to(docs_root)}:{match.group(0)}"
                )
    check("generated markdown docs do not expose duplicate AssetType selector leaves",
          not repeated_doc_selectors,
          f"repeated={repeated_doc_selectors[:8]}")


def test_compact_route_helpers() -> None:
    legacy_authoring_selector = "asset_authoring.asset." + "asset_batch_delete"
    legacy_operation_selector = "asset_operation.edit.asset." + "asset_batch_delete"
    legacy_data_selector = "asset_authoring.data." + "data_datatable_strict_rejection"
    legacy_animation_selector = "asset_operation.edit.animation." + "animation_pose_search_database"
    canonical_examples = {
        "asset": {
            "asset_batch_delete": "batch_delete",
            "asset_asset_batch_delete": "batch_delete",
            "asset": "base",
        },
        "data": {
            "data_asset": "asset",
            "data_datatable_strict_rejection": "datatable_strict_rejection",
            "data": "base",
        },
        "animation": {
            "animation_pose_search_database": "pose_search_database",
            "animation_retarget_chain_lifecycle": "retarget_chain_lifecycle",
        },
    }
    for domain, edit_domains in canonical_examples.items():
        for edit_domain, expected_leaf in edit_domains.items():
            check(f"{domain}.{edit_domain} route leaf compacts",
                  aeb.compact_edit_domain_slug(domain, edit_domain) == expected_leaf)

    check("asset edit-domain prefix compacts",
          aeb.compact_edit_domain_slug("asset", "asset_batch_delete") == "batch_delete")
    check("repeated asset edit-domain prefixes compact",
          aeb.compact_edit_domain_slug("asset", "asset_asset_batch_delete") == "batch_delete")
    check("asset base edit-domain compacts",
          aeb.compact_edit_domain_slug("asset", "asset") == "base")
    check("canonical authoring route helper uses compact leaf",
          aeb.asset_type_case_module_id("asset", "asset_batch_delete") == "asset_authoring.asset.batch_delete")
    check("canonical asset-operation route helper uses compact leaf",
          aeb.asset_operation_case_module_ids(
              "asset",
              "asset_batch_delete",
              [{"capabilities": {"asset_operations": ["edit"]}}],
          ) == {"edit": "asset_operation.edit.asset.batch_delete"})
    check("canonical data route helper uses compact leaf",
          aeb.asset_type_case_module_id("data", "data_datatable_strict_rejection")
          == "asset_authoring.data.datatable_strict_rejection")
    check("canonical animation route helper uses compact leaf",
          aeb.asset_type_case_module_id("animation", "animation_pose_search_database")
          == "asset_authoring.animation.pose_search_database")
    check("canonical data asset-operation routes use compact leaf",
          aeb.asset_operation_case_module_ids(
              "data",
              "data_datatable_strict_rejection",
              [{"capabilities": {"asset_operations": ["creation_or_import", "edit", "save", "readback_verify"]}}],
          ) == {
              "creation_or_import": "asset_operation.creation_or_import.data.datatable_strict_rejection",
              "edit": "asset_operation.edit.data.datatable_strict_rejection",
              "save": "asset_operation.save.data.datatable_strict_rejection",
              "readback_verify": "asset_operation.readback_verify.data.datatable_strict_rejection",
          })
    check("legacy authoring selector normalizes",
          aeb.normalize_testset_module_id(legacy_authoring_selector) == "asset_authoring.asset.batch_delete")
    check("legacy asset-operation selector normalizes",
          aeb.normalize_testset_module_id(legacy_operation_selector) == "asset_operation.edit.asset.batch_delete")
    check("legacy data selector normalizes",
          aeb.normalize_testset_module_id(legacy_data_selector)
          == "asset_authoring.data.datatable_strict_rejection")
    check("legacy animation selector normalizes",
          aeb.normalize_testset_module_id(legacy_animation_selector)
          == "asset_operation.edit.animation.pose_search_database")
    check("legacy and canonical selector inputs dedupe",
          aeb.normalize_testset_module_ids([
              legacy_authoring_selector,
              "asset_authoring.asset.batch_delete",
          ]) == ["asset_authoring.asset.batch_delete"])


_GATE_STATUS_RESPONSE = {
    "jsonrpc": "2.0",
    "result": {
        "isError": False,
        "structuredContent": {
            "version": "0.20.3", "server_running": True, "total_actions": 1840,
            "namespaces": 61, "catalog_version": "sha256:test",
            "engine_version": "++UE5+Release-5.8", "project_name": "Speed",
        },
        "content": [{"type": "text", "text": STRUCTURED_STUB_TEXT}],
    },
}


def _gate_row(task: Dict[str, Any], *, transport_error: bool) -> Dict[str, Any]:
    return {
        "task_id": task.get("id"), "category": task.get("category"),
        "namespace": task.get("namespace", ""), "action": task.get("action", ""),
        "blueprint_type": task.get("blueprint_type", ""), "domain": task.get("domain", ""),
        "edit_domain": task.get("edit_domain", ""), "workflow": task.get("workflow", ""),
        "direct_success": not transport_error, "planning_signals": not transport_error,
        "evidence": {},
        "transport_error": transport_error,
        "transport_error_raw": "urlopen error [WinError 10061] connection refused" if transport_error else "",
        "response_is_error": transport_error, "response_text": "",
    }


def _run_gate_benchmark(out: pathlib.Path, task_ids: List[str], score_task) -> Dict[str, Any]:
    original_mcp_call = aeb.mcp_call
    original_score_task = aeb.score_task
    try:
        aeb.mcp_call = lambda url, tool, arguments, timeout_s=45.0: _GATE_STATUS_RESPONSE  # type: ignore[assignment]
        aeb.score_task = score_task  # type: ignore[assignment]
        return aeb.run_benchmark(
            "http://localhost:9316/mcp", aeb.DEFAULT_TASKS, out, "gate-test", 5.0,
            jobs=1, task_ids=task_ids,
        )
    finally:
        aeb.mcp_call = original_mcp_call  # type: ignore[assignment]
        aeb.score_task = original_score_task  # type: ignore[assignment]


def test_transport_failure_gate() -> None:
    """A run against a dead/restarting editor must be rejected, not scored.

    Transport errors produce empty responses that score exactly like capability failures.
    On 2026-07-11 a 578-task run with 157 transport errors (27%) still wrote a scored
    summary.json and exited 0, publishing an editor outage as an AssetEditing capability
    regression. The gate must instead leave run_failure.json plus partial_summary.json
    and NO summary.json -- including a stale one from an earlier run in the same dir.
    """
    tasks = aeb.load_jsonl(aeb.resolve_plugin_path(aeb.DEFAULT_TASKS))[:40]
    with tempfile.TemporaryDirectory() as tmp:
        out = pathlib.Path(tmp) / "run"
        out.mkdir(parents=True, exist_ok=True)
        (out / "summary.json").write_text('{"stale": true}', encoding="utf-8")

        summary = _run_gate_benchmark(
            out,
            [str(t["id"]) for t in tasks],
            lambda url, task, timeout_s: _gate_row(task, transport_error=True),
        )

        check("transport gate rejects the run", summary.get("run_valid") is False,
              f"run_valid={summary.get('run_valid')}")
        check("transport gate reports the abort reason",
              summary.get("completion_status") == "aborted_transport_failure_budget",
              str(summary.get("completion_status")))
        check("transport gate fires on the consecutive-failure budget",
              summary.get("transport_abort", {}).get("reason") == "consecutive_transport_failures",
              json.dumps(summary.get("transport_abort", {})))
        check("transport gate stops early instead of burning the whole corpus",
              summary.get("completed_task_count", 0) <= aeb.DEFAULT_MAX_CONSECUTIVE_TRANSPORT_FAILURES,
              f"completed={summary.get('completed_task_count')}")
        check("transport gate writes run_failure.json", (out / "run_failure.json").exists())
        check("transport gate writes partial_summary.json", (out / "partial_summary.json").exists())
        check("transport gate writes NO summary.json (stale one removed)",
              not (out / "summary.json").exists())
        check("transport gate marks metrics as an outage-contaminated prefix",
              summary.get("metrics_scope") == "attempted_prefix_transport_failure",
              str(summary.get("metrics_scope")))


def test_transport_flapping_editor_gate() -> None:
    """A flapping editor never trips the consecutive gate; the fraction gate must still reject.

    Every 4th task fails, so consecutive failures never reach 3 -- exactly the shape that
    let the 27%-outage run through. finalize()/the fraction gate must still reject it.
    """
    tasks = aeb.load_jsonl(aeb.resolve_plugin_path(aeb.DEFAULT_TASKS))[:40]
    seen: List[int] = []

    def flapping(url: str, task: Dict[str, Any], timeout_s: float) -> Dict[str, Any]:
        seen.append(1)
        return _gate_row(task, transport_error=(len(seen) % 4 == 0))

    with tempfile.TemporaryDirectory() as tmp:
        out = pathlib.Path(tmp) / "run"
        summary = _run_gate_benchmark(out, [str(t["id"]) for t in tasks], flapping)

        check("flapping editor is rejected by the fraction gate",
              summary.get("run_valid") is False, f"run_valid={summary.get('run_valid')}")
        check("flapping editor never trips the consecutive gate",
              summary.get("transport_abort", {}).get("reason") != "consecutive_transport_failures",
              json.dumps(summary.get("transport_abort", {})))
        check("flapping editor writes NO summary.json", not (out / "summary.json").exists())
        check("flapping editor writes run_failure.json", (out / "run_failure.json").exists())


def test_transport_gate_passes_a_healthy_run() -> None:
    """A clean run still writes a scored summary.json with run_valid=True."""
    tasks = aeb.load_jsonl(aeb.resolve_plugin_path(aeb.DEFAULT_TASKS))[:12]
    with tempfile.TemporaryDirectory() as tmp:
        out = pathlib.Path(tmp) / "run"
        summary = _run_gate_benchmark(
            out,
            [str(t["id"]) for t in tasks],
            lambda url, task, timeout_s: _gate_row(task, transport_error=False),
        )
        check("healthy run stays valid", summary.get("run_valid") is True,
              f"run_valid={summary.get('run_valid')}")
        check("healthy run writes summary.json", (out / "summary.json").exists())
        check("healthy run writes no run_failure.json", not (out / "run_failure.json").exists())
        check("healthy run records a zero transport-failure snapshot",
              summary.get("transport", {}).get("transport_failure_count") == 0,
              json.dumps(summary.get("transport", {})))
        inputs = summary.get("benchmark_inputs", {})
        check("asset benchmark fingerprints its runner",
              set(inputs.get("files", {})) == {"benchmark_common", "tasks", "manifest", "runner"},
              json.dumps(sorted(inputs.get("files", {}))))
        check("asset task selection is covered by the stored fingerprint",
              inputs.get("task_selection") == summary.get("task_selection")
              and inputs.get("fingerprint_sha256")
              == benchmark_common.benchmark_input_fingerprint(inputs),
              str(inputs.get("fingerprint_sha256")))
        check("explicit subset is not comparison-valid",
              summary.get("metrics_valid") is True
              and summary.get("metrics_scope") == "explicit_subset"
              and summary.get("comparison_valid") is False,
              json.dumps({
                  "metrics_valid": summary.get("metrics_valid"),
                  "metrics_scope": summary.get("metrics_scope"),
                  "comparison_valid": summary.get("comparison_valid"),
              }))


def test_status_preflight_and_stale_output_contract() -> None:
    """Invalid endpoint identity must abort before scoring and cannot preserve stale success."""
    tasks = aeb.load_jsonl(aeb.resolve_plugin_path(aeb.DEFAULT_TASKS))[:1]
    task_ids = [str(tasks[0]["id"])]
    invalid_statuses = (
        (
            "server stopped",
            {
                "jsonrpc": "2.0",
                "result": {
                    "isError": False,
                    "structuredContent": {
                        "server_running": False,
                        "project_name": "Speed",
                    },
                    "content": [{"type": "text", "text": STRUCTURED_STUB_TEXT}],
                },
            },
        ),
        (
            "wrong project",
            {
                "jsonrpc": "2.0",
                "result": {
                    "isError": False,
                    "structuredContent": {
                        "server_running": True,
                        "project_name": "AnotherProject",
                    },
                    "content": [{"type": "text", "text": STRUCTURED_STUB_TEXT}],
                },
            },
        ),
    )

    for case_name, status_response in invalid_statuses:
        with tempfile.TemporaryDirectory() as tmp:
            out = pathlib.Path(tmp) / "run"
            out.mkdir(parents=True, exist_ok=True)
            (out / "summary.json").write_text('{"stale": true}', encoding="utf-8")
            score_calls: List[str] = []
            original_mcp_call = aeb.mcp_call
            original_score_task = aeb.score_task
            try:
                aeb.mcp_call = lambda url, tool, arguments, timeout_s=45.0: status_response  # type: ignore[assignment]
                aeb.score_task = lambda url, task, timeout_s: score_calls.append(str(task["id"]))  # type: ignore[assignment]
                failure = aeb.run_benchmark(
                    "http://localhost:9316/mcp", aeb.DEFAULT_TASKS, out,
                    f"invalid-status-{case_name}", 5.0, jobs=1, task_ids=task_ids,
                )
            finally:
                aeb.mcp_call = original_mcp_call  # type: ignore[assignment]
                aeb.score_task = original_score_task  # type: ignore[assignment]

            check(f"{case_name} status is rejected", failure.get("run_valid") is False,
                  json.dumps(failure, default=str)[:500])
            check(f"{case_name} aborts at status preflight",
                  failure.get("failure_stage") == "status_preflight",
                  str(failure.get("failure_stage")))
            check(f"{case_name} never scores a task", not score_calls, str(score_calls))
            check(f"{case_name} removes stale summary", not (out / "summary.json").exists())
            check(f"{case_name} writes failure evidence", (out / "run_failure.json").exists())


def test_selection_preflight_clears_stale_summary() -> None:
    """A zero-match selector can raise, but it cannot leave an older accepted result behind."""
    with tempfile.TemporaryDirectory() as tmp:
        out = pathlib.Path(tmp) / "run"
        out.mkdir(parents=True, exist_ok=True)
        (out / "summary.json").write_text('{"stale": true}', encoding="utf-8")
        raised = False
        try:
            aeb.run_benchmark(
                "http://localhost:9316/mcp", aeb.DEFAULT_TASKS, out,
                "zero-match", 5.0, jobs=1,
                task_ids=["ASSET-BENCHMARK-NO-SUCH-TASK"],
            )
        except RuntimeError as exc:
            raised = "matched 0 tasks" in str(exc)
        check("zero-match selector still reports its contract error", raised)
        check("selection preflight removes stale summary before raising",
              not (out / "summary.json").exists())


def test_failed_then_healthy_run_removes_invalid_artifacts() -> None:
    """Reusing an output directory must publish one coherent terminal state."""
    tasks = aeb.load_jsonl(aeb.resolve_plugin_path(aeb.DEFAULT_TASKS))[:2]
    task_ids = [str(task["id"]) for task in tasks]
    stopped_status = {
        "jsonrpc": "2.0",
        "result": {
            "isError": False,
            "structuredContent": {"server_running": False, "project_name": "Speed"},
            "content": [{"type": "text", "text": STRUCTURED_STUB_TEXT}],
        },
    }
    with tempfile.TemporaryDirectory() as tmp:
        out = pathlib.Path(tmp) / "run"
        original_mcp_call = aeb.mcp_call
        try:
            aeb.mcp_call = lambda url, tool, arguments, timeout_s=45.0: stopped_status  # type: ignore[assignment]
            failed = aeb.run_benchmark(
                "http://localhost:9316/mcp", aeb.DEFAULT_TASKS, out,
                "failed-first", 5.0, jobs=1, task_ids=task_ids,
            )
        finally:
            aeb.mcp_call = original_mcp_call  # type: ignore[assignment]
        check("failed first run writes invalid artifacts",
              failed.get("run_valid") is False
              and (out / "run_failure.json").exists()
              and (out / "partial_summary.json").exists())

        healthy = _run_gate_benchmark(
            out, task_ids,
            lambda url, task, timeout_s: _gate_row(task, transport_error=False),
        )
        check("healthy rerun succeeds in the same output directory",
              healthy.get("run_valid") is True)
        check("healthy rerun publishes summary", (out / "summary.json").exists())
        check("healthy rerun removes run_failure", not (out / "run_failure.json").exists())
        check("healthy rerun removes partial summary", not (out / "partial_summary.json").exists())


def main() -> int:
    tasks = aeb.load_jsonl(aeb.resolve_plugin_path(aeb.DEFAULT_TASKS))
    manifest = load_json(aeb.DEFAULT_MANIFEST)

    test_generator_preserves_expected_read_only_outputs()
    test_generator_uses_platform_native_line_endings()
    test_manifest_matches_tasks(tasks, manifest)
    test_task_shape(tasks)
    test_transport_failure_gate()
    test_transport_flapping_editor_gate()
    test_transport_gate_passes_a_healthy_run()
    test_status_preflight_and_stale_output_contract()
    test_selection_preflight_clears_stale_summary()
    test_failed_then_healthy_run_removes_invalid_artifacts()
    test_expect_error_requires_a_rejection()
    test_expected_rejection_has_non_success_lifecycle(tasks)
    test_structured_results_token_scanning()
    test_compile_signal_requires_evidence()
    test_error_text_assertions_still_use_human_text()
    test_structured_expect_scoring()
    test_engine_font_resolver_discovery()
    test_pcg_asset_authoring_contract(tasks)
    test_ui_high_usage_asset_authoring_contract(tasks)
    test_high_error_recovery_coverage(tasks)
    test_asset_type_and_testset_indexes(tasks, manifest)
    test_root_readme_generated_summary(tasks, manifest)
    test_compact_route_helpers()

    if _FAILURES:
        print(f"\nFAILED: {len(_FAILURES)} AssetEditing benchmark check(s) failed")
        for failure in _FAILURES[:25]:
            print(f" - {failure}")
        if len(_FAILURES) > 25:
            print(f" - ... {len(_FAILURES) - 25} more")
        return 1

    print("\nOK: AssetEditing benchmark corpus is internally consistent")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
