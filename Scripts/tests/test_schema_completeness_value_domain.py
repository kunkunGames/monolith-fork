#!/usr/bin/env python3
"""Offline unit tests for the SchemaCompleteness value_domain dimension.

These tests run with no live editor and no network. They exercise the new
scoring branches added for ROI report A4 items 1 (value_domain) and 2 (closing
the vacuous-true holes) by:

  * calling score_schema_quality directly on fabricated discover schemas, and
  * driving discover_schema_for_action through a monkeypatched mcp_call that
    returns fabricated MCP envelopes of the documented shape
        {"result": {"content": [{"type": "text", "text": <json>}],
                    "isError": <bool>}}

Run:
    python Plugins/Monolith/Scripts/tests/test_schema_completeness_value_domain.py
"""

from __future__ import annotations

import json
import pathlib
import sys
import unittest

SCRIPTS_DIR = pathlib.Path(__file__).resolve().parent.parent
if str(SCRIPTS_DIR) not in sys.path:
    sys.path.insert(0, str(SCRIPTS_DIR))

import schema_completeness_benchmark as scb  # noqa: E402


def mcp_envelope(payload: dict, *, is_error: bool = False) -> dict:
    """Build a fabricated MCP tools/call response of the documented shape.

    The discover transport wraps the JSON schema as text in
    result.content[0].text; discover_schema_for_action then reads the top-level
    "schema" key. So callers pass {"schema": {...}} as payload.
    """
    return {
        "result": {
            "content": [{"type": "text", "text": json.dumps(payload)}],
            "isError": is_error,
        }
    }


# --- Fabricated schemas -----------------------------------------------------

# A fully-specified action: every param typed + described + required flag set,
# the constrained "mode" param documents its enum, the numeric "limit" param
# documents its range. Top-level planning/skill/output contract all present.
HEALTHY_SCHEMA = {
    "skill": "unreal-cpp",
    "planning_signals": ["read_first", "then_edit"],
    "output_contract_status": "declared",
    "params": {
        "_validate_types": True,  # internal control flag, must be ignored
        "mode": {
            "type": "string",
            "description": "Discovery mode selector",
            "required": True,
            "enum": ["summary", "actions", "schema"],
        },
        "limit": {
            "type": "integer",
            "description": "Maximum number of results to return",
            "required": False,
            "minimum": 1,
            "maximum": 100,
        },
    },
}

# The NEW failure mode: a required param that is untyped AND undescribed.
# Under the old benchmark this still scored param_types_declared/required True
# as long as some other param had a type / was required. value_domain must FAIL.
UNTYPED_UNDESCRIBED_REQUIRED_SCHEMA = {
    "skill": "unreal-blueprints",
    "planning_signals": ["create_first"],
    "output_contract_status": "declared",
    "params": {
        # Required param with no type and no description — exactly the wrong /
        # undocumented param contract that drives production failures.
        "variable_name": {
            "required": True,
        },
        "default_value": {
            "type": "string",
            "description": "Initial value for the variable",
            "required": False,
        },
    },
}

# A constrained param (enum) that does not actually document its allowed values:
# the enum list is empty. value_domain must FAIL.
EMPTY_ENUM_SCHEMA = {
    "skill": "unreal-ui",
    "planning_signals": ["x"],
    "output_contract_status": "declared",
    "params": {
        "alignment": {
            "type": "string",
            "description": "Widget alignment",
            "required": True,
            "enum": [],
        },
    },
}

# A numeric param that declares no range and no enum. value_domain must FAIL.
UNBOUNDED_NUMERIC_SCHEMA = {
    "skill": "unreal-scene",
    "planning_signals": ["x"],
    "output_contract_status": "declared",
    "params": {
        "radius": {
            "type": "number",
            "description": "Search radius",
            "required": True,
        },
    },
}

# A mixed representation union is not a numeric-only domain.  The numeric
# spelling is one accepted representation alongside an array, string, and
# object, so a single numeric minimum/maximum cannot describe the whole input
# contract.  This is the live imagegen resolution shape.
MIXED_REPRESENTATION_UNION_SCHEMA = {
    "skill": "unreal-imagegen",
    "planning_signals": ["x"],
    "output_contract_status": "declared",
    "params": {
        "resolution": {
            "type": "array|string|object|number",
            "description": "Image resolution in one of the documented representations",
            "required": False,
        },
    },
}

