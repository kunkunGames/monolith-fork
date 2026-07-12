#!/usr/bin/env python3
"""Offline tests: benchmark fixture traffic must not be reported as real agent demand.

The benchmark suites deliberately call hallucinated action names and transposed-letter
typos as NEGATIVE fixtures. The action logger's only synthetic signal used to be
``environment.is_automation_test`` (stamped from ``GIsAutomationTesting``), which covers
in-process C++ automation but NOT the out-of-process HTTP benchmark runners. As a result
the 2026-07-11 analyzer run ranked 100+ fixture-only actions (``worldgen.get_blockout_volumse``,
``ui.get_widget_tree_typo``, ``mesh.get_mesh_inof``, ...) as ``needed_action`` findings --
the very report an agent reads to decide which actions to build next.

The runners now self-declare through the existing ``_monolith_routing_context`` extension
point, and the analyzer treats ``routing_context.client_kind == "benchmark"`` as a primary
synthetic signal.
"""

from __future__ import annotations

import importlib.util
import pathlib
import sys
import unittest

SCRIPTS_DIR = pathlib.Path(__file__).resolve().parent.parent
PLUGIN_ROOT = SCRIPTS_DIR.parent
if str(SCRIPTS_DIR) not in sys.path:
    sys.path.insert(0, str(SCRIPTS_DIR))

from benchmark_common import BENCHMARK_CLIENT_KIND, benchmark_routing_context  # noqa: E402

_ANALYZER_PATH = PLUGIN_ROOT / "Analyzer" / "analyze_invocation_logs.py"
_spec = importlib.util.spec_from_file_location("analyze_invocation_logs", _ANALYZER_PATH)
assert _spec and _spec.loader
analyzer = importlib.util.module_from_spec(_spec)
# The analyzer defines @dataclass types, and dataclasses resolves annotations through
# sys.modules[cls.__module__] — so the module must be registered before it executes.
sys.modules[_spec.name] = analyzer
_spec.loader.exec_module(analyzer)


class BenchmarkRoutingContextTests(unittest.TestCase):
    def test_routing_context_declares_client_kind_and_suite(self):
        ctx = benchmark_routing_context("AssetEditing")
        self.assertEqual(ctx["client_kind"], BENCHMARK_CLIENT_KIND)
        self.assertEqual(ctx["suite"], "AssetEditing")

    def test_suite_is_required_so_noisy_traffic_stays_attributable(self):
        with self.assertRaises(ValueError):
            benchmark_routing_context("")

    def test_every_live_suite_stamps_the_routing_context_on_its_mcp_calls(self):
        suites = {
            "action_guidance_benchmark.py": "ActionGuidance",
            "ai_capability_benchmark.py": "AICapability",
            "asset_editing_benchmark.py": "AssetEditing",
            "project_index_benchmark.py": "ProjectIndex",
            "schema_completeness_benchmark.py": "SchemaCompleteness",
            "source_index_benchmark.py": "SourceIndex",
        }
        for filename, suite in suites.items():
            src = (SCRIPTS_DIR / filename).read_text(encoding="utf-8")
            with self.subTest(suite=suite):
                self.assertIn(f'benchmark_routing_context("{suite}")', src)
                self.assertIn('"_monolith_routing_context": _BENCHMARK_ROUTING_CONTEXT', src)


class ClassifyNoiseBenchmarkClientTests(unittest.TestCase):
    def _classify(self, routing_context):
        # A hallucinated action name a benchmark sends on purpose.
        return analyzer.classify_noise(
            "action", "worldgen", "get_blockout_volumse", "worldgen_query", (),
            {"namespace": "worldgen", "action": "get_blockout_volumse"},
            "Unknown action: get_blockout_volumse",
            {},
            routing_context,
        )

    def test_benchmark_client_kind_is_classified_synthetic(self):
        self.assertEqual(self._classify(benchmark_routing_context("WorldGen")), "synthetic_test")

    def test_real_agent_traffic_is_not_classified_synthetic(self):
        self.assertNotEqual(self._classify({"client_kind": "agent"}), "synthetic_test")
        self.assertNotEqual(self._classify({}), "synthetic_test")
        self.assertNotEqual(self._classify(None), "synthetic_test")

    def test_automation_stamp_still_wins(self):
        self.assertEqual(
            analyzer.classify_noise(
                "action", "ui", "get_widget_tree_typo", "ui_query", (), {}, "",
                {"is_automation_test": True}, None,
            ),
            "synthetic_test",
        )

    def test_benchmark_rows_are_excluded_from_needed_action_findings(self):
        """The end-to-end guarantee: a benchmark's unknown-action row must not surface
        as unmet demand, while the same row from a real agent still must."""
        event_args = dict(
            surface="action", namespace="worldgen", action="get_blockout_volumse",
            tool_name="worldgen_query", tags=(),
            call={"namespace": "worldgen", "action": "get_blockout_volumse"},
            message="Unknown action: get_blockout_volumse", environment={},
        )
        benchmark_class = analyzer.classify_noise(
            routing_context=benchmark_routing_context("WorldGen"), **event_args
        )
        agent_class = analyzer.classify_noise(routing_context={"client_kind": "agent"}, **event_args)

        self.assertEqual(benchmark_class, "synthetic_test")
        self.assertNotEqual(agent_class, "synthetic_test")


if __name__ == "__main__":
    unittest.main()
