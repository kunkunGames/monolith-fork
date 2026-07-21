#pragma once

// Portable EngineSource symbol/source FTS schema authority shared by the
// Unreal database adapter and standalone Query. These UTF-8 literals
// intentionally have no Unreal dependency.
namespace MonolithSourceSymbolSearchSchema
{
	static constexpr const char SymbolsFtsTable[] = "symbols_fts";
	static constexpr const char SourceFtsTable[] = "source_fts";
	static constexpr const char* TriggerNames[] = {
		"symbols_ai",
		"symbols_ad",
		"symbols_au",
	};

	static constexpr const char TablesSql[] = R"MONOLITH_SQL(
CREATE VIRTUAL TABLE IF NOT EXISTS symbols_fts USING fts5(
    name, qualified_name, docstring,
    content=symbols, content_rowid=id
);
CREATE VIRTUAL TABLE IF NOT EXISTS source_fts USING fts5(
    file_id UNINDEXED, line_number UNINDEXED, text
);
)MONOLITH_SQL";

	static constexpr const char TriggersSql[] = R"MONOLITH_SQL(
CREATE TRIGGER IF NOT EXISTS symbols_ai AFTER INSERT ON symbols BEGIN
    INSERT INTO symbols_fts(rowid, name, qualified_name, docstring)
    VALUES (new.id, new.name, new.qualified_name, new.docstring);
END;
CREATE TRIGGER IF NOT EXISTS symbols_ad AFTER DELETE ON symbols BEGIN
    INSERT INTO symbols_fts(symbols_fts, rowid, name, qualified_name, docstring)
    VALUES ('delete', old.id, old.name, old.qualified_name, old.docstring);
END;
CREATE TRIGGER IF NOT EXISTS symbols_au AFTER UPDATE ON symbols BEGIN
    INSERT INTO symbols_fts(symbols_fts, rowid, name, qualified_name, docstring)
    VALUES ('delete', old.id, old.name, old.qualified_name, old.docstring);
    INSERT INTO symbols_fts(rowid, name, qualified_name, docstring)
    VALUES (new.id, new.name, new.qualified_name, new.docstring);
END;
)MONOLITH_SQL";
}
