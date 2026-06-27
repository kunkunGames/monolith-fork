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
        repeated_asset_type_cases: List[str] = []
        for item in asset_types:
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
            shard = aeb.resolve_plugin_path(shard)
            if not shard.exists():
                missing_shards.append(str(shard))
        check("all split testset module shards exist", not missing_shards,
              f"missing={missing_shards[:5]}")

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


def main() -> int:
    tasks = aeb.load_jsonl(aeb.resolve_plugin_path(aeb.DEFAULT_TASKS))
    manifest = load_json(aeb.DEFAULT_MANIFEST)

    test_manifest_matches_tasks(tasks, manifest)
    test_task_shape(tasks)
    test_high_error_recovery_coverage(tasks)
    test_asset_type_and_testset_indexes(tasks, manifest)
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