# Nullable numeric input remains numeric-domain constrained: null is absence,
# not an alternate non-numeric representation.
UNBOUNDED_NULLABLE_NUMERIC_SCHEMA = {
    "skill": "unreal-scene",
    "planning_signals": ["x"],
    "output_contract_status": "declared",
    "params": {
        "radius": {
            "type": "number|null",
            "description": "Optional search radius",
            "required": False,
        },
    },
}

# A param-less action (no "params" key at all). The three param-gated dimensions
# must be N/A (None), not auto-1.0.
PARAMLESS_SCHEMA = {
    "skill": "unreal-scene",
    "planning_signals": ["x"],
    "output_contract_status": "not_declared",
}


def schema_with_param(meta: dict, *, name: str = "value") -> dict:
    return {
        "skill": "unreal-cpp",
        "planning_signals": ["x"],
        "output_contract_status": "declared",
        "params": {name: meta},
    }


def numeric_param(**extra) -> dict:
    meta = {
        "type": "number",
        "description": "Numeric test value",
        "required": False,
    }
    meta.update(extra)
    return meta


class ValueDomainScoringTests(unittest.TestCase):
    def test_healthy_schema_passes_value_domain(self):
        q = scb.score_schema_quality(HEALTHY_SCHEMA)
        self.assertIs(q["value_domain"], True)
        self.assertIs(q["param_types_declared"], True)
        self.assertIs(q["required_params_marked"], True)
        # All six dimensions satisfied -> perfect per-action score.
        self.assertEqual(q["schema_score"], 1.0)

    def test_untyped_undescribed_required_param_fails_value_domain(self):
        q = scb.score_schema_quality(UNTYPED_UNDESCRIBED_REQUIRED_SCHEMA)
        # NEW failure mode scores LOW on value_domain.
        self.assertIs(q["value_domain"], False)
        # param_types_declared also now fails because NOT every param is typed
        # (closing the vacuous-true hole — it is no longer "any param typed").
        self.assertIs(q["param_types_declared"], False)
        # required_params_marked still True: both params carry a boolean flag.
        self.assertIs(q["required_params_marked"], True)
        self.assertLess(q["schema_score"], 1.0)

    def test_empty_enum_constrained_param_fails_value_domain(self):
        q = scb.score_schema_quality(EMPTY_ENUM_SCHEMA)
        self.assertIs(q["value_domain"], False)
        # The param is typed + described, so the older structural dimensions
        # pass; only value_domain catches the undocumented enum domain.
        self.assertIs(q["param_types_declared"], True)

    def test_unbounded_numeric_param_fails_value_domain(self):
        q = scb.score_schema_quality(UNBOUNDED_NUMERIC_SCHEMA)
        self.assertIs(q["value_domain"], False)
        self.assertIs(q["param_types_declared"], True)

    def test_mixed_representation_union_does_not_require_numeric_range(self):
        q = scb.score_schema_quality(MIXED_REPRESENTATION_UNION_SCHEMA)
        self.assertIs(q["value_domain"], True)

    def test_nullable_numeric_union_still_requires_numeric_range(self):
        q = scb.score_schema_quality(UNBOUNDED_NULLABLE_NUMERIC_SCHEMA)
        self.assertIs(q["value_domain"], False)

    def test_one_sided_numeric_bounds_pass(self):
        cases = [
            ({"minimum": 0}, "lower_bounded", "ok_lower_bounded"),
            ({"maximum": 10}, "upper_bounded", "ok_upper_bounded"),
        ]
        for fields, expected_kind, expected_reason in cases:
            with self.subTest(fields=fields):
                q = scb.score_schema_quality(schema_with_param(numeric_param(**fields)))
                self.assertIs(q["value_domain"], True)
                diagnostic = q["value_domain_diagnostics"][0]
                self.assertEqual(diagnostic["derived_domain_kind"], expected_kind)
                self.assertEqual(diagnostic["reason"], expected_reason)

    def test_each_explicit_domain_kind_passes_with_required_evidence(self):
        cases = {
            "unbounded": {
                "kind": "unbounded",
                "rationale": "Material scalar values have no universal static bounds.",
            },
            "dynamic": {
                "kind": "dynamic",
                "source": "Current animation play length",
                "rationale": "The upper bound changes with the target animation asset.",
                "sentinels": [{"value": -1, "meaning": "Append to the current collection."}],
            },
            "cross_field": {
                "kind": "cross_field",
                "rule": "delay_max must be greater than or equal to delay_min.",
                "depends_on": ["delay_min"],
            },
            "composite": {
                "kind": "composite",
                "rule": "Every representation resolves to width and height.",
                "variants": ["array", "string", "object", "number"],
            },
            "normalized": {
                "kind": "normalized",
                "mode": "clamp",
                "minimum": 1,
                "maximum": 1000,
                "rationale": "The handler clamps result limits to its resource ceiling.",
            },
        }
        for kind, domain in cases.items():
            with self.subTest(kind=kind):
                q = scb.score_schema_quality(
                    schema_with_param(numeric_param(domain=domain))
                )
                self.assertIs(q["value_domain"], True)
                diagnostic = q["value_domain_diagnostics"][0]
                self.assertEqual(diagnostic["declared_domain_kind"], kind)
                self.assertEqual(diagnostic["derived_domain_kind"], kind)
                self.assertEqual(diagnostic["reason"], f"ok_explicit_{kind}")

    def test_nullable_numeric_passes_with_explicit_domain(self):
        schema = schema_with_param(
            {
                "type": "number|null",
                "description": "Optional arbitrary scalar",
                "required": False,
                "domain": {
                    "kind": "unbounded",
                    "rationale": "Null means omitted; numeric values are intentionally unbounded.",
                },
            }
        )
        q = scb.score_schema_quality(schema)
        self.assertIs(q["value_domain"], True)

    def test_invalid_or_gameable_domain_metadata_fails(self):
        invalid_domains = [
            ({}, "domain_kind_required"),
            ({"kind": "anything"}, "domain_kind_unknown"),
            ({"kind": "unbounded", "rationale": "   "}, "unbounded_domain_rationale_required"),
            (
                {"kind": "dynamic", "source": "", "rationale": "Changes at runtime"},
                "dynamic_domain_source_required",
            ),
            (
                {"kind": "dynamic", "source": "asset", "rationale": ""},
                "dynamic_domain_rationale_required",
            ),
            (
                {"kind": "cross_field", "rule": "", "depends_on": ["other"]},
                "cross_field_domain_rule_required",
            ),
            (
                {"kind": "cross_field", "rule": "value >= other", "depends_on": []},
                "cross_field_domain_depends_on_required",
            ),
            (
                {"kind": "composite", "rule": "normalize", "variants": ["", "number"]},
                "composite_domain_variants_required",
            ),
            (
                {
                    "kind": "normalized",
                    "mode": "truncate",
                    "minimum": 1,
                    "maximum": 10,
                    "rationale": "bounded",
                },
                "normalized_domain_mode_must_be_clamp",
            ),
            (
                {
                    "kind": "normalized",
                    "mode": "clamp",
                    "minimum": 10,
                    "maximum": 1,
                    "rationale": "bounded",
                },
                "normalized_domain_bounds_inverted",
            ),
            (
                {
                    "kind": "dynamic",
                    "source": "collection",
                    "rationale": "runtime-sized",
                    "sentinels": [{"value": -1, "meaning": ""}],
                },
                "domain_sentinel_meaning_required",
            ),
        ]
        for domain, expected_reason in invalid_domains:
            with self.subTest(domain=domain):
                q = scb.score_schema_quality(
                    schema_with_param(numeric_param(domain=domain))
                )
                self.assertIs(q["value_domain"], False)
                self.assertEqual(
                    q["value_domain_diagnostics"][0]["reason"], expected_reason
                )

    def test_invalid_top_level_bounds_fail(self):
        cases = [
            (numeric_param(minimum=True), "minimum_must_be_finite_number"),
            (numeric_param(maximum=float("inf")), "maximum_must_be_finite_number"),
            (numeric_param(minimum=2, maximum=1), "accepted_bounds_inverted"),
            (
                {
                    "type": "string",
                    "description": "Not numeric",
                    "required": False,
                    "minimum": 0,
                },
                "accepted_bounds_require_numeric_domain_type",
            ),
        ]
        for meta, expected_reason in cases:
            with self.subTest(meta=meta):
                q = scb.score_schema_quality(schema_with_param(meta))
                self.assertIs(q["value_domain"], False)
                self.assertEqual(
                    q["value_domain_diagnostics"][0]["reason"], expected_reason
                )

    def test_param_diagnostics_expose_stable_evidence(self):
        q = scb.score_schema_quality(UNBOUNDED_NULLABLE_NUMERIC_SCHEMA)
        self.assertEqual(
            q["value_domain_diagnostics"],
            [
                {
                    "param": "radius",
                    "ok": False,
                    "reason": "numeric_domain_missing",
                    "type_variants": ["null", "number"],
                    "derived_domain_kind": "missing",
                    "declared_domain_kind": None,
                }
            ],
        )

    def test_paramless_action_is_na_not_auto_pass(self):
        q = scb.score_schema_quality(PARAMLESS_SCHEMA)
        # Param-gated dimensions are N/A (None), NOT True.
        self.assertIsNone(q["param_types_declared"])
        self.assertIsNone(q["required_params_marked"])
        self.assertIsNone(q["value_domain"])
        # schema_score is the mean of only the applicable (non-None) dimensions.
        # planning + skill + output_contract all pass here -> 1.0, with no free
        # credit folded in from the N/A param dimensions.
        self.assertEqual(q["schema_score"], 1.0)

    def test_none_schema_is_hard_fail_not_na(self):
        q = scb.score_schema_quality(None)
        # A fetch failure is a hard failure, not an N/A.
        self.assertIs(q["value_domain"], False)
        self.assertIs(q["param_types_declared"], False)
        self.assertEqual(q["schema_score"], 0.0)


