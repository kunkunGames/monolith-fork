#include "MonolithIndexDatabase.h"
#include "SQLiteDatabase.h"
#include "Misc/Paths.h"
#include "HAL/PlatformFileManager.h"
#include "Serialization/JsonWriter.h"
#include "Serialization/JsonSerializer.h"

DEFINE_LOG_CATEGORY(LogMonolithIndex);

// ============================================================
// Full table creation SQL
// ============================================================
static const TCHAR* GCreateTablesSQL = TEXT(R"SQL(

-- Core asset table: every indexed asset
CREATE TABLE IF NOT EXISTS assets (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    package_path TEXT NOT NULL UNIQUE,
    asset_name TEXT NOT NULL,
    asset_class TEXT NOT NULL,
    module_name TEXT DEFAULT '',
    description TEXT DEFAULT '',
    file_size_bytes INTEGER DEFAULT 0,
    last_modified TEXT DEFAULT '',
    saved_hash TEXT DEFAULT '',
    indexed_at TEXT DEFAULT (datetime('now'))
);
CREATE INDEX IF NOT EXISTS idx_assets_class ON assets(asset_class);
CREATE INDEX IF NOT EXISTS idx_assets_name ON assets(asset_name);

-- Graph nodes (Blueprint nodes, Material expressions, Niagara modules, etc.)
CREATE TABLE IF NOT EXISTS nodes (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    asset_id INTEGER NOT NULL REFERENCES assets(id) ON DELETE CASCADE,
    node_type TEXT NOT NULL,
    node_name TEXT NOT NULL,
    node_class TEXT DEFAULT '',
    properties TEXT DEFAULT '{}',
    pos_x INTEGER DEFAULT 0,
    pos_y INTEGER DEFAULT 0
);
CREATE INDEX IF NOT EXISTS idx_nodes_asset ON nodes(asset_id);
CREATE INDEX IF NOT EXISTS idx_nodes_class ON nodes(node_class);

-- Pin connections between nodes
CREATE TABLE IF NOT EXISTS connections (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    source_node_id INTEGER NOT NULL REFERENCES nodes(id) ON DELETE CASCADE,
    source_pin TEXT NOT NULL,
    target_node_id INTEGER NOT NULL REFERENCES nodes(id) ON DELETE CASCADE,
    target_pin TEXT NOT NULL,
    pin_type TEXT DEFAULT ''
);
CREATE INDEX IF NOT EXISTS idx_conn_source ON connections(source_node_id);
CREATE INDEX IF NOT EXISTS idx_conn_target ON connections(target_node_id);

-- Variables (Blueprint variables, material parameters, niagara parameters)
CREATE TABLE IF NOT EXISTS variables (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    asset_id INTEGER NOT NULL REFERENCES assets(id) ON DELETE CASCADE,
    var_name TEXT NOT NULL,
    var_type TEXT NOT NULL,
    category TEXT DEFAULT '',
    default_value TEXT DEFAULT '',
    is_exposed INTEGER DEFAULT 0,
    is_replicated INTEGER DEFAULT 0
);
CREATE INDEX IF NOT EXISTS idx_vars_asset ON variables(asset_id);

-- Parameters (Material params, Niagara params, etc.)
CREATE TABLE IF NOT EXISTS parameters (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    asset_id INTEGER NOT NULL REFERENCES assets(id) ON DELETE CASCADE,
    param_name TEXT NOT NULL,
    param_type TEXT NOT NULL,
    param_group TEXT DEFAULT '',
    default_value TEXT DEFAULT '',
    source TEXT DEFAULT ''
);
CREATE INDEX IF NOT EXISTS idx_params_asset ON parameters(asset_id);

-- Asset dependency graph
CREATE TABLE IF NOT EXISTS dependencies (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    source_asset_id INTEGER NOT NULL REFERENCES assets(id) ON DELETE CASCADE,
    target_asset_id INTEGER NOT NULL REFERENCES assets(id) ON DELETE CASCADE,
    dependency_type TEXT DEFAULT 'Hard'
);
CREATE INDEX IF NOT EXISTS idx_dep_source ON dependencies(source_asset_id);
CREATE INDEX IF NOT EXISTS idx_dep_target ON dependencies(target_asset_id);

-- Level actors
CREATE TABLE IF NOT EXISTS actors (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    asset_id INTEGER NOT NULL REFERENCES assets(id) ON DELETE CASCADE,
    actor_name TEXT NOT NULL,
    actor_class TEXT NOT NULL,
    actor_label TEXT DEFAULT '',
    transform TEXT DEFAULT '{}',
    components TEXT DEFAULT '[]'
);
CREATE INDEX IF NOT EXISTS idx_actors_asset ON actors(asset_id);
CREATE INDEX IF NOT EXISTS idx_actors_class ON actors(actor_class);

-- Gameplay tags
CREATE TABLE IF NOT EXISTS tags (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    tag_name TEXT NOT NULL UNIQUE,
    parent_tag TEXT DEFAULT '',
    reference_count INTEGER DEFAULT 0
);

-- Tag references (which assets use which tags)
CREATE TABLE IF NOT EXISTS tag_references (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    tag_id INTEGER NOT NULL REFERENCES tags(id) ON DELETE CASCADE,
    asset_id INTEGER NOT NULL REFERENCES assets(id) ON DELETE CASCADE,
    context TEXT DEFAULT ''
);
CREATE INDEX IF NOT EXISTS idx_tagref_tag ON tag_references(tag_id);
CREATE INDEX IF NOT EXISTS idx_tagref_asset ON tag_references(asset_id);

-- Config/INI entries
CREATE TABLE IF NOT EXISTS configs (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    file_path TEXT NOT NULL,
    section TEXT NOT NULL,
    key TEXT NOT NULL,
    value TEXT DEFAULT ''
);
CREATE INDEX IF NOT EXISTS idx_configs_file ON configs(file_path);

-- C++ symbols (from tree-sitter via MonolithSource)
CREATE TABLE IF NOT EXISTS cpp_symbols (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    file_path TEXT NOT NULL,
    symbol_name TEXT NOT NULL,
    symbol_type TEXT NOT NULL,
    signature TEXT DEFAULT '',
    line_number INTEGER DEFAULT 0,
    parent_symbol TEXT DEFAULT ''
);
CREATE INDEX IF NOT EXISTS idx_cpp_file ON cpp_symbols(file_path);
CREATE INDEX IF NOT EXISTS idx_cpp_name ON cpp_symbols(symbol_name);

-- Data table rows
CREATE TABLE IF NOT EXISTS datatable_rows (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    asset_id INTEGER NOT NULL REFERENCES assets(id) ON DELETE CASCADE,
    row_name TEXT NOT NULL,
    row_data TEXT DEFAULT '{}'
);
CREATE INDEX IF NOT EXISTS idx_dt_asset ON datatable_rows(asset_id);

-- FTS5 index over assets (name, class, description, path, module)
CREATE VIRTUAL TABLE IF NOT EXISTS fts_assets USING fts5(
    asset_name,
    asset_class,
    description,
    package_path,
    module_name,
    content=assets,
    content_rowid=id,
    tokenize='porter unicode61'
);

-- FTS5 triggers to keep index in sync
CREATE TRIGGER IF NOT EXISTS fts_assets_ai AFTER INSERT ON assets BEGIN
    INSERT INTO fts_assets(rowid, asset_name, asset_class, description, package_path, module_name)
    VALUES (new.id, new.asset_name, new.asset_class, new.description, new.package_path, new.module_name);
END;
CREATE TRIGGER IF NOT EXISTS fts_assets_ad AFTER DELETE ON assets BEGIN
    INSERT INTO fts_assets(fts_assets, rowid, asset_name, asset_class, description, package_path, module_name)
    VALUES ('delete', old.id, old.asset_name, old.asset_class, old.description, old.package_path, old.module_name);
END;
CREATE TRIGGER IF NOT EXISTS fts_assets_au AFTER UPDATE ON assets BEGIN
    INSERT INTO fts_assets(fts_assets, rowid, asset_name, asset_class, description, package_path, module_name)
    VALUES ('delete', old.id, old.asset_name, old.asset_class, old.description, old.package_path, old.module_name);
    INSERT INTO fts_assets(rowid, asset_name, asset_class, description, package_path, module_name)
    VALUES (new.id, new.asset_name, new.asset_class, new.description, new.package_path, new.module_name);
END;

-- FTS5 index over nodes (name, class, type)
CREATE VIRTUAL TABLE IF NOT EXISTS fts_nodes USING fts5(
    node_name,
    node_class,
    node_type,
    content=nodes,
    content_rowid=id,
    tokenize='porter unicode61'
);

CREATE TRIGGER IF NOT EXISTS fts_nodes_ai AFTER INSERT ON nodes BEGIN
    INSERT INTO fts_nodes(rowid, node_name, node_class, node_type)
    VALUES (new.id, new.node_name, new.node_class, new.node_type);
END;
CREATE TRIGGER IF NOT EXISTS fts_nodes_ad AFTER DELETE ON nodes BEGIN
    INSERT INTO fts_nodes(fts_nodes, rowid, node_name, node_class, node_type)
    VALUES ('delete', old.id, old.node_name, old.node_class, old.node_type);
END;
CREATE TRIGGER IF NOT EXISTS fts_nodes_au AFTER UPDATE ON nodes BEGIN
    INSERT INTO fts_nodes(fts_nodes, rowid, node_name, node_class, node_type)
    VALUES ('delete', old.id, old.node_name, old.node_class, old.node_type);
    INSERT INTO fts_nodes(rowid, node_name, node_class, node_type)
    VALUES (new.id, new.node_name, new.node_class, new.node_type);
END;

-- Metadata table for tracking index state
CREATE TABLE IF NOT EXISTS meta (
    key TEXT PRIMARY KEY,
    value TEXT DEFAULT ''
);

)SQL");

