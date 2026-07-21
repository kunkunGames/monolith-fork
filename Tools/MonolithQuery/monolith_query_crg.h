#pragma once

// CRG graph/cache, snapshot, and source review maintenance helpers.

static int64_t count_rows(Database& db, const std::string& sql) {
    auto rows = query(db, sql);
    if (rows.empty() || rows[0].cols.empty()) return -1;
    return rows[0].cols.begin()->second.empty() ? 0 : rows[0].get_int64(rows[0].cols.begin()->first);
}

static std::string scalar_str(Database& db, const std::string& sql) {
    auto rows = query(db, sql);
    if (rows.empty() || rows[0].cols.empty()) return "";
    return rows[0].cols.begin()->second;
}

static bool object_exists(Database& db, const std::string& type, const std::string& name) {
    return !query(db, "SELECT 1 FROM sqlite_master WHERE type = ? AND name = ?;", {type, name}).empty();
}

static void add_next(json& root, std::initializer_list<const char*> actions) {
    root["next_actions"] = json::array();
    for (const char* action : actions) root["next_actions"].push_back(action);
}

static bool remove_file_best_effort(const std::string& path, std::string* error = nullptr) {
    std::error_code ec;
    fs::remove(path, ec);
    if (ec && error) *error = ec.message();
    return !ec;
}

static void remove_sqlite_sidecars_best_effort(const std::string& db_path) {
    remove_file_best_effort(db_path + "-journal");
    remove_file_best_effort(db_path + "-wal");
    remove_file_best_effort(db_path + "-shm");
}