class AggregateTests(unittest.TestCase):
    def test_paramless_rows_excluded_from_param_gated_rates(self):
        # One healthy param-bearing action + one param-less action. The
        # param-gated rates must reflect ONLY the param-bearing action, so a
        # param-less action cannot inflate (or deflate) those rates.
        rows = [
            dict(scb.score_schema_quality(HEALTHY_SCHEMA), namespace="a", action="h", error=""),
            dict(scb.score_schema_quality(PARAMLESS_SCHEMA), namespace="a", action="p", error=""),
        ]
        ns_breakdown = scb.build_namespace_breakdown(rows)
        summary = scb.aggregate_metrics("t", rows, len(rows), ns_breakdown)
        m = summary["metrics"]
        self.assertEqual(m["param_bearing_action_count"], 1)
        self.assertEqual(m["param_less_action_count"], 1)
        # Both param-gated rates are 1.0 (the single param-bearing action is
        # healthy), proving the N/A row was excluded rather than counted as 0.
        self.assertEqual(m["value_domain_rate"], 1.0)
        self.assertEqual(m["param_types_declared_rate"], 1.0)

    def test_value_domain_failure_drops_aggregate_score(self):
        healthy = [
            dict(scb.score_schema_quality(HEALTHY_SCHEMA), namespace="a", action="h", error=""),
        ]
        broken = [
            dict(
                scb.score_schema_quality(UNTYPED_UNDESCRIBED_REQUIRED_SCHEMA),
                namespace="a",
                action="b",
                error="",
            ),
        ]
        s_healthy = scb.aggregate_metrics("h", healthy, 1, scb.build_namespace_breakdown(healthy))
        s_broken = scb.aggregate_metrics("b", broken, 1, scb.build_namespace_breakdown(broken))
        self.assertGreater(
            s_healthy["metrics"]["schema_completeness_score"],
            s_broken["metrics"]["schema_completeness_score"],
        )
        self.assertEqual(s_broken["metrics"]["value_domain_rate"], 0.0)

    def test_weights_sum_to_one(self):
        total = (
            scb.W_PARAM_TYPES
            + scb.W_REQUIRED_PARAMS
            + scb.W_VALUE_DOMAIN
            + scb.W_PLANNING_SIGNALS
            + scb.W_SKILL_ROUTING
            + scb.W_OUTPUT_CONTRACT
        )
        self.assertEqual(round(total, 6), 1.0)

    def test_paramless_namespace_score_renormalizes_not_caps(self):
        # A namespace of ONLY param-less actions used to cap at 0.35 (the three
        # param-gated dimensions folded in as 0.0). With renormalization over
        # the applicable dimensions, an all-passing param-less namespace
        # scores 1.0 and reports param_gated_applicable=False.
        rows = [
            dict(scb.score_schema_quality(PARAMLESS_SCHEMA), namespace="slate", action="a", error=""),
            dict(scb.score_schema_quality(PARAMLESS_SCHEMA), namespace="slate", action="b", error=""),
        ]
        breakdown = scb.build_namespace_breakdown(rows)
        ns = breakdown["slate"]
        self.assertFalse(ns["param_gated_applicable"])
        self.assertEqual(ns["schema_completeness_score"], 1.0)

    def test_mixed_namespace_score_still_uses_full_weights(self):
        # A namespace with at least one param-bearing action keeps all six
        # dimensions in the score (weights un-renormalized sum to 1.0).
        rows = [
            dict(scb.score_schema_quality(HEALTHY_SCHEMA), namespace="a", action="h", error=""),
            dict(scb.score_schema_quality(PARAMLESS_SCHEMA), namespace="a", action="p", error=""),
        ]
        breakdown = scb.build_namespace_breakdown(rows)
        ns = breakdown["a"]
        self.assertTrue(ns["param_gated_applicable"])
        self.assertEqual(ns["schema_completeness_score"], 1.0)

    def test_param_domain_aggregate_counts_and_coverage(self):
        rows = [
            dict(scb.score_schema_quality(HEALTHY_SCHEMA), namespace="a", action="healthy", error=""),
            dict(
                scb.score_schema_quality(UNBOUNDED_NUMERIC_SCHEMA),
                namespace="a",
                action="missing",
                error="",
            ),
            dict(scb.score_schema_quality(PARAMLESS_SCHEMA), namespace="a", action="none", error=""),
        ]
        summary = scb.aggregate_metrics(
            "aggregate", rows, len(rows), scb.build_namespace_breakdown(rows)
        )
        metrics = summary["metrics"]
        self.assertEqual(metrics["param_domain_total"], 3)
        self.assertEqual(metrics["param_domain_pass"], 2)
        self.assertEqual(metrics["param_domain_coverage"], 0.666667)
        ns = summary["namespace_breakdown"]["a"]
        self.assertEqual(ns["param_domain_total"], 3)
        self.assertEqual(ns["param_domain_pass"], 2)
        self.assertEqual(ns["param_domain_coverage"], 0.666667)


