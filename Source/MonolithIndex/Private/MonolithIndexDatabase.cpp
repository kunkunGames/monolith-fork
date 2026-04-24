#include "MonolithIndexDatabase.h"
#include "MonolithSQLiteExec.h"
#include "MonolithSQLiteMaintenance.h"
#include "MonolithSQLitePragmaPolicy.h"
#include "MonolithSQLiteSearchText.h"
#include "SQLiteDatabase.h"
#include "Serialization/JsonWriter.h"
#include "Serialization/JsonSerializer.h"

DEFINE_LOG_CATEGORY(LogMonolithIndex);

// ============================================================
// Full table creation SQL
// ============================================================
static constexpr int32 GIndexSchemaVersion = 4;

static const TCHAR* GCreateCoreTablesSQL = TEXT(R"SQL(

-- Core asset table: every indexed asset
CREATE TABLE IF NOT EXISTS assets (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    package_path TEXT NOT NULL UNIQUE,
    asset_name TEXT NOT NULL,
    asset_class TEXT NOT NULL,
    module_name TEXT DEFAULT '',
    description TEXT DEFAULT '',
    search_tokens TEXT DEFAULT '',
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
    search_tokens TEXT DEFAULT '',
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

)SQL");

static const TCHAR* GCreateFtsTablesSQL = TEXT(R"SQL(

-- FTS5 index over assets (name, class, description, path, module)
CREATE VIRTUAL TABLE IF NOT EXISTS fts_assets USING fts5(
    asset_name,
    asset_class,
    description,
    search_tokens,
    package_path,
    module_name,
    content=assets,
    content_rowid=id,
    tokenize='unicode61 remove_diacritics 2',
    prefix='2 3 4'
);

-- FTS5 triggers to keep index in sync
CREATE TRIGGER IF NOT EXISTS fts_assets_ai AFTER INSERT ON assets BEGIN
    INSERT INTO fts_assets(rowid, asset_name, asset_class, description, search_tokens, package_path, module_name)
    VALUES (new.id, new.asset_name, new.asset_class, new.description, new.search_tokens, new.package_path, new.module_name);
END;
CREATE TRIGGER IF NOT EXISTS fts_assets_ad AFTER DELETE ON assets BEGIN
    INSERT INTO fts_assets(fts_assets, rowid, asset_name, asset_class, description, search_tokens, package_path, module_name)
    VALUES ('delete', old.id, old.asset_name, old.asset_class, old.description, old.search_tokens, old.package_path, old.module_name);
END;
CREATE TRIGGER IF NOT EXISTS fts_assets_au AFTER UPDATE ON assets BEGIN
    INSERT INTO fts_assets(fts_assets, rowid, asset_name, asset_class, description, search_tokens, package_path, module_name)
    VALUES ('delete', old.id, old.asset_name, old.asset_class, old.description, old.search_tokens, old.package_path, old.module_name);
    INSERT INTO fts_assets(rowid, asset_name, asset_class, description, search_tokens, package_path, module_name)
    VALUES (new.id, new.asset_name, new.asset_class, new.description, new.search_tokens, new.package_path, new.module_name);
END;

-- FTS5 index over nodes (name, class, type)
CREATE VIRTUAL TABLE IF NOT EXISTS fts_nodes USING fts5(
    node_name,
    node_class,
    node_type,
    search_tokens,
    content=nodes,
    content_rowid=id,
    tokenize='unicode61 remove_diacritics 2',
    prefix='2 3 4'
);

CREATE TRIGGER IF NOT EXISTS fts_nodes_ai AFTER INSERT ON nodes BEGIN
    INSERT INTO fts_nodes(rowid, node_name, node_class, node_type, search_tokens)
    VALUES (new.id, new.node_name, new.node_class, new.node_type, new.search_tokens);
END;
CREATE TRIGGER IF NOT EXISTS fts_nodes_ad AFTER DELETE ON nodes BEGIN
    INSERT INTO fts_nodes(fts_nodes, rowid, node_name, node_class, node_type, search_tokens)
    VALUES ('delete', old.id, old.node_name, old.node_class, old.node_type, old.search_tokens);
END;
CREATE TRIGGER IF NOT EXISTS fts_nodes_au AFTER UPDATE ON nodes BEGIN
    INSERT INTO fts_nodes(fts_nodes, rowid, node_name, node_class, node_type, search_tokens)
    VALUES ('delete', old.id, old.node_name, old.node_class, old.node_type, old.search_tokens);
    INSERT INTO fts_nodes(rowid, node_name, node_class, node_type, search_tokens)
    VALUES (new.id, new.node_name, new.node_class, new.node_type, new.search_tokens);
END;

-- Metadata table for tracking index state
CREATE TABLE IF NOT EXISTS meta (
    key TEXT PRIMARY KEY,
    value TEXT DEFAULT ''
);

)SQL");

static bool MonolithIndexTableHasColumn(FSQLiteDatabase& Database, const TCHAR* TableName, const TCHAR* ColumnName)
{
	FSQLitePreparedStatement Stmt;
	const FString SQL = FString::Printf(TEXT("PRAGMA table_info(%s);"), TableName);
	if (!Stmt.Create(Database, *SQL, ESQLitePreparedStatementFlags::Persistent))
	{
		return false;
	}

	while (Stmt.Step() == ESQLitePreparedStatementStepResult::Row)
	{
		FString ExistingColumn;
		Stmt.GetColumnValueByIndex(1, ExistingColumn);
		if (ExistingColumn == ColumnName)
		{
			return true;
		}
	}
	return false;
}

static FString BuildIndexAssetSearchTokens(const FString& PackagePath, const FString& AssetName, const FString& AssetClass)
{
	return BuildMonolithSQLiteSearchText(FString::Printf(TEXT("%s %s %s"), *PackagePath, *AssetName, *AssetClass));
}

static FString BuildIndexNodeSearchTokens(const FString& NodeName, const FString& NodeClass, const FString& NodeType)
{
	return BuildMonolithSQLiteSearchText(FString::Printf(TEXT("%s %s %s"), *NodeName, *NodeClass, *NodeType));
}

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
	return OpenForWrite(InDbPath);
}

bool FMonolithIndexDatabase::OpenForQuery(const FString& InDbPath)
{
	if (Database)
	{
		Close();
	}

	DbPath = InDbPath;

	Database = new FSQLiteDatabase();
	FMonolithSQLiteOpenPolicy Policy;
	Policy.Intent = EMonolithSQLiteIntent::QueryOnly;
	Policy.Role = EMonolithSQLiteConnectionRole::ReadMostly;
	Policy.bEnableForeignKeys = false;
	Policy.bVerifyIntegrity = false;
	bRunOptimizeOnClose = false;
	if (!OpenMonolithSQLiteDatabase(*Database, DbPath, Policy))
	{
		UE_LOG(LogMonolithIndex, Error, TEXT("Failed to open index database for query: %s"), *DbPath);
		delete Database;
		Database = nullptr;
		return false;
	}

	UE_LOG(LogMonolithIndex, Log, TEXT("Index database opened for query: %s"), *DbPath);
	return true;
}