static const TCHAR* GCreateSearchValueTablesSQL = TEXT(R"SQL(

-- Curated supplemental values that are valuable for search but do not belong in
-- a primary structured table column (Blueprint comments, pin defaults, row fields).
CREATE TABLE IF NOT EXISTS asset_search_values (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    asset_id INTEGER NOT NULL REFERENCES assets(id) ON DELETE CASCADE,
    source_kind TEXT NOT NULL,
    object_name TEXT DEFAULT '',
    object_path TEXT DEFAULT '',
    object_class TEXT DEFAULT '',
    field_name TEXT DEFAULT '',
    field_path TEXT DEFAULT '',
    value_text TEXT NOT NULL,
    signal TEXT DEFAULT '',
    indexed_at TEXT DEFAULT (datetime('now'))
);
CREATE INDEX IF NOT EXISTS idx_search_values_asset ON asset_search_values(asset_id);
CREATE INDEX IF NOT EXISTS idx_search_values_source ON asset_search_values(source_kind);

-- FTS5 indexes over structured content columns that already exist in ProjectIndex.
CREATE VIRTUAL TABLE IF NOT EXISTS fts_variables USING fts5(
    var_name,
    var_type,
    category,
    default_value,
    content=variables,
    content_rowid=id,
    tokenize='porter unicode61'
);
CREATE TRIGGER IF NOT EXISTS fts_variables_ai AFTER INSERT ON variables BEGIN
    INSERT INTO fts_variables(rowid, var_name, var_type, category, default_value)
    VALUES (new.id, new.var_name, new.var_type, new.category, new.default_value);
END;
CREATE TRIGGER IF NOT EXISTS fts_variables_ad AFTER DELETE ON variables BEGIN
    INSERT INTO fts_variables(fts_variables, rowid, var_name, var_type, category, default_value)
    VALUES ('delete', old.id, old.var_name, old.var_type, old.category, old.default_value);
END;
CREATE TRIGGER IF NOT EXISTS fts_variables_au AFTER UPDATE ON variables BEGIN
    INSERT INTO fts_variables(fts_variables, rowid, var_name, var_type, category, default_value)
    VALUES ('delete', old.id, old.var_name, old.var_type, old.category, old.default_value);
    INSERT INTO fts_variables(rowid, var_name, var_type, category, default_value)
    VALUES (new.id, new.var_name, new.var_type, new.category, new.default_value);
END;

CREATE VIRTUAL TABLE IF NOT EXISTS fts_parameters USING fts5(
    param_name,
    param_type,
    param_group,
    default_value,
    source,
    content=parameters,
    content_rowid=id,
    tokenize='porter unicode61'
);
CREATE TRIGGER IF NOT EXISTS fts_parameters_ai AFTER INSERT ON parameters BEGIN
    INSERT INTO fts_parameters(rowid, param_name, param_type, param_group, default_value, source)
    VALUES (new.id, new.param_name, new.param_type, new.param_group, new.default_value, new.source);
END;
CREATE TRIGGER IF NOT EXISTS fts_parameters_ad AFTER DELETE ON parameters BEGIN
    INSERT INTO fts_parameters(fts_parameters, rowid, param_name, param_type, param_group, default_value, source)
    VALUES ('delete', old.id, old.param_name, old.param_type, old.param_group, old.default_value, old.source);
END;
CREATE TRIGGER IF NOT EXISTS fts_parameters_au AFTER UPDATE ON parameters BEGIN
    INSERT INTO fts_parameters(fts_parameters, rowid, param_name, param_type, param_group, default_value, source)
    VALUES ('delete', old.id, old.param_name, old.param_type, old.param_group, old.default_value, old.source);
    INSERT INTO fts_parameters(rowid, param_name, param_type, param_group, default_value, source)
    VALUES (new.id, new.param_name, new.param_type, new.param_group, new.default_value, new.source);
END;

CREATE VIRTUAL TABLE IF NOT EXISTS fts_datatable_rows USING fts5(
    row_name,
    content=datatable_rows,
    content_rowid=id,
    tokenize='porter unicode61'
);
CREATE TRIGGER IF NOT EXISTS fts_datatable_rows_ai AFTER INSERT ON datatable_rows BEGIN
    INSERT INTO fts_datatable_rows(rowid, row_name)
    VALUES (new.id, new.row_name);
END;
CREATE TRIGGER IF NOT EXISTS fts_datatable_rows_ad AFTER DELETE ON datatable_rows BEGIN
    INSERT INTO fts_datatable_rows(fts_datatable_rows, rowid, row_name)
    VALUES ('delete', old.id, old.row_name);
END;
CREATE TRIGGER IF NOT EXISTS fts_datatable_rows_au AFTER UPDATE ON datatable_rows BEGIN
    INSERT INTO fts_datatable_rows(fts_datatable_rows, rowid, row_name)
    VALUES ('delete', old.id, old.row_name);
    INSERT INTO fts_datatable_rows(rowid, row_name)
    VALUES (new.id, new.row_name);
END;

CREATE VIRTUAL TABLE IF NOT EXISTS fts_actors USING fts5(
    actor_name,
    actor_class,
    actor_label,
    content=actors,
    content_rowid=id,
    tokenize='porter unicode61'
);
CREATE TRIGGER IF NOT EXISTS fts_actors_ai AFTER INSERT ON actors BEGIN
    INSERT INTO fts_actors(rowid, actor_name, actor_class, actor_label)
    VALUES (new.id, new.actor_name, new.actor_class, new.actor_label);
END;
CREATE TRIGGER IF NOT EXISTS fts_actors_ad AFTER DELETE ON actors BEGIN
    INSERT INTO fts_actors(fts_actors, rowid, actor_name, actor_class, actor_label)
    VALUES ('delete', old.id, old.actor_name, old.actor_class, old.actor_label);
END;
CREATE TRIGGER IF NOT EXISTS fts_actors_au AFTER UPDATE ON actors BEGIN
    INSERT INTO fts_actors(fts_actors, rowid, actor_name, actor_class, actor_label)
    VALUES ('delete', old.id, old.actor_name, old.actor_class, old.actor_label);
    INSERT INTO fts_actors(rowid, actor_name, actor_class, actor_label)
    VALUES (new.id, new.actor_name, new.actor_class, new.actor_label);
END;

CREATE VIRTUAL TABLE IF NOT EXISTS fts_asset_search_values USING fts5(
    value_text,
    content=asset_search_values,
    content_rowid=id,
    tokenize='porter unicode61'
);
CREATE TRIGGER IF NOT EXISTS fts_asset_search_values_ai AFTER INSERT ON asset_search_values BEGIN
    INSERT INTO fts_asset_search_values(rowid, value_text)
    VALUES (new.id, new.value_text);
END;
CREATE TRIGGER IF NOT EXISTS fts_asset_search_values_ad AFTER DELETE ON asset_search_values BEGIN
    INSERT INTO fts_asset_search_values(fts_asset_search_values, rowid, value_text)
    VALUES ('delete', old.id, old.value_text);
END;
CREATE TRIGGER IF NOT EXISTS fts_asset_search_values_au AFTER UPDATE ON asset_search_values BEGIN
    INSERT INTO fts_asset_search_values(fts_asset_search_values, rowid, value_text)
    VALUES ('delete', old.id, old.value_text);
    INSERT INTO fts_asset_search_values(rowid, value_text)
    VALUES (new.id, new.value_text);
END;

)SQL");

// ============================================================
// Constructor / Destructor
// ============================================================

FMonolithIndexDatabase::FMonolithIndexDatabase()
{
}

FMonolithIndexDatabase::~FMonolithIndexDatabase()
{
	Close();
}

bool FMonolithIndexDatabase::Open(const FString& InDbPath)
{
	if (Database)
	{
		Close();
	}

	DbPath = InDbPath;

	// Ensure directory exists
	FString Dir = FPaths::GetPath(DbPath);
	IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
	if (!PlatformFile.DirectoryExists(*Dir))
	{
		PlatformFile.CreateDirectoryTree(*Dir);
	}

	Database = new FSQLiteDatabase();
	if (!Database->Open(*DbPath, ESQLiteDatabaseOpenMode::ReadWriteCreate))
	{
		UE_LOG(LogMonolithIndex, Error, TEXT("Failed to open index database: %s"), *DbPath);
		delete Database;
		Database = nullptr;
		return false;
	}

	// Force DELETE journal mode — WAL + ReadOnly on Windows silently returns 0 rows.
	// Belt-and-suspenders: force DELETE here regardless of what the DB was created with.
	ExecuteSQL(TEXT("PRAGMA journal_mode=DELETE;"));
	ExecuteSQL(TEXT("PRAGMA synchronous=NORMAL;"));
	ExecuteSQL(TEXT("PRAGMA foreign_keys=ON;"));
	ExecuteSQL(TEXT("PRAGMA cache_size=-64000;")); // 64MB cache

	if (!CreateTables())
	{
		UE_LOG(LogMonolithIndex, Error, TEXT("Failed to create index tables"));
		Close();
		return false;
	}

	// Schema migration: v1 -> v2
	{
		FString SchemaVersion = ReadMeta(TEXT("schema_version"));
		if (SchemaVersion.IsEmpty() || FCString::Atoi(*SchemaVersion) < 2)
		{
			// Check if saved_hash column already exists (fresh DBs have it from CreateTables)
			bool bHasSavedHash = false;
			FSQLitePreparedStatement PragmaStmt;
			if (PragmaStmt.Create(*Database, TEXT("PRAGMA table_info(assets);"), ESQLitePreparedStatementFlags::Persistent))
			{
				while (PragmaStmt.Step() == ESQLitePreparedStatementStepResult::Row)
				{
					FString ColName;
					PragmaStmt.GetColumnValueByIndex(1, ColName);
					if (ColName == TEXT("saved_hash"))
					{
						bHasSavedHash = true;
						break;
					}
				}
			}

			if (!bHasSavedHash)
			{
				ExecuteSQL(TEXT("ALTER TABLE assets ADD COLUMN saved_hash TEXT DEFAULT '';"));
				ExecuteSQL(TEXT("CREATE INDEX IF NOT EXISTS idx_assets_hash ON assets(saved_hash);"));
			}
			WriteMeta(TEXT("schema_version"), TEXT("2"));
		}
	}

	// Ensure hash index exists (safe for both fresh and migrated DBs)
	ExecuteSQL(TEXT("CREATE INDEX IF NOT EXISTS idx_assets_hash ON assets(saved_hash);"));
	WriteMeta(TEXT("asset_search_values_schema_version"), TEXT("1"));
	// Bumped to 2 for Q1 (PRD AssetSearchSemanticSearch): the asset_search_values extractor
	// now also emits an 'identifier_split' CamelCase/snake sub-word row per asset name.
	WriteMeta(TEXT("asset_search_values_extractor_version"), TEXT("2"));

	UE_LOG(LogMonolithIndex, Log, TEXT("Index database opened: %s"), *DbPath);
	return true;
}

void FMonolithIndexDatabase::Close()
{
	if (Database)
	{
		Database->Close();
		delete Database;
		Database = nullptr;
	}
}

bool FMonolithIndexDatabase::IsOpen() const
{
	return Database != nullptr && Database->IsValid();
}

