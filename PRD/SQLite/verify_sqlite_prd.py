#!/usr/bin/env python3
"""Static and SQLite smoke checks for the SQLite PRD implementation."""

from __future__ import annotations

import re
import sqlite3
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / "Source"


def read(rel: str) -> str:
    return (ROOT / rel).read_text(encoding="utf-8")


def fail(message: str) -> None:
    raise AssertionError(message)


def test_query_connection_is_read_only() -> None:
    header = read("Source/MonolithIndex/Public/MonolithIndexSubsystem.h")
    source = read("Source/MonolithIndex/Private/MonolithIndexSubsystem.cpp")
    if "QueryDatabase" not in header:
        fail("UMonolithIndexSubsystem must own a separate query-only database connection.")
    if "OpenQueryDatabase" not in source:
        fail("UMonolithIndexSubsystem must have a helper that opens the query database.")
    if not re.search(r"QueryDatabase->OpenForQuery\(", source):
        fail("Query database must open through FMonolithIndexDatabase::OpenForQuery().")
    if "GetQueryDatabaseForRead" not in source:
        fail("Query wrappers must resolve a query-only database before falling back to the writer.")
    if not re.search(r"return\s+ReadDatabase->FullTextSearch", source):
        fail("Search() must route through the resolved query-only connection when available.")


def test_query_connection_uses_stable_database_file() -> None:
    header = read("Source/MonolithIndex/Public/MonolithIndexDatabase.h")
    source = read("Source/MonolithIndex/Private/MonolithIndexSubsystem.cpp")
    db_source = read("Source/MonolithIndex/Private/MonolithIndexDatabase.cpp")
    core_source = read("Source/MonolithCore/Private/MonolithSQLitePragmaPolicy.cpp")
    if "ConvertRelativePathToFull" not in source or "NormalizeFilename" not in source:
        fail("Project index path must be absolute and normalized before SQLite open.")
    if "ConvertRelativePathToFull" not in core_source or "NormalizeFilename" not in core_source:
        fail("Shared SQLite open helper must normalize paths before FSQLiteDatabase::Open().")
    if "FileSize(*DbPath)" not in source:
        fail("Query DB open must reject missing or zero-byte index files before read-only open.")
    if "ClearStatementCache" not in header + db_source + source:
        fail("Writer prepared statements must be releasable before opening the read-only query connection.")
    if "CloseWriteDatabase();" not in source:
        fail("Writer connection must close before opening the read-only query database.")
    if (
        "FMonolithIndexDatabase* UMonolithIndexSubsystem::GetDatabase()" not in source
        or "return GetQueryDatabaseForRead();" not in source
    ):
        fail("Public GetDatabase() must expose the query connection, not the writer connection.")
    if "OpenWriteDatabase()" not in source:
        fail("Indexing and live updates must reopen the writer connection explicitly.")


def test_full_text_search_uses_single_comparable_score() -> None:
    source = read("Source/MonolithIndex/Private/MonolithIndexDatabase.cpp")
    match = re.search(
        r"TArray<FSearchResult> FMonolithIndexDatabase::FullTextSearch.*?^}",
        source,
        flags=re.S | re.M,
    )
    if not match:
        fail("FullTextSearch implementation not found.")
    body = match.group(0)
    if "UNION ALL" not in body:
        fail("FullTextSearch must use a single UNION ALL query rather than two independent LIMIT queries.")
    if "source_weight" not in body:
        fail("FullTextSearch must normalize cross-table ranking with an explicit source_weight.")
    if body.count("LIMIT") != 1:
        fail("FullTextSearch must apply the caller limit once after the union.")
    if re.search(r"Stmt2\b", body):
        fail("FullTextSearch must not use a second statement for node results.")


def test_persistent_statement_cache_present() -> None:
    index_header = read("Source/MonolithIndex/Public/MonolithIndexDatabase.h")
    source_header = read("Source/MonolithSource/Public/MonolithSourceDatabase.h")
    all_source = "\n".join(p.read_text(encoding="utf-8", errors="ignore") for p in SOURCE.rglob("*.[ch]pp"))
    if "FMonolithSQLiteStatementCache" not in index_header + source_header:
        fail("Index and Source DBs must own a FMonolithSQLiteStatementCache.")
    cache_calls = all_source.count("StatementCache.FindOrCreate")
    if cache_calls < 14:
        fail(f"Expected at least 14 statement-cache call sites, found {cache_calls}.")
    if "ESQLitePreparedStatementFlags::Persistent" not in all_source:
        fail("Statement cache must create persistent SQLite prepared statements.")


