#!/usr/bin/env python3
"""Offline unit tests for ActionGuidance ROI items A1-2 and A1-3.

These tests run with no live editor and no network. They exercise:

  * ITEM 1 (needed_action_routing): score_task on fabricated routing responses,
    asserting a candidate-bearing monolith_find response that names the real
    action scores HIGH, while a bare no-candidate monolith_discover error scores
    LOW.
  * ITEM 2 (demand weighting): action_weight / task_weight and the weighted
    aggregate, asserting that weighting by live invocation volume x error cost
    changes the aggregate in the expected direction.

MCP responses use the documented envelope shape
    {"result": {"content": [{"type": "text", "text": <json>}], "isError": <bool>}}

Run:
    python Plugins/Monolith/Scripts/tests/test_action_guidance_routing_weight.py
"""

from __future__ import annotations

import json
import pathlib
import sys
import tempfile
import unittest
from unittest import mock

SCRIPTS_DIR = pathlib.Path(__file__).resolve().parent.parent
if str(SCRIPTS_DIR) not in sys.path:
    sys.path.insert(0, str(SCRIPTS_DIR))

import action_guidance_benchmark as agb  # noqa: E402


def mcp_envelope(payload: dict, *, is_error: bool = False) -> dict:
    """Fabricated MCP tools/call response of the documented shape."""
    return {
        "result": {
            "content": [{"type": "text", "text": json.dumps(payload)}],
            "isError": is_error,
        }
    }


# --- Fabricated routing responses -------------------------------------------

# A healthy monolith_find response: matches[] name the real action_id the agent
# needs. This is the candidate-bearing response that must score HIGH.
FIND_NAMES_ACTION = {
    "status": "ok",
    "query": "get function signature for a C++ symbol",
    "count": 2,
    "matches": [
        {"action_id": "source.get_signature", "namespace": "source", "action": "get_signature", "score": 210},
        {"action_id": "source.find_callers", "namespace": "source", "action": "find_callers", "score": 80},
    ],
}

# A bare monolith_find response with no useful candidates (empty matches). This is
# the no-candidate failure mode that must score LOW.
FIND_NO_CANDIDATES = {
    "status": "ok",
    "query": "get function signature for a C++ symbol",
    "count": 0,
    "matches": [],
}

# A monolith_discover error for an absent action that DOES carry a routing hint
# (candidate_actions name the real action). Must score HIGH.
DISCOVER_WITH_HINT = {
    "failure_cause": "needed_action",
    "candidate_actions": [
        {"action_id": "source.get_signature", "namespace": "source", "action": "get_signature"},
    ],
}

# A bare monolith_discover "Unknown action" error: no candidates anywhere. This is
# the exact live pain (dead-ends the agent). Must score LOW.
DISCOVER_BARE_ERROR = {
    "error": {"code": -32602, "message": "Unknown action: source.get_function_signature"},
}


class _PatchedMCP:
    """Context manager that makes agb.mcp_call return one fixed envelope."""

    def __init__(self, payload: dict, *, is_error: bool = False):
        self._envelope = mcp_envelope(payload, is_error=is_error)
        self._original = None

    def __enter__(self):
        self._original = agb.mcp_call

        def fake_mcp_call(url, tool, arguments, timeout_s=8.0):
            return self._envelope

        agb.mcp_call = fake_mcp_call
        return self

    def __exit__(self, *exc):
        agb.mcp_call = self._original
        return False


def routing_task(tool: str, expected_action_id: str, subtype: str) -> dict:
    namespace, action = expected_action_id.split(".", 1)
    return {
        "id": "AGB-T",
        "category": "needed_action_routing",
        "namespace": namespace,
        "action": action,
        "tool": tool,
        "arguments": {"query": "x"} if tool == "monolith_find" else {"namespace": namespace, "action": "typo"},
        "expected": {
            "candidate_action": expected_action_id,
            "routing_subtype": subtype,
            "failure_cause": "needed_action",
        },
    }


