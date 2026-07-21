#pragma once

// Portable EngineSource console snapshot schema authority shared by the Unreal
// database adapter and standalone Query. These UTF-8 literals intentionally
// have no Unreal dependency.
namespace MonolithSourceConsoleSchema
{
	static constexpr const char ObjectsTable[] = "console_objects";
	static constexpr const char FtsTable[] = "console_objects_fts";

#define MONOLITH_SOURCE_CONSOLE_TRIGGER_NAMES(X) \
	X("console_objects_ai") \
	X("console_objects_ad") \
	X("console_objects_au")

	static constexpr const char* TriggerNames[] = {
#define MONOLITH_SOURCE_CONSOLE_NAME_UTF8(Name) Name,
		MONOLITH_SOURCE_CONSOLE_TRIGGER_NAMES(MONOLITH_SOURCE_CONSOLE_NAME_UTF8)
#undef MONOLITH_SOURCE_CONSOLE_NAME_UTF8
	};

	static constexpr const char TablesSql[] = R"MONOLITH_SQL(
CREATE TABLE IF NOT EXISTS console_objects (
    name          TEXT PRIMARY KEY,
    object_type   TEXT NOT NULL,
    help          TEXT,
    flags         INTEGER NOT NULL DEFAULT 0,
    is_enabled    INTEGER NOT NULL DEFAULT 0,
    is_deprecated INTEGER NOT NULL DEFAULT 0,
    value         TEXT,
    default_value TEXT,
    variable_type TEXT,
    set_by        TEXT,
    read_only     INTEGER NOT NULL DEFAULT 0,
    cheat         INTEGER NOT NULL DEFAULT 0,
    source        TEXT,
    captured_at   TEXT NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_console_objects_type ON console_objects(object_type);
CREATE INDEX IF NOT EXISTS idx_console_objects_flags ON console_objects(flags);
CREATE TABLE IF NOT EXISTS console_snapshot_meta (
    key   TEXT PRIMARY KEY,
    value TEXT
);
)MONOLITH_SQL";

	static constexpr const char FtsSql[] = R"MONOLITH_SQL(
CREATE VIRTUAL TABLE IF NOT EXISTS console_objects_fts USING fts5(
    name, object_type, help, value, default_value, variable_type, set_by,
    content=console_objects, content_rowid=rowid
);
)MONOLITH_SQL";

	static constexpr const char TriggersSql[] = R"MONOLITH_SQL(
CREATE TRIGGER IF NOT EXISTS console_objects_ai AFTER INSERT ON console_objects BEGIN
    INSERT INTO console_objects_fts(rowid, name, object_type, help, value, default_value, variable_type, set_by)
    VALUES (new.rowid, new.name, new.object_type, new.help, new.value, new.default_value, new.variable_type, new.set_by);
END;
CREATE TRIGGER IF NOT EXISTS console_objects_ad AFTER DELETE ON console_objects BEGIN
    INSERT INTO console_objects_fts(console_objects_fts, rowid, name, object_type, help, value, default_value, variable_type, set_by)
    VALUES ('delete', old.rowid, old.name, old.object_type, old.help, old.value, old.default_value, old.variable_type, old.set_by);
END;
CREATE TRIGGER IF NOT EXISTS console_objects_au AFTER UPDATE ON console_objects BEGIN
    INSERT INTO console_objects_fts(console_objects_fts, rowid, name, object_type, help, value, default_value, variable_type, set_by)
    VALUES ('delete', old.rowid, old.name, old.object_type, old.help, old.value, old.default_value, old.variable_type, old.set_by);
    INSERT INTO console_objects_fts(rowid, name, object_type, help, value, default_value, variable_type, set_by)
    VALUES (new.rowid, new.name, new.object_type, new.help, new.value, new.default_value, new.variable_type, new.set_by);
END;
)MONOLITH_SQL";
}
