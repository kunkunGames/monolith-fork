#!/usr/bin/env python3
"""Offline contract tests for the ActionGuidance benchmark corpus.

This script does not contact the editor or MCP endpoint. It verifies that the
checked-in ActionGuidance task set is internally consistent and still exercises
the recovery categories used by SPEC_MonolithPractitionerWorkflowROI P0.3.4.
"""

from __future__ import annotations

import json
import pathlib
import re
import sys
from typing import Any, Dict, List

_SCRIPTS_DIR = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(_SCRIPTS_DIR))

import action_guidance_benchmark as agb  # noqa: E402

REQUIRED_CATEGORIES = {
    "discovery_planning",
    "needed_action_routing",
    "unknown_action_recovery",
    "missing_required_param",
    "invalid_param_type",
}

REQUIRED_HIGH_TRAFFIC_ACTIONS = {
    "monolith.find",
    "source.search_source",
    "project.search",
    "blueprint.add_variable",
    "blueprint.add_function",
    "blueprint.create_blueprint",
}

REQUIRED_SOURCE_CONTROL_POLICY_ACTIONS = {
    "source_control.list_opened",
    "source_control.map_depot_paths",
}

_FAILURES: List[str] = []


def check(name: str, condition: bool, detail: str = "") -> None:
    status = "PASS" if condition else "FAIL"
    print(f"[{status}] {name}" + (f" -- {detail}" if detail else ""))
    if not condition:
        _FAILURES.append(name)


def load_manifest() -> Dict[str, Any]:
    return json.loads(agb.resolve_plugin_path(agb.DEFAULT_MANIFEST).read_text(encoding="utf-8"))


def action_id(task: Dict[str, Any]) -> str:
    return f"{task.get('namespace', '')}.{task.get('action', '')}"


def load_source_catalog_namespaces() -> List[Dict[str, Any]]:
    catalog_path = agb.resolve_plugin_path(
        pathlib.Path("Tools/MonolithQuery/Generated/monolith_catalog_snapshot.json")
    )
    catalog = json.loads(catalog_path.read_text(encoding="utf-8"))
    actions = catalog.get("actions")
    if not isinstance(actions, list) or not actions:
        raise RuntimeError(f"generated source catalog has no actions: {catalog_path}")

    by_namespace: Dict[str, set[str]] = {}
    full_names: set[str] = set()
    for row in actions:
        if not isinstance(row, dict):
            continue
        namespace = str(row.get("namespace", "")).strip()
        action = str(row.get("action", "")).strip()
        if namespace and action:
            by_namespace.setdefault(namespace, set()).add(action)
            full_names.add(f"{namespace}.{action}")
    declared_action_count = catalog.get("action_count")
    if declared_action_count != len(full_names) or len(actions) != len(full_names):
        raise RuntimeError(
            f"generated source catalog action count mismatch: declared={declared_action_count!r} "
            f"rows={len(actions)} unique={len(full_names)}"
        )
    return [
        {"namespace": namespace, "actions": sorted(namespace_actions)}
        for namespace, namespace_actions in sorted(by_namespace.items())
    ]


def test_static_action_contracts_against_source_catalog() -> None:
    detail = ""
    try:
        agb.validate_static_unreal_practical_action_contracts(load_source_catalog_namespaces())
        valid = True
    except Exception as exc:  # noqa: BLE001 - report contract drift through the check harness
        valid = False
        detail = f"{type(exc).__name__}: {exc}"
    check("static action ids exist in generated source catalog", valid, detail)


def test_static_tasks_are_materialized(tasks: List[Dict[str, Any]]) -> None:
    expected_tasks: List[Dict[str, Any]] = []
    agb.append_static_unreal_practical_tasks(expected_tasks)
    actual_fingerprints = {agb.task_fingerprint(task) for task in tasks}
    missing = [
        action_id(task)
        for task in expected_tasks
        if agb.task_fingerprint(task) not in actual_fingerprints
    ]
    check(
        "canonical corpus materializes every static task contract",
        not missing,
        f"missing={missing[:12]}",
    )