class RoutingScoringTests(unittest.TestCase):
    def test_find_naming_real_action_scores_high(self):
        task = routing_task("monolith_find", "source.get_signature", "find")
        with _PatchedMCP(FIND_NAMES_ACTION):
            row = agb.score_task("http://x", task, max_recovery_calls=3, timeout_s=1.0)
        self.assertTrue(row["direct_success"])
        self.assertTrue(row["task_success"])
        self.assertEqual(row["action_selection_score"], 1.0)
        self.assertEqual(row["tool_calls_to_success"], 1)

    def test_find_with_no_candidates_scores_low(self):
        task = routing_task("monolith_find", "source.get_signature", "find")
        # The recovery fallback re-issues monolith_find; with the patched call it
        # also returns no candidates, so the task stays a hard failure.
        with _PatchedMCP(FIND_NO_CANDIDATES):
            row = agb.score_task("http://x", task, max_recovery_calls=3, timeout_s=1.0)
        self.assertFalse(row["direct_success"])
        self.assertFalse(row["task_success"])
        self.assertEqual(row["action_selection_score"], 0.0)

    def test_recovery_transport_failure_is_reported_on_task_row(self):
        task = routing_task("monolith_find", "source.get_signature", "find")
        with mock.patch.object(
            agb,
            "mcp_call",
            side_effect=[
                mcp_envelope(FIND_NO_CANDIDATES),
                {"transport_error": True, "status": 503, "raw": "recovery down"},
            ],
        ):
            row = agb.score_task("http://x", task, max_recovery_calls=3, timeout_s=1.0)
        self.assertTrue(row["transport_error"])
        self.assertEqual(row["transport_status"], 503)
        self.assertEqual(row["transport_error_raw"], "recovery down")
        self.assertEqual(row["transport_failure_call_count"], 1)
        self.assertEqual(row["last_transport_tool"], "monolith_find")

    def test_discover_with_routing_hint_scores_high(self):
        task = routing_task("monolith_discover", "source.get_signature", "discover_unknown_action")
        with _PatchedMCP(DISCOVER_WITH_HINT):
            row = agb.score_task("http://x", task, max_recovery_calls=3, timeout_s=1.0)
        self.assertTrue(row["direct_success"])
        self.assertEqual(row["action_selection_score"], 1.0)

    def test_discover_bare_unknown_action_error_scores_low(self):
        task = routing_task("monolith_discover", "source.get_signature", "discover_unknown_action")
        with _PatchedMCP(DISCOVER_BARE_ERROR, is_error=True):
            row = agb.score_task("http://x", task, max_recovery_calls=3, timeout_s=1.0)
        self.assertFalse(row["direct_success"])
        self.assertFalse(row["task_success"])
        self.assertEqual(row["action_selection_score"], 0.0)


class ReadOnlyPolicyScoringTests(unittest.TestCase):
    @staticmethod
    def _task() -> dict:
        return {
            "id": "AGB-SC-POLICY",
            "category": "discovery_planning",
            "namespace": "source_control",
            "action": "map_depot_paths",
            "tool": "monolith_discover",
            "arguments": {"namespace": "source_control", "action": "map_depot_paths", "mode": "schema"},
            "expected": {
                "requires_planning_signals": True,
                "execution_policy_id": "read_only",
                "execution_policy_defaulted": False,
                "mutates_assets": False,
            },
        }

    @staticmethod
    def _schema(policy_id: str, *, defaulted: bool, mutates_assets: bool) -> dict:
        return {
            "schema": {
                "action": "map_depot_paths",
                "skill": "unreal-source-control",
                "planning_signals": [{"kind": "skill"}],
                "output_contract_status": "declared",
                "next_actions_status": "declared",
                "mutates_assets": mutates_assets,
                "execution_policy": {
                    "policy_id": policy_id,
                    "defaulted": defaulted,
                    "dirty_package_tracking": mutates_assets,
                    "transaction_wrapping": mutates_assets,
                    "post_edit_validation": False,
                },
            }
        }

    def test_explicit_read_only_policy_scores_high(self):
        with _PatchedMCP(self._schema("read_only", defaulted=False, mutates_assets=False)):
            row = agb.score_task("http://x", self._task(), max_recovery_calls=3, timeout_s=1.0)
        self.assertTrue(row["direct_success"])
        self.assertTrue(row["evidence"]["policy_ok"])

    def test_inferred_mutation_policy_scores_low(self):
        with _PatchedMCP(self._schema("transaction_optional", defaulted=True, mutates_assets=True)):
            row = agb.score_task("http://x", self._task(), max_recovery_calls=3, timeout_s=1.0)
        self.assertFalse(row["direct_success"])
        self.assertFalse(row["task_success"])
        self.assertFalse(row["evidence"]["policy_ok"])