bool FMonolithIndexDatabase::OpenForWrite(const FString& InDbPath)
{
	if (Database)
	{
		Close();
	}

	DbPath = InDbPath;

	Database = new FSQLiteDatabase();
	FMonolithSQLiteOpenPolicy Policy;
	Policy.Intent = EMonolithSQLiteIntent::CreateOrRebuild;
	Policy.Role = EMonolithSQLiteConnectionRole::WriteHeavy;
	Policy.bEnableForeignKeys = true;
	Policy.bVerifyIntegrity = false;
	bRunOptimizeOnClose = true;
	FMonolithSQLiteTuningResult TuningResult;
	if (!OpenMonolithSQLiteDatabase(*Database, DbPath, Policy, &TuningResult))
	{
		UE_LOG(LogMonolithIndex, Error, TEXT("Failed to open index database: %s"), *DbPath);
		delete Database;
		Database = nullptr;
		return false;
	}

	if (!CreateTables())
	{
		UE_LOG(LogMonolithIndex, Error, TEXT("Failed to create index tables"));
		Close();
		return false;
	}

	// Schema migrations: v1/v2 -> v3
	{
		int32 HeaderSchemaVersion = 0;
		Database->GetUserVersion(HeaderSchemaVersion);
		const FString SchemaVersion = ReadMeta(TEXT("schema_version"));
		const int32 MetaSchemaVersion = SchemaVersion.IsEmpty() ? 0 : FCString::Atoi(*SchemaVersion);
		const int32 CurrentSchemaVersion = FMath::Max(HeaderSchemaVersion, MetaSchemaVersion);

		if (CurrentSchemaVersion < 2)
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
		}

		if (!TuningResult.bFreshDatabase && CurrentSchemaVersion < GIndexSchemaVersion)
		{
			if (!MigrateFtsSchemaToV4())
			{
				UE_LOG(LogMonolithIndex, Error, TEXT("Failed to migrate index FTS schema to v%d"), GIndexSchemaVersion);
				Close();
				return false;
			}
		}

		WriteMeta(TEXT("schema_version"), FString::FromInt(GIndexSchemaVersion));
		Database->SetUserVersion(GIndexSchemaVersion);
	}

	// Ensure hash index exists (safe for both fresh and migrated DBs)
	ExecuteSQL(TEXT("CREATE INDEX IF NOT EXISTS idx_assets_hash ON assets(saved_hash);"));

	if (TuningResult.bFreshDatabase)
	{
		FMonolithSQLiteMaintenanceOptions MaintenanceOptions;
		MaintenanceOptions.bRunPragmaOptimize = true;
		MaintenanceOptions.bUseFullOptimizeScan = true;
		RunMonolithSQLiteMaintenance(*Database, MaintenanceOptions);
		Database->PerformQuickIntegrityCheck();
	}

	UE_LOG(LogMonolithIndex, Log, TEXT("Index database opened: %s"), *DbPath);
	return true;
}

void FMonolithIndexDatabase::Close()
{
	if (Database)
	{
		StatementCache.Clear();
		if (bRunOptimizeOnClose && Database->IsValid())
		{
			FMonolithSQLiteMaintenanceOptions MaintenanceOptions;
			MaintenanceOptions.bRunPragmaOptimize = true;
			RunMonolithSQLiteMaintenance(*Database, MaintenanceOptions);
		}
		Database->Close();
		delete Database;
		Database = nullptr;
	}
	bRunOptimizeOnClose = false;
}

bool FMonolithIndexDatabase::IsOpen() const
{
	return Database != nullptr && Database->IsValid();
}

bool FMonolithIndexDatabase::ResetDatabase()
{
	if (!IsOpen()) return false;

	StatementCache.Clear();

	// Drop all tables and recreate — order matters for foreign keys
	ExecuteSQL(TEXT("DROP TRIGGER IF EXISTS fts_assets_ai;"));
	ExecuteSQL(TEXT("DROP TRIGGER IF EXISTS fts_assets_ad;"));
	ExecuteSQL(TEXT("DROP TRIGGER IF EXISTS fts_assets_au;"));
	ExecuteSQL(TEXT("DROP TRIGGER IF EXISTS fts_nodes_ai;"));
	ExecuteSQL(TEXT("DROP TRIGGER IF EXISTS fts_nodes_ad;"));
	ExecuteSQL(TEXT("DROP TRIGGER IF EXISTS fts_nodes_au;"));
	ExecuteSQL(TEXT("DROP TABLE IF EXISTS fts_assets;"));
	ExecuteSQL(TEXT("DROP TABLE IF EXISTS fts_nodes;"));
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
	ExecuteSQL(TEXT("DROP TABLE IF EXISTS meta;"));
	ExecuteSQL(TEXT("DROP TABLE IF EXISTS assets;"));

	const bool bCreated = CreateTables();
	if (bCreated)
	{
		WriteMeta(TEXT("schema_version"), FString::FromInt(GIndexSchemaVersion));
		Database->SetUserVersion(GIndexSchemaVersion);

		FMonolithSQLiteMaintenanceOptions MaintenanceOptions;
		MaintenanceOptions.bRunPragmaOptimize = true;
		MaintenanceOptions.bUseFullOptimizeScan = true;
		MaintenanceOptions.bRunIncrementalVacuum = true;
		RunMonolithSQLiteMaintenance(*Database, MaintenanceOptions);
		Database->PerformQuickIntegrityCheck();
	}
	return bCreated;
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

	const FString SearchTokens = BuildIndexAssetSearchTokens(Asset.PackagePath, Asset.AssetName, Asset.AssetClass);
	FSQLitePreparedStatement* Stmt = StatementCache.FindOrCreate(*Database, TEXT("InsertAsset"), TEXT("INSERT INTO assets (package_path, asset_name, asset_class, module_name, description, search_tokens, file_size_bytes, last_modified, saved_hash) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?);"));
	if (!Stmt) return -1;
	Stmt->SetBindingValueByIndex(1, Asset.PackagePath);
	Stmt->SetBindingValueByIndex(2, Asset.AssetName);
	Stmt->SetBindingValueByIndex(3, Asset.AssetClass);
	Stmt->SetBindingValueByIndex(4, Asset.ModuleName);
	Stmt->SetBindingValueByIndex(5, Asset.Description);
	Stmt->SetBindingValueByIndex(6, SearchTokens);
	Stmt->SetBindingValueByIndex(7, Asset.FileSizeBytes);
	Stmt->SetBindingValueByIndex(8, Asset.LastModified);
	Stmt->SetBindingValueByIndex(9, Asset.SavedHash);

	if (!Stmt->Execute()) return -1;
	return Database->GetLastInsertRowId();
}