bool FMonolithIndexDatabase::ResetDatabase()
{
	if (!IsOpen()) return false;

	// Drop all tables and recreate — order matters for foreign keys
	ExecuteSQL(TEXT("DROP TRIGGER IF EXISTS fts_assets_ai;"));
	ExecuteSQL(TEXT("DROP TRIGGER IF EXISTS fts_assets_ad;"));
	ExecuteSQL(TEXT("DROP TRIGGER IF EXISTS fts_assets_au;"));
	ExecuteSQL(TEXT("DROP TRIGGER IF EXISTS fts_nodes_ai;"));
	ExecuteSQL(TEXT("DROP TRIGGER IF EXISTS fts_nodes_ad;"));
	ExecuteSQL(TEXT("DROP TRIGGER IF EXISTS fts_nodes_au;"));
	ExecuteSQL(TEXT("DROP TRIGGER IF EXISTS fts_variables_ai;"));
	ExecuteSQL(TEXT("DROP TRIGGER IF EXISTS fts_variables_ad;"));
	ExecuteSQL(TEXT("DROP TRIGGER IF EXISTS fts_variables_au;"));
	ExecuteSQL(TEXT("DROP TRIGGER IF EXISTS fts_parameters_ai;"));
	ExecuteSQL(TEXT("DROP TRIGGER IF EXISTS fts_parameters_ad;"));
	ExecuteSQL(TEXT("DROP TRIGGER IF EXISTS fts_parameters_au;"));
	ExecuteSQL(TEXT("DROP TRIGGER IF EXISTS fts_datatable_rows_ai;"));
	ExecuteSQL(TEXT("DROP TRIGGER IF EXISTS fts_datatable_rows_ad;"));
	ExecuteSQL(TEXT("DROP TRIGGER IF EXISTS fts_datatable_rows_au;"));
	ExecuteSQL(TEXT("DROP TRIGGER IF EXISTS fts_actors_ai;"));
	ExecuteSQL(TEXT("DROP TRIGGER IF EXISTS fts_actors_ad;"));
	ExecuteSQL(TEXT("DROP TRIGGER IF EXISTS fts_actors_au;"));
	ExecuteSQL(TEXT("DROP TRIGGER IF EXISTS fts_asset_search_values_ai;"));
	ExecuteSQL(TEXT("DROP TRIGGER IF EXISTS fts_asset_search_values_ad;"));
	ExecuteSQL(TEXT("DROP TRIGGER IF EXISTS fts_asset_search_values_au;"));
	ExecuteSQL(TEXT("DROP TABLE IF EXISTS fts_assets;"));
	ExecuteSQL(TEXT("DROP TABLE IF EXISTS fts_nodes;"));
	ExecuteSQL(TEXT("DROP TABLE IF EXISTS fts_variables;"));
	ExecuteSQL(TEXT("DROP TABLE IF EXISTS fts_parameters;"));
	ExecuteSQL(TEXT("DROP TABLE IF EXISTS fts_datatable_rows;"));
	ExecuteSQL(TEXT("DROP TABLE IF EXISTS fts_actors;"));
	ExecuteSQL(TEXT("DROP TABLE IF EXISTS fts_asset_search_values;"));
	ExecuteSQL(TEXT("DROP TABLE IF EXISTS tag_references;"));
	ExecuteSQL(TEXT("DROP TABLE IF EXISTS tags;"));
	ExecuteSQL(TEXT("DROP TABLE IF EXISTS connections;"));
	ExecuteSQL(TEXT("DROP TABLE IF EXISTS nodes;"));
	ExecuteSQL(TEXT("DROP TABLE IF EXISTS variables;"));
	ExecuteSQL(TEXT("DROP TABLE IF EXISTS parameters;"));
	ExecuteSQL(TEXT("DROP TABLE IF EXISTS dependencies;"));
	ExecuteSQL(TEXT("DROP TABLE IF EXISTS actors;"));
	ExecuteSQL(TEXT("DROP TABLE IF EXISTS configs;"));
	ExecuteSQL(TEXT("DROP TABLE IF EXISTS cpp_symbols;"));
	ExecuteSQL(TEXT("DROP TABLE IF EXISTS datatable_rows;"));
	ExecuteSQL(TEXT("DROP TABLE IF EXISTS asset_search_values;"));
	ExecuteSQL(TEXT("DROP TABLE IF EXISTS meta;"));
	ExecuteSQL(TEXT("DROP TABLE IF EXISTS assets;"));

	if (!CreateTables())
	{
		return false;
	}

	return WriteMeta(TEXT("schema_version"), TEXT("2"))
		&& WriteMeta(TEXT("asset_search_values_schema_version"), TEXT("1"))
		&& WriteMeta(TEXT("asset_search_values_extractor_version"), TEXT("2"));
}

// ============================================================
// Transaction helpers
// ============================================================

bool FMonolithIndexDatabase::BeginTransaction()
{
	return ExecuteSQL(TEXT("BEGIN TRANSACTION;"));
}

bool FMonolithIndexDatabase::CommitTransaction()
{
	return ExecuteSQL(TEXT("COMMIT;"));
}

bool FMonolithIndexDatabase::RollbackTransaction()
{
	return ExecuteSQL(TEXT("ROLLBACK;"));
}

// ============================================================
// Asset CRUD
// ============================================================

int64 FMonolithIndexDatabase::InsertAsset(const FIndexedAsset& Asset)
{
	if (!IsOpen()) return -1;

	FSQLitePreparedStatement Stmt;
	Stmt.Create(*Database, TEXT("INSERT INTO assets (package_path, asset_name, asset_class, module_name, description, file_size_bytes, last_modified, saved_hash) VALUES (?, ?, ?, ?, ?, ?, ?, ?);"));
	Stmt.SetBindingValueByIndex(1, Asset.PackagePath);
	Stmt.SetBindingValueByIndex(2, Asset.AssetName);
	Stmt.SetBindingValueByIndex(3, Asset.AssetClass);
	Stmt.SetBindingValueByIndex(4, Asset.ModuleName);
	Stmt.SetBindingValueByIndex(5, Asset.Description);
	Stmt.SetBindingValueByIndex(6, Asset.FileSizeBytes);
	Stmt.SetBindingValueByIndex(7, Asset.LastModified);
	Stmt.SetBindingValueByIndex(8, Asset.SavedHash);

	if (!Stmt.Execute()) return -1;
	return Database->GetLastInsertRowId();
}

TOptional<FIndexedAsset> FMonolithIndexDatabase::GetAssetByPath(const FString& PackagePath)
{
	if (!IsOpen()) return {};

	FSQLitePreparedStatement Stmt;
	Stmt.Create(*Database, TEXT("SELECT id, package_path, asset_name, asset_class, module_name, description, file_size_bytes, last_modified, saved_hash, indexed_at FROM assets WHERE package_path = ?;"));
	Stmt.SetBindingValueByIndex(1, PackagePath);

	if (Stmt.Step() == ESQLitePreparedStatementStepResult::Row)
	{
		FIndexedAsset Asset;
		Stmt.GetColumnValueByIndex(0, Asset.Id);
		Stmt.GetColumnValueByIndex(1, Asset.PackagePath);
		Stmt.GetColumnValueByIndex(2, Asset.AssetName);
		Stmt.GetColumnValueByIndex(3, Asset.AssetClass);
		Stmt.GetColumnValueByIndex(4, Asset.ModuleName);
		Stmt.GetColumnValueByIndex(5, Asset.Description);
		Stmt.GetColumnValueByIndex(6, Asset.FileSizeBytes);
		Stmt.GetColumnValueByIndex(7, Asset.LastModified);
		Stmt.GetColumnValueByIndex(8, Asset.SavedHash);
		Stmt.GetColumnValueByIndex(9, Asset.IndexedAt);
		return Asset;
	}
	return {};
}

int64 FMonolithIndexDatabase::GetAssetId(const FString& PackagePath)
{
	if (!IsOpen()) return -1;

	FSQLitePreparedStatement Stmt;
	Stmt.Create(*Database, TEXT("SELECT id FROM assets WHERE package_path = ?;"));
	Stmt.SetBindingValueByIndex(1, PackagePath);

	if (Stmt.Step() == ESQLitePreparedStatementStepResult::Row)
	{
		int64 Id = 0;
		Stmt.GetColumnValueByIndex(0, Id);
		return Id;
	}
	return -1;
}

bool FMonolithIndexDatabase::DeleteAssetAndRelated(int64 AssetId)
{
	// CASCADE handles child rows
	return ExecuteSQL(FString::Printf(TEXT("DELETE FROM assets WHERE id = %lld;"), AssetId));
}

// ============================================================
// Node CRUD
// ============================================================

int64 FMonolithIndexDatabase::InsertNode(const FIndexedNode& Node)
{
	if (!IsOpen()) return -1;

	FSQLitePreparedStatement Stmt;
	Stmt.Create(*Database, TEXT("INSERT INTO nodes (asset_id, node_type, node_name, node_class, properties, pos_x, pos_y) VALUES (?, ?, ?, ?, ?, ?, ?);"));
	Stmt.SetBindingValueByIndex(1, Node.AssetId);
	Stmt.SetBindingValueByIndex(2, Node.NodeType);
	Stmt.SetBindingValueByIndex(3, Node.NodeName);
	Stmt.SetBindingValueByIndex(4, Node.NodeClass);
	Stmt.SetBindingValueByIndex(5, Node.Properties);
	Stmt.SetBindingValueByIndex(6, static_cast<int64>(Node.PosX));
	Stmt.SetBindingValueByIndex(7, static_cast<int64>(Node.PosY));

	if (!Stmt.Execute()) return -1;
	return Database->GetLastInsertRowId();
}

TArray<FIndexedNode> FMonolithIndexDatabase::GetNodesForAsset(int64 AssetId)
{
	TArray<FIndexedNode> Result;
	if (!IsOpen()) return Result;

	FSQLitePreparedStatement Stmt;
	Stmt.Create(*Database, TEXT("SELECT id, asset_id, node_type, node_name, node_class, properties, pos_x, pos_y FROM nodes WHERE asset_id = ?;"));
	Stmt.SetBindingValueByIndex(1, AssetId);

	while (Stmt.Step() == ESQLitePreparedStatementStepResult::Row)
	{
		FIndexedNode Node;
		Stmt.GetColumnValueByIndex(0, Node.Id);
		Stmt.GetColumnValueByIndex(1, Node.AssetId);
		Stmt.GetColumnValueByIndex(2, Node.NodeType);
		Stmt.GetColumnValueByIndex(3, Node.NodeName);
		Stmt.GetColumnValueByIndex(4, Node.NodeClass);
		Stmt.GetColumnValueByIndex(5, Node.Properties);
		Stmt.GetColumnValueByIndex(6, Node.PosX);
		Stmt.GetColumnValueByIndex(7, Node.PosY);
		Result.Add(MoveTemp(Node));
	}
	return Result;
}

// ============================================================
// Connection CRUD
// ============================================================

int64 FMonolithIndexDatabase::InsertConnection(const FIndexedConnection& Conn)
{
	if (!IsOpen()) return -1;

	FSQLitePreparedStatement Stmt;
	Stmt.Create(*Database, TEXT("INSERT INTO connections (source_node_id, source_pin, target_node_id, target_pin, pin_type) VALUES (?, ?, ?, ?, ?);"));
	Stmt.SetBindingValueByIndex(1, Conn.SourceNodeId);
	Stmt.SetBindingValueByIndex(2, Conn.SourcePin);
	Stmt.SetBindingValueByIndex(3, Conn.TargetNodeId);
	Stmt.SetBindingValueByIndex(4, Conn.TargetPin);
	Stmt.SetBindingValueByIndex(5, Conn.PinType);

	if (!Stmt.Execute()) return -1;
	return Database->GetLastInsertRowId();
}

TArray<FIndexedConnection> FMonolithIndexDatabase::GetConnectionsForAsset(int64 AssetId)
{
	TArray<FIndexedConnection> Result;
	if (!IsOpen()) return Result;

	FSQLitePreparedStatement Stmt;
	Stmt.Create(*Database, TEXT("SELECT c.id, c.source_node_id, c.source_pin, c.target_node_id, c.target_pin, c.pin_type FROM connections c INNER JOIN nodes n ON c.source_node_id = n.id WHERE n.asset_id = ?;"));
	Stmt.SetBindingValueByIndex(1, AssetId);

	while (Stmt.Step() == ESQLitePreparedStatementStepResult::Row)
	{
		FIndexedConnection Conn;
		Stmt.GetColumnValueByIndex(0, Conn.Id);
		Stmt.GetColumnValueByIndex(1, Conn.SourceNodeId);
		Stmt.GetColumnValueByIndex(2, Conn.SourcePin);
		Stmt.GetColumnValueByIndex(3, Conn.TargetNodeId);
		Stmt.GetColumnValueByIndex(4, Conn.TargetPin);
		Stmt.GetColumnValueByIndex(5, Conn.PinType);
		Result.Add(MoveTemp(Conn));
	}
	return Result;
}

