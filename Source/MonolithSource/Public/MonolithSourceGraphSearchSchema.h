#pragma once

// Portable EngineSource graph-search schema authority.
//
// This header intentionally depends on neither Unreal nor the standalone Query
// runtime. MonolithSource converts these UTF-8 literals to TCHAR, while
// monolith_query.cpp passes them directly to SQLite. Keep the graph-node VIEW,
// external-content FTS table, maintenance triggers, and version markers here so
// live/editor and offline repair cannot drift apart.
namespace MonolithSourceGraphSearchSchema
{
	static constexpr int SchemaVersion = 4;
	static constexpr const char SchemaVersionText[] = "4";
	static constexpr const char FtsVersion[] = "1";

#define MONOLITH_SOURCE_GRAPH_SEARCH_TRIGGER_NAMES(X) \
	X("source_graph_nodes_files_ai") \
	X("source_graph_nodes_files_bd") \
	X("source_graph_nodes_files_ad") \
	X("source_graph_nodes_files_bu") \
	X("source_graph_nodes_files_au") \
	X("source_graph_nodes_symbols_ai") \
	X("source_graph_nodes_symbols_bd") \
	X("source_graph_nodes_symbols_bu") \
	X("source_graph_nodes_symbols_au")

	static constexpr const char* TriggerNames[] = {
#define MONOLITH_SOURCE_GRAPH_SEARCH_NAME_UTF8(Name) Name,
		MONOLITH_SOURCE_GRAPH_SEARCH_TRIGGER_NAMES(MONOLITH_SOURCE_GRAPH_SEARCH_NAME_UTF8)
#undef MONOLITH_SOURCE_GRAPH_SEARCH_NAME_UTF8
	};

	static constexpr const char ViewSql[] = R"MONOLITH_SQL(
CREATE VIEW IF NOT EXISTS source_graph_nodes AS
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
           CASE WHEN lower(COALESCE(s.name,'') || ' ' || COALESCE(f.path,'')) LIKE '%test%' THEN 'Test' ELSE 'Function' END
         WHEN lower(COALESCE(s.kind,'')) IN ('enum','typedef','type','delegate') THEN 'Type'
         ELSE COALESCE(NULLIF(s.kind,''),'Symbol')
       END AS kind,
       COALESCE(NULLIF(s.name,''), 'symbol#' || s.id) AS name,
       COALESCE(NULLIF(s.qualified_name,''), COALESCE(f.path,'') || '::' || COALESCE(s.name,'symbol')) || '#' || s.id AS qualified_name,
       COALESCE(f.path,'') AS file_path,
       COALESCE(s.line_start,0) AS line_start,
       COALESCE(s.line_end,0) AS line_end,
       CASE WHEN lower(COALESCE(f.file_type,'')) LIKE '%shader%' THEN 'shader' ELSE 'cpp' END AS language,
       s.signature AS signature
FROM symbols s
LEFT JOIN files f ON f.id = s.file_id;
)MONOLITH_SQL";

	static constexpr const char FtsSql[] = R"MONOLITH_SQL(
CREATE VIRTUAL TABLE IF NOT EXISTS source_graph_nodes_fts USING fts5(
    name, qualified_name, file_path, signature,
    content=source_graph_nodes, content_rowid=id,
    tokenize='porter unicode61'
);
)MONOLITH_SQL";

	// External-content FTS does not maintain itself. BEFORE triggers remove the
	// exact old VIEW projection while it is still visible; AFTER triggers insert
	// the new projection. File changes cover child symbols because file_path also
	// participates in Test-kind normalization.
	static constexpr const char TriggersSql[] = R"MONOLITH_SQL(
CREATE TRIGGER IF NOT EXISTS source_graph_nodes_files_ai AFTER INSERT ON files BEGIN
    INSERT INTO source_graph_nodes_fts(rowid, name, qualified_name, file_path, signature)
    SELECT id, name, qualified_name, file_path, signature FROM source_graph_nodes
    WHERE id = -new.id OR id IN (SELECT id FROM symbols WHERE file_id = new.id);