def test_legacy_action_alias_contract(tasks: List[Dict[str, Any]]) -> None:
    expected_rows = [
        (str(namespace), str(target), str(retired))
        for namespace, target, retired in agb._STATIC_LEGACY_ACTION_MIGRATIONS_20260717
    ]
    expected_folded = {
        (namespace.casefold(), target.casefold(), retired.casefold())
        for namespace, target, retired in expected_rows
    }
    check(
        "legacy alias Python table has 15 unique mappings",
        len(expected_rows) == 15 and len(expected_folded) == len(expected_rows),
        f"rows={len(expected_rows)} unique={len(expected_folded)}",
    )
    check(
        "legacy aliases never equal their current action",
        all(target.casefold() != retired.casefold() for _, target, retired in expected_rows),
    )

    registry_path = agb.resolve_plugin_path(
        pathlib.Path("Source/MonolithCore/Private/MonolithToolRegistry.cpp")
    )
    registry_text = registry_path.read_text(encoding="utf-8")
    begin_marker = "// BEGIN ACTION_GUIDANCE_LEGACY_ALIAS_CONTRACT_20260717"
    end_marker = "// END ACTION_GUIDANCE_LEGACY_ALIAS_CONTRACT_20260717"
    marker_count_ok = registry_text.count(begin_marker) == 1 and registry_text.count(end_marker) == 1
    check("legacy alias C++ contract has one marker pair", marker_count_ok)

    cpp_rows: List[tuple[str, str, str]] = []
    if marker_count_ok:
        contract_block = registry_text.split(begin_marker, 1)[1].split(end_marker, 1)[0]
        seed_pattern = re.compile(
            r'AddLegacyActionAliasSeed\(\s*Result,\s*TEXT\("([^"]+)"\),\s*'
            r'TEXT\("([^"]+)"\),\s*\{(.*?)\}\s*\);',
            re.DOTALL,
        )
        for namespace, target, alias_block in seed_pattern.findall(contract_block):
            for retired in re.findall(r'TEXT\("([^"]+)"\)', alias_block):
                cpp_rows.append((namespace, target, retired))
    cpp_folded = {
        (namespace.casefold(), target.casefold(), retired.casefold())
        for namespace, target, retired in cpp_rows
    }
    check(
        "legacy alias C++ seed has no duplicate mappings",
        len(cpp_rows) == len(cpp_folded),
        f"rows={len(cpp_rows)} unique={len(cpp_folded)}",
    )
    check(
        "legacy alias C++ and Python mappings match exactly",
        cpp_folded == expected_folded,
        f"missing_in_cpp={sorted(expected_folded - cpp_folded)} extra_in_cpp={sorted(cpp_folded - expected_folded)}",
    )

    snapshot_path = agb.resolve_plugin_path(
        pathlib.Path("Tools/MonolithQuery/Generated/monolith_catalog_snapshot.json")
    )
    snapshot = json.loads(snapshot_path.read_text(encoding="utf-8"))
    snapshot_rows = snapshot.get("actions", [])
    source_ids = [
        f"{row.get('namespace', '')}.{row.get('action', '')}"
        for row in snapshot_rows
        if isinstance(row, dict) and row.get("namespace") and row.get("action")
    ]
    source_ids_folded = {action_id.casefold() for action_id in source_ids}
    check(
        "source snapshot action ids are case-insensitively unique",
        len(source_ids) == len(source_ids_folded),
        f"rows={len(source_ids)} unique={len(source_ids_folded)}",
    )
    target_missing = []
    retired_present = []
    for namespace, target, retired in expected_rows:
        target_id = f"{namespace}.{target}"
        retired_id = f"{namespace}.{retired}"
        if target_id.casefold() not in source_ids_folded:
            target_missing.append(target_id)
        if retired_id.casefold() in source_ids_folded:
            retired_present.append(retired_id)
    check("legacy alias targets exist in source snapshot", not target_missing, f"missing={target_missing}")
    check("legacy aliases remain absent from source actions", not retired_present, f"present={retired_present}")

    bad_task_rows = []
    for namespace, target, retired in expected_rows:
        matching_tasks = [
            task
            for task in tasks
            if str(task.get("namespace", "")).casefold() == namespace.casefold()
            and str(task.get("action", "")).casefold() == retired.casefold()
        ]
        expected_candidate = f"{namespace}.{target}"
        if (
            len(matching_tasks) != 1
            or matching_tasks[0].get("category") != "unknown_action_recovery"
            or matching_tasks[0].get("expected", {}).get("candidate_action") != expected_candidate
        ):
            bad_task_rows.append(
                {
                    "retired": f"{namespace}.{retired}",
                    "target": expected_candidate,
                    "task_ids": [task.get("id") for task in matching_tasks],
                }
            )
    check(
        "canonical corpus covers each legacy alias exactly once",
        not bad_task_rows,
        f"bad={bad_task_rows[:5]}",
    )