// ============================================================
// Variable CRUD
// ============================================================

int64 FMonolithIndexDatabase::InsertVariable(const FIndexedVariable& Var)
{
	if (!IsOpen()) return -1;

	FSQLitePreparedStatement Stmt;
	Stmt.Create(*Database, TEXT("INSERT INTO variables (asset_id, var_name, var_type, category, default_value, is_exposed, is_replicated) VALUES (?, ?, ?, ?, ?, ?, ?);"));
	Stmt.SetBindingValueByIndex(1, Var.AssetId);
	Stmt.SetBindingValueByIndex(2, Var.VarName);
	Stmt.SetBindingValueByIndex(3, Var.VarType);
	Stmt.SetBindingValueByIndex(4, Var.Category);
	Stmt.SetBindingValueByIndex(5, Var.DefaultValue);
	Stmt.SetBindingValueByIndex(6, static_cast<int64>(Var.bIsExposed ? 1 : 0));
	Stmt.SetBindingValueByIndex(7, static_cast<int64>(Var.bIsReplicated ? 1 : 0));

	if (!Stmt.Execute()) return -1;
	return Database->GetLastInsertRowId();
}

TArray<FIndexedVariable> FMonolithIndexDatabase::GetVariablesForAsset(int64 AssetId)
{
	TArray<FIndexedVariable> Result;
	if (!IsOpen()) return Result;

	FSQLitePreparedStatement Stmt;
	Stmt.Create(*Database, TEXT("SELECT id, asset_id, var_name, var_type, category, default_value, is_exposed, is_replicated FROM variables WHERE asset_id = ?;"));
	Stmt.SetBindingValueByIndex(1, AssetId);

	while (Stmt.Step() == ESQLitePreparedStatementStepResult::Row)
	{
		FIndexedVariable Var;
		Stmt.GetColumnValueByIndex(0, Var.Id);
		Stmt.GetColumnValueByIndex(1, Var.AssetId);
		Stmt.GetColumnValueByIndex(2, Var.VarName);
		Stmt.GetColumnValueByIndex(3, Var.VarType);
		Stmt.GetColumnValueByIndex(4, Var.Category);
		Stmt.GetColumnValueByIndex(5, Var.DefaultValue);
		int32 Exposed = 0, Replicated = 0;
		Stmt.GetColumnValueByIndex(6, Exposed);
		Stmt.GetColumnValueByIndex(7, Replicated);
		Var.bIsExposed = Exposed != 0;
		Var.bIsReplicated = Replicated != 0;
		Result.Add(MoveTemp(Var));
	}
	return Result;
}

// ============================================================
// Parameter CRUD
// ============================================================

int64 FMonolithIndexDatabase::InsertParameter(const FIndexedParameter& Param)
{
	if (!IsOpen()) return -1;

	FSQLitePreparedStatement Stmt;
	Stmt.Create(*Database, TEXT("INSERT INTO parameters (asset_id, param_name, param_type, param_group, default_value, source) VALUES (?, ?, ?, ?, ?, ?);"));
	Stmt.SetBindingValueByIndex(1, Param.AssetId);
	Stmt.SetBindingValueByIndex(2, Param.ParamName);
	Stmt.SetBindingValueByIndex(3, Param.ParamType);
	Stmt.SetBindingValueByIndex(4, Param.ParamGroup);
	Stmt.SetBindingValueByIndex(5, Param.DefaultValue);
	Stmt.SetBindingValueByIndex(6, Param.Source);

	if (!Stmt.Execute()) return -1;
	return Database->GetLastInsertRowId();
}

// ============================================================
// Dependency CRUD
// ============================================================

int64 FMonolithIndexDatabase::InsertDependency(const FIndexedDependency& Dep)
{
	if (!IsOpen()) return -1;

	FSQLitePreparedStatement Stmt;
	Stmt.Create(*Database, TEXT("INSERT INTO dependencies (source_asset_id, target_asset_id, dependency_type) VALUES (?, ?, ?);"));
	Stmt.SetBindingValueByIndex(1, Dep.SourceAssetId);
	Stmt.SetBindingValueByIndex(2, Dep.TargetAssetId);
	Stmt.SetBindingValueByIndex(3, Dep.DependencyType);

	if (!Stmt.Execute()) return -1;
	return Database->GetLastInsertRowId();
}

TArray<FIndexedDependency> FMonolithIndexDatabase::GetDependenciesForAsset(int64 AssetId)
{
	TArray<FIndexedDependency> Result;
	if (!IsOpen()) return Result;

	FSQLitePreparedStatement Stmt;
	Stmt.Create(*Database, TEXT("SELECT id, source_asset_id, target_asset_id, dependency_type FROM dependencies WHERE source_asset_id = ?;"));
	Stmt.SetBindingValueByIndex(1, AssetId);

	while (Stmt.Step() == ESQLitePreparedStatementStepResult::Row)
	{
		FIndexedDependency Dep;
		Stmt.GetColumnValueByIndex(0, Dep.Id);
		Stmt.GetColumnValueByIndex(1, Dep.SourceAssetId);
		Stmt.GetColumnValueByIndex(2, Dep.TargetAssetId);
		Stmt.GetColumnValueByIndex(3, Dep.DependencyType);
		Result.Add(MoveTemp(Dep));
	}
	return Result;
}

TArray<FIndexedDependency> FMonolithIndexDatabase::GetReferencersOfAsset(int64 AssetId)
{
	TArray<FIndexedDependency> Result;
	if (!IsOpen()) return Result;

	FSQLitePreparedStatement Stmt;
	Stmt.Create(*Database, TEXT("SELECT id, source_asset_id, target_asset_id, dependency_type FROM dependencies WHERE target_asset_id = ?;"));
	Stmt.SetBindingValueByIndex(1, AssetId);

	while (Stmt.Step() == ESQLitePreparedStatementStepResult::Row)
	{
		FIndexedDependency Dep;
		Stmt.GetColumnValueByIndex(0, Dep.Id);
		Stmt.GetColumnValueByIndex(1, Dep.SourceAssetId);
		Stmt.GetColumnValueByIndex(2, Dep.TargetAssetId);
		Stmt.GetColumnValueByIndex(3, Dep.DependencyType);
		Result.Add(MoveTemp(Dep));
	}
	return Result;
}

// ============================================================
// Actor CRUD
// ============================================================

int64 FMonolithIndexDatabase::InsertActor(const FIndexedActor& Actor)
{
	if (!IsOpen()) return -1;

	FSQLitePreparedStatement Stmt;
	Stmt.Create(*Database, TEXT("INSERT INTO actors (asset_id, actor_name, actor_class, actor_label, transform, components) VALUES (?, ?, ?, ?, ?, ?);"));
	Stmt.SetBindingValueByIndex(1, Actor.AssetId);
	Stmt.SetBindingValueByIndex(2, Actor.ActorName);
	Stmt.SetBindingValueByIndex(3, Actor.ActorClass);
	Stmt.SetBindingValueByIndex(4, Actor.ActorLabel);
	Stmt.SetBindingValueByIndex(5, Actor.Transform);
	Stmt.SetBindingValueByIndex(6, Actor.Components);

	if (!Stmt.Execute()) return -1;
	return Database->GetLastInsertRowId();
}

// ============================================================
// Tag CRUD
// ============================================================

int64 FMonolithIndexDatabase::InsertTag(const FIndexedTag& Tag)
{
	if (!IsOpen()) return -1;

	FSQLitePreparedStatement Stmt;
	Stmt.Create(*Database, TEXT("INSERT OR IGNORE INTO tags (tag_name, parent_tag, reference_count) VALUES (?, ?, ?);"));
	Stmt.SetBindingValueByIndex(1, Tag.TagName);
	Stmt.SetBindingValueByIndex(2, Tag.ParentTag);
	Stmt.SetBindingValueByIndex(3, static_cast<int64>(Tag.ReferenceCount));

	if (!Stmt.Execute()) return -1;
	return GetOrCreateTag(Tag.TagName, Tag.ParentTag);
}

int64 FMonolithIndexDatabase::GetOrCreateTag(const FString& TagName, const FString& ParentTag)
{
	if (!IsOpen()) return -1;

	// Try to get existing
	FSQLitePreparedStatement Stmt;
	Stmt.Create(*Database, TEXT("SELECT id FROM tags WHERE tag_name = ?;"));
	Stmt.SetBindingValueByIndex(1, TagName);

	if (Stmt.Step() == ESQLitePreparedStatementStepResult::Row)
	{
		int64 Id = 0;
		Stmt.GetColumnValueByIndex(0, Id);
		return Id;
	}

	// Insert new
	FSQLitePreparedStatement InsertStmt;
	InsertStmt.Create(*Database, TEXT("INSERT INTO tags (tag_name, parent_tag) VALUES (?, ?);"));
	InsertStmt.SetBindingValueByIndex(1, TagName);
	InsertStmt.SetBindingValueByIndex(2, ParentTag);
	InsertStmt.Execute();
	return Database->GetLastInsertRowId();
}

int64 FMonolithIndexDatabase::InsertTagReference(const FIndexedTagReference& Ref)
{
	if (!IsOpen()) return -1;

	FSQLitePreparedStatement Stmt;
	Stmt.Create(*Database, TEXT("INSERT INTO tag_references (tag_id, asset_id, context) VALUES (?, ?, ?);"));
	Stmt.SetBindingValueByIndex(1, Ref.TagId);
	Stmt.SetBindingValueByIndex(2, Ref.AssetId);
	Stmt.SetBindingValueByIndex(3, Ref.Context);

	if (!Stmt.Execute()) return -1;

	// Update reference count
	FSQLitePreparedStatement UpdateStmt;
	UpdateStmt.Create(*Database, TEXT("UPDATE tags SET reference_count = (SELECT COUNT(*) FROM tag_references WHERE tag_id = ?) WHERE id = ?;"));
	UpdateStmt.SetBindingValueByIndex(1, Ref.TagId);
	UpdateStmt.SetBindingValueByIndex(2, Ref.TagId);
	UpdateStmt.Execute();

	return Database->GetLastInsertRowId();
}

// ============================================================
// Config CRUD
// ============================================================

int64 FMonolithIndexDatabase::InsertConfig(const FIndexedConfig& Config)
{
	if (!IsOpen()) return -1;

	FSQLitePreparedStatement Stmt;
	Stmt.Create(*Database, TEXT("INSERT INTO configs (file_path, section, key, value) VALUES (?, ?, ?, ?);"));
	Stmt.SetBindingValueByIndex(1, Config.FilePath);
	Stmt.SetBindingValueByIndex(2, Config.Section);
	Stmt.SetBindingValueByIndex(3, Config.Key);
	Stmt.SetBindingValueByIndex(4, Config.Value);

	if (!Stmt.Execute()) return -1;
	return Database->GetLastInsertRowId();
}

// ============================================================
// C++ Symbol CRUD
// ============================================================

