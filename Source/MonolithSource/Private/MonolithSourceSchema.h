#pragma once

#include "MonolithSourceConsoleSchema.h"
#include "MonolithSourceGraphSearchSchema.h"
#include "MonolithSourceSymbolSearchSchema.h"

/**
 * DDL constants for the Monolith engine source SQLite database.
 *
 * This native schema is the SOLE source of truth for EngineSource.db. The legacy
 * Python tree-sitter indexer (Scripts/source_indexer/db/schema.py) is no longer
 * invoked (UMonolithSourceSubsystem builds the DB in-process); that script is
 * vestigial and must not be treated as a schema authority.
 *
 * Each DDL_* constant may contain multiple semicolon-separated statements.
 * Use ExecuteMulti() in MonolithSourceDatabase.cpp — FSQLiteDatabase::Execute() only
 * runs the first statement of a multi-statement string.
 * Constants are split into logical groups so callers can execute them independently.
 */
namespace MonolithSourceSchema
{
	static const int32 SchemaVersion = MonolithSourceGraphSearchSchema::SchemaVersion;
	static const FString GraphSearchFtsVersionStorage(
		UTF8_TO_TCHAR(MonolithSourceGraphSearchSchema::FtsVersion));
	static const TCHAR* GraphSearchFtsVersion = *GraphSearchFtsVersionStorage;
	static const TCHAR* const GraphSearchTriggerNames[] = {
#define MONOLITH_SOURCE_GRAPH_SEARCH_NAME_TCHAR(Name) TEXT(Name),
		MONOLITH_SOURCE_GRAPH_SEARCH_TRIGGER_NAMES(MONOLITH_SOURCE_GRAPH_SEARCH_NAME_TCHAR)
#undef MONOLITH_SOURCE_GRAPH_SEARCH_NAME_TCHAR
	};
#undef MONOLITH_SOURCE_GRAPH_SEARCH_TRIGGER_NAMES

	// ----------------------------------------------------------------
	// Core tables + indexes
	// ----------------------------------------------------------------
	static const TCHAR* DDL_Tables =
		TEXT("CREATE TABLE IF NOT EXISTS modules (")
		TEXT("    id          INTEGER PRIMARY KEY AUTOINCREMENT,")
		TEXT("    name        TEXT NOT NULL,")
		TEXT("    path        TEXT NOT NULL,")
		TEXT("    module_type TEXT NOT NULL,")
		TEXT("    build_cs_path TEXT,")
		TEXT("    UNIQUE(name, path)")
		TEXT(");")

		TEXT("CREATE TABLE IF NOT EXISTS files (")
		TEXT("    id            INTEGER PRIMARY KEY AUTOINCREMENT,")
		TEXT("    path          TEXT NOT NULL UNIQUE,")
		TEXT("    module_id     INTEGER REFERENCES modules(id),")
		TEXT("    file_type     TEXT NOT NULL,")
		TEXT("    line_count    INTEGER NOT NULL DEFAULT 0,")
		TEXT("    last_modified REAL NOT NULL DEFAULT 0.0")
		TEXT(");")

		TEXT("CREATE TABLE IF NOT EXISTS symbols (")
		TEXT("    id               INTEGER PRIMARY KEY AUTOINCREMENT,")
		TEXT("    name             TEXT NOT NULL,")
		TEXT("    qualified_name   TEXT NOT NULL,")
		TEXT("    kind             TEXT NOT NULL,")
		TEXT("    file_id          INTEGER REFERENCES files(id),")
		TEXT("    line_start       INTEGER,")
		TEXT("    line_end         INTEGER,")
		TEXT("    parent_symbol_id INTEGER REFERENCES symbols(id),")
		TEXT("    access           TEXT,")
		TEXT("    signature        TEXT,")
		TEXT("    docstring        TEXT,")
		TEXT("    is_ue_macro      INTEGER NOT NULL DEFAULT 0")
		TEXT(");")

		TEXT("CREATE INDEX IF NOT EXISTS idx_symbols_name           ON symbols(name);")
		TEXT("CREATE INDEX IF NOT EXISTS idx_symbols_qualified_name ON symbols(qualified_name);")
		TEXT("CREATE INDEX IF NOT EXISTS idx_symbols_kind           ON symbols(kind);")
		TEXT("CREATE INDEX IF NOT EXISTS idx_symbols_file_id        ON symbols(file_id);")
		TEXT("CREATE INDEX IF NOT EXISTS idx_symbols_parent         ON symbols(parent_symbol_id);")
		TEXT("CREATE INDEX IF NOT EXISTS idx_symbols_parent_name_kind ON symbols(parent_symbol_id, name, kind);")
		TEXT("CREATE INDEX IF NOT EXISTS idx_symbols_name_kind_parent ON symbols(name, kind, parent_symbol_id);")
		TEXT("CREATE INDEX IF NOT EXISTS idx_symbols_override_signature ON symbols(kind, parent_symbol_id, name) WHERE kind='function' AND signature LIKE '%override%';")

		TEXT("CREATE TABLE IF NOT EXISTS inheritance (")
		TEXT("    id        INTEGER PRIMARY KEY AUTOINCREMENT,")
		TEXT("    child_id  INTEGER NOT NULL REFERENCES symbols(id),")
		TEXT("    parent_id INTEGER NOT NULL REFERENCES symbols(id),")
		TEXT("    UNIQUE(child_id, parent_id)")
		TEXT(");")
		TEXT("CREATE INDEX IF NOT EXISTS idx_inheritance_parent_child ON inheritance(parent_id, child_id);")
		TEXT("CREATE INDEX IF NOT EXISTS idx_inheritance_child_parent ON inheritance(child_id, parent_id);")