static bool atomic_replace_file(const std::string& source_path,
                                const std::string& target_path,
                                std::string& error) {
#ifdef _WIN32
    fs::path source_fs(source_path);
    fs::path target_fs(target_path);
    if (!MoveFileExW(source_fs.wstring().c_str(),
                     target_fs.wstring().c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DWORD code = GetLastError();
        std::ostringstream out;
        out << "MoveFileExW failed with Win32 error " << code;
        error = out.str();
        return false;
    }
    return true;
#else
    std::error_code ec;
    fs::rename(source_path, target_path, ec);
    if (ec) {
        error = ec.message();
        return false;
    }
    return true;
#endif
}

static bool try_open_readonly_database(Database& db,
                                       const std::string& path,
                                       std::string& error) {
    try {
        db.open(path, true);
        return true;
    } catch (const QueryFatal& e) {
        error = e.what();
    } catch (const std::exception& e) {
        error = e.what();
    }
    return false;
}

static bool looks_like_graph_busy_error(const std::string& error) {
    const std::string lower = lower_copy(error);
    return lower.find("database is locked") != std::string::npos
        || lower.find("rollback journal") != std::string::npos
        || lower.find("sharing violation") != std::string::npos
        || lower.find("locked") != std::string::npos;
}

static json graph_busy_json(const std::string& action,
                            const std::string& graph_db_path,
                            const std::string& reason) {
    json root = {
        {"status", "busy"},
        {"summary", "CRG graph database is busy; use EngineSource-backed source actions while the graph rebuild finishes"},
        {"action", action},
        {"graph_db", graph_db_path},
        {"journal_path", graph_db_path + "-journal"},
        {"busy_reason", reason},
        {"warnings", json::array({reason})},
        {"next_actions", json::array({"source.search_source", "source.risk_score", "source.review_context", "source.crg_graph_health"})},
        {"truncated", false},
    };
    return root;
}

class GraphRebuildLock {
public:
    explicit GraphRebuildLock(std::string path)
        : lock_path(std::move(path)) {}

    GraphRebuildLock(const GraphRebuildLock&) = delete;
    GraphRebuildLock& operator=(const GraphRebuildLock&) = delete;

    ~GraphRebuildLock() {
        release();
    }

    bool acquire(json& busy) {
        for (int attempt = 0; attempt < 2; ++attempt) {
            if (try_create_lock()) {
                write_payload();
                return true;
            }

            json existing = read_payload();
            const int64_t owner_pid = existing.value("owner_pid", static_cast<int64_t>(0));
            const bool stale = owner_pid > 0 && !process_is_running(owner_pid);
            if (stale) {
                remove_file_best_effort(lock_path);
                continue;
            }

            busy = {
                {"status", "busy"},
                {"summary", "CRG graph rebuild is already running"},
                {"lock_path", lock_path},
                {"owner_pid", owner_pid},
                {"lock", existing},
                {"next_actions", json::array({"source.crg_graph_health", "source.search_source", "source.review_context"})},
                {"warnings", json::array({"another build_crg_graph --execute process owns the graph rebuild lock"})},
                {"truncated", false},
            };
            return false;
        }

        busy = {
            {"status", "busy"},
            {"summary", "CRG graph rebuild lock could not be acquired"},
            {"lock_path", lock_path},
            {"next_actions", json::array({"source.crg_graph_health", "source.search_source", "source.review_context"})},
            {"warnings", json::array({"graph rebuild lock exists and could not be reclaimed"})},
            {"truncated", false},
        };
        return false;
    }

    const std::string& path() const {
        return lock_path;
    }

private:
    std::string lock_path;
#ifdef _WIN32
    HANDLE handle = INVALID_HANDLE_VALUE;
#else
    int fd = -1;
#endif

    bool try_create_lock() {
        fs::path parent = fs::path(lock_path).parent_path();
        if (!parent.empty()) fs::create_directories(parent);
#ifdef _WIN32
        handle = CreateFileW(fs::path(lock_path).wstring().c_str(),
                             GENERIC_WRITE | DELETE,
                             FILE_SHARE_READ,
                             nullptr,
                             CREATE_NEW,
                             FILE_ATTRIBUTE_TEMPORARY,
                             nullptr);
        return handle != INVALID_HANDLE_VALUE;
#else
        fd = ::open(lock_path.c_str(), O_CREAT | O_EXCL | O_WRONLY, 0600);
        return fd >= 0;
#endif
    }

    void write_payload() {
        json payload = {
            {"owner_pid", current_process_id()},
            {"created_at", iso_local_now()},
            {"tool", "monolith_query"},
            {"purpose", "source build_crg_graph atomic rebuild"},
        };
        const std::string text = payload.dump(2);
#ifdef _WIN32
        if (handle != INVALID_HANDLE_VALUE) {
            DWORD written = 0;
            WriteFile(handle, text.data(), static_cast<DWORD>(text.size()), &written, nullptr);
            FlushFileBuffers(handle);
        }
#else
        if (fd >= 0) {
            (void)::write(fd, text.data(), text.size());
            (void)::fsync(fd);
        }
#endif
    }

    json read_payload() const {
        try {
            std::ifstream in(lock_path, std::ios::binary);
            std::stringstream buffer;
            buffer << in.rdbuf();
            json parsed = json::parse(buffer.str(), nullptr, false);
            if (parsed.is_object()) return parsed;
        } catch (...) {
        }
        return json::object();
    }

    void release() {
#ifdef _WIN32
        if (handle != INVALID_HANDLE_VALUE) {
            CloseHandle(handle);
            handle = INVALID_HANDLE_VALUE;
            remove_file_best_effort(lock_path);
        }
#else
        if (fd >= 0) {
            close(fd);
            fd = -1;
            remove_file_best_effort(lock_path);
        }
#endif
    }
};

static std::string json_string_field(const json& object, const std::string& field,
                                     const std::string& fallback) {
    if (object.is_object() && object.contains(field) && object[field].is_string())
        return object[field].get<std::string>();
    return fallback;
}

static int json_int_field(const json& object, const std::string& field) {
    if (!object.is_object() || !object.contains(field) || !object[field].is_number())
        return 0;
    return object[field].get<int>();
}

static double json_num_field(const json& object, const std::string& field) {
    if (!object.is_object() || !object.contains(field) || !object[field].is_number())
        return 0.0;
    return object[field].get<double>();
}

static bool json_bool_field(const json& object, const std::string& field) {
    if (!object.is_object() || !object.contains(field) || !object[field].is_boolean())
        return false;
    return object[field].get<bool>();
}

static const char* premerge_severity_name(int severity) {
    return severity >= 2 ? "fail" : severity == 1 ? "warn" : "pass";
}

static void premerge_add_check(json& checks, int& severity, const std::string& name,
                               const std::string& status, const std::string& summary,
                               int check_severity) {
    checks.push_back({
        {"name", name},
        {"status", status},
        {"severity", premerge_severity_name(check_severity)},
        {"summary", summary},
    });
    severity = std::max(severity, check_severity);
}

static void premerge_add_finding(json& findings, const std::string& severity,
                                 const std::string& check, const std::string& message) {
    findings.push_back({
        {"severity", severity},
        {"check", check},
        {"message", message},
    });
}

struct SnapshotManifest {
    std::set<std::string> nodes;
    std::set<std::string> edges;
};

struct SnapshotRecord {
    int64_t id = 0;
    std::string label;
    SnapshotManifest manifest;
};

static json set_to_json_array(const std::set<std::string>& values) {
    json arr = json::array();
    for (const auto& value : values) arr.push_back(value);
    return arr;
}

static std::string serialize_manifest(const SnapshotManifest& manifest) {
    json object = {
        {"nodes", set_to_json_array(manifest.nodes)},
        {"edges", set_to_json_array(manifest.edges)},
    };
    return object.dump();
}

static bool parse_manifest(const std::string& text, SnapshotManifest& out) {
    json object = json::parse(text, nullptr, false);
    if (!object.is_object() || !object.contains("nodes") || !object.contains("edges")
        || !object["nodes"].is_array() || !object["edges"].is_array())
        return false;
    out.nodes.clear();
    out.edges.clear();
    for (const auto& node : object["nodes"]) {
        if (!node.is_string()) return false;
        out.nodes.insert(node.get<std::string>());
    }
    for (const auto& edge : object["edges"]) {
        if (!edge.is_string()) return false;
        out.edges.insert(edge.get<std::string>());
    }
    return true;
}

static bool exec_sql_ok(Database& db, const std::string& sql, std::string& error) {
    char* raw_error = nullptr;
    int rc = sqlite3_exec(db.db, sql.c_str(), nullptr, nullptr, &raw_error);
    if (raw_error) {
        error = raw_error;
        sqlite3_free(raw_error);
    }
    if (rc != SQLITE_OK) {
        if (error.empty()) error = sqlite3_errmsg(db.db);
        return false;
    }
    return true;
}

static bool ensure_snapshot_table(Database& db, std::string& error) {
    return exec_sql_ok(db,
        "CREATE TABLE IF NOT EXISTS crg_snapshots ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "label TEXT NOT NULL,"
        "domain TEXT NOT NULL,"
        "captured_at INTEGER NOT NULL,"
        "node_count INTEGER NOT NULL,"
        "edge_count INTEGER NOT NULL,"
        "manifest_json TEXT NOT NULL,"
        "UNIQUE(domain,label)"
        ");", error);
}

static std::string snapshot_edge_key(const Row& row);

static const char* kCrgProjectionDdl[] = {
    "CREATE TABLE IF NOT EXISTS crg_nodes ("
    "id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "domain TEXT NOT NULL,"
    "native_table TEXT NOT NULL,"
    "native_id INTEGER NOT NULL,"
    "stable_key TEXT NOT NULL,"
    "kind TEXT,"
    "name TEXT,"
    "path TEXT,"
    "module TEXT,"
    "source_revision TEXT,"
    "extra TEXT,"
    "updated_at INTEGER NOT NULL,"
    "UNIQUE(domain, native_table, native_id),"
    "UNIQUE(domain, stable_key)"
    ");",
    "CREATE TABLE IF NOT EXISTS crg_edges ("
    "id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "domain TEXT NOT NULL,"
    "source_node_id INTEGER NOT NULL,"
    "target_node_id INTEGER NOT NULL,"
    "edge_kind TEXT NOT NULL,"
    "edge_subkind TEXT,"
    "weight REAL NOT NULL DEFAULT 1.0,"
    "native_table TEXT,"
    "native_id INTEGER,"
    "updated_at INTEGER NOT NULL"
    ");",
    "CREATE TABLE IF NOT EXISTS crg_node_metrics ("
    "node_id INTEGER PRIMARY KEY,"
    "fan_in INTEGER NOT NULL DEFAULT 0,"
    "fan_out INTEGER NOT NULL DEFAULT 0,"
    "hard_in INTEGER NOT NULL DEFAULT 0,"
    "descendants INTEGER NOT NULL DEFAULT 0,"
    "risk_score REAL NOT NULL DEFAULT 0.0,"
    "risk_tier TEXT NOT NULL DEFAULT 'low',"
    "reasons_json TEXT NOT NULL DEFAULT '[]',"
    "raw_counts_json TEXT NOT NULL DEFAULT '{}',"
    "scoring_version TEXT NOT NULL,"
    "computed_at INTEGER NOT NULL"
    ");",
    "CREATE TABLE IF NOT EXISTS crg_meta ("
    "key TEXT PRIMARY KEY,"
    "value TEXT NOT NULL"
    ");",
    "CREATE TABLE IF NOT EXISTS source_override_edges ("
    "child_symbol_id INTEGER NOT NULL,"
    "parent_symbol_id INTEGER NOT NULL,"
    "confidence TEXT NOT NULL DEFAULT 'high',"
    "reason TEXT NOT NULL DEFAULT '',"
    "updated_at INTEGER NOT NULL,"
    "PRIMARY KEY(child_symbol_id, parent_symbol_id)"
    ");",
    "CREATE INDEX IF NOT EXISTS idx_crg_nodes_domain_native ON crg_nodes(domain, native_table, native_id);",
    "CREATE INDEX IF NOT EXISTS idx_crg_nodes_stable ON crg_nodes(domain, stable_key);",
    "CREATE INDEX IF NOT EXISTS idx_crg_edges_domain_source ON crg_edges(domain, source_node_id);",
    "CREATE INDEX IF NOT EXISTS idx_crg_edges_domain_target ON crg_edges(domain, target_node_id);",
    "CREATE INDEX IF NOT EXISTS idx_crg_edges_kind_subkind ON crg_edges(domain, edge_kind, edge_subkind);",
    "CREATE INDEX IF NOT EXISTS idx_crg_metrics_score ON crg_node_metrics(risk_score DESC);",
    "CREATE INDEX IF NOT EXISTS idx_source_override_edges_parent ON source_override_edges(parent_symbol_id, child_symbol_id);",
    "CREATE INDEX IF NOT EXISTS idx_source_override_edges_child ON source_override_edges(child_symbol_id, parent_symbol_id);",
};

static const char* kSourceReviewIndexDdl[] = {
    "CREATE INDEX IF NOT EXISTS idx_symbols_parent_name_kind ON symbols(parent_symbol_id, name, kind);",
    "CREATE INDEX IF NOT EXISTS idx_symbols_name_kind_parent ON symbols(name, kind, parent_symbol_id);",
    "CREATE INDEX IF NOT EXISTS idx_symbols_override_signature ON symbols(kind, parent_symbol_id, name) WHERE kind='function' AND signature LIKE '%override%';",
    "CREATE INDEX IF NOT EXISTS idx_inheritance_parent_child ON inheritance(parent_id, child_id);",
    "CREATE INDEX IF NOT EXISTS idx_inheritance_child_parent ON inheritance(child_id, parent_id);",
};

static bool ensure_crg_projection_tables(Database& db, std::string& error) {
    for (const char* sql : kCrgProjectionDdl) {
        if (!exec_sql_ok(db, sql, error)) return false;
    }
    return true;
}

static bool ensure_source_review_indexes(Database& db, std::string& error) {
    for (const char* sql : kSourceReviewIndexDdl) {
        if (!exec_sql_ok(db, sql, error)) return false;
    }
    return true;
}

static bool populate_source_override_edge_cache(Database& db, int64_t& out_count, std::string& error) {
    out_count = 0;
    Rows candidates;
    if (!query_rows_ok(db, R"SQL(
WITH RECURSIVE child_seed(id,name,qualified_name,kind,parent_symbol_id,signature) AS (
  SELECT id,name,qualified_name,kind,parent_symbol_id,signature FROM symbols
  WHERE kind = 'function' AND signature LIKE '%override%'
), ancestors(child_class_id, ancestor_class_id) AS (
  SELECT child_id, parent_id FROM inheritance
  UNION
  SELECT a.child_class_id, i.parent_id
  FROM ancestors a
  JOIN inheritance AS i ON i.child_id = a.ancestor_class_id
)
SELECT child_fn.id AS child_id,base_fn.id AS parent_id,
       child_fn.name AS child_name,base_fn.name AS parent_name,
       COALESCE(child_fn.signature,'') AS child_signature,
       COALESCE(base_fn.signature,'') AS parent_signature
FROM child_seed AS child_fn
JOIN symbols child_cls ON child_cls.id = child_fn.parent_symbol_id
JOIN ancestors AS a ON a.child_class_id = child_cls.id
JOIN symbols base_cls ON base_cls.id = a.ancestor_class_id
JOIN symbols AS base_fn INDEXED BY idx_symbols_parent_name_kind ON base_fn.parent_symbol_id = base_cls.id
    AND (base_fn.name = child_fn.name
      OR base_fn.name = base_cls.name || '::' || child_fn.name)
    AND base_fn.kind = child_fn.kind
WHERE child_fn.kind = 'function'
ORDER BY base_fn.id, child_fn.id;
)SQL", {}, candidates, error)) {
        return false;
    }

    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(db.db,
        "INSERT OR IGNORE INTO source_override_edges "
        "(child_symbol_id,parent_symbol_id,confidence,reason,updated_at) "
        "VALUES (?, ?, ?, ?, CAST(strftime('%s','now') AS INTEGER));",
        -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        error = sqlite3_errmsg(db.db);
        return false;
    }

    for (const Row& row : candidates) {
        std::string reason;
        if (!override_signatures_match(row.get("child_signature"), row.get("parent_signature"),
                                       row.get("child_name"), row.get("parent_name"), reason)) {
            continue;
        }
        sqlite3_reset(stmt);
        sqlite3_clear_bindings(stmt);
        sqlite3_bind_int64(stmt, 1, static_cast<sqlite3_int64>(row.get_int64("child_id")));
        sqlite3_bind_int64(stmt, 2, static_cast<sqlite3_int64>(row.get_int64("parent_id")));
        sqlite3_bind_text(stmt, 3, override_confidence_for_reason(reason), -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 4, reason.c_str(), -1, SQLITE_TRANSIENT);
        rc = sqlite3_step(stmt);
        if (rc != SQLITE_DONE) {
            error = sqlite3_errmsg(db.db);
            sqlite3_finalize(stmt);
            return false;
        }
        ++out_count;
    }
    sqlite3_finalize(stmt);
    return true;
}

static bool has_crg_projection_tables(Database& db) {
    return object_exists(db, "table", "crg_nodes")
        && object_exists(db, "table", "crg_edges")
        && object_exists(db, "table", "crg_node_metrics")
        && object_exists(db, "table", "crg_meta");
}

static const char* kCrgGraphDdl[] = {
    "CREATE TABLE IF NOT EXISTS nodes ("
    "id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "kind TEXT NOT NULL,"
    "name TEXT NOT NULL,"
    "qualified_name TEXT NOT NULL UNIQUE,"
    "file_path TEXT NOT NULL,"
    "line_start INTEGER,"
    "line_end INTEGER,"
    "language TEXT,"
    "parent_name TEXT,"
    "params TEXT,"
    "return_type TEXT,"
    "modifiers TEXT,"
    "is_test INTEGER DEFAULT 0,"
    "file_hash TEXT,"
    "extra TEXT DEFAULT '{}',"
    "signature TEXT,"
    "community_id INTEGER,"
    "updated_at REAL NOT NULL"
    ");",
    "CREATE TABLE IF NOT EXISTS edges ("
    "id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "kind TEXT NOT NULL,"
    "source_qualified TEXT NOT NULL,"
    "target_qualified TEXT NOT NULL,"
    "file_path TEXT NOT NULL,"
    "line INTEGER DEFAULT 0,"
    "extra TEXT DEFAULT '{}',"
    "confidence REAL DEFAULT 1.0,"
    "confidence_tier TEXT DEFAULT 'EXTRACTED',"
    "updated_at REAL NOT NULL"
    ");",
    "CREATE TABLE IF NOT EXISTS metadata (key TEXT PRIMARY KEY,value TEXT NOT NULL);",
    "CREATE TABLE IF NOT EXISTS flows ("
    "id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "name TEXT NOT NULL,"
    "entry_point_id INTEGER NOT NULL,"
    "depth INTEGER NOT NULL,"
    "node_count INTEGER NOT NULL,"
    "file_count INTEGER NOT NULL,"
    "criticality REAL NOT NULL DEFAULT 0.0,"
    "path_json TEXT NOT NULL,"
    "created_at TEXT NOT NULL DEFAULT (datetime('now')),"
    "updated_at TEXT NOT NULL DEFAULT (datetime('now'))"
    ");",
    "CREATE TABLE IF NOT EXISTS flow_memberships ("
    "flow_id INTEGER NOT NULL,"
    "node_id INTEGER NOT NULL,"
    "position INTEGER NOT NULL,"
    "PRIMARY KEY (flow_id, node_id)"
    ");",
    "CREATE TABLE IF NOT EXISTS communities ("
    "id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "name TEXT NOT NULL,"
    "level INTEGER NOT NULL DEFAULT 0,"
    "parent_id INTEGER,"
    "cohesion REAL NOT NULL DEFAULT 0.0,"
    "size INTEGER NOT NULL DEFAULT 0,"
    "dominant_language TEXT,"
    "description TEXT,"
    "created_at TEXT NOT NULL DEFAULT (datetime('now'))"
    ");",
    "CREATE TABLE IF NOT EXISTS community_summaries ("
    "community_id INTEGER PRIMARY KEY,"
    "name TEXT NOT NULL,"
    "purpose TEXT DEFAULT '',"
    "key_symbols TEXT DEFAULT '[]',"
    "risk TEXT DEFAULT 'unknown',"
    "size INTEGER DEFAULT 0,"
    "dominant_language TEXT DEFAULT ''"
    ");",
    "CREATE TABLE IF NOT EXISTS flow_snapshots ("
    "flow_id INTEGER PRIMARY KEY,"
    "name TEXT NOT NULL,"
    "entry_point TEXT NOT NULL,"
    "critical_path TEXT DEFAULT '[]',"
    "criticality REAL DEFAULT 0.0,"
    "node_count INTEGER DEFAULT 0,"
    "file_count INTEGER DEFAULT 0"
    ");",
    "CREATE TABLE IF NOT EXISTS risk_index ("
    "node_id INTEGER PRIMARY KEY,"
    "qualified_name TEXT NOT NULL,"
    "risk_score REAL DEFAULT 0.0,"
    "caller_count INTEGER DEFAULT 0,"
    "test_coverage TEXT DEFAULT 'unknown',"
    "security_relevant INTEGER DEFAULT 0,"
    "last_computed TEXT DEFAULT ''"
    ");",
    "CREATE INDEX IF NOT EXISTS idx_nodes_file ON nodes(file_path);",
    "CREATE INDEX IF NOT EXISTS idx_nodes_kind ON nodes(kind);",
    "CREATE INDEX IF NOT EXISTS idx_nodes_qualified ON nodes(qualified_name);",
    "CREATE INDEX IF NOT EXISTS idx_nodes_community ON nodes(community_id);",
    "CREATE INDEX IF NOT EXISTS idx_edges_source ON edges(source_qualified);",
    "CREATE INDEX IF NOT EXISTS idx_edges_target ON edges(target_qualified);",
    "CREATE INDEX IF NOT EXISTS idx_edges_kind ON edges(kind);",
    "CREATE INDEX IF NOT EXISTS idx_edges_target_kind ON edges(target_qualified, kind);",
    "CREATE INDEX IF NOT EXISTS idx_edges_source_kind ON edges(source_qualified, kind);",
    "CREATE INDEX IF NOT EXISTS idx_edges_file ON edges(file_path);",
    "CREATE INDEX IF NOT EXISTS idx_edges_composite ON edges(kind, source_qualified, target_qualified, file_path, line);",
    "CREATE INDEX IF NOT EXISTS idx_flows_criticality ON flows(criticality DESC);",
    "CREATE INDEX IF NOT EXISTS idx_flows_entry ON flows(entry_point_id);",
    "CREATE INDEX IF NOT EXISTS idx_flow_memberships_node ON flow_memberships(node_id);",
    "CREATE INDEX IF NOT EXISTS idx_communities_parent ON communities(parent_id);",
    "CREATE INDEX IF NOT EXISTS idx_communities_cohesion ON communities(cohesion DESC);",
    "CREATE INDEX IF NOT EXISTS idx_risk_index_score ON risk_index(risk_score DESC);",
    "CREATE VIRTUAL TABLE IF NOT EXISTS nodes_fts USING fts5("
    "name, qualified_name, file_path, signature,"
    "content='nodes', content_rowid='id', tokenize='porter unicode61'"
    ");",
};

static bool ensure_crg_graph_tables(Database& db, std::string& error) {
    for (const char* sql : kCrgGraphDdl) {
        if (!exec_sql_ok(db, sql, error)) return false;
    }
    return true;
}

static bool crg_graph_ddl_is_index(const char* sql) {
    return lower_copy(trim_copy(sql)).rfind("create index", 0) == 0;
}

static bool ensure_crg_graph_base_tables(Database& db, std::string& error) {
    for (const char* sql : kCrgGraphDdl) {
        if (crg_graph_ddl_is_index(sql)) continue;
        if (!exec_sql_ok(db, sql, error)) return false;
    }
    return true;
}

static bool ensure_crg_graph_indexes(Database& db, std::string& error) {
    for (const char* sql : kCrgGraphDdl) {
        if (!crg_graph_ddl_is_index(sql)) continue;
        if (!exec_sql_ok(db, sql, error)) return false;
    }
    return true;
}

static bool has_crg_graph_tables(Database& db) {
    return object_exists(db, "table", "nodes")
        && object_exists(db, "table", "edges")
        && object_exists(db, "table", "metadata")
        && object_exists(db, "table", "nodes_fts");
}

static json crg_graph_counts(Database& db) {
    json counts = json::object();
    counts["nodes"] = object_exists(db, "table", "nodes") ? count_rows(db, "SELECT COUNT(*) FROM nodes;") : -1;
    counts["edges"] = object_exists(db, "table", "edges") ? count_rows(db, "SELECT COUNT(*) FROM edges;") : -1;
    counts["files"] = object_exists(db, "table", "nodes") ? count_rows(db, "SELECT COUNT(*) FROM nodes WHERE kind = 'File';") : -1;
    counts["symbols"] = object_exists(db, "table", "nodes") ? count_rows(db, "SELECT COUNT(*) FROM nodes WHERE kind != 'File';") : -1;
    counts["fts_rows"] = object_exists(db, "table", "nodes_fts") ? count_rows(db, "SELECT COUNT(*) FROM nodes_fts;") : -1;
    return counts;
}

static json crg_graph_auxiliary_counts(Database& db) {
    json counts = json::object();
    for (const char* table : {
        "flows",
        "flow_memberships",
        "flow_snapshots",
        "communities",
        "community_summaries",
        "risk_index",
    }) {
        counts[table] = object_exists(db, "table", table)
            ? count_rows(db, std::string("SELECT COUNT(*) FROM ") + table + ";")
            : -1;
    }
    return counts;
}

static json crg_graph_health_json(const std::string& graph_db_path) {
    json checks = json::array();
    json warnings = json::array();
    json root = {
        {"input", {{"graph_db", graph_db_path}}},
        {"graph_db", graph_db_path},
        {"checks", checks},
        {"warnings", warnings},
    };

    if (!fs::exists(graph_db_path)) {
        root["status"] = "warning";
        root["summary"] = "CRG graph database is missing";
        root["counts"] = json::object();
        root["next_actions"] = json::array({"source.build_crg_graph --execute"});
        root["checks"].push_back({{"check", "graph_db_exists"}, {"result", "warning"}, {"detail", "missing: " + graph_db_path}});
        root["warnings"].push_back("Saved/graph.db is missing; run source.build_crg_graph --execute");
        return root;
    }

    Database graph;
    std::string open_error;
    if (!try_open_readonly_database(graph, graph_db_path, open_error)) {
        json busy = graph_busy_json("source.crg_graph_health", graph_db_path, open_error);
        busy["input"] = root["input"];
        busy["checks"] = json::array({
            {{"check", "graph_db_readable"}, {"result", looks_like_graph_busy_error(open_error) ? "busy" : "warning"}, {"detail", open_error}}
        });
        if (!looks_like_graph_busy_error(open_error)) busy["status"] = "warning";
        return busy;
    }
    auto add_check = [&](const std::string& name, bool ok, const std::string& detail) {
        root["checks"].push_back({{"check", name}, {"result", ok ? "ok" : "warning"}, {"detail", detail}});
        if (!ok) root["warnings"].push_back(detail);
    };

    add_check("table:nodes", object_exists(graph, "table", "nodes"), "table nodes present");
    add_check("table:edges", object_exists(graph, "table", "edges"), "table edges present");
    add_check("table:metadata", object_exists(graph, "table", "metadata"), "table metadata present");
    add_check("fts:nodes_fts", object_exists(graph, "table", "nodes_fts"), "FTS table nodes_fts present");
    json counts = crg_graph_counts(graph);
    if (counts["nodes"].get<int64_t>() >= 0 && counts["fts_rows"].get<int64_t>() >= 0) {
        add_check("fts:row_parity", counts["nodes"] == counts["fts_rows"],
                  "nodes=" + std::to_string(counts["nodes"].get<int64_t>()) +
                  " nodes_fts=" + std::to_string(counts["fts_rows"].get<int64_t>()));
    }

    std::string schema_version = scalar_str(graph, "SELECT value FROM metadata WHERE key = 'schema_version';");
    add_check("metadata:schema_version", schema_version == "9",
              schema_version.empty() ? "metadata.schema_version missing" : "schema_version=" + schema_version + " (expected 9)");
    root["counts"] = counts;
    root["auxiliary_tables"] = {
        {"population_status", "reserved_unimplemented"},
        {"detail", "flow, community, and graph risk-index tables are schema-reserved and are not populated by the current builder"},
        {"counts", crg_graph_auxiliary_counts(graph)},
    };
    root["status"] = root["warnings"].empty() ? "ok" : "warning";
    root["summary"] = root["warnings"].empty()
        ? "CRG graph database schema, FTS, and metadata OK"
        : std::to_string(root["warnings"].size()) + " CRG graph warning(s)";
    root["next_actions"] = root["warnings"].empty()
        ? json::array({"source.search_crg_graph", "source.review_context"})
        : json::array({"source.build_crg_graph --execute", "source.crg_graph_health"});
    return root;
}

static json source_graph_build_counts(Database& source_db) {
    json counts = json::object();
    counts["files"] = count_rows(source_db, "SELECT COUNT(*) FROM files;");
    counts["symbols"] = count_rows(source_db, "SELECT COUNT(*) FROM symbols;");
    counts["valid_references"] = count_rows(source_db,
        "SELECT COUNT(*) FROM \"references\" r "
        "JOIN symbols fs ON fs.id = r.from_symbol_id "
        "JOIN symbols ts ON ts.id = r.to_symbol_id;");
    counts["inheritance"] = count_rows(source_db,
        "SELECT COUNT(*) FROM inheritance i "
        "JOIN symbols cs ON cs.id = i.child_id "
        "JOIN symbols ps ON ps.id = i.parent_id;");
    return counts;
}

static int64_t json_count_value(const json& counts, const std::string& key) {
    if (!counts.is_object() || !counts.contains(key) || !counts[key].is_number_integer()) return -1;
    return counts[key].get<int64_t>();
}

static std::string source_graph_signature(const json& source_counts) {
    std::ostringstream out;
    out << "v1"
        << ":files=" << json_count_value(source_counts, "files")
        << ":symbols=" << json_count_value(source_counts, "symbols")
        << ":valid_references=" << json_count_value(source_counts, "valid_references")
        << ":inheritance=" << json_count_value(source_counts, "inheritance");
    return out.str();
}

static bool graph_metadata_matches_source(Database& graph,
                                          const json& source_counts,
                                          json& metadata) {
    metadata = json::object();
    if (!has_crg_graph_tables(graph)) return false;

    Rows rows = query(graph,
        "SELECT key,value FROM metadata WHERE key IN ("
        "'schema_version','source_signature_version','source_files','source_symbols',"
        "'source_valid_references','source_inheritance','source_signature'"
        ");");
    for (const auto& row : rows) {
        metadata[row.get("key")] = row.get("value");
    }

    auto equals_count = [&](const char* metadata_key, const char* count_key) {
        return metadata.value(metadata_key, std::string()) == std::to_string(json_count_value(source_counts, count_key));
    };

    return metadata.value("schema_version", std::string()) == "9"
        && metadata.value("source_signature_version", std::string()) == "1"
        && equals_count("source_files", "files")
        && equals_count("source_symbols", "symbols")
        && equals_count("source_valid_references", "valid_references")
        && equals_count("source_inheritance", "inheritance")
        && metadata.value("source_signature", std::string()) == source_graph_signature(source_counts);
}

static std::vector<std::pair<std::string, std::string>> crg_graph_build_sql(const std::string& source_db_path,
                                                                            const json& source_counts) {
    return {
        {"clear risk index", "DELETE FROM risk_index;"},
        {"clear flow memberships", "DELETE FROM flow_memberships;"},
        {"clear flows", "DELETE FROM flows;"},
        {"clear community summaries", "DELETE FROM community_summaries;"},
        {"clear communities", "DELETE FROM communities;"},
        {"clear flow snapshots", "DELETE FROM flow_snapshots;"},
        {"clear edges", "DELETE FROM edges;"},
        {"clear nodes", "DELETE FROM nodes;"},
        {"clear metadata", "DELETE FROM metadata;"},
        {"recreate fts", "CREATE VIRTUAL TABLE IF NOT EXISTS nodes_fts USING fts5("
            "name, qualified_name, file_path, signature,"
            "content='nodes', content_rowid='id', tokenize='porter unicode61'"
            ");"},
        {"file nodes", R"SQL(
INSERT INTO nodes(id,kind,name,qualified_name,file_path,line_start,line_end,language,parent_name,params,return_type,modifiers,is_test,file_hash,extra,signature,community_id,updated_at)
SELECT -f.id,
       'File',
       COALESCE(NULLIF(f.path,''), 'file#' || f.id),
       COALESCE(NULLIF(f.path,''), 'file#' || f.id),
       COALESCE(f.path,''),
       1,
       COALESCE(f.line_count,0),
       CASE
         WHEN lower(COALESCE(f.file_type,'')) IN ('header','hpp','h') THEN 'cpp'
         WHEN lower(COALESCE(f.file_type,'')) IN ('cpp','cc','cxx') THEN 'cpp'
         WHEN lower(COALESCE(f.file_type,'')) LIKE '%shader%' THEN 'shader'
         ELSE COALESCE(NULLIF(f.file_type,''),'cpp')
       END,
       NULL,NULL,NULL,NULL,0,'',
       printf('{"source":"monolith","file_id":%lld,"module":"%s"}', f.id, replace(COALESCE(m.name,''), '"', '')),
       NULL,NULL,CAST(strftime('%s','now') AS REAL)
FROM src.files f
LEFT JOIN src.modules m ON m.id = f.module_id;
)SQL"},
        {"symbol nodes", R"SQL(
INSERT INTO nodes(id,kind,name,qualified_name,file_path,line_start,line_end,language,parent_name,params,return_type,modifiers,is_test,file_hash,extra,signature,community_id,updated_at)
SELECT s.id,
       CASE
         WHEN lower(COALESCE(s.kind,'')) IN ('class','struct','interface') THEN 'Class'
         WHEN lower(COALESCE(s.kind,'')) IN ('function','method','constructor','destructor') THEN
           CASE WHEN lower(COALESCE(s.name,'') || ' ' || COALESCE(f.path,'')) LIKE '%test%' THEN 'Test' ELSE 'Function' END
         WHEN lower(COALESCE(s.kind,'')) IN ('enum','typedef','type','delegate') THEN 'Type'
         ELSE COALESCE(NULLIF(s.kind,''),'Symbol')
       END,
       COALESCE(NULLIF(s.name,''), 'symbol#' || s.id),
       COALESCE(NULLIF(s.qualified_name,''), COALESCE(f.path,'') || '::' || COALESCE(s.name,'symbol')) || '#' || s.id,
       COALESCE(f.path,''),
       COALESCE(s.line_start,0),
       COALESCE(s.line_end,0),
       CASE
         WHEN lower(COALESCE(f.file_type,'')) LIKE '%shader%' THEN 'shader'
         ELSE 'cpp'
       END,
       p.name,
       NULL,
       NULL,
       s.access,
       CASE WHEN lower(COALESCE(s.name,'') || ' ' || COALESCE(f.path,'')) LIKE '%test%' THEN 1 ELSE 0 END,
       '',
       printf('{"source":"monolith","symbol_id":%lld,"kind":"%s","module":"%s","is_ue_macro":%d}',
              s.id, replace(COALESCE(s.kind,''), '"', ''), replace(COALESCE(m.name,''), '"', ''), COALESCE(s.is_ue_macro,0)),
       s.signature,
       NULL,
       CAST(strftime('%s','now') AS REAL)
FROM src.symbols s
LEFT JOIN src.files f ON f.id = s.file_id
LEFT JOIN src.modules m ON m.id = f.module_id
LEFT JOIN src.symbols p ON p.id = s.parent_symbol_id;
)SQL"},
        {"contains edges", R"SQL(
INSERT INTO edges(kind,source_qualified,target_qualified,file_path,line,extra,confidence,confidence_tier,updated_at)
SELECT 'CONTAINS',
       COALESCE(f.path,''),
       COALESCE(NULLIF(s.qualified_name,''), COALESCE(f.path,'') || '::' || COALESCE(s.name,'symbol')) || '#' || s.id,
       COALESCE(f.path,''),
       COALESCE(s.line_start,0),
       printf('{"source":"monolith","file_id":%lld,"symbol_id":%lld}', f.id, s.id),
       1.0,
       'EXTRACTED',
       CAST(strftime('%s','now') AS REAL)
FROM src.symbols s
JOIN src.files f ON f.id = s.file_id;
)SQL"},
        {"reference edges", R"SQL(
INSERT INTO edges(kind,source_qualified,target_qualified,file_path,line,extra,confidence,confidence_tier,updated_at)
SELECT CASE
         WHEN lower(COALESCE(r.ref_kind,'')) IN ('call','calls','function_call') THEN 'CALLS'
         WHEN lower(COALESCE(r.ref_kind,'')) IN ('type','type_ref','reference') THEN 'REFERENCES'
         ELSE upper(COALESCE(NULLIF(r.ref_kind,''),'REFERENCES'))
       END,
       COALESCE(NULLIF(fs.qualified_name,''), COALESCE(ff.path,'') || '::' || COALESCE(fs.name,'symbol')) || '#' || fs.id,
       COALESCE(NULLIF(ts.qualified_name,''), COALESCE(tf.path,'') || '::' || COALESCE(ts.name,'symbol')) || '#' || ts.id,
       COALESCE(rf.path, ff.path, ''),
       COALESCE(r.line,0),
       printf('{"source":"monolith","reference_id":%lld,"ref_kind":"%s"}', r.id, replace(COALESCE(r.ref_kind,''), '"', '')),
       1.0,
       'EXTRACTED',
       CAST(strftime('%s','now') AS REAL)
FROM src."references" r
JOIN src.symbols fs ON fs.id = r.from_symbol_id
JOIN src.symbols ts ON ts.id = r.to_symbol_id
LEFT JOIN src.files ff ON ff.id = fs.file_id
LEFT JOIN src.files tf ON tf.id = ts.file_id
LEFT JOIN src.files rf ON rf.id = r.file_id;
)SQL"},
        {"inheritance edges", R"SQL(
INSERT INTO edges(kind,source_qualified,target_qualified,file_path,line,extra,confidence,confidence_tier,updated_at)
SELECT 'INHERITS',
       COALESCE(NULLIF(cs.qualified_name,''), COALESCE(cf.path,'') || '::' || COALESCE(cs.name,'symbol')) || '#' || cs.id,
       COALESCE(NULLIF(ps.qualified_name,''), COALESCE(pf.path,'') || '::' || COALESCE(ps.name,'symbol')) || '#' || ps.id,
       COALESCE(cf.path,''),
       COALESCE(cs.line_start,0),
       printf('{"source":"monolith","inheritance_id":%lld}', i.id),
       1.0,
       'EXTRACTED',
       CAST(strftime('%s','now') AS REAL)
FROM src.inheritance i
JOIN src.symbols cs ON cs.id = i.child_id
JOIN src.symbols ps ON ps.id = i.parent_id
LEFT JOIN src.files cf ON cf.id = cs.file_id
LEFT JOIN src.files pf ON pf.id = ps.file_id;
)SQL"},
        {"metadata schema", "INSERT OR REPLACE INTO metadata(key,value) VALUES('schema_version','9');"},
        {"metadata source", "INSERT OR REPLACE INTO metadata(key,value) VALUES('source','monolith');"},
        {"metadata source db", "INSERT OR REPLACE INTO metadata(key,value) VALUES('monolith_source_db'," + sql_quote(source_db_path) + ");"},
        {"metadata built_at", "INSERT OR REPLACE INTO metadata(key,value) VALUES('built_at',datetime('now'));"},
        {"metadata builder", "INSERT OR REPLACE INTO metadata(key,value) VALUES('builder','monolith_query source build_crg_graph');"},
        {"metadata source signature version", "INSERT OR REPLACE INTO metadata(key,value) VALUES('source_signature_version','1');"},
        {"metadata source files", "INSERT OR REPLACE INTO metadata(key,value) VALUES('source_files','" + std::to_string(json_count_value(source_counts, "files")) + "');"},
        {"metadata source symbols", "INSERT OR REPLACE INTO metadata(key,value) VALUES('source_symbols','" + std::to_string(json_count_value(source_counts, "symbols")) + "');"},
        {"metadata source valid references", "INSERT OR REPLACE INTO metadata(key,value) VALUES('source_valid_references','" + std::to_string(json_count_value(source_counts, "valid_references")) + "');"},
        {"metadata source inheritance", "INSERT OR REPLACE INTO metadata(key,value) VALUES('source_inheritance','" + std::to_string(json_count_value(source_counts, "inheritance")) + "');"},
        {"metadata source signature", "INSERT OR REPLACE INTO metadata(key,value) VALUES('source_signature'," + sql_quote(source_graph_signature(source_counts)) + ");"},
        {"metadata auxiliary tables", "INSERT OR REPLACE INTO metadata(key,value) VALUES('auxiliary_tables','reserved_unimplemented:flows,flow_memberships,flow_snapshots,communities,community_summaries,risk_index');"},
        {"rebuild fts", "INSERT INTO nodes_fts(nodes_fts) VALUES('rebuild');"},
    };
}

static json crg_repair_counts(Database& db, const std::string& domain) {
    json counts = json::object();
    if (domain == "source") {
        counts["symbols"] = count_rows(db, "SELECT COUNT(*) FROM symbols;");
        counts["references"] = count_rows(db, "SELECT COUNT(*) FROM \"references\";");
        counts["inheritance"] = count_rows(db, "SELECT COUNT(*) FROM inheritance;");
    } else {
        counts["assets"] = count_rows(db, "SELECT COUNT(*) FROM assets;");
        counts["dependencies"] = count_rows(db, "SELECT COUNT(*) FROM dependencies;");
    }
    if (has_crg_projection_tables(db)) {
        counts["crg_nodes"] = count_rows(db, "SELECT COUNT(*) FROM crg_nodes WHERE domain = '" + domain + "';");
        counts["crg_edges"] = count_rows(db, "SELECT COUNT(*) FROM crg_edges WHERE domain = '" + domain + "';");
        counts["crg_node_metrics"] = count_rows(db,
            "SELECT COUNT(*) FROM crg_node_metrics m JOIN crg_nodes n ON n.id = m.node_id "
            "WHERE n.domain = '" + domain + "';");
        if (domain == "source" && object_exists(db, "table", "source_override_edges")) {
            counts["source_override_edges"] = count_rows(db, "SELECT COUNT(*) FROM source_override_edges;");
        }
    }
    return counts;
}

static std::vector<std::pair<std::string, std::string>> crg_repair_sql(const std::string& domain) {
    if (domain == "source") {
        return {
            {"clear orphan metrics", "DELETE FROM crg_node_metrics WHERE node_id NOT IN (SELECT id FROM crg_nodes);"},
            {"clear metrics", "DELETE FROM crg_node_metrics WHERE node_id IN (SELECT id FROM crg_nodes WHERE domain = 'source');"},
            {"clear edges", "DELETE FROM crg_edges WHERE domain = 'source';"},
            {"clear nodes", "DELETE FROM crg_nodes WHERE domain = 'source';"},
            {"clear source override edges", "DELETE FROM source_override_edges;"},
            {"source nodes", R"SQL(
INSERT INTO crg_nodes(id,domain,native_table,native_id,stable_key,kind,name,path,module,source_revision,extra,updated_at)
SELECT s.id,'source','symbols',s.id,COALESCE(s.qualified_name,s.name) || '#' || s.id,
s.kind,s.name,COALESCE(f.path,''),COALESCE(m.name,''),'','{}',CAST(strftime('%s','now') AS INTEGER)
FROM symbols s
LEFT JOIN files f ON f.id = s.file_id
LEFT JOIN modules m ON m.id = f.module_id;
)SQL"},
            {"source reference edges", R"SQL(
INSERT INTO crg_edges(domain,source_node_id,target_node_id,edge_kind,edge_subkind,weight,native_table,native_id,updated_at)
SELECT 'source',r.from_symbol_id,r.to_symbol_id,COALESCE(r.ref_kind,'reference'),'reference',1.0,'references',r.id,CAST(strftime('%s','now') AS INTEGER)
FROM "references" r
JOIN symbols fs ON fs.id = r.from_symbol_id
JOIN symbols ts ON ts.id = r.to_symbol_id;
)SQL"},
            {"source inheritance edges", R"SQL(
INSERT INTO crg_edges(domain,source_node_id,target_node_id,edge_kind,edge_subkind,weight,native_table,native_id,updated_at)
SELECT 'source',i.child_id,i.parent_id,'inheritance','extends',1.0,'inheritance',i.id,CAST(strftime('%s','now') AS INTEGER)
FROM inheritance i
JOIN symbols cs ON cs.id = i.child_id
JOIN symbols ps ON ps.id = i.parent_id;
)SQL"},
            {"source metrics", R"SQL(
WITH ref_in AS (
   SELECT to_symbol_id AS symbol_id, COUNT(*) AS fan_in, COUNT(DISTINCT r.file_id) AS caller_files
   FROM "references" r
   JOIN symbols fs ON fs.id = r.from_symbol_id
   JOIN symbols ts ON ts.id = r.to_symbol_id
   GROUP BY to_symbol_id
 ), ref_out AS (
   SELECT from_symbol_id AS symbol_id, COUNT(*) AS fan_out
   FROM "references" r
   JOIN symbols fs ON fs.id = r.from_symbol_id
   JOIN symbols ts ON ts.id = r.to_symbol_id
   GROUP BY from_symbol_id
 ), inh_desc AS (
   SELECT parent_id AS symbol_id, COUNT(*) AS descendants
   FROM inheritance i JOIN symbols cs ON cs.id = i.child_id JOIN symbols ps ON ps.id = i.parent_id GROUP BY parent_id
 ), inh_anc AS (
   SELECT child_id AS symbol_id, COUNT(*) AS ancestors
   FROM inheritance i JOIN symbols cs ON cs.id = i.child_id JOIN symbols ps ON ps.id = i.parent_id GROUP BY child_id
 ), counts AS (
 SELECT s.id AS native_id,
        COALESCE(ri.fan_in,0) AS fan_in,
        COALESCE(ro.fan_out,0) AS fan_out,
        COALESCE(id.descendants,0) AS descendants,
        COALESCE(ia.ancestors,0) AS ancestors,
        COALESCE(ri.caller_files,0) AS caller_files,
        s.is_ue_macro AS is_ue_macro,
        CASE
          WHEN lower(COALESCE(s.qualified_name,'') || ' ' || COALESCE(s.name,'') || ' ' || COALESCE(s.signature,'')) LIKE '%ufunction%'
            OR lower(COALESCE(s.qualified_name,'') || ' ' || COALESCE(s.name,'') || ' ' || COALESCE(s.signature,'')) LIKE '%server%'
            OR lower(COALESCE(s.qualified_name,'') || ' ' || COALESCE(s.name,'') || ' ' || COALESCE(s.signature,'')) LIKE '%client%'
            OR lower(COALESCE(s.qualified_name,'') || ' ' || COALESCE(s.name,'') || ' ' || COALESCE(s.signature,'')) LIKE '%netmulticast%'
            OR lower(COALESCE(s.qualified_name,'') || ' ' || COALESCE(s.name,'') || ' ' || COALESCE(s.signature,'')) LIKE '%onrep%'
            OR lower(COALESCE(s.qualified_name,'') || ' ' || COALESCE(s.name,'') || ' ' || COALESCE(s.signature,'')) LIKE '%replication%'
            OR lower(COALESCE(s.qualified_name,'') || ' ' || COALESCE(s.name,'') || ' ' || COALESCE(s.signature,'')) LIKE '%rpc%'
            OR lower(COALESCE(s.qualified_name,'') || ' ' || COALESCE(s.name,'') || ' ' || COALESCE(s.signature,'')) LIKE '%network%'
            OR lower(COALESCE(s.qualified_name,'') || ' ' || COALESCE(s.name,'') || ' ' || COALESCE(s.signature,'')) LIKE '%save%'
            OR lower(COALESCE(s.qualified_name,'') || ' ' || COALESCE(s.name,'') || ' ' || COALESCE(s.signature,'')) LIKE '%serialize%'
            OR lower(COALESCE(s.qualified_name,'') || ' ' || COALESCE(s.name,'') || ' ' || COALESCE(s.signature,'')) LIKE '%archive%'
            OR lower(COALESCE(s.qualified_name,'') || ' ' || COALESCE(s.name,'') || ' ' || COALESCE(s.signature,'')) LIKE '%auth%'
            OR lower(COALESCE(s.qualified_name,'') || ' ' || COALESCE(s.name,'') || ' ' || COALESCE(s.signature,'')) LIKE '%login%'
            OR lower(COALESCE(s.qualified_name,'') || ' ' || COALESCE(s.name,'') || ' ' || COALESCE(s.signature,'')) LIKE '%account%'
            OR lower(COALESCE(s.qualified_name,'') || ' ' || COALESCE(s.name,'') || ' ' || COALESCE(s.signature,'')) LIKE '%session%'
            OR lower(COALESCE(s.qualified_name,'') || ' ' || COALESCE(s.name,'') || ' ' || COALESCE(s.signature,'')) LIKE '%purchase%'
            OR lower(COALESCE(s.qualified_name,'') || ' ' || COALESCE(s.name,'') || ' ' || COALESCE(s.signature,'')) LIKE '%iap%'
            OR lower(COALESCE(s.qualified_name,'') || ' ' || COALESCE(s.name,'') || ' ' || COALESCE(s.signature,'')) LIKE '%store%'
            OR lower(COALESCE(s.qualified_name,'') || ' ' || COALESCE(s.name,'') || ' ' || COALESCE(s.signature,'')) LIKE '%entitlement%'
            OR lower(COALESCE(s.qualified_name,'') || ' ' || COALESCE(s.name,'') || ' ' || COALESCE(s.signature,'')) LIKE '%anticheat%'
            OR lower(COALESCE(s.qualified_name,'') || ' ' || COALESCE(s.name,'') || ' ' || COALESCE(s.signature,'')) LIKE '%crypt%'
            OR lower(COALESCE(s.qualified_name,'') || ' ' || COALESCE(s.name,'') || ' ' || COALESCE(s.signature,'')) LIKE '%encrypt%'
            OR lower(COALESCE(s.qualified_name,'') || ' ' || COALESCE(s.name,'') || ' ' || COALESCE(s.signature,'')) LIKE '%decrypt%'
            OR lower(COALESCE(s.qualified_name,'') || ' ' || COALESCE(s.name,'') || ' ' || COALESCE(s.signature,'')) LIKE '%signature%'
            OR lower(COALESCE(s.qualified_name,'') || ' ' || COALESCE(s.name,'') || ' ' || COALESCE(s.signature,'')) LIKE '%signed%'
            OR lower(COALESCE(s.qualified_name,'') || ' ' || COALESCE(s.name,'') || ' ' || COALESCE(s.signature,'')) LIKE '%signing%'
            OR lower(COALESCE(s.qualified_name,'') || ' ' || COALESCE(s.name,'') || ' ' || COALESCE(s.signature,'')) LIKE '% sign %'
            OR lower(COALESCE(s.qualified_name,'') || ' ' || COALESCE(s.name,'') || ' ' || COALESCE(s.signature,'')) LIKE '% sign'
            OR lower(COALESCE(s.qualified_name,'') || ' ' || COALESCE(s.name,'') || ' ' || COALESCE(s.signature,'')) LIKE '%hash%'
            OR lower(COALESCE(s.qualified_name,'') || ' ' || COALESCE(s.name,'') || ' ' || COALESCE(s.signature,'')) LIKE '%exec%'
            OR lower(COALESCE(s.qualified_name,'') || ' ' || COALESCE(s.name,'') || ' ' || COALESCE(s.signature,'')) LIKE '%eval%'
            OR lower(COALESCE(s.qualified_name,'') || ' ' || COALESCE(s.name,'') || ' ' || COALESCE(s.signature,'')) LIKE '%command%'
            OR lower(COALESCE(s.qualified_name,'') || ' ' || COALESCE(s.name,'') || ' ' || COALESCE(s.signature,'')) LIKE '%file%'
            OR lower(COALESCE(s.qualified_name,'') || ' ' || COALESCE(s.name,'') || ' ' || COALESCE(s.signature,'')) LIKE '%registry%'
            OR lower(COALESCE(s.qualified_name,'') || ' ' || COALESCE(s.name,'') || ' ' || COALESCE(s.signature,'')) LIKE '%process%'
          THEN 1 ELSE 0 END AS sensitivity
 FROM symbols s
 LEFT JOIN ref_in ri ON ri.symbol_id = s.id
 LEFT JOIN ref_out ro ON ro.symbol_id = s.id
 LEFT JOIN inh_desc id ON id.symbol_id = s.id
 LEFT JOIN inh_anc ia ON ia.symbol_id = s.id
), scored AS (
 SELECT c.*, MIN(1.0,
        MIN(c.fan_in,50) / 50.0 * 0.35 +
        MIN(c.descendants,30) / 30.0 * 0.25 +
        MIN(c.fan_out,50) / 50.0 * 0.10 +
        CASE WHEN c.is_ue_macro != 0 THEN 0.15 ELSE 0.0 END +
        MIN(c.caller_files,20) / 20.0 * 0.15 +
        CASE WHEN c.sensitivity != 0 THEN 0.15 ELSE 0.0 END) AS score
 FROM counts c
)
INSERT INTO crg_node_metrics(node_id,fan_in,fan_out,hard_in,descendants,risk_score,risk_tier,reasons_json,raw_counts_json,scoring_version,computed_at)
SELECT s.native_id,s.fan_in,s.fan_out,0,s.descendants,ROUND(s.score,3),
       CASE WHEN s.score >= 0.66 THEN 'high' WHEN s.score >= 0.33 THEN 'medium' ELSE 'low' END,
       CASE WHEN s.sensitivity != 0 THEN
         printf('["caller fan-in: %d","inheritance descendants (1-hop): %d","callee fan-out: %d","module/file boundary crossing: %d distinct caller file(s)","sensitivity: UE-domain sensitive surface (token-boundary matched)"]',
                s.fan_in,s.descendants,s.fan_out,s.caller_files)
       ELSE
         printf('["caller fan-in: %d","inheritance descendants (1-hop): %d","callee fan-out: %d","module/file boundary crossing: %d distinct caller file(s)"]',
                s.fan_in,s.descendants,s.fan_out,s.caller_files)
       END,
       printf('{"callers":%d,"callees":%d,"descendants":%d,"ancestors":%d,"caller_files":%d,"is_ue_macro":%d,"sensitivity":%d}',
              s.fan_in,s.fan_out,s.descendants,s.ancestors,s.caller_files,s.is_ue_macro,s.sensitivity),
       '3',CAST(strftime('%s','now') AS INTEGER)
FROM scored s;
)SQL"},
            {"cache_version", "INSERT OR REPLACE INTO crg_meta(key,value) VALUES('cache_version','1');"},
            {"scoring_version", "INSERT OR REPLACE INTO crg_meta(key,value) VALUES('scoring_version','3');"},
            {"built_at", "INSERT OR REPLACE INTO crg_meta(key,value) VALUES('built_at',datetime('now'));"},
            {"source_built_at", "INSERT OR REPLACE INTO crg_meta(key,value) VALUES('source_built_at',datetime('now'));"},
        };
    }

    return {
        {"clear orphan metrics", "DELETE FROM crg_node_metrics WHERE node_id NOT IN (SELECT id FROM crg_nodes);"},
        {"clear metrics", "DELETE FROM crg_node_metrics WHERE node_id IN (SELECT id FROM crg_nodes WHERE domain = 'project');"},
        {"clear edges", "DELETE FROM crg_edges WHERE domain = 'project';"},
        {"clear nodes", "DELETE FROM crg_nodes WHERE domain = 'project';"},
        {"project nodes", R"SQL(
INSERT INTO crg_nodes(domain,native_table,native_id,stable_key,kind,name,path,module,source_revision,extra,updated_at)
SELECT 'project','assets',a.id,a.package_path,a.asset_class,a.asset_name,a.package_path,a.module_name,COALESCE(a.saved_hash,''),'{}',
CAST(strftime('%s','now') AS INTEGER)
FROM assets a;
)SQL"},
        {"project edges", R"SQL(
INSERT INTO crg_edges(domain,source_node_id,target_node_id,edge_kind,edge_subkind,weight,native_table,native_id,updated_at)
SELECT 'project',sn.id,tn.id,'dependency',COALESCE(d.dependency_type,''),
CASE WHEN d.dependency_type = 'Hard' THEN 1.0 ELSE 0.5 END,
'dependencies',d.id,CAST(strftime('%s','now') AS INTEGER)
FROM dependencies d
JOIN crg_nodes sn ON sn.domain='project' AND sn.native_table='assets' AND sn.native_id=d.source_asset_id
JOIN crg_nodes tn ON tn.domain='project' AND tn.native_table='assets' AND tn.native_id=d.target_asset_id;
)SQL"},
        {"project metrics", R"SQL(
WITH counts AS (
 SELECT a.id AS native_id,
        (SELECT COUNT(*) FROM dependencies d WHERE d.target_asset_id = a.id) AS fan_in,
        (SELECT COUNT(*) FROM dependencies d WHERE d.source_asset_id = a.id) AS fan_out,
        (SELECT COUNT(*) FROM dependencies d WHERE d.target_asset_id = a.id AND d.dependency_type = 'Hard') AS hard_in,
        (SELECT COUNT(*) FROM nodes x WHERE x.asset_id = a.id) AS node_count,
        (SELECT COUNT(*) FROM variables x WHERE x.asset_id = a.id) AS var_count,
        (SELECT COUNT(*) FROM parameters x WHERE x.asset_id = a.id) AS param_count,
        (SELECT COUNT(*) FROM tag_references x WHERE x.asset_id = a.id) AS tag_refs,
        CASE
          WHEN a.asset_class LIKE '%World%' OR a.asset_class LIKE '%Level%' THEN 5
          WHEN a.asset_class LIKE '%GameplayAbility%' OR a.asset_class LIKE '%AttributeSet%' OR a.asset_class LIKE '%GameplayEffect%' THEN 4
          WHEN a.asset_class LIKE '%Blueprint%' THEN 3
          WHEN a.asset_class LIKE '%NiagaraSystem%' OR a.asset_class LIKE '%Material%' THEN 2
          WHEN a.asset_class LIKE '%DataTable%' OR a.asset_class LIKE '%DataAsset%' THEN 2
          ELSE 1
        END AS class_weight,
        CASE
          WHEN lower(a.asset_class || ' ' || a.package_path || ' ' || a.asset_name) LIKE '%network%'
            OR lower(a.asset_class || ' ' || a.package_path || ' ' || a.asset_name) LIKE '%replication%'
            OR lower(a.asset_class || ' ' || a.package_path || ' ' || a.asset_name) LIKE '%rpc%'
            OR lower(a.asset_class || ' ' || a.package_path || ' ' || a.asset_name) LIKE '%netmulticast%'
            OR lower(a.asset_class || ' ' || a.package_path || ' ' || a.asset_name) LIKE '%onrep%'
            OR lower(a.asset_class || ' ' || a.package_path || ' ' || a.asset_name) LIKE '%save%'
            OR lower(a.asset_class || ' ' || a.package_path || ' ' || a.asset_name) LIKE '%serialize%'
            OR lower(a.asset_class || ' ' || a.package_path || ' ' || a.asset_name) LIKE '%archive%'
            OR lower(a.asset_class || ' ' || a.package_path || ' ' || a.asset_name) LIKE '%auth%'
            OR lower(a.asset_class || ' ' || a.package_path || ' ' || a.asset_name) LIKE '%login%'
            OR lower(a.asset_class || ' ' || a.package_path || ' ' || a.asset_name) LIKE '%account%'
            OR lower(a.asset_class || ' ' || a.package_path || ' ' || a.asset_name) LIKE '%session%'
            OR lower(a.asset_class || ' ' || a.package_path || ' ' || a.asset_name) LIKE '%purchase%'
            OR lower(a.asset_class || ' ' || a.package_path || ' ' || a.asset_name) LIKE '%store%'
            OR lower(a.asset_class || ' ' || a.package_path || ' ' || a.asset_name) LIKE '%entitlement%'
            OR lower(a.asset_class || ' ' || a.package_path || ' ' || a.asset_name) LIKE '%anticheat%'
            OR lower(a.asset_class || ' ' || a.package_path || ' ' || a.asset_name) LIKE '%crypt%'
            OR lower(a.asset_class || ' ' || a.package_path || ' ' || a.asset_name) LIKE '%encrypt%'
            OR lower(a.asset_class || ' ' || a.package_path || ' ' || a.asset_name) LIKE '%decrypt%'
            OR lower(a.asset_class || ' ' || a.package_path || ' ' || a.asset_name) LIKE '%signature%'
            OR lower(a.asset_class || ' ' || a.package_path || ' ' || a.asset_name) LIKE '%signed%'
            OR lower(a.asset_class || ' ' || a.package_path || ' ' || a.asset_name) LIKE '%signing%'
            OR lower(a.asset_class || ' ' || a.package_path || ' ' || a.asset_name) LIKE '% sign %'
            OR lower(a.asset_class || ' ' || a.package_path || ' ' || a.asset_name) LIKE '% sign'
            OR lower(a.asset_class || ' ' || a.package_path || ' ' || a.asset_name) LIKE '%/sign/%'
            OR lower(a.asset_class || ' ' || a.package_path || ' ' || a.asset_name) LIKE '%hash%'
            OR lower(a.asset_class || ' ' || a.package_path || ' ' || a.asset_name) LIKE '%exec%'
            OR lower(a.asset_class || ' ' || a.package_path || ' ' || a.asset_name) LIKE '%eval%'
            OR lower(a.asset_class || ' ' || a.package_path || ' ' || a.asset_name) LIKE '%command%'
            OR lower(a.asset_class || ' ' || a.package_path || ' ' || a.asset_name) LIKE '%file%'
            OR lower(a.asset_class || ' ' || a.package_path || ' ' || a.asset_name) LIKE '%registry%'
            OR lower(a.asset_class || ' ' || a.package_path || ' ' || a.asset_name) LIKE '%process%'
          THEN 1 ELSE 0 END AS sensitivity
 FROM assets a
), scored AS (
 SELECT c.*, MIN(1.0,
        MIN(c.fan_in,30) / 30.0 * 0.30 +
        MIN(c.hard_in,20) / 20.0 * 0.20 +
        MIN(c.fan_out,30) / 30.0 * 0.10 +
        (c.class_weight - 1) / 4.0 * 0.20 +
        CASE WHEN c.sensitivity != 0 THEN 0.15 ELSE 0.0 END +
        MIN(c.node_count,400) / 400.0 * 0.15 +
        MIN(c.tag_refs,20) / 20.0 * 0.05) AS score
 FROM counts c
)
INSERT INTO crg_node_metrics(node_id,fan_in,fan_out,hard_in,descendants,risk_score,risk_tier,reasons_json,raw_counts_json,scoring_version,computed_at)
SELECT n.id,s.fan_in,s.fan_out,s.hard_in,0,ROUND(s.score,3),
       CASE WHEN s.score >= 0.66 THEN 'high' WHEN s.score >= 0.33 THEN 'medium' ELSE 'low' END,
       CASE WHEN s.sensitivity != 0 THEN
         printf('["inbound dependency fan-in: %d referencer(s)","hard inbound dependencies: %d","outbound dependencies: %d","sensitivity: UE-domain sensitive surface (token-boundary matched)","graph density: %d node(s)"]',
                s.fan_in,s.hard_in,s.fan_out,s.node_count)
       ELSE
         printf('["inbound dependency fan-in: %d referencer(s)","hard inbound dependencies: %d","outbound dependencies: %d","graph density: %d node(s)"]',
                s.fan_in,s.hard_in,s.fan_out,s.node_count)
       END,
       printf('{"inbound":%d,"inbound_hard":%d,"outbound":%d,"nodes":%d,"variables":%d,"parameters":%d,"tag_references":%d,"class_weight":%d,"sensitivity":%d}',
              s.fan_in,s.hard_in,s.fan_out,s.node_count,s.var_count,s.param_count,s.tag_refs,s.class_weight,s.sensitivity),
       '3',CAST(strftime('%s','now') AS INTEGER)
FROM scored s
JOIN crg_nodes n ON n.domain='project' AND n.native_table='assets' AND n.native_id=s.native_id;
)SQL"},
        {"cache_version", "INSERT OR REPLACE INTO crg_meta(key,value) VALUES('cache_version','1');"},
        {"scoring_version", "INSERT OR REPLACE INTO crg_meta(key,value) VALUES('scoring_version','3');"},
        {"built_at", "INSERT OR REPLACE INTO crg_meta(key,value) VALUES('built_at',datetime('now'));"},
        {"project_built_at", "INSERT OR REPLACE INTO crg_meta(key,value) VALUES('project_built_at',datetime('now'));"},
    };
}

struct CrgWriteProbeResult {
    bool success = false;
    bool transaction_started = false;
    std::string error;
};

static CrgWriteProbeResult crg_probe_write_access(Database& db, const std::string& domain,
                                                   bool keep_transaction) {
    CrgWriteProbeResult result;
    const int main_readonly = sqlite3_db_readonly(db.db, "main");
    if (main_readonly != 0) {
        result.error = main_readonly > 0
            ? "SQLite opened the main database read-only"
            : "SQLite could not resolve the main database write state";
        return result;
    }

    if (!exec_sql_ok(db, "BEGIN IMMEDIATE;", result.error)) return result;
    result.transaction_started = true;

    const char* write_probe = domain == "source"
        ? "DELETE FROM symbols WHERE 0;"
        : "DELETE FROM assets WHERE 0;";
    if (!exec_sql_ok(db, write_probe, result.error)) {
        std::string rollback_error;
        if (!exec_sql_ok(db, "ROLLBACK;", rollback_error) && !rollback_error.empty()) {
            result.error += "; rollback failed: " + rollback_error;
        }
        result.transaction_started = false;
        return result;
    }

    if (!keep_transaction) {
        if (!exec_sql_ok(db, "ROLLBACK;", result.error)) {
            result.transaction_started = false;
            return result;
        }
        result.transaction_started = false;
    }
    result.success = true;
    return result;
}

static json crg_repair_cache_json(Database& db, const std::string& domain,
                                  const std::string& scope, bool execute) {
    std::string normalized_scope = scope.empty() ? "all" : lower_copy(scope);
    json warnings = json::array();
    json plan = json::array();
    if (domain == "source" && normalized_scope == "override_edges") {
        plan = json::array({
            "CREATE IF MISSING source_override_edges and helper indexes",
            "DELETE existing source_override_edges rows",
            "Recompute signature-aware source_override_edges cache",
            "Update crg_meta.source_override_edges_version",
        });
    } else {
        plan = json::array({
            "CREATE IF MISSING crg_nodes/crg_edges/crg_node_metrics/crg_meta",
            domain == "source" ? "CREATE IF MISSING review/override helper indexes" : "USE existing project review indexes",
            "DELETE existing " + domain + " CRG projection rows",
            domain == "source"
                ? "SOURCE symbols -> crg_nodes; references/inheritance -> crg_edges"
                : "PROJECT assets -> crg_nodes; dependencies -> crg_edges",
            domain == "source"
                ? "Recompute caller/callee/descendant/risk_score into crg_node_metrics"
                : "Recompute fan-in/fan-out/hard-in/risk_score into crg_node_metrics",
        });
    }
    if (domain == "source" && normalized_scope != "override_edges") {
        plan.push_back("Recompute signature-aware source_override_edges cache");
    }
    json root = {
        {"input", {{"execute", execute}, {"scope", normalized_scope}}},
        {"limits", {{"execute", execute}, {"scope", normalized_scope}}},
        {"plan", plan},
        {"warnings", warnings},
        {"truncated", false},
    };
    if (normalized_scope != "all" && !(domain == "source" && normalized_scope == "override_edges")) {
        root["status"] = "error";
        root["summary"] = domain == "source"
            ? "Unsupported scope for repair_crg_cache (expected 'all' or 'override_edges')"
            : "Unsupported scope for repair_crg_cache (expected 'all')";
        root["next_actions"] = domain == "source"
            ? json::array({"source.repair_crg_cache --scope=all", "source.repair_crg_cache --scope=override_edges", "source.health"})
            : json::array({domain + ".repair_crg_cache --scope=all", domain + ".health"});
        return root;
    }

    const std::string retry_action = domain + ".repair_crg_cache --scope=" + normalized_scope + " --execute";
    const std::string write_remediation =
        "Close UnrealEditor, Monolith indexers, and any other process using the " + domain +
        " index database, then retry " + retry_action + ".";
    auto write_blocked_result = [&](const std::string& error) {
        const std::string detail = error.empty() ? write_remediation : error + ". " + write_remediation;
        root["write_preflight"] = {
            {"status", "unavailable"},
            {"error", error},
            {"remediation", write_remediation},
        };
        root["warnings"].push_back("CRG cache rebuild failed at database write preflight: " + detail);
        if (!root.contains("before")) root["before"] = json::object();
        if (!root.contains("freshness_checks")) root["freshness_checks"] = json::array();
        if (!root.contains("repair_needed")) root["repair_needed"] = nullptr;
        root["after"] = root["before"];
        root["status"] = "error";
        root["summary"] = domain + " CRG projection/cache rebuild blocked because database write access is unavailable";
        root["next_actions"] = json::array({retry_action, domain + ".health"});
        return root;
    };

    // Fail before multi-million-row freshness scans when another process has
    // opened the database without write sharing. This probe is rolled back and
    // therefore does not turn an already-fresh execute request into a mutation.
    if (execute) {
        const CrgWriteProbeResult early_probe = crg_probe_write_access(db, domain, false);
        if (!early_probe.success) return write_blocked_result(early_probe.error);
        root["write_preflight"] = {
            {"status", "ok"},
            {"probe", "transactional no-op write"},
        };
    }

    root["before"] = crg_repair_counts(db, domain);
    json freshness_checks = json::array();
    bool repair_needed = false;
    bool projection_corruption_detected = false;
    auto add_freshness_check = [&](const std::string& name, bool pass, const std::string& detail) {
        freshness_checks.push_back({{"check", name}, {"result", pass ? "ok" : "stale"}, {"detail", detail}});
        if (!pass) repair_needed = true;
    };
    if (domain == "source" && normalized_scope == "override_edges") {
        bool has_override_table = object_exists(db, "table", "source_override_edges");
        add_freshness_check("table:source_override_edges", has_override_table,
                            has_override_table ? "source_override_edges table present" : "source_override_edges table missing");
        bool has_parent_index = object_exists(db, "index", "idx_source_override_edges_parent");
        add_freshness_check("index:idx_source_override_edges_parent", has_parent_index,
                            has_parent_index ? "source override parent index present" : "source override parent index missing");
        bool has_signature_index = object_exists(db, "index", "idx_symbols_override_signature");
        add_freshness_check("index:idx_symbols_override_signature", has_signature_index,
                            has_signature_index ? "symbol override signature index present" : "symbol override signature index missing");
        std::string override_version = object_exists(db, "table", "crg_meta")
            ? scalar_str(db, "SELECT value FROM crg_meta WHERE key='source_override_edges_version';")
            : "";
        add_freshness_check("meta:source_override_edges_version", override_version == "2",
                            override_version.empty() ? "source_override_edges_version missing" : "source_override_edges_version=" + override_version + " (expected 2)");
    } else {
        bool has_core_crg = has_crg_projection_tables(db);
        add_freshness_check("tables:crg_core", has_core_crg,
                            has_core_crg ? "core CRG projection tables present" : "one or more core CRG projection tables are missing");
        if (has_core_crg) {
            if (domain == "source") {
                for (const char* index_name : {
                    "idx_crg_nodes_domain_native", "idx_crg_nodes_stable",
                    "idx_crg_edges_domain_source", "idx_crg_edges_domain_target",
                    "idx_crg_edges_kind_subkind", "idx_crg_metrics_score",
                    "idx_source_override_edges_parent",
                    "idx_symbols_override_signature",
                    "idx_inheritance_parent_child", "idx_inheritance_child_parent"}) {
                    bool has_index = object_exists(db, "index", index_name);
                    add_freshness_check(std::string("index:") + index_name, has_index,
                                        has_index ? std::string("index ") + index_name + " present" : std::string("index ") + index_name + " missing");
                }
                int64_t symbols = count_rows(db, "SELECT COUNT(*) FROM symbols;");
                int64_t valid_refs = count_rows(db,
                    "SELECT COUNT(*) FROM \"references\" r "
                    "JOIN symbols fs ON fs.id = r.from_symbol_id "
                    "JOIN symbols ts ON ts.id = r.to_symbol_id;");
                int64_t inheritance = count_rows(db, "SELECT COUNT(*) FROM inheritance;");
                int64_t crg_nodes = count_rows(db, "SELECT COUNT(*) FROM crg_nodes WHERE domain = 'source';");
                int64_t crg_edges = count_rows(db, "SELECT COUNT(*) FROM crg_edges WHERE domain = 'source';");
                int64_t crg_metrics = count_rows(db,
                    "SELECT COUNT(*) FROM crg_node_metrics m JOIN crg_nodes n ON n.id = m.node_id "
                    "WHERE n.domain = 'source';");
                projection_corruption_detected = crg_nodes < 0 || crg_edges < 0 || crg_metrics < 0;
                add_freshness_check("crg:nodes_row_parity", crg_nodes == symbols,
                                    "symbols=" + std::to_string(symbols) + " crg_nodes(source)=" + std::to_string(crg_nodes));
                add_freshness_check("crg:edges_row_parity", crg_edges == valid_refs + inheritance,
                                    "valid references+inheritance=" + std::to_string(valid_refs + inheritance) + " crg_edges(source)=" + std::to_string(crg_edges));
                add_freshness_check("crg:metrics_row_parity", crg_metrics == crg_nodes,
                                    "crg_nodes(source)=" + std::to_string(crg_nodes) + " crg_node_metrics=" + std::to_string(crg_metrics));
                std::string override_version = scalar_str(db, "SELECT value FROM crg_meta WHERE key='source_override_edges_version';");
                add_freshness_check("meta:source_override_edges_version", override_version == "2",
                                    override_version.empty() ? "source_override_edges_version missing" : "source_override_edges_version=" + override_version + " (expected 2)");
            } else {
                int64_t assets = count_rows(db, "SELECT COUNT(*) FROM assets;");
                int64_t dependencies = count_rows(db, "SELECT COUNT(*) FROM dependencies;");
                int64_t crg_nodes = count_rows(db, "SELECT COUNT(*) FROM crg_nodes WHERE domain = 'project';");
                int64_t crg_edges = count_rows(db, "SELECT COUNT(*) FROM crg_edges WHERE domain = 'project';");
                int64_t crg_metrics = count_rows(db,
                    "SELECT COUNT(*) FROM crg_node_metrics m JOIN crg_nodes n ON n.id = m.node_id "
                    "WHERE n.domain = 'project';");
                projection_corruption_detected = crg_nodes < 0 || crg_edges < 0 || crg_metrics < 0;
                add_freshness_check("crg:nodes_row_parity", crg_nodes == assets,
                                    "assets=" + std::to_string(assets) + " crg_nodes(project)=" + std::to_string(crg_nodes));
                add_freshness_check("crg:edges_row_parity", crg_edges == dependencies,
                                    "dependencies=" + std::to_string(dependencies) + " crg_edges(project)=" + std::to_string(crg_edges));
                add_freshness_check("crg:metrics_row_parity", crg_metrics == crg_nodes,
                                    "crg_nodes(project)=" + std::to_string(crg_nodes) + " crg_node_metrics=" + std::to_string(crg_metrics));
            }
            std::string cache_version = scalar_str(db, "SELECT value FROM crg_meta WHERE key='cache_version';");
            add_freshness_check("meta:cache_version", !cache_version.empty(),
                                cache_version.empty() ? "cache_version missing" : "cache_version=" + cache_version);
            std::string scoring_version = scalar_str(db, "SELECT value FROM crg_meta WHERE key='scoring_version';");
            add_freshness_check("meta:scoring_version", scoring_version == "3",
                                scoring_version.empty() ? "scoring_version missing" : "scoring_version=" + scoring_version + " (expected 3)");
        }
    }
    root["freshness_checks"] = freshness_checks;
    root["repair_needed"] = repair_needed;
    root["projection_corruption_detected"] = projection_corruption_detected;
    if (!execute) {
        root["status"] = "ok";
        root["summary"] = repair_needed
            ? (normalized_scope == "override_edges"
                ? "Dry-run: source override edge cache is stale and would be rebuilt. Pass --execute to apply."
                : "Dry-run: " + domain + " CRG projection/cache is stale and would be rebuilt. Pass --execute to apply.")
            : "Dry-run: " + domain + " CRG projection/cache is already fresh; --execute would skip the rebuild.";
        root["after"] = json::object();
        if (repair_needed) {
            root["next_actions"] = domain == "source"
                ? json::array({"source.repair_crg_cache --execute", "source.repair_crg_cache --scope=override_edges --execute", "source.health --include-deep-checks=true", "source.risk_score"})
                : json::array({domain + ".repair_crg_cache --execute", domain + ".health", domain + ".risk_score"});
        } else {
            root["next_actions"] = domain == "source"
                ? json::array({"source.health", "source.risk_score", "source.review_context"})
                : json::array({domain + ".health", domain + ".risk_score", domain + ".review_context"});
        }
        return root;
    }

    if (!repair_needed) {
        root["after"] = root["before"];
        root["skipped"] = true;
        root["status"] = "ok";
        root["summary"] = domain + " CRG projection/cache already fresh; skipped rebuild.";
        root["next_actions"] = domain == "source"
            ? json::array({"source.health", "source.risk_score", "source.review_context"})
            : json::array({"project.health", "project.risk_score", "project.review_context"});
        return root;
    }

    bool ok = true;
    bool transaction_started = false;
    std::string error;
    auto fail = [&](const std::string& label, const std::string& err) {
        ok = false;
        root["warnings"].push_back("CRG cache rebuild failed at " + label + (err.empty() ? "" : ": " + err));
    };
    auto finish_transaction = [&]() {
        if (!transaction_started) return;
        if (ok) {
            if (exec_sql_ok(db, "COMMIT;", error)) {
                transaction_started = false;
                return;
            }
            fail("commit", error);
        }

        std::string rollback_error;
        if (!exec_sql_ok(db, "ROLLBACK;", rollback_error) && !rollback_error.empty()) {
            root["warnings"].push_back("CRG cache rollback failed: " + rollback_error);
        }
        transaction_started = false;
    };

    // Repeat the probe and retain its transaction so a writer cannot appear
    // between freshness assessment and cache replacement.
    const CrgWriteProbeResult rebuild_probe = crg_probe_write_access(db, domain, true);
    if (!rebuild_probe.success) return write_blocked_result(rebuild_probe.error);
    transaction_started = rebuild_probe.transaction_started;
    root["write_preflight"] = {
        {"status", "ok"},
        {"probe", "transactional no-op write"},
    };

    // CRG projection tables are disposable caches. A damaged projection B-tree
    // cannot be healed by DELETE because SQLite must traverse the corrupt pages
    // first. When native source/project counts remain readable but a derived count
    // fails, replace only the derived schema inside the repair transaction and then
    // repopulate it from the authoritative native tables.
    if (projection_corruption_detected) {
        for (const auto& step : std::vector<std::pair<std::string, std::string>>{
                 {"drop corrupt CRG metrics", "DROP TABLE IF EXISTS crg_node_metrics;"},
                 {"drop corrupt CRG edges", "DROP TABLE IF EXISTS crg_edges;"},
                 {"drop corrupt CRG nodes", "DROP TABLE IF EXISTS crg_nodes;"},
                 {"drop corrupt CRG metadata", "DROP TABLE IF EXISTS crg_meta;"},
                 {"drop corrupt source override cache", "DROP TABLE IF EXISTS source_override_edges;"},
             }) {
            if (!exec_sql_ok(db, step.second, error)) {
                fail(step.first, error);
                break;
            }
        }
        if (ok) root["projection_schema_recreated"] = true;
    }

    // Schema/index creation and projection replacement are one transaction. A later
    // failure must not leave a half-created maintenance surface behind.
    if (!ensure_crg_projection_tables(db, error)) {
        fail("create schema", error);
    }
    if (ok && domain == "source" && !ensure_source_review_indexes(db, error)) {
        fail("create source review indexes", error);
    }
    if (domain == "source" && normalized_scope == "override_edges") {
        if (ok && !exec_sql_ok(db, "DELETE FROM source_override_edges;", error)) {
            fail("clear source override edges", error);
        }
        if (ok) {
            int64_t override_edges = 0;
            if (!populate_source_override_edge_cache(db, override_edges, error)) {
                fail("source override edge cache", error);
            } else if (!exec_sql_ok(db, "INSERT OR REPLACE INTO crg_meta(key,value) VALUES('source_override_edges_version','2');", error)) {
                fail("source_override_edges_version", error);
            } else {
                root["source_override_edges_built"] = override_edges;
            }
        }
        finish_transaction();
        root["after"] = crg_repair_counts(db, domain);
        root["status"] = ok ? "ok" : "error";
        root["summary"] = ok ? "Rebuilt source override edge cache" : "source override edge cache rebuild failed; transaction rolled back";
        root["next_actions"] = ok
            ? json::array({"source.health", "source.find_overrides", "source.review_hotspots"})
            : json::array({retry_action, "source.health"});
        return root;
    }
    if (ok) {
        for (const auto& step : crg_repair_sql(domain)) {
            if (!exec_sql_ok(db, step.second, error)) {
                fail(step.first, error);
                break;
            }
        }
    }
    if (ok && domain == "source") {
        int64_t override_edges = 0;
        if (!populate_source_override_edge_cache(db, override_edges, error)) {
            fail("source override edge cache", error);
        } else if (!exec_sql_ok(db, "INSERT OR REPLACE INTO crg_meta(key,value) VALUES('source_override_edges_version','2');", error)) {
            fail("source_override_edges_version", error);
        } else {
            root["source_override_edges_built"] = override_edges;
        }
    }
    finish_transaction();

    root["after"] = crg_repair_counts(db, domain);
    root["status"] = ok ? "ok" : "error";
    root["summary"] = ok
        ? std::string("Rebuilt ") + domain + " CRG projection/cache from indexed " + (domain == "source" ? "source symbols, references and inheritance" : "project assets and dependencies")
        : domain + " CRG projection/cache rebuild failed; transaction rolled back";
    root["next_actions"] = ok
        ? (domain == "source"
            ? json::array({"source.health", "source.risk_score", "source.review_context"})
            : json::array({"project.health", "project.risk_score", "project.review_context"}))
        : json::array({retry_action, domain + ".health"});
    return root;
}

static bool load_current_manifest(Database& db, const std::string& domain, SnapshotManifest& out) {
    out.nodes.clear();
    out.edges.clear();
    std::string error;
    Rows nodes;
    if (!query_rows_ok(db,
        "SELECT stable_key FROM crg_nodes WHERE domain = ? ORDER BY stable_key;",
        {domain}, nodes, error)) {
        std::cerr << "[snapshot current manifest error] " << error << std::endl;
        return false;
    }
    for (const auto& row : nodes) out.nodes.insert(row.get("stable_key"));

    Rows edges;
    if (!query_rows_ok(db,
        "SELECT sn.stable_key AS source_key,tn.stable_key AS target_key,"
        "e.edge_kind,COALESCE(e.edge_subkind,'') AS edge_subkind "
        "FROM crg_edges e "
        "JOIN crg_nodes sn ON sn.id = e.source_node_id "
        "JOIN crg_nodes tn ON tn.id = e.target_node_id "
        "WHERE e.domain = ? ORDER BY sn.stable_key,tn.stable_key,e.edge_kind,e.edge_subkind;",
        {domain}, edges, error)) {
        std::cerr << "[snapshot current manifest error] " << error << std::endl;
        return false;
    }
    for (const auto& row : edges) out.edges.insert(snapshot_edge_key(row));
    return true;
}

static std::string snapshot_edge_key_part(const std::string& value) {
    std::string out;
    out.reserve(value.size());
    for (char c : value) {
        if (c == '\\' || c == '|') out.push_back('\\');
        out.push_back(c);
    }
    return out;
}

static std::string snapshot_edge_key(const Row& row) {
    return snapshot_edge_key_part(row.get("source_key")) + "|" +
           snapshot_edge_key_part(row.get("target_key")) + "|" +
           snapshot_edge_key_part(row.get("edge_kind")) + "|" +
           snapshot_edge_key_part(row.get("edge_subkind"));
}

static std::vector<std::string> split_snapshot_edge_key(const std::string& key) {
    std::vector<std::string> parts;
    std::string part;
    bool escaped = false;
    for (char c : key) {
        if (escaped) {
            part.push_back(c);
            escaped = false;
            continue;
        }
        if (c == '\\') {
            escaped = true;
            continue;
        }
        if (c == '|') {
            parts.push_back(part);
            part.clear();
            continue;
        }
        part.push_back(c);
    }
    parts.push_back(part);
    return parts;
}

static bool load_snapshot_record(Database& db, const std::string& domain,
                                 const std::string& ref, SnapshotRecord& out) {
    std::string clean_ref = trim_copy(ref);
    out = SnapshotRecord{};
    if (clean_ref.empty() || lower_copy(clean_ref) == "current") {
        out.label = "current";
        return load_current_manifest(db, domain, out.manifest);
    }

    Rows rows;
    if (is_numeric_string(clean_ref)) {
        rows = query(db, "SELECT id,label,manifest_json FROM crg_snapshots WHERE domain = ? AND id = ? LIMIT 1;",
                     {domain, clean_ref});
    }
    if (rows.empty()) {
        rows = query(db, "SELECT id,label,manifest_json FROM crg_snapshots WHERE domain = ? AND label = ? LIMIT 1;",
                     {domain, clean_ref});
    }
    if (rows.empty()) return false;
    out.id = rows[0].get_int64("id");
    out.label = rows[0].get("label");
    return parse_manifest(rows[0].get("manifest_json"), out.manifest);
}

static std::set<std::string> set_difference(const std::set<std::string>& left,
                                            const std::set<std::string>& right) {
    std::set<std::string> out;
    for (const auto& value : left) {
        if (!right.count(value)) out.insert(value);
    }
    return out;
}

static json take_string_samples(const std::set<std::string>& values, int limit, bool& truncated) {
    json arr = json::array();
    int count = 0;
    for (const auto& value : values) {
        if (count >= limit) {
            truncated = true;
            break;
        }
        arr.push_back(value);
        ++count;
    }
    return arr;
}

static json edge_object(const std::string& key) {
    json edge = {{"key", key}};
    auto parts = split_snapshot_edge_key(key);
    if (parts.size() == 4) {
        edge["source"] = parts[0];
        edge["target"] = parts[1];
        edge["kind"] = parts[2];
        edge["subkind"] = parts[3];
    }
    return edge;
}

static json take_edge_samples(const std::set<std::string>& values, int limit, bool& truncated) {
    json arr = json::array();
    int count = 0;
    for (const auto& value : values) {
        if (count >= limit) {
            truncated = true;
            break;
        }
        arr.push_back(edge_object(value));
        ++count;
    }
    return arr;
}

static bool insert_snapshot_record(Database& db, const std::string& domain,
                                   const std::string& label,
                                   const SnapshotManifest& manifest,
                                   int64_t& out_id, std::string& error) {
    sqlite3_stmt* stmt = nullptr;
    const char* sql =
        "INSERT OR REPLACE INTO crg_snapshots(label,domain,captured_at,node_count,edge_count,manifest_json) "
        "VALUES(?,?,?,?,?,?);";
    int rc = sqlite3_prepare_v2(db.db, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        error = sqlite3_errmsg(db.db);
        return false;
    }
    std::string manifest_json = serialize_manifest(manifest);
    sqlite3_bind_text(stmt, 1, label.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, domain.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 3, static_cast<sqlite3_int64>(std::time(nullptr)));
    sqlite3_bind_int64(stmt, 4, static_cast<sqlite3_int64>(manifest.nodes.size()));
    sqlite3_bind_int64(stmt, 5, static_cast<sqlite3_int64>(manifest.edges.size()));
    sqlite3_bind_text(stmt, 6, manifest_json.c_str(), -1, SQLITE_TRANSIENT);
    rc = sqlite3_step(stmt);
    if (rc != SQLITE_DONE) error = sqlite3_errmsg(db.db);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) return false;
    out_id = sqlite3_last_insert_rowid(db.db);
    return true;
}

