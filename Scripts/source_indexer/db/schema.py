"""SQLite schema for source_indexer."""

from __future__ import annotations

import os
import re
import sqlite3

SCHEMA_VERSION = 3
APPLICATION_ID = 0x4D4F4E4C

_DDL = """
-- Core tables ----------------------------------------------------------------

CREATE TABLE IF NOT EXISTS modules (
    id          INTEGER PRIMARY KEY AUTOINCREMENT,
    name        TEXT NOT NULL,
    path        TEXT NOT NULL,
    module_type TEXT NOT NULL,
    build_cs_path TEXT,
    UNIQUE(name, path)
);

CREATE TABLE IF NOT EXISTS files (
    id            INTEGER PRIMARY KEY AUTOINCREMENT,
    path          TEXT NOT NULL UNIQUE,
    module_id     INTEGER REFERENCES modules(id),
    file_type     TEXT NOT NULL,
    line_count    INTEGER NOT NULL DEFAULT 0,
    last_modified REAL NOT NULL DEFAULT 0.0
);

CREATE TABLE IF NOT EXISTS symbols (
    id               INTEGER PRIMARY KEY AUTOINCREMENT,
    name             TEXT NOT NULL,
    qualified_name   TEXT NOT NULL,
    kind             TEXT NOT NULL,
    file_id          INTEGER REFERENCES files(id),
    line_start       INTEGER,
    line_end         INTEGER,
    parent_symbol_id INTEGER REFERENCES symbols(id),
    access           TEXT,
    signature        TEXT,
    docstring        TEXT,
    search_tokens    TEXT DEFAULT '',
    is_ue_macro      INTEGER NOT NULL DEFAULT 0
);

CREATE INDEX IF NOT EXISTS idx_symbols_name            ON symbols(name);
CREATE INDEX IF NOT EXISTS idx_symbols_qualified_name  ON symbols(qualified_name);
CREATE INDEX IF NOT EXISTS idx_symbols_kind            ON symbols(kind);
CREATE INDEX IF NOT EXISTS idx_symbols_file_id         ON symbols(file_id);
CREATE INDEX IF NOT EXISTS idx_symbols_parent          ON symbols(parent_symbol_id);

CREATE TABLE IF NOT EXISTS inheritance (
    id        INTEGER PRIMARY KEY AUTOINCREMENT,
    child_id  INTEGER NOT NULL REFERENCES symbols(id),
    parent_id INTEGER NOT NULL REFERENCES symbols(id),
    UNIQUE(child_id, parent_id)
);

CREATE TABLE IF NOT EXISTS "references" (
    id             INTEGER PRIMARY KEY AUTOINCREMENT,
    from_symbol_id INTEGER NOT NULL REFERENCES symbols(id),
    to_symbol_id   INTEGER NOT NULL REFERENCES symbols(id),
    ref_kind       TEXT NOT NULL,
    file_id        INTEGER REFERENCES files(id),
    line           INTEGER
);

CREATE INDEX IF NOT EXISTS idx_refs_from ON "references"(from_symbol_id);
CREATE INDEX IF NOT EXISTS idx_refs_to   ON "references"(to_symbol_id);
CREATE INDEX IF NOT EXISTS idx_refs_kind ON "references"(ref_kind);

CREATE TABLE IF NOT EXISTS includes (
    id            INTEGER PRIMARY KEY AUTOINCREMENT,
    file_id       INTEGER NOT NULL REFERENCES files(id),
    included_path TEXT NOT NULL,
    line          INTEGER
);

-- FTS5 virtual tables --------------------------------------------------------

CREATE VIRTUAL TABLE IF NOT EXISTS symbols_fts USING fts5(
    name,
    qualified_name,
    docstring,
    search_tokens,
    content=symbols,
    content_rowid=id,
    tokenize='unicode61 remove_diacritics 2',
    prefix='2 3 4'
);

CREATE VIRTUAL TABLE IF NOT EXISTS source_fts USING fts5(
    file_id UNINDEXED,
    line_number UNINDEXED,
    text,
    tokenize='unicode61 remove_diacritics 2',
    prefix='2 3 4'
);

-- Meta table -----------------------------------------------------------------

CREATE TABLE IF NOT EXISTS meta (
    key   TEXT PRIMARY KEY,
    value TEXT
);
"""

_TRIGGERS = """
-- Keep symbols_fts in sync with symbols table

CREATE TRIGGER IF NOT EXISTS symbols_ai AFTER INSERT ON symbols BEGIN
    INSERT INTO symbols_fts(rowid, name, qualified_name, docstring, search_tokens)
    VALUES (new.id, new.name, new.qualified_name, new.docstring, new.search_tokens);
END;

CREATE TRIGGER IF NOT EXISTS symbols_ad AFTER DELETE ON symbols BEGIN
    INSERT INTO symbols_fts(symbols_fts, rowid, name, qualified_name, docstring, search_tokens)
    VALUES ('delete', old.id, old.name, old.qualified_name, old.docstring, old.search_tokens);
END;

CREATE TRIGGER IF NOT EXISTS symbols_au AFTER UPDATE ON symbols BEGIN
    INSERT INTO symbols_fts(symbols_fts, rowid, name, qualified_name, docstring, search_tokens)
    VALUES ('delete', old.id, old.name, old.qualified_name, old.docstring, old.search_tokens);
    INSERT INTO symbols_fts(rowid, name, qualified_name, docstring, search_tokens)
    VALUES (new.id, new.name, new.qualified_name, new.docstring, new.search_tokens);
END;
"""