def test_static_action_contract_validator_rejects_schema_drift() -> None:
    contracts = [
        {
            "source": "test_missing_param",
            "namespace": "fixture",
            "action": "inspect",
            "required_param": "asset_path",
        },
        {
            "source": "test_invalid_param",
            "namespace": "fixture",
            "action": "inspect",
            "invalid_param": "asset_path",
            "provided_params": {"asset_path": 12345},
        },
    ]
    namespaces = [{"namespace": "fixture", "actions": ["inspect"]}]
    valid_schema = {"params": {"asset_path": {"required": True, "type": "string"}}}

    try:
        agb.validate_action_contracts(contracts, namespaces, lambda _namespace, _action: valid_schema)
        accepts_valid_contract = True
    except Exception:
        accepts_valid_contract = False
    check("static contract validator accepts matching schema", accepts_valid_contract)

    drifted_schema = {"params": {"asset_path": {"required": False, "type": "string"}}}
    try:
        agb.validate_action_contracts(contracts, namespaces, lambda _namespace, _action: drifted_schema)
        rejected_drift = False
    except RuntimeError as exc:
        rejected_drift = "no longer requires 'asset_path'" in str(exc)
    check("static contract validator rejects required-param drift", rejected_drift)


def test_action_contract_validator_rejects_unexpected_probe_action() -> None:
    contracts = [
        {
            "source": "test_unknown_probe",
            "namespace": "fixture",
            "action": "retired_action",
            "expected_absent": True,
        }
    ]
    namespaces = [{"namespace": "fixture", "actions": ["retired_action"]}]
    try:
        agb.validate_action_contracts(contracts, namespaces)
        rejected_probe = False
    except RuntimeError as exc:
        rejected_probe = "unexpectedly exists" in str(exc)
    check("static contract validator rejects a probe that became real", rejected_probe)


def test_action_contract_validator_rejects_missing_action() -> None:
    contracts = [{"source": "test_missing_action", "namespace": "fixture", "action": "inspect"}]
    namespaces = [{"namespace": "fixture", "actions": []}]
    try:
        agb.validate_action_contracts(contracts, namespaces)
        rejected_missing = False
    except RuntimeError as exc:
        rejected_missing = "test_missing_action" in str(exc) and "fixture.inspect" in str(exc)
    check("static contract validator rejects a removed action", rejected_missing)


