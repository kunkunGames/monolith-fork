#!/usr/bin/env python3
"""Integration coverage for Query CRG repair write-preflight and atomicity."""

from __future__ import annotations

import argparse
from contextlib import closing
import ctypes
import json
import os
from pathlib import Path
import sqlite3
import subprocess
import sys
import tempfile
import unittest


QUERY_EXE: Path


def create_minimal_source_db(path: Path) -> None:
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
            """
        )
        conn.executescript(
            """
            INSERT INTO modules(id,name) VALUES(1,'Fixture');
            INSERT INTO files(id,path,module_id,line_count,file_type)
            VALUES(1,'Fixture.cpp',1,10,'cpp');
            INSERT INTO symbols(id,name,qualified_name,kind,file_id,signature,line_start,line_end,access)
            VALUES(1,'FixtureFunction','FixtureFunction','function',1,'void FixtureFunction()',1,3,'public');
            """
        )
        conn.commit()


def run_repair(db_path: Path) -> tuple[subprocess.CompletedProcess[str], dict[str, object]]:
    completed = subprocess.run(
        [
            str(QUERY_EXE),
            "source",
            "repair_crg_cache",
            f"--db={db_path}",
            "--execute=true",
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


@unittest.skipUnless(os.name == "nt", "Win32 file-sharing contract")
class CrgRepairWritePreflightTests(unittest.TestCase):
    def test_share_denial_is_actionable_atomic_and_retryable(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            db_path = Path(temp_dir) / "EngineSource.db"
            create_minimal_source_db(db_path)

            kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
            create_file = kernel32.CreateFileW
            create_file.argtypes = [
                ctypes.c_wchar_p,
                ctypes.c_uint32,
                ctypes.c_uint32,
                ctypes.c_void_p,
                ctypes.c_uint32,
                ctypes.c_uint32,
                ctypes.c_void_p,
            ]
            create_file.restype = ctypes.c_void_p
            close_handle = kernel32.CloseHandle
            close_handle.argtypes = [ctypes.c_void_p]
            close_handle.restype = ctypes.c_int

            generic_read = 0x80000000
            file_share_read = 0x00000001
            file_share_delete = 0x00000004
            open_existing = 3
            handle = create_file(
                str(db_path),
                generic_read,
                file_share_read | file_share_delete,
                None,
                open_existing,
                0,
                None,
            )
            invalid_handle = ctypes.c_void_p(-1).value
            self.assertNotIn(handle, (None, invalid_handle), ctypes.get_last_error())
            try:
                blocked_process, blocked = run_repair(db_path)
            finally:
                self.assertTrue(close_handle(handle))

            self.assertEqual(blocked_process.returncode, 0, blocked_process.stderr)
            self.assertEqual(blocked["status"], "error")
            self.assertIn("write access is unavailable", blocked["summary"])
            self.assertEqual(blocked["write_preflight"]["status"], "unavailable")
            self.assertIn("Close UnrealEditor", blocked["write_preflight"]["remediation"])
            self.assertIn("source.repair_crg_cache --scope=all --execute", blocked["next_actions"])

            with closing(sqlite3.connect(db_path)) as conn:
                created_while_blocked = conn.execute(
                    "SELECT COUNT(*) FROM sqlite_master WHERE name LIKE 'crg_%'"
                ).fetchone()[0]
            self.assertEqual(created_while_blocked, 0, "blocked repair must not create partial CRG schema")
            self.assertFalse(Path(f"{db_path}-journal").exists())

            retry_process, retry = run_repair(db_path)
            self.assertEqual(retry_process.returncode, 0, retry_process.stderr)
            self.assertEqual(retry["status"], "ok", retry)
            self.assertEqual(retry["write_preflight"]["status"], "ok")
            with closing(sqlite3.connect(db_path)) as conn:
                tables = {
                    row[0]
                    for row in conn.execute(
                        "SELECT name FROM sqlite_master WHERE type='table' AND name LIKE 'crg_%'"
                    )
                }
                versions = dict(conn.execute("SELECT key,value FROM crg_meta"))
            self.assertTrue({"crg_nodes", "crg_edges", "crg_node_metrics", "crg_meta"}.issubset(tables))
            self.assertEqual(versions["cache_version"], "1")
            self.assertEqual(versions["scoring_version"], "3")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--query-exe", type=Path, required=True)
    args, unittest_args = parser.parse_known_args()
    global QUERY_EXE
    QUERY_EXE = args.query_exe.resolve()
    if not QUERY_EXE.is_file():
        parser.error(f"query executable not found: {QUERY_EXE}")
    program = unittest.main(argv=[sys.argv[0], *unittest_args], verbosity=2, exit=False)
    return 0 if program.result.wasSuccessful() else 1


if __name__ == "__main__":
    raise SystemExit(main())