class WeightTests(unittest.TestCase):
    def test_normalized_tool_calls_reaches_one_at_runtime_maximum(self):
        task = {
            "id": "norm",
            "category": "discovery_planning",
            "namespace": "source",
            "action": "read_file",
        }
        row = {
            "category": "discovery_planning",
            "weight": 1.0,
            "task_success": False,
            "direct_success": False,
            "tool_calls_to_success": 3,
            "action_selection_score": 0.0,
            "param_correction_score": None,
            "hallucinated_workflow_risk": 1.0,
        }
        aggregate = agb.aggregate("normalization", {}, [task], [row], max_recovery_calls=3)
        self.assertEqual(aggregate["metrics"]["normalized_tool_calls"], 1.0)

    def test_high_volume_high_error_action_outweighs_dead_action(self):
        # create_blueprint (101 calls, 0.871 err) must outweigh a dead action.
        heavy = agb.action_weight("blueprint", "create_blueprint")
        dead = agb.action_weight("nonexistent_ns", "nonexistent_action")
        self.assertEqual(dead, agb.DEFAULT_WEIGHT)
        self.assertGreater(heavy, dead)
        # add_variable (294*0.466 ~= 137 error mass) should outweigh a clean,
        # low-error action of similar call volume.
        add_var = agb.action_weight("blueprint", "add_variable")
        clean = agb.action_weight("blueprint", "get_functions")  # 223 calls, 0.018 err
        self.assertGreater(add_var, clean)

    def test_task_weight_honors_explicit_field(self):
        task = {"namespace": "blueprint", "action": "create_blueprint", "weight": 5.0}
        self.assertEqual(agb.task_weight(task), 5.0)
        # Without an explicit weight it derives from the demand table.
        task2 = {"namespace": "blueprint", "action": "create_blueprint"}
        self.assertGreater(agb.task_weight(task2), agb.DEFAULT_WEIGHT)

    def _row(self, namespace, action, success):
        return {
            "task_id": f"{namespace}.{action}",
            "category": "needed_action_routing",
            "namespace": namespace,
            "action": action,
            "weight": agb.action_weight(namespace, action),
            "direct_success": success,
            "task_success": success,
            "tool_calls_to_success": 1 if success else 3,
            "action_selection_score": 1.0 if success else 0.0,
            "param_correction_score": None,
            "hallucinated_workflow_risk": None,
        }

    def test_weighting_changes_aggregate_when_heavy_action_fails(self):
        # Scenario A: the heavy (high-demand) action SUCCEEDS, a dead action FAILS.
        # Scenario B: the heavy action FAILS, the dead action SUCCEEDS.
        # Pass count is identical (1/2) in both, so an UNWEIGHTED mean would be
        # equal. The weighted mean must score B materially lower because the
        # failing action carries far more live demand.
        rows_a = [
            self._row("blueprint", "create_blueprint", True),   # heavy, pass
            self._row("nonexistent_ns", "dead_action", False),  # dead, fail
        ]
        rows_b = [
            self._row("blueprint", "create_blueprint", False),  # heavy, fail
            self._row("nonexistent_ns", "dead_action", True),   # dead, pass
        ]
        tasks_a = [{"namespace": r["namespace"]} for r in rows_a]
        tasks_b = [{"namespace": r["namespace"]} for r in rows_b]
        agg_a = agb.aggregate("a", {}, tasks_a, rows_a, max_recovery_calls=3)
        agg_b = agb.aggregate("b", {}, tasks_b, rows_b, max_recovery_calls=3)

        # Unweighted task_success_rate would be 0.5 for both; weighted is not.
        sr_a = agg_a["metrics"]["task_success_rate"]
        sr_b = agg_b["metrics"]["task_success_rate"]
        self.assertGreater(sr_a, sr_b)
        # Effectiveness follows the same direction.
        self.assertGreater(
            agg_a["metrics"]["effectiveness_score"],
            agg_b["metrics"]["effectiveness_score"],
        )

    def test_weighted_avg_falls_back_to_unweighted_on_zero_mass(self):
        self.assertEqual(agb.weighted_avg([(1.0, 0.0), (0.0, 0.0)]), 0.5)
        self.assertEqual(agb.weighted_avg([]), 0.0)

    def _param_row(self, namespace, action, success):
        row = self._row(namespace, action, success)
        row["category"] = "missing_required_param"
        row["action_selection_score"] = None
        row["param_correction_score"] = 1.0 if success else 0.0
        return row

    def _planning_row(self, namespace, action, success):
        row = self._row(namespace, action, success)
        row["category"] = "discovery_planning"
        row["param_correction_score"] = None
        row["hallucinated_workflow_risk"] = 0.0 if success else 1.0
        return row

    def test_effectiveness_component_weights_sum_to_one(self):
        # Component weights (0.30/0.20/0.15/0.15/0.10/0.10) must still sum to 1.0
        # after the weighting change — only the per-task averaging is weighted.
        self.assertEqual(round(0.30 + 0.20 + 0.15 + 0.15 + 0.10 + 0.10, 6), 1.0)
        # A run that satisfies every component (routing + param + planning, all
        # passing) scores exactly 1.0, proving the components form a convex
        # combination whose weights sum to 1.0 even under demand weighting.
        rows = [
            self._row("blueprint", "create_blueprint", True),       # routing/action + success
            self._param_row("blueprint", "add_variable", True),     # param correction
            self._planning_row("source", "get_signature", True),    # planning + no hallucination
        ]
        tasks = [{"namespace": r["namespace"]} for r in rows]
        agg = agb.aggregate("p", {}, tasks, rows, max_recovery_calls=3)
        self.assertEqual(agg["metrics"]["effectiveness_score"], 1.0)