TOptional<FIndexedAsset> FMonolithIndexDatabase::GetAssetByPath(const FString& PackagePath)
{
	if (!IsOpen()) return {};

	FSQLitePreparedStatement* Stmt = StatementCache.FindOrCreate(*Database, TEXT("GetAssetByPath"), TEXT("SELECT id, package_path, asset_name, asset_class, module_name, description, file_size_bytes, last_modified, saved_hash, indexed_at FROM assets WHERE package_path = ?;"));
	if (!Stmt) return {};
	Stmt->SetBindingValueByIndex(1, PackagePath);

	if (Stmt->Step() == ESQLitePreparedStatementStepResult::Row)
	{
		FIndexedAsset Asset;
		Stmt->GetColumnValueByIndex(0, Asset.Id);
		Stmt->GetColumnValueByIndex(1, Asset.PackagePath);
		Stmt->GetColumnValueByIndex(2, Asset.AssetName);
		Stmt->GetColumnValueByIndex(3, Asset.AssetClass);
		Stmt->GetColumnValueByIndex(4, Asset.ModuleName);
		Stmt->GetColumnValueByIndex(5, Asset.Description);
		Stmt->GetColumnValueByIndex(6, Asset.FileSizeBytes);
		Stmt->GetColumnValueByIndex(7, Asset.LastModified);
		Stmt->GetColumnValueByIndex(8, Asset.SavedHash);
		Stmt->GetColumnValueByIndex(9, Asset.IndexedAt);
		return Asset;
	}
	return {};
}

