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
import unittest

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


class WeightTests(unittest.TestCase):
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


if __name__ == "__main__":
    unittest.main(verbosity=2)