END;
CREATE TRIGGER IF NOT EXISTS source_graph_nodes_files_bd BEFORE DELETE ON files BEGIN
    INSERT INTO source_graph_nodes_fts(source_graph_nodes_fts, rowid, name, qualified_name, file_path, signature)
    SELECT 'delete', id, name, qualified_name, file_path, signature FROM source_graph_nodes
    WHERE id = -old.id OR id IN (SELECT id FROM symbols WHERE file_id = old.id);
END;
CREATE TRIGGER IF NOT EXISTS source_graph_nodes_files_ad AFTER DELETE ON files BEGIN
    INSERT INTO source_graph_nodes_fts(rowid, name, qualified_name, file_path, signature)
    SELECT id, name, qualified_name, file_path, signature FROM source_graph_nodes
    WHERE id IN (SELECT id FROM symbols WHERE file_id = old.id);
END;
CREATE TRIGGER IF NOT EXISTS source_graph_nodes_files_bu BEFORE UPDATE OF path ON files BEGIN
    INSERT INTO source_graph_nodes_fts(source_graph_nodes_fts, rowid, name, qualified_name, file_path, signature)
    SELECT 'delete', id, name, qualified_name, file_path, signature FROM source_graph_nodes
    WHERE id = -old.id OR id IN (SELECT id FROM symbols WHERE file_id = old.id);
END;
CREATE TRIGGER IF NOT EXISTS source_graph_nodes_files_au AFTER UPDATE OF path ON files BEGIN
    INSERT INTO source_graph_nodes_fts(rowid, name, qualified_name, file_path, signature)
    SELECT id, name, qualified_name, file_path, signature FROM source_graph_nodes
    WHERE id = -new.id OR id IN (SELECT id FROM symbols WHERE file_id = new.id);
END;
CREATE TRIGGER IF NOT EXISTS source_graph_nodes_symbols_ai AFTER INSERT ON symbols BEGIN
    INSERT INTO source_graph_nodes_fts(rowid, name, qualified_name, file_path, signature)
    SELECT id, name, qualified_name, file_path, signature FROM source_graph_nodes WHERE id = new.id;
END;
CREATE TRIGGER IF NOT EXISTS source_graph_nodes_symbols_bd BEFORE DELETE ON symbols BEGIN
    INSERT INTO source_graph_nodes_fts(source_graph_nodes_fts, rowid, name, qualified_name, file_path, signature)
    SELECT 'delete', id, name, qualified_name, file_path, signature FROM source_graph_nodes WHERE id = old.id;
END;
CREATE TRIGGER IF NOT EXISTS source_graph_nodes_symbols_bu BEFORE UPDATE OF name, qualified_name, file_id, signature ON symbols BEGIN
    INSERT INTO source_graph_nodes_fts(source_graph_nodes_fts, rowid, name, qualified_name, file_path, signature)
    SELECT 'delete', id, name, qualified_name, file_path, signature FROM source_graph_nodes WHERE id = old.id;
END;
CREATE TRIGGER IF NOT EXISTS source_graph_nodes_symbols_au AFTER UPDATE OF name, qualified_name, file_id, signature ON symbols BEGIN
    INSERT INTO source_graph_nodes_fts(rowid, name, qualified_name, file_path, signature)
    SELECT id, name, qualified_name, file_path, signature FROM source_graph_nodes WHERE id = new.id;
END;
)MONOLITH_SQL";

	static constexpr const char DropSql[] = R"MONOLITH_SQL(
DROP TRIGGER IF EXISTS source_graph_nodes_symbols_au;
DROP TRIGGER IF EXISTS source_graph_nodes_symbols_bu;
DROP TRIGGER IF EXISTS source_graph_nodes_symbols_bd;
DROP TRIGGER IF EXISTS source_graph_nodes_symbols_ai;
DROP TRIGGER IF EXISTS source_graph_nodes_files_au;
DROP TRIGGER IF EXISTS source_graph_nodes_files_bu;
DROP TRIGGER IF EXISTS source_graph_nodes_files_ad;
DROP TRIGGER IF EXISTS source_graph_nodes_files_bd;
DROP TRIGGER IF EXISTS source_graph_nodes_files_ai;
DROP TABLE IF EXISTS source_graph_nodes_fts;
DROP VIEW IF EXISTS source_graph_nodes;
)MONOLITH_SQL";
}
