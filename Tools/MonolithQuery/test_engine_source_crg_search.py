#!/usr/bin/env python3
"""Focused EngineSource-backed search_crg_graph integration coverage."""

from __future__ import annotations

import argparse
from contextlib import closing
import json
from pathlib import Path
import sqlite3
import subprocess
import sys
import tempfile
import unittest


QUERY_EXE: Path


GRAPH_NODE_SCHEMA = """
CREATE VIEW source_graph_nodes AS
SELECT -f.id AS id,
       'File' AS kind,
       COALESCE(NULLIF(f.path,''), 'file#' || f.id) AS name,
       COALESCE(NULLIF(f.path,''), 'file#' || f.id) AS qualified_name,
       COALESCE(f.path,'') AS file_path,
       1 AS line_start,
       COALESCE(f.line_count,0) AS line_end,
       CASE
         WHEN lower(COALESCE(f.file_type,'')) IN ('header','hpp','h') THEN 'cpp'
         WHEN lower(COALESCE(f.file_type,'')) IN ('cpp','cc','cxx') THEN 'cpp'
         WHEN lower(COALESCE(f.file_type,'')) LIKE '%shader%' THEN 'shader'
         ELSE COALESCE(NULLIF(f.file_type,''),'cpp')
       END AS language,
       NULL AS signature
FROM files f
UNION ALL
SELECT s.id AS id,
       CASE
         WHEN lower(COALESCE(s.kind,'')) IN ('class','struct','interface') THEN 'Class'
         WHEN lower(COALESCE(s.kind,'')) IN ('function','method','constructor','destructor') THEN
           CASE
             WHEN lower(COALESCE(s.name,'') || ' ' || COALESCE(f.path,'')) LIKE '%test%'
             THEN 'Test'
             ELSE 'Function'
           END
         WHEN lower(COALESCE(s.kind,'')) IN ('enum','typedef','type','delegate') THEN 'Type'
         ELSE COALESCE(NULLIF(s.kind,''),'Symbol')
       END AS kind,
       COALESCE(NULLIF(s.name,''), 'symbol#' || s.id) AS name,
       COALESCE(
         NULLIF(s.qualified_name,''),
         COALESCE(f.path,'') || '::' || COALESCE(s.name,'symbol')
       ) || '#' || s.id AS qualified_name,
       COALESCE(f.path,'') AS file_path,
       COALESCE(s.line_start,0) AS line_start,
       COALESCE(s.line_end,0) AS line_end,
       CASE
         WHEN lower(COALESCE(f.file_type,'')) LIKE '%shader%' THEN 'shader'
         ELSE 'cpp'
       END AS language,
       s.signature AS signature
FROM symbols s
LEFT JOIN files f ON f.id = s.file_id;

CREATE VIRTUAL TABLE source_graph_nodes_fts USING fts5(
    name,
    qualified_name,
    file_path,
    signature,
    content='source_graph_nodes',
    content_rowid='id',
    tokenize='porter unicode61'
);
INSERT INTO source_graph_nodes_fts(source_graph_nodes_fts) VALUES('rebuild');

CREATE TRIGGER source_graph_nodes_files_ai AFTER INSERT ON files BEGIN SELECT 1; END;
CREATE TRIGGER source_graph_nodes_files_ad AFTER DELETE ON files BEGIN SELECT 1; END;
CREATE TRIGGER source_graph_nodes_files_bd BEFORE DELETE ON files BEGIN SELECT 1; END;
CREATE TRIGGER source_graph_nodes_files_bu BEFORE UPDATE ON files BEGIN SELECT 1; END;
CREATE TRIGGER source_graph_nodes_files_au AFTER UPDATE ON files BEGIN SELECT 1; END;
CREATE TRIGGER source_graph_nodes_symbols_ai AFTER INSERT ON symbols BEGIN SELECT 1; END;
CREATE TRIGGER source_graph_nodes_symbols_bd BEFORE DELETE ON symbols BEGIN SELECT 1; END;
CREATE TRIGGER source_graph_nodes_symbols_bu BEFORE UPDATE ON symbols BEGIN SELECT 1; END;
CREATE TRIGGER source_graph_nodes_symbols_au AFTER UPDATE ON symbols BEGIN SELECT 1; END;
"""


def create_source_db(path: Path, *, with_graph_search: bool = True) -> None:
    with closing(sqlite3.connect(path)) as conn:
        conn.executescript(
            """
            CREATE TABLE modules (id INTEGER PRIMARY KEY, name TEXT NOT NULL);
            CREATE TABLE files (
                id INTEGER PRIMARY KEY,
                path TEXT NOT NULL,
                module_id INTEGER,
                line_count INTEGER NOT NULL DEFAULT 0,
                file_type TEXT
            );
            CREATE TABLE symbols (
                id INTEGER PRIMARY KEY,
                name TEXT NOT NULL,
                qualified_name TEXT,
                kind TEXT NOT NULL,
                file_id INTEGER,
                parent_symbol_id INTEGER,
                signature TEXT,
                docstring TEXT,
                line_start INTEGER NOT NULL DEFAULT 0,
                line_end INTEGER NOT NULL DEFAULT 0,
                access TEXT,
                is_ue_macro INTEGER NOT NULL DEFAULT 0
            );
            CREATE TABLE "references" (
                id INTEGER PRIMARY KEY,
                from_symbol_id INTEGER,
                to_symbol_id INTEGER,
                ref_kind TEXT,
                file_id INTEGER,
                line INTEGER NOT NULL DEFAULT 0
            );
            CREATE TABLE inheritance (
                id INTEGER PRIMARY KEY,
                child_id INTEGER,
                parent_id INTEGER
            );
            CREATE TABLE includes (
                id INTEGER PRIMARY KEY,
                from_file_id INTEGER,
                to_file_id INTEGER
            );
            CREATE TABLE meta (key TEXT PRIMARY KEY, value TEXT);

            INSERT INTO meta(key,value) VALUES
                ('schema_version','4'),
                ('source_graph_nodes_fts_version','1');
            INSERT INTO modules(id,name) VALUES(1,'Fixture');
            INSERT INTO files(id,path,module_id,line_count,file_type) VALUES
                (1,'D:/UE/Source/Fixture/BuildCards.Build.cs',1,20,'cs'),
                (2,'D:/UE/Source/Fixture/Object.h',1,120,'header'),
                (3,'D:/UE/Source/Fixture/Twin.cpp',1,80,'cpp');
            INSERT INTO symbols(
                id,name,qualified_name,kind,file_id,signature,line_start,line_end,access
            ) VALUES
                (10,'UObject','UObject','class',2,'class UObject',10,100,'public'),
                (11,'ExtractSkinWeights','Fixture::ExtractSkinWeights','function',3,
                    'void ExtractSkinWeights(TArray<int32> Values)',20,25,'public'),
                (12,'MiddleNeedle','Fixture::MiddleNeedle','function',3,
                    'void MiddleNeedle()',30,31,'public'),
                (13,'Twin','Alpha::Twin','class',3,'class Twin',40,50,'public'),
                (14,'Twin','Beta::Twin','class',3,'class Twin',40,50,'public'),
                (15,'FixtureTestRunner','Fixture::FixtureTestRunner','function',3,
                    'void FixtureTestRunner()',60,61,'public');

            CREATE VIRTUAL TABLE symbols_fts USING fts5(
                name,
                qualified_name,
                docstring,
                content='symbols',
                content_rowid='id'
            );
            INSERT INTO symbols_fts(symbols_fts) VALUES('rebuild');
            CREATE TRIGGER symbols_ai AFTER INSERT ON symbols BEGIN
                INSERT INTO symbols_fts(rowid,name,qualified_name,docstring)
                VALUES(new.id,new.name,new.qualified_name,new.docstring);
            END;
            CREATE TRIGGER symbols_ad AFTER DELETE ON symbols BEGIN
                INSERT INTO symbols_fts(symbols_fts,rowid,name,qualified_name,docstring)
                VALUES('delete',old.id,old.name,old.qualified_name,old.docstring);
            END;
            CREATE TRIGGER symbols_au AFTER UPDATE ON symbols BEGIN
                INSERT INTO symbols_fts(symbols_fts,rowid,name,qualified_name,docstring)
                VALUES('delete',old.id,old.name,old.qualified_name,old.docstring);
                INSERT INTO symbols_fts(rowid,name,qualified_name,docstring)
                VALUES(new.id,new.name,new.qualified_name,new.docstring);
            END;
            CREATE VIRTUAL TABLE source_fts USING fts5(
                file_id UNINDEXED,
                line_number UNINDEXED,
                text
            );
            """
        )
        if with_graph_search:
            conn.executescript(GRAPH_NODE_SCHEMA)
        conn.commit()