int64 FMonolithIndexDatabase::GetAssetId(const FString& PackagePath)
{
	if (!IsOpen()) return -1;

	FSQLitePreparedStatement* Stmt = StatementCache.FindOrCreate(*Database, TEXT("GetAssetId"), TEXT("SELECT id FROM assets WHERE package_path = ?;"));
	if (!Stmt) return -1;
	Stmt->SetBindingValueByIndex(1, PackagePath);

	if (Stmt->Step() == ESQLitePreparedStatementStepResult::Row)
	{
		int64 Id = 0;
		Stmt->GetColumnValueByIndex(0, Id);
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

	const FString SearchTokens = BuildIndexNodeSearchTokens(Node.NodeName, Node.NodeClass, Node.NodeType);
	FSQLitePreparedStatement* Stmt = StatementCache.FindOrCreate(*Database, TEXT("InsertNode"), TEXT("INSERT INTO nodes (asset_id, node_type, node_name, node_class, search_tokens, properties, pos_x, pos_y) VALUES (?, ?, ?, ?, ?, ?, ?, ?);"));
	if (!Stmt) return -1;
	Stmt->SetBindingValueByIndex(1, Node.AssetId);
	Stmt->SetBindingValueByIndex(2, Node.NodeType);
	Stmt->SetBindingValueByIndex(3, Node.NodeName);
	Stmt->SetBindingValueByIndex(4, Node.NodeClass);
	Stmt->SetBindingValueByIndex(5, SearchTokens);
	Stmt->SetBindingValueByIndex(6, Node.Properties);
	Stmt->SetBindingValueByIndex(7, static_cast<int64>(Node.PosX));
	Stmt->SetBindingValueByIndex(8, static_cast<int64>(Node.PosY));

	if (!Stmt->Execute()) return -1;
	return Database->GetLastInsertRowId();
}

TArray<FIndexedNode> FMonolithIndexDatabase::GetNodesForAsset(int64 AssetId)
{
	TArray<FIndexedNode> Result;
	if (!IsOpen()) return Result;

	FSQLitePreparedStatement* Stmt = StatementCache.FindOrCreate(*Database, TEXT("GetNodesForAsset"), TEXT("SELECT id, asset_id, node_type, node_name, node_class, properties, pos_x, pos_y FROM nodes WHERE asset_id = ?;"));
	if (!Stmt) return Result;
	Stmt->SetBindingValueByIndex(1, AssetId);

	while (Stmt->Step() == ESQLitePreparedStatementStepResult::Row)
	{
		FIndexedNode Node;
		Stmt->GetColumnValueByIndex(0, Node.Id);
		Stmt->GetColumnValueByIndex(1, Node.AssetId);
		Stmt->GetColumnValueByIndex(2, Node.NodeType);
		Stmt->GetColumnValueByIndex(3, Node.NodeName);
		Stmt->GetColumnValueByIndex(4, Node.NodeClass);
		Stmt->GetColumnValueByIndex(5, Node.Properties);
		Stmt->GetColumnValueByIndex(6, Node.PosX);
		Stmt->GetColumnValueByIndex(7, Node.PosY);
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

	FSQLitePreparedStatement* Stmt = StatementCache.FindOrCreate(*Database, TEXT("InsertConnection"), TEXT("INSERT INTO connections (source_node_id, source_pin, target_node_id, target_pin, pin_type) VALUES (?, ?, ?, ?, ?);"));
	if (!Stmt) return -1;
	Stmt->SetBindingValueByIndex(1, Conn.SourceNodeId);
	Stmt->SetBindingValueByIndex(2, Conn.SourcePin);
	Stmt->SetBindingValueByIndex(3, Conn.TargetNodeId);
	Stmt->SetBindingValueByIndex(4, Conn.TargetPin);
	Stmt->SetBindingValueByIndex(5, Conn.PinType);

	if (!Stmt->Execute()) return -1;
	return Database->GetLastInsertRowId();
}

TArray<FIndexedConnection> FMonolithIndexDatabase::GetConnectionsForAsset(int64 AssetId)
{
	TArray<FIndexedConnection> Result;
	if (!IsOpen()) return Result;

	FSQLitePreparedStatement* Stmt = StatementCache.FindOrCreate(*Database, TEXT("GetConnectionsForAsset"), TEXT("SELECT c.id, c.source_node_id, c.source_pin, c.target_node_id, c.target_pin, c.pin_type FROM connections c INNER JOIN nodes n ON c.source_node_id = n.id WHERE n.asset_id = ?;"));
	if (!Stmt) return Result;
	Stmt->SetBindingValueByIndex(1, AssetId);

	while (Stmt->Step() == ESQLitePreparedStatementStepResult::Row)
	{
		FIndexedConnection Conn;
		Stmt->GetColumnValueByIndex(0, Conn.Id);
		Stmt->GetColumnValueByIndex(1, Conn.SourceNodeId);
		Stmt->GetColumnValueByIndex(2, Conn.SourcePin);
		Stmt->GetColumnValueByIndex(3, Conn.TargetNodeId);
		Stmt->GetColumnValueByIndex(4, Conn.TargetPin);
		Stmt->GetColumnValueByIndex(5, Conn.PinType);
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

	FSQLitePreparedStatement* Stmt = StatementCache.FindOrCreate(*Database, TEXT("InsertVariable"), TEXT("INSERT INTO variables (asset_id, var_name, var_type, category, default_value, is_exposed, is_replicated) VALUES (?, ?, ?, ?, ?, ?, ?);"));
	if (!Stmt) return -1;
	Stmt->SetBindingValueByIndex(1, Var.AssetId);
	Stmt->SetBindingValueByIndex(2, Var.VarName);
	Stmt->SetBindingValueByIndex(3, Var.VarType);
	Stmt->SetBindingValueByIndex(4, Var.Category);
	Stmt->SetBindingValueByIndex(5, Var.DefaultValue);
	Stmt->SetBindingValueByIndex(6, static_cast<int64>(Var.bIsExposed ? 1 : 0));
	Stmt->SetBindingValueByIndex(7, static_cast<int64>(Var.bIsReplicated ? 1 : 0));

	if (!Stmt->Execute()) return -1;
	return Database->GetLastInsertRowId();
}

TArray<FIndexedVariable> FMonolithIndexDatabase::GetVariablesForAsset(int64 AssetId)
{
	TArray<FIndexedVariable> Result;
	if (!IsOpen()) return Result;

	FSQLitePreparedStatement* Stmt = StatementCache.FindOrCreate(*Database, TEXT("GetVariablesForAsset"), TEXT("SELECT id, asset_id, var_name, var_type, category, default_value, is_exposed, is_replicated FROM variables WHERE asset_id = ?;"));
	if (!Stmt) return Result;
	Stmt->SetBindingValueByIndex(1, AssetId);

	while (Stmt->Step() == ESQLitePreparedStatementStepResult::Row)
	{
		FIndexedVariable Var;
		Stmt->GetColumnValueByIndex(0, Var.Id);
		Stmt->GetColumnValueByIndex(1, Var.AssetId);
		Stmt->GetColumnValueByIndex(2, Var.VarName);
		Stmt->GetColumnValueByIndex(3, Var.VarType);
		Stmt->GetColumnValueByIndex(4, Var.Category);
		Stmt->GetColumnValueByIndex(5, Var.DefaultValue);
		int32 Exposed = 0, Replicated = 0;
		Stmt->GetColumnValueByIndex(6, Exposed);
		Stmt->GetColumnValueByIndex(7, Replicated);
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

	FSQLitePreparedStatement* Stmt = StatementCache.FindOrCreate(*Database, TEXT("InsertParameter"), TEXT("INSERT INTO parameters (asset_id, param_name, param_type, param_group, default_value, source) VALUES (?, ?, ?, ?, ?, ?);"));
	if (!Stmt) return -1;
	Stmt->SetBindingValueByIndex(1, Param.AssetId);
	Stmt->SetBindingValueByIndex(2, Param.ParamName);
	Stmt->SetBindingValueByIndex(3, Param.ParamType);
	Stmt->SetBindingValueByIndex(4, Param.ParamGroup);
	Stmt->SetBindingValueByIndex(5, Param.DefaultValue);
	Stmt->SetBindingValueByIndex(6, Param.Source);

	if (!Stmt->Execute()) return -1;
	return Database->GetLastInsertRowId();
}

// ============================================================
// Dependency CRUD
// ============================================================

int64 FMonolithIndexDatabase::InsertDependency(const FIndexedDependency& Dep)
{
	if (!IsOpen()) return -1;

	FSQLitePreparedStatement* Stmt = StatementCache.FindOrCreate(*Database, TEXT("InsertDependency"), TEXT("INSERT INTO dependencies (source_asset_id, target_asset_id, dependency_type) VALUES (?, ?, ?);"));
	if (!Stmt) return -1;
	Stmt->SetBindingValueByIndex(1, Dep.SourceAssetId);
	Stmt->SetBindingValueByIndex(2, Dep.TargetAssetId);
	Stmt->SetBindingValueByIndex(3, Dep.DependencyType);

	if (!Stmt->Execute()) return -1;
	return Database->GetLastInsertRowId();
}

TArray<FIndexedDependency> FMonolithIndexDatabase::GetDependenciesForAsset(int64 AssetId)
{
	TArray<FIndexedDependency> Result;
	if (!IsOpen()) return Result;

	FSQLitePreparedStatement* Stmt = StatementCache.FindOrCreate(*Database, TEXT("GetDependenciesForAsset"), TEXT("SELECT id, source_asset_id, target_asset_id, dependency_type FROM dependencies WHERE source_asset_id = ?;"));
	if (!Stmt) return Result;
	Stmt->SetBindingValueByIndex(1, AssetId);

	while (Stmt->Step() == ESQLitePreparedStatementStepResult::Row)
	{
		FIndexedDependency Dep;
		Stmt->GetColumnValueByIndex(0, Dep.Id);
		Stmt->GetColumnValueByIndex(1, Dep.SourceAssetId);
		Stmt->GetColumnValueByIndex(2, Dep.TargetAssetId);
		Stmt->GetColumnValueByIndex(3, Dep.DependencyType);
		Result.Add(MoveTemp(Dep));
	}
	return Result;
}

TArray<FIndexedDependency> FMonolithIndexDatabase::GetReferencersOfAsset(int64 AssetId)
{
	TArray<FIndexedDependency> Result;
	if (!IsOpen()) return Result;

	FSQLitePreparedStatement* Stmt = StatementCache.FindOrCreate(*Database, TEXT("GetReferencersOfAsset"), TEXT("SELECT id, source_asset_id, target_asset_id, dependency_type FROM dependencies WHERE target_asset_id = ?;"));
	if (!Stmt) return Result;
	Stmt->SetBindingValueByIndex(1, AssetId);

	while (Stmt->Step() == ESQLitePreparedStatementStepResult::Row)
	{
		FIndexedDependency Dep;
		Stmt->GetColumnValueByIndex(0, Dep.Id);
		Stmt->GetColumnValueByIndex(1, Dep.SourceAssetId);
		Stmt->GetColumnValueByIndex(2, Dep.TargetAssetId);
		Stmt->GetColumnValueByIndex(3, Dep.DependencyType);
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

	FSQLitePreparedStatement* Stmt = StatementCache.FindOrCreate(*Database, TEXT("InsertActor"), TEXT("INSERT INTO actors (asset_id, actor_name, actor_class, actor_label, transform, components) VALUES (?, ?, ?, ?, ?, ?);"));
	if (!Stmt) return -1;
	Stmt->SetBindingValueByIndex(1, Actor.AssetId);
	Stmt->SetBindingValueByIndex(2, Actor.ActorName);
	Stmt->SetBindingValueByIndex(3, Actor.ActorClass);
	Stmt->SetBindingValueByIndex(4, Actor.ActorLabel);
	Stmt->SetBindingValueByIndex(5, Actor.Transform);
	Stmt->SetBindingValueByIndex(6, Actor.Components);

	if (!Stmt->Execute()) return -1;
	return Database->GetLastInsertRowId();
}

// ============================================================
// Tag CRUD
// ============================================================

int64 FMonolithIndexDatabase::InsertTag(const FIndexedTag& Tag)
{
	if (!IsOpen()) return -1;

	FSQLitePreparedStatement* Stmt = StatementCache.FindOrCreate(*Database, TEXT("InsertTag"), TEXT("INSERT OR IGNORE INTO tags (tag_name, parent_tag, reference_count) VALUES (?, ?, ?);"));
	if (!Stmt) return -1;
	Stmt->SetBindingValueByIndex(1, Tag.TagName);
	Stmt->SetBindingValueByIndex(2, Tag.ParentTag);
	Stmt->SetBindingValueByIndex(3, static_cast<int64>(Tag.ReferenceCount));

	if (!Stmt->Execute()) return -1;
	return GetOrCreateTag(Tag.TagName, Tag.ParentTag);
}

int64 FMonolithIndexDatabase::GetOrCreateTag(const FString& TagName, const FString& ParentTag)
{
	if (!IsOpen()) return -1;

	// Try to get existing
	FSQLitePreparedStatement* Stmt = StatementCache.FindOrCreate(*Database, TEXT("GetTagByName"), TEXT("SELECT id FROM tags WHERE tag_name = ?;"));
	if (!Stmt) return -1;
	Stmt->SetBindingValueByIndex(1, TagName);

	if (Stmt->Step() == ESQLitePreparedStatementStepResult::Row)
	{
		int64 Id = 0;
		Stmt->GetColumnValueByIndex(0, Id);
		return Id;
	}

	// Insert new
	FSQLitePreparedStatement* InsertStmt = StatementCache.FindOrCreate(*Database, TEXT("InsertTagName"), TEXT("INSERT INTO tags (tag_name, parent_tag) VALUES (?, ?);"));
	if (!InsertStmt) return -1;
	InsertStmt->SetBindingValueByIndex(1, TagName);
	InsertStmt->SetBindingValueByIndex(2, ParentTag);
	InsertStmt->Execute();
	return Database->GetLastInsertRowId();
}

int64 FMonolithIndexDatabase::InsertTagReference(const FIndexedTagReference& Ref)
{
	if (!IsOpen()) return -1;

	FSQLitePreparedStatement* Stmt = StatementCache.FindOrCreate(*Database, TEXT("InsertTagReference"), TEXT("INSERT INTO tag_references (tag_id, asset_id, context) VALUES (?, ?, ?);"));
	if (!Stmt) return -1;
	Stmt->SetBindingValueByIndex(1, Ref.TagId);
	Stmt->SetBindingValueByIndex(2, Ref.AssetId);
	Stmt->SetBindingValueByIndex(3, Ref.Context);

	if (!Stmt->Execute()) return -1;

	// Update reference count
	FSQLitePreparedStatement* UpdateStmt = StatementCache.FindOrCreate(*Database, TEXT("UpdateTagReferenceCount"), TEXT("UPDATE tags SET reference_count = (SELECT COUNT(*) FROM tag_references WHERE tag_id = ?) WHERE id = ?;"));
	if (UpdateStmt)
	{
		UpdateStmt->SetBindingValueByIndex(1, Ref.TagId);
		UpdateStmt->SetBindingValueByIndex(2, Ref.TagId);
		UpdateStmt->Execute();
	}

	return Database->GetLastInsertRowId();
}

// ============================================================
// Config CRUD
// ============================================================

int64 FMonolithIndexDatabase::InsertConfig(const FIndexedConfig& Config)
{
	if (!IsOpen()) return -1;

	FSQLitePreparedStatement* Stmt = StatementCache.FindOrCreate(*Database, TEXT("InsertConfig"), TEXT("INSERT INTO configs (file_path, section, key, value) VALUES (?, ?, ?, ?);"));
	if (!Stmt) return -1;
	Stmt->SetBindingValueByIndex(1, Config.FilePath);
	Stmt->SetBindingValueByIndex(2, Config.Section);
	Stmt->SetBindingValueByIndex(3, Config.Key);
	Stmt->SetBindingValueByIndex(4, Config.Value);

	if (!Stmt->Execute()) return -1;
	return Database->GetLastInsertRowId();
}

// ============================================================
// C++ Symbol CRUD
// ============================================================

int64 FMonolithIndexDatabase::InsertCppSymbol(const FIndexedCppSymbol& Symbol)
{
	if (!IsOpen()) return -1;

	FSQLitePreparedStatement* Stmt = StatementCache.FindOrCreate(*Database, TEXT("InsertCppSymbol"), TEXT("INSERT INTO cpp_symbols (file_path, symbol_name, symbol_type, signature, line_number, parent_symbol) VALUES (?, ?, ?, ?, ?, ?);"));
	if (!Stmt) return -1;
	Stmt->SetBindingValueByIndex(1, Symbol.FilePath);
	Stmt->SetBindingValueByIndex(2, Symbol.SymbolName);
	Stmt->SetBindingValueByIndex(3, Symbol.SymbolType);
	Stmt->SetBindingValueByIndex(4, Symbol.Signature);
	Stmt->SetBindingValueByIndex(5, static_cast<int64>(Symbol.LineNumber));
	Stmt->SetBindingValueByIndex(6, Symbol.ParentSymbol);

	if (!Stmt->Execute()) return -1;
	return Database->GetLastInsertRowId();
}

// ============================================================
// DataTable Row CRUD
// ============================================================

int64 FMonolithIndexDatabase::InsertDataTableRow(const FIndexedDataTableRow& Row)
{
	if (!IsOpen()) return -1;

	FSQLitePreparedStatement* Stmt = StatementCache.FindOrCreate(*Database, TEXT("InsertDataTableRow"), TEXT("INSERT INTO datatable_rows (asset_id, row_name, row_data) VALUES (?, ?, ?);"));
	if (!Stmt) return -1;
	Stmt->SetBindingValueByIndex(1, Row.AssetId);
	Stmt->SetBindingValueByIndex(2, Row.RowName);
	Stmt->SetBindingValueByIndex(3, Row.RowData);

	if (!Stmt->Execute()) return -1;
	return Database->GetLastInsertRowId();
}

// ============================================================
// Meta
// ============================================================

bool FMonolithIndexDatabase::WriteMeta(const FString& Key, const FString& Value)
{
	if (!IsOpen()) return false;

	FSQLitePreparedStatement* Stmt = StatementCache.FindOrCreate(*Database, TEXT("WriteMeta"), TEXT("INSERT OR REPLACE INTO meta (key, value) VALUES (?, ?);"));
	if (!Stmt) return false;
	Stmt->SetBindingValueByIndex(1, Key);
	Stmt->SetBindingValueByIndex(2, Value);
	return Stmt->Execute();
}

FString FMonolithIndexDatabase::ReadMeta(const FString& Key) const
{
	if (!Database || !Database->IsValid()) return FString();

	FSQLitePreparedStatement* Stmt = StatementCache.FindOrCreate(*Database, TEXT("ReadMeta"), TEXT("SELECT value FROM meta WHERE key = ?;"));
	if (!Stmt) return FString();
	Stmt->SetBindingValueByIndex(1, Key);

	if (Stmt->Step() == ESQLitePreparedStatementStepResult::Row)
	{
		FString Value;
		Stmt->GetColumnValueByIndex(0, Value);
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

	FSQLitePreparedStatement* Stmt = StatementCache.FindOrCreate(*Database, TEXT("GetAllIndexedPaths"), TEXT("SELECT package_path FROM assets;"));
	if (!Stmt) return Result;

	while (Stmt->Step() == ESQLitePreparedStatementStepResult::Row)
	{
		FString Path;
		Stmt->GetColumnValueByIndex(0, Path);
		Result.Add(MoveTemp(Path));
	}
	return Result;
}

FString FMonolithIndexDatabase::GetSavedHash(const FString& PackagePath)
{
	if (!IsOpen()) return FString();

	FSQLitePreparedStatement* Stmt = StatementCache.FindOrCreate(*Database, TEXT("GetSavedHash"), TEXT("SELECT saved_hash FROM assets WHERE package_path = ?;"));
	if (!Stmt) return FString();
	Stmt->SetBindingValueByIndex(1, PackagePath);

	if (Stmt->Step() == ESQLitePreparedStatementStepResult::Row)
	{
		FString Hash;
		Stmt->GetColumnValueByIndex(0, Hash);
		return Hash;
	}
	return FString();
}

TMap<FString, FString> FMonolithIndexDatabase::GetAllPathsAndHashes()
{
	TMap<FString, FString> Result;
	if (!IsOpen()) return Result;

	FSQLitePreparedStatement* Stmt = StatementCache.FindOrCreate(*Database, TEXT("GetAllPathsAndHashes"), TEXT("SELECT package_path, saved_hash FROM assets;"));
	if (!Stmt) return Result;

	while (Stmt->Step() == ESQLitePreparedStatementStepResult::Row)
	{
		FString Path, Hash;
		Stmt->GetColumnValueByIndex(0, Path);
		Stmt->GetColumnValueByIndex(1, Hash);
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

	const FString SearchTokens = BuildIndexAssetSearchTokens(NewPath, NewAssetName, FString());
	FSQLitePreparedStatement* Stmt = StatementCache.FindOrCreate(*Database, TEXT("UpdateAssetPath"), TEXT("UPDATE assets SET package_path = ?, asset_name = COALESCE(NULLIF(?, ''), asset_name), search_tokens = ? WHERE package_path = ?;"));
	if (!Stmt) return false;
	Stmt->SetBindingValueByIndex(1, NewPath);
	Stmt->SetBindingValueByIndex(2, NewAssetName);
	Stmt->SetBindingValueByIndex(3, SearchTokens);
	Stmt->SetBindingValueByIndex(4, OldPath);

	if (!Stmt->Execute()) return false;

	// Check if a row was actually updated
	// GetLastInsertRowId isn't useful for UPDATE; use changes count via a follow-up query
	FSQLitePreparedStatement* ChangesStmt = StatementCache.FindOrCreate(*Database, TEXT("Changes"), TEXT("SELECT changes();"));
	if (ChangesStmt && ChangesStmt->Step() == ESQLitePreparedStatementStepResult::Row)
	{
		int64 Changes = 0;
		ChangesStmt->GetColumnValueByIndex(0, Changes);
		return Changes > 0;
	}
	return false;
}

bool FMonolithIndexDatabase::UpdateAssetMetadata(const FIndexedAsset& Asset)
{
	if (!IsOpen()) return false;

	const FString SearchTokens = BuildIndexAssetSearchTokens(Asset.PackagePath, Asset.AssetName, Asset.AssetClass);
	FSQLitePreparedStatement* Stmt = StatementCache.FindOrCreate(*Database, TEXT("UpdateAssetMetadata"), TEXT("UPDATE assets SET asset_name = ?, asset_class = ?, module_name = ?, description = ?, search_tokens = ?, file_size_bytes = ?, last_modified = ?, saved_hash = ?, indexed_at = datetime('now') WHERE package_path = ?;"));
	if (!Stmt) return false;
	Stmt->SetBindingValueByIndex(1, Asset.AssetName);
	Stmt->SetBindingValueByIndex(2, Asset.AssetClass);
	Stmt->SetBindingValueByIndex(3, Asset.ModuleName);
	Stmt->SetBindingValueByIndex(4, Asset.Description);
	Stmt->SetBindingValueByIndex(5, SearchTokens);
	Stmt->SetBindingValueByIndex(6, Asset.FileSizeBytes);
	Stmt->SetBindingValueByIndex(7, Asset.LastModified);
	Stmt->SetBindingValueByIndex(8, Asset.SavedHash);
	Stmt->SetBindingValueByIndex(9, Asset.PackagePath);

	if (!Stmt->Execute()) return false;

	FSQLitePreparedStatement* ChangesStmt = StatementCache.FindOrCreate(*Database, TEXT("Changes"), TEXT("SELECT changes();"));
	if (ChangesStmt && ChangesStmt->Step() == ESQLitePreparedStatementStepResult::Row)
	{
		int64 Changes = 0;
		ChangesStmt->GetColumnValueByIndex(0, Changes);
		return Changes > 0;
	}
	return false;
}

// Deletes per-asset child data that deep indexing repopulates (nodes, variables, parameters, datatable_rows).
// Does NOT delete: dependencies (DependencyIndexer sentinel), tag_references (GameplayTagIndexer sentinel),
// actors (LevelIndexer sentinel). Those are scoped separately.
bool FMonolithIndexDatabase::DeleteChildDataForAsset(int64 AssetId)
{
	if (!IsOpen()) return false;

	bool bSuccess = true;

	FSQLitePreparedStatement* Stmt1 = StatementCache.FindOrCreate(*Database, TEXT("DeleteAssetNodes"), TEXT("DELETE FROM nodes WHERE asset_id = ?;"));
	if (Stmt1)
	{
		Stmt1->SetBindingValueByIndex(1, AssetId);
		bSuccess &= Stmt1->Execute();
	}
	else
	{
		bSuccess = false;
	}

	FSQLitePreparedStatement* Stmt2 = StatementCache.FindOrCreate(*Database, TEXT("DeleteAssetVariables"), TEXT("DELETE FROM variables WHERE asset_id = ?;"));
	if (Stmt2)
	{
		Stmt2->SetBindingValueByIndex(1, AssetId);
		bSuccess &= Stmt2->Execute();
	}
	else
	{
		bSuccess = false;
	}

	FSQLitePreparedStatement* Stmt3 = StatementCache.FindOrCreate(*Database, TEXT("DeleteAssetParameters"), TEXT("DELETE FROM parameters WHERE asset_id = ?;"));
	if (Stmt3)
	{
		Stmt3->SetBindingValueByIndex(1, AssetId);
		bSuccess &= Stmt3->Execute();
	}
	else
	{
		bSuccess = false;
	}

	FSQLitePreparedStatement* Stmt4 = StatementCache.FindOrCreate(*Database, TEXT("DeleteAssetDataTableRows"), TEXT("DELETE FROM datatable_rows WHERE asset_id = ?;"));
	if (Stmt4)
	{
		Stmt4->SetBindingValueByIndex(1, AssetId);
		bSuccess &= Stmt4->Execute();
	}
	else
	{
		bSuccess = false;
	}

	return bSuccess;
}

bool FMonolithIndexDatabase::UpdateSavedHash(const FString& PackagePath, const FString& HashHex)
{
	if (!IsOpen()) return false;

	FSQLitePreparedStatement* Stmt = StatementCache.FindOrCreate(*Database, TEXT("UpdateSavedHash"), TEXT("UPDATE assets SET saved_hash = ? WHERE package_path = ?;"));
	if (!Stmt) return false;
	Stmt->SetBindingValueByIndex(1, HashHex);
	Stmt->SetBindingValueByIndex(2, PackagePath);

	if (!Stmt->Execute()) return false;

	FSQLitePreparedStatement* ChangesStmt = StatementCache.FindOrCreate(*Database, TEXT("Changes"), TEXT("SELECT changes();"));
	if (ChangesStmt && ChangesStmt->Step() == ESQLitePreparedStatementStepResult::Row)
	{
		int64 Changes = 0;
		ChangesStmt->GetColumnValueByIndex(0, Changes);
		return Changes > 0;
	}
	return false;
}

// ============================================================
// FTS5 Full-text search
// ============================================================

TArray<FSearchResult> FMonolithIndexDatabase::FullTextSearch(const FString& Query, int32 Limit)
{
	TArray<FSearchResult> Results;
	if (!IsOpen()) return Results;

	static const TCHAR* SearchSQL = TEXT(R"SQL(
SELECT asset_path, asset_name, asset_class, module_name, match_context, score
FROM (
    SELECT
        a.package_path AS asset_path,
        a.asset_name AS asset_name,
        a.asset_class AS asset_class,
        a.module_name AS module_name,
        snippet(fts_assets, -1, '>>>', '<<<', '...', 32) AS match_context,
        bm25(fts_assets, 10.0, 3.0, 1.0, 5.0, 2.0, 1.0) * 1.00 AS score,
        1.00 AS source_weight
    FROM fts_assets f
    JOIN assets a ON a.id = f.rowid
    WHERE fts_assets MATCH ?

    UNION ALL

    SELECT
        a.package_path AS asset_path,
        a.asset_name AS asset_name,
        a.asset_class AS asset_class,
        a.module_name AS module_name,
        snippet(fts_nodes, -1, '>>>', '<<<', '...', 32) AS match_context,
        bm25(fts_nodes, 5.0, 2.0, 1.0, 4.0) * 1.20 AS score,
        1.20 AS source_weight
    FROM fts_nodes f
    JOIN nodes n ON n.id = f.rowid
    JOIN assets a ON a.id = n.asset_id
    WHERE fts_nodes MATCH ?
)
ORDER BY score ASC
LIMIT ?;
)SQL");

	FSQLitePreparedStatement* Stmt = StatementCache.FindOrCreate(*Database, TEXT("FullTextSearch"), SearchSQL);
	if (!Stmt) return Results;
	Stmt->SetBindingValueByIndex(1, Query);
	Stmt->SetBindingValueByIndex(2, Query);
	Stmt->SetBindingValueByIndex(3, static_cast<int64>(FMath::Max(1, Limit)));

	while (Stmt->Step() == ESQLitePreparedStatementStepResult::Row)
	{
		FSearchResult R;
		Stmt->GetColumnValueByIndex(0, R.AssetPath);
		Stmt->GetColumnValueByIndex(1, R.AssetName);
		Stmt->GetColumnValueByIndex(2, R.AssetClass);
		Stmt->GetColumnValueByIndex(3, R.ModuleName);
		Stmt->GetColumnValueByIndex(4, R.MatchContext);
		double RankD = 0.0;
		Stmt->GetColumnValueByIndex(5, RankD);
		R.Rank = static_cast<float>(RankD);
		Results.Add(MoveTemp(R));
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
	ModStmt.Create(*Database, TEXT("SELECT CASE WHEN module_name = '' THEN 'Project' ELSE module_name END as mod, COUNT(*) as cnt FROM assets GROUP BY module_name ORDER BY cnt DESC;"));
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
	TArray<TSharedPtr<FJsonValue>> NodesArr;
	for (const auto& Node : GetNodesForAsset(Asset.Id))
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
	TArray<TSharedPtr<FJsonValue>> VarsArr;
	for (const auto& Var : GetVariablesForAsset(Asset.Id))
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
	TArray<FIndexedAsset> Result;
	if (!IsOpen()) return Result;

	FSQLitePreparedStatement Stmt;
	Stmt.Create(*Database, TEXT("SELECT id, package_path, asset_name, asset_class, module_name, description, file_size_bytes, last_modified, saved_hash, indexed_at FROM assets WHERE asset_class = ? LIMIT ? OFFSET ?;"));
	Stmt.SetBindingValueByIndex(1, AssetClass);
	Stmt.SetBindingValueByIndex(2, static_cast<int64>(Limit));
	Stmt.SetBindingValueByIndex(3, static_cast<int64>(Offset));

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
	TArray<TSharedPtr<FJsonValue>> DepsArr;
	for (const auto& Dep : GetDependenciesForAsset(AssetId))
	{
		FSQLitePreparedStatement Stmt;
		Stmt.Create(*Database, TEXT("SELECT package_path, asset_class FROM assets WHERE id = ?;"));
		Stmt.SetBindingValueByIndex(1, Dep.TargetAssetId);
		if (Stmt.Step() == ESQLitePreparedStatementStepResult::Row)
		{
			auto DepObj = MakeShared<FJsonObject>();
			FString Path, Class;
			Stmt.GetColumnValueByIndex(0, Path);
			Stmt.GetColumnValueByIndex(1, Class);
			DepObj->SetStringField(TEXT("path"), Path);
			DepObj->SetStringField(TEXT("class"), Class);
			DepObj->SetStringField(TEXT("type"), Dep.DependencyType);
			DepsArr.Add(MakeShared<FJsonValueObject>(DepObj));
		}
	}
	Result->SetArrayField(TEXT("depends_on"), DepsArr);

	// What references this asset
	TArray<TSharedPtr<FJsonValue>> RefsArr;
	for (const auto& Ref : GetReferencersOfAsset(AssetId))
	{
		FSQLitePreparedStatement Stmt;
		Stmt.Create(*Database, TEXT("SELECT package_path, asset_class FROM assets WHERE id = ?;"));
		Stmt.SetBindingValueByIndex(1, Ref.SourceAssetId);
		if (Stmt.Step() == ESQLitePreparedStatementStepResult::Row)
		{
			auto RefObj = MakeShared<FJsonObject>();
			FString Path, Class;
			Stmt.GetColumnValueByIndex(0, Path);
			Stmt.GetColumnValueByIndex(1, Class);
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

	const bool bCoreSchemaOk = ExecuteMonolithSQLiteMulti(*Database, GCreateCoreTablesSQL, true);
	const bool bFtsSchemaOk = ExecuteMonolithSQLiteMulti(*Database, GCreateFtsTablesSQL, true);
	if (!bCoreSchemaOk || !bFtsSchemaOk)
	{
		UE_LOG(LogMonolithIndex, Warning, TEXT("Some schema statements failed -- FTS5 may not be available in this SQLite build: %s"),
			*Database->GetLastError());
	}
	return true; // Return true even if FTS fails -- basic tables should work
}

bool FMonolithIndexDatabase::MigrateFtsSchemaToV4()
{
	if (!Database || !Database->IsValid())
	{
		return false;
	}

	UE_LOG(LogMonolithIndex, Log, TEXT("Migrating index FTS schema to v%d"), GIndexSchemaVersion);
	StatementCache.Clear();

	if (!ExecuteSQL(TEXT("BEGIN;")))
	{
		return false;
	}

	if (!MonolithIndexTableHasColumn(*Database, TEXT("assets"), TEXT("search_tokens")))
	{
		if (!ExecuteSQL(TEXT("ALTER TABLE assets ADD COLUMN search_tokens TEXT DEFAULT '';")))
		{
			ExecuteSQL(TEXT("ROLLBACK;"));
			return false;
		}
	}
	if (!MonolithIndexTableHasColumn(*Database, TEXT("nodes"), TEXT("search_tokens")))
	{
		if (!ExecuteSQL(TEXT("ALTER TABLE nodes ADD COLUMN search_tokens TEXT DEFAULT '';")))
		{
			ExecuteSQL(TEXT("ROLLBACK;"));
			return false;
		}
	}

	{
		TArray<FIndexedAsset> AssetRows;
		FSQLitePreparedStatement SelectAssets;
		if (SelectAssets.Create(*Database, TEXT("SELECT id, package_path, asset_name, asset_class FROM assets;"), ESQLitePreparedStatementFlags::Persistent))
		{
			while (SelectAssets.Step() == ESQLitePreparedStatementStepResult::Row)
			{
				FIndexedAsset Asset;
				SelectAssets.GetColumnValueByIndex(0, Asset.Id);
				SelectAssets.GetColumnValueByIndex(1, Asset.PackagePath);
				SelectAssets.GetColumnValueByIndex(2, Asset.AssetName);
				SelectAssets.GetColumnValueByIndex(3, Asset.AssetClass);
				AssetRows.Add(MoveTemp(Asset));
			}
		}

		FSQLitePreparedStatement UpdateAssetTokens;
		if (UpdateAssetTokens.Create(*Database, TEXT("UPDATE assets SET search_tokens = ? WHERE id = ?;"), ESQLitePreparedStatementFlags::Persistent))
		{
			for (const FIndexedAsset& Asset : AssetRows)
			{
				UpdateAssetTokens.Reset();
				UpdateAssetTokens.SetBindingValueByIndex(1, BuildIndexAssetSearchTokens(Asset.PackagePath, Asset.AssetName, Asset.AssetClass));
				UpdateAssetTokens.SetBindingValueByIndex(2, Asset.Id);
				UpdateAssetTokens.Execute();
			}
		}

		TArray<FIndexedNode> NodeRows;
		FSQLitePreparedStatement SelectNodes;
		if (SelectNodes.Create(*Database, TEXT("SELECT id, node_name, node_class, node_type FROM nodes;"), ESQLitePreparedStatementFlags::Persistent))
		{
			while (SelectNodes.Step() == ESQLitePreparedStatementStepResult::Row)
			{
				FIndexedNode Node;
				SelectNodes.GetColumnValueByIndex(0, Node.Id);
				SelectNodes.GetColumnValueByIndex(1, Node.NodeName);
				SelectNodes.GetColumnValueByIndex(2, Node.NodeClass);
				SelectNodes.GetColumnValueByIndex(3, Node.NodeType);
				NodeRows.Add(MoveTemp(Node));
			}
		}

		FSQLitePreparedStatement UpdateNodeTokens;
		if (UpdateNodeTokens.Create(*Database, TEXT("UPDATE nodes SET search_tokens = ? WHERE id = ?;"), ESQLitePreparedStatementFlags::Persistent))
		{
			for (const FIndexedNode& Node : NodeRows)
			{
				UpdateNodeTokens.Reset();
				UpdateNodeTokens.SetBindingValueByIndex(1, BuildIndexNodeSearchTokens(Node.NodeName, Node.NodeClass, Node.NodeType));
				UpdateNodeTokens.SetBindingValueByIndex(2, Node.Id);
				UpdateNodeTokens.Execute();
			}
		}
	}

	ExecuteSQL(TEXT("DROP TRIGGER IF EXISTS fts_assets_ai;"));
	ExecuteSQL(TEXT("DROP TRIGGER IF EXISTS fts_assets_ad;"));
	ExecuteSQL(TEXT("DROP TRIGGER IF EXISTS fts_assets_au;"));
	ExecuteSQL(TEXT("DROP TRIGGER IF EXISTS fts_nodes_ai;"));
	ExecuteSQL(TEXT("DROP TRIGGER IF EXISTS fts_nodes_ad;"));
	ExecuteSQL(TEXT("DROP TRIGGER IF EXISTS fts_nodes_au;"));
	ExecuteSQL(TEXT("DROP TABLE IF EXISTS fts_assets;"));
	ExecuteSQL(TEXT("DROP TABLE IF EXISTS fts_nodes;"));

	const bool bCreated = CreateTables();

	const bool bAssetsRebuilt = ExecuteSQL(TEXT("INSERT INTO fts_assets(rowid, asset_name, asset_class, description, search_tokens, package_path, module_name) SELECT id, asset_name, asset_class, description, search_tokens, package_path, module_name FROM assets;"));
	const bool bNodesRebuilt = ExecuteSQL(TEXT("INSERT INTO fts_nodes(rowid, node_name, node_class, node_type, search_tokens) SELECT id, node_name, node_class, node_type, search_tokens FROM nodes;"));
	if (!bCreated || !bAssetsRebuilt || !bNodesRebuilt)
	{
		ExecuteSQL(TEXT("ROLLBACK;"));
		UE_LOG(LogMonolithIndex, Warning, TEXT("Index FTS v3 rebuild incomplete: %s"), *Database->GetLastError());
		return false;
	}

	if (!ExecuteSQL(TEXT("COMMIT;")))
	{
		return false;
	}

	FMonolithSQLiteMaintenanceOptions MaintenanceOptions;
	MaintenanceOptions.bRunPragmaOptimize = true;
	MaintenanceOptions.bUseFullOptimizeScan = true;
	MaintenanceOptions.FtsTablesToOptimize.Add(TEXT("fts_assets"));
	MaintenanceOptions.FtsTablesToOptimize.Add(TEXT("fts_nodes"));
	RunMonolithSQLiteMaintenance(*Database, MaintenanceOptions);

	return true;
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