static json crg_snapshot_json(Database& db, const std::string& domain,
                              const std::string& raw_label, bool execute) {
    std::string label = trim_copy(raw_label);
    if (label.empty()) {
        const auto ticks = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        label = domain + "-" + std::to_string(ticks);
    }
    json root = {
        {"input", {{"label", label}, {"execute", execute}}},
    };

    if (!object_exists(db, "table", "crg_nodes") || !object_exists(db, "table", "crg_edges")) {
        root["status"] = "error";
        root["summary"] = "CRG projection tables are missing; run " + domain + ".repair_crg_cache --execute first";
        root["next_actions"] = json::array({domain + ".repair_crg_cache", domain + ".health"});
        return root;
    }

    SnapshotManifest manifest;
    if (!load_current_manifest(db, domain, manifest)) {
        root["status"] = "error";
        root["summary"] = "Failed to read current " + domain + " CRG projection";
        root["next_actions"] = json::array({domain + ".health", domain + ".repair_crg_cache"});
        return root;
    }

    root["node_count"] = manifest.nodes.size();
    root["edge_count"] = manifest.edges.size();
    root["executed"] = execute;
    root["truncated"] = false;
    if (!execute) {
        root["status"] = "ok";
        root["summary"] = "Would capture " + domain + " CRG snapshot '" + label + "' with " +
            std::to_string(manifest.nodes.size()) + " node(s), " +
            std::to_string(manifest.edges.size()) + " edge(s)";
        root["next_actions"] = json::array({domain + ".snapshot --execute", domain + ".diff_snapshots"});
        return root;
    }

    std::string error;
    if (!ensure_snapshot_table(db, error)) {
        root["status"] = "error";
        root["summary"] = "Failed to create crg_snapshots table: " + error;
        root["next_actions"] = json::array({domain + ".health"});
        return root;
    }
    int64_t id = 0;
    if (!insert_snapshot_record(db, domain, label, manifest, id, error)) {
        root["status"] = "error";
        root["summary"] = "Failed to store " + domain + " CRG snapshot: " + error;
        root["next_actions"] = json::array({domain + ".health"});
        return root;
    }
    root["id"] = id;
    root["label"] = label;
    root["status"] = "ok";
    root["summary"] = "Captured " + domain + " CRG snapshot '" + label + "' with " +
        std::to_string(manifest.nodes.size()) + " node(s), " +
        std::to_string(manifest.edges.size()) + " edge(s)";
    root["next_actions"] = json::array({domain + ".diff_snapshots", domain + ".repair_crg_cache"});
    return root;
}