int64 FMonolithIndexDatabase::InsertCppSymbol(const FIndexedCppSymbol& Symbol)
{
	if (!IsOpen()) return -1;

	FSQLitePreparedStatement Stmt;
	Stmt.Create(*Database, TEXT("INSERT INTO cpp_symbols (file_path, symbol_name, symbol_type, signature, line_number, parent_symbol) VALUES (?, ?, ?, ?, ?, ?);"));
	Stmt.SetBindingValueByIndex(1, Symbol.FilePath);
	Stmt.SetBindingValueByIndex(2, Symbol.SymbolName);
	Stmt.SetBindingValueByIndex(3, Symbol.SymbolType);
	Stmt.SetBindingValueByIndex(4, Symbol.Signature);
	Stmt.SetBindingValueByIndex(5, static_cast<int64>(Symbol.LineNumber));
	Stmt.SetBindingValueByIndex(6, Symbol.ParentSymbol);

	if (!Stmt.Execute()) return -1;
	return Database->GetLastInsertRowId();
}

// ============================================================
// DataTable Row CRUD
// ============================================================

int64 FMonolithIndexDatabase::InsertDataTableRow(const FIndexedDataTableRow& Row)
{
	if (!IsOpen()) return -1;

	FSQLitePreparedStatement Stmt;
	Stmt.Create(*Database, TEXT("INSERT INTO datatable_rows (asset_id, row_name, row_data) VALUES (?, ?, ?);"));
	Stmt.SetBindingValueByIndex(1, Row.AssetId);
	Stmt.SetBindingValueByIndex(2, Row.RowName);
	Stmt.SetBindingValueByIndex(3, Row.RowData);

	if (!Stmt.Execute()) return -1;
	return Database->GetLastInsertRowId();
}

// ============================================================
// Supplemental Search Value CRUD
// ============================================================

int64 FMonolithIndexDatabase::InsertAssetSearchValue(const FIndexedSearchValue& Value)
{
	if (!IsOpen() || Value.AssetId <= 0 || Value.ValueText.TrimStartAndEnd().IsEmpty())
	{
		return -1;
	}

	FSQLitePreparedStatement Stmt;
	Stmt.Create(*Database, TEXT(
		"INSERT INTO asset_search_values "
		"(asset_id, source_kind, object_name, object_path, object_class, field_name, field_path, value_text, signal) "
		"VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?);"));
	Stmt.SetBindingValueByIndex(1, Value.AssetId);
	Stmt.SetBindingValueByIndex(2, Value.SourceKind);
	Stmt.SetBindingValueByIndex(3, Value.ObjectName);
	Stmt.SetBindingValueByIndex(4, Value.ObjectPath);
	Stmt.SetBindingValueByIndex(5, Value.ObjectClass);
	Stmt.SetBindingValueByIndex(6, Value.FieldName);
	Stmt.SetBindingValueByIndex(7, Value.FieldPath);
	Stmt.SetBindingValueByIndex(8, Value.ValueText);
	Stmt.SetBindingValueByIndex(9, Value.Signal);

	if (!Stmt.Execute()) return -1;
	return Database->GetLastInsertRowId();
}

bool FMonolithIndexDatabase::DeleteAssetSearchValuesForAsset(int64 AssetId)
{
	if (!IsOpen()) return false;

	FSQLitePreparedStatement Stmt;
	Stmt.Create(*Database, TEXT("DELETE FROM asset_search_values WHERE asset_id = ?;"));
	Stmt.SetBindingValueByIndex(1, AssetId);
	return Stmt.Execute();
}

bool FMonolithIndexDatabase::DeleteAssetSearchValuesBySourceKind(const FString& SearchSourceKind)
{
	if (!IsOpen()) return false;

	FSQLitePreparedStatement Stmt;
	Stmt.Create(*Database, TEXT("DELETE FROM asset_search_values WHERE source_kind = ?;"));
	Stmt.SetBindingValueByIndex(1, SearchSourceKind);
	return Stmt.Execute();
}

// ============================================================
// Meta
// ============================================================

bool FMonolithIndexDatabase::WriteMeta(const FString& Key, const FString& Value)
{
	if (!IsOpen()) return false;

	FSQLitePreparedStatement Stmt;
	Stmt.Create(*Database, TEXT("INSERT OR REPLACE INTO meta (key, value) VALUES (?, ?);"));
	Stmt.SetBindingValueByIndex(1, Key);
	Stmt.SetBindingValueByIndex(2, Value);
	return Stmt.Execute();
}

FString FMonolithIndexDatabase::ReadMeta(const FString& Key) const
{
	if (!Database || !Database->IsValid()) return FString();

	FSQLitePreparedStatement Stmt;
	Stmt.Create(*Database, TEXT("SELECT value FROM meta WHERE key = ?;"));
	Stmt.SetBindingValueByIndex(1, Key);

	if (Stmt.Step() == ESQLitePreparedStatementStepResult::Row)
	{
		FString Value;
		Stmt.GetColumnValueByIndex(0, Value);
		return Value;
	}
	return FString();
}

// ============================================================
// Incremental indexing helpers
// ============================================================

TArray<FString> FMonolithIndexDatabase::GetAllIndexedPaths()
{
	TArray<FString> Result;
	if (!IsOpen()) return Result;

	FSQLitePreparedStatement Stmt;
	Stmt.Create(*Database, TEXT("SELECT package_path FROM assets;"));

	while (Stmt.Step() == ESQLitePreparedStatementStepResult::Row)
	{
		FString Path;
		Stmt.GetColumnValueByIndex(0, Path);
		Result.Add(MoveTemp(Path));
	}
	return Result;
}

FString FMonolithIndexDatabase::GetSavedHash(const FString& PackagePath)
{
	if (!IsOpen()) return FString();

	FSQLitePreparedStatement Stmt;
	Stmt.Create(*Database, TEXT("SELECT saved_hash FROM assets WHERE package_path = ?;"));
	Stmt.SetBindingValueByIndex(1, PackagePath);

	if (Stmt.Step() == ESQLitePreparedStatementStepResult::Row)
	{
		FString Hash;
		Stmt.GetColumnValueByIndex(0, Hash);
		return Hash;
	}
	return FString();
}

TMap<FString, FString> FMonolithIndexDatabase::GetAllPathsAndHashes()
{
	TMap<FString, FString> Result;
	if (!IsOpen()) return Result;

	FSQLitePreparedStatement Stmt;
	Stmt.Create(*Database, TEXT("SELECT package_path, saved_hash FROM assets;"));

	while (Stmt.Step() == ESQLitePreparedStatementStepResult::Row)
	{
		FString Path, Hash;
		Stmt.GetColumnValueByIndex(0, Path);
		Stmt.GetColumnValueByIndex(1, Hash);
		Result.Add(MoveTemp(Path), MoveTemp(Hash));
	}
	return Result;
}

bool FMonolithIndexDatabase::DeleteAssetByPath(const FString& PackagePath)
{
	if (!IsOpen()) return false;

	int64 AssetId = GetAssetId(PackagePath);
	if (AssetId < 0) return false;

	return DeleteAssetAndRelated(AssetId);
}

bool FMonolithIndexDatabase::UpdateAssetPath(const FString& OldPath, const FString& NewPath, const FString& NewAssetName)
{
	if (!IsOpen()) return false;

	FSQLitePreparedStatement Stmt;
	Stmt.Create(*Database, TEXT("UPDATE assets SET package_path = ?, asset_name = COALESCE(NULLIF(?, ''), asset_name) WHERE package_path = ?;"));
	Stmt.SetBindingValueByIndex(1, NewPath);
	Stmt.SetBindingValueByIndex(2, NewAssetName);
	Stmt.SetBindingValueByIndex(3, OldPath);

	if (!Stmt.Execute()) return false;

	// Check if a row was actually updated
	// GetLastInsertRowId isn't useful for UPDATE; use changes count via a follow-up query
	FSQLitePreparedStatement ChangesStmt;
	ChangesStmt.Create(*Database, TEXT("SELECT changes();"));
	if (ChangesStmt.Step() == ESQLitePreparedStatementStepResult::Row)
	{
		int64 Changes = 0;
		ChangesStmt.GetColumnValueByIndex(0, Changes);
		return Changes > 0;
	}
	return false;
}

bool FMonolithIndexDatabase::UpdateAssetMetadata(const FIndexedAsset& Asset)
{
	if (!IsOpen()) return false;

	FSQLitePreparedStatement Stmt;
	Stmt.Create(*Database, TEXT("UPDATE assets SET asset_name = ?, asset_class = ?, module_name = ?, description = ?, file_size_bytes = ?, last_modified = ?, saved_hash = ?, indexed_at = datetime('now') WHERE package_path = ?;"));
	Stmt.SetBindingValueByIndex(1, Asset.AssetName);
	Stmt.SetBindingValueByIndex(2, Asset.AssetClass);
	Stmt.SetBindingValueByIndex(3, Asset.ModuleName);
	Stmt.SetBindingValueByIndex(4, Asset.Description);
	Stmt.SetBindingValueByIndex(5, Asset.FileSizeBytes);
	Stmt.SetBindingValueByIndex(6, Asset.LastModified);
	Stmt.SetBindingValueByIndex(7, Asset.SavedHash);
	Stmt.SetBindingValueByIndex(8, Asset.PackagePath);

	if (!Stmt.Execute()) return false;

	FSQLitePreparedStatement ChangesStmt;
	ChangesStmt.Create(*Database, TEXT("SELECT changes();"));
	if (ChangesStmt.Step() == ESQLitePreparedStatementStepResult::Row)
	{
		int64 Changes = 0;
		ChangesStmt.GetColumnValueByIndex(0, Changes);
		return Changes > 0;
	}
	return false;
}

// Deletes per-asset child data that deep indexing repopulates (nodes, variables, parameters, datatable_rows, search_values).
// Does NOT delete: dependencies (DependencyIndexer sentinel), tag_references (GameplayTagIndexer sentinel),
// actors (LevelIndexer sentinel). Those are scoped separately.
bool FMonolithIndexDatabase::DeleteChildDataForAsset(int64 AssetId)
{
	if (!IsOpen()) return false;

	bool bSuccess = true;

	FSQLitePreparedStatement Stmt1;
	Stmt1.Create(*Database, TEXT("DELETE FROM nodes WHERE asset_id = ?;"));
	Stmt1.SetBindingValueByIndex(1, AssetId);
	bSuccess &= Stmt1.Execute();

	FSQLitePreparedStatement Stmt2;
	Stmt2.Create(*Database, TEXT("DELETE FROM variables WHERE asset_id = ?;"));
	Stmt2.SetBindingValueByIndex(1, AssetId);
	bSuccess &= Stmt2.Execute();

	FSQLitePreparedStatement Stmt3;
	Stmt3.Create(*Database, TEXT("DELETE FROM parameters WHERE asset_id = ?;"));
	Stmt3.SetBindingValueByIndex(1, AssetId);
	bSuccess &= Stmt3.Execute();

	FSQLitePreparedStatement Stmt4;
	Stmt4.Create(*Database, TEXT("DELETE FROM datatable_rows WHERE asset_id = ?;"));
	Stmt4.SetBindingValueByIndex(1, AssetId);
	bSuccess &= Stmt4.Execute();

	FSQLitePreparedStatement Stmt5;
	Stmt5.Create(*Database, TEXT("DELETE FROM asset_search_values WHERE asset_id = ?;"));
	Stmt5.SetBindingValueByIndex(1, AssetId);
	bSuccess &= Stmt5.Execute();

	return bSuccess;
}

