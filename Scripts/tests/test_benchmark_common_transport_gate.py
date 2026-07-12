#!/usr/bin/env python3
"""Offline boundary tests for the shared benchmark transport gate."""

from __future__ import annotations

import pathlib
import sys
import unittest

SCRIPTS_DIR = pathlib.Path(__file__).resolve().parent.parent
if str(SCRIPTS_DIR) not in sys.path:
    sys.path.insert(0, str(SCRIPTS_DIR))

from benchmark_common import TransportFailureTracker  # noqa: E402


class TransportFailureTrackerTests(unittest.TestCase):
    def test_three_consecutive_failures_abort_on_third_observation(self):
        tracker = TransportFailureTracker()
        self.assertIsNone(tracker.observe(transport_error=True, item_id="a1"))
        self.assertIsNone(tracker.observe(transport_error=True, item_id="a2"))
        decision = tracker.observe(transport_error=True, item_id="a3")
        self.assertIsNotNone(decision)
        self.assertEqual(decision.reason, "consecutive_transport_failures")
        self.assertEqual(decision.attempted_count, 3)

    def test_success_resets_consecutive_counter(self):
        tracker = TransportFailureTracker(max_failed_fraction=1.0)
        for failed in (True, True, False, True, True):
            self.assertIsNone(tracker.observe(transport_error=failed))
        self.assertEqual(tracker.consecutive_failures, 2)

    def test_nontransport_fetch_failure_resets_transport_streak(self):
        tracker = TransportFailureTracker(max_failed_fraction=1.0)
        tracker.observe(transport_error=True)
        tracker.observe(transport_error=True)
        self.assertIsNone(tracker.observe(transport_error=False, item_id="schema_missing"))
        self.assertEqual(tracker.consecutive_failures, 0)

    def test_fraction_gate_waits_for_minimum_sample(self):
        tracker = TransportFailureTracker(
            max_failed_fraction=0.05,
            max_consecutive_failures=20,
            min_fraction_samples=20,
        )
        for index in range(19):
            self.assertIsNone(tracker.observe(transport_error=index in {0, 5}))
        decision = tracker.observe(transport_error=False)
        self.assertIsNotNone(decision)
        self.assertEqual(decision.reason, "transport_failed_fraction")

    def test_fraction_decision_keeps_last_transport_identity_when_success_triggers_gate(self):
        tracker = TransportFailureTracker(
            max_failed_fraction=0.05,
            max_consecutive_failures=20,
            min_fraction_samples=20,
        )
        for index in range(1, 20):
            failed = index in {1, 6}
            self.assertIsNone(tracker.observe(
                transport_error=failed,
                item_id=f"a{index}",
                status=503 if failed else None,
                raw=f"down{index}" if failed else "",
            ))
        decision = tracker.observe(transport_error=False, item_id="a20")
        self.assertIsNotNone(decision)
        self.assertEqual(decision.item_id, "a6")
        self.assertEqual(decision.status, 503)
        self.assertEqual(decision.raw, "down6")
        self.assertEqual(tracker.snapshot()["last_transport_item_id"], "a6")

    def test_exact_fraction_threshold_is_allowed(self):
        tracker = TransportFailureTracker(
            max_failed_fraction=0.05,
            max_consecutive_failures=20,
            min_fraction_samples=20,
        )
        for index in range(20):
            self.assertIsNone(tracker.observe(transport_error=index == 0))
        self.assertIsNone(tracker.finalize())

    def test_finalize_checks_short_population_fraction(self):
        tracker = TransportFailureTracker(
            max_failed_fraction=0.05,
            max_consecutive_failures=20,
            min_fraction_samples=20,
        )
        for index in range(10):
            self.assertIsNone(tracker.observe(transport_error=index == 0))
        decision = tracker.finalize()
        self.assertIsNotNone(decision)
        self.assertEqual(decision.reason, "final_transport_failed_fraction")

    def test_empty_raw_still_counts_when_transport_flag_is_true(self):
        tracker = TransportFailureTracker(max_failed_fraction=1.0)
        tracker.observe(transport_error=True, raw="")
        self.assertEqual(tracker.failure_count, 1)
        self.assertEqual(tracker.snapshot()["last_transport_error_raw"], "")

    def test_snapshot_preserves_status_and_item(self):
        tracker = TransportFailureTracker(max_failed_fraction=1.0)
        tracker.observe(transport_error=True, item_id="project.search", status=503, raw="down")
        decision = tracker.finalize()
        self.assertIsNone(decision)
        self.assertEqual(tracker.last_item_id, "project.search")
        self.assertEqual(tracker.snapshot()["last_transport_status"], 503)

    def test_invalid_configuration_is_rejected(self):
        for kwargs in (
            {"max_failed_fraction": -0.1},
            {"max_failed_fraction": 1.1},
            {"max_consecutive_failures": 0},
            {"min_fraction_samples": 0},
        ):
            with self.subTest(kwargs=kwargs):
                with self.assertRaises(ValueError):
                    TransportFailureTracker(**kwargs)


if __name__ == "__main__":
    unittest.main(verbosity=2)