class GenerationTests(unittest.TestCase):
    def test_generation_fingerprint_rejects_status_discover_version_mismatch(self):
        def fake_mcp_call(url, tool, arguments, timeout_s=45.0):
            if tool == "monolith_status":
                return mcp_envelope({
                    "server_running": True,
                    "catalog_version": "sha256:v1",
                    "catalog_action_count": 1,
                    "catalog_namespace_count": 1,
                })
            return mcp_envelope({
                "catalog_version": "sha256:v2",
                "total_actions": 1,
                "namespaces": [{"namespace": "source", "action_count": 1}],
            })

        with mock.patch.object(agb, "mcp_call", side_effect=fake_mcp_call):
            with self.assertRaisesRegex(RuntimeError, "catalog version mismatch"):
                agb.read_generation_catalog_fingerprint("http://unused")

    def test_compact_structured_summary_enumerates_all_action_pages(self):
        calls = []

        def fake_mcp_call(url, tool, arguments, timeout_s=45.0):
            calls.append(dict(arguments))
            if arguments.get("mode") == "summary":
                payload = {
                    "total_actions": 3,
                    "namespaces": [{"namespace": "source_control", "action_count": 3}],
                }
            elif arguments.get("mode") == "actions" and arguments.get("offset") == 0:
                payload = {
                    "actions": [{"action": "list_opened"}, {"action": "map_depot_paths"}],
                    "truncated": True,
                    "next_offset": 2,
                }
            elif arguments.get("mode") == "actions" and arguments.get("offset") == 2:
                payload = {
                    "actions": [{"action": "status"}],
                    "truncated": False,
                }
            else:
                self.fail(f"unexpected discovery call: {arguments}")
            return {
                "result": {
                    "content": [{"type": "text", "text": "compact discovery response"}],
                    "structuredContent": payload,
                }
            }

        with mock.patch.object(agb, "mcp_call", side_effect=fake_mcp_call):
            rows = agb.discover_catalog_namespaces("http://unused")

        self.assertEqual(rows[0]["actions"], ["list_opened", "map_depot_paths", "status"])
        self.assertEqual(calls[0], {"mode": "summary", "limit": 1000})
        self.assertEqual([call["offset"] for call in calls[1:]], [0, 2])

    def test_compact_summary_action_count_mismatch_fails_fast(self):
        def fake_mcp_call(url, tool, arguments, timeout_s=45.0):
            if arguments.get("mode") == "summary":
                payload = {
                    "total_actions": 2,
                    "namespaces": [{"namespace": "source_control", "action_count": 2}],
                }
            else:
                payload = {"actions": [{"action": "list_opened"}], "truncated": False}
            return {"result": {"structuredContent": payload}}

        with mock.patch.object(agb, "mcp_call", side_effect=fake_mcp_call):
            with self.assertRaisesRegex(RuntimeError, "action count mismatch"):
                agb.discover_catalog_namespaces("http://unused")

    def test_static_supplement_builds_routing_tasks_with_weights(self):
        tasks: list = []
        agb.append_static_unreal_practical_tasks(tasks)
        routing = [t for t in tasks if t["category"] == "needed_action_routing"]
        self.assertEqual(len(routing), len(agb._STATIC_NEEDED_ACTION_ROUTING_TASKS_20260618))
        # Every routing task names a real expected action_id and uses a routing tool.
        for t in routing:
            self.assertIn(t["tool"], ("monolith_find", "monolith_discover"))
            self.assertIn(".", t["expected"]["candidate_action"])
        # The supplement does not stamp weights itself (generate_tasks does), but
        # task_weight resolves a demand weight on the fly for heavy targets.
        sig = next(t for t in routing if t["action"] == "get_signature")
        self.assertGreater(agb.task_weight(sig), agb.DEFAULT_WEIGHT)

    def test_static_supplement_contains_explicit_source_control_policy_tasks(self):
        tasks: list = []
        agb.append_static_unreal_practical_tasks(tasks)
        policy_tasks = {
            f"{task['namespace']}.{task['action']}": task
            for task in tasks
            if task.get("expected", {}).get("execution_policy_id") == "read_only"
        }
        self.assertEqual(set(policy_tasks), {
            "source_control.list_opened",
            "source_control.map_depot_paths",
        })
        for task in policy_tasks.values():
            self.assertFalse(task["expected"]["execution_policy_defaulted"])
            self.assertFalse(task["expected"]["mutates_assets"])

    def test_generate_refuses_catalog_drift_without_overwriting_outputs(self):
        start = {
            "catalog_version": "sha256:v1",
            "catalog_action_count": 1,
            "catalog_namespace_count": 1,
        }
        end = dict(start, catalog_version="sha256:v2")
        namespaces = [{"namespace": "source", "actions": ["read_file"]}]
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            tasks_path = root / "tasks.jsonl"
            manifest_path = root / "manifest.json"
            tasks_path.write_text("sentinel tasks\n", encoding="utf-8")
            manifest_path.write_text('{"sentinel":true}\n', encoding="utf-8")
            with mock.patch.object(
                agb,
                "read_generation_catalog_fingerprint",
                side_effect=[start, end],
            ), mock.patch.object(
                agb,
                "discover_catalog_namespaces",
                return_value=namespaces,
            ), mock.patch.object(agb, "discover_schema", return_value=None):
                with self.assertRaisesRegex(RuntimeError, "refusing to overwrite"):
                    agb.generate_tasks(
                        "http://unused",
                        1,
                        tasks_path,
                        manifest_path,
                    )
            self.assertEqual(tasks_path.read_text(encoding="utf-8"), "sentinel tasks\n")
            self.assertEqual(
                manifest_path.read_text(encoding="utf-8"),
                '{"sentinel":true}\n',
            )

    def test_generate_records_stable_catalog_version(self):
        fingerprint = {
            "catalog_version": "sha256:stable",
            "catalog_action_count": 1,
            "catalog_namespace_count": 1,
        }
        namespaces = [{"namespace": "source", "actions": ["read_file"]}]
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            with mock.patch.object(
                agb,
                "read_generation_catalog_fingerprint",
                side_effect=[fingerprint, dict(fingerprint)],
            ), mock.patch.object(
                agb,
                "discover_catalog_namespaces",
                return_value=namespaces,
            ), mock.patch.object(agb, "discover_schema", return_value=None):
                manifest = agb.generate_tasks(
                    "http://unused",
                    1,
                    root / "tasks.jsonl",
                    root / "manifest.json",
                )
            self.assertEqual(manifest["catalog_version"], "sha256:stable")
            self.assertEqual(
                json.loads((root / "manifest.json").read_text(encoding="utf-8"))[
                    "catalog_version"
                ],
                "sha256:stable",
            )