def test_action_contract_validator_rejects_invalid_fixture_drift() -> None:
    valid_value_contract = [
        {
            "source": "test_invalid_value",
            "namespace": "fixture",
            "action": "inspect",
            "invalid_param": "asset_path",
            "provided_params": {"asset_path": "now-valid"},
        }
    ]
    base_schema = {"params": {"asset_path": {"required": True, "type": "string"}}}
    namespaces = [{"namespace": "fixture", "actions": ["inspect"]}]
    try:
        agb.validate_action_contracts(valid_value_contract, namespaces, lambda _namespace, _action: base_schema)
        rejected_valid_value = False
    except RuntimeError as exc:
        rejected_valid_value = "now matches type" in str(exc)
    check("static contract validator rejects an invalid fixture that became valid", rejected_valid_value)

    omitted_required_contract = [
        {
            "source": "test_new_required_param",
            "namespace": "fixture",
            "action": "inspect",
            "invalid_param": "asset_path",
            "provided_params": {"asset_path": 12345},
        }
    ]
    expanded_schema = {
        "params": {
            "asset_path": {"required": True, "type": "string"},
            "context": {"required": True, "type": "string"},
        }
    }
    try:
        agb.validate_action_contracts(
            omitted_required_contract,
            namespaces,
            lambda _namespace, _action: expanded_schema,
        )
        rejected_omission = False
    except RuntimeError as exc:
        rejected_omission = "omits required params ['context']" in str(exc)
    check("static contract validator rejects newly omitted required params", rejected_omission)


def test_action_contract_validator_rejects_identity_and_policy_drift() -> None:
    identity_contract = [
        {
            "source": "test_identity",
            "namespace": "fixture",
            "action": "inspect",
            "expected_action_id": "fixture.other",
        }
    ]
    namespaces = [{"namespace": "fixture", "actions": ["inspect"]}]
    try:
        agb.validate_action_contracts(identity_contract, namespaces)
        rejected_identity = False
    except RuntimeError as exc:
        rejected_identity = "does not match" in str(exc)
    check("static contract validator rejects routing identity drift", rejected_identity)

    policy_contract = [
        {
            "source": "test_policy",
            "namespace": "fixture",
            "action": "inspect",
            "expected_execution_policy_id": "read_only",
            "expected_execution_policy_defaulted": False,
            "expected_mutates_assets": False,
        }
    ]
    drifted_policy_schema = {
        "params": {},
        "execution_policy": {"policy_id": "mutation", "defaulted": True},
        "mutates_assets": True,
    }
    try:
        agb.validate_action_contracts(
            policy_contract,
            namespaces,
            lambda _namespace, _action: drifted_policy_schema,
        )
        rejected_policy = False
    except RuntimeError as exc:
        error = str(exc)
        rejected_policy = "policy_id='mutation'" in error and "mutates_assets=True" in error
    check("static contract validator rejects execution-policy drift", rejected_policy)


def test_manifest_matches_tasks(tasks: List[Dict[str, Any]], manifest: Dict[str, Any]) -> None:
    check("task_count matches JSONL rows", manifest.get("task_count") == len(tasks),
          f"manifest={manifest.get('task_count')} rows={len(tasks)}")
    check("task_count >= min_tasks_requested",
          int(manifest.get("task_count", 0)) >= int(manifest.get("min_tasks_requested", 0)),
          f"task_count={manifest.get('task_count')} min={manifest.get('min_tasks_requested')}")
    check("catalog_action_count is nonzero", int(manifest.get("catalog_action_count", 0)) > 0)
    check("namespace_coverage is nonempty", bool(manifest.get("namespace_coverage")))


def test_required_categories(tasks: List[Dict[str, Any]], manifest: Dict[str, Any]) -> None:
    counts: Dict[str, int] = {}
    for task in tasks:
        counts[str(task.get("category", ""))] = counts.get(str(task.get("category", "")), 0) + 1

    check("all required categories are present",
          REQUIRED_CATEGORIES.issubset(counts.keys()),
          f"missing={sorted(REQUIRED_CATEGORIES - set(counts.keys()))}")

    manifest_counts = manifest.get("category_counts", {})
    for category in sorted(REQUIRED_CATEGORIES):
        check(f"manifest category count matches {category}",
              manifest_counts.get(category) == counts.get(category),
              f"manifest={manifest_counts.get(category)} rows={counts.get(category)}")