def _table_exists(conn: sqlite3.Connection, name: str) -> bool:
    return conn.execute(
        "SELECT 1 FROM sqlite_master WHERE type='table' AND name=? LIMIT 1",
        (name,),
    ).fetchone() is not None


def _column_exists(conn: sqlite3.Connection, table: str, column: str) -> bool:
    return any(row[1] == column for row in conn.execute(f"PRAGMA table_info({table})"))


def _search_text(*parts: str | None) -> str:
    text = " ".join(p or "" for p in parts)
    tokens: list[str] = []
    seen: set[str] = set()
    for raw in re.split(r"[^0-9A-Za-z]+", text):
        if not raw:
            continue
        expanded = re.sub(r"(?<=[a-z0-9])(?=[A-Z])|(?<=[A-Z])(?=[A-Z][a-z])", " ", raw)
        for token in expanded.split():
            key = token.lower()
            if len(token) > 1 and key not in seen and key != raw.lower():
                seen.add(key)
                tokens.append(token)
    return " ".join([text, *tokens]).strip()


def _read_schema_version(conn: sqlite3.Connection) -> int:
    header_version = conn.execute("PRAGMA user_version").fetchone()[0]
    if header_version:
        return int(header_version)
    if not _table_exists(conn, "meta"):
        return 0
    row = conn.execute("SELECT value FROM meta WHERE key='schema_version'").fetchone()
    return int(row[0]) if row and row[0] else 0


def _migrate_fts_v2(conn: sqlite3.Connection) -> None:
    if _table_exists(conn, "symbols") and not _column_exists(conn, "symbols", "search_tokens"):
        conn.execute("ALTER TABLE symbols ADD COLUMN search_tokens TEXT DEFAULT ''")
    if _table_exists(conn, "symbols"):
        rows = conn.execute(
            "SELECT id, name, qualified_name, kind FROM symbols"
        ).fetchall()
        conn.executemany(
            "UPDATE symbols SET search_tokens=? WHERE id=?",
            ((_search_text(row[1], row[2], row[3]), row[0]) for row in rows),
        )

    had_source_fts = _table_exists(conn, "source_fts")
    if had_source_fts:
        conn.execute("DROP TABLE IF EXISTS temp.monolith_source_fts_backup")
        conn.execute(
            "CREATE TEMP TABLE monolith_source_fts_backup AS "
            "SELECT file_id, line_number, text FROM source_fts"
        )

    conn.executescript(
        """
        DROP TRIGGER IF EXISTS symbols_au;
        DROP TRIGGER IF EXISTS symbols_ad;
        DROP TRIGGER IF EXISTS symbols_ai;
        DROP TABLE IF EXISTS symbols_fts;
        DROP TABLE IF EXISTS source_fts;
        """
    )
    conn.executescript(_DDL)
    conn.executescript(_TRIGGERS)
    conn.execute(
        "INSERT INTO symbols_fts(rowid, name, qualified_name, docstring, search_tokens) "
        "SELECT id, name, qualified_name, docstring, search_tokens FROM symbols"
    )
    if had_source_fts:
        conn.execute(
            "INSERT INTO source_fts(file_id, line_number, text) "
            "SELECT file_id, line_number, text FROM temp.monolith_source_fts_backup"
        )
        conn.execute("DROP TABLE IF EXISTS temp.monolith_source_fts_backup")


def init_db(conn: sqlite3.Connection) -> None:
    """Create all tables, indexes, FTS virtual tables, and triggers.

    Sets schema_version in meta and PRAGMA user_version. Safe to call on an existing DB
    (all CREATE statements use IF NOT EXISTS).
    """
    try:
        current_version = _read_schema_version(conn)
        had_fts_before_init = _table_exists(conn, "symbols_fts") or _table_exists(conn, "source_fts")
        conn.commit()
        conn.execute("PRAGMA journal_mode=DELETE")
        conn.execute("PRAGMA synchronous=NORMAL")
        conn.execute(f"PRAGMA threads={min(os.cpu_count() or 0, 4)}")
        conn.executescript(_DDL)
        if (0 < current_version < SCHEMA_VERSION) or (current_version == 0 and had_fts_before_init):
            _migrate_fts_v2(conn)
        else:
            conn.executescript(_TRIGGERS)
        conn.execute(
            "INSERT OR REPLACE INTO meta (key, value) VALUES ('schema_version', ?)",
            (str(SCHEMA_VERSION),),
        )
        conn.execute(f"PRAGMA user_version={SCHEMA_VERSION}")
        application_id = conn.execute("PRAGMA application_id").fetchone()[0]
        if application_id not in (0, APPLICATION_ID):
            raise sqlite3.DatabaseError(
                f"foreign SQLite application_id=0x{application_id:08x}"
            )
        if application_id == 0:
            conn.execute(f"PRAGMA application_id={APPLICATION_ID}")
        conn.commit()
    except sqlite3.OperationalError as e:
        if "readonly" in str(e).lower():
            import logging
            logging.warning(f"Database is read-only, schema already exists: {e}")
        else:
            raise