bool FMonolithIndexDatabase::UpdateSavedHash(const FString& PackagePath, const FString& HashHex)
{
	if (!IsOpen()) return false;

	FSQLitePreparedStatement Stmt;
	Stmt.Create(*Database, TEXT("UPDATE assets SET saved_hash = ? WHERE package_path = ?;"));
	Stmt.SetBindingValueByIndex(1, HashHex);
	Stmt.SetBindingValueByIndex(2, PackagePath);

	if (!Stmt.Execute()) return false;

	FSQLitePreparedStatement ChangesStmt;
	ChangesStmt.Create(*Database, TEXT("SELECT changes();"));
	if (ChangesStmt.Step() == ESQLitePreparedStatementStepResult::Row)
	{
		int64 Changes = 0;
		ChangesStmt.GetColumnValueByIndex(0, Changes);
		return Changes > 0;
	}
	return false;
}

// ============================================================
// FTS5 Full-text search
// ============================================================

// Number of K-combinations of N items, saturating to avoid overflow. Used to bound the
// min-should-match OR-of-subsets MATCH expression so a many-token query cannot explode it.
static int64 MonolithCombinationCount(int32 N, int32 K)
{
	if (K < 0 || K > N) { return 0; }
	K = FMath::Min(K, N - K);
	int64 Result = 1;
	for (int32 i = 0; i < K; ++i)
	{
		Result = Result * (N - i) / (i + 1);
		if (Result > 1000000) { return Result; }
	}
	return Result;
}

FString FMonolithIndexDatabase::EscapeFTS(const FString& Query, int32 MinShouldMatchPct)
{
	// Replace :: with space
	FString Q = Query.Replace(TEXT("::"), TEXT(" "));

	// Replace non-alphanumeric/non-underscore with space
	FString Cleaned;
	Cleaned.Reserve(Q.Len());
	for (TCHAR Ch : Q)
	{
		if (FChar::IsAlnum(Ch) || Ch == TEXT('_'))
		{
			Cleaned += Ch;
		}
		else
		{
			Cleaned += TEXT(' ');
		}
	}

	// Split into tokens, wrap each with quotes and trailing *
	TArray<FString> Tokens;
	Cleaned.ParseIntoArray(Tokens, TEXT(" "), true);

	if (Tokens.Num() == 0)
	{
		return TEXT("\"\""); // Empty search string
	}

	// Prefix term for each token ("tok"* — case-insensitive prefix under unicode61).
	TArray<FString> Prefix;
	Prefix.Reserve(Tokens.Num());
	for (const FString& Token : Tokens)
	{
		Prefix.Add(FString::Printf(TEXT("\"%s\"*"), *Token));
	}

	// min-should-match (PRD AssetSearchSemanticSearch residual, opt-in). Require at least
	// ceil(N*pct/100) of the N query tokens. Expressed NATIVELY in FTS5 as the OR of every
	// K-token subset (each an implicit-AND prefix group): a document matches the OR iff it
	// contains some full K-subset iff it has >= K distinct query tokens — provably exact under
	// the porter/unicode61 tokenizer, with no C++ token reconstruction and no per-token
	// re-query. Bounded: if the subset count would exceed 64 (many-token query) it collapses to
	// require-all. pct==0 (default) skips this entirely and keeps the pure AND-prefix query below.
	if (MinShouldMatchPct > 0 && Tokens.Num() > 1)
	{
		const int32 N = Tokens.Num();
		const int32 K = FMath::Clamp(FMath::DivideAndRoundUp(N * MinShouldMatchPct, 100), 1, N);

		if (K <= 1)
		{
			// "at least 1 of N" — OR of all single tokens (broad recall).
			return FString::Join(Prefix, TEXT(" OR "));
		}
		if (K == N || MonolithCombinationCount(N, K) > 64)
		{
			// "all N tokens" (or bounded fallback) — a single implicit-AND group, no widening.
			return FString::Printf(TEXT("(%s)"), *FString::Join(Prefix, TEXT(" ")));
		}

		// General K-of-N: OR of all K-subsets, each an implicit-AND prefix group.
		TArray<FString> Groups;
		TArray<int32> Idx;
		Idx.SetNum(K);
		for (int32 i = 0; i < K; ++i) { Idx[i] = i; }
		for (;;)
		{
			TArray<FString> Group;
			Group.Reserve(K);
			for (int32 i = 0; i < K; ++i) { Group.Add(Prefix[Idx[i]]); }
			Groups.Add(FString::Printf(TEXT("(%s)"), *FString::Join(Group, TEXT(" "))));

			// Advance to the next lexicographic K-combination of [0, N).
			int32 p = K - 1;
			while (p >= 0 && Idx[p] == N - K + p) { --p; }
			if (p < 0) { break; }
			++Idx[p];
			for (int32 i = p + 1; i < K; ++i) { Idx[i] = Idx[i - 1] + 1; }
		}
		return FString::Join(Groups, TEXT(" OR "));
	}

	// pct == 0 (default): pure safe-token prefix expression. Identifier recall is handled
	// by supplemental indexed values, not by widening the escaped MATCH query itself.
	return FString::Join(Prefix, TEXT(" "));
}

TArray<FSearchResult> FMonolithIndexDatabase::FullTextSearch(const FString& Query, int32 Limit)
{
	return FullTextSearch(Query, Limit, FProjectSearchOptions::AssetNodeOnly());
}

TArray<FSearchResult> FMonolithIndexDatabase::FullTextSearch(const FString& Query, int32 Limit, const FProjectSearchOptions& Options)
{
	TArray<FSearchResult> Results;
	if (!IsOpen()) return Results;
	const int32 ClampedLimit = FMath::Clamp(Limit, 1, 1001);
	const int32 PageOffset = FMath::Max(Options.Offset, 0);
	const FString FTSQuery = EscapeFTS(Query, Options.MinShouldMatchPct);

	// Q4 (PRD AssetSearchSemanticSearch): RRF cross-table fusion. The 7 FTS tables produce
	// bm25 on incomparable scales, so the old concat-and-sort-by-raw-bm25 let a 1-column
	// table's score compete unfairly against a wide table's. Reciprocal Rank Fusion fuses
	// by RANK POSITION (scale-independent): each asset accumulates SourceWeight/(K+rank)
	// across the tables it matched in, dedup'd to one result per asset (package_path). The
	// per-source weight encodes AssetSearch-style name-over-body priority. Tables are
	// oversampled 3x so fusion sees candidate depth before slicing/truncating to the caller's page.
	const int32 MaxSearchWindow = 100000;
	const int64 PageEnd = static_cast<int64>(PageOffset) + static_cast<int64>(ClampedLimit);
	const int32 RequiredWindow = static_cast<int32>(FMath::Min<int64>(PageEnd, MaxSearchWindow));
	const int32 OversampleLimit = FMath::Clamp(FMath::Max(ClampedLimit * 3, RequiredWindow), 1, MaxSearchWindow);
	const float RRFConstant = 60.0f;
	auto SourceWeightFor = [](const FString& Src) -> float
	{
		if (Src == TEXT("asset") || Src == TEXT("node")) return 3.0f;      // identity-bearing
		if (Src == TEXT("supplemental_value")) return 1.0f;                 // catch-all values
		return 2.0f;                                                        // variable/parameter/datatable_row/actor
	};
	// package_path -> fused result; the accumulated RRF score lives in FSearchResult::Rank
	// (higher = better AFTER fusion), and the representative provenance is the first (i.e.
	// highest-priority-table, best-rank) contribution seen for that asset.
	TMap<FString, FSearchResult> Fused;

	// Q6 (PRD AssetSearchSemanticSearch): optional pushed-down scope filters. asset_class is
	// an exact indexed match (idx_assets_class); package_path is a substring LIKE on the
	// UNIQUE-indexed path. Injected as indexed WHERE conditions INSIDE each per-table FTS
	// query (not a C++ post-filter) so scoping stays correct beyond the oversample window.
	FString ScopeClause;
	if (!Options.AssetClassFilter.IsEmpty()) ScopeClause += TEXT(" AND a.asset_class = ?");
	FString EscapedPath;
	if (!Options.PathFilter.IsEmpty())
	{
		ScopeClause += TEXT(" AND a.package_path LIKE ? ESCAPE '\\'");
		EscapedPath = FString::Printf(TEXT("%%%s%%"),
			*Options.PathFilter.Replace(TEXT("\\"), TEXT("\\\\")).Replace(TEXT("%"), TEXT("\\%")).Replace(TEXT("_"), TEXT("\\_")));
	}

	auto AddMatches = [&](const FString& SQL, const FString& MatchSource)
	{
		FString FinalSQL = SQL;
		if (!ScopeClause.IsEmpty())
		{
			// All 7 per-table queries share the "... MATCH ? ORDER BY ..." shape; inject the
			// scope conditions immediately before ORDER BY.
			FinalSQL.ReplaceInline(TEXT(" ORDER BY"), *(ScopeClause + TEXT(" ORDER BY")), ESearchCase::CaseSensitive);
		}
		FSQLitePreparedStatement Stmt;
		if (!Stmt.Create(*Database, *FinalSQL))
		{
			return;
		}
		int32 BindIdx = 1;
		Stmt.SetBindingValueByIndex(BindIdx++, FTSQuery);
		if (!Options.AssetClassFilter.IsEmpty()) Stmt.SetBindingValueByIndex(BindIdx++, Options.AssetClassFilter);
		if (!EscapedPath.IsEmpty()) Stmt.SetBindingValueByIndex(BindIdx++, EscapedPath);
		Stmt.SetBindingValueByIndex(BindIdx++, static_cast<int64>(OversampleLimit));

		const float SourceWeight = SourceWeightFor(MatchSource);
		int32 RankPos = 0;
		while (Stmt.Step() == ESQLitePreparedStatementStepResult::Row)
		{
			const float Contribution = SourceWeight / (RRFConstant + static_cast<float>(RankPos));
			const int32 ThisRankPos = RankPos;
			++RankPos;

			FString AssetPath;
			Stmt.GetColumnValueByIndex(0, AssetPath);
			if (FSearchResult* Existing = Fused.Find(AssetPath))
			{
				// Same asset matched again (another table or row): accumulate evidence,
				// keep the first-seen (highest-priority) provenance.
				Existing->Rank += Contribution;
				// score-explain (opt-in): record this contributing hit's source/rank.
				if (Options.bExplain)
				{
					Existing->ScoreBySource.FindOrAdd(MatchSource) += Contribution;
					++Existing->ContributingHits;
					Existing->BestRank = FMath::Min(Existing->BestRank, ThisRankPos);
				}
				continue;
			}

			FSearchResult R;
			R.AssetPath = MoveTemp(AssetPath);
			Stmt.GetColumnValueByIndex(1, R.AssetName);
			Stmt.GetColumnValueByIndex(2, R.AssetClass);
			Stmt.GetColumnValueByIndex(3, R.ModuleName);
			Stmt.GetColumnValueByIndex(4, R.MatchContext);
			// bm25 (col 5) is intentionally ignored — RRF ranks by position, not score.
			R.MatchSource = MatchSource;
			Stmt.GetColumnValueByIndex(6, R.MatchTable);
			Stmt.GetColumnValueByIndex(7, R.MatchField);
			Stmt.GetColumnValueByIndex(8, R.MatchObjectPath);
			Stmt.GetColumnValueByIndex(9, R.MatchValue);
			R.Rank = Contribution;
			// score-explain (opt-in): seed the provenance for this asset's first contributing hit.
			if (Options.bExplain)
			{
				R.ScoreBySource.FindOrAdd(MatchSource) += Contribution;
				R.ContributingHits = 1;
				R.BestRank = ThisRankPos;
			}
			Fused.Add(R.AssetPath, MoveTemp(R));
		}
	};

	if (Options.bIncludeAssetMatches)
	{
		AddMatches(TEXT(
			"SELECT a.package_path, a.asset_name, a.asset_class, a.module_name, "
			"snippet(fts_assets, -1, '>>>', '<<<', '...', 32) AS ctx, bm25(fts_assets, 10.0, 2.0, 3.0, 1.0, 1.0), "
			"'assets', 'asset', a.package_path, a.asset_name "
			"FROM fts_assets f JOIN assets a ON a.id = f.rowid "
			"WHERE fts_assets MATCH ? ORDER BY bm25(fts_assets, 10.0, 2.0, 3.0, 1.0, 1.0) LIMIT ?;"),
			TEXT("asset"));
	}

	if (Options.bIncludeNodeMatches)
	{
		AddMatches(TEXT(
			"SELECT a.package_path, a.asset_name, a.asset_class, a.module_name, "
			"snippet(fts_nodes, -1, '>>>', '<<<', '...', 32) AS ctx, bm25(fts_nodes, 10.0, 3.0, 2.0), "
			"'nodes', n.node_name, n.node_name, n.node_class "
			"FROM fts_nodes f JOIN nodes n ON n.id = f.rowid JOIN assets a ON a.id = n.asset_id "
			"WHERE fts_nodes MATCH ? ORDER BY bm25(fts_nodes, 10.0, 3.0, 2.0) LIMIT ?;"),
			TEXT("node"));
	}

	if (Options.bIncludeStructuredContent)
	{
		AddMatches(TEXT(
			"SELECT a.package_path, a.asset_name, a.asset_class, a.module_name, "
			"snippet(fts_variables, -1, '>>>', '<<<', '...', 32) AS ctx, bm25(fts_variables, 10.0, 2.0, 1.0, 1.0), "
			"'variables', v.var_name, v.var_name, v.default_value "
			"FROM fts_variables f JOIN variables v ON v.id = f.rowid JOIN assets a ON a.id = v.asset_id "
			"WHERE fts_variables MATCH ? ORDER BY bm25(fts_variables, 10.0, 2.0, 1.0, 1.0) LIMIT ?;"),
			TEXT("variable"));

		AddMatches(TEXT(
			"SELECT a.package_path, a.asset_name, a.asset_class, a.module_name, "
			"snippet(fts_parameters, -1, '>>>', '<<<', '...', 32) AS ctx, bm25(fts_parameters, 10.0, 2.0, 1.0, 1.0, 1.0), "
			"'parameters', p.param_name, p.param_name, p.default_value "
			"FROM fts_parameters f JOIN parameters p ON p.id = f.rowid JOIN assets a ON a.id = p.asset_id "
			"WHERE fts_parameters MATCH ? ORDER BY bm25(fts_parameters, 10.0, 2.0, 1.0, 1.0, 1.0) LIMIT ?;"),
			TEXT("parameter"));

		AddMatches(TEXT(
			"SELECT a.package_path, a.asset_name, a.asset_class, a.module_name, "
			"snippet(fts_datatable_rows, -1, '>>>', '<<<', '...', 32) AS ctx, f.rank, "
			"'datatable_rows', 'row_name', r.row_name, r.row_name "
			"FROM fts_datatable_rows f JOIN datatable_rows r ON r.id = f.rowid JOIN assets a ON a.id = r.asset_id "
			"WHERE fts_datatable_rows MATCH ? ORDER BY f.rank LIMIT ?;"),
			TEXT("datatable_row"));

		AddMatches(TEXT(
			"SELECT a.package_path, a.asset_name, a.asset_class, a.module_name, "
			"snippet(fts_actors, -1, '>>>', '<<<', '...', 32) AS ctx, bm25(fts_actors, 10.0, 3.0, 5.0), "
			"'actors', ac.actor_label, ac.actor_name, ac.actor_class "
			"FROM fts_actors f JOIN actors ac ON ac.id = f.rowid JOIN assets a ON a.id = ac.asset_id "
			"WHERE fts_actors MATCH ? ORDER BY bm25(fts_actors, 10.0, 3.0, 5.0) LIMIT ?;"),
			TEXT("actor"));
	}

	if (Options.bIncludeSupplementalValues)
	{
		AddMatches(TEXT(
			"SELECT a.package_path, a.asset_name, a.asset_class, a.module_name, "
			"snippet(fts_asset_search_values, -1, '>>>', '<<<', '...', 32) AS ctx, f.rank, "
			"'asset_search_values', sv.field_name, sv.object_path, sv.value_text "
			"FROM fts_asset_search_values f JOIN asset_search_values sv ON sv.id = f.rowid JOIN assets a ON a.id = sv.asset_id "
			"WHERE fts_asset_search_values MATCH ? ORDER BY f.rank LIMIT ?;"),
			TEXT("supplemental_value"));
	}

	// Q4: materialize the fused per-asset results and rank by accumulated RRF score
	// (higher = better — the opposite direction of raw FTS5 bm25).
	Fused.GenerateValueArray(Results);
	Results.Sort([](const FSearchResult& A, const FSearchResult& B)
	{
		if (!FMath::IsNearlyEqual(A.Rank, B.Rank))
		{
			return A.Rank > B.Rank;
		}
		return A.AssetPath.Compare(B.AssetPath, ESearchCase::IgnoreCase) < 0;
	});

	if (PageOffset > 0 || Results.Num() > ClampedLimit)
	{
		const int32 SliceStart = FMath::Min(PageOffset, Results.Num());
		const int32 SliceEnd = FMath::Min(SliceStart + ClampedLimit, Results.Num());
		TArray<FSearchResult> SlicedResults;
		SlicedResults.Reserve(SliceEnd - SliceStart);
		for (int32 Index = SliceStart; Index < SliceEnd; ++Index)
		{
			SlicedResults.Add(Results[Index]);
		}
		Results = SlicedResults;
	}

	return Results;
}

