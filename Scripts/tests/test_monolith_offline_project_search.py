#!/usr/bin/env python3
"""Focused stdlib tests for the Python project-search fallback."""

import contextlib
import importlib.util
import io
import json
import sqlite3
import sys
import types
import unittest
from pathlib import Path


MODULE_PATH = Path(__file__).resolve().parents[1] / "monolith_offline.py"
MODULE_SPEC = importlib.util.spec_from_file_location(
    "monolith_offline_project_search_test_target",
    MODULE_PATH,
)
MONOLITH_OFFLINE = importlib.util.module_from_spec(MODULE_SPEC)
sys.modules[MODULE_SPEC.name] = MONOLITH_OFFLINE
MODULE_SPEC.loader.exec_module(MONOLITH_OFFLINE)

ASSET_FIELDS = (
    "asset_name",
    "asset_class",
    "description",
    "package_path",
    "module_name",
)
NODE_FIELDS = ("node_name", "node_class", "node_type")
ENABLED_FIELDS = set(ASSET_FIELDS + NODE_FIELDS)


class FtsProjectionTests(unittest.TestCase):
    def assert_invalid(self, query, expected_error):
        for current_fields in (ASSET_FIELDS, NODE_FIELDS):
            result, projected, error = MONOLITH_OFFLINE._project_fts_query(
                query,
                current_fields,
                ENABLED_FIELDS,
            )
            self.assertEqual("invalid", result)
            self.assertEqual("", projected)
            self.assertIn(expected_error, error)

    def test_valid_near_phrase_grammar_is_preserved(self):
        query = 'NEAR("Common Search" Branch* + Tail, 8)'
        result, projected, error = MONOLITH_OFFLINE._project_fts_query(
            query,
            ASSET_FIELDS,
            ENABLED_FIELDS,
        )
        self.assertEqual("applicable", result)
        self.assertEqual(query, projected)
        self.assertEqual("", error)

    def test_near_boolean_is_rejected_before_field_projection(self):
        self.assert_invalid(
            "asset_name:Foo AND node_name:NEAR(Foo OR Bar)",
            "boolean operators",
        )

    def test_invalid_near_phrase_and_distance_forms_are_explicit(self):
        cases = (
            ("NEAR(^Foo Bar)", "initial-token anchor"),
            ("NEAR(Foo +, 8)", "requires a following string"),
            ("NEAR(Foo, abc)", "unsigned decimal integer"),
            ("NEAR(Foo, 1, 2)", "at most one distance"),
            ("NEAR()", "at least one phrase"),
        )
        for query, expected_error in cases:
            with self.subTest(query=query):
                self.assert_invalid(query, expected_error)


class ProjectSearchErrorEnvelopeTests(unittest.TestCase):
    def test_database_corruption_uses_structured_failure_envelope(self):
        connection = sqlite3.connect(":memory:")
        self.addCleanup(connection.close)
        connection.row_factory = sqlite3.Row
        connection.execute(
            """
            CREATE TABLE assets (
                id INTEGER PRIMARY KEY,
                package_path TEXT,
                asset_name TEXT,
                asset_class TEXT,
                module_name TEXT,
                description TEXT
            )
            """
        )
        connection.execute(
            """
            CREATE VIRTUAL TABLE fts_assets USING fts5(
                asset_name,
                asset_class,
                description,
                package_path,
                module_name
            )
            """
        )
        for index in range(100):
            values = (
                f"/Game/CorruptFixture{index}",
                f"Needle{index}",
                "Blueprint",
                "Game",
                "Common description",
            )
            connection.execute(
                """
                INSERT INTO assets (
                    package_path,
                    asset_name,
                    asset_class,
                    module_name,
                    description
                ) VALUES (?, ?, ?, ?, ?)
                """,
                values,
            )
            connection.execute(
                """
                INSERT INTO fts_assets (
                    rowid,
                    asset_name,
                    asset_class,
                    description,
                    package_path,
                    module_name
                ) VALUES (?, ?, ?, ?, ?, ?)
                """,
                (
                    index + 1,
                    values[1],
                    values[2],
                    values[4],
                    values[0],
                    values[3],
                ),
            )
        connection.commit()

        corrupted = connection.execute(
            """
            UPDATE fts_assets_data
            SET block = randomblob(length(block))
            WHERE id = 10
            """
        )
        self.assertEqual(1, corrupted.rowcount)
        connection.commit()
        with self.assertRaisesRegex(
            sqlite3.DatabaseError,
            "database disk image is malformed",
        ):
            connection.execute(
                """
                SELECT rowid
                FROM fts_assets
                WHERE fts_assets MATCH ?
                """,
                ("Needle1",),
            ).fetchall()

        actions = MONOLITH_OFFLINE.ProjectActions.__new__(
            MONOLITH_OFFLINE.ProjectActions
        )
        actions._sqlite3 = sqlite3
        actions.db = connection
        args = types.SimpleNamespace(query="Needle", limit=50)

        output = io.StringIO()
        with contextlib.redirect_stdout(output):
            actions.search(args)

        payload = json.loads(output.getvalue())
        self.assertFalse(payload["success"])
        self.assertIn("for assets", payload["error"])
        self.assertIn("database disk image is malformed", payload["error"])


if __name__ == "__main__":
    unittest.main()
