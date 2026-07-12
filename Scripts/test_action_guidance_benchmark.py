#!/usr/bin/env python3
"""Offline contract tests for the ActionGuidance benchmark corpus.

This script does not contact the editor or MCP endpoint. It verifies that the
checked-in ActionGuidance task set is internally consistent and still exercises
the recovery categories used by SPEC_MonolithPractitionerWorkflowROI P0.3.4.
"""

from __future__ import annotations

import json
import pathlib
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