static json crg_diff_snapshots_json(Database& db, const std::string& domain,
                                    const std::string& before,
                                    const std::string& after,
                                    int limit) {
    int cap = clamp_int(limit <= 0 ? 100 : limit, 1, 1000);
    std::string before_ref = trim_copy(before);
    std::string after_ref = trim_copy(after);
    if (after_ref.empty()) after_ref = "current";
    json root = {
        {"input", {{"before", before_ref}, {"after", after_ref}}},
        {"limits", {{"limit", cap}}},
    };
    if (before_ref.empty()) {
        root["status"] = "error";
        root["summary"] = "before snapshot label/id is required";
        root["next_actions"] = json::array({domain + ".snapshot --execute"});
        return root;
    }
    if (!object_exists(db, "table", "crg_snapshots")) {
        root["status"] = "error";
        root["summary"] = "crg_snapshots table is missing; capture a " + domain + ".snapshot first";
        root["next_actions"] = json::array({domain + ".snapshot --execute"});
        return root;
    }

    SnapshotRecord before_record;
    SnapshotRecord after_record;
    if (!load_snapshot_record(db, domain, before_ref, before_record)) {
        root["status"] = "error";
        root["summary"] = "Before snapshot not found or invalid: " + before_ref;
        root["next_actions"] = json::array({domain + ".snapshot --execute"});
        return root;
    }
    if (!load_snapshot_record(db, domain, after_ref, after_record)) {
        root["status"] = "error";
        root["summary"] = "After snapshot not found or invalid: " + after_ref;
        root["next_actions"] = json::array({domain + ".snapshot --execute"});
        return root;
    }

    auto new_nodes = set_difference(after_record.manifest.nodes, before_record.manifest.nodes);
    auto removed_nodes = set_difference(before_record.manifest.nodes, after_record.manifest.nodes);
    auto new_edges = set_difference(after_record.manifest.edges, before_record.manifest.edges);
    auto removed_edges = set_difference(before_record.manifest.edges, after_record.manifest.edges);

    bool truncated = false;
    root["new_nodes"] = take_string_samples(new_nodes, cap, truncated);
    root["removed_nodes"] = take_string_samples(removed_nodes, cap, truncated);
    root["new_edges"] = take_edge_samples(new_edges, cap, truncated);
    root["removed_edges"] = take_edge_samples(removed_edges, cap, truncated);
    root["summary_counts"] = {
        {"nodes_added", new_nodes.size()},
        {"nodes_removed", removed_nodes.size()},
        {"edges_added", new_edges.size()},
        {"edges_removed", removed_edges.size()},
        {"before_total_nodes", before_record.manifest.nodes.size()},
        {"after_total_nodes", after_record.manifest.nodes.size()},
        {"before_total_edges", before_record.manifest.edges.size()},
        {"after_total_edges", after_record.manifest.edges.size()},
    };
    root["before_label"] = before_record.label;
    root["after_label"] = after_record.label;
    root["truncated"] = truncated;
    root["status"] = "ok";
    root["summary"] = domain + " CRG diff " + before_record.label + " -> " + after_record.label +
        ": +" + std::to_string(new_nodes.size()) + "/-" + std::to_string(removed_nodes.size()) +
        " node(s), +" + std::to_string(new_edges.size()) + "/-" + std::to_string(removed_edges.size()) +
        " edge(s)";
    root["next_actions"] = json::array({domain + ".snapshot", domain + ".review_hotspots", domain + ".health"});
    return root;
}

