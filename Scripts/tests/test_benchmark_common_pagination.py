#!/usr/bin/env python3
"""Offline unit tests for benchmark_common.paginate_discover_action_names.

The shared pager backs ActionGuidance error-recovery enumeration and the
SourceIndex task-generation top-up. It must join every mode="actions" page via
next_offset (the server's default 50-row page hides everything past page 1 for
namespaces like ai with 182 actions) and fail fast on contract drift.

Run:
    python Plugins/Monolith/Scripts/tests/test_benchmark_common_pagination.py
"""

from __future__ import annotations

import pathlib
import sys
import unittest

SCRIPTS_DIR = pathlib.Path(__file__).resolve().parent.parent
if str(SCRIPTS_DIR) not in sys.path:
    sys.path.insert(0, str(SCRIPTS_DIR))

from benchmark_common import paginate_discover_action_names  # noqa: E402


def paged_server(actions, page_size):
    calls = []

    def fetch_page(arguments):
        calls.append(dict(arguments))
        offset = int(arguments.get("offset", 0))
        page = actions[offset:offset + page_size]
        payload = {
            "actions": [{"action": name} for name in page],
            "total": len(actions),
            "truncated": offset + page_size < len(actions),
        }
        if payload["truncated"]:
            payload["next_offset"] = offset + page_size
        return payload

    return fetch_page, calls


class PaginateDiscoverActionNamesTests(unittest.TestCase):
    def test_joins_pages_via_next_offset(self):
        fetch_page, calls = paged_server([f"a{i}" for i in range(5)], page_size=2)
        names = paginate_discover_action_names(fetch_page, "ai")
        self.assertEqual(names, ["a0", "a1", "a2", "a3", "a4"])
        self.assertEqual(len(calls), 3)
        self.assertTrue(all(c["namespace"] == "ai" and c["mode"] == "actions" for c in calls))

    def test_single_page_returns_without_extra_calls(self):
        fetch_page, calls = paged_server(["x", "y"], page_size=50)
        names = paginate_discover_action_names(fetch_page, "config")
        self.assertEqual(names, ["x", "y"])
        self.assertEqual(len(calls), 1)

    def test_string_rows_are_accepted(self):
        def fetch_page(arguments):
            return {"actions": ["plain_one", {"action": "dict_two"}], "truncated": False}

        names = paginate_discover_action_names(fetch_page, "source")
        self.assertEqual(names, ["plain_one", "dict_two"])

    def test_missing_actions_list_raises(self):
        def fetch_page(arguments):
            return {"namespaces": []}

        with self.assertRaises(RuntimeError):
            paginate_discover_action_names(fetch_page, "ai")

    def test_non_advancing_pagination_raises(self):
        def fetch_page(arguments):
            return {"actions": [{"action": "a"}], "truncated": True, "next_offset": 0}

        with self.assertRaises(RuntimeError):
            paginate_discover_action_names(fetch_page, "ai")


if __name__ == "__main__":
    unittest.main(verbosity=2)