def invoke(
    db_path: Path,
    action: str,
    *arguments: str,
) -> tuple[subprocess.CompletedProcess[str], dict[str, object]]:
    completed = subprocess.run(
        [
            str(QUERY_EXE),
            "source",
            action,
            *arguments,
            f"--source_db={db_path}",
            "--no-log",
        ],
        check=False,
        capture_output=True,
        text=True,
        encoding="utf-8",
        timeout=30,
    )
    payload = json.loads(completed.stdout)
    return completed, payload


def invoke_readonly(
    db_path: Path,
    action: str,
    *arguments: str,
) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [
            str(QUERY_EXE),
            "--readonly",
            "source",
            action,
            *arguments,
            f"--source_db={db_path}",
            "--no-log",
        ],
        check=False,
        capture_output=True,
        text=True,
        encoding="utf-8",
        timeout=30,
    )


class EngineSourceCrgSearchTests(unittest.TestCase):
    def test_readonly_refuses_active_rollback_journal_without_touching_writer(
        self,
    ) -> None:
        with tempfile.TemporaryDirectory(prefix="monolith-engine-source-crg-") as temp:
            db_path = Path(temp) / "EngineSource.db"
            journal_path = Path(f"{db_path}-journal")
            create_source_db(db_path)

            with closing(sqlite3.connect(db_path)) as writer:
                writer.execute("PRAGMA journal_mode=DELETE")
                writer.execute("BEGIN IMMEDIATE")
                writer.execute(
                    "UPDATE meta SET value='active-writer' "
                    "WHERE key='schema_version'"
                )
                self.assertTrue(journal_path.is_file())

                completed = invoke_readonly(
                    db_path,
                    "health",
                    "--include-deep-checks=true",
                )
                self.assertNotEqual(0, completed.returncode, completed.stdout)
                self.assertIn(
                    "Rollback journal exists for database and could not be recovered safely:",
                    completed.stderr,
                )
                self.assertIn("global --readonly refuses database access", completed.stderr)
                self.assertTrue(
                    journal_path.is_file(),
                    "readonly refusal must not recover, delete, or truncate the writer journal",
                )
                writer.rollback()

            self.assertFalse(journal_path.exists())

    def test_malformed_fts_query_is_structured_and_never_falls_back(self) -> None:
        with tempfile.TemporaryDirectory(prefix="monolith-engine-source-crg-") as temp:
            db_path = Path(temp) / "EngineSource.db"
            create_source_db(db_path)
            with closing(sqlite3.connect(db_path)) as conn:
                conn.executescript(
                    """
                    DROP TABLE source_graph_nodes_fts;
                    CREATE TABLE source_graph_nodes_fts(
                        rowid INTEGER,
                        name TEXT,
                        qualified_name TEXT,
                        file_path TEXT,
                        signature TEXT
                    );
                    """
                )
                conn.commit()

            process, payload = invoke(db_path, "search_crg_graph", "UObject")
            self.assertEqual(0, process.returncode, process.stderr)
            self.assertEqual("warning", payload["status"])
            self.assertEqual([], payload["results"])
            self.assertEqual(0, payload["count"])
            self.assertFalse(payload["used_fts"])
            self.assertTrue(
                any("LIKE fallback was not run" in warning for warning in payload["warnings"]),
                payload,
            )

    def test_fts_preserves_file_symbol_signature_and_kind_semantics(self) -> None:
        with tempfile.TemporaryDirectory(prefix="monolith-engine-source-crg-") as temp:
            root = Path(temp)
            db_path = root / "EngineSource.db"
            create_source_db(db_path)

            process, file_payload = invoke(
                db_path,
                "search_crg_graph",
                "BuildCards",
                "--kind=File",
            )
            self.assertEqual(0, process.returncode, process.stderr)
            self.assertEqual("ok", file_payload["status"])
            self.assertEqual("engine_source_fts", file_payload["backend"])
            self.assertTrue(file_payload["used_fts"])
            self.assertEqual(-1, file_payload["results"][0]["id"])
            self.assertEqual("File", file_payload["results"][0]["kind"])
            self.assertTrue(
                file_payload["results"][0]["file_path"].endswith("BuildCards.Build.cs")
            )
            self.assertEqual("known", file_payload["results"][0]["path_status"])
            self.assertNotIn("graph_db", file_payload)
            self.assertNotIn("graph_db", file_payload["input"])

            process, signature_payload = invoke(
                db_path,
                "search_crg_graph",
                "TArray",
                "--kind=function",
            )
            self.assertEqual(0, process.returncode, process.stderr)
            self.assertTrue(signature_payload["used_fts"])
            self.assertEqual([11], [row["id"] for row in signature_payload["results"]])
            self.assertEqual("Function", signature_payload["results"][0]["kind"])
            self.assertIn("TArray", signature_payload["results"][0]["signature"])
            self.assertGreater(signature_payload["results"][0]["score"], 0)

            process, qualified_payload = invoke(
                db_path,
                "search_crg_graph",
                "Fixture::ExtractSkinWeights",
            )
            self.assertEqual(0, process.returncode, process.stderr)
            self.assertEqual(11, qualified_payload["results"][0]["id"])

            # Compact golden extension: one semantic symbol identity appears
            # with both a known and missing path, while Type and fallback kind
            # families exercise the canonical normalization surface.
            with closing(sqlite3.connect(db_path)) as conn:
                conn.executescript(
                    """
                    INSERT INTO symbols(
                        id,name,qualified_name,kind,file_id,signature,line_start,line_end,access
                    ) VALUES
                        (20,'IdentityPriorityNeedle','Fixture::IdentityPriorityNeedle',
                         'class',3,'class IdentityPriorityNeedle',70,72,'public'),
                        (21,'IdentityPriorityNeedle','Fixture::IdentityPriorityNeedle',
                         'class',NULL,'class IdentityPriorityNeedle',70,72,'public'),
                        (22,'EnumPriorityNeedle','Fixture::EnumPriorityNeedle',
                         'enum',3,'enum class EnumPriorityNeedle',73,75,'public'),
                        (23,'CustomPriorityNeedle','Fixture::CustomPriorityNeedle',
                         'macro',3,'CUSTOM CustomPriorityNeedle',76,77,'public');
                    INSERT INTO source_graph_nodes_fts(source_graph_nodes_fts)
                    VALUES('rebuild');
                    """
                )
                conn.commit()

            process, identity_payload = invoke(
                db_path,
                "search_crg_graph",
                "IdentityPriorityNeedle",
            )
            self.assertEqual(0, process.returncode, process.stderr)
            self.assertTrue(identity_payload["used_fts"])
            self.assertEqual(1, identity_payload["count"])
            self.assertEqual([20], [row["id"] for row in identity_payload["results"]])
            self.assertEqual("known", identity_payload["results"][0]["path_status"])

            process, type_payload = invoke(
                db_path,
                "search_crg_graph",
                "EnumPriorityNeedle",
            )
            self.assertEqual(0, process.returncode, process.stderr)
            self.assertEqual("Type", type_payload["results"][0]["kind"])

            process, custom_payload = invoke(
                db_path,
                "search_crg_graph",
                "CustomPriorityNeedle",
            )
            self.assertEqual(0, process.returncode, process.stderr)
            self.assertEqual("macro", custom_payload["results"][0]["kind"])

            process, low_clamp = invoke(
                db_path,
                "search_crg_graph",
                "Twin",
                "--limit=0",
            )
            self.assertEqual(0, process.returncode, process.stderr)
            self.assertEqual(1, low_clamp["limits"]["limit"])
            self.assertTrue(low_clamp["truncated"])

            process, high_clamp = invoke(
                db_path,
                "search_crg_graph",
                "Twin",
                "--limit=999",
            )
            self.assertEqual(0, process.returncode, process.stderr)
            self.assertEqual(200, high_clamp["limits"]["limit"])
            self.assertGreaterEqual(high_clamp["count"], 2)
            self.assertFalse(high_clamp["truncated"])
            self.assertFalse((root / "graph.db").exists())

    def test_zero_hit_like_fallback_and_deterministic_truncation(self) -> None:
        with tempfile.TemporaryDirectory(prefix="monolith-engine-source-crg-") as temp:
            root = Path(temp)
            db_path = root / "EngineSource.db"
            create_source_db(db_path)

            process, fallback = invoke(
                db_path,
                "search_crg_graph",
                "iddleNeed",
                "--kind=Function",
            )
            self.assertEqual(0, process.returncode, process.stderr)
            self.assertEqual("ok", fallback["status"])
            self.assertFalse(fallback["used_fts"])
            self.assertEqual([12], [row["id"] for row in fallback["results"]])
            self.assertEqual(0.0, fallback["results"][0]["score"])

            process, twins = invoke(
                db_path,
                "search_crg_graph",
                "Twin",
                "--kind=Class",
                "--limit=1",
            )
            self.assertEqual(0, process.returncode, process.stderr)
            self.assertTrue(twins["used_fts"])
            self.assertTrue(twins["truncated"])
            self.assertEqual(1, twins["count"])
            self.assertEqual(13, twins["results"][0]["id"])
            self.assertEqual("Alpha::Twin#13", twins["results"][0]["qualified_name"])
            self.assertFalse((root / "graph.db").exists())

    def test_broad_fts_uses_bounded_ranked_candidates_without_semantic_fallback(self) -> None:
        with tempfile.TemporaryDirectory(prefix="monolith-engine-source-crg-") as temp:
            root = Path(temp)
            db_path = root / "EngineSource.db"
            create_source_db(db_path)

            # Exceed the minimum candidate budget with known-path matches. The
            # common-token query must use FTS5's bounded rank scan, resolve the
            # candidates through the canonical VIEW, and prove the result
            # boundary without invoking the expensive full-rank fallback.
            with closing(sqlite3.connect(db_path)) as conn:
                conn.executemany(
                    """
                    INSERT INTO symbols(
                        id,name,qualified_name,kind,file_id,signature,
                        line_start,line_end,access
                    ) VALUES(?,?,?,?,?,?,?,?,?)
                    """,
                    [
                        (
                            1000 + index,
                            f"BroadRankNeedle{index}",
                            f"Fixture::BroadRankNeedle{index}",
                            "class",
                            3,
                            (
                                f"class BroadRankNeedle{index} "
                                "BroadRankNeedle BroadRankNeedle BroadRankNeedle"
                                if index < 16
                                else f"class BroadRankNeedle{index}"
                            ),
                            1,
                            2,
                            "public",
                        )
                        for index in range(4200)
                    ],
                )
                conn.execute(
                    "INSERT INTO source_graph_nodes_fts(source_graph_nodes_fts) "
                    "VALUES('rebuild')"
                )
                conn.commit()

            process, payload = invoke(
                db_path,
                "search_crg_graph",
                "BroadRankNeedle",
                "--kind=Class",
                "--limit=5",
            )
            self.assertEqual(0, process.returncode, process.stderr)
            self.assertEqual("ok", payload["status"])
            self.assertTrue(payload["used_fts"])
            self.assertEqual(5, payload["count"])
            self.assertTrue(payload["truncated"])
            self.assertEqual(4096, payload["limits"]["fts_candidate_limit"])
            self.assertEqual(4096, payload["limits"]["fts_candidates"])
            self.assertTrue(payload["limits"]["fts_candidate_pool_truncated"])
            self.assertFalse(payload["limits"]["fts_full_rank_fallback"])
            self.assertTrue(
                all(row["path_status"] == "known" for row in payload["results"])
            )

            # Adversarial boundary: every match has exactly the same BM25
            # score, while the lexically smallest qualified names were inserted
            # after the bounded pool. The optimization must detect the equal-
            # rank cutoff and expand the bounded pool so tie ordering stays
            # identical instead of silently returning the first rowids.
            with closing(sqlite3.connect(db_path)) as conn:
                conn.executemany(
                    """
                    INSERT INTO symbols(
                        id,name,qualified_name,kind,file_id,signature,
                        line_start,line_end,access
                    ) VALUES(?,?,?,?,?,?,?,?,?)
                    """,
                    [
                        (
                            10000 + index,
                            "BroadTieNeedle",
                            f"Fixture::{4100 - index:04d}::BroadTieNeedle",
                            "class",
                            3,
                            "class BroadTieNeedle",
                            1,
                            2,
                            "public",
                        )
                        for index in range(4100)
                    ],
                )
                conn.execute(
                    "INSERT INTO source_graph_nodes_fts(source_graph_nodes_fts) "
                    "VALUES('rebuild')"
                )
                conn.commit()

            process, tied = invoke(
                db_path,
                "search_crg_graph",
                "BroadTieNeedle",
                "--kind=Class",
                "--limit=5",
            )
            self.assertEqual(0, process.returncode, process.stderr)
            self.assertTrue(tied["used_fts"])
            self.assertTrue(tied["truncated"])
            self.assertEqual(4096, tied["limits"]["fts_candidate_limit_initial"])
            self.assertEqual(8192, tied["limits"]["fts_candidate_limit"])
            self.assertEqual(1, tied["limits"]["fts_candidate_expansions"])
            self.assertEqual(4100, tied["limits"]["fts_candidates"])
            self.assertFalse(tied["limits"]["fts_candidate_pool_truncated"])
            self.assertFalse(tied["limits"]["fts_full_rank_fallback"])
            self.assertEqual(
                [14099, 14098, 14097, 14096, 14095],
                [row["id"] for row in tied["results"]],
            )
            self.assertFalse((root / "graph.db").exists())

    def test_missing_schema_is_structured_and_retired_actions_are_unavailable(self) -> None:
        with tempfile.TemporaryDirectory(prefix="monolith-engine-source-crg-") as temp:
            root = Path(temp)
            db_path = root / "EngineSource.db"
            create_source_db(db_path, with_graph_search=False)

            process, payload = invoke(db_path, "search_crg_graph", "UObject")
            self.assertEqual(0, process.returncode, process.stderr)
            self.assertEqual("warning", payload["status"])
            self.assertEqual("engine_source_fts", payload["backend"])
            self.assertEqual([], payload["results"])
            self.assertEqual(0, payload["count"])
            self.assertFalse(payload["used_fts"])
            self.assertFalse(payload["truncated"])
            self.assertIn(
                "source.repair_fts --target=graph_nodes --execute",
                payload["next_actions"],
            )

            retired_flag = subprocess.run(
                [
                    str(QUERY_EXE),
                    "source",
                    "search_crg_graph",
                    "UObject",
                    f"--source_db={db_path}",
                    f"--graph_db={root / 'legacy-graph.db'}",
                    "--no-log",
                ],
                check=False,
                capture_output=True,
                text=True,
                encoding="utf-8",
                timeout=30,
            )
            self.assertNotEqual(0, retired_flag.returncode)
            self.assertIn(
                "--graph-db was removed",
                retired_flag.stdout + retired_flag.stderr,
            )

            for retired_action in (
                "build_crg_graph",
                "rebuild_crg_graph",
                "repair_crg_graph",
                "crg_graph_health",
            ):
                with self.subTest(retired_action=retired_action):
                    retired = subprocess.run(
                        [
                            str(QUERY_EXE),
                            "source",
                            retired_action,
                            f"--source_db={db_path}",
                            "--no-log",
                        ],
                        check=False,
                        capture_output=True,
                        text=True,
                        encoding="utf-8",
                        timeout=30,
                    )
                    self.assertNotEqual(0, retired.returncode)
                    retired_output = retired.stdout + retired.stderr
                    self.assertTrue(
                        "Unknown action" in retired_output
                        or (
                            '"status": "live_only"' in retired_output
                            and '"offline_supported": false' in retired_output
                        ),
                        retired_output,
                    )
            self.assertFalse((root / "graph.db").exists())

    def test_graph_db_is_rejected_globally_before_health_opens_any_database(self) -> None:
        with tempfile.TemporaryDirectory(prefix="monolith-engine-source-crg-") as temp:
            root = Path(temp)
            missing_db = root / "does-not-exist.db"
            retired_graph = root / "legacy-graph.db"
            process = subprocess.run(
                [
                    str(QUERY_EXE),
                    "source",
                    "health",
                    f"--source-db={missing_db}",
                    f"--graph-db={retired_graph}",
                    "--no-log",
                ],
                check=False,
                capture_output=True,
                text=True,
                encoding="utf-8",
                timeout=30,
            )
            self.assertNotEqual(0, process.returncode)
            self.assertIn("--graph-db was removed", process.stdout + process.stderr)
            self.assertFalse(missing_db.exists())
            self.assertFalse(retired_graph.exists())

            for help_args in (
                (
                    "source",
                    "health",
                    "--help",
                    f"--graph-db={retired_graph}",
                ),
                ("--help", f"--graph_db={retired_graph}"),
            ):
                with self.subTest(help_args=help_args):
                    help_process = subprocess.run(
                        [str(QUERY_EXE), *help_args, "--no-log"],
                        check=False,
                        capture_output=True,
                        text=True,
                        encoding="utf-8",
                        timeout=30,
                    )
                    self.assertNotEqual(0, help_process.returncode)
                    self.assertIn(
                        "--graph-db was removed",
                        help_process.stdout + help_process.stderr,
                    )
                    self.assertFalse(retired_graph.exists())

    def test_explicit_query_option_preserves_a_leading_dash_search_value(self) -> None:
        with tempfile.TemporaryDirectory(prefix="monolith-engine-source-crg-") as temp:
            db_path = Path(temp) / "EngineSource.db"
            create_source_db(db_path)

            process, payload = invoke(
                db_path,
                "search_crg_graph",
                "--query=--graph-db",
            )
            self.assertEqual(0, process.returncode, process.stderr)
            self.assertEqual("ok", payload["status"], payload)
            self.assertEqual("--graph-db", payload["input"]["query"])

    def test_graph_nodes_repair_rebuilds_the_external_content_index(self) -> None:
        with tempfile.TemporaryDirectory(prefix="monolith-engine-source-crg-") as temp:
            db_path = Path(temp) / "EngineSource.db"
            create_source_db(db_path)
            with closing(sqlite3.connect(db_path)) as conn:
                conn.execute(
                    """
                    INSERT INTO symbols(
                        id,name,qualified_name,kind,file_id,signature,line_start,line_end,access
                    ) VALUES(20,'LateIndexedSymbol','Fixture::LateIndexedSymbol','class',3,
                             'class LateIndexedSymbol',70,72,'public')
                    """
                )
                conn.commit()

            process, stale = invoke(db_path, "search_crg_graph", "LateIndexedSymbol")
            self.assertEqual(0, process.returncode, process.stderr)
            self.assertFalse(stale["used_fts"])
            self.assertEqual([20], [row["id"] for row in stale["results"]])

            process, repaired = invoke(
                db_path,
                "repair_fts",
                "--target=graph_nodes",
                "--execute=true",
            )
            self.assertEqual(0, process.returncode, process.stderr)
            self.assertEqual("ok", repaired["status"])
            self.assertEqual("Rebuilt FTS tables", repaired["summary"])

            process, fresh = invoke(db_path, "search_crg_graph", "LateIndexedSymbol")
            self.assertEqual(0, process.returncode, process.stderr)
            self.assertTrue(fresh["used_fts"])
            self.assertEqual([20], [row["id"] for row in fresh["results"]])

    def test_graph_nodes_repair_recreates_missing_objects_and_stamps_versions(self) -> None:
        with tempfile.TemporaryDirectory(prefix="monolith-engine-source-crg-") as temp:
            db_path = Path(temp) / "EngineSource.db"
            create_source_db(db_path, with_graph_search=False)
            with closing(sqlite3.connect(db_path)) as conn:
                conn.execute(
                    "UPDATE meta SET value='3' WHERE key='schema_version'"
                )
                conn.execute(
                    "DELETE FROM meta WHERE key='source_graph_nodes_fts_version'"
                )
                conn.commit()

            process, health = invoke(db_path, "health")
            self.assertEqual(0, process.returncode, process.stderr)
            self.assertIn(
                "source.repair_fts --target=graph_nodes",
                health["next_actions"],
            )

            process, repaired = invoke(
                db_path,
                "repair_fts",
                "--target=graph_nodes",
                "--execute=true",
            )
            self.assertEqual(0, process.returncode, process.stderr)
            self.assertEqual("ok", repaired["status"], repaired)
            self.assertEqual("ok", repaired["integrity"]["after"]["status"])

            with closing(sqlite3.connect(db_path)) as conn:
                objects = {
                    (row[0], row[1])
                    for row in conn.execute(
                        "SELECT type,name FROM sqlite_master "
                        "WHERE name LIKE 'source_graph_nodes%'"
                    )
                }
                marker = conn.execute(
                    "SELECT value FROM meta WHERE key='source_graph_nodes_fts_version'"
                ).fetchone()[0]
                schema = conn.execute(
                    "SELECT value FROM meta WHERE key='schema_version'"
                ).fetchone()[0]
                trigger_count = conn.execute(
                    "SELECT COUNT(*) FROM sqlite_master "
                    "WHERE type='trigger' AND name LIKE 'source_graph_nodes_%'"
                ).fetchone()[0]
            self.assertIn(("view", "source_graph_nodes"), objects)
            self.assertIn(("table", "source_graph_nodes_fts"), objects)
            self.assertEqual(9, trigger_count)
            self.assertEqual("1", marker)
            self.assertEqual("4", schema)

            process, searchable = invoke(db_path, "search_crg_graph", "UObject")
            self.assertEqual(0, process.returncode, process.stderr)
            self.assertTrue(searchable["used_fts"])
            self.assertEqual([10], [row["id"] for row in searchable["results"]])

    def test_graph_nodes_repair_target_is_case_insensitive(self) -> None:
        with tempfile.TemporaryDirectory(prefix="monolith-engine-source-crg-") as temp:
            db_path = Path(temp) / "EngineSource.db"
            create_source_db(db_path, with_graph_search=False)

            process, repaired = invoke(
                db_path,
                "repair_fts",
                "--target=Graph_Nodes",
                "--execute=true",
            )
            self.assertEqual(0, process.returncode, process.stderr)
            self.assertEqual("ok", repaired["status"], repaired)
            self.assertEqual("graph_nodes", repaired["input"]["target"])
            self.assertEqual("graph_nodes", repaired["limits"]["target"])
            with closing(sqlite3.connect(db_path)) as conn:
                object_type = conn.execute(
                    "SELECT type FROM sqlite_master WHERE name='source_graph_nodes'"
                ).fetchone()[0]
                indexed_docs = conn.execute(
                    "SELECT COUNT(*) FROM source_graph_nodes_fts_docsize"
                ).fetchone()[0]
            self.assertEqual("view", object_type)
            self.assertEqual(9, indexed_docs)

    def test_graph_nodes_repair_recovers_opposite_sqlite_object_types(self) -> None:
        with tempfile.TemporaryDirectory(prefix="monolith-engine-source-crg-") as temp:
            db_path = Path(temp) / "EngineSource.db"
            create_source_db(db_path)
            with closing(sqlite3.connect(db_path)) as conn:
                trigger_names = [
                    row[0]
                    for row in conn.execute(
                        "SELECT name FROM sqlite_master "
                        "WHERE type='trigger' AND name LIKE 'source_graph_nodes_%'"
                    )
                ]
                for trigger_name in trigger_names:
                    conn.execute(f'DROP TRIGGER "{trigger_name}"')
                conn.executescript(
                    """
                    DROP TABLE source_graph_nodes_fts;
                    DROP VIEW source_graph_nodes;
                    CREATE TABLE source_graph_nodes(
                        id INTEGER PRIMARY KEY,
                        name TEXT
                    );
                    CREATE VIEW source_graph_nodes_fts AS
                    SELECT id AS rowid,name FROM source_graph_nodes;
                    """
                )
                conn.commit()

            process, repaired = invoke(
                db_path,
                "repair_fts",
                "--target=graph_nodes",
                "--execute=true",
            )
            self.assertEqual(0, process.returncode, process.stderr)
            self.assertEqual("ok", repaired["status"], repaired)

            with closing(sqlite3.connect(db_path)) as conn:
                object_types = dict(
                    conn.execute(
                        "SELECT name,type FROM sqlite_master "
                        "WHERE name IN ('source_graph_nodes','source_graph_nodes_fts')"
                    )
                )
                trigger_count = conn.execute(
                    "SELECT COUNT(*) FROM sqlite_master "
                    "WHERE type='trigger' AND name LIKE 'source_graph_nodes_%'"
                ).fetchone()[0]
                indexed_docs = conn.execute(
                    "SELECT COUNT(*) FROM source_graph_nodes_fts_docsize"
                ).fetchone()[0]
            self.assertEqual("view", object_types["source_graph_nodes"])
            self.assertEqual("table", object_types["source_graph_nodes_fts"])
            self.assertEqual(9, trigger_count)
            self.assertEqual(9, indexed_docs)

            process, searchable = invoke(db_path, "search_crg_graph", "UObject")
            self.assertEqual(0, process.returncode, process.stderr)
            self.assertTrue(searchable["used_fts"])
            self.assertEqual([10], [row["id"] for row in searchable["results"]])

    def test_console_objects_repair_creates_and_rebuilds_shared_schema(self) -> None:
        with tempfile.TemporaryDirectory(prefix="monolith-engine-source-crg-") as temp:
            db_path = Path(temp) / "EngineSource.db"
            create_source_db(db_path)

            process, created = invoke(
                db_path,
                "repair_fts",
                "--target=console_objects",
                "--execute=true",
            )
            self.assertEqual(0, process.returncode, process.stderr)
            self.assertEqual("ok", created["status"], created)
            with closing(sqlite3.connect(db_path)) as conn:
                object_types = dict(
                    conn.execute(
                        "SELECT name,type FROM sqlite_master "
                        "WHERE name IN ('console_objects','console_objects_fts')"
                    )
                )
                trigger_count = conn.execute(
                    "SELECT COUNT(*) FROM sqlite_master "
                    "WHERE type='trigger' AND name LIKE 'console_objects_%'"
                ).fetchone()[0]
                conn.execute(
                    "INSERT INTO console_objects(name,object_type,help,captured_at) "
                    "VALUES('r.MonolithFixture','variable','ConsoleRepairNeedle','2026-07-21T00:00:00Z')"
                )
                rowid = conn.execute(
                    "SELECT rowid FROM console_objects WHERE name='r.MonolithFixture'"
                ).fetchone()[0]
                conn.execute(
                    "DELETE FROM console_objects_fts WHERE rowid=?",
                    (rowid,),
                )
                conn.commit()
                stale_docs = conn.execute(
                    "SELECT COUNT(*) FROM console_objects_fts_docsize"
                ).fetchone()[0]
            self.assertEqual("table", object_types["console_objects"])
            self.assertEqual("table", object_types["console_objects_fts"])
            self.assertEqual(3, trigger_count)
            self.assertEqual(0, stale_docs)

            process, rebuilt = invoke(
                db_path,
                "repair_fts",
                "--target=Console_Objects",
                "--execute=true",
            )
            self.assertEqual(0, process.returncode, process.stderr)
            self.assertEqual("ok", rebuilt["status"], rebuilt)
            self.assertEqual("console_objects", rebuilt["input"]["target"])
            self.assertEqual("console_objects", rebuilt["limits"]["target"])
            with closing(sqlite3.connect(db_path)) as conn:
                indexed_docs = conn.execute(
                    "SELECT COUNT(*) FROM console_objects_fts_docsize"
                ).fetchone()[0]
                matches = conn.execute(
                    "SELECT COUNT(*) FROM console_objects_fts "
                    "WHERE console_objects_fts MATCH 'ConsoleRepairNeedle'"
                ).fetchone()[0]
            self.assertEqual(1, indexed_docs)
            self.assertEqual(1, matches)

    def test_all_repair_also_recreates_missing_graph_search_schema(self) -> None:
        with tempfile.TemporaryDirectory(prefix="monolith-engine-source-crg-") as temp:
            db_path = Path(temp) / "EngineSource.db"
            create_source_db(db_path, with_graph_search=False)

            process, repaired = invoke(
                db_path,
                "repair_fts",
                "--target=all",
                "--execute=true",
            )
            self.assertEqual(0, process.returncode, process.stderr)
            self.assertEqual("ok", repaired["status"], repaired)
            self.assertEqual("Rebuilt FTS tables", repaired["summary"])
            with closing(sqlite3.connect(db_path)) as conn:
                trigger_count = conn.execute(
                    "SELECT COUNT(*) FROM sqlite_master "
                    "WHERE type='trigger' AND name LIKE 'source_graph_nodes_%'"
                ).fetchone()[0]
                indexed_docs = conn.execute(
                    "SELECT COUNT(*) FROM source_graph_nodes_fts_docsize"
                ).fetchone()[0]
                console_types = dict(
                    conn.execute(
                        "SELECT name,type FROM sqlite_master "
                        "WHERE name IN ('console_objects','console_objects_fts')"
                    )
                )
                console_trigger_count = conn.execute(
                    "SELECT COUNT(*) FROM sqlite_master "
                    "WHERE type='trigger' AND name LIKE 'console_objects_%'"
                ).fetchone()[0]
            self.assertEqual(9, trigger_count)
            self.assertEqual(9, indexed_docs)
            self.assertEqual("table", console_types["console_objects"])
            self.assertEqual("table", console_types["console_objects_fts"])
            self.assertEqual(3, console_trigger_count)

    def test_health_rejects_a_symbols_only_graph_view_even_when_fts_matches(self) -> None:
        with tempfile.TemporaryDirectory(prefix="monolith-engine-source-crg-") as temp:
            db_path = Path(temp) / "EngineSource.db"
            create_source_db(db_path)
            with closing(sqlite3.connect(db_path)) as conn:
                conn.executescript(
                    """
                    DROP TABLE source_graph_nodes_fts;
                    DROP VIEW source_graph_nodes;
                    CREATE VIEW source_graph_nodes AS
                    SELECT s.id AS id,
                           'Symbol' AS kind,
                           COALESCE(NULLIF(s.name,''),'symbol#' || s.id) AS name,
                           COALESCE(NULLIF(s.qualified_name,''),
                                    COALESCE(f.path,'') || '::' || COALESCE(s.name,'symbol'))
                               || '#' || s.id AS qualified_name,
                           COALESCE(f.path,'') AS file_path,
                           COALESCE(s.line_start,0) AS line_start,
                           COALESCE(s.line_end,0) AS line_end,
                           'cpp' AS language,
                           s.signature AS signature
                    FROM symbols s
                    LEFT JOIN files f ON f.id=s.file_id;
                    CREATE VIRTUAL TABLE source_graph_nodes_fts USING fts5(
                        name,qualified_name,file_path,signature,
                        content='source_graph_nodes',content_rowid='id',
                        tokenize='porter unicode61'
                    );
                    INSERT INTO source_graph_nodes_fts(source_graph_nodes_fts)
                    VALUES('rebuild');
                    """
                )
                conn.commit()

            process, shallow = invoke(db_path, "health")
            self.assertEqual(0, process.returncode, process.stderr)
            shallow_checks = {row["check"]: row for row in shallow["checks"]}
            self.assertEqual(
                "warning",
                shallow_checks["fts:graph_nodes_readiness"]["result"],
            )
            self.assertIn(
                "source.repair_fts --target=graph_nodes",
                shallow["next_actions"],
            )

            process, deep = invoke(
                db_path,
                "health",
                "--include-deep-checks=true",
            )
            self.assertEqual(0, process.returncode, process.stderr)
            deep_checks = {row["check"]: row for row in deep["checks"]}
            self.assertEqual(
                "warning",
                deep_checks["fts:graph_nodes_projection_parity"]["result"],
            )
            self.assertEqual(
                "ok",
                deep_checks["fts:graph_nodes_row_parity"]["result"],
            )

    def test_graph_repair_failure_rolls_back_recreate_and_rebuild(self) -> None:
        with tempfile.TemporaryDirectory(prefix="monolith-engine-source-crg-") as temp:
            db_path = Path(temp) / "EngineSource.db"
            create_source_db(db_path)
            with closing(sqlite3.connect(db_path)) as conn:
                conn.execute("DELETE FROM source_graph_nodes_fts WHERE rowid=13")
                stale_docs = conn.execute(
                    "SELECT COUNT(*) FROM source_graph_nodes_fts_docsize"
                ).fetchone()[0]
                conn.executescript(
                    """
                    DROP TABLE meta;
                    CREATE VIEW meta AS
                    SELECT 'schema_version' AS key, '4' AS value;
                    """
                )
                conn.commit()
            self.assertEqual(8, stale_docs)

            process, failed = invoke(
                db_path,
                "repair_fts",
                "--target=graph_nodes",
                "--execute=true",
            )
            self.assertEqual(0, process.returncode, process.stderr)
            self.assertEqual("error", failed["status"], failed)
            self.assertIn("rolled back", failed["summary"])

            with closing(sqlite3.connect(db_path)) as conn:
                docs_after = conn.execute(
                    "SELECT COUNT(*) FROM source_graph_nodes_fts_docsize"
                ).fetchone()[0]
                object_types = dict(
                    conn.execute(
                        "SELECT name,type FROM sqlite_master "
                        "WHERE name IN ('source_graph_nodes','source_graph_nodes_fts','meta')"
                    )
                )
            self.assertEqual(stale_docs, docs_after)
            self.assertEqual("view", object_types["source_graph_nodes"])
            self.assertEqual("table", object_types["source_graph_nodes_fts"])
            self.assertEqual("view", object_types["meta"])

    def test_shallow_health_detects_negative_file_row_drift(self) -> None:
        with tempfile.TemporaryDirectory(prefix="monolith-engine-source-crg-") as temp:
            db_path = Path(temp) / "EngineSource.db"
            create_source_db(db_path)
            with closing(sqlite3.connect(db_path)) as conn:
                conn.execute("DELETE FROM source_graph_nodes_fts WHERE rowid=-1")
                conn.commit()

            process, health = invoke(db_path, "health")
            self.assertEqual(0, process.returncode, process.stderr)
            self.assertEqual("shallow", health["check_depth"])
            checks = {row["check"]: row for row in health["checks"]}
            self.assertEqual(
                "warning",
                checks["fts:graph_nodes_readiness"]["result"],
            )
            self.assertIn(
                "source.repair_fts --target=graph_nodes",
                health["next_actions"],
            )

    def test_symbols_repair_recreates_missing_maintenance_trigger(self) -> None:
        with tempfile.TemporaryDirectory(prefix="monolith-engine-source-crg-") as temp:
            db_path = Path(temp) / "EngineSource.db"
            create_source_db(db_path)
            with closing(sqlite3.connect(db_path)) as conn:
                conn.execute("DROP TRIGGER symbols_au")
                conn.commit()

            process, health = invoke(db_path, "health")
            self.assertEqual(0, process.returncode, process.stderr)
            checks = {row["check"]: row for row in health["checks"]}
            self.assertEqual("warning", checks["trigger:symbols_au"]["result"])
            self.assertIn(
                "source.repair_fts --target=symbols",
                health["next_actions"],
            )

            process, repaired = invoke(
                db_path,
                "repair_fts",
                "--target=symbols",
                "--execute=true",
            )
            self.assertEqual(0, process.returncode, process.stderr)
            self.assertEqual("ok", repaired["status"], repaired)

            with closing(sqlite3.connect(db_path)) as conn:
                trigger_type = conn.execute(
                    "SELECT type FROM sqlite_master WHERE name='symbols_au'"
                ).fetchone()
            self.assertEqual(("trigger",), trigger_type)

            process, health = invoke(db_path, "health")
            self.assertEqual(0, process.returncode, process.stderr)
            checks = {row["check"]: row for row in health["checks"]}
            self.assertEqual("ok", checks["trigger:symbols_au"]["result"])
            self.assertNotIn(
                "source.repair_fts --target=symbols",
                health["next_actions"],
            )

    def test_missing_graph_fts_marker_is_health_gated_and_repair_stamped(self) -> None:
        with tempfile.TemporaryDirectory(prefix="monolith-engine-source-crg-") as temp:
            db_path = Path(temp) / "EngineSource.db"
            create_source_db(db_path)
            with closing(sqlite3.connect(db_path)) as conn:
                conn.execute(
                    "DELETE FROM meta WHERE key='source_graph_nodes_fts_version'"
                )
                conn.commit()

            process, health = invoke(db_path, "health")
            self.assertEqual(0, process.returncode, process.stderr)
            checks = {row["check"]: row for row in health["checks"]}
            self.assertEqual(
                "warning",
                checks["meta:source_graph_nodes_fts_version"]["result"],
            )
            self.assertIn(
                "source.repair_fts --target=graph_nodes",
                health["next_actions"],
            )
            self.assertTrue(
                health["maintenance_recommendation"][
                    "repair_graph_nodes_fts_required"
                ]
            )
            self.assertNotIn(
                "repair_graph_node_fts_required",
                health["maintenance_recommendation"],
            )
            self.assertNotIn("source.repair_fts --target=symbols", health["next_actions"])
            self.assertNotIn("source.trigger_reindex", health["next_actions"])

            process, repaired = invoke(
                db_path,
                "repair_fts",
                "--target=graph_nodes",
                "--execute=true",
            )
            self.assertEqual(0, process.returncode, process.stderr)
            self.assertEqual("ok", repaired["status"], repaired)
            with closing(sqlite3.connect(db_path)) as conn:
                marker = conn.execute(
                    "SELECT value FROM meta WHERE key='source_graph_nodes_fts_version'"
                ).fetchone()[0]
                schema = conn.execute(
                    "SELECT value FROM meta WHERE key='schema_version'"
                ).fetchone()[0]
            self.assertEqual("1", marker)
            self.assertEqual("4", schema)

    def test_scoped_maintenance_routing_avoids_overlapping_repairs(self) -> None:
        with tempfile.TemporaryDirectory(prefix="monolith-engine-source-crg-") as temp:
            db_path = Path(temp) / "EngineSource.db"
            create_source_db(db_path)

            process, initial_repair = invoke(
                db_path,
                "repair_crg_cache",
                "--scope=all",
                "--execute=true",
            )
            self.assertEqual(0, process.returncode, process.stderr)
            self.assertEqual("ok", initial_repair["status"], initial_repair)

            with closing(sqlite3.connect(db_path)) as conn:
                conn.execute(
                    "INSERT OR REPLACE INTO crg_meta(key,value) "
                    "VALUES('source_override_edges_version','stale')"
                )
                conn.commit()

            process, override_health = invoke(
                db_path,
                "health",
                "--include-deep-checks=true",
            )
            self.assertEqual(0, process.returncode, process.stderr)
            override_maintenance = override_health["maintenance_recommendation"]
            self.assertFalse(override_maintenance["repair_crg_cache_required"])
            self.assertTrue(override_maintenance["repair_override_edges_required"])
            self.assertIn(
                "source.repair_crg_cache --scope=override_edges",
                override_health["next_actions"],
            )
            self.assertNotIn(
                "source.repair_crg_cache --scope=all",
                override_health["next_actions"],
            )

            process, override_dry_run = invoke(
                db_path,
                "repair_crg_cache",
                "--scope=override_edges",
            )
            self.assertEqual(0, process.returncode, process.stderr)
            self.assertIn(
                "source.repair_crg_cache --scope=override_edges --execute",
                override_dry_run["next_actions"],
            )
            self.assertNotIn(
                "source.repair_crg_cache --scope=all --execute",
                override_dry_run["next_actions"],
            )

            process, override_repair = invoke(
                db_path,
                "repair_crg_cache",
                "--scope=override_edges",
                "--execute=true",
            )
            self.assertEqual(0, process.returncode, process.stderr)
            self.assertEqual("ok", override_repair["status"], override_repair)

            with closing(sqlite3.connect(db_path)) as conn:
                conn.execute("DELETE FROM crg_node_metrics WHERE node_id=10")
                conn.execute(
                    "INSERT OR REPLACE INTO crg_meta(key,value) "
                    "VALUES('source_override_edges_version','stale')"
                )
                conn.commit()

            process, full_health = invoke(
                db_path,
                "health",
                "--include-deep-checks=true",
            )
            self.assertEqual(0, process.returncode, process.stderr)
            full_maintenance = full_health["maintenance_recommendation"]
            self.assertTrue(full_maintenance["repair_crg_cache_required"])
            self.assertTrue(full_maintenance["repair_override_edges_required"])
            self.assertIn(
                "source.repair_crg_cache --scope=all",
                full_health["next_actions"],
            )
            self.assertNotIn(
                "source.repair_crg_cache --scope=override_edges",
                full_health["next_actions"],
            )

            process, full_dry_run = invoke(
                db_path,
                "repair_crg_cache",
                "--scope=all",
            )
            self.assertEqual(0, process.returncode, process.stderr)
            self.assertIn(
                "source.repair_crg_cache --scope=all --execute",
                full_dry_run["next_actions"],
            )
            self.assertNotIn(
                "source.repair_crg_cache --scope=override_edges --execute",
                full_dry_run["next_actions"],
            )

            process, full_repair = invoke(
                db_path,
                "repair_crg_cache",
                "--scope=all",
                "--execute=true",
            )
            self.assertEqual(0, process.returncode, process.stderr)
            self.assertEqual("ok", full_repair["status"], full_repair)

            with closing(sqlite3.connect(db_path)) as conn:
                conn.execute("DROP TRIGGER source_graph_nodes_symbols_au")
                conn.commit()

            process, graph_health = invoke(
                db_path,
                "health",
                "--include-deep-checks=true",
            )
            self.assertEqual(0, process.returncode, process.stderr)
            graph_maintenance = graph_health["maintenance_recommendation"]
            self.assertTrue(graph_maintenance["repair_graph_nodes_fts_required"])
            self.assertFalse(graph_maintenance["repair_crg_cache_required"])
            self.assertFalse(graph_maintenance["repair_override_edges_required"])
            self.assertIn(
                "source.repair_fts --target=graph_nodes",
                graph_health["next_actions"],
            )
            self.assertNotIn(
                "source.repair_crg_cache --scope=all",
                graph_health["next_actions"],
            )
            self.assertNotIn(
                "source.repair_crg_cache --scope=override_edges",
                graph_health["next_actions"],
            )

    def test_deep_health_uses_index_owned_docsize_parity(self) -> None:
        with tempfile.TemporaryDirectory(prefix="monolith-engine-source-crg-") as temp:
            db_path = Path(temp) / "EngineSource.db"
            create_source_db(db_path)

            process, crg_repair = invoke(
                db_path,
                "repair_crg_cache",
                "--execute=true",
            )
            self.assertEqual(0, process.returncode, process.stderr)
            self.assertEqual("ok", crg_repair["status"], crg_repair)

            with closing(sqlite3.connect(db_path)) as conn:
                content_count = conn.execute(
                    "SELECT COUNT(*) FROM source_graph_nodes"
                ).fetchone()[0]
                conn.execute("DELETE FROM source_graph_nodes_fts WHERE rowid=13")
                conn.commit()
                apparent_fts_count = conn.execute(
                    "SELECT COUNT(*) FROM source_graph_nodes_fts"
                ).fetchone()[0]
            self.assertEqual(
                content_count,
                apparent_fts_count,
                "external-content COUNT parity should still look healthy in this fixture",
            )

            process, health = invoke(
                db_path,
                "health",
                "--include-deep-checks=true",
            )
            self.assertEqual(0, process.returncode, process.stderr)
            checks = {row["check"]: row for row in health["checks"]}
            self.assertEqual("ok", checks["fts:graph_nodes_readiness"]["result"])
            self.assertEqual(
                "warning", checks["fts:graph_nodes_row_parity"]["result"]
            )
            self.assertIn(
                "source.repair_fts --target=graph_nodes",
                health["next_actions"],
            )

            process, repaired = invoke(
                db_path,
                "repair_fts",
                "--target=graph_nodes",
                "--execute=true",
            )
            self.assertEqual(0, process.returncode, process.stderr)
            self.assertEqual("ok", repaired["status"], repaired)
            self.assertEqual("ok", repaired["integrity"]["after"]["status"])

            process, healthy = invoke(
                db_path,
                "health",
                "--include-deep-checks=true",
            )
            self.assertEqual(0, process.returncode, process.stderr)
            checks = {row["check"]: row for row in healthy["checks"]}
            self.assertEqual(
                "ok",
                checks["fts:graph_nodes_readiness"]["result"],
            )
            self.assertEqual("ok", checks["fts:graph_nodes_row_parity"]["result"])


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--query-exe", type=Path, required=True)
    args, unittest_args = parser.parse_known_args()
    global QUERY_EXE
    QUERY_EXE = args.query_exe.resolve()
    if not QUERY_EXE.is_file():
        parser.error(f"query executable not found: {QUERY_EXE}")
    program = unittest.main(
        argv=[sys.argv[0], *unittest_args],
        verbosity=2,
        exit=False,
    )
    return 0 if program.result.wasSuccessful() else 1


if __name__ == "__main__":
    raise SystemExit(main())