class TransportTests(unittest.TestCase):
    """Drive discover_schema_for_action through a fabricated MCP envelope."""

    def _patched_call(self, payload, *, is_error=False):
        def fake_mcp_call(url, tool, arguments, timeout_s=8.0):
            return mcp_envelope(payload, is_error=is_error)

        return fake_mcp_call

    def test_discover_then_score_healthy(self):
        original = scb.mcp_call
        scb.mcp_call = self._patched_call({"schema": HEALTHY_SCHEMA})
        try:
            schema = scb.discover_schema_for_action("http://x", "source", "search_source", 1.0)
        finally:
            scb.mcp_call = original
        self.assertIsNotNone(schema)
        q = scb.score_schema_quality(schema)
        self.assertIs(q["value_domain"], True)

    def test_discover_then_score_broken(self):
        original = scb.mcp_call
        scb.mcp_call = self._patched_call({"schema": UNTYPED_UNDESCRIBED_REQUIRED_SCHEMA})
        try:
            schema = scb.discover_schema_for_action("http://x", "blueprint", "add_variable", 1.0)
        finally:
            scb.mcp_call = original
        self.assertIsNotNone(schema)
        q = scb.score_schema_quality(schema)
        self.assertIs(q["value_domain"], False)

    def test_fetch_row_carries_param_level_diagnostics(self):
        original = scb.mcp_call
        scb.mcp_call = self._patched_call({"schema": UNBOUNDED_NUMERIC_SCHEMA})
        try:
            outcome, row = scb.fetch_and_score_schema_target(
                "http://x", "scene", "query", 1.0
            )
        finally:
            scb.mcp_call = original
        self.assertEqual(outcome.failure_kind, "ok")
        self.assertEqual(row["value_domain_diagnostics"][0]["param"], "radius")
        self.assertEqual(
            row["value_domain_diagnostics"][0]["reason"],
            "numeric_domain_missing",
        )


if __name__ == "__main__":
    unittest.main(verbosity=2)