class TransportGateTests(unittest.TestCase):
    def test_task_protocol_failure_aborts_on_the_triggering_call(self):
        task = {
            "id": "AGB-T-PROTOCOL",
            "category": "discovery_planning",
            "namespace": "source",
            "action": "read_file",
            "tool": "monolith_discover",
            "arguments": {"namespace": "source", "action": "read_file", "mode": "schema"},
            "expected": {},
        }
        calls = []

        def fake_call(url, tool, arguments, timeout_s=45.0):
            calls.append(tool)
            if tool == "monolith_status":
                return mcp_envelope({"server_running": True, "catalog_version": "sha256:v1"})
            return []

        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            tasks_path = root / "tasks.jsonl"
            tasks_path.write_text(json.dumps(task) + "\n", encoding="utf-8")
            output_dir = root / "run"
            with mock.patch.object(agb, "mcp_call", side_effect=fake_call):
                result = agb.run_benchmark(
                    "http://unused", tasks_path, output_dir, "task-protocol", 3, 1.0,
                    allow_subset=True,
                )
            self.assertEqual(calls, ["monolith_status", "monolith_discover"])
            self.assertEqual(result["completion_status"], "aborted_protocol_failure")
            self.assertEqual(result["completed_task_count"], 1)
            row = json.loads((output_dir / "per_task.jsonl").read_text(encoding="utf-8"))
            self.assertEqual(row["failure_kind"], "protocol_error")
            self.assertEqual(row["last_protocol_tool"], "monolith_discover")
            self.assertFalse((output_dir / "summary.json").exists())

    def test_status_identity_drift_invalidates_a_complete_subset(self):
        task = {
            "id": "AGB-T-IDENTITY",
            "category": "discovery_planning",
            "namespace": "source",
            "action": "read_file",
            "tool": "monolith_discover",
            "arguments": {},
            "expected": {},
        }
        healthy_row = {
            "task_id": task["id"],
            "category": task["category"],
            "namespace": task["namespace"],
            "action": task["action"],
            "weight": 1.0,
            "direct_success": True,
            "task_success": True,
            "tool_calls_to_success": 1,
            "action_selection_score": 1.0,
            "param_correction_score": None,
            "hallucinated_workflow_risk": 0.0,
            "evidence": {},
            "transport_error": False,
            "transport_status": None,
            "transport_error_raw": "",
            "response_is_error": False,
            "response_text": "",
        }
        statuses = iter([
            mcp_envelope({"server_running": True, "catalog_version": "sha256:v1", "editor_pid": 10}),
            mcp_envelope({"server_running": True, "catalog_version": "sha256:v2", "editor_pid": 10}),
        ])
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            tasks_path = root / "tasks.jsonl"
            tasks_path.write_text(json.dumps(task) + "\n", encoding="utf-8")
            output_dir = root / "run"
            with mock.patch.object(agb, "mcp_call", side_effect=lambda *args, **kwargs: next(statuses)), \
                    mock.patch.object(agb, "score_task", return_value=healthy_row):
                result = agb.run_benchmark(
                    "http://unused", tasks_path, output_dir, "identity-drift", 3, 1.0,
                    allow_subset=True,
                )
            self.assertEqual(result["completion_status"], "aborted_status_identity_drift")
            self.assertIn("catalog_version", result["status_identity_mismatches"])
            self.assertFalse(result["comparison_valid"])
            self.assertFalse((output_dir / "summary.json").exists())

    def test_three_consecutive_transport_failures_abort_without_summary(self):
        tasks = [
            {
                "id": f"AGB-T-{index}",
                "category": "discovery_planning",
                "namespace": "source_control",
                "action": "list_opened",
                "tool": "monolith_discover",
                "arguments": {},
                "expected": {},
                "weight": 1.0,
            }
            for index in range(1, 7)
        ]
        calls = []

        def fake_score(url, task, max_recovery_calls, timeout_s):
            calls.append(task["id"])
            return {
                "task_id": task["id"],
                "category": task["category"],
                "namespace": task["namespace"],
                "action": task["action"],
                "weight": 1.0,
                "direct_success": False,
                "task_success": False,
                "tool_calls_to_success": max_recovery_calls,
                "action_selection_score": 0.0,
                "param_correction_score": None,
                "hallucinated_workflow_risk": 1.0,
                "evidence": {},
                "transport_error": True,
                "transport_status": None,
                "transport_error_raw": "",
                "response_is_error": False,
                "response_text": "",
            }

        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            tasks_path = root / "tasks.jsonl"
            tasks_path.write_text(
                "".join(json.dumps(task) + "\n" for task in tasks),
                encoding="utf-8",
            )
            output_dir = root / "run"
            with mock.patch.object(agb, "mcp_call", return_value=mcp_envelope({"server_running": True})):
                with mock.patch.object(agb, "score_task", side_effect=fake_score):
                    result = agb.run_benchmark(
                        "http://unused",
                        tasks_path,
                        output_dir,
                        "transport-gate",
                        3,
                        1.0,
                        allow_subset=True,
                    )

            self.assertFalse(result["run_valid"])
            self.assertEqual(result["completion_status"], "aborted_transport_failure_budget")
            self.assertEqual(result["completed_task_count"], 3)
            self.assertEqual(calls, ["AGB-T-1", "AGB-T-2", "AGB-T-3"])
            self.assertTrue((output_dir / "run_failure.json").exists())
            self.assertFalse((output_dir / "summary.json").exists())

    def test_status_protocol_error_aborts_without_scoring_or_summary(self):
        task = {
            "id": "AGB-T-1",
            "category": "discovery_planning",
            "namespace": "source",
            "action": "read_file",
            "tool": "monolith_discover",
            "arguments": {},
            "expected": {},
            "weight": 1.0,
        }
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            tasks_path = root / "tasks.jsonl"
            tasks_path.write_text(json.dumps(task) + "\n", encoding="utf-8")
            output_dir = root / "run"
            with mock.patch.object(
                agb,
                "mcp_call",
                return_value={"parse_error": True, "raw": "not-json"},
            ), mock.patch.object(agb, "score_task") as score:
                result = agb.run_benchmark(
                    "http://unused", tasks_path, output_dir, "status-protocol", 3, 1.0,
                    allow_subset=True,
                )
            score.assert_not_called()
            self.assertFalse(result["run_valid"])
            self.assertEqual(result["failure_kind"], "protocol_error")
            self.assertFalse(result["metrics_valid"])
            self.assertTrue((output_dir / "run_failure.json").exists())
            self.assertTrue((output_dir / "partial_summary.json").exists())
            self.assertFalse((output_dir / "summary.json").exists())

    def test_task_scoring_exception_preserves_trigger_row_and_invalid_artifacts(self):
        task = {
            "id": "AGB-T-1",
            "category": "discovery_planning",
            "namespace": "source",
            "action": "read_file",
            "tool": "monolith_discover",
            "arguments": {},
            "expected": {},
            "weight": 1.0,
        }
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            tasks_path = root / "tasks.jsonl"
            tasks_path.write_text(json.dumps(task) + "\n", encoding="utf-8")
            output_dir = root / "run"
            with mock.patch.object(
                agb,
                "mcp_call",
                return_value=mcp_envelope({"server_running": True}),
            ), mock.patch.object(
                agb,
                "score_task",
                side_effect=RuntimeError("score exploded"),
            ):
                result = agb.run_benchmark(
                    "http://unused", tasks_path, output_dir, "runner-error", 3, 1.0,
                    allow_subset=True,
                )
            row = json.loads(
                (output_dir / "per_task.jsonl").read_text(encoding="utf-8").strip()
            )
            partial = json.loads(
                (output_dir / "partial_summary.json").read_text(encoding="utf-8")
            )
            self.assertEqual(result["completion_status"], "aborted_runner_exception")
            self.assertFalse(result["metrics_valid"])
            self.assertFalse(partial["metrics_valid"])
            self.assertEqual(row["failure_kind"], "runner_exception")
            self.assertIn("score exploded", row["error"])
            self.assertFalse((output_dir / "summary.json").exists())

    def test_short_population_transport_fraction_is_rejected_at_finalize(self):
        tasks = [
            {
                "id": f"AGB-T-{index}",
                "category": "discovery_planning",
                "namespace": "source",
                "action": "read_file",
                "tool": "monolith_discover",
                "arguments": {},
                "expected": {},
                "weight": 1.0,
            }
            for index in range(1, 11)
        ]
        calls = 0

        def fake_score(url, task, max_recovery_calls, timeout_s):
            nonlocal calls
            calls += 1
            transport = calls == 1
            return {
                "task_id": task["id"],
                "category": task["category"],
                "namespace": task["namespace"],
                "action": task["action"],
                "weight": 1.0,
                "direct_success": not transport,
                "task_success": not transport,
                "tool_calls_to_success": 1,
                "action_selection_score": 0.0 if transport else 1.0,
                "param_correction_score": None,
                "hallucinated_workflow_risk": 1.0 if transport else 0.0,
                "evidence": {},
                "transport_error": transport,
                "transport_status": 503 if transport else None,
                "transport_error_raw": "down" if transport else "",
                "response_is_error": False,
                "response_text": "",
            }

        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            tasks_path = root / "tasks.jsonl"
            tasks_path.write_text(
                "".join(json.dumps(task) + "\n" for task in tasks),
                encoding="utf-8",
            )
            output_dir = root / "run"
            with mock.patch.object(
                agb,
                "mcp_call",
                return_value=mcp_envelope({"server_running": True}),
            ), mock.patch.object(agb, "score_task", side_effect=fake_score):
                result = agb.run_benchmark(
                    "http://unused",
                    tasks_path,
                    output_dir,
                    "short-finalize",
                    3,
                    1.0,
                    max_consecutive_transport_failures=20,
                    min_transport_fraction_sample=20,
                    allow_subset=True,
                )
            self.assertFalse(result["run_valid"])
            self.assertEqual(
                result["transport_gate_reason"], "final_transport_failed_fraction"
            )
            self.assertEqual(result["last_task_id"], "AGB-T-1")
            self.assertEqual(result["last_transport_status"], 503)
            self.assertFalse((output_dir / "summary.json").exists())

    def test_success_removes_in_progress_partial_summary(self):
        task = {
            "id": "AGB-T-1",
            "category": "discovery_planning",
            "namespace": "source",
            "action": "read_file",
            "tool": "monolith_discover",
            "arguments": {},
            "expected": {},
            "weight": 1.0,
        }
        healthy_row = {
            "task_id": task["id"],
            "category": task["category"],
            "namespace": task["namespace"],
            "action": task["action"],
            "weight": 1.0,
            "direct_success": True,
            "task_success": True,
            "tool_calls_to_success": 1,
            "action_selection_score": 1.0,
            "param_correction_score": None,
            "hallucinated_workflow_risk": 0.0,
            "evidence": {},
            "transport_error": False,
            "transport_status": None,
            "transport_error_raw": "",
            "response_is_error": False,
            "response_text": "",
        }
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            tasks_path = root / "tasks.jsonl"
            tasks_path.write_text(json.dumps(task) + "\n", encoding="utf-8")
            output_dir = root / "run"
            with mock.patch.object(
                agb,
                "mcp_call",
                return_value=mcp_envelope({"server_running": True}),
            ), mock.patch.object(agb, "score_task", return_value=healthy_row):
                result = agb.run_benchmark(
                    "http://unused", tasks_path, output_dir, "success", 3, 1.0,
                    allow_subset=True,
                )
            self.assertTrue(result["run_valid"])
            self.assertTrue((output_dir / "summary.json").exists())
            self.assertFalse((output_dir / "partial_summary.json").exists())


if __name__ == "__main__":
    unittest.main(verbosity=2)