def test_task_shape(tasks: List[Dict[str, Any]]) -> None:
    seen_ids = set()
    duplicate_ids: List[str] = []
    missing_fields: List[str] = []
    bad_shapes: List[str] = []
    bad_expectations: List[str] = []
    for index, task in enumerate(tasks, 1):
        task_id = str(task.get("id", ""))
        if not task_id:
            missing_fields.append(f"row {index}:id")
        if task_id in seen_ids:
            duplicate_ids.append(task_id)
        seen_ids.add(task_id)

        for field in ("namespace", "action", "tool", "category", "arguments", "expected", "safety"):
            if field not in task:
                missing_fields.append(f"{task_id or index}:{field}")
        if not isinstance(task.get("arguments"), dict):
            bad_shapes.append(f"{task_id or index}:arguments")
        if not isinstance(task.get("expected"), dict):
            bad_shapes.append(f"{task_id or index}:expected")
        if not (isinstance(task.get("weight"), (int, float)) and float(task["weight"]) > 0):
            bad_shapes.append(f"{task_id or index}:weight")

        category = task.get("category")
        expected = task.get("expected", {})
        if category == "missing_required_param":
            if "missing_required_params" not in expected and "failure_cause" not in expected:
                bad_expectations.append(f"{task_id}:required-param")
        elif category == "invalid_param_type":
            if "validation_errors" not in expected and expected.get("failure_cause") != "invalid_param":
                bad_expectations.append(f"{task_id}:invalid-param")
        elif category == "unknown_action_recovery":
            if not bool(expected.get("candidate_action")) and not bool(expected.get("candidate_actions")):
                bad_expectations.append(f"{task_id}:unknown-action")
        elif category == "discovery_planning":
            if not bool(expected.get("requires_planning_signals")):
                bad_expectations.append(f"{task_id}:planning")

    check("all tasks have unique ids", not duplicate_ids, f"duplicates={duplicate_ids[:5]}")
    check("all tasks have required fields", not missing_fields, f"missing={missing_fields[:8]}")
    check("all task payload fields have valid shape", not bad_shapes, f"bad={bad_shapes[:8]}")
    check("category-specific expectations are declared", not bad_expectations, f"bad={bad_expectations[:8]}")


def test_demand_weighting(tasks: List[Dict[str, Any]], manifest: Dict[str, Any]) -> None:
    weighted_tasks = [task for task in tasks if float(task.get("weight", 1.0)) != agb.DEFAULT_WEIGHT]
    manifest_weighting = manifest.get("demand_weighting", {})
    check("weighted_task_count matches rows",
          manifest_weighting.get("weighted_task_count") == len(weighted_tasks),
          f"manifest={manifest_weighting.get('weighted_task_count')} rows={len(weighted_tasks)}")

    check("weighted_action_count matches demand table",
          manifest_weighting.get("weighted_action_count") == len(agb._ACTION_STATS_20260618),
          f"manifest={manifest_weighting.get('weighted_action_count')} table={len(agb._ACTION_STATS_20260618)}")

    covered_actions = {action_id(task) for task in tasks}
    check("high-traffic actions covered",
          REQUIRED_HIGH_TRAFFIC_ACTIONS.issubset(covered_actions),
          f"missing={sorted(REQUIRED_HIGH_TRAFFIC_ACTIONS - covered_actions)}")

    discovery_tool_count = sum(1 for task in tasks if task.get("tool") == "monolith_discover")
    check("monolith_discover coverage exists", discovery_tool_count > 0,
          f"monolith_discover_tasks={discovery_tool_count}")