// ============================================================
// Stats
// ============================================================

TSharedPtr<FJsonObject> FMonolithIndexDatabase::GetStats()
{
	auto Stats = MakeShared<FJsonObject>();
	if (!IsOpen()) return Stats;

	auto GetCount = [this](const TCHAR* Table) -> int64
	{
		FSQLitePreparedStatement Stmt;
		FString SQL = FString::Printf(TEXT("SELECT COUNT(*) FROM %s;"), Table);
		Stmt.Create(*Database, *SQL);
		if (Stmt.Step() == ESQLitePreparedStatementStepResult::Row)
		{
			int64 Count = 0;
			Stmt.GetColumnValueByIndex(0, Count);
			return Count;
		}
		return 0;
	};

	Stats->SetNumberField(TEXT("assets"), GetCount(TEXT("assets")));
	Stats->SetNumberField(TEXT("nodes"), GetCount(TEXT("nodes")));
	Stats->SetNumberField(TEXT("connections"), GetCount(TEXT("connections")));
	Stats->SetNumberField(TEXT("variables"), GetCount(TEXT("variables")));
	Stats->SetNumberField(TEXT("parameters"), GetCount(TEXT("parameters")));
	Stats->SetNumberField(TEXT("dependencies"), GetCount(TEXT("dependencies")));
	Stats->SetNumberField(TEXT("actors"), GetCount(TEXT("actors")));
	Stats->SetNumberField(TEXT("tags"), GetCount(TEXT("tags")));
	Stats->SetNumberField(TEXT("configs"), GetCount(TEXT("configs")));
	Stats->SetNumberField(TEXT("cpp_symbols"), GetCount(TEXT("cpp_symbols")));
	Stats->SetNumberField(TEXT("datatable_rows"), GetCount(TEXT("datatable_rows")));
	Stats->SetNumberField(TEXT("asset_search_values"), GetCount(TEXT("asset_search_values")));
	Stats->SetNumberField(TEXT("tag_references"), GetCount(TEXT("tag_references")));
	Stats->SetNumberField(TEXT("meta"), GetCount(TEXT("meta")));

	// Asset class breakdown
	auto ClassBreakdown = MakeShared<FJsonObject>();
	FSQLitePreparedStatement Stmt;
	Stmt.Create(*Database, TEXT("SELECT asset_class, COUNT(*) as cnt FROM assets GROUP BY asset_class ORDER BY cnt DESC LIMIT 20;"));
	while (Stmt.Step() == ESQLitePreparedStatementStepResult::Row)
	{
		FString ClassName;
		int64 Count = 0;
		Stmt.GetColumnValueByIndex(0, ClassName);
		Stmt.GetColumnValueByIndex(1, Count);
		ClassBreakdown->SetNumberField(ClassName, Count);
	}
	Stats->SetObjectField(TEXT("asset_class_breakdown"), ClassBreakdown);

	// Module breakdown (which plugins have how many assets)
	auto ModuleBreakdown = MakeShared<FJsonObject>();
	FSQLitePreparedStatement ModStmt;
	ModStmt.Create(*Database, TEXT("SELECT CASE WHEN module_name = '' THEN 'Project' ELSE module_name END as mod, COUNT(*) as cnt FROM assets GROUP BY mod ORDER BY cnt DESC;"));
	while (ModStmt.Step() == ESQLitePreparedStatementStepResult::Row)
	{
		FString ModName;
		int64 Count = 0;
		ModStmt.GetColumnValueByIndex(0, ModName);
		ModStmt.GetColumnValueByIndex(1, Count);
		ModuleBreakdown->SetNumberField(ModName, Count);
	}
	Stats->SetObjectField(TEXT("module_breakdown"), ModuleBreakdown);

	return Stats;
}

// ============================================================
// Asset details
// ============================================================