// ============================================================
// RX-2: offline CRG projection-cache READ parity
//
// The editor (FMonolithIndexReview::TryCachedAssetRisk /
// FMonolithSourceDatabase::GetCachedRiskForSymbol) reads crg_node_metrics
// when present and falls back to query-time scoring otherwise. The offline
// tool previously NEVER read the cache (always scoring_version=1, no
// cache metadata, no crg:* health). These read-only helpers mirror the
// editor behavior. Offline CRG cache writes are execute-gated behind
// repair_crg_cache so read-only actions can keep read-only database handles.
// ============================================================

static bool crg_cache_present(Database& db) {
    return object_exists(db, "table", "crg_nodes")
        && object_exists(db, "table", "crg_node_metrics")
        && object_exists(db, "table", "crg_meta");
}

static bool source_override_edge_cache_ready(Database& db) {
    return object_exists(db, "table", "source_override_edges")
        && object_exists(db, "table", "crg_meta")
        && scalar_str(db, "SELECT value FROM crg_meta WHERE key='source_override_edges_version';") == "2";
}

// Returns a populated risk item (score/tier/reasons/raw_counts + cache) on a
// cache hit, or a null json on miss/absent. Joins on native_id so it is
// robust whether crg_nodes.id is AUTOINCREMENT or aliased to the native id.
static json try_cached_risk(Database& db, const std::string& domain,
                            const std::string& native_table, int64_t native_id) {
    if (!crg_cache_present(db)) return json();
    auto rows = query(db,
        "SELECT m.risk_score, m.risk_tier, m.reasons_json, m.raw_counts_json, "
        "m.scoring_version, "
        "COALESCE((SELECT value FROM crg_meta WHERE key='cache_version'),'1') AS cache_version "
        "FROM crg_nodes n JOIN crg_node_metrics m ON m.node_id = n.id "
        "WHERE n.domain = ? AND n.native_table = ? AND n.native_id = ? LIMIT 1;",
        {domain, native_table, std::to_string(native_id)});
    if (rows.empty()) return json();
    const Row& r = rows[0];
    json reasons = json::array();
    {
        json p = json::parse(r.get("reasons_json", "[]"), nullptr, false);
        if (p.is_array()) reasons = p;
    }
    json raw = json::object();
    {
        json p = json::parse(r.get("raw_counts_json", "{}"), nullptr, false);
        if (p.is_object()) raw = p;
    }
    double score = r.get_double("risk_score");
    std::string sv = r.get("scoring_version", "3");
    std::string cv = r.get("cache_version", "1");
    json item;
    item["score"] = std::round(score * 1000.0) / 1000.0;
    item["tier"] = r.get("risk_tier", tier_for(score));
    item["reasons"] = reasons;
    item["raw_counts"] = raw;
    item["scoring_version"] = sv;
    item["cache"] = {
        {"status", "hit"}, {"version", cv}, {"cache_version", cv}, {"scoring_version", sv},
    };
    return item;
}