		TEXT("CREATE TABLE IF NOT EXISTS \"references\" (")
		TEXT("    id             INTEGER PRIMARY KEY AUTOINCREMENT,")
		TEXT("    from_symbol_id INTEGER NOT NULL REFERENCES symbols(id),")
		TEXT("    to_symbol_id   INTEGER NOT NULL REFERENCES symbols(id),")
		TEXT("    ref_kind       TEXT NOT NULL,")
		TEXT("    file_id        INTEGER REFERENCES files(id),")
		TEXT("    line           INTEGER")
		TEXT(");")

		TEXT("CREATE INDEX IF NOT EXISTS idx_refs_from ON \"references\"(from_symbol_id);")
		TEXT("CREATE INDEX IF NOT EXISTS idx_refs_to   ON \"references\"(to_symbol_id);")
		TEXT("CREATE INDEX IF NOT EXISTS idx_refs_kind ON \"references\"(ref_kind);")

		TEXT("CREATE TABLE IF NOT EXISTS includes (")
		TEXT("    id            INTEGER PRIMARY KEY AUTOINCREMENT,")
		TEXT("    file_id       INTEGER NOT NULL REFERENCES files(id),")
		TEXT("    included_path TEXT NOT NULL,")
		TEXT("    line          INTEGER")
		TEXT(");")

		// Schema v2 (item 3): UE_DEPRECATED extraction. symbol_id is NULLABLE —
		// class-body method declarations are NOT indexed as `symbols` rows
		// (Step-0 finding), so the extractor parses the symbol NAME from the
		// declaration text after the macro and stores symbol_id = NULL when it
		// cannot resolve a symbols row. Lookups (check_deprecations) key on
		// symbol_name, so a NULL symbol_id does not break the read path.
		TEXT("CREATE TABLE IF NOT EXISTS symbol_deprecations (")
		TEXT("    id          INTEGER PRIMARY KEY AUTOINCREMENT,")
		TEXT("    symbol_id   INTEGER REFERENCES symbols(id),")
		TEXT("    symbol_name TEXT NOT NULL,")
		TEXT("    version     TEXT,")
		TEXT("    message     TEXT,")
		TEXT("    kind        TEXT NOT NULL")
		TEXT(");")

		TEXT("CREATE INDEX IF NOT EXISTS idx_deprecations_name ON symbol_deprecations(symbol_name);")

		TEXT("CREATE TABLE IF NOT EXISTS meta (")
		TEXT("    key   TEXT PRIMARY KEY,")
		TEXT("    value TEXT")
		TEXT(");");

	// ----------------------------------------------------------------
	// FTS5 virtual tables
	// ----------------------------------------------------------------
	static const FString DDL_FTSStorage(
		UTF8_TO_TCHAR(MonolithSourceSymbolSearchSchema::TablesSql));
	static const TCHAR* DDL_FTS = *DDL_FTSStorage;

	// ----------------------------------------------------------------
	// Canonical source graph-node projection + search index (schema v4).
	// UTF-8 SQL lives in the portable public header shared with Query; these
	// stable FString owners provide the TCHAR pointers expected by SQLiteCore.
	// ----------------------------------------------------------------
	static const FString DDL_GraphSearchViewStorage(
		UTF8_TO_TCHAR(MonolithSourceGraphSearchSchema::ViewSql));
	static const TCHAR* DDL_GraphSearchView = *DDL_GraphSearchViewStorage;
	static const FString DDL_GraphSearchFTSStorage(
		UTF8_TO_TCHAR(MonolithSourceGraphSearchSchema::FtsSql));
	static const TCHAR* DDL_GraphSearchFTS = *DDL_GraphSearchFTSStorage;
	static const FString DDL_GraphSearchTriggersStorage(
		UTF8_TO_TCHAR(MonolithSourceGraphSearchSchema::TriggersSql));
	static const TCHAR* DDL_GraphSearchTriggers = *DDL_GraphSearchTriggersStorage;
	static const FString DDL_GraphSearchDropStorage(
		UTF8_TO_TCHAR(MonolithSourceGraphSearchSchema::DropSql));
	static const TCHAR* DDL_GraphSearchDrop = *DDL_GraphSearchDropStorage;

	// ----------------------------------------------------------------
	// Live console-object snapshot tables.
	// These are populated from IConsoleManager on the game thread by the
	// console namespace. They intentionally live in EngineSource.db so live MCP
	// and offline monolith_query.exe share one searchable console surface.
	// ----------------------------------------------------------------
	static const FString DDL_ConsoleTablesStorage(
		UTF8_TO_TCHAR(MonolithSourceConsoleSchema::TablesSql));
	static const TCHAR* DDL_ConsoleTables = *DDL_ConsoleTablesStorage;
	static const FString DDL_ConsoleFTSStorage(
		UTF8_TO_TCHAR(MonolithSourceConsoleSchema::FtsSql));
	static const TCHAR* DDL_ConsoleFTS = *DDL_ConsoleFTSStorage;
	static const FString DDL_ConsoleTriggersStorage(
		UTF8_TO_TCHAR(MonolithSourceConsoleSchema::TriggersSql));
	static const TCHAR* DDL_ConsoleTriggers = *DDL_ConsoleTriggersStorage;
#undef MONOLITH_SOURCE_CONSOLE_TRIGGER_NAMES

	// ----------------------------------------------------------------
	// Triggers to keep symbols_fts in sync with symbols
	// ----------------------------------------------------------------
	static const FString DDL_TriggersStorage(
		UTF8_TO_TCHAR(MonolithSourceSymbolSearchSchema::TriggersSql));
	static const TCHAR* DDL_Triggers = *DDL_TriggersStorage;

} // namespace MonolithSourceSchema