def test_available_ram_caps_pragma_preset() -> None:
    header = read("Source/MonolithCore/Public/MonolithSQLitePragmaPolicy.h")
    source = read("Source/MonolithCore/Private/MonolithSQLitePragmaPolicy.cpp")
    if "AvailableRAM_MB" not in header + source:
        fail("Pragma preset selection must accept available RAM.")
    if "AvailablePhysical" not in source:
        fail("OpenMonolithSQLiteDatabase must inspect FPlatformMemory::GetStats().AvailablePhysical.")
    if "CapPragmaPresetByAvailableMemory" not in source:
        fail("Pragma preset selection must cap mmap/cache requests by available memory.")


def test_camelcase_tokens_are_written_to_fts_payload() -> None:
    core_header = read("Source/MonolithCore/Public/MonolithSQLiteSearchText.h")
    index_source = read("Source/MonolithIndex/Private/MonolithIndexDatabase.cpp")
    source_schema = read("Source/MonolithSource/Private/MonolithSourceSchema.h")
    python_queries = read("Scripts/source_indexer/db/queries.py")
    if "BuildMonolithSQLiteSearchText" not in core_header + index_source:
        fail("CamelCase/PascalCase expansion must be centralized in MonolithCore and used by index writes.")
    if "search_tokens" not in index_source:
        fail("Project index FTS payload must include search_tokens.")
    if "search_tokens" not in source_schema + python_queries:
        fail("Source index C++ and Python schemas must include search_tokens.")


def test_sqlite_fts_smoke() -> None:
    conn = sqlite3.connect(":memory:")
    conn.execute(
        "CREATE TABLE symbols(id INTEGER PRIMARY KEY, name TEXT, qualified_name TEXT, docstring TEXT, search_tokens TEXT)"
    )
    conn.execute(
        "CREATE VIRTUAL TABLE symbols_fts USING fts5("
        "name, qualified_name, docstring, search_tokens, content=symbols, content_rowid=id, "
        "tokenize='unicode61 remove_diacritics 2', prefix='2 3 4')"
    )
    conn.executescript(
        """
        CREATE TRIGGER symbols_ai AFTER INSERT ON symbols BEGIN
          INSERT INTO symbols_fts(rowid, name, qualified_name, docstring, search_tokens)
          VALUES (new.id, new.name, new.qualified_name, new.docstring, new.search_tokens);
        END;
        CREATE TRIGGER symbols_au AFTER UPDATE ON symbols BEGIN
          INSERT INTO symbols_fts(symbols_fts, rowid, name, qualified_name, docstring, search_tokens)
          VALUES ('delete', old.id, old.name, old.qualified_name, old.docstring, old.search_tokens);
          INSERT INTO symbols_fts(rowid, name, qualified_name, docstring, search_tokens)
          VALUES (new.id, new.name, new.qualified_name, new.docstring, new.search_tokens);
        END;
        """
    )
    conn.execute(
        "INSERT INTO symbols(name, qualified_name, docstring, search_tokens) VALUES(?, ?, ?, ?)",
        ("MyPlayerCharacter", "MyPlayerCharacter", "", "MyPlayerCharacter My Player Character"),
    )
    assert conn.execute(
        "SELECT count(*) FROM symbols_fts WHERE symbols_fts MATCH 'MyP*'"
    ).fetchone()[0] == 1
    assert conn.execute(
        "SELECT count(*) FROM symbols_fts WHERE symbols_fts MATCH 'Player'"
    ).fetchone()[0] == 1


def main() -> int:
    tests = [
        test_query_connection_is_read_only,
        test_query_connection_uses_stable_database_file,
        test_full_text_search_uses_single_comparable_score,
        test_persistent_statement_cache_present,
        test_available_ram_caps_pragma_preset,
        test_camelcase_tokens_are_written_to_fts_payload,
        test_sqlite_fts_smoke,
    ]
    failures: list[str] = []
    for test in tests:
        try:
            test()
            print(f"PASS {test.__name__}")
        except Exception as exc:
            failures.append(f"FAIL {test.__name__}: {exc}")
            print(failures[-1])
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
