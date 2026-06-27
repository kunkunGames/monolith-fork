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
import pathlib
import sys
from typing import Any, Dict, Iterable, List, Set

_SCRIPTS_DIR = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(_SCRIPTS_DIR))

import asset_editing_benchmark as aeb  # noqa: E402

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

_FAILURES: List[str] = []


def check(name: str, condition: bool, detail: str = "") -> None:
    status = "PASS" if condition else "FAIL"
    print(f"[{status}] {name}" + (f" -- {detail}" if detail else ""))
    if not condition:
        _FAILURES.append(name)


def load_json(path: pathlib.Path) -> Dict[str, Any]:
    return json.loads(path.read_text(encoding="utf-8"))


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
    asset_index_path = pathlib.Path(manifest.get("asset_type_index", ""))
    testset_index_path = pathlib.Path(manifest.get("testset_index", ""))
    testset_modules_path = pathlib.Path(manifest.get("testset_modules", ""))

    check("asset type index exists", asset_index_path.exists(), str(asset_index_path))
    check("testset index exists", testset_index_path.exists(), str(testset_index_path))
    check("testset modules manifest exists", testset_modules_path.exists(), str(testset_modules_path))

    if asset_index_path.exists():
        asset_index = load_json(asset_index_path)
        asset_types = asset_index.get("asset_types", [])
        check("asset_type_count matches index",
              manifest.get("asset_type_count") == len(asset_types),
              f"manifest={manifest.get('asset_type_count')} index={len(asset_types)}")
        for item in asset_types:
            task_file = pathlib.Path(str(item.get("tasks_file", "")))
            rows = aeb.load_jsonl(task_file) if task_file.exists() else []
            check(f"asset type task count matches {item.get('asset_type')}",
                  task_file.exists() and item.get("task_count") == len(rows),
                  f"file={task_file} count={item.get('task_count')} rows={len(rows)}")

    if testset_index_path.exists() and testset_modules_path.exists():
        testset_index = load_json(testset_index_path)
        modules_payload = load_json(testset_modules_path)
        module_refs = modules_payload.get("module_refs", [])
        module_lookup = modules_payload.get("module_lookup", {})
        check("testset task_count matches tasks",
              testset_index.get("task_count") == len(tasks),
              f"index={testset_index.get('task_count')} rows={len(tasks)}")
        check("testset module_count matches modules",
              testset_index.get("module_count") == modules_payload.get("module_count") == len(module_refs) == len(module_lookup),
              f"index={testset_index.get('module_count')} manifest={modules_payload.get('module_count')} refs={len(module_refs)} lookup={len(module_lookup)}")
        missing_shards = []
        for shard_info in modules_payload.get("module_shards", {}).values():
            shard = pathlib.Path(str(shard_info.get("file", ""))) if isinstance(shard_info, dict) else pathlib.Path(str(shard_info))
            if not shard.exists():
                missing_shards.append(str(shard))
        check("all split testset module shards exist", not missing_shards,
              f"missing={missing_shards[:5]}")


def main() -> int:
    tasks = aeb.load_jsonl(aeb.DEFAULT_TASKS)
    manifest = load_json(aeb.DEFAULT_MANIFEST)

    test_manifest_matches_tasks(tasks, manifest)
    test_task_shape(tasks)
    test_high_error_recovery_coverage(tasks)
    test_asset_type_and_testset_indexes(tasks, manifest)

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