// Appends crg:* checks mirroring the editor ComputeHealth. Cache ABSENT is
// informational only (keeps offline `health` "ok" for pre-cache DBs, per
// projection-cache REQ-006); cache PRESENT-but-inconsistent warns like the
// editor. `valid_edges` lets the caller pass the dangling-ref-corrected
// native edge count so parity matches the editor's current behavior.
static void append_crg_health_checks(Database& db, const std::string& domain,
                                     int64_t native_node_cnt, int64_t valid_edges,
                                     json& root) {
    auto info = [&](const std::string& name, const std::string& detail) {
        root["checks"].push_back({{"check", name}, {"result", "info"}, {"detail", detail}});
    };
    auto check = [&](const std::string& name, bool pass, const std::string& detail) {
        root["checks"].push_back({{"check", name}, {"result", pass ? "ok" : "warning"}, {"detail", detail}});
        if (!pass) root["warnings"].push_back(detail);
    };
    if (!crg_cache_present(db)) {
        info("crg:cache_absent",
             "CRG projection cache not built (run repair_crg_cache for cached risk + parity checks)");
        return;
    }
    for (const char* t : {"crg_nodes", "crg_edges", "crg_node_metrics", "crg_meta"})
        check(std::string("crg:table:") + t, object_exists(db, "table", t),
              object_exists(db, "table", t) ? std::string("CRG table ") + t + " present"
                                            : std::string("missing CRG table ") + t);
    if (domain == "source") {
        check("crg:table:source_override_edges", object_exists(db, "table", "source_override_edges"),
              object_exists(db, "table", "source_override_edges")
                  ? "CRG table source_override_edges present"
                  : "missing CRG table source_override_edges (run source.repair_crg_cache)");
    }
    int64_t cnodes = count_rows(db, "SELECT COUNT(*) FROM crg_nodes WHERE domain = '" + domain + "';");
    int64_t cedges = count_rows(db, "SELECT COUNT(*) FROM crg_edges WHERE domain = '" + domain + "';");
    int64_t cmetrics = count_rows(db,
        "SELECT COUNT(*) FROM crg_node_metrics m JOIN crg_nodes n ON n.id = m.node_id "
        "WHERE n.domain = '" + domain + "';");
    check("crg:nodes_row_parity", cnodes == native_node_cnt,
          domain + " native=" + std::to_string(native_node_cnt) + " crg_nodes=" + std::to_string(cnodes) +
          (cnodes == native_node_cnt ? "" : " (mismatch -> repair_crg_cache)"));
    check("crg:edges_row_parity", cedges == valid_edges,
          "valid native edges=" + std::to_string(valid_edges) + " crg_edges=" + std::to_string(cedges) +
          (cedges == valid_edges ? "" : " (mismatch -> repair_crg_cache)"));
    check("crg:metrics_row_parity", cmetrics == cnodes,
          "crg_nodes=" + std::to_string(cnodes) + " crg_node_metrics=" + std::to_string(cmetrics) +
          (cmetrics == cnodes ? "" : " (mismatch -> repair_crg_cache)"));
    int64_t orphan = count_rows(db,
        "SELECT COUNT(*) FROM crg_edges e WHERE e.domain = '" + domain + "' AND ("
        "e.source_node_id NOT IN (SELECT id FROM crg_nodes) OR "
        "e.target_node_id NOT IN (SELECT id FROM crg_nodes));");
    check("crg:orphan_edges", orphan == 0,
          orphan == 0 ? "no orphan CRG projection edge rows"
                      : std::to_string(orphan) + " orphan CRG projection edge row(s)");
    std::string cache_ver = scalar_str(db, "SELECT value FROM crg_meta WHERE key = 'cache_version';");
    check("crg:cache_version", !cache_ver.empty(),
          cache_ver.empty() ? "crg_meta.cache_version missing (run repair_crg_cache)"
                            : "crg cache_version=" + cache_ver);
    std::string scoring_ver = scalar_str(db, "SELECT value FROM crg_meta WHERE key = 'scoring_version';");
    check("crg:scoring_version", scoring_ver == "3",
          scoring_ver.empty() ? "crg_meta.scoring_version missing (run repair_crg_cache)"
                              : "crg scoring_version=" + scoring_ver + " (expected 3)");
    if (domain == "source") {
        std::string override_ver = scalar_str(db, "SELECT value FROM crg_meta WHERE key = 'source_override_edges_version';");
        check("crg:source_override_edges_version", override_ver == "2",
              override_ver.empty() ? "crg_meta.source_override_edges_version missing (run source.repair_crg_cache)"
                                   : "source override edge cache version=" + override_ver + " (expected 2)");
    }
}