TSharedPtr<FJsonObject> FMonolithIndexDatabase::GetAssetDetails(const FString& PackagePath)
{
	auto Details = MakeShared<FJsonObject>();
	if (!IsOpen()) return Details;

	auto MaybeAsset = GetAssetByPath(PackagePath);
	if (!MaybeAsset.IsSet()) return Details;

	const FIndexedAsset& Asset = MaybeAsset.GetValue();
	Details->SetStringField(TEXT("package_path"), Asset.PackagePath);
	Details->SetStringField(TEXT("asset_name"), Asset.AssetName);
	Details->SetStringField(TEXT("asset_class"), Asset.AssetClass);
	Details->SetStringField(TEXT("module_name"), Asset.ModuleName);
	Details->SetStringField(TEXT("description"), Asset.Description);
	Details->SetNumberField(TEXT("file_size_bytes"), Asset.FileSizeBytes);
	Details->SetStringField(TEXT("last_modified"), Asset.LastModified);
	Details->SetStringField(TEXT("indexed_at"), Asset.IndexedAt);

	// Nodes
	TArray<FIndexedNode> Nodes = GetNodesForAsset(Asset.Id);
	TArray<TSharedPtr<FJsonValue>> NodesArr;
	NodesArr.Reserve(Nodes.Num());
	for (const auto& Node : Nodes)
	{
		auto NodeObj = MakeShared<FJsonObject>();
		NodeObj->SetStringField(TEXT("node_type"), Node.NodeType);
		NodeObj->SetStringField(TEXT("node_name"), Node.NodeName);
		NodeObj->SetStringField(TEXT("node_class"), Node.NodeClass);

		// Include stored properties (type-specific metadata from indexers)
		if (!Node.Properties.IsEmpty() && Node.Properties != TEXT("{}"))
		{
			TSharedPtr<FJsonObject> PropsObj;
			auto Reader = TJsonReaderFactory<>::Create(Node.Properties);
			if (FJsonSerializer::Deserialize(Reader, PropsObj) && PropsObj.IsValid() && PropsObj->Values.Num() > 0)
			{
				NodeObj->SetObjectField(TEXT("properties"), PropsObj);
			}
		}

		NodesArr.Add(MakeShared<FJsonValueObject>(NodeObj));
	}
	Details->SetArrayField(TEXT("nodes"), NodesArr);

	// Variables
	TArray<FIndexedVariable> Vars = GetVariablesForAsset(Asset.Id);
	TArray<TSharedPtr<FJsonValue>> VarsArr;
	VarsArr.Reserve(Vars.Num());
	for (const auto& Var : Vars)
	{
		auto VarObj = MakeShared<FJsonObject>();
		VarObj->SetStringField(TEXT("name"), Var.VarName);
		VarObj->SetStringField(TEXT("type"), Var.VarType);
		VarObj->SetStringField(TEXT("category"), Var.Category);
		VarObj->SetBoolField(TEXT("exposed"), Var.bIsExposed);
		VarsArr.Add(MakeShared<FJsonValueObject>(VarObj));
	}
	Details->SetArrayField(TEXT("variables"), VarsArr);

	// Dependencies — wrap in safety check to prevent invalid JSON propagation
	auto Refs = FindReferences(PackagePath);
	if (Refs.IsValid())
	{
		// Validate the references object can serialize cleanly
		FString SerializedRefs;
		auto Writer = TJsonWriterFactory<>::Create(&SerializedRefs);
		if (FJsonSerializer::Serialize(Refs.ToSharedRef(), Writer))
		{
			Details->SetObjectField(TEXT("references"), Refs);
		}
		else
		{
			// Fallback: provide empty references rather than invalid JSON
			auto EmptyRefs = MakeShared<FJsonObject>();
			EmptyRefs->SetArrayField(TEXT("depends_on"), TArray<TSharedPtr<FJsonValue>>());
			EmptyRefs->SetArrayField(TEXT("referenced_by"), TArray<TSharedPtr<FJsonValue>>());
			Details->SetObjectField(TEXT("references"), EmptyRefs);
		}
	}

	return Details;
}

// ============================================================
// Find by type
// ============================================================

TArray<FIndexedAsset> FMonolithIndexDatabase::FindByType(const FString& AssetClass, int32 Limit, int32 Offset)
{
	return FindByType(AssetClass, FString(), Limit, Offset);
}

TArray<FIndexedAsset> FMonolithIndexDatabase::FindByType(
	const FString& AssetClass,
	const FString& ModuleFilter,
	int32 Limit,
	int32 Offset)
{
	TArray<FIndexedAsset> Result;
	if (!IsOpen()) return Result;

	FSQLitePreparedStatement Stmt;
	const bool bFilterModule = !ModuleFilter.IsEmpty();
	const TCHAR* Sql = bFilterModule
		? TEXT("SELECT id, package_path, asset_name, asset_class, module_name, description, file_size_bytes, last_modified, saved_hash, indexed_at FROM assets WHERE asset_class = ? AND module_name = ? LIMIT ? OFFSET ?;")
		: TEXT("SELECT id, package_path, asset_name, asset_class, module_name, description, file_size_bytes, last_modified, saved_hash, indexed_at FROM assets WHERE asset_class = ? LIMIT ? OFFSET ?;");
	Stmt.Create(*Database, Sql);
	Stmt.SetBindingValueByIndex(1, AssetClass);
	int32 BindIndex = 2;
	if (bFilterModule)
	{
		Stmt.SetBindingValueByIndex(BindIndex++, ModuleFilter);
	}
	Stmt.SetBindingValueByIndex(BindIndex++, static_cast<int64>(Limit));
	Stmt.SetBindingValueByIndex(BindIndex, static_cast<int64>(Offset));

	while (Stmt.Step() == ESQLitePreparedStatementStepResult::Row)
	{
		FIndexedAsset Asset;
		Stmt.GetColumnValueByIndex(0, Asset.Id);
		Stmt.GetColumnValueByIndex(1, Asset.PackagePath);
		Stmt.GetColumnValueByIndex(2, Asset.AssetName);
		Stmt.GetColumnValueByIndex(3, Asset.AssetClass);
		Stmt.GetColumnValueByIndex(4, Asset.ModuleName);
		Stmt.GetColumnValueByIndex(5, Asset.Description);
		Stmt.GetColumnValueByIndex(6, Asset.FileSizeBytes);
		Stmt.GetColumnValueByIndex(7, Asset.LastModified);
		Stmt.GetColumnValueByIndex(8, Asset.SavedHash);
		Stmt.GetColumnValueByIndex(9, Asset.IndexedAt);
		Result.Add(MoveTemp(Asset));
	}
	return Result;
}

// ============================================================
// Find references (bidirectional dependency lookup)
// ============================================================

TSharedPtr<FJsonObject> FMonolithIndexDatabase::FindReferences(const FString& PackagePath)
{
	auto Result = MakeShared<FJsonObject>();
	if (!IsOpen()) return Result;

	int64 AssetId = GetAssetId(PackagePath);
	if (AssetId < 0) return Result;

	// What this asset depends on
	TArray<FIndexedDependency> Deps = GetDependenciesForAsset(AssetId);
	TArray<TSharedPtr<FJsonValue>> DepsArr;
	DepsArr.Reserve(Deps.Num());

	FSQLitePreparedStatement DepStmt;
	DepStmt.Create(*Database, TEXT("SELECT package_path, asset_class FROM assets WHERE id = ?;"));

	for (const auto& Dep : Deps)
	{
		DepStmt.Reset();
		DepStmt.SetBindingValueByIndex(1, Dep.TargetAssetId);
		if (DepStmt.Step() == ESQLitePreparedStatementStepResult::Row)
		{
			auto DepObj = MakeShared<FJsonObject>();
			FString Path, Class;
			DepStmt.GetColumnValueByIndex(0, Path);
			DepStmt.GetColumnValueByIndex(1, Class);
			DepObj->SetStringField(TEXT("path"), Path);
			DepObj->SetStringField(TEXT("class"), Class);
			DepObj->SetStringField(TEXT("type"), Dep.DependencyType);
			DepsArr.Add(MakeShared<FJsonValueObject>(DepObj));
		}
	}
	Result->SetArrayField(TEXT("depends_on"), DepsArr);

	// What references this asset
	TArray<FIndexedDependency> Refs = GetReferencersOfAsset(AssetId);
	TArray<TSharedPtr<FJsonValue>> RefsArr;
	RefsArr.Reserve(Refs.Num());

	FSQLitePreparedStatement RefStmt;
	RefStmt.Create(*Database, TEXT("SELECT package_path, asset_class FROM assets WHERE id = ?;"));

	for (const auto& Ref : Refs)
	{
		RefStmt.Reset();
		RefStmt.SetBindingValueByIndex(1, Ref.SourceAssetId);
		if (RefStmt.Step() == ESQLitePreparedStatementStepResult::Row)
		{
			auto RefObj = MakeShared<FJsonObject>();
			FString Path, Class;
			RefStmt.GetColumnValueByIndex(0, Path);
			RefStmt.GetColumnValueByIndex(1, Class);
			RefObj->SetStringField(TEXT("path"), Path);
			RefObj->SetStringField(TEXT("class"), Class);
			RefObj->SetStringField(TEXT("type"), Ref.DependencyType);
			RefsArr.Add(MakeShared<FJsonValueObject>(RefObj));
		}
	}
	Result->SetArrayField(TEXT("referenced_by"), RefsArr);

	return Result;
}

// ============================================================
// Internal helpers
// ============================================================

bool FMonolithIndexDatabase::CreateTables()
{
	if (!Database || !Database->IsValid())
	{
		return false;
	}

	// GCreateTablesSQL contains multiple statements separated by semicolons.
	// FSQLiteDatabase::Execute() only handles one statement at a time,
	// so we split and execute each individually.
	TArray<FString> Statements;

	auto AppendStatements = [&Statements](const TCHAR* SqlText)
	{
		FString FullSQL(SqlText);
		// Split on semicolons, tracking BEGIN/END depth for trigger bodies
		int32 Start = 0;
		int32 Depth = 0;
		for (int32 i = 0; i < FullSQL.Len(); ++i)
		{
			// Check for BEGIN keyword (trigger body start)
			if (i + 5 <= FullSQL.Len())
			{
				FString Word = FullSQL.Mid(i, 5).ToUpper();
				if (Word == TEXT("BEGIN") && (i == 0 || FChar::IsWhitespace(FullSQL[i - 1]) || FullSQL[i - 1] == '\n'))
				{
					if (i + 5 >= FullSQL.Len() || FChar::IsWhitespace(FullSQL[i + 5]) || FullSQL[i + 5] == '\n')
					{
						Depth++;
					}
				}
			}
			// Check for END keyword (trigger body end)
			if (i + 3 <= FullSQL.Len())
			{
				FString Word = FullSQL.Mid(i, 3).ToUpper();
				if (Word == TEXT("END") && (i == 0 || FChar::IsWhitespace(FullSQL[i - 1]) || FullSQL[i - 1] == '\n'))
				{
					if (i + 3 >= FullSQL.Len() || FullSQL[i + 3] == ';' || FChar::IsWhitespace(FullSQL[i + 3]))
					{
						if (Depth > 0) Depth--;
					}
				}
			}

			if (FullSQL[i] == ';' && Depth == 0)
			{
				FString Stmt = FullSQL.Mid(Start, i - Start + 1).TrimStartAndEnd();
				if (!Stmt.IsEmpty() && Stmt != TEXT(";"))
				{
					Statements.Add(Stmt);
				}
				Start = i + 1;
			}
		}
	};
	AppendStatements(GCreateTablesSQL);
	AppendStatements(GCreateSearchValueTablesSQL);

	bool bAllSucceeded = true;
	for (const FString& Stmt : Statements)
	{
		if (!Database->Execute(*Stmt))
		{
			UE_LOG(LogMonolithIndex, Warning, TEXT("Schema statement failed: %s -- Error: %s"),
				*Stmt.Left(100), *Database->GetLastError());
			bAllSucceeded = false;
			// Don't stop -- try remaining statements (some may be IF NOT EXISTS)
		}
	}

	if (!bAllSucceeded)
	{
		UE_LOG(LogMonolithIndex, Warning, TEXT("Some schema statements failed -- FTS5 may not be available in this SQLite build"));
	}

	return true; // Return true even if FTS fails -- basic tables should work
}

bool FMonolithIndexDatabase::ExecuteSQL(const FString& SQL)
{
	if (!Database || !Database->IsValid())
	{
		UE_LOG(LogMonolithIndex, Error, TEXT("Cannot execute SQL -- database not open"));
		return false;
	}

	if (!Database->Execute(*SQL))
	{
		UE_LOG(LogMonolithIndex, Error, TEXT("SQL execution failed: %s"), *Database->GetLastError());
		return false;
	}
	return true;
}