def test_source_control_read_only_policy_contract(tasks: List[Dict[str, Any]]) -> None:
    policy_tasks = [
        task for task in tasks
        if action_id(task) in REQUIRED_SOURCE_CONTROL_POLICY_ACTIONS
        and task.get("category") == "discovery_planning"
        and task.get("expected", {}).get("execution_policy_id") == "read_only"
    ]
    counts = {required: 0 for required in REQUIRED_SOURCE_CONTROL_POLICY_ACTIONS}
    bad_contracts: List[str] = []
    for task in policy_tasks:
        task_action_id = action_id(task)
        counts[task_action_id] += 1
        expected = task.get("expected", {})
        if expected.get("execution_policy_defaulted") is not False or expected.get("mutates_assets") is not False:
            bad_contracts.append(task_action_id)

    check("source-control read-only policy tasks exist exactly once",
          all(count == 1 for count in counts.values()),
          f"counts={counts}")
    check("source-control policy tasks require explicit non-mutating policy",
          not bad_contracts,
          f"bad={bad_contracts}")


def test_transport_failure_gates(manifest: Dict[str, Any]) -> None:
    gates = manifest.get("run_gates", {})
    check(
        "transport failure fraction matches runner default",
        gates.get("max_transport_failed_fraction") == agb.DEFAULT_MAX_TRANSPORT_FAILED_FRACTION,
        f"manifest={gates.get('max_transport_failed_fraction')} runner={agb.DEFAULT_MAX_TRANSPORT_FAILED_FRACTION}",
    )
    check(
        "consecutive transport gate matches runner default",
        gates.get("max_consecutive_transport_failures") == agb.DEFAULT_MAX_CONSECUTIVE_TRANSPORT_FAILURES,
        f"manifest={gates.get('max_consecutive_transport_failures')} runner={agb.DEFAULT_MAX_CONSECUTIVE_TRANSPORT_FAILURES}",
    )
    check(
        "transport fraction minimum sample matches shared runner default",
        gates.get("min_transport_fraction_sample") == agb.DEFAULT_MIN_TRANSPORT_FRACTION_SAMPLES,
        f"manifest={gates.get('min_transport_fraction_sample')} runner={agb.DEFAULT_MIN_TRANSPORT_FRACTION_SAMPLES}",
    )
    check(
        "invalid transport run cannot write a normal summary",
        gates.get("status_transport_failure_aborts_before_tasks") is True
        and gates.get("invalid_status_response_aborts_before_tasks") is True
        and gates.get("invalid_run_writes_summary") is False,
        f"gates={gates}",
    )


def test_catalog_provenance(manifest: Dict[str, Any]) -> None:
    catalog_version = manifest.get("catalog_version")
    check(
        "catalog version provenance is recorded",
        isinstance(catalog_version, str) and catalog_version.startswith("sha256:"),
        f"catalog_version={catalog_version!r}",
    )


def main() -> int:
    tasks = agb.load_jsonl(agb.DEFAULT_TASKS)
    manifest = load_manifest()

    test_static_action_contracts_against_source_catalog()
    test_static_action_contract_validator_rejects_schema_drift()
    test_action_contract_validator_rejects_unexpected_probe_action()
    test_action_contract_validator_rejects_missing_action()
    test_action_contract_validator_rejects_invalid_fixture_drift()
    test_action_contract_validator_rejects_identity_and_policy_drift()
    test_static_tasks_are_materialized(tasks)
    test_legacy_action_alias_contract(tasks)
    test_manifest_matches_tasks(tasks, manifest)
    test_required_categories(tasks, manifest)
    test_task_shape(tasks)
    test_demand_weighting(tasks, manifest)
    test_source_control_read_only_policy_contract(tasks)
    test_transport_failure_gates(manifest)
    test_catalog_provenance(manifest)

    if _FAILURES:
        print(f"\nFAILED: {len(_FAILURES)} ActionGuidance benchmark check(s) failed")
        for failure in _FAILURES[:20]:
            print(f" - {failure}")
        if len(_FAILURES) > 20:
            print(f" - ... {len(_FAILURES) - 20} more")
        return 1

    print("\nOK: ActionGuidance benchmark corpus is internally consistent")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
