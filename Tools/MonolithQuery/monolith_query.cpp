// monolith_query.exe — Standalone offline query tool for Monolith databases.
// Replaces monolith_offline.py with zero Python dependency.
// Links sqlite3 amalgamation directly. No Unreal Engine dependency.
//
// Usage:
//   monolith_query.exe source <action> [params...] [--options]
//   monolith_query.exe project <action> [params...] [--options]
//   monolith_query.exe context <action> [params...] [--options]

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <map>
#include <set>
#include <fstream>
#include <iostream>
#include <sstream>
#include <algorithm>
#include <cmath>
#include <functional>
#include <filesystem>
#include <ctime>
#include <chrono>

#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#include "sqlite3.h"
#include <nlohmann/json.hpp>

namespace fs = std::filesystem;
using json = nlohmann::json;

// ============================================================
// Utility
// ============================================================

static void die(const std::string& msg) {
    std::cerr << "ERROR: " << msg << std::endl;
    std::exit(1);
}

// FTS5 query escaping — mirrors Python escape_fts() and C++ EscapeFTS()
static std::string escape_fts(const std::string& query) {
    // Replace :: with space (C++ qualified names)
    std::string q = query;
    for (size_t pos = 0; (pos = q.find("::", pos)) != std::string::npos;)
        q.replace(pos, 2, " ");

    // Strip non-alphanumeric, non-whitespace
    std::string cleaned;
    cleaned.reserve(q.size());
    for (char c : q) {
        if (std::isalnum(static_cast<unsigned char>(c)) || std::isspace(static_cast<unsigned char>(c)) || c == '_')
            cleaned += c;
    }

    // Tokenize and wrap
    std::istringstream iss(cleaned);
    std::string token;
    std::vector<std::string> tokens;
    while (iss >> token)
        tokens.push_back(token);

    if (tokens.empty())
        return "\"\"";

    std::string result;
    for (size_t i = 0; i < tokens.size(); ++i) {
        if (i > 0) result += " ";
        result += "\"" + tokens[i] + "\"*";
    }
    return result;
}

// ============================================================
// SQLite RAII wrapper
// ============================================================

class Database {
public:
    sqlite3* db = nullptr;

    Database() = default;
    ~Database() { if (db) sqlite3_close(db); }

    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;

    void open(const std::string& path, bool query_only = true) {
        if (!fs::exists(path))
            die("Database not found: " + path);

        int rc = sqlite3_open_v2(path.c_str(), &db, query_only ? SQLITE_OPEN_READONLY : SQLITE_OPEN_READWRITE, nullptr);
        if (rc != SQLITE_OK)
            die("Failed to open database: " + path + " — " + sqlite3_errmsg(db));

        if (query_only)
            exec("PRAGMA query_only=ON;");
        else
            exec("PRAGMA journal_mode=DELETE;");
    }

    void exec(const char* sql) {
        char* err = nullptr;
        sqlite3_exec(db, sql, nullptr, nullptr, &err);
        if (err) {
            std::string msg(err);
            sqlite3_free(err);
            die("SQL error: " + msg);
        }
    }
};

// Simple row type: vector of (column_name, value) pairs
struct Row {
    std::map<std::string, std::string> cols;

    std::string get(const std::string& key, const std::string& def = "") const {
        auto it = cols.find(key);
        return (it != cols.end()) ? it->second : def;
    }

    int get_int(const std::string& key, int def = 0) const {
        auto it = cols.find(key);
        if (it == cols.end() || it->second.empty()) return def;
        try { return std::stoi(it->second); }
        catch (...) { return def; }
    }

    int64_t get_int64(const std::string& key, int64_t def = 0) const {
        auto it = cols.find(key);
        if (it == cols.end() || it->second.empty()) return def;
        try { return std::stoll(it->second); }
        catch (...) { return def; }
    }

    double get_double(const std::string& key, double def = 0.0) const {
        auto it = cols.find(key);
        if (it == cols.end() || it->second.empty()) return def;
        try { return std::stod(it->second); }
        catch (...) { return def; }
    }
};

using Rows = std::vector<Row>;

static Rows query(Database& db, const std::string& sql, const std::vector<std::string>& params = {}) {
    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(db.db, sql.c_str(), -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        std::string err = sqlite3_errmsg(db.db);
        // Log to stderr for diagnostics, return empty on operational error
        std::cerr << "[query error] " << err << "\n  SQL: " << sql.substr(0, 200) << std::endl;
        return {};
    }

    for (int i = 0; i < (int)params.size(); ++i)
        sqlite3_bind_text(stmt, i + 1, params[i].c_str(), -1, SQLITE_TRANSIENT);

    Rows rows;
    int ncols = sqlite3_column_count(stmt);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        Row row;
        for (int c = 0; c < ncols; ++c) {
            const char* name = sqlite3_column_name(stmt, c);
            const char* val = (const char*)sqlite3_column_text(stmt, c);
            row.cols[name ? name : ""] = val ? val : "";
        }
        rows.push_back(std::move(row));
    }
    sqlite3_finalize(stmt);
    return rows;
}

static bool query_rows_ok(Database& db, const std::string& sql,
                          const std::vector<std::string>& params,
                          Rows& rows, std::string& error) {
    rows.clear();
    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(db.db, sql.c_str(), -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        error = sqlite3_errmsg(db.db);
        return false;
    }

    for (int i = 0; i < (int)params.size(); ++i)
        sqlite3_bind_text(stmt, i + 1, params[i].c_str(), -1, SQLITE_TRANSIENT);

    int ncols = sqlite3_column_count(stmt);
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        Row row;
        for (int c = 0; c < ncols; ++c) {
            const char* name = sqlite3_column_name(stmt, c);
            const char* val = (const char*)sqlite3_column_text(stmt, c);
            row.cols[name ? name : ""] = val ? val : "";
        }
        rows.push_back(std::move(row));
    }
    if (rc != SQLITE_DONE) {
        error = sqlite3_errmsg(db.db);
        sqlite3_finalize(stmt);
        return false;
    }
    sqlite3_finalize(stmt);
    return true;
}

static void print_json(const json& value) {
    std::cout << value.dump(2) << std::endl;
}

static int clamp_int(int value, int lo, int hi) {
    return std::max(lo, std::min(value, hi));
}

static std::vector<std::string> split_kinds(const std::string& kinds) {
    std::vector<std::string> out;
    std::string cur;
    std::istringstream ss(kinds);
    while (std::getline(ss, cur, '|')) {
        cur.erase(cur.begin(), std::find_if(cur.begin(), cur.end(), [](unsigned char ch) { return !std::isspace(ch); }));
        cur.erase(std::find_if(cur.rbegin(), cur.rend(), [](unsigned char ch) { return !std::isspace(ch); }).base(), cur.end());
        if (!cur.empty()) out.push_back(cur);
    }
    return out;
}

static bool wants_kind(const std::string& kinds, const std::string& kind) {
    if (kinds.empty()) return true;
    for (const auto& k : split_kinds(kinds)) {
        if (k == kind) return true;
    }
    return false;
}

static std::string tier_for(double score) {
    if (score >= 0.66) return "high";
    if (score >= 0.33) return "medium";
    return "low";
}

static int tier_rank(const std::string& tier) {
    if (tier == "high") return 2;
    if (tier == "medium") return 1;
    return 0;
}

static std::string lower_copy(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

static std::string trim_copy(std::string value) {
    value.erase(value.begin(), std::find_if(value.begin(), value.end(),
        [](unsigned char ch) { return !std::isspace(ch); }));
    value.erase(std::find_if(value.rbegin(), value.rend(),
        [](unsigned char ch) { return !std::isspace(ch); }).base(), value.end());
    return value;
}

static bool is_numeric_string(const std::string& value) {
    if (value.empty()) return false;
    return std::all_of(value.begin(), value.end(),
        [](unsigned char ch) { return std::isdigit(ch); });
}

static bool contains_any_token(const std::string& lower_text,
                               std::initializer_list<const char*> tokens) {
    for (const char* token : tokens) {
        if (lower_text.find(token) != std::string::npos)
            return true;
    }
    return false;
}

static std::pair<double, std::string> sensitivity_factor(const std::string& text,
                                                         bool source_domain) {
    std::string lower = lower_copy(text);
    if (source_domain && contains_any_token(lower, {"ufunction", "server", "client", "netmulticast", "onrep", "replication", "rpc", "network"}))
        return {0.15, "sensitivity: replication/RPC or network surface"};
    if (!source_domain && contains_any_token(lower, {"replication", "network", "rpc", "netmulticast", "onrep", "server", "client"}))
        return {0.15, "sensitivity: replication/RPC or network surface"};
    if (contains_any_token(lower, {"save", "serialize", "archive"}))
        return {0.15, "sensitivity: save/serialization surface"};
    if (contains_any_token(lower, {"auth", "login", "account", "session"}))
        return {0.15, "sensitivity: auth/account/session surface"};
    if (contains_any_token(lower, {"purchase", "iap", "store", "entitlement"}))
        return {0.15, "sensitivity: purchase/store entitlement surface"};
    if (contains_any_token(lower, {"anticheat", "anti_cheat", "cheat"}))
        return {0.15, "sensitivity: anticheat surface"};
    if (contains_any_token(lower, {"crypt", "encrypt", "decrypt", "sign", "hash"}))
        return {0.15, "sensitivity: crypto/signing/hash surface"};
    if (contains_any_token(lower, {"exec", "eval", "command"}))
        return {0.15, "sensitivity: exec/eval/command surface"};
    if (contains_any_token(lower, {"file", "registry", "process"}))
        return {0.15, "sensitivity: file/registry/process surface"};
    return {0.0, ""};
}

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
    "CREATE INDEX IF NOT EXISTS idx_crg_nodes_domain_native ON crg_nodes(domain, native_table, native_id);",
    "CREATE INDEX IF NOT EXISTS idx_crg_nodes_stable ON crg_nodes(domain, stable_key);",
    "CREATE INDEX IF NOT EXISTS idx_crg_edges_domain_source ON crg_edges(domain, source_node_id);",
    "CREATE INDEX IF NOT EXISTS idx_crg_edges_domain_target ON crg_edges(domain, target_node_id);",
    "CREATE INDEX IF NOT EXISTS idx_crg_edges_kind_subkind ON crg_edges(domain, edge_kind, edge_subkind);",
    "CREATE INDEX IF NOT EXISTS idx_crg_metrics_score ON crg_node_metrics(risk_score DESC);",
};

static bool ensure_crg_projection_tables(Database& db, std::string& error) {
    for (const char* sql : kCrgProjectionDdl) {
        if (!exec_sql_ok(db, sql, error)) return false;
    }
    return true;
}

static bool has_crg_projection_tables(Database& db) {
    return object_exists(db, "table", "crg_nodes")
        && object_exists(db, "table", "crg_edges")
        && object_exists(db, "table", "crg_node_metrics")
        && object_exists(db, "table", "crg_meta");
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
            OR lower(COALESCE(s.qualified_name,'') || ' ' || COALESCE(s.name,'') || ' ' || COALESCE(s.signature,'')) LIKE '%sign%'
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
         printf('["caller fan-in: %d","inheritance descendants (1-hop): %d","callee fan-out: %d","module/file boundary crossing: %d distinct caller file(s)","sensitivity: UE-domain sensitive surface"]',
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
            OR lower(a.asset_class || ' ' || a.package_path || ' ' || a.asset_name) LIKE '%sign%'
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
         printf('["inbound dependency fan-in: %d referencer(s)","hard inbound dependencies: %d","outbound dependencies: %d","sensitivity: UE-domain sensitive surface","graph density: %d node(s)"]',
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

static json crg_repair_cache_json(Database& db, const std::string& domain,
                                  const std::string& scope, bool execute) {
    std::string normalized_scope = scope.empty() ? "all" : lower_copy(scope);
    json warnings = json::array();
    json plan = json::array({
        "CREATE IF MISSING crg_nodes/crg_edges/crg_node_metrics/crg_meta",
        "DELETE existing " + domain + " CRG projection rows",
        domain == "source"
            ? "SOURCE symbols -> crg_nodes; references/inheritance -> crg_edges"
            : "PROJECT assets -> crg_nodes; dependencies -> crg_edges",
        domain == "source"
            ? "Recompute caller/callee/descendant/risk_score into crg_node_metrics"
            : "Recompute fan-in/fan-out/hard-in/risk_score into crg_node_metrics",
    });
    json root = {
        {"input", {{"execute", execute}, {"scope", normalized_scope}}},
        {"limits", {{"execute", execute}, {"scope", normalized_scope}}},
        {"plan", plan},
        {"warnings", warnings},
        {"truncated", false},
    };
    if (normalized_scope != "all") {
        root["status"] = "error";
        root["summary"] = "Unsupported scope for repair_crg_cache (expected 'all')";
        root["next_actions"] = json::array({domain + ".repair_crg_cache --scope=all", domain + ".health"});
        return root;
    }

    root["before"] = crg_repair_counts(db, domain);
    if (!execute) {
        root["status"] = "ok";
        root["summary"] = "Dry-run: " + domain + " CRG projection/cache would be rebuilt. Pass --execute to apply.";
        root["after"] = json::object();
        root["next_actions"] = json::array({domain + ".repair_crg_cache --execute", domain + ".health", domain + ".risk_score"});
        return root;
    }

    bool ok = true;
    std::string error;
    auto fail = [&](const std::string& label, const std::string& err) {
        ok = false;
        root["warnings"].push_back("CRG cache rebuild failed at " + label + (err.empty() ? "" : ": " + err));
    };
    if (!ensure_crg_projection_tables(db, error)) {
        fail("create schema", error);
    }
    if (ok && !exec_sql_ok(db, "BEGIN;", error)) {
        fail("begin transaction", error);
    }
    if (ok) {
        for (const auto& step : crg_repair_sql(domain)) {
            if (!exec_sql_ok(db, step.second, error)) {
                fail(step.first, error);
                break;
            }
        }
    }
    if (ok) {
        if (!exec_sql_ok(db, "COMMIT;", error)) fail("commit", error);
    } else {
        std::string rollback_error;
        exec_sql_ok(db, "ROLLBACK;", rollback_error);
    }

    root["after"] = crg_repair_counts(db, domain);
    root["status"] = ok ? "ok" : "error";
    root["summary"] = ok
        ? std::string("Rebuilt ") + domain + " CRG projection/cache from indexed " + (domain == "source" ? "source symbols, references and inheritance" : "project assets and dependencies")
        : domain + " CRG projection/cache rebuild failed; rolled back";
    root["next_actions"] = domain == "source"
        ? json::array({"source.health", "source.risk_score", "source.review_context"})
        : json::array({"project.health", "project.risk_score", "project.review_context"});
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
}

// ============================================================
// CLI argument parser
// ============================================================

struct Args {
    std::string ns;       // "source" or "project"
    std::string action;   // e.g. "search_source", "read_source"
    std::vector<std::string> positional;
    std::map<std::string, std::string> options;

    std::string opt(const std::string& key, const std::string& def = "") const {
        auto it = options.find(key);
        return (it != options.end()) ? it->second : def;
    }

    int opt_int(const std::string& key, int def) const {
        auto it = options.find(key);
        if (it == options.end() || it->second.empty()) return def;
        try { return std::stoi(it->second); }
        catch (...) { return def; }
    }

    bool opt_bool(const std::string& key, bool def = false) const {
        auto it = options.find(key);
        if (it == options.end()) return def;
        const auto& v = it->second;
        return v.empty() || v == "true" || v == "1" || v == "yes";
    }
};

static Args parse_args(int argc, char* argv[]) {
    Args args;
    if (argc < 3) {
        std::cerr << "Usage: monolith_query <source|project> <action> [params...] [--options]\n\n"
                  << "Source actions:\n"
                  << "  search_source <query> [--scope=all|cpp|shaders] [--limit=N] [--module=M] [--kind=K]\n"
                  << "  read_source <symbol> [--max-lines=N] [--no-header] [--members-only]\n"
                  << "  find_references <symbol> [--ref-kind=K] [--limit=N]\n"
                  << "  find_callers <symbol> [--limit=N]\n"
                  << "  find_callees <symbol> [--limit=N]\n"
                  << "  get_class_hierarchy <symbol> [--direction=up|down|both] [--depth=N]\n"
                  << "  get_module_info <module_name>\n"
                  << "  get_symbol_context <symbol> [--context-lines=N]\n"
                  << "  read_file <file_path> [--start=N] [--end=N]\n"
                  << "  impact_radius <symbol> [--edge-kinds=call|type|inheritance] [--direction=in|out|both] [--max-depth=N] [--max-results=N]\n"
                  << "  health [--include-counts=false]\n"
                  << "  repair_fts [--target=all|symbols|source] [--execute]\n"
                  << "  repair_crg_cache [--scope=all] [--execute]\n"
                  << "  risk_score <symbol> [--limit=N] [--min-tier=low|medium|high]\n"
                  << "  review_hotspots [--kind=fan_in|fan_out|risk|large|all] [--limit=N] [--min-lines=N] [--include-questions=false]\n"
                  << "  review_context <symbol> [--direction=in|out|both] [--detail-level=minimal|standard]\n"
                  << "  detect_changes <path...> [--changed-paths=a,b] [--ranges=path:s-e,...] [--diff-file=PATH] [--diff-stdin] [--max-results=N] [--detail-level=minimal|standard]\n"
                  << "  find_unused [--kind=function|class|struct|all] [--limit=N] [--min-confidence=low|medium|high]\n"
                  << "  pre_merge_check <path...> [--changed-paths=a,b] [--max-results=N] [--unused-limit=N] [--detail-level=minimal|standard] [--include-unused=false]\n"
                  << "  snapshot [label] [--label=name] [--execute]\n"
                  << "  diff_snapshots <before> [after] [--before=label-or-id] [--after=label-or-id|current] [--limit=N]\n"
                  << "\nContext actions:\n"
                  << "  bridge_asset_symbols [--asset-path=/Game/...] [--symbol=Name] [--limit=N] [--detail-level=minimal|standard]\n"
                  << "\nProject actions:\n"
                  << "  search <query> [--limit=N]\n"
                  << "  find_by_type <asset_class> [--limit=N] [--offset=N]\n"
                  << "  find_references <asset_path>\n"
                  << "  get_stats\n"
                  << "  get_asset_details <asset_path>\n"
                  << "  impact_radius <asset_path> [--direction=in|out|both] [--max-depth=N] [--max-results=N] [--dependency-type=Hard|Soft]\n"
                  << "  health [--include-counts=false]\n"
                  << "  repair_fts [--target=all|assets|nodes] [--execute]\n"
                  << "  repair_crg_cache [--scope=all] [--execute]\n"
                  << "  risk_score [asset_path] [--limit=N] [--min-tier=low|medium|high]\n"
                  << "  review_hotspots [--kind=fan_in|fan_out|risk|large|all] [--limit=N] [--min-lines=N] [--include-questions=false]\n"
                  << "  review_context <asset_path> [--direction=in|out|both] [--detail-level=minimal|standard]\n"
                  << "  detect_changes <path...> [--changed-paths=a,b] [--ranges=path:s-e,...] [--diff-file=PATH] [--diff-stdin] [--max-results=N] [--detail-level=minimal|standard]\n"
                  << "  find_unused [--kind=<asset_class>] [--limit=N] [--min-confidence=low|medium|high]\n"
                  << "  pre_merge_check <path...> [--changed-paths=a,b] [--max-results=N] [--unused-limit=N] [--detail-level=minimal|standard] [--include-unused=false]\n"
                  << "  snapshot [label] [--label=name] [--execute]\n"
                  << "  diff_snapshots <before> [after] [--before=label-or-id] [--after=label-or-id|current] [--limit=N]\n";
        std::exit(1);
    }

    args.ns = argv[1];
    args.action = argv[2];

    for (int i = 3; i < argc; ++i) {
        std::string a = argv[i];
        if (a.substr(0, 2) == "--") {
            std::string key, val;
            auto eq = a.find('=');
            if (eq != std::string::npos) {
                key = a.substr(2, eq - 2);
                val = a.substr(eq + 1);
            } else {
                key = a.substr(2);
                val = "";  // flag-style
            }
            // Normalize hyphens to underscores for consistency
            std::replace(key.begin(), key.end(), '-', '_');
            args.options[key] = val;
        } else {
            args.positional.push_back(a);
        }
    }

    return args;
}

static std::vector<std::string> collect_changed_paths(const Args& args) {
    std::vector<std::string> paths = args.positional;
    std::string csv = args.opt("changed_paths");
    if (!csv.empty()) {
        std::stringstream ss(csv);
        std::string path;
        while (std::getline(ss, path, ',')) {
            if (!path.empty()) paths.push_back(path);
        }
    }
    return paths;
}

// RX-1.1: unified-diff parsing (port of code_review_graph/changes.py
// _parse_unified_diff). Pure text; no VCS shell-out — the caller produces
// the diff (parent RX-1 contract; CRG incremental.py is the caller's job).
static std::map<std::string, std::vector<std::pair<int,int>>>
parse_unified_diff_ranges(const std::string& diff_text) {
    std::map<std::string, std::vector<std::pair<int,int>>> ranges;
    std::stringstream ss(diff_text);
    std::string line, current;
    bool have_file = false;
    while (std::getline(ss, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.rfind("+++ b/", 0) == 0) {
            current = line.substr(6);
            size_t tab = current.find('\t');
            if (tab != std::string::npos) current = current.substr(0, tab);
            std::replace(current.begin(), current.end(), '\\', '/');
            have_file = !current.empty();
            continue;
        }
        if (!have_file || line.rfind("@@ ", 0) != 0) continue;
        size_t plus = line.find('+');
        if (plus == std::string::npos || plus + 1 >= line.size()) continue;
        size_t i = plus + 1;
        auto read_int = [&]() -> int {
            int v = 0; bool any = false;
            while (i < line.size() && line[i] >= '0' && line[i] <= '9') {
                v = v * 10 + (line[i] - '0'); ++i; any = true;
            }
            return any ? v : -1;
        };
        int start = read_int();
        if (start <= 0) continue;
        int count = 1;
        if (i < line.size() && line[i] == ',') {
            ++i;
            int p = read_int();
            count = (p >= 0) ? p : 1;
        }
        int end = (count == 0) ? start : (start + count - 1);
        ranges[current].push_back({start, end});
    }
    return ranges;
}

// VCS-agnostic line ranges from --diff-file=PATH, --diff-stdin, and/or
// --ranges=path:start-end[,path2:s-e]. Read-only, no shell-out.
static std::map<std::string, std::vector<std::pair<int,int>>>
collect_changed_ranges(const Args& args) {
    std::map<std::string, std::vector<std::pair<int,int>>> ranges;
    std::string diff_text;
    std::string df = args.opt("diff_file");
    if (!df.empty()) {
        std::ifstream f(df, std::ios::binary);
        if (f) { std::stringstream b; b << f.rdbuf(); diff_text = b.str(); }
    }
    if (args.options.count("diff_stdin")) {
        std::stringstream b; b << std::cin.rdbuf(); diff_text += b.str();
    }
    if (!diff_text.empty()) ranges = parse_unified_diff_ranges(diff_text);

    std::string spec = args.opt("ranges");
    if (!spec.empty()) {
        std::stringstream ss(spec);
        std::string token;
        while (std::getline(ss, token, ',')) {
            size_t colon = token.rfind(':');
            if (colon == std::string::npos) continue;
            std::string path = token.substr(0, colon);
            std::string span = token.substr(colon + 1);
            size_t dash = span.find('-');
            if (dash == std::string::npos) continue;
            try {
                int s = std::stoi(span.substr(0, dash));
                int e = std::stoi(span.substr(dash + 1));
                std::replace(path.begin(), path.end(), '\\', '/');
                if (s > 0 && e >= s && !path.empty()) ranges[path].push_back({s, e});
            } catch (...) {}
        }
    }
    return ranges;
}

// ============================================================
// Path utilities
// ============================================================

static std::string short_path(const std::string& full_path) {
    static const char* markers[] = {
        "Engine\\Source\\", "Engine/Source/",
        "Engine\\Shaders\\", "Engine/Shaders/"
    };
    for (auto m : markers) {
        auto idx = full_path.find(m);
        if (idx != std::string::npos)
            return full_path.substr(idx);
    }
    return full_path;
}

static std::string read_file_lines(const std::string& file_path, int start, int end) {
    std::ifstream f(file_path);
    if (!f.is_open())
        return "[File not found: " + file_path + "]";

    std::vector<std::string> lines;
    std::string line;
    while (std::getline(f, line))
        lines.push_back(line);

    start = std::max(1, start);
    end = std::min((int)lines.size(), end);

    std::ostringstream out;
    for (int i = start - 1; i < end; ++i) {
        // Trim trailing whitespace
        std::string& l = lines[i];
        while (!l.empty() && (l.back() == '\r' || l.back() == '\n' || l.back() == ' '))
            l.pop_back();

        char buf[16];
        std::snprintf(buf, sizeof(buf), "%5d", i + 1);
        out << buf << " | " << l;
        if (i < end - 1) out << "\n";
    }
    return out.str();
}

// ============================================================
// Context bridge utilities
// ============================================================

static bool iequals(const std::string& a, const std::string& b) {
    return lower_copy(a) == lower_copy(b);
}

static bool bridge_starts_with_any_prefix(const std::string& value,
                                          const std::vector<std::string>& prefixes,
                                          std::string& out_trimmed) {
    std::string lower = lower_copy(value);
    for (const auto& prefix : prefixes) {
        std::string lower_prefix = lower_copy(prefix);
        if (lower.rfind(lower_prefix, 0) == 0 && value.size() > prefix.size()) {
            out_trimmed = value.substr(prefix.size());
            return true;
        }
    }
    return false;
}

static std::string bridge_clean_token(std::string value) {
    value = trim_copy(value);
    std::replace(value.begin(), value.end(), '\\', '/');
    for (char sep : {'/', '.', ':'}) {
        size_t idx = value.find_last_of(sep);
        if (idx != std::string::npos && idx + 1 < value.size())
            value = value.substr(idx + 1);
    }
    if (value.size() > 2 && iequals(value.substr(value.size() - 2), "_C"))
        value.resize(value.size() - 2);
    return value;
}

static bool bridge_is_generic_asset_class(const std::string& value) {
    static const std::set<std::string> generic = {
        "blueprint", "widgetblueprint", "animblueprint", "dataasset",
        "primarydataasset", "material", "materialinstanceconstant",
        "texture2d", "staticmesh", "skeletalmesh"
    };
    return generic.count(lower_copy(value)) > 0;
}

static std::string bridge_normalize_name(const std::string& value) {
    std::string normalized = bridge_clean_token(value);
    static const std::vector<std::string> prefixes = {
        "BP_", "B_", "WBP_", "ABP_", "DA_", "PDA_", "GA_", "GE_", "GC_", "GCN_"
    };
    bool trimmed = true;
    while (trimmed) {
        std::string next;
        trimmed = bridge_starts_with_any_prefix(normalized, prefixes, next);
        if (trimmed) normalized = next;
    }
    if (normalized.size() > 2) {
        char first = normalized[0];
        char second = normalized[1];
        if ((first == 'U' || first == 'A' || first == 'F' || first == 'I') &&
            std::isupper(static_cast<unsigned char>(second))) {
            normalized = normalized.substr(1);
        }
    }
    return normalized;
}

static void bridge_add_unique(std::vector<std::string>& out,
                              std::set<std::string>& seen,
                              const std::string& candidate) {
    std::string cleaned = trim_copy(bridge_clean_token(candidate));
    if (cleaned.empty()) return;
    std::string key = lower_copy(cleaned);
    if (seen.insert(key).second) out.push_back(cleaned);
}

static std::vector<std::string> bridge_asset_symbol_candidates(const std::string& asset_path,
                                                               const std::string& asset_name,
                                                               const std::string& asset_class) {
    std::vector<std::string> candidates;
    std::set<std::string> seen;
    std::string clean_asset_name = bridge_clean_token(asset_name.empty() ? asset_path : asset_name);
    std::string clean_path_name = bridge_clean_token(asset_path);
    bridge_add_unique(candidates, seen, clean_asset_name);
    bridge_add_unique(candidates, seen, clean_path_name);

    std::string normalized = bridge_normalize_name(clean_asset_name);
    bridge_add_unique(candidates, seen, normalized);
    if (!normalized.empty()) {
        bridge_add_unique(candidates, seen, "U" + normalized);
        bridge_add_unique(candidates, seen, "A" + normalized);
        bridge_add_unique(candidates, seen, "F" + normalized);
    }

    std::string clean_class = bridge_clean_token(asset_class);
    if (!clean_class.empty() && !bridge_is_generic_asset_class(clean_class)) {
        bridge_add_unique(candidates, seen, clean_class);
        bridge_add_unique(candidates, seen, bridge_normalize_name(clean_class));
    }
    return candidates;
}

static std::vector<std::string> bridge_symbol_asset_candidates(const std::string& symbol_name,
                                                               const std::string& qualified_name) {
    std::vector<std::string> candidates;
    std::set<std::string> seen;
    std::string clean_symbol = bridge_clean_token(symbol_name.empty() ? qualified_name : symbol_name);
    std::string clean_qualified = bridge_clean_token(qualified_name);
    bridge_add_unique(candidates, seen, clean_symbol);
    bridge_add_unique(candidates, seen, clean_qualified);

    std::string normalized = bridge_normalize_name(clean_symbol);
    bridge_add_unique(candidates, seen, normalized);
    if (!normalized.empty()) {
        bridge_add_unique(candidates, seen, "BP_" + normalized);
        bridge_add_unique(candidates, seen, "WBP_" + normalized);
        bridge_add_unique(candidates, seen, "DA_" + normalized);
        bridge_add_unique(candidates, seen, "GA_" + normalized);
        bridge_add_unique(candidates, seen, "GE_" + normalized);
    }
    return candidates;
}

static bool bridge_names_match_normalized(const std::string& left, const std::string& right) {
    return iequals(bridge_normalize_name(left), bridge_normalize_name(right));
}

static std::string bridge_source_file_path(Database& db, int file_id) {
    auto rows = query(db, "SELECT path FROM files WHERE id = ?", {std::to_string(file_id)});
    return rows.empty() ? "" : rows[0].get("path");
}

static json bridge_asset_object(const Row& asset,
                                const std::string& fallback_path = "",
                                const std::string& fallback_name = "",
                                const std::string& fallback_class = "") {
    return {
        {"asset_path", asset.cols.empty() ? fallback_path : asset.get("package_path")},
        {"asset_name", asset.cols.empty() ? fallback_name : asset.get("asset_name")},
        {"asset_class", asset.cols.empty() ? fallback_class : asset.get("asset_class")},
        {"module_name", asset.cols.empty() ? "" : asset.get("module_name")},
    };
}

static json bridge_symbol_object(Database& db, const Row& symbol, bool standard) {
    json result = {
        {"id", symbol.get_int64("id")},
        {"name", symbol.get("name")},
        {"qualified_name", symbol.get("qualified_name")},
        {"kind", symbol.get("kind")},
        {"line_start", symbol.get_int("line_start")},
        {"line_end", symbol.get_int("line_end")},
        {"path", short_path(bridge_source_file_path(db, symbol.get_int("file_id")))},
    };
    if (standard) {
        result["signature"] = symbol.get("signature");
        result["docstring"] = symbol.get("docstring");
        result["access"] = symbol.get("access");
        result["is_ue_macro"] = symbol.get_int("is_ue_macro") != 0;
    }
    return result;
}

static std::string bridge_confidence(double score) {
    if (score >= 0.80) return "high";
    if (score >= 0.58) return "medium";
    return "low";
}

struct BridgeLinkCandidate {
    json object;
    double score = 0.0;
    std::string key;
};

static void bridge_add_link(std::vector<BridgeLinkCandidate>& links,
                            std::set<std::string>& seen,
                            const json& asset,
                            const json& symbol,
                            const std::string& direction,
                            double score,
                            const std::vector<std::string>& reasons) {
    std::string asset_path = json_string_field(asset, "asset_path", "");
    std::string symbol_name = json_string_field(symbol, "qualified_name",
        json_string_field(symbol, "name", ""));
    std::string key = lower_copy(direction + "|" + asset_path + "|" + symbol_name);

    json link = {
        {"direction", direction},
        {"confidence", bridge_confidence(score)},
        {"score", score},
        {"reasons", reasons},
        {"asset", asset},
        {"symbol", symbol},
    };
    if (!seen.insert(key).second) {
        auto existing = std::find_if(links.begin(), links.end(),
            [&](const BridgeLinkCandidate& candidate) { return candidate.key == key; });
        if (existing != links.end() && score > existing->score) {
            existing->object = link;
            existing->score = score;
        }
        return;
    }
    links.push_back({link, score, key});
}

static Rows bridge_source_symbols_exact(Database& db, const std::string& name, int limit) {
    return query(db,
        "SELECT id, name, qualified_name, kind, file_id, line_start, line_end, "
        "parent_symbol_id, access, signature, docstring, is_ue_macro "
        "FROM symbols WHERE lower(name) = lower(?) "
        "ORDER BY (line_end > line_start) DESC LIMIT " + std::to_string(limit),
        {name});
}

static Rows bridge_source_symbols_fts(Database& db, const std::string& name, int limit) {
    return query(db,
        "SELECT s.id, s.name, s.qualified_name, s.kind, s.file_id, s.line_start, s.line_end, "
        "s.parent_symbol_id, s.access, s.signature, s.docstring, s.is_ue_macro "
        "FROM symbols_fts f JOIN symbols s ON s.id = f.rowid "
        "WHERE symbols_fts MATCH ? ORDER BY bm25(symbols_fts) LIMIT " + std::to_string(limit),
        {escape_fts(name)});
}

static Row bridge_asset_by_path(Database& db, const std::string& path) {
    auto rows = query(db,
        "SELECT id, package_path, asset_name, asset_class, module_name "
        "FROM assets WHERE package_path = ? LIMIT 1",
        {path});
    return rows.empty() ? Row{} : rows[0];
}

static Rows bridge_project_assets_search(Database& db, const std::string& candidate, int limit) {
    Rows exact = query(db,
        "SELECT id, package_path, asset_name, asset_class, module_name "
        "FROM assets WHERE lower(asset_name) = lower(?) LIMIT " + std::to_string(limit),
        {candidate});
    Rows fts = query(db,
        "SELECT a.id, a.package_path, a.asset_name, a.asset_class, a.module_name "
        "FROM fts_assets f JOIN assets a ON a.id = f.rowid "
        "WHERE fts_assets MATCH ? ORDER BY rank LIMIT " + std::to_string(limit),
        {escape_fts(candidate)});

    Rows rows;
    std::set<std::string> seen;
    auto add_unique = [&](const Row& row) {
        if ((int)rows.size() >= limit) return;
        std::string key = row.get("id");
        if (key.empty()) key = lower_copy(row.get("package_path"));
        if (key.empty() || !seen.insert(key).second) return;
        rows.push_back(row);
    };
    for (const auto& row : exact) add_unique(row);
    for (const auto& row : fts) add_unique(row);
    return rows;
}

static void bridge_add_source_matches_for_candidate(Database& source_db,
                                                    const json& asset,
                                                    const std::string& asset_name,
                                                    const std::string& candidate,
                                                    int limit,
                                                    bool standard,
                                                    std::vector<BridgeLinkCandidate>& links,
                                                    std::set<std::string>& seen) {
    if (candidate.empty()) return;
    for (const auto& symbol : bridge_source_symbols_exact(source_db, candidate, limit)) {
        std::vector<std::string> reasons = {"exact source symbol name match: " + candidate};
        double score = 0.90;
        if (bridge_names_match_normalized(asset_name, symbol.get("name"))) {
            reasons.push_back("normalized UE asset/symbol names match");
            score = 0.95;
        }
        bridge_add_link(links, seen, asset, bridge_symbol_object(source_db, symbol, standard),
                        "asset_to_symbol", score, reasons);
    }
    for (const auto& symbol : bridge_source_symbols_fts(source_db, candidate, limit)) {
        std::vector<std::string> reasons;
        double score = 0.48;
        if (iequals(symbol.get("name"), candidate)) {
            reasons.push_back("FTS result has exact symbol name: " + candidate);
            score = 0.82;
        } else if (bridge_names_match_normalized(asset_name, symbol.get("name"))) {
            reasons.push_back("FTS result normalized name matches the asset");
            score = 0.72;
        } else {
            reasons.push_back("source FTS fallback matched candidate: " + candidate);
        }
        bridge_add_link(links, seen, asset, bridge_symbol_object(source_db, symbol, standard),
                        "asset_to_symbol", score, reasons);
    }
}

static void bridge_add_asset_matches_for_symbol(Database& project_db,
                                                Database& source_db,
                                                const Row& symbol,
                                                const std::string& candidate,
                                                int limit,
                                                bool standard,
                                                std::vector<BridgeLinkCandidate>& links,
                                                std::set<std::string>& seen) {
    if (candidate.empty()) return;
    for (const auto& asset : bridge_project_assets_search(project_db, candidate, limit)) {
        std::vector<std::string> reasons;
        double score = 0.45;
        if (iequals(asset.get("asset_name"), candidate)) {
            reasons.push_back("exact project asset name match: " + candidate);
            score = 0.88;
        } else if (bridge_names_match_normalized(asset.get("asset_name"), symbol.get("name"))) {
            reasons.push_back("normalized UE symbol/asset names match");
            score = 0.74;
        } else {
            reasons.push_back("project index FTS fallback matched candidate: " + candidate);
        }
        bridge_add_link(links, seen, bridge_asset_object(asset),
                        bridge_symbol_object(source_db, symbol, standard),
                        "symbol_to_asset", score, reasons);
    }
}

// ============================================================
// Source actions
// ============================================================

class SourceActions {
    Database db;

public:
    void open(const std::string& path, bool query_only = true) { db.open(path, query_only); }

    std::string get_file_path(int file_id) {
        auto rows = query(db, "SELECT path FROM files WHERE id = ?", {std::to_string(file_id)});
        return rows.empty() ? "<unknown>" : rows[0].get("path");
    }

    // --- search_source ---
    void search_source(const Args& args) {
        if (args.positional.empty()) die("search_source requires a query argument");
        std::string q = args.positional[0];
        int limit = args.opt_int("limit", 20);
        std::string module = args.opt("module");
        std::string kind = args.opt("kind");
        std::string fts_q = escape_fts(q);

        std::ostringstream out;

        // Symbol FTS search
        {
            std::string sql = "SELECT s.id, s.name, s.qualified_name, s.kind, s.file_id, "
                              "s.line_start, s.line_end, s.access, s.signature, s.docstring "
                              "FROM symbols_fts f JOIN symbols s ON s.id = f.rowid";
            std::vector<std::string> conditions = {"symbols_fts MATCH ?"};
            std::vector<std::string> params = {fts_q};

            if (!module.empty()) {
                sql += " JOIN files fi ON fi.id = s.file_id JOIN modules m ON m.id = fi.module_id";
                conditions.push_back("m.name = ?");
                params.push_back(module);
            }
            if (!kind.empty()) {
                conditions.push_back("s.kind = ?");
                params.push_back(kind);
            }

            sql += " WHERE ";
            for (size_t i = 0; i < conditions.size(); ++i) {
                if (i > 0) sql += " AND ";
                sql += conditions[i];
            }
            sql += " ORDER BY bm25(symbols_fts) LIMIT " + std::to_string(limit);

            auto rows = query(db, sql, params);
            if (!rows.empty()) {
                out << "=== Symbol Matches ===\n";
                for (auto& r : rows) {
                    std::string fp = short_path(get_file_path(r.get_int("file_id")));
                    out << "  [" << r.get("kind") << "] " << r.get("qualified_name")
                        << " (" << fp << ":" << r.get("line_start") << ")\n";
                    std::string sig = r.get("signature");
                    if (!sig.empty())
                        out << "         " << sig << "\n";
                }
            }
        }

        // Source line FTS search
        {
            std::string sql = "SELECT sf.file_id, sf.line_number, sf.text FROM source_fts sf";
            std::vector<std::string> conditions = {"source_fts MATCH ?"};
            std::vector<std::string> params = {fts_q};

            if (!module.empty()) {
                sql += " JOIN files fi ON fi.id = sf.file_id JOIN modules m ON m.id = fi.module_id";
                conditions.push_back("m.name = ?");
                params.push_back(module);
            }

            sql += " WHERE ";
            for (size_t i = 0; i < conditions.size(); ++i) {
                if (i > 0) sql += " AND ";
                sql += conditions[i];
            }
            sql += " ORDER BY bm25(source_fts) LIMIT " + std::to_string(limit);

            auto rows = query(db, sql, params);
            if (!rows.empty()) {
                out << "\n=== Source Line Matches ===\n";
                std::set<std::pair<int, int>> seen;
                for (auto& r : rows) {
                    int fid = r.get_int("file_id");
                    int ln = r.get_int("line_number");
                    if (seen.count({fid, ln})) continue;
                    seen.insert({fid, ln});

                    std::string fp = short_path(get_file_path(fid));
                    std::string text = r.get("text");
                    // Trim and truncate
                    while (!text.empty() && std::isspace((unsigned char)text.front())) text.erase(text.begin());
                    if (text.size() > 120) text = text.substr(0, 120) + "...";
                    out << "  " << fp << ":" << ln << "\n";
                    out << "    " << text << "\n";
                }
            }
        }

        std::string result = out.str();
        if (result.empty())
            std::cout << "No results found for '" << q << "'." << std::endl;
        else
            std::cout << result;
    }

    // --- read_source ---
    void read_source(const Args& args) {
        if (args.positional.empty()) die("read_source requires a symbol argument");
        std::string symbol = args.positional[0];
        int max_lines = args.opt_int("max_lines", 0);
        bool include_header = !args.options.count("no_header");
        // bool members_only = args.opt_bool("members_only"); // reserved for future use

        // Exact name lookup
        auto rows = query(db, "SELECT * FROM symbols WHERE name = ? ORDER BY (line_end > line_start) DESC", {symbol});
        if (rows.empty()) {
            std::string fts_q = escape_fts(symbol);
            rows = query(db, "SELECT s.* FROM symbols_fts f JOIN symbols s ON s.id = f.rowid "
                             "WHERE symbols_fts MATCH ? ORDER BY bm25(symbols_fts) LIMIT 5", {fts_q});
        }
        if (rows.empty()) die("No symbol found matching '" + symbol + "'.");

        std::vector<std::string> parts;
        std::set<std::tuple<int, int, int>> seen;

        for (auto& r : rows) {
            int fid = r.get_int("file_id");
            int ls = r.get_int("line_start");
            int le = r.get_int("line_end");
            if (seen.count({fid, ls, le})) continue;
            seen.insert({fid, ls, le});

            std::string fp = get_file_path(fid);
            if (!include_header && fp.size() > 2 && fp.substr(fp.size() - 2) == ".h")
                continue;

            std::string header = "--- " + short_path(fp) + " (lines " +
                                 std::to_string(ls) + "-" + std::to_string(le) + ") ---";
            std::string source = read_file_lines(fp, ls, le);
            parts.push_back(header + "\n" + source);
        }

        std::string result;
        if (parts.empty()) {
            result = "Found symbol '" + symbol + "' but could not read source.";
        } else {
            for (size_t i = 0; i < parts.size(); ++i) {
                if (i > 0) result += "\n\n";
                result += parts[i];
            }
        }

        if (max_lines > 0) {
            std::istringstream iss(result);
            std::string line;
            std::vector<std::string> lines;
            while (std::getline(iss, line)) lines.push_back(line);
            if ((int)lines.size() > max_lines) {
                int remaining = (int)lines.size() - max_lines;
                lines.resize(max_lines);
                std::ostringstream trunc;
                for (auto& l : lines) trunc << l << "\n";
                trunc << "[...truncated, " << remaining << " more lines]";
                result = trunc.str();
            }
        }

        std::cout << result << std::endl;
    }

    // --- find_references ---
    void find_references(const Args& args) {
        if (args.positional.empty()) die("find_references requires a symbol argument");
        std::string symbol = args.positional[0];
        std::string ref_kind = args.opt("ref_kind");
        int limit = args.opt_int("limit", 50);

        auto sym_rows = query(db, "SELECT id, name FROM symbols WHERE name = ?", {symbol});
        if (sym_rows.empty()) {
            std::string fts_q = escape_fts(symbol);
            sym_rows = query(db, "SELECT s.id, s.name FROM symbols_fts f JOIN symbols s ON s.id = f.rowid "
                                 "WHERE symbols_fts MATCH ? LIMIT 5", {fts_q});
        }
        if (sym_rows.empty()) die("No symbol found matching '" + symbol + "'.");

        std::vector<std::string> lines;
        for (auto& sym : sym_rows) {
            Rows refs;
            if (!ref_kind.empty()) {
                refs = query(db,
                    "SELECT r.ref_kind, r.line, s.name as from_name, f.path "
                    "FROM \"references\" r JOIN symbols s ON s.id = r.from_symbol_id "
                    "JOIN files f ON f.id = r.file_id WHERE r.to_symbol_id = ? AND r.ref_kind = ? LIMIT ?",
                    {sym.get("id"), ref_kind, std::to_string(limit)});
            } else {
                refs = query(db,
                    "SELECT r.ref_kind, r.line, s.name as from_name, f.path "
                    "FROM \"references\" r JOIN symbols s ON s.id = r.from_symbol_id "
                    "JOIN files f ON f.id = r.file_id WHERE r.to_symbol_id = ? LIMIT ?",
                    {sym.get("id"), std::to_string(limit)});
            }
            for (auto& ref : refs) {
                lines.push_back("[" + ref.get("ref_kind") + "] " + short_path(ref.get("path")) +
                                ":" + ref.get("line") + " (from " + ref.get("from_name") + ")");
            }
        }

        if (lines.empty())
            std::cout << "No references found for '" << symbol << "'." << std::endl;
        else
            for (auto& l : lines) std::cout << l << "\n";
    }

    // --- find_callers ---
    void find_callers(const Args& args) {
        if (args.positional.empty()) die("find_callers requires a symbol argument");
        std::string symbol = args.positional[0];
        int limit = args.opt_int("limit", 50);

        auto sym_rows = query(db, "SELECT id FROM symbols WHERE name = ? AND kind = 'function'", {symbol});
        if (sym_rows.empty()) {
            std::string fts_q = escape_fts(symbol);
            sym_rows = query(db,
                "SELECT s.id FROM symbols_fts f JOIN symbols s ON s.id = f.rowid "
                "WHERE symbols_fts MATCH ? AND s.kind = 'function' LIMIT 5", {fts_q});
        }
        if (sym_rows.empty()) die("No function found matching '" + symbol + "'.");

        std::vector<std::string> lines;
        for (auto& sym : sym_rows) {
            auto refs = query(db,
                "SELECT s.name as from_name, f.path, r.line "
                "FROM \"references\" r JOIN symbols s ON s.id = r.from_symbol_id "
                "JOIN files f ON f.id = r.file_id WHERE r.to_symbol_id = ? AND r.ref_kind = 'call' LIMIT ?",
                {sym.get("id"), std::to_string(limit)});
            for (auto& ref : refs)
                lines.push_back(ref.get("from_name") + " -- " + short_path(ref.get("path")) + ":" + ref.get("line"));
        }

        if (lines.empty())
            std::cout << "No callers found for '" << symbol << "'." << std::endl;
        else
            for (auto& l : lines) std::cout << l << "\n";
    }

    // --- find_callees ---
    void find_callees(const Args& args) {
        if (args.positional.empty()) die("find_callees requires a symbol argument");
        std::string symbol = args.positional[0];
        int limit = args.opt_int("limit", 50);

        auto sym_rows = query(db, "SELECT id FROM symbols WHERE name = ? AND kind = 'function'", {symbol});
        if (sym_rows.empty()) {
            std::string fts_q = escape_fts(symbol);
            sym_rows = query(db,
                "SELECT s.id FROM symbols_fts f JOIN symbols s ON s.id = f.rowid "
                "WHERE symbols_fts MATCH ? AND s.kind = 'function' LIMIT 5", {fts_q});
        }
        if (sym_rows.empty()) die("No function found matching '" + symbol + "'.");

        std::vector<std::string> lines;
        for (auto& sym : sym_rows) {
            auto refs = query(db,
                "SELECT s.name as to_name, f.path, r.line "
                "FROM \"references\" r JOIN symbols s ON s.id = r.to_symbol_id "
                "JOIN files f ON f.id = r.file_id WHERE r.from_symbol_id = ? AND r.ref_kind = 'call' LIMIT ?",
                {sym.get("id"), std::to_string(limit)});
            for (auto& ref : refs)
                lines.push_back(ref.get("to_name") + " -- " + short_path(ref.get("path")) + ":" + ref.get("line"));
        }

        if (lines.empty())
            std::cout << "No callees found for '" << symbol << "'." << std::endl;
        else
            for (auto& l : lines) std::cout << l << "\n";
    }

    // --- get_class_hierarchy ---
    void get_class_hierarchy(const Args& args) {
        if (args.positional.empty()) die("get_class_hierarchy requires a symbol argument");
        std::string symbol = args.positional[0];
        std::string direction = args.opt("direction", "both");
        int depth = args.opt_int("depth", 5);

        auto sym_rows = query(db,
            "SELECT id, name, file_id FROM symbols WHERE name = ? AND kind IN ('class','struct') "
            "ORDER BY (line_end > line_start) DESC", {symbol});
        if (sym_rows.empty()) {
            std::string fts_q = escape_fts(symbol);
            sym_rows = query(db,
                "SELECT s.id, s.name, s.file_id FROM symbols_fts f JOIN symbols s ON s.id = f.rowid "
                "WHERE symbols_fts MATCH ? AND s.kind IN ('class','struct') LIMIT 1", {fts_q});
        }
        if (sym_rows.empty()) die("No class/struct found matching '" + symbol + "'.");

        auto& sym = sym_rows[0];
        std::string fp = short_path(get_file_path(sym.get_int("file_id")));
        std::vector<std::string> lines;
        lines.push_back(sym.get("name") + " (" + fp + ")");

        std::set<std::string> visited;

        std::function<void(const std::string&, int, int)> walk_up;
        walk_up = [&](const std::string& sid, int indent, int max_d) {
            if (indent > max_d || visited.count(sid)) return;
            visited.insert(sid);
            auto parents = query(db,
                "SELECT s.id, s.name FROM inheritance i JOIN symbols s ON s.id = i.parent_id WHERE i.child_id = ?",
                {sid});
            for (auto& p : parents) {
                lines.push_back(std::string(indent * 2, ' ') + "<- " + p.get("name"));
                walk_up(p.get("id"), indent + 1, max_d);
            }
        };

        std::function<void(const std::string&, int, int)> walk_down;
        walk_down = [&](const std::string& sid, int indent, int max_d) {
            if (indent > max_d || visited.count(sid)) return;
            visited.insert(sid);
            auto children = query(db,
                "SELECT s.id, s.name FROM inheritance i JOIN symbols s ON s.id = i.child_id WHERE i.parent_id = ?",
                {sid});
            for (auto& c : children) {
                lines.push_back(std::string(indent * 2, ' ') + "-> " + c.get("name"));
                walk_down(c.get("id"), indent + 1, max_d);
            }
        };

        if (direction == "up" || direction == "both") {
            lines.push_back("\nAncestors:");
            size_t count_before = lines.size();
            visited.clear();
            walk_up(sym.get("id"), 1, depth);
            if (lines.size() == count_before)
                lines.push_back("  (none)");
        }

        if (direction == "down" || direction == "both") {
            lines.push_back("\nDescendants:");
            size_t count_before = lines.size();
            visited.clear();
            walk_down(sym.get("id"), 1, depth);
            if (lines.size() == count_before)
                lines.push_back("  (none)");
        }

        for (auto& l : lines) std::cout << l << "\n";
    }

    // --- get_module_info ---
    void get_module_info(const Args& args) {
        if (args.positional.empty()) die("get_module_info requires a module_name argument");
        std::string module_name = args.positional[0];

        auto mods = query(db, "SELECT id, name, path, module_type FROM modules WHERE name = ?", {module_name});
        if (mods.empty()) die("No module found matching '" + module_name + "'.");

        auto& mod = mods[0];
        auto file_count = query(db, "SELECT COUNT(*) as c FROM files WHERE module_id = ?", {mod.get("id")});
        auto kind_rows = query(db,
            "SELECT s.kind, COUNT(*) as cnt FROM symbols s JOIN files f ON f.id = s.file_id "
            "WHERE f.module_id = ? GROUP BY s.kind", {mod.get("id")});

        std::cout << "Module: " << mod.get("name") << "\n"
                  << "Path: " << short_path(mod.get("path")) << "\n"
                  << "Type: " << mod.get("module_type") << "\n"
                  << "Files: " << (file_count.empty() ? "0" : file_count[0].get("c")) << "\n"
                  << "\nSymbol counts by kind:\n";

        // Sort by kind name
        std::sort(kind_rows.begin(), kind_rows.end(),
                  [](const Row& a, const Row& b) { return a.get("kind") < b.get("kind"); });
        for (auto& kr : kind_rows)
            std::cout << "  " << kr.get("kind") << ": " << kr.get("cnt") << "\n";

        auto key_classes = query(db,
            "SELECT s.name, s.line_start FROM symbols s JOIN files f ON f.id = s.file_id "
            "JOIN modules m ON m.id = f.module_id WHERE m.name = ? AND s.kind = 'class' LIMIT 20",
            {module_name});
        if (!key_classes.empty()) {
            std::cout << "\nKey classes:\n";
            for (auto& c : key_classes)
                std::cout << "  " << c.get("name") << " (line " << c.get("line_start") << ")\n";
        }
    }

    // --- get_symbol_context ---
    void get_symbol_context(const Args& args) {
        if (args.positional.empty()) die("get_symbol_context requires a symbol argument");
        std::string symbol = args.positional[0];
        int ctx_lines = args.opt_int("context_lines", 10);

        auto rows = query(db, "SELECT * FROM symbols WHERE name = ? ORDER BY (line_end > line_start) DESC", {symbol});
        if (rows.empty()) {
            std::string fts_q = escape_fts(symbol);
            rows = query(db,
                "SELECT s.* FROM symbols_fts f JOIN symbols s ON s.id = f.rowid "
                "WHERE symbols_fts MATCH ? LIMIT 5", {fts_q});
        }
        if (rows.empty()) die("No symbol found matching '" + symbol + "'.");

        std::vector<std::string> parts;
        int count = 0;
        for (auto& r : rows) {
            if (count++ >= 3) break;

            std::string fp = get_file_path(r.get_int("file_id"));
            int ls = r.get_int("line_start");
            int le = r.get_int("line_end");
            int ctx_start = std::max(1, ls - ctx_lines);
            int ctx_end = le + ctx_lines;

            std::ostringstream part;
            part << "--- " << r.get("qualified_name") << " ---\n";
            part << "File: " << short_path(fp) << " (lines " << ls << "-" << le << ")\n";
            std::string sig = r.get("signature");
            if (!sig.empty()) part << "Signature: " << sig << "\n";
            std::string doc = r.get("docstring");
            if (!doc.empty()) part << "Docstring: " << doc << "\n";
            part << "\n" << read_file_lines(fp, ctx_start, ctx_end);
            parts.push_back(part.str());
        }

        for (size_t i = 0; i < parts.size(); ++i) {
            if (i > 0) std::cout << "\n\n";
            std::cout << parts[i];
        }
        std::cout << std::endl;
    }

    // --- read_file ---
    void read_file(const Args& args) {
        if (args.positional.empty()) die("read_file requires a file_path argument");
        std::string file_path = args.positional[0];
        int start = args.opt_int("start", 1);
        int end = args.opt_int("end", 0);

        std::string resolved;

        if (fs::exists(file_path)) {
            resolved = file_path;
        } else {
            // Normalize slashes to backslash for DB lookup
            std::string normalized = file_path;
            std::replace(normalized.begin(), normalized.end(), '/', '\\');

            auto rows = query(db, "SELECT path FROM files WHERE path = ?", {normalized});
            if (rows.empty())
                rows = query(db, "SELECT path FROM files WHERE path LIKE ? LIMIT 1", {"%" + normalized});

            if (!rows.empty())
                resolved = rows[0].get("path");
        }

        if (resolved.empty()) die("No file found matching '" + file_path + "'.");

        if (end <= 0) end = start + 199;

        std::cout << "--- " << short_path(resolved) << " (lines " << start << "-" << end << ") ---\n";
        std::cout << read_file_lines(resolved, start, end) << std::endl;
    }

    Rows symbols_by_name(const std::string& symbol, int limit = 5) {
        auto rows = query(db,
            "SELECT id, name, qualified_name, kind, file_id, line_start, line_end, "
            "parent_symbol_id, access, signature, docstring, is_ue_macro "
            "FROM symbols WHERE name = ? ORDER BY (line_end > line_start) DESC LIMIT " + std::to_string(limit),
            {symbol});
        if (rows.empty()) {
            rows = query(db,
                "SELECT s.id, s.name, s.qualified_name, s.kind, s.file_id, s.line_start, s.line_end, "
                "s.parent_symbol_id, s.access, s.signature, s.docstring, s.is_ue_macro "
                "FROM symbols_fts f JOIN symbols s ON s.id = f.rowid "
                "WHERE symbols_fts MATCH ? ORDER BY bm25(symbols_fts) LIMIT " + std::to_string(limit),
                {escape_fts(symbol)});
        }
        return rows;
    }

    Row symbol_by_id(const std::string& id) {
        auto rows = query(db,
            "SELECT id, name, qualified_name, kind, file_id, line_start, line_end, "
            "parent_symbol_id, access, signature, docstring, is_ue_macro FROM symbols WHERE id = ?",
            {id});
        return rows.empty() ? Row{} : rows[0];
    }

    json symbol_json(const Row& r, int depth = -1) {
        json o = {
            {"id", r.get_int64("id")},
            {"name", r.get("name")},
            {"qualified_name", r.get("qualified_name")},
            {"kind", r.get("kind")},
            {"file", short_path(get_file_path(r.get_int("file_id")))},
            {"line_start", r.get_int("line_start")},
            {"line_end", r.get_int("line_end")},
        };
        if (depth >= 0) o["depth"] = depth;
        std::string sig = r.get("signature");
        if (!sig.empty()) o["signature"] = sig;
        return o;
    }

    json source_health_json(bool include_counts) {
        json root = {
            {"input", {{"include_counts", include_counts}}},
            {"limits", {{"include_counts", include_counts}}},
            {"checks", json::array()},
            {"warnings", json::array()},
            {"truncated", false},
        };
        auto check = [&](const std::string& name, bool pass, const std::string& detail) {
            root["checks"].push_back({{"check", name}, {"result", pass ? "ok" : "warning"}, {"detail", detail}});
            if (!pass) root["warnings"].push_back(detail);
        };

        for (const char* t : {"modules", "files", "symbols", "inheritance", "references", "includes", "meta"})
            check(std::string("table:") + t, object_exists(db, "table", t), object_exists(db, "table", t) ? std::string("table ") + t + " present" : std::string("missing table ") + t);
        for (const char* f : {"symbols_fts", "source_fts"})
            check(std::string("fts:") + f, object_exists(db, "table", f), object_exists(db, "table", f) ? std::string("FTS table ") + f + " present" : std::string("missing FTS table ") + f);
        for (const char* tr : {"symbols_ai", "symbols_ad"})
            check(std::string("trigger:") + tr, object_exists(db, "trigger", tr), object_exists(db, "trigger", tr) ? std::string("trigger ") + tr + " present" : std::string("missing trigger ") + tr + " (symbols_fts may drift)");

        std::string schema_ver = scalar_str(db, "SELECT value FROM meta WHERE key = 'schema_version';");
        check("meta:schema_version", schema_ver == "1", schema_ver.empty() ? "meta.schema_version missing" : "schema_version=" + schema_ver + " (expected 1)");
        int64_t orphan_refs = count_rows(db,
            "SELECT COUNT(*) FROM \"references\" r "
            "WHERE r.from_symbol_id NOT IN (SELECT id FROM symbols) "
            "OR r.to_symbol_id NOT IN (SELECT id FROM symbols);");
        check("integrity:orphan_references", orphan_refs == 0, orphan_refs == 0 ? "no orphan reference rows" : std::to_string(orphan_refs) + " orphan reference row(s)");

        int64_t sym_cnt = count_rows(db, "SELECT COUNT(*) FROM symbols;");
        int64_t sym_fts_cnt = count_rows(db, "SELECT COUNT(*) FROM symbols_fts;");
        check("fts:symbols_row_parity", sym_cnt == sym_fts_cnt,
            "symbols=" + std::to_string(sym_cnt) + " symbols_fts=" + std::to_string(sym_fts_cnt) + (sym_cnt == sym_fts_cnt ? "" : " (mismatch -> source.repair_fts target=symbols)"));
        int64_t source_fts_cnt = count_rows(db, "SELECT COUNT(*) FROM source_fts;");
        root["checks"].push_back({{"check", "fts:source_fts_info"}, {"result", "info"}, {"detail", "source_fts rows=" + std::to_string(source_fts_cnt) + " (plain fts5; not rebuildable; reindex to repair)"}});
        root["schema"] = {{"schema_version", schema_ver}, {"journal_mode", scalar_str(db, "PRAGMA journal_mode;")}};
        if (include_counts) {
            root["row_counts"] = {
                {"symbols", sym_cnt},
                {"references", count_rows(db, "SELECT COUNT(*) FROM \"references\";")},
                {"inheritance", count_rows(db, "SELECT COUNT(*) FROM inheritance;")},
                {"source_fts", source_fts_cnt},
            };
        }
        // RX-2: CRG projection-cache health parity (mirrors editor ComputeHealth).
        // valid edges use the symbols-joined ref count so parity matches the
        // editor's dangling-ref-corrected rebuild.
        int64_t valid_refs = count_rows(db,
            "SELECT COUNT(*) FROM \"references\" r "
            "JOIN symbols fs ON fs.id = r.from_symbol_id "
            "JOIN symbols ts ON ts.id = r.to_symbol_id;");
        int64_t inh_cnt = count_rows(db, "SELECT COUNT(*) FROM inheritance;");
        append_crg_health_checks(db, "source", sym_cnt, valid_refs + inh_cnt, root);
        root["status"] = root["warnings"].empty() ? "ok" : "warning";
        root["summary"] = root["warnings"].empty() ? "EngineSource schema, triggers, symbols_fts parity, CRG cache and integrity OK" : std::to_string(root["warnings"].size()) + " health warning(s)";
        add_next(root, {"source.repair_crg_cache", "source.repair_fts", "source.trigger_project_reindex", "source.search_source"});
        return root;
    }

    void health(const Args& args) {
        print_json(source_health_json(args.opt_bool("include_counts", true)));
    }

    json source_repair_fts_json(const std::string& target, bool execute) {
        std::string t = target.empty() ? "all" : target;
        json root = {
            {"input", {{"target", t}, {"execute", execute}}},
            {"limits", {{"target", t}, {"execute", execute}}},
            {"warnings", json::array()},
            {"plan", json::array()},
            {"truncated", false},
        };
        if (t != "all" && t != "symbols" && t != "source") {
            root["status"] = "error";
            root["summary"] = "Unknown target '" + t + "' (expected all|symbols|source)";
            add_next(root, {"source.repair_fts", "source.health"});
            return root;
        }
        bool do_symbols = (t == "all" || t == "symbols");
        bool asked_source = (t == "all" || t == "source");
        json before = json::object();
        if (do_symbols) before["symbols_fts"] = count_rows(db, "SELECT COUNT(*) FROM symbols_fts;");
        root["before"] = before;
        if (do_symbols) root["plan"].push_back("INSERT INTO symbols_fts(symbols_fts) VALUES('rebuild');");
        if (asked_source) root["warnings"].push_back("source_fts is a plain fts5 table (no backing content); run source.trigger_reindex / trigger_project_reindex to repopulate source line search.");
        if (!execute) {
            root["status"] = "ok";
            root["summary"] = do_symbols ? "Dry-run: symbols_fts would be rebuilt. Pass --execute to apply." : "Dry-run: nothing rebuildable for this target.";
            root["after"] = json::object();
            add_next(root, {"source.repair_fts", "source.health"});
            return root;
        }
        if (do_symbols) {
            db.exec("BEGIN;");
            db.exec("INSERT INTO symbols_fts(symbols_fts) VALUES('rebuild');");
            db.exec("COMMIT;");
        }
        json after = json::object();
        if (do_symbols) after["symbols_fts"] = count_rows(db, "SELECT COUNT(*) FROM symbols_fts;");
        root["after"] = after;
        root["status"] = "ok";
        root["summary"] = do_symbols ? "Rebuilt symbols_fts" : "Nothing rebuilt; see warnings for source_fts reindex guidance";
        add_next(root, {"source.health", "source.search_symbols"});
        return root;
    }

    void repair_fts(const Args& args) {
        print_json(source_repair_fts_json(args.opt("target", "all"), args.opt_bool("execute", false)));
    }

    void repair_crg_cache(const Args& args) {
        print_json(crg_repair_cache_json(db, "source", args.opt("scope", "all"), args.opt_bool("execute", false)));
    }

    json score_symbol(const Row& sym) {
        std::string id = sym.get("id");
        auto callers = query(db, "SELECT ref_kind, file_id FROM \"references\" WHERE to_symbol_id = ? LIMIT 500", {id});
        auto callees = query(db, "SELECT ref_kind, file_id FROM \"references\" WHERE from_symbol_id = ? LIMIT 500", {id});
        auto children = query(db, "SELECT child_id FROM inheritance WHERE parent_id = ?", {id});
        auto parents = query(db, "SELECT parent_id FROM inheritance WHERE child_id = ?", {id});
        std::set<std::string> caller_files;
        for (const auto& r : callers) caller_files.insert(r.get("file_id"));
        json reasons = json::array();
        double raw = 0.0;
        auto factor = [&](double c, const std::string& why) {
            if (c > 0.0) { raw += c; reasons.push_back(why); }
        };
        factor(std::min<double>(callers.size(), 50) / 50.0 * 0.35, "caller fan-in: " + std::to_string(callers.size()));
        factor(std::min<double>(children.size(), 30) / 30.0 * 0.25, "inheritance descendants (1-hop): " + std::to_string(children.size()));
        factor(std::min<double>(callees.size(), 50) / 50.0 * 0.10, "callee fan-out: " + std::to_string(callees.size()));
        factor(sym.get_int("is_ue_macro") != 0 ? 0.15 : 0.0, "UE reflection macro symbol (UCLASS/UFUNCTION/UPROPERTY family)");
        factor(caller_files.size() > 1 ? std::min<double>(caller_files.size(), 20) / 20.0 * 0.15 : 0.0,
            "module/file boundary crossing: " + std::to_string(caller_files.size()) + " distinct caller file(s)");
        auto sensitivity = sensitivity_factor(
            sym.get("name") + " " + sym.get("qualified_name") + " " + sym.get("kind") + " " + sym.get("signature"),
            true);
        factor(sensitivity.first, sensitivity.second);
        if (callers.empty() && sym.get("kind").find("function") != std::string::npos)
            reasons.push_back("missing direct callers: function has 0 indexed callers; may be reflection/delegate/Blueprint-invoked");
        double score = std::max(0.0, std::min(raw, 1.0));
        return {
            {"id", sym.get_int64("id")},
            {"name", sym.get("name")},
            {"qualified_name", sym.get("qualified_name")},
            {"kind", sym.get("kind")},
            {"score", std::round(score * 1000.0) / 1000.0},
            {"tier", tier_for(score)},
            {"reasons", reasons},
            {"raw_counts", {
                {"callers", callers.size()},
                {"callees", callees.size()},
                {"descendants", children.size()},
                {"ancestors", parents.size()},
                {"caller_files", caller_files.size()},
                {"is_ue_macro", sym.get_int("is_ue_macro") != 0},
                {"sensitivity", sensitivity.first},
            }},
        };
    }

    json source_risk_score_json(const std::string& symbol, int limit, const std::string& min_tier) {
        int cap = clamp_int(limit <= 0 ? 10 : limit, 1, 100);
        json root = {{"input", {{"symbol", symbol}}}, {"limits", {{"limit", cap}, {"min_tier", min_tier.empty() ? "low" : min_tier}}}};
        auto rows = symbols_by_name(symbol, cap);
        json items = json::array();
        if (rows.empty()) {
            root["status"] = "error";
            root["summary"] = "Symbol not found: " + symbol;
            root["items"] = items;
            root["truncated"] = false;
            add_next(root, {"source.search_source"});
            return root;
        }
        int min_rank = tier_rank(min_tier.empty() ? "low" : min_tier);
        bool any_cache_hit = false;
        for (const auto& row : rows) {
            json cached = try_cached_risk(db, "source", "symbols", row.get_int64("id"));
            json scored;
            if (!cached.is_null()) {
                any_cache_hit = true;
                scored = cached;
                scored["id"] = row.get_int64("id");
                scored["name"] = row.get("name");
                scored["qualified_name"] = row.get("qualified_name");
                scored["kind"] = row.get("kind");
            } else {
                scored = score_symbol(row);
                scored["cache"] = {{"status", crg_cache_present(db) ? "miss" : "unavailable"}};
                scored["scoring_version"] = "3";
            }
            if (tier_rank(scored["tier"].get<std::string>()) >= min_rank) items.push_back(scored);
        }
        std::sort(items.begin(), items.end(), [](const json& a, const json& b) { return a["score"].get<double>() > b["score"].get<double>(); });
        const char* sv = "3";
        root["status"] = "ok";
        root["summary"] = std::to_string(items.size()) + " symbol overload(s) scored (scoring_version="
            + std::string(sv) + (any_cache_hit ? " cached when available, v3 query fallback; CRG cache hit)" : " cached when available, v3 query fallback)");
        root["scoring_version"] = sv;
        root["items"] = items;
        root["truncated"] = false;
        add_next(root, {"source.review_context", "source.impact_radius"});
        return root;
    }

    void risk_score(const Args& args) {
        std::string symbol = args.opt("symbol");
        if (symbol.empty() && !args.positional.empty()) symbol = args.positional[0];
        if (symbol.empty()) die("risk_score requires a symbol argument");
        print_json(source_risk_score_json(symbol, args.opt_int("limit", 10), args.opt("min_tier", "low")));
    }

    json source_review_hotspots_json(const std::string& kind, int limit, int min_lines, bool include_questions) {
        std::string k = kind.empty() ? "all" : lower_copy(kind);
        int cap = clamp_int(limit <= 0 ? 50 : limit, 1, 200);
        int loc_floor = std::max(min_lines <= 0 ? 100 : min_lines, 0);
        json root = {
            {"input", {{"kind", k}, {"include_questions", include_questions}}},
            {"limits", {{"limit", cap}, {"min_lines", loc_floor}}},
        };
        if (k != "fan_in" && k != "fan_out" && k != "risk" && k != "large" && k != "all") {
            root["status"] = "error";
            root["summary"] = "Unsupported kind for source.review_hotspots (expected fan_in|fan_out|risk|large|all)";
            root["hotspots"] = json::array();
            root["truncated"] = false;
            add_next(root, {"source.review_hotspots", "source.risk_score"});
            return root;
        }

        bool has_cache = crg_cache_present(db);
        std::string cache_join = has_cache
            ? "LEFT JOIN crg_nodes n ON n.domain='source' AND n.native_table='symbols' AND n.native_id=c.id "
              "LEFT JOIN crg_node_metrics m ON m.node_id=n.id "
            : "";
        std::string risk_expr = has_cache ? "COALESCE(m.risk_score, c.estimated_risk)" : "c.estimated_risk";
        std::string tier_expr = has_cache
            ? "COALESCE(m.risk_tier, CASE WHEN c.estimated_risk >= 0.66 THEN 'high' WHEN c.estimated_risk >= 0.33 THEN 'medium' ELSE 'low' END)"
            : "CASE WHEN c.estimated_risk >= 0.66 THEN 'high' WHEN c.estimated_risk >= 0.33 THEN 'medium' ELSE 'low' END";
        std::string where = (k == "large")
            ? "WHERE lines >= " + std::to_string(loc_floor) + " "
            : "WHERE fan_in > 0 OR fan_out > 0 OR descendants > 0 OR risk_score > 0 OR lines >= " + std::to_string(loc_floor) + " ";
        std::string order = "ORDER BY hotspot_score DESC, risk_score DESC, fan_in DESC, lines DESC ";
        if (k == "fan_in") order = "ORDER BY fan_in DESC, risk_score DESC, lines DESC ";
        else if (k == "fan_out") order = "ORDER BY fan_out DESC, risk_score DESC, lines DESC ";
        else if (k == "risk") order = "ORDER BY risk_score DESC, fan_in DESC, lines DESC ";
        else if (k == "large") order = "ORDER BY lines DESC, risk_score DESC, fan_in DESC ";

        std::string sql = R"SQL(
WITH ref_in AS (
  SELECT to_symbol_id AS symbol_id, COUNT(*) AS fan_in, COUNT(DISTINCT r.file_id) AS caller_files
  FROM "references" r JOIN symbols fs ON fs.id=r.from_symbol_id JOIN symbols ts ON ts.id=r.to_symbol_id GROUP BY to_symbol_id
), ref_out AS (
  SELECT from_symbol_id AS symbol_id, COUNT(*) AS fan_out
  FROM "references" r JOIN symbols fs ON fs.id=r.from_symbol_id JOIN symbols ts ON ts.id=r.to_symbol_id GROUP BY from_symbol_id
), inh_desc AS (
  SELECT parent_id AS symbol_id, COUNT(*) AS descendants FROM inheritance i
  JOIN symbols cs ON cs.id=i.child_id JOIN symbols ps ON ps.id=i.parent_id GROUP BY parent_id
), base AS (
  SELECT s.id,s.name,s.qualified_name,s.kind,COALESCE(f.path,'') AS file,s.line_start,s.line_end,
         CASE WHEN s.line_end >= s.line_start THEN s.line_end - s.line_start + 1 ELSE 0 END AS lines,
         COALESCE(ri.fan_in,0) AS fan_in,COALESCE(ro.fan_out,0) AS fan_out,
         COALESCE(id.descendants,0) AS descendants,COALESCE(ri.caller_files,0) AS caller_files,
         s.is_ue_macro AS is_ue_macro,
         CASE WHEN lower(COALESCE(s.qualified_name,'') || ' ' || COALESCE(s.name,'') || ' ' || COALESCE(s.signature,'')) LIKE '%ufunction%'
            OR lower(COALESCE(s.qualified_name,'') || ' ' || COALESCE(s.name,'') || ' ' || COALESCE(s.signature,'')) LIKE '%server%'
            OR lower(COALESCE(s.qualified_name,'') || ' ' || COALESCE(s.name,'') || ' ' || COALESCE(s.signature,'')) LIKE '%client%'
            OR lower(COALESCE(s.qualified_name,'') || ' ' || COALESCE(s.name,'') || ' ' || COALESCE(s.signature,'')) LIKE '%netmulticast%'
            OR lower(COALESCE(s.qualified_name,'') || ' ' || COALESCE(s.name,'') || ' ' || COALESCE(s.signature,'')) LIKE '%save%'
            OR lower(COALESCE(s.qualified_name,'') || ' ' || COALESCE(s.name,'') || ' ' || COALESCE(s.signature,'')) LIKE '%serialize%'
            OR lower(COALESCE(s.qualified_name,'') || ' ' || COALESCE(s.name,'') || ' ' || COALESCE(s.signature,'')) LIKE '%auth%'
            OR lower(COALESCE(s.qualified_name,'') || ' ' || COALESCE(s.name,'') || ' ' || COALESCE(s.signature,'')) LIKE '%purchase%'
            OR lower(COALESCE(s.qualified_name,'') || ' ' || COALESCE(s.name,'') || ' ' || COALESCE(s.signature,'')) LIKE '%anticheat%'
            OR lower(COALESCE(s.qualified_name,'') || ' ' || COALESCE(s.name,'') || ' ' || COALESCE(s.signature,'')) LIKE '%crypt%'
            OR lower(COALESCE(s.qualified_name,'') || ' ' || COALESCE(s.name,'') || ' ' || COALESCE(s.signature,'')) LIKE '%exec%'
            OR lower(COALESCE(s.qualified_name,'') || ' ' || COALESCE(s.name,'') || ' ' || COALESCE(s.signature,'')) LIKE '%file%'
          THEN 1 ELSE 0 END AS sensitivity
  FROM symbols s LEFT JOIN files f ON f.id=s.file_id
  LEFT JOIN ref_in ri ON ri.symbol_id=s.id LEFT JOIN ref_out ro ON ro.symbol_id=s.id LEFT JOIN inh_desc id ON id.symbol_id=s.id
), counts AS (
  SELECT *, MIN(1.0, MIN(fan_in,50)/50.0*0.35 + MIN(descendants,30)/30.0*0.25 +
         MIN(fan_out,50)/50.0*0.10 + CASE WHEN is_ue_macro != 0 THEN 0.15 ELSE 0 END +
         MIN(caller_files,20)/20.0*0.15 + CASE WHEN sensitivity != 0 THEN 0.15 ELSE 0 END) AS estimated_risk
  FROM base
), scored AS (
  SELECT c.id,c.name,c.qualified_name,c.kind,c.file,c.line_start,c.line_end,c.lines,
         c.fan_in,c.fan_out,c.descendants,)SQL" + risk_expr + R"SQL( AS risk_score,)SQL" + tier_expr + R"SQL( AS risk_tier
  FROM counts c )SQL" + cache_join + R"SQL(
)
SELECT *, MAX(risk_score, MIN(fan_in,50)/50.0, MIN(fan_out,50)/50.0, MIN(lines,500)/500.0) AS hotspot_score
FROM scored )SQL" + where + order + "LIMIT " + std::to_string(cap + 1) + ";";

        Rows rows = query(db, sql);
        bool truncated = (int)rows.size() > cap;
        if (truncated) rows.pop_back();
        json hotspots = json::array();
        json questions = json::array();
        for (const auto& r : rows) {
            int fan_in = r.get_int("fan_in");
            int fan_out = r.get_int("fan_out");
            int lines = r.get_int("lines");
            double risk = r.get_double("risk_score");
            std::string primary = k;
            if (primary == "all") {
                double best = risk;
                primary = "risk";
                double in_signal = std::min<double>(fan_in, 50) / 50.0;
                double out_signal = std::min<double>(fan_out, 50) / 50.0;
                double large_signal = std::min<double>(lines, 500) / 500.0;
                if (in_signal > best) { best = in_signal; primary = "fan_in"; }
                if (out_signal > best) { best = out_signal; primary = "fan_out"; }
                if (large_signal > best) primary = "large";
            }
            json signals = {
                {"fan_in", fan_in},
                {"fan_out", fan_out},
                {"descendants", r.get_int("descendants")},
                {"risk_score", std::round(risk * 1000.0) / 1000.0},
                {"risk_tier", r.get("risk_tier", tier_for(risk))},
                {"lines", lines},
            };
            hotspots.push_back({
                {"primary_kind", primary},
                {"id", r.get_int64("id")},
                {"name", r.get("name")},
                {"qualified_name", r.get("qualified_name")},
                {"kind", r.get("kind")},
                {"file", short_path(r.get("file"))},
                {"line_start", r.get_int("line_start")},
                {"line_end", r.get_int("line_end")},
                {"signals", signals},
                {"metrics", signals},
            });
            if (include_questions && questions.size() < 5) {
                questions.push_back({
                    {"target", r.get("qualified_name").empty() ? r.get("name") : r.get("qualified_name")},
                    {"reason", primary},
                    {"question", primary == "large"
                        ? "Can this large symbol be split or covered by focused tests before risky edits?"
                        : "Which callers and tests cover this hotspot before changing it?"},
                });
            }
        }
        root["status"] = "ok";
        root["summary"] = std::to_string(hotspots.size()) + " source review hotspot(s) ranked by " + k
            + (has_cache ? " using CRG cache when available" : " using native fallback");
        root["hotspots"] = hotspots;
        if (include_questions) root["questions"] = questions;
        root["truncated"] = truncated;
        add_next(root, {"source.review_context", "source.risk_score", "source.impact_radius"});
        return root;
    }

    void review_hotspots(const Args& args) {
        print_json(source_review_hotspots_json(args.opt("kind", "all"),
                                               args.opt_int("limit", 50),
                                               args.opt_int("min_lines", 100),
                                               args.opt_bool("include_questions", true)));
    }

    json source_impact_radius_json(const std::string& symbol, const std::string& edge_kinds, const std::string& direction, int max_depth, int max_results) {
        int depth = clamp_int(max_depth <= 0 ? 2 : max_depth, 0, 8);
        int limit = clamp_int(max_results <= 0 ? 200 : max_results, 1, 1000);
        std::string kinds = edge_kinds.empty() ? "call|type|inheritance" : edge_kinds;
        json root = {
            {"input", {{"symbol", symbol}, {"edge_kinds", kinds}, {"direction", direction}}},
            {"limits", {{"max_depth", depth}, {"max_results", limit}}},
            {"warnings", json::array()},
        };
        bool include_requested = !edge_kinds.empty() && wants_kind(edge_kinds, "include");
        if (include_requested) root["warnings"].push_back("include edges are excluded in P0 until includes.included_path can be resolved to files.path with file-level fixtures");
        auto seeds = symbols_by_name(symbol, 5);
        if (seeds.empty()) {
            root["status"] = "error";
            root["summary"] = "Symbol not found in EngineSource: " + symbol;
            root["impacted_symbols"] = json::array();
            root["edges"] = json::array();
            root["truncated"] = false;
            add_next(root, {"source.search_source", "source.read_source"});
            return root;
        }
        bool in = direction != "out";
        bool out = direction != "in";
        bool call = wants_kind(kinds, "call");
        bool type = wants_kind(kinds, "type");
        bool inh = wants_kind(kinds, "inheritance");
        std::set<std::string> visited;
        std::vector<std::string> frontier;
        root["seed_symbols"] = json::array();
        for (const auto& s : seeds) {
            visited.insert(s.get("id"));
            frontier.push_back(s.get("id"));
            root["seed_symbols"].push_back(symbol_json(s, 0));
        }
        json impacted = json::array();
        json edges = json::array();
        bool truncated = false;
        int emitted = 0;
        int per_node = clamp_int(limit, 50, 500);
        for (int d = 1; d <= depth && !frontier.empty() && !truncated; ++d) {
            std::vector<std::string> next;
            for (const auto& cur : frontier) {
                std::vector<std::pair<std::string, std::string>> neighbors;
                auto append_refs = [&](const char* sql, const std::string& kind, bool reverse) {
                    auto refs = query(db, sql, {cur, kind, std::to_string(per_node)});
                    for (const auto& r : refs) neighbors.push_back({r.get(reverse ? "from_symbol_id" : "to_symbol_id"), r.get("ref_kind", kind)});
                };
                if (out && call) append_refs("SELECT from_symbol_id, to_symbol_id, ref_kind FROM \"references\" WHERE from_symbol_id = ? AND ref_kind = ? LIMIT ?", "call", false);
                if (out && type) append_refs("SELECT from_symbol_id, to_symbol_id, ref_kind FROM \"references\" WHERE from_symbol_id = ? AND ref_kind = ? LIMIT ?", "type", false);
                if (in && call) append_refs("SELECT from_symbol_id, to_symbol_id, ref_kind FROM \"references\" WHERE to_symbol_id = ? AND ref_kind = ? LIMIT ?", "call", true);
                if (in && type) append_refs("SELECT from_symbol_id, to_symbol_id, ref_kind FROM \"references\" WHERE to_symbol_id = ? AND ref_kind = ? LIMIT ?", "type", true);
                if (out && inh) for (const auto& r : query(db, "SELECT parent_id AS id FROM inheritance WHERE child_id = ?", {cur})) neighbors.push_back({r.get("id"), "inheritance"});
                if (in && inh) for (const auto& r : query(db, "SELECT child_id AS id FROM inheritance WHERE parent_id = ?", {cur})) neighbors.push_back({r.get("id"), "inheritance"});
                for (const auto& n : neighbors) {
                    if (n.first.empty() || n.first == cur) continue;
                    edges.push_back({{"from", std::stoll(cur)}, {"to", std::stoll(n.first)}, {"kind", n.second}, {"depth", d}});
                    if (visited.count(n.first)) continue;
                    visited.insert(n.first);
                    if (emitted >= limit) { truncated = true; break; }
                    Row other = symbol_by_id(n.first);
                    if (!other.cols.empty()) {
                        impacted.push_back(symbol_json(other, d));
                        ++emitted;
                    }
                    next.push_back(n.first);
                }
                if (truncated) break;
            }
            frontier = std::move(next);
        }
        root["status"] = "ok";
        root["summary"] = std::to_string(emitted) + " impacted symbol(s) within depth " + std::to_string(depth) + " (" + direction + ") of " + symbol + (truncated ? " [truncated]" : "");
        root["impacted_symbols"] = impacted;
        root["edges"] = edges;
        root["truncated"] = truncated;
        add_next(root, {"source.review_context", "source.risk_score", "source.find_callers"});
        return root;
    }

    void impact_radius(const Args& args) {
        std::string symbol = args.opt("symbol");
        if (symbol.empty() && !args.positional.empty()) symbol = args.positional[0];
        if (symbol.empty()) die("impact_radius requires a symbol argument");
        print_json(source_impact_radius_json(symbol, args.opt("edge_kinds", "call|type|inheritance"), args.opt("direction", "both"), args.opt_int("max_depth", 2), args.opt_int("max_results", 200)));
    }

    void review_context(const Args& args) {
        std::string symbol = args.opt("symbol");
        if (symbol.empty() && !args.positional.empty()) symbol = args.positional[0];
        if (symbol.empty()) die("review_context requires a symbol argument");
        std::string direction = args.opt("direction", "both");
        std::string detail = args.opt("detail_level", "minimal");
        bool minimal = detail != "standard";
        int max_depth = args.opt_int("max_depth", 2);
        int max_results = args.opt_int("max_results", 200);
        auto seeds = symbols_by_name(symbol, 1);
        json root = {{"input", {{"symbol", symbol}, {"direction", direction}, {"detail_level", minimal ? "minimal" : "standard"}}},
                     {"limits", {{"max_depth", clamp_int(max_depth <= 0 ? 2 : max_depth, 0, 8)}, {"max_results", minimal ? std::min(clamp_int(max_results <= 0 ? 200 : max_results, 1, 1000), 25) : clamp_int(max_results <= 0 ? 200 : max_results, 1, 1000)}}}};
        if (seeds.empty()) {
            root["status"] = "error";
            root["summary"] = "Symbol not found: " + symbol;
            root["truncated"] = false;
            add_next(root, {"source.search_source"});
            print_json(root);
            return;
        }
        Row seed = seeds[0];
        json risk = score_symbol(seed);
        root["risk"] = risk;
        root["top_risks"] = json::array();
        for (size_t i = 0; i < risk["reasons"].size() && i < 5; ++i) root["top_risks"].push_back(risk["reasons"][i]);
        json impact = source_impact_radius_json(symbol, "call|type|inheritance", direction, max_depth, minimal ? std::min(max_results, 25) : max_results);
        json summary = {{"truncated", impact.value("truncated", false)}};
        summary["impacted_count"] = impact["impacted_symbols"].size();
        summary["top_impacted"] = json::array();
        for (size_t i = 0; i < impact["impacted_symbols"].size() && i < (minimal ? 5u : 25u); ++i) summary["top_impacted"].push_back(impact["impacted_symbols"][i]);
        root["impact"] = summary;
        root["seed"] = symbol_json(seed, 0);
        root["context"] = json::array({{{"type", "seed_symbol"}, {"name", seed.get("name")}, {"qualified_name", seed.get("qualified_name")}, {"file", short_path(get_file_path(seed.get_int("file_id")))}, {"line", seed.get_int("line_start")}}});
        if (!minimal) root["details"] = symbol_json(seed, 0);
        root["status"] = "ok";
        root["summary"] = seed.get("name") + " risk=" + risk["tier"].get<std::string>() + " (" + (minimal ? "minimal" : "standard") + "); review the top impacted symbols and high-risk reasons";
        root["truncated"] = impact.value("truncated", false);
        add_next(root, {"source.read_source", "source.impact_radius", "source.get_class_hierarchy"});
        print_json(root);
    }

    // ============================================================
    // RX-1: detect_changes — changelist/diff -> mapped symbols ->
    // reused risk (RX-2 cache or query-time) + bounded impact +
    // advisory (heuristic) test-gaps + review priorities.
    // changed_paths is the VCS-agnostic primary input (positional or
    // --changed-paths=a,b); no P4/git shell-out in this path.
    // ============================================================
    json source_detect_changes_json(const std::vector<std::string>& changed_paths,
                                    int max_results, const std::string& detail_level,
                                    const std::map<std::string, std::vector<std::pair<int,int>>>& changed_ranges = {}) {
        int cap = clamp_int(max_results <= 0 ? 200 : max_results, 1, 2000);
        bool minimal = (detail_level != "standard");

        // Normalize/sanitize range keys; a path that only appears in
        // changed_ranges still counts as a changed path.
        std::map<std::string, std::vector<std::pair<int,int>>> path_ranges;
        for (const auto& kv : changed_ranges) {
            std::string key = kv.first;
            std::replace(key.begin(), key.end(), '\\', '/');
            if (key.empty()) continue;
            auto& out = path_ranges[key];
            for (const auto& pr : kv.second) {
                if ((int)out.size() >= 256) break;
                if (pr.first > 0 && pr.second >= pr.first) out.push_back(pr);
            }
            if (out.empty()) path_ranges.erase(key);
        }

        std::vector<std::string> all_paths;
        auto add_path = [&](const std::string& p) {
            std::string n = p;
            std::replace(n.begin(), n.end(), '\\', '/');
            if (!n.empty() && std::find(all_paths.begin(), all_paths.end(), n) == all_paths.end())
                all_paths.push_back(n);
        };
        for (const auto& p : changed_paths) add_path(p);
        for (const auto& kv : path_ranges) add_path(kv.first);

        const bool line_precision = !path_ranges.empty();
        json root = {
            {"input", {{"changed_paths", all_paths}, {"detail_level", minimal ? "minimal" : "standard"},
                       {"precision", line_precision ? "line" : "file"},
                       {"range_paths", path_ranges.size()}}},
            {"limits", {{"max_results", cap}, {"detail_level", minimal ? "minimal" : "standard"}}},
            {"truncated", false},
        };
        if (all_paths.empty()) {
            root["status"] = "error";
            root["summary"] = "detect_changes requires >=1 changed path (positional, --changed-paths=a,b, --ranges, or --diff-file/--diff-stdin)";
            root["changed_entities"] = json::array();
            add_next(root, {"source.search_source"});
            return root;
        }
        std::set<int64_t> seen;
        json changed = json::array();
        bool truncated = false, any_cache = false;
        for (const auto& norm : all_paths) {
            auto rit = path_ranges.find(norm);
            const bool line_mode = (rit != path_ranges.end() && !rit->second.empty());
            std::string sql =
                "SELECT s.id,s.name,s.qualified_name,s.kind,s.file_id,s.line_start,s.line_end,s.is_ue_macro "
                "FROM symbols s JOIN files f ON f.id = s.file_id "
                "WHERE replace(f.path,'\\','/') LIKE '%' || ? ";
            if (line_mode) {
                // CRG changes.py:204 overlap rule: line_start <= end AND line_end >= start.
                // Non-positive spans are not line-provable (CRG None-skip analog).
                sql += "AND s.line_start > 0 AND s.line_end > 0 AND (";
                for (size_t ri = 0; ri < rit->second.size(); ++ri) {
                    const auto& pr = rit->second[ri];
                    sql += (ri ? " OR " : "");
                    sql += "(s.line_start <= " + std::to_string(pr.second)
                         + " AND s.line_end >= " + std::to_string(pr.first) + ")";
                }
                sql += ") ";
            }
            sql += "ORDER BY s.id LIMIT " + std::to_string(cap + 1);
            auto rows = query(db, sql, {norm});
            for (const auto& r : rows) {
                int64_t sid = r.get_int64("id");
                if (seen.count(sid)) continue;
                if ((int)seen.size() >= cap) { truncated = true; break; }
                seen.insert(sid);
                json cached = try_cached_risk(db, "source", "symbols", sid);
                json item;
                if (!cached.is_null()) { any_cache = true; item = cached; }
                else { item = score_symbol(r); item["cache"] = {{"status", crg_cache_present(db) ? "miss" : "unavailable"}}; item["scoring_version"] = "3"; }
                item["id"] = sid;
                item["name"] = r.get("name");
                item["qualified_name"] = r.get("qualified_name");
                item["kind"] = r.get("kind");
                item["file"] = short_path(get_file_path(r.get_int("file_id")));
                if (line_mode && !minimal) {
                    int ls = r.get_int("line_start"), le = r.get_int("line_end");
                    json mr = json::array();
                    for (const auto& pr : rit->second)
                        if (ls <= pr.second && le >= pr.first)
                            mr.push_back({{"start", pr.first}, {"end", pr.second}});
                    item["matched_ranges"] = mr;
                }
                changed.push_back(item);
            }
        }
        double overall = 0.0;
        for (const auto& c : changed) overall = std::max(overall, c.value("score", 0.0));
        std::set<int64_t> impacted;
        for (const auto& c : changed) {
            if ((int)impacted.size() >= cap) { truncated = true; break; }
            auto callers = query(db,
                "SELECT DISTINCT from_symbol_id FROM \"references\" WHERE to_symbol_id = ? LIMIT 200",
                {std::to_string(c.value("id", (int64_t)0))});
            for (const auto& cr : callers) {
                int64_t fid = cr.get_int64("from_symbol_id");
                if (fid > 0 && !seen.count(fid)) impacted.insert(fid);
            }
        }
        json test_gaps = json::array();
        for (const auto& c : changed) {
            if (c.value("kind", std::string()).find("function") == std::string::npos) continue;
            auto cov = query(db,
                "SELECT 1 FROM \"references\" r JOIN symbols fs ON fs.id = r.from_symbol_id "
                "JOIN files ff ON ff.id = fs.file_id WHERE r.to_symbol_id = ? AND ("
                "replace(ff.path,'\\','/') LIKE '%/Tests/%' OR fs.name LIKE '%Spec%' "
                "OR fs.qualified_name LIKE '%AutomationTest%' OR fs.name LIKE '%_Test%') LIMIT 1",
                {std::to_string(c.value("id", (int64_t)0))});
            if (cov.empty())
                test_gaps.push_back({
                    {"name", c.value("name", std::string())},
                    {"qualified_name", c.value("qualified_name", std::string())},
                    {"confidence", "medium"},
                    {"reason", "no inbound reference from a Tests/ file or automation-named symbol "
                               "(heuristic: EngineSource has no TESTED_BY edge; may miss "
                               "reflection/Blueprint-driven tests)"},
                });
        }
        std::sort(changed.begin(), changed.end(),
                  [](const json& a, const json& b) { return a.value("score", 0.0) > b.value("score", 0.0); });
        json priorities = json::array();
        for (size_t i = 0; i < changed.size() && i < 10; ++i) priorities.push_back(changed[i]);
        const char* sv = "3";
        root["risk_score"] = std::round(overall * 1000.0) / 1000.0;
        root["scoring_version"] = sv;
        root["truncated"] = truncated;
        root["status"] = "ok";
        root["changed_entity_count"] = changed.size();
        root["impacted_count"] = impacted.size();
        root["test_gap_count"] = test_gaps.size();
        if (minimal) {
            json topn = json::array();
            for (size_t i = 0; i < changed.size() && i < 3; ++i) topn.push_back(changed[i].value("name", std::string()));
            root["summary"] = "changed " + std::to_string(changed.size()) + " symbol(s), risk=" + tier_for(overall)
                + ", " + std::to_string(test_gaps.size()) + " advisory test-gap(s), scoring_version=" + sv;
            root["review_priorities"] = topn;
        } else {
            root["summary"] = "changed " + std::to_string(changed.size()) + " symbol(s) across "
                + std::to_string(all_paths.size()) + " path(s); precision="
                + (line_precision ? "line" : "file") + "; risk=" + tier_for(overall)
                + " scoring_version=" + sv;
            root["changed_entities"] = changed;
            root["impact"] = {{"depth", 1}, {"impacted_count", impacted.size()}};
            root["test_gaps"] = test_gaps;
            root["review_priorities"] = priorities;
        }
        add_next(root, {"source.review_context", "source.impact_radius", "source.risk_score"});
        return root;
    }

    void detect_changes(const Args& args) {
        std::vector<std::string> paths = args.positional;
        std::string csv = args.opt("changed_paths");
        if (!csv.empty()) {
            std::stringstream ss(csv);
            std::string p;
            while (std::getline(ss, p, ',')) if (!p.empty()) paths.push_back(p);
        }
        print_json(source_detect_changes_json(paths, args.opt_int("max_results", 200),
                                              args.opt("detail_level", "minimal"),
                                              collect_changed_ranges(args)));
    }

    // ============================================================
    // RX-3: find_unused — advisory dead-symbol detection.
    // 0 inbound "references", not an inheritance parent, is_ue_macro=0,
    // name/signature not matching UE reflection/automation/entry patterns.
    // Recall-first & advisory: UE reflection/delegate/Blueprint call edges
    // are not in the symbol graph, so confidence is lowered on ambiguity
    // (overloaded name) and never reported as "high".
    // ============================================================
    json source_find_unused_json(const std::string& kind, int limit, const std::string& min_conf) {
        int cap = clamp_int(limit <= 0 ? 100 : limit, 1, 1000);
        json root = {
            {"input", {{"kind", kind.empty() ? "all" : kind}, {"min_confidence", min_conf.empty() ? "low" : min_conf}}},
            {"limits", {{"limit", cap}, {"min_confidence", min_conf.empty() ? "low" : min_conf}}},
            {"truncated", false},
        };
        std::string kfilter;
        if (kind == "function" || kind == "class" || kind == "struct")
            kfilter = " AND s.kind = '" + kind + "'";
        else
            kfilter = " AND s.kind IN ('function','class','struct')";
        // Candidate set: zero inbound refs, not an inheritance parent, not a
        // reflection/automation/entry symbol. LIMIT cap+1 for truncation flag.
        auto rows = query(db,
            "SELECT s.id,s.name,s.qualified_name,s.kind,s.file_id,s.signature "
            "FROM symbols s WHERE s.is_ue_macro = 0" + kfilter + " "
            "AND NOT EXISTS (SELECT 1 FROM \"references\" r WHERE r.to_symbol_id = s.id) "
            "AND NOT EXISTS (SELECT 1 FROM inheritance i WHERE i.parent_id = s.id) "
            "AND s.name NOT LIKE '~%' "
            "AND s.name NOT IN ('main','WinMain','DllMain','StaticClass','StaticRegisterNatives','GetPrivateStaticClass') "
            "AND s.name NOT LIKE 'Execute_%' AND s.name NOT LIKE 'exec%' "
            "AND s.name NOT LIKE '%AutomationTest%' AND s.name NOT LIKE '%Spec' "
            "AND s.qualified_name NOT LIKE '%AutomationTest%' "
            "AND COALESCE(s.signature,'') NOT LIKE '%UFUNCTION%' "
            "AND COALESCE(s.signature,'') NOT LIKE '%UPROPERTY%' "
            "ORDER BY s.id LIMIT " + std::to_string(cap + 1));
        bool truncated = (int)rows.size() > cap;
        if (truncated) rows.pop_back();
        auto conf_rank = [](const std::string& c) { return c == "high" ? 2 : c == "medium" ? 1 : 0; };
        int min_rank = conf_rank(min_conf.empty() ? "low" : min_conf);
        json items = json::array();
        for (const auto& r : rows) {
            // Ambiguity: a non-unique symbol name means a 0-inbound result is
            // less reliable (call may resolve to another overload).
            auto ncrow = query(db, "SELECT COUNT(*) AS c FROM symbols WHERE name = ?;", {r.get("name")});
            int64_t namecount = ncrow.empty() ? 1 : ncrow[0].get_int64("c");
            std::string conf = (namecount <= 1) ? "medium" : "low";
            if (conf_rank(conf) < min_rank) continue;
            json reasons = json::array();
            reasons.push_back("0 inbound references (no indexed caller/user)");
            reasons.push_back("not an inheritance parent; is_ue_macro=0; no UFUNCTION/UPROPERTY in signature");
            if (namecount > 1)
                reasons.push_back("ambiguous: " + std::to_string(namecount) +
                                  " symbols share this name — may be reflection/Blueprint/overload-invoked (advisory)");
            else
                reasons.push_back("unique name — but UE reflection/delegate/Blueprint edges are not in the symbol graph (advisory, never 'high')");
            items.push_back({
                {"id", r.get_int64("id")},
                {"name", r.get("name")},
                {"qualified_name", r.get("qualified_name")},
                {"kind", r.get("kind")},
                {"file", short_path(get_file_path(r.get_int("file_id")))},
                {"confidence", conf},
                {"reasons", reasons},
            });
        }
        root["status"] = "ok";
        root["summary"] = std::to_string(items.size()) + " advisory unused symbol candidate(s) "
            "(recall-first; verify reflection/Blueprint usage before removal)";
        root["items"] = items;
        root["truncated"] = truncated;
        add_next(root, {"source.find_callers", "source.review_context", "source.impact_radius"});
        return root;
    }

    void find_unused(const Args& args) {
        print_json(source_find_unused_json(args.opt("kind", "all"), args.opt_int("limit", 100),
                                           args.opt("min_confidence", "low")));
    }

    json source_pre_merge_check_json(const std::vector<std::string>& changed_paths,
                                     int max_results, int unused_limit,
                                     const std::string& detail_level,
                                     bool include_unused) {
        bool standard = detail_level == "standard";
        int change_cap = clamp_int(max_results <= 0 ? 200 : max_results, 1, 2000);
        int unused_cap = clamp_int(unused_limit <= 0 ? 20 : unused_limit, 1, 1000);
        json root = {
            {"input", {
                {"changed_paths", changed_paths},
                {"detail_level", standard ? "standard" : "minimal"},
                {"include_unused", include_unused},
            }},
            {"limits", {{"max_results", change_cap}, {"unused_limit", unused_cap}}},
            {"scoring_version", "3"},
        };

        json health = source_health_json(false);
        json changes = source_detect_changes_json(changed_paths, change_cap, standard ? "standard" : "minimal");
        json unused = include_unused ? source_find_unused_json("all", unused_cap, "low") : json();

        json checks = json::array();
        json findings = json::array();
        int severity = 0;

        std::string health_status = json_string_field(health, "status", "error");
        int health_severity = health_status == "error" ? 2 : health_status == "warning" ? 1 : 0;
        premerge_add_check(checks, severity, "health", health_status,
                           json_string_field(health, "summary", "Source health could not run"),
                           health_severity);
        if (health_severity > 0) {
            premerge_add_finding(findings, health_severity >= 2 ? "error" : "warning", "health",
                                 json_string_field(health, "summary", "Source health failed"));
        }

        std::string change_status = json_string_field(changes, "status", "error");
        int changed_count = json_int_field(changes, "changed_entity_count");
        int impacted_count = json_int_field(changes, "impacted_count");
        int test_gap_count = json_int_field(changes, "test_gap_count");
        double risk_score = json_num_field(changes, "risk_score");
        int change_severity = change_status == "error" ? 2 : 0;
        if (change_severity == 0 && changed_count == 0) {
            change_severity = 1;
            premerge_add_finding(findings, "warning", "detect_changes",
                                 "No indexed source symbol matched the changed path set");
        }
        if (risk_score >= 0.66) {
            change_severity = std::max(change_severity, 1);
            premerge_add_finding(findings, "warning", "detect_changes",
                                 "Changed source risk score is high: " + std::to_string(std::round(risk_score * 1000.0) / 1000.0));
        }
        if (impacted_count > 50) {
            change_severity = std::max(change_severity, 1);
            premerge_add_finding(findings, "warning", "detect_changes",
                                 "Changed source set has broad direct caller impact: " + std::to_string(impacted_count) + " caller(s)");
        }
        if (test_gap_count > 0) {
            change_severity = std::max(change_severity, 1);
            premerge_add_finding(findings, "warning", "detect_changes",
                                 std::to_string(test_gap_count) + " changed function(s) have no indexed test/automation reference");
        }
        if (change_status == "error") {
            premerge_add_finding(findings, "error", "detect_changes",
                                 json_string_field(changes, "summary", "detect_changes failed"));
        }
        premerge_add_check(checks, severity, "detect_changes", change_status,
                           std::to_string(changed_count) + " changed source symbol(s), " +
                               std::to_string(impacted_count) + " impacted caller(s), " +
                               std::to_string(test_gap_count) + " test gap(s), risk=" +
                               std::to_string(std::round(risk_score * 1000.0) / 1000.0),
                           change_severity);

        int unused_count = 0;
        if (include_unused && unused.is_object()) {
            if (unused.contains("items") && unused["items"].is_array())
                unused_count = static_cast<int>(unused["items"].size());
            std::string unused_status = json_string_field(unused, "status", "error");
            int unused_severity = unused_status == "error" ? 2 : unused_count > 0 ? 1 : 0;
            premerge_add_check(checks, severity, "find_unused", unused_status,
                               std::to_string(unused_count) + " advisory unused source candidate(s) sampled",
                               unused_severity);
            if (unused_count > 0) {
                premerge_add_finding(findings, "warning", "find_unused",
                                     std::to_string(unused_count) + " advisory unused source candidate(s) present in sampled index");
            }
        }

        std::string decision = severity >= 2 ? "fail" : severity == 1 ? "warn" : "pass";
        root["status"] = severity >= 2 ? "error" : severity == 1 ? "warning" : "ok";
        root["decision"] = decision;
        root["summary"] = "Source pre-merge check " + decision + ": " +
            std::to_string(changed_count) + " changed symbol(s), " +
            std::to_string(impacted_count) + " impacted caller(s), " +
            std::to_string(test_gap_count) + " test gap(s), " +
            std::to_string(findings.size()) + " finding(s)";
        root["risk_score"] = std::round(risk_score * 1000.0) / 1000.0;
        root["checks"] = checks;
        root["findings"] = findings;
        root["changed_entity_count"] = changed_count;
        root["impacted_count"] = impacted_count;
        root["test_gap_count"] = test_gap_count;
        root["unused_count"] = unused_count;
        root["truncated"] = json_bool_field(changes, "truncated") || json_bool_field(unused, "truncated");
        if (standard) {
            root["health"] = health;
            root["change_analysis"] = changes;
            if (include_unused && unused.is_object()) root["unused"] = unused;
        }
        add_next(root, severity >= 2
            ? std::initializer_list<const char*>{"source.health", "source.search_source"}
            : std::initializer_list<const char*>{"source.detect_changes", "source.review_context", "source.find_unused"});
        return root;
    }

    void pre_merge_check(const Args& args) {
        print_json(source_pre_merge_check_json(collect_changed_paths(args),
                                               args.opt_int("max_results", 200),
                                               args.opt_int("unused_limit", 20),
                                               args.opt("detail_level", "minimal"),
                                               args.opt_bool("include_unused", true)));
    }

    void snapshot(const Args& args) {
        std::string label = args.opt("label");
        if (label.empty() && !args.positional.empty()) label = args.positional[0];
        print_json(crg_snapshot_json(db, "source", label, args.opt_bool("execute", false)));
    }

    void diff_snapshots(const Args& args) {
        std::string before = args.opt("before");
        std::string after = args.opt("after");
        if (before.empty()) {
            if (!args.positional.empty()) before = args.positional[0];
            if (after.empty() && args.positional.size() > 1) after = args.positional[1];
        } else if (after.empty() && !args.positional.empty()) {
            after = args.positional[0];
        }
        print_json(crg_diff_snapshots_json(db, "source", before, after, args.opt_int("limit", 100)));
    }
};

// ============================================================
// Project actions
// ============================================================

class ProjectActions {
    Database db;

public:
    void open(const std::string& path, bool query_only = true) { db.open(path, query_only); }

    // --- search ---
    void search(const Args& args) {
        if (args.positional.empty()) die("search requires a query argument");
        std::string q = args.positional[0];
        int limit = args.opt_int("limit", 50);

        json results = json::array();

        // Search assets FTS
        {
            auto rows = query(db,
                "SELECT a.package_path, a.asset_name, a.asset_class, a.module_name, "
                "snippet(fts_assets, 2, '>>>', '<<<', '...', 32) as ctx, rank "
                "FROM fts_assets f JOIN assets a ON a.id = f.rowid "
                "WHERE fts_assets MATCH ? ORDER BY rank LIMIT " + std::to_string(limit),
                {q});
            for (auto& r : rows) {
                results.push_back({
                    {"asset_path", r.get("package_path")},
                    {"asset_name", r.get("asset_name")},
                    {"asset_class", r.get("asset_class")},
                    {"module_name", r.get("module_name")},
                    {"match_context", r.get("ctx")},
                    {"rank", r.get_double("rank")},
                });
            }
        }

        // Search nodes FTS
        {
            auto rows = query(db,
                "SELECT a.package_path, a.asset_name, a.asset_class, a.module_name, "
                "snippet(fts_nodes, 0, '>>>', '<<<', '...', 32) as ctx, f.rank "
                "FROM fts_nodes f JOIN nodes n ON n.id = f.rowid "
                "JOIN assets a ON a.id = n.asset_id "
                "WHERE fts_nodes MATCH ? ORDER BY f.rank LIMIT " + std::to_string(limit),
                {q});
            for (auto& r : rows) {
                results.push_back({
                    {"asset_path", r.get("package_path")},
                    {"asset_name", r.get("asset_name")},
                    {"asset_class", r.get("asset_class")},
                    {"module_name", r.get("module_name")},
                    {"match_context", r.get("ctx")},
                    {"rank", r.get_double("rank")},
                });
            }
        }

        // Sort by rank, truncate
        std::sort(results.begin(), results.end(),
                  [](const json& a, const json& b) { return a["rank"].get<double>() < b["rank"].get<double>(); });
        if ((int)results.size() > limit)
            results = json(std::vector<json>(results.begin(), results.begin() + limit));

        json out = {{"success", true}, {"count", results.size()}, {"results", results}};
        std::cout << out.dump(2) << std::endl;
    }

    // --- find_by_type ---
    void find_by_type(const Args& args) {
        if (args.positional.empty()) die("find_by_type requires an asset_class argument");
        std::string asset_class = args.positional[0];
        int limit = args.opt_int("limit", 50);
        int offset = args.opt_int("offset", 0);

        auto rows = query(db,
            "SELECT package_path, asset_name, asset_class, module_name, description FROM assets "
            "WHERE asset_class = ? LIMIT " + std::to_string(limit) + " OFFSET " + std::to_string(offset),
            {asset_class});

        json results = json::array();
        for (auto& r : rows) {
            results.push_back({
                {"package_path", r.get("package_path")},
                {"asset_name", r.get("asset_name")},
                {"asset_class", r.get("asset_class")},
                {"module_name", r.get("module_name")},
                {"description", r.get("description")},
            });
        }

        json out = {{"success", true}, {"count", results.size()}, {"results", results}};
        std::cout << out.dump(2) << std::endl;
    }

    // --- find_references ---
    void find_references(const Args& args) {
        if (args.positional.empty()) die("find_references requires an asset_path argument");
        std::string asset_path = args.positional[0];

        auto assets = query(db, "SELECT id FROM assets WHERE package_path = ?", {asset_path});
        if (assets.empty()) {
            json out = {{"success", false}, {"error", "Asset not found: " + asset_path}};
            std::cout << out.dump(2) << std::endl;
            return;
        }

        std::string aid = assets[0].get("id");

        // Depends on
        auto deps = query(db,
            "SELECT a.package_path, a.asset_class, d.dependency_type "
            "FROM dependencies d JOIN assets a ON a.id = d.target_asset_id WHERE d.source_asset_id = ?",
            {aid});

        // Referenced by
        auto refs = query(db,
            "SELECT a.package_path, a.asset_class, d.dependency_type "
            "FROM dependencies d JOIN assets a ON a.id = d.source_asset_id WHERE d.target_asset_id = ?",
            {aid});

        json depends_on = json::array();
        for (auto& r : deps) {
            depends_on.push_back({
                {"path", r.get("package_path")},
                {"class", r.get("asset_class")},
                {"type", r.get("dependency_type")},
            });
        }

        json referenced_by = json::array();
        for (auto& r : refs) {
            referenced_by.push_back({
                {"path", r.get("package_path")},
                {"class", r.get("asset_class")},
                {"type", r.get("dependency_type")},
            });
        }

        json out = {{"success", true}, {"depends_on", depends_on}, {"referenced_by", referenced_by}};
        std::cout << out.dump(2) << std::endl;
    }

    // --- get_stats ---
    void get_stats(const Args&) {
        json stats;
        static const char* tables[] = {
            "assets", "nodes", "connections", "variables", "parameters",
            "dependencies", "actors", "tags", "configs", "datatable_rows"
        };
        for (auto t : tables) {
            auto rows = query(db, std::string("SELECT COUNT(*) as c FROM ") + t);
            stats[t] = rows.empty() ? 0 : rows[0].get_int("c");
        }

        // Class breakdown
        json breakdown;
        auto class_rows = query(db,
            "SELECT asset_class, COUNT(*) as cnt FROM assets GROUP BY asset_class ORDER BY cnt DESC LIMIT 20");
        for (auto& r : class_rows)
            breakdown[r.get("asset_class")] = r.get_int("cnt");
        stats["asset_class_breakdown"] = breakdown;

        // Module breakdown
        json mod_breakdown;
        auto mod_rows = query(db,
            "SELECT CASE WHEN module_name = '' THEN 'Project' ELSE module_name END as mod, "
            "COUNT(*) as cnt FROM assets GROUP BY module_name ORDER BY cnt DESC");
        for (auto& r : mod_rows)
            mod_breakdown[r.get("mod")] = r.get_int("cnt");
        stats["module_breakdown"] = mod_breakdown;

        std::cout << stats.dump(2) << std::endl;
    }

    // --- get_asset_details ---
    void get_asset_details(const Args& args) {
        if (args.positional.empty()) die("get_asset_details requires an asset_path argument");
        std::string asset_path = args.positional[0];

        auto assets = query(db, "SELECT * FROM assets WHERE package_path = ?", {asset_path});
        if (assets.empty()) {
            json out = {{"error", "Asset not found: " + asset_path}};
            std::cout << out.dump(2) << std::endl;
            return;
        }

        auto& asset = assets[0];
        json details;
        for (auto& [k, v] : asset.cols) details[k] = v;

        std::string aid = asset.get("id");

        // Nodes
        auto nodes = query(db, "SELECT node_type, node_name, node_class FROM nodes WHERE asset_id = ?", {aid});
        json jnodes = json::array();
        for (auto& n : nodes)
            jnodes.push_back({{"node_type", n.get("node_type")}, {"node_name", n.get("node_name")}, {"node_class", n.get("node_class")}});
        details["nodes"] = jnodes;

        // Variables
        auto vars = query(db,
            "SELECT var_name, var_type, category, default_value, is_exposed, is_replicated "
            "FROM variables WHERE asset_id = ?", {aid});
        json jvars = json::array();
        for (auto& v : vars) {
            jvars.push_back({
                {"var_name", v.get("var_name")}, {"var_type", v.get("var_type")},
                {"category", v.get("category")}, {"default_value", v.get("default_value")},
                {"is_exposed", v.get("is_exposed")}, {"is_replicated", v.get("is_replicated")},
            });
        }
        details["variables"] = jvars;

        // Parameters
        auto params = query(db,
            "SELECT param_name, param_type, param_group, default_value FROM parameters WHERE asset_id = ?", {aid});
        json jparams = json::array();
        for (auto& p : params) {
            jparams.push_back({
                {"param_name", p.get("param_name")}, {"param_type", p.get("param_type")},
                {"param_group", p.get("param_group")}, {"default_value", p.get("default_value")},
            });
        }
        details["parameters"] = jparams;

        std::cout << details.dump(2) << std::endl;
    }

    Row asset_by_id(const std::string& id) {
        auto rows = query(db, "SELECT id, package_path, asset_name, asset_class, module_name FROM assets WHERE id = ?", {id});
        return rows.empty() ? Row{} : rows[0];
    }

    Row asset_by_path(const std::string& path) {
        auto rows = query(db, "SELECT id, package_path, asset_name, asset_class, module_name FROM assets WHERE package_path = ?", {path});
        return rows.empty() ? Row{} : rows[0];
    }

    json asset_json(const Row& r, int depth = -1) {
        json o = {
            {"id", r.get_int64("id")},
            {"asset_path", r.get("package_path")},
            {"asset_name", r.get("asset_name")},
            {"asset_class", r.get("asset_class")},
            {"module_name", r.get("module_name")},
        };
        if (depth >= 0) o["depth"] = depth;
        return o;
    }

    int asset_class_weight(const std::string& cls) {
        if (cls.find("World") != std::string::npos || cls.find("Level") != std::string::npos) return 5;
        if (cls.find("GameplayAbility") != std::string::npos || cls.find("AttributeSet") != std::string::npos || cls.find("GameplayEffect") != std::string::npos) return 4;
        if (cls.find("Blueprint") != std::string::npos) return 3;
        if (cls.find("NiagaraSystem") != std::string::npos || cls.find("Material") != std::string::npos) return 2;
        if (cls.find("DataTable") != std::string::npos || cls.find("DataAsset") != std::string::npos) return 2;
        return 1;
    }

    json project_impact_radius_json(const std::string& asset_path, const std::string& direction, int max_depth, int max_results, const std::string& dependency_type) {
        int depth = clamp_int(max_depth <= 0 ? 2 : max_depth, 0, 8);
        int limit = clamp_int(max_results <= 0 ? 200 : max_results, 1, 1000);
        json root = {
            {"input", {{"asset_path", asset_path}, {"direction", direction}}},
            {"limits", {{"max_depth", depth}, {"max_results", limit}}},
        };
        if (!dependency_type.empty()) root["input"]["dependency_type"] = dependency_type;
        Row seed = asset_by_path(asset_path);
        if (seed.cols.empty()) {
            root["status"] = "error";
            root["summary"] = "Asset not found in ProjectIndex: " + asset_path;
            root["impacted_assets"] = json::array();
            root["edges"] = json::array();
            root["truncated"] = false;
            add_next(root, {"project.search", "project.find_by_type"});
            return root;
        }
        root["seed"] = asset_json(seed, 0);
        bool in = direction != "out";
        bool out = direction != "in";
        std::set<std::string> visited;
        std::vector<std::string> frontier = {seed.get("id")};
        visited.insert(seed.get("id"));
        json impacted = json::array();
        json edges = json::array();
        bool truncated = false;
        int emitted = 0;
        for (int d = 1; d <= depth && !frontier.empty() && !truncated; ++d) {
            std::vector<std::string> next;
            for (const auto& cur : frontier) {
                Rows edge_rows;
                if (out) {
                    auto rows = query(db, "SELECT id, source_asset_id, target_asset_id, dependency_type FROM dependencies WHERE source_asset_id = ?", {cur});
                    edge_rows.insert(edge_rows.end(), rows.begin(), rows.end());
                }
                if (in) {
                    auto rows = query(db, "SELECT id, source_asset_id, target_asset_id, dependency_type FROM dependencies WHERE target_asset_id = ?", {cur});
                    edge_rows.insert(edge_rows.end(), rows.begin(), rows.end());
                }
                for (const auto& e : edge_rows) {
                    if (!dependency_type.empty() && e.get("dependency_type") != dependency_type) continue;
                    std::string other = (e.get("source_asset_id") == cur) ? e.get("target_asset_id") : e.get("source_asset_id");
                    if (other.empty() || other == cur) continue;
                    edges.push_back({{"from", e.get_int64("source_asset_id")}, {"to", e.get_int64("target_asset_id")}, {"dependency_type", e.get("dependency_type")}, {"depth", d}});
                    if (visited.count(other)) continue;
                    visited.insert(other);
                    if (emitted >= limit) { truncated = true; break; }
                    Row a = asset_by_id(other);
                    if (!a.cols.empty()) {
                        impacted.push_back(asset_json(a, d));
                        ++emitted;
                    }
                    next.push_back(other);
                }
                if (truncated) break;
            }
            frontier = std::move(next);
        }
        root["status"] = "ok";
        root["summary"] = std::to_string(emitted) + " impacted asset(s) within depth " + std::to_string(depth) + " (" + direction + ") of " + asset_path + (truncated ? " [truncated]" : "");
        root["impacted_assets"] = impacted;
        root["edges"] = edges;
        root["truncated"] = truncated;
        add_next(root, {"project.review_context", "project.risk_score", "project.get_asset_details"});
        return root;
    }

    void impact_radius(const Args& args) {
        std::string asset_path = args.opt("asset_path");
        if (asset_path.empty() && !args.positional.empty()) asset_path = args.positional[0];
        if (asset_path.empty()) die("impact_radius requires an asset_path argument");
        print_json(project_impact_radius_json(asset_path, args.opt("direction", "both"), args.opt_int("max_depth", 2), args.opt_int("max_results", 200), args.opt("dependency_type")));
    }

    json project_health_json(bool include_counts) {
        json root = {
            {"input", {{"include_counts", include_counts}}},
            {"limits", {{"include_counts", include_counts}}},
            {"checks", json::array()},
            {"warnings", json::array()},
            {"truncated", false},
        };
        auto check = [&](const std::string& name, bool pass, const std::string& detail) {
            root["checks"].push_back({{"check", name}, {"result", pass ? "ok" : "warning"}, {"detail", detail}});
            if (!pass) root["warnings"].push_back(detail);
        };
        for (const char* t : {"assets", "nodes", "connections", "variables", "parameters", "dependencies", "actors", "tags", "tag_references", "configs", "cpp_symbols", "datatable_rows", "meta"})
            check(std::string("table:") + t, object_exists(db, "table", t), object_exists(db, "table", t) ? std::string("table ") + t + " present" : std::string("missing table ") + t);
        for (const char* f : {"fts_assets", "fts_nodes"})
            check(std::string("fts:") + f, object_exists(db, "table", f), object_exists(db, "table", f) ? std::string("FTS table ") + f + " present" : std::string("missing FTS table ") + f);
        for (const char* tr : {"fts_assets_ai", "fts_assets_ad", "fts_assets_au", "fts_nodes_ai", "fts_nodes_ad", "fts_nodes_au"})
            check(std::string("trigger:") + tr, object_exists(db, "trigger", tr), object_exists(db, "trigger", tr) ? std::string("trigger ") + tr + " present" : std::string("missing trigger ") + tr + " (FTS may drift)");
        std::string schema_ver = scalar_str(db, "SELECT value FROM meta WHERE key = 'schema_version';");
        check("meta:schema_version", !schema_ver.empty(), schema_ver.empty() ? "meta.schema_version missing (pre-v2 / corrupt)" : "schema_version=" + schema_ver);
        bool saved_hash = false;
        for (const auto& col : query(db, "PRAGMA table_info(assets);")) {
            if (col.get("name") == "saved_hash") saved_hash = true;
        }
        check("schema:assets.saved_hash", saved_hash, saved_hash ? "assets.saved_hash present (v2)" : "assets.saved_hash missing (incremental index disabled)");
        int64_t orphan_deps = count_rows(db,
            "SELECT COUNT(*) FROM dependencies d "
            "WHERE d.source_asset_id NOT IN (SELECT id FROM assets) "
            "OR d.target_asset_id NOT IN (SELECT id FROM assets);");
        check("integrity:orphan_dependencies", orphan_deps == 0, orphan_deps == 0 ? "no orphan dependency rows" : std::to_string(orphan_deps) + " orphan dependency row(s)");
        int64_t acnt = count_rows(db, "SELECT COUNT(*) FROM assets;");
        int64_t facnt = count_rows(db, "SELECT COUNT(*) FROM fts_assets;");
        int64_t ncnt = count_rows(db, "SELECT COUNT(*) FROM nodes;");
        int64_t fncnt = count_rows(db, "SELECT COUNT(*) FROM fts_nodes;");
        check("fts:assets_row_parity", acnt == facnt, "assets=" + std::to_string(acnt) + " fts_assets=" + std::to_string(facnt) + (acnt == facnt ? "" : " (mismatch -> project.repair_fts)"));
        check("fts:nodes_row_parity", ncnt == fncnt, "nodes=" + std::to_string(ncnt) + " fts_nodes=" + std::to_string(fncnt) + (ncnt == fncnt ? "" : " (mismatch -> project.repair_fts)"));
        root["schema"] = {{"schema_version", schema_ver}, {"journal_mode", scalar_str(db, "PRAGMA journal_mode;")}};
        if (include_counts) root["row_counts"] = {{"assets", acnt}, {"nodes", ncnt}, {"dependencies", count_rows(db, "SELECT COUNT(*) FROM dependencies;")}};
        // RX-2: CRG projection-cache health parity (mirrors editor Health).
        int64_t dep_cnt = count_rows(db, "SELECT COUNT(*) FROM dependencies;");
        append_crg_health_checks(db, "project", acnt, dep_cnt, root);
        root["status"] = root["warnings"].empty() ? "ok" : "warning";
        root["summary"] = root["warnings"].empty() ? "ProjectIndex schema, triggers, FTS parity, CRG cache and integrity OK" : std::to_string(root["warnings"].size()) + " health warning(s)";
        add_next(root, {"project.repair_crg_cache", "project.repair_fts", "project.get_stats"});
        return root;
    }

    void health(const Args& args) {
        print_json(project_health_json(args.opt_bool("include_counts", true)));
    }

    json project_repair_fts_json(const std::string& target, bool execute) {
        std::string t = target.empty() ? "all" : target;
        json root = {
            {"input", {{"target", t}, {"execute", execute}}},
            {"limits", {{"target", t}, {"execute", execute}}},
            {"warnings", json::array()},
            {"plan", json::array()},
            {"truncated", false},
        };
        std::vector<std::string> tables;
        if (t == "all" || t == "assets") tables.push_back("fts_assets");
        if (t == "all" || t == "nodes") tables.push_back("fts_nodes");
        if (tables.empty()) {
            root["status"] = "error";
            root["summary"] = "Unknown target '" + t + "' (expected all|assets|nodes)";
            add_next(root, {"project.health"});
            return root;
        }
        json before = json::object();
        for (const auto& table : tables) {
            before[table] = count_rows(db, "SELECT COUNT(*) FROM " + table + ";");
            root["plan"].push_back("INSERT INTO " + table + "(" + table + ") VALUES('rebuild');");
        }
        root["before"] = before;
        if (!execute) {
            root["status"] = "ok";
            root["summary"] = "Dry-run: " + std::to_string(tables.size()) + " FTS table(s) would be rebuilt. Pass --execute to apply.";
            root["after"] = json::object();
            add_next(root, {"project.repair_fts", "project.health"});
            return root;
        }
        db.exec("BEGIN;");
        for (const auto& table : tables) {
            db.exec(("INSERT INTO " + table + "(" + table + ") VALUES('rebuild');").c_str());
        }
        db.exec("COMMIT;");
        json after = json::object();
        for (const auto& table : tables) after[table] = count_rows(db, "SELECT COUNT(*) FROM " + table + ";");
        root["after"] = after;
        root["status"] = "ok";
        root["summary"] = "Rebuilt " + std::to_string(tables.size()) + " FTS table(s)";
        add_next(root, {"project.health", "project.search"});
        return root;
    }

    void repair_fts(const Args& args) {
        print_json(project_repair_fts_json(args.opt("target", "all"), args.opt_bool("execute", false)));
    }

    void repair_crg_cache(const Args& args) {
        print_json(crg_repair_cache_json(db, "project", args.opt("scope", "all"), args.opt_bool("execute", false)));
    }

    json score_asset(const Row& asset) {
        std::string id = asset.get("id");
        auto out = query(db, "SELECT dependency_type FROM dependencies WHERE source_asset_id = ?", {id});
        auto in = query(db, "SELECT dependency_type FROM dependencies WHERE target_asset_id = ?", {id});
        int hard_in = 0;
        for (const auto& d : in) if (d.get("dependency_type") == "Hard") ++hard_in;
        int64_t node_count = count_rows(db, "SELECT COUNT(*) FROM nodes WHERE asset_id = " + id + ";");
        int64_t var_count = count_rows(db, "SELECT COUNT(*) FROM variables WHERE asset_id = " + id + ";");
        int64_t param_count = count_rows(db, "SELECT COUNT(*) FROM parameters WHERE asset_id = " + id + ";");
        int64_t tag_refs = count_rows(db, "SELECT COUNT(*) FROM tag_references WHERE asset_id = " + id + ";");
        int class_w = asset_class_weight(asset.get("asset_class"));
        auto sensitivity = sensitivity_factor(
            asset.get("asset_class") + " " + asset.get("package_path") + " " + asset.get("asset_name"),
            false);
        json reasons = json::array();
        double raw = 0.0;
        auto factor = [&](double c, const std::string& why) {
            if (c > 0.0) { raw += c; reasons.push_back(why); }
        };
        factor(std::min<double>(in.size(), 30) / 30.0 * 0.30, "inbound dependency fan-in: " + std::to_string(in.size()) + " referencer(s)");
        factor(std::min<double>(hard_in, 20) / 20.0 * 0.20, "hard inbound dependencies: " + std::to_string(hard_in));
        factor(std::min<double>(out.size(), 30) / 30.0 * 0.10, "outbound dependencies: " + std::to_string(out.size()));
        factor((class_w - 1) / 4.0 * 0.20, "asset class weight: " + asset.get("asset_class") + " (w=" + std::to_string(class_w) + ")");
        factor(sensitivity.first, sensitivity.second);
        factor(std::min<double>(node_count, 400) / 400.0 * 0.15, "graph density: " + std::to_string(node_count) + " node(s), " + std::to_string(var_count) + " var(s), " + std::to_string(param_count) + " param(s)");
        factor(std::min<double>(tag_refs, 20) / 20.0 * 0.05, "gameplay tag involvement: " + std::to_string(tag_refs) + " reference(s)");
        if (node_count == 0 && asset.get("asset_class").find("Blueprint") != std::string::npos) reasons.push_back("missing detail signal: Blueprint indexed with 0 graph nodes (stale index?)");
        double score = std::max(0.0, std::min(raw, 1.0));
        return {
            {"id", asset.get_int64("id")},
            {"asset_path", asset.get("package_path")},
            {"asset_name", asset.get("asset_name")},
            {"asset_class", asset.get("asset_class")},
            {"score", std::round(score * 1000.0) / 1000.0},
            {"tier", tier_for(score)},
            {"reasons", reasons},
            {"raw_counts", {
                {"inbound", in.size()},
                {"inbound_hard", hard_in},
                {"outbound", out.size()},
                {"nodes", node_count},
                {"variables", var_count},
                {"parameters", param_count},
                {"tag_references", tag_refs},
                {"class_weight", class_w},
                {"sensitivity", sensitivity.first},
            }},
        };
    }

    json project_risk_score_json(const std::string& seed_path, int limit, const std::string& min_tier) {
        int cap = clamp_int(limit <= 0 ? 20 : limit, 1, 200);
        json root = {{"input", {{"seed", seed_path}}}, {"limits", {{"limit", cap}, {"min_tier", min_tier.empty() ? "low" : min_tier}}}};
        json items = json::array();
        bool any_cache_hit = false;
        auto score_or_cached = [&](const Row& a) -> json {
            json cached = try_cached_risk(db, "project", "assets", a.get_int64("id"));
            if (!cached.is_null()) {
                any_cache_hit = true;
                cached["id"] = a.get_int64("id");
                cached["asset_path"] = a.get("package_path");
                cached["asset_name"] = a.get("asset_name");
                cached["asset_class"] = a.get("asset_class");
                return cached;
            }
            json s = score_asset(a);
            s["cache"] = {{"status", crg_cache_present(db) ? "miss" : "unavailable"}};
            s["scoring_version"] = "3";
            return s;
        };
        if (!seed_path.empty()) {
            Row seed = asset_by_path(seed_path);
            if (seed.cols.empty()) {
                root["status"] = "error";
                root["summary"] = "Asset not found: " + seed_path;
                root["items"] = items;
                root["truncated"] = false;
                add_next(root, {"project.search"});
                return root;
            }
            items.push_back(score_or_cached(seed));
        } else {
            auto candidates = query(db,
                "SELECT target_asset_id, COUNT(*) c FROM dependencies GROUP BY target_asset_id ORDER BY c DESC LIMIT " + std::to_string(cap));
            for (const auto& c : candidates) {
                Row a = asset_by_id(c.get("target_asset_id"));
                if (!a.cols.empty()) items.push_back(score_or_cached(a));
            }
        }
        int min_rank = tier_rank(min_tier.empty() ? "low" : min_tier);
        json filtered = json::array();
        for (const auto& item : items) if (tier_rank(item["tier"].get<std::string>()) >= min_rank) filtered.push_back(item);
        std::sort(filtered.begin(), filtered.end(), [](const json& a, const json& b) { return a["score"].get<double>() > b["score"].get<double>(); });
        const char* sv = "3";
        root["status"] = "ok";
        root["summary"] = std::to_string(filtered.size()) + " asset(s) scored (scoring_version="
            + std::string(sv) + (any_cache_hit ? " cached when available, v3 query fallback; CRG cache hit)" : " cached when available, v3 query fallback)");
        root["scoring_version"] = sv;
        root["items"] = filtered;
        root["truncated"] = false;
        add_next(root, {"project.review_context", "project.impact_radius"});
        return root;
    }

    void risk_score(const Args& args) {
        std::string seed = args.opt("asset_path", args.opt("seed"));
        if (seed.empty() && !args.positional.empty()) seed = args.positional[0];
        print_json(project_risk_score_json(seed, args.opt_int("limit", 20), args.opt("min_tier", "low")));
    }

    json project_review_hotspots_json(const std::string& kind, int limit, int min_lines, bool include_questions) {
        std::string k = kind.empty() ? "all" : lower_copy(kind);
        int cap = clamp_int(limit <= 0 ? 50 : limit, 1, 200);
        int size_floor = std::max(min_lines <= 0 ? 100 : min_lines, 0);
        json root = {
            {"input", {{"kind", k}, {"include_questions", include_questions}}},
            {"limits", {{"limit", cap}, {"min_lines", size_floor}}},
        };
        if (k != "fan_in" && k != "fan_out" && k != "risk" && k != "large" && k != "all") {
            root["status"] = "error";
            root["summary"] = "Unsupported kind for project.review_hotspots (expected fan_in|fan_out|risk|large|all)";
            root["hotspots"] = json::array();
            root["truncated"] = false;
            add_next(root, {"project.review_hotspots", "project.risk_score"});
            return root;
        }

        bool has_cache = crg_cache_present(db);
        std::string cache_join = has_cache
            ? "LEFT JOIN crg_nodes n ON n.domain='project' AND n.native_table='assets' AND n.native_id=c.id "
              "LEFT JOIN crg_node_metrics m ON m.node_id=n.id "
            : "";
        std::string risk_expr = has_cache ? "COALESCE(m.risk_score, c.estimated_risk)" : "c.estimated_risk";
        std::string tier_expr = has_cache
            ? "COALESCE(m.risk_tier, CASE WHEN c.estimated_risk >= 0.66 THEN 'high' WHEN c.estimated_risk >= 0.33 THEN 'medium' ELSE 'low' END)"
            : "CASE WHEN c.estimated_risk >= 0.66 THEN 'high' WHEN c.estimated_risk >= 0.33 THEN 'medium' ELSE 'low' END";
        std::string where = (k == "large")
            ? "WHERE size_signal >= " + std::to_string(size_floor) + " "
            : "WHERE fan_in > 0 OR fan_out > 0 OR hard_in > 0 OR risk_score > 0 OR size_signal >= " + std::to_string(size_floor) + " ";
        std::string order = "ORDER BY hotspot_score DESC, risk_score DESC, fan_in DESC, size_signal DESC ";
        if (k == "fan_in") order = "ORDER BY fan_in DESC, risk_score DESC, hard_in DESC ";
        else if (k == "fan_out") order = "ORDER BY fan_out DESC, risk_score DESC, size_signal DESC ";
        else if (k == "risk") order = "ORDER BY risk_score DESC, fan_in DESC, size_signal DESC ";
        else if (k == "large") order = "ORDER BY size_signal DESC, risk_score DESC, fan_in DESC ";

        std::string sql = R"SQL(
WITH base AS (
 SELECT a.id,a.package_path,a.asset_name,a.asset_class,COALESCE(a.module_name,'') AS module_name,
        (SELECT COUNT(*) FROM dependencies d WHERE d.target_asset_id=a.id) AS fan_in,
        (SELECT COUNT(*) FROM dependencies d WHERE d.source_asset_id=a.id) AS fan_out,
        (SELECT COUNT(*) FROM dependencies d WHERE d.target_asset_id=a.id AND d.dependency_type='Hard') AS hard_in,
        (SELECT COUNT(*) FROM nodes x WHERE x.asset_id=a.id) AS node_count,
        (SELECT COUNT(*) FROM variables x WHERE x.asset_id=a.id) AS variable_count,
        (SELECT COUNT(*) FROM parameters x WHERE x.asset_id=a.id) AS parameter_count,
        (SELECT COUNT(*) FROM tag_references x WHERE x.asset_id=a.id) AS tag_refs,
        CASE
          WHEN a.asset_class LIKE '%World%' OR a.asset_class LIKE '%Level%' THEN 5
          WHEN a.asset_class LIKE '%GameplayAbility%' OR a.asset_class LIKE '%AttributeSet%' OR a.asset_class LIKE '%GameplayEffect%' THEN 4
          WHEN a.asset_class LIKE '%Blueprint%' THEN 3
          WHEN a.asset_class LIKE '%NiagaraSystem%' OR a.asset_class LIKE '%Material%' THEN 2
          WHEN a.asset_class LIKE '%DataTable%' OR a.asset_class LIKE '%DataAsset%' THEN 2
          ELSE 1 END AS class_weight,
        CASE WHEN lower(a.asset_class || ' ' || a.package_path || ' ' || a.asset_name) LIKE '%network%'
            OR lower(a.asset_class || ' ' || a.package_path || ' ' || a.asset_name) LIKE '%replication%'
            OR lower(a.asset_class || ' ' || a.package_path || ' ' || a.asset_name) LIKE '%save%'
            OR lower(a.asset_class || ' ' || a.package_path || ' ' || a.asset_name) LIKE '%serialize%'
            OR lower(a.asset_class || ' ' || a.package_path || ' ' || a.asset_name) LIKE '%auth%'
            OR lower(a.asset_class || ' ' || a.package_path || ' ' || a.asset_name) LIKE '%purchase%'
            OR lower(a.asset_class || ' ' || a.package_path || ' ' || a.asset_name) LIKE '%anticheat%'
            OR lower(a.asset_class || ' ' || a.package_path || ' ' || a.asset_name) LIKE '%crypt%'
            OR lower(a.asset_class || ' ' || a.package_path || ' ' || a.asset_name) LIKE '%exec%'
            OR lower(a.asset_class || ' ' || a.package_path || ' ' || a.asset_name) LIKE '%file%'
          THEN 1 ELSE 0 END AS sensitivity
 FROM assets a
), counts AS (
 SELECT *, (node_count + variable_count + parameter_count + tag_refs) AS size_signal,
        MIN(1.0, MIN(fan_in,30)/30.0*0.30 + MIN(hard_in,20)/20.0*0.20 +
        MIN(fan_out,30)/30.0*0.10 + (class_weight-1)/4.0*0.20 +
        CASE WHEN sensitivity != 0 THEN 0.15 ELSE 0 END +
        MIN(node_count,400)/400.0*0.15 + MIN(tag_refs,20)/20.0*0.05) AS estimated_risk
 FROM base
), scored AS (
 SELECT c.id,c.package_path,c.asset_name,c.asset_class,c.module_name,c.fan_in,c.fan_out,c.hard_in,
        c.node_count,c.variable_count,c.parameter_count,c.tag_refs,c.size_signal,)SQL" + risk_expr + R"SQL( AS risk_score,)SQL" + tier_expr + R"SQL( AS risk_tier
 FROM counts c )SQL" + cache_join + R"SQL(
)
SELECT *, MAX(risk_score, MIN(fan_in,30)/30.0, MIN(fan_out,30)/30.0, MIN(size_signal,500)/500.0) AS hotspot_score
FROM scored )SQL" + where + order + "LIMIT " + std::to_string(cap + 1) + ";";

        Rows rows = query(db, sql);
        bool truncated = (int)rows.size() > cap;
        if (truncated) rows.pop_back();
        json hotspots = json::array();
        json questions = json::array();
        for (const auto& r : rows) {
            int fan_in = r.get_int("fan_in");
            int fan_out = r.get_int("fan_out");
            int size_signal = r.get_int("size_signal");
            double risk = r.get_double("risk_score");
            std::string primary = k;
            if (primary == "all") {
                double best = risk;
                primary = "risk";
                double in_signal = std::min<double>(fan_in, 30) / 30.0;
                double out_signal = std::min<double>(fan_out, 30) / 30.0;
                double large_signal = std::min<double>(size_signal, 500) / 500.0;
                if (in_signal > best) { best = in_signal; primary = "fan_in"; }
                if (out_signal > best) { best = out_signal; primary = "fan_out"; }
                if (large_signal > best) primary = "large";
            }
            json signals = {
                {"fan_in", fan_in},
                {"fan_out", fan_out},
                {"hard_in", r.get_int("hard_in")},
                {"risk_score", std::round(risk * 1000.0) / 1000.0},
                {"risk_tier", r.get("risk_tier", tier_for(risk))},
                {"nodes", r.get_int("node_count")},
                {"variables", r.get_int("variable_count")},
                {"parameters", r.get_int("parameter_count")},
                {"tag_references", r.get_int("tag_refs")},
                {"size_signal", size_signal},
            };
            hotspots.push_back({
                {"primary_kind", primary},
                {"id", r.get_int64("id")},
                {"asset_path", r.get("package_path")},
                {"asset_name", r.get("asset_name")},
                {"asset_class", r.get("asset_class")},
                {"module_name", r.get("module_name")},
                {"signals", signals},
                {"metrics", signals},
            });
            if (include_questions && questions.size() < 5) {
                questions.push_back({
                    {"target", r.get("package_path")},
                    {"reason", primary},
                    {"question", primary == "large"
                        ? "Can this asset's graph/detail size be reduced or validated with focused coverage before editing?"
                        : "Which dependent assets and gameplay paths should be checked before changing this hotspot?"},
                });
            }
        }
        root["status"] = "ok";
        root["summary"] = std::to_string(hotspots.size()) + " project review hotspot(s) ranked by " + k
            + (has_cache ? " using CRG cache when available" : " using native fallback");
        root["hotspots"] = hotspots;
        if (include_questions) root["questions"] = questions;
        root["truncated"] = truncated;
        add_next(root, {"project.review_context", "project.risk_score", "project.impact_radius"});
        return root;
    }

    void review_hotspots(const Args& args) {
        print_json(project_review_hotspots_json(args.opt("kind", "all"),
                                                args.opt_int("limit", 50),
                                                args.opt_int("min_lines", 100),
                                                args.opt_bool("include_questions", true)));
    }

    void review_context(const Args& args) {
        std::string asset_path = args.opt("asset_path");
        if (asset_path.empty() && !args.positional.empty()) asset_path = args.positional[0];
        if (asset_path.empty()) die("review_context requires an asset_path argument");
        std::string direction = args.opt("direction", "both");
        std::string detail = args.opt("detail_level", "minimal");
        bool minimal = detail != "standard";
        int max_depth = args.opt_int("max_depth", 2);
        int max_results = args.opt_int("max_results", 200);
        int effective_results = minimal ? std::min(clamp_int(max_results <= 0 ? 200 : max_results, 1, 1000), 25) : clamp_int(max_results <= 0 ? 200 : max_results, 1, 1000);
        Row seed = asset_by_path(asset_path);
        json root = {{"input", {{"asset_path", asset_path}, {"direction", direction}, {"detail_level", minimal ? "minimal" : "standard"}}},
                     {"limits", {{"max_depth", clamp_int(max_depth <= 0 ? 2 : max_depth, 0, 8)}, {"max_results", effective_results}}}};
        if (seed.cols.empty()) {
            root["status"] = "error";
            root["summary"] = "Asset not found: " + asset_path;
            root["truncated"] = false;
            add_next(root, {"project.search"});
            print_json(root);
            return;
        }
        json risk = score_asset(seed);
        root["risk"] = risk;
        root["top_risks"] = json::array();
        for (size_t i = 0; i < risk["reasons"].size() && i < 5; ++i) root["top_risks"].push_back(risk["reasons"][i]);
        json impact = project_impact_radius_json(asset_path, direction, max_depth, effective_results, "");
        json summary = {{"truncated", impact.value("truncated", false)}};
        summary["impacted_count"] = impact["impacted_assets"].size();
        summary["top_impacted"] = json::array();
        for (size_t i = 0; i < impact["impacted_assets"].size() && i < (minimal ? 5u : 25u); ++i) summary["top_impacted"].push_back(impact["impacted_assets"][i]);
        root["impact"] = summary;
        root["seed"] = asset_json(seed, 0);
        root["context"] = json::array({{{"type", "seed_asset"}, {"asset_path", seed.get("package_path")}, {"asset_name", seed.get("asset_name")}, {"asset_class", seed.get("asset_class")}, {"module_name", seed.get("module_name")}}});
        if (!minimal) root["details"] = asset_json(seed, 0);
        root["status"] = "ok";
        root["summary"] = seed.get("asset_name") + " risk=" + risk["tier"].get<std::string>() + " (" + (minimal ? "minimal" : "standard") + "); review the top impacted assets and high-risk reasons";
        root["truncated"] = impact.value("truncated", false);
        add_next(root, {"project.get_asset_details", "project.impact_radius", "project.find_references"});
        print_json(root);
    }

    // ============================================================
    // RX-1: detect_changes (project) — changed .uasset/.umap paths ->
    // assets -> reused risk (RX-2 cache or query-time) + bounded
    // dependency impact + review priorities. No test-gap (assets have
    // no test concept). changed_paths is the VCS-agnostic input.
    // ============================================================
    json project_detect_changes_json(const std::vector<std::string>& changed_paths,
                                     int max_results, const std::string& detail_level) {
        int cap = clamp_int(max_results <= 0 ? 200 : max_results, 1, 2000);
        bool minimal = (detail_level != "standard");
        json root = {
            {"input", {{"changed_paths", changed_paths}, {"detail_level", minimal ? "minimal" : "standard"}}},
            {"limits", {{"max_results", cap}, {"detail_level", minimal ? "minimal" : "standard"}}},
            {"truncated", false},
        };
        if (changed_paths.empty()) {
            root["status"] = "error";
            root["summary"] = "detect_changes requires >=1 changed path (positional or --changed-paths=a,b)";
            root["changed_entities"] = json::array();
            add_next(root, {"project.search"});
            return root;
        }
        std::set<int64_t> seen;
        json changed = json::array();
        bool truncated = false, any_cache = false;
        for (const auto& rawp : changed_paths) {
            std::string norm = rawp;
            std::replace(norm.begin(), norm.end(), '\\', '/');
            // stem = filename without directory or extension
            std::string base = norm;
            auto sl = base.find_last_of('/');
            if (sl != std::string::npos) base = base.substr(sl + 1);
            auto dot = base.find_last_of('.');
            std::string stem = (dot != std::string::npos) ? base.substr(0, dot) : base;
            auto rows = query(db,
                "SELECT id,package_path,asset_name,asset_class FROM assets "
                "WHERE asset_name = ? OR replace(package_path,'\\','/') LIKE '%/' || ? "
                "ORDER BY id LIMIT " + std::to_string(cap + 1), {stem, stem});
            for (const auto& r : rows) {
                int64_t aid = r.get_int64("id");
                if (seen.count(aid)) continue;
                if ((int)seen.size() >= cap) { truncated = true; break; }
                seen.insert(aid);
                json cached = try_cached_risk(db, "project", "assets", aid);
                json item;
                if (!cached.is_null()) { any_cache = true; item = cached; }
                else { item = score_asset(r); item["cache"] = {{"status", crg_cache_present(db) ? "miss" : "unavailable"}}; item["scoring_version"] = "3"; }
                item["id"] = aid;
                item["asset_path"] = r.get("package_path");
                item["asset_name"] = r.get("asset_name");
                item["asset_class"] = r.get("asset_class");
                changed.push_back(item);
            }
        }
        double overall = 0.0;
        for (const auto& c : changed) overall = std::max(overall, c.value("score", 0.0));
        std::set<int64_t> impacted;
        for (const auto& c : changed) {
            if ((int)impacted.size() >= cap) { truncated = true; break; }
            auto refs = query(db,
                "SELECT DISTINCT source_asset_id FROM dependencies WHERE target_asset_id = ? LIMIT 200",
                {std::to_string(c.value("id", (int64_t)0))});
            for (const auto& rr : refs) {
                int64_t s = rr.get_int64("source_asset_id");
                if (s > 0 && !seen.count(s)) impacted.insert(s);
            }
        }
        std::sort(changed.begin(), changed.end(),
                  [](const json& a, const json& b) { return a.value("score", 0.0) > b.value("score", 0.0); });
        json priorities = json::array();
        for (size_t i = 0; i < changed.size() && i < 10; ++i) priorities.push_back(changed[i]);
        const char* sv = "3";
        root["risk_score"] = std::round(overall * 1000.0) / 1000.0;
        root["scoring_version"] = sv;
        root["truncated"] = truncated;
        root["status"] = "ok";
        root["changed_entity_count"] = changed.size();
        root["impacted_count"] = impacted.size();
        if (minimal) {
            json topn = json::array();
            for (size_t i = 0; i < changed.size() && i < 3; ++i) topn.push_back(changed[i].value("asset_name", std::string()));
            root["summary"] = "changed " + std::to_string(changed.size()) + " asset(s), risk=" + tier_for(overall)
                + ", scoring_version=" + sv;
            root["review_priorities"] = topn;
        } else {
            root["summary"] = "changed " + std::to_string(changed.size()) + " asset(s) across "
                + std::to_string(changed_paths.size()) + " path(s); risk=" + tier_for(overall)
                + " scoring_version=" + sv;
            root["changed_entities"] = changed;
            root["impact"] = {{"depth", 1}, {"impacted_count", impacted.size()}};
            root["test_gaps"] = json::array();  // assets have no native test concept
            root["review_priorities"] = priorities;
        }
        if (changed.empty()) {
            root["summary"] = "no indexed asset matched the given changed path(s)";
            add_next(root, {"project.search", "project.find_by_type"});
        } else {
            add_next(root, {"project.review_context", "project.impact_radius", "project.risk_score"});
        }
        return root;
    }

    void detect_changes(const Args& args) {
        std::vector<std::string> paths = args.positional;
        std::string csv = args.opt("changed_paths");
        if (!csv.empty()) {
            std::stringstream ss(csv);
            std::string p;
            while (std::getline(ss, p, ',')) if (!p.empty()) paths.push_back(p);
        }
        print_json(project_detect_changes_json(paths, args.opt_int("max_results", 200),
                                               args.opt("detail_level", "minimal")));
    }

    // ============================================================
    // RX-3: find_unused — advisory orphan-asset detection.
    // Assets that are never a dependencies.target_asset_id (0 referencers)
    // and are not roots (World/Level/PrimaryAssetLabel). Advisory: soft /
    // path-based / config references are not in the dependency table, so
    // confidence is lowered for classes commonly referenced indirectly.
    // ============================================================
    json project_find_unused_json(const std::string& kind, int limit, const std::string& min_conf) {
        int cap = clamp_int(limit <= 0 ? 100 : limit, 1, 1000);
        json root = {
            {"input", {{"kind", kind.empty() ? "all" : kind}, {"min_confidence", min_conf.empty() ? "low" : min_conf}}},
            {"limits", {{"limit", cap}, {"min_confidence", min_conf.empty() ? "low" : min_conf}}},
            {"truncated", false},
        };
        std::vector<std::string> params;
        std::string kfilter;
        if (!kind.empty() && kind != "all") { kfilter = " AND a.asset_class = ?"; params.push_back(kind); }
        auto rows = query(db,
            "SELECT a.id,a.package_path,a.asset_name,a.asset_class FROM assets a "
            "WHERE NOT EXISTS (SELECT 1 FROM dependencies d WHERE d.target_asset_id = a.id) "
            "AND a.asset_class NOT LIKE '%World%' AND a.asset_class NOT LIKE '%Level%' "
            "AND a.asset_class NOT LIKE '%PrimaryAssetLabel%' "
            "AND a.asset_class NOT LIKE '%DirectoryPlaceholder%'" + kfilter + " "
            "ORDER BY a.id LIMIT " + std::to_string(cap + 1), params);
        bool truncated = (int)rows.size() > cap;
        if (truncated) rows.pop_back();
        auto conf_rank = [](const std::string& c) { return c == "high" ? 2 : c == "medium" ? 1 : 0; };
        int min_rank = conf_rank(min_conf.empty() ? "low" : min_conf);
        json items = json::array();
        for (const auto& r : rows) {
            std::string cls = r.get("asset_class");
            // Classes commonly referenced by soft/path/config (not in the
            // dependency table) -> lower confidence to advisory 'low'.
            bool indirect = cls.find("Texture") != std::string::npos
                || cls.find("Material") != std::string::npos
                || cls.find("Sound") != std::string::npos
                || cls.find("DataTable") != std::string::npos
                || cls.find("DataAsset") != std::string::npos
                || cls.find("Paper") != std::string::npos;
            std::string conf = indirect ? "low" : "medium";
            if (conf_rank(conf) < min_rank) continue;
            json reasons = json::array();
            reasons.push_back("0 inbound dependency referencers (no asset hard/soft-references it in the index)");
            reasons.push_back(indirect
                ? cls + " is often referenced by soft/path/config (not in the dependency table) — low confidence, advisory"
                : "not a World/Level/PrimaryAssetLabel root — likely orphan content (advisory; verify soft refs)");
            items.push_back({
                {"id", r.get_int64("id")},
                {"asset_path", r.get("package_path")},
                {"asset_name", r.get("asset_name")},
                {"asset_class", cls},
                {"confidence", conf},
                {"reasons", reasons},
            });
        }
        root["status"] = "ok";
        root["summary"] = std::to_string(items.size()) + " advisory unused asset candidate(s) "
            "(recall-first; verify soft/path references before deletion)";
        root["items"] = items;
        root["truncated"] = truncated;
        add_next(root, {"project.find_references", "project.review_context", "project.impact_radius"});
        return root;
    }

    void find_unused(const Args& args) {
        print_json(project_find_unused_json(args.opt("kind", "all"), args.opt_int("limit", 100),
                                            args.opt("min_confidence", "low")));
    }

    json project_pre_merge_check_json(const std::vector<std::string>& changed_paths,
                                      int max_results, int unused_limit,
                                      const std::string& detail_level,
                                      bool include_unused) {
        bool standard = detail_level == "standard";
        int change_cap = clamp_int(max_results <= 0 ? 200 : max_results, 1, 2000);
        int unused_cap = clamp_int(unused_limit <= 0 ? 20 : unused_limit, 1, 1000);
        json root = {
            {"input", {
                {"changed_paths", changed_paths},
                {"detail_level", standard ? "standard" : "minimal"},
                {"include_unused", include_unused},
            }},
            {"limits", {{"max_results", change_cap}, {"unused_limit", unused_cap}}},
            {"scoring_version", "3"},
        };

        json health = project_health_json(false);
        json changes = project_detect_changes_json(changed_paths, change_cap, standard ? "standard" : "minimal");
        json unused = include_unused ? project_find_unused_json("all", unused_cap, "low") : json();

        json checks = json::array();
        json findings = json::array();
        int severity = 0;

        std::string health_status = json_string_field(health, "status", "error");
        int health_severity = health_status == "error" ? 2 : health_status == "warning" ? 1 : 0;
        premerge_add_check(checks, severity, "health", health_status,
                           json_string_field(health, "summary", "Project health could not run"),
                           health_severity);
        if (health_severity > 0) {
            premerge_add_finding(findings, health_severity >= 2 ? "error" : "warning", "health",
                                 json_string_field(health, "summary", "Project health failed"));
        }

        std::string change_status = json_string_field(changes, "status", "error");
        int changed_count = json_int_field(changes, "changed_entity_count");
        int impacted_count = json_int_field(changes, "impacted_count");
        double risk_score = json_num_field(changes, "risk_score");
        int change_severity = change_status == "error" ? 2 : 0;
        if (change_severity == 0 && changed_count == 0) {
            change_severity = 1;
            premerge_add_finding(findings, "warning", "detect_changes",
                                 "No indexed asset matched the changed path set");
        }
        if (risk_score >= 0.66) {
            change_severity = std::max(change_severity, 1);
            premerge_add_finding(findings, "warning", "detect_changes",
                                 "Changed asset risk score is high: " + std::to_string(std::round(risk_score * 1000.0) / 1000.0));
        }
        if (impacted_count > 50) {
            change_severity = std::max(change_severity, 1);
            premerge_add_finding(findings, "warning", "detect_changes",
                                 "Changed asset set has broad direct impact: " + std::to_string(impacted_count) + " referencer(s)");
        }
        if (change_status == "error") {
            premerge_add_finding(findings, "error", "detect_changes",
                                 json_string_field(changes, "summary", "detect_changes failed"));
        }
        premerge_add_check(checks, severity, "detect_changes", change_status,
                           std::to_string(changed_count) + " changed asset(s), " +
                               std::to_string(impacted_count) + " impacted referencer(s), risk=" +
                               std::to_string(std::round(risk_score * 1000.0) / 1000.0),
                           change_severity);

        int unused_count = 0;
        if (include_unused && unused.is_object()) {
            if (unused.contains("items") && unused["items"].is_array())
                unused_count = static_cast<int>(unused["items"].size());
            std::string unused_status = json_string_field(unused, "status", "error");
            int unused_severity = unused_status == "error" ? 2 : unused_count > 0 ? 1 : 0;
            premerge_add_check(checks, severity, "find_unused", unused_status,
                               std::to_string(unused_count) + " advisory unused asset candidate(s) sampled",
                               unused_severity);
            if (unused_count > 0) {
                premerge_add_finding(findings, "warning", "find_unused",
                                     std::to_string(unused_count) + " advisory unused asset candidate(s) present in sampled index");
            }
        }

        std::string decision = severity >= 2 ? "fail" : severity == 1 ? "warn" : "pass";
        root["status"] = severity >= 2 ? "error" : severity == 1 ? "warning" : "ok";
        root["decision"] = decision;
        root["summary"] = "Project pre-merge check " + decision + ": " +
            std::to_string(changed_count) + " changed asset(s), " +
            std::to_string(impacted_count) + " impacted referencer(s), " +
            std::to_string(findings.size()) + " finding(s)";
        root["risk_score"] = std::round(risk_score * 1000.0) / 1000.0;
        root["checks"] = checks;
        root["findings"] = findings;
        root["changed_entity_count"] = changed_count;
        root["impacted_count"] = impacted_count;
        root["unused_count"] = unused_count;
        root["truncated"] = json_bool_field(changes, "truncated") || json_bool_field(unused, "truncated");
        if (standard) {
            root["health"] = health;
            root["change_analysis"] = changes;
            if (include_unused && unused.is_object()) root["unused"] = unused;
        }
        add_next(root, severity >= 2
            ? std::initializer_list<const char*>{"project.health", "project.search"}
            : std::initializer_list<const char*>{"project.detect_changes", "project.review_context", "project.find_unused"});
        return root;
    }

    void pre_merge_check(const Args& args) {
        print_json(project_pre_merge_check_json(collect_changed_paths(args),
                                                args.opt_int("max_results", 200),
                                                args.opt_int("unused_limit", 20),
                                                args.opt("detail_level", "minimal"),
                                                args.opt_bool("include_unused", true)));
    }

    void snapshot(const Args& args) {
        std::string label = args.opt("label");
        if (label.empty() && !args.positional.empty()) label = args.positional[0];
        print_json(crg_snapshot_json(db, "project", label, args.opt_bool("execute", false)));
    }

    void diff_snapshots(const Args& args) {
        std::string before = args.opt("before");
        std::string after = args.opt("after");
        if (before.empty()) {
            if (!args.positional.empty()) before = args.positional[0];
            if (after.empty() && args.positional.size() > 1) after = args.positional[1];
        } else if (after.empty() && !args.positional.empty()) {
            after = args.positional[0];
        }
        print_json(crg_diff_snapshots_json(db, "project", before, after, args.opt_int("limit", 100)));
    }
};

// ============================================================
// Context actions
// ============================================================

class ContextActions {
    Database project_db;
    Database source_db;
    bool project_open = false;
    bool source_open = false;
    json open_warnings = json::array();

public:
    void open(const std::string& project_path, const std::string& source_path) {
        if (fs::exists(project_path)) {
            project_db.open(project_path, true);
            project_open = true;
        } else {
            open_warnings.push_back("project index database unavailable: " + project_path);
        }
        if (fs::exists(source_path)) {
            source_db.open(source_path, true);
            source_open = true;
        } else {
            open_warnings.push_back("source database unavailable: " + source_path);
        }
    }

    void bridge_asset_symbols(const Args& args) {
        std::string asset_path = args.opt("asset_path");
        std::string symbol_seed = args.opt("symbol");
        if (asset_path.empty() && args.positional.size() == 1 && symbol_seed.empty())
            asset_path = args.positional[0];
        asset_path = trim_copy(asset_path);
        symbol_seed = trim_copy(symbol_seed);

        bool has_asset = !asset_path.empty();
        bool has_symbol = !symbol_seed.empty();
        if (has_asset == has_symbol)
            die("bridge_asset_symbols requires exactly one of --asset-path or --symbol");

        int limit = clamp_int(args.opt_int("limit", 20), 1, 100);
        std::string detail_level = args.opt("detail_level", "minimal");
        if (detail_level != "minimal" && detail_level != "standard")
            die("detail_level must be 'minimal' or 'standard'");
        bool standard = detail_level == "standard";

        json warnings = open_warnings;
        std::vector<BridgeLinkCandidate> candidates;
        std::set<std::string> seen_links;
        std::vector<std::string> candidate_names;
        std::set<std::string> seen_candidate_names;

        auto remember_candidate = [&](const std::string& value) {
            std::string cleaned = trim_copy(value);
            if (cleaned.empty()) return;
            std::string key = lower_copy(cleaned);
            if (seen_candidate_names.insert(key).second) candidate_names.push_back(cleaned);
        };

        if (has_asset) {
            Row asset;
            if (project_open) {
                asset = bridge_asset_by_path(project_db, asset_path);
                if (asset.cols.empty())
                    warnings.push_back("asset '" + asset_path + "' was not found in the project index");
            }
            std::string asset_name = asset.cols.empty() ? bridge_clean_token(asset_path) : asset.get("asset_name");
            std::string asset_class = asset.cols.empty() ? "" : asset.get("asset_class");
            json asset_object = bridge_asset_object(asset, asset_path, asset_name, asset_class);
            for (const auto& candidate : bridge_asset_symbol_candidates(asset_path, asset_name, asset_class)) {
                remember_candidate(candidate);
                if (source_open)
                    bridge_add_source_matches_for_candidate(source_db, asset_object, asset_name, candidate,
                                                            limit, standard, candidates, seen_links);
            }
            if (!source_open)
                warnings.push_back("source database unavailable. Provide --source-db or run source.trigger_project_reindex first.");
        } else {
            std::vector<Row> symbols;
            if (source_open) {
                symbols = bridge_source_symbols_exact(source_db, symbol_seed, limit);
                if (symbols.empty())
                    symbols = bridge_source_symbols_fts(source_db, symbol_seed, limit);
                if (symbols.empty())
                    warnings.push_back("symbol '" + symbol_seed + "' was not found in the source index");
            } else {
                warnings.push_back("source database unavailable. Provide --source-db or run source.trigger_project_reindex first.");
            }

            if (!project_open)
                warnings.push_back("project index database unavailable. Provide --project-db or run project indexing first.");

            for (const auto& symbol : symbols) {
                for (const auto& candidate : bridge_symbol_asset_candidates(symbol.get("name"), symbol.get("qualified_name"))) {
                    remember_candidate(candidate);
                    if (project_open)
                        bridge_add_asset_matches_for_symbol(project_db, source_db, symbol, candidate,
                                                            limit, standard, candidates, seen_links);
                }
            }
        }

        std::sort(candidates.begin(), candidates.end(),
                  [](const BridgeLinkCandidate& a, const BridgeLinkCandidate& b) {
                      return a.score > b.score;
                  });
        bool truncated = (int)candidates.size() > limit;
        if (truncated) candidates.resize(limit);

        json links = json::array();
        for (const auto& candidate : candidates)
            links.push_back(candidate.object);

        json input = {
            {"mode", has_asset ? "asset" : "symbol"},
            {"candidate_names", candidate_names},
        };
        if (has_asset) input["asset_path"] = asset_path;
        else input["symbol"] = symbol_seed;

        json next_actions = json::array({
            "Use context.build_attachment on matching asset/source_symbol items for prompt materialization.",
            "Use source.review_context or project.review_context on high-confidence matches before code review.",
        });
        if (!warnings.empty())
            next_actions.push_back("Run context.get_index_status or context.start_indexing if an index is unavailable or stale.");

        json root = {
            {"status", warnings.empty() ? "ok" : "warning"},
            {"success", true},
            {"read_only", true},
            {"lexical_only", true},
            {"input", input},
            {"limits", {{"limit", limit}, {"detail_level", detail_level}}},
            {"links", links},
            {"warnings", warnings},
            {"count", links.size()},
            {"truncated", truncated},
            {"next_actions", next_actions},
        };
        print_json(root);
    }
};

// ============================================================
// DB path resolution
// ============================================================

static std::string resolve_db_dir() {
    // Default: ../../Saved/ relative to exe location
    // (exe at Plugins/Monolith/Tools/MonolithQuery/ or Plugins/Monolith/Binaries/)
    // Get exe path
    fs::path exe_path;
#ifdef _WIN32
    char buf[MAX_PATH];
    DWORD len = GetModuleFileNameA(nullptr, buf, MAX_PATH);
    if (len > 0 && len < MAX_PATH)
        exe_path = buf;
#endif
    if (exe_path.empty()) {
        // Fallback: try current directory
        return ".";
    }

    auto exe_dir = exe_path.parent_path();

    // Try ../../Saved/ (from Tools/MonolithQuery/)
    auto saved1 = exe_dir / ".." / ".." / "Saved";
    if (fs::exists(saved1)) return fs::canonical(saved1).string();

    // Try ../Saved/ (from Binaries/)
    auto saved2 = exe_dir / ".." / "Saved";
    if (fs::exists(saved2)) return fs::canonical(saved2).string();

    // Try ./Saved/ (from plugin root)
    auto saved3 = exe_dir / "Saved";
    if (fs::exists(saved3)) return fs::canonical(saved3).string();

    return exe_dir.string();
}

// ============================================================
// Main
// ============================================================

int main(int argc, char* argv[]) {
    Args args = parse_args(argc, argv);

    std::string db_dir = args.opt("db");
    if (db_dir.empty()) db_dir = resolve_db_dir();

    if (args.ns == "source") {
        std::string db_path = args.opt("source_db");
        if (db_path.empty()) db_path = (fs::path(db_dir) / "EngineSource.db").string();

        bool write_action = args.opt_bool("execute", false)
            && (args.action == "repair_fts" || args.action == "snapshot" || args.action == "repair_crg_cache");
        SourceActions sa;
        sa.open(db_path, !write_action);

        static const std::map<std::string, std::function<void(SourceActions&, const Args&)>> actions = {
            {"search_source",       [](SourceActions& s, const Args& a) { s.search_source(a); }},
            {"read_source",         [](SourceActions& s, const Args& a) { s.read_source(a); }},
            {"find_references",     [](SourceActions& s, const Args& a) { s.find_references(a); }},
            {"find_callers",        [](SourceActions& s, const Args& a) { s.find_callers(a); }},
            {"find_callees",        [](SourceActions& s, const Args& a) { s.find_callees(a); }},
            {"get_class_hierarchy", [](SourceActions& s, const Args& a) { s.get_class_hierarchy(a); }},
            {"get_module_info",     [](SourceActions& s, const Args& a) { s.get_module_info(a); }},
            {"get_symbol_context",  [](SourceActions& s, const Args& a) { s.get_symbol_context(a); }},
            {"read_file",           [](SourceActions& s, const Args& a) { s.read_file(a); }},
            {"impact_radius",       [](SourceActions& s, const Args& a) { s.impact_radius(a); }},
            {"health",              [](SourceActions& s, const Args& a) { s.health(a); }},
            {"repair_fts",          [](SourceActions& s, const Args& a) { s.repair_fts(a); }},
            {"repair_crg_cache",    [](SourceActions& s, const Args& a) { s.repair_crg_cache(a); }},
            {"risk_score",          [](SourceActions& s, const Args& a) { s.risk_score(a); }},
            {"review_hotspots",     [](SourceActions& s, const Args& a) { s.review_hotspots(a); }},
            {"review_context",      [](SourceActions& s, const Args& a) { s.review_context(a); }},
            {"detect_changes",      [](SourceActions& s, const Args& a) { s.detect_changes(a); }},
            {"find_unused",         [](SourceActions& s, const Args& a) { s.find_unused(a); }},
            {"pre_merge_check",     [](SourceActions& s, const Args& a) { s.pre_merge_check(a); }},
            {"snapshot",            [](SourceActions& s, const Args& a) { s.snapshot(a); }},
            {"diff_snapshots",      [](SourceActions& s, const Args& a) { s.diff_snapshots(a); }},
        };

        auto it = actions.find(args.action);
        if (it == actions.end()) die("Unknown source action: " + args.action);
        it->second(sa, args);

    } else if (args.ns == "context") {
        std::string project_db = args.opt("project_db");
        if (project_db.empty()) project_db = (fs::path(db_dir) / "ProjectIndex.db").string();
        std::string source_db = args.opt("source_db");
        if (source_db.empty()) source_db = (fs::path(db_dir) / "EngineSource.db").string();

        ContextActions ca;
        ca.open(project_db, source_db);

        static const std::map<std::string, std::function<void(ContextActions&, const Args&)>> actions = {
            {"bridge_asset_symbols", [](ContextActions& c, const Args& a) { c.bridge_asset_symbols(a); }},
        };

        auto it = actions.find(args.action);
        if (it == actions.end()) die("Unknown context action: " + args.action);
        it->second(ca, args);

    } else if (args.ns == "project") {
        std::string db_path = args.opt("project_db");
        if (db_path.empty()) db_path = (fs::path(db_dir) / "ProjectIndex.db").string();

        bool write_action = args.opt_bool("execute", false)
            && (args.action == "repair_fts" || args.action == "snapshot" || args.action == "repair_crg_cache");
        ProjectActions pa;
        pa.open(db_path, !write_action);

        static const std::map<std::string, std::function<void(ProjectActions&, const Args&)>> actions = {
            {"search",            [](ProjectActions& p, const Args& a) { p.search(a); }},
            {"find_by_type",      [](ProjectActions& p, const Args& a) { p.find_by_type(a); }},
            {"find_references",   [](ProjectActions& p, const Args& a) { p.find_references(a); }},
            {"get_stats",         [](ProjectActions& p, const Args& a) { p.get_stats(a); }},
            {"get_asset_details", [](ProjectActions& p, const Args& a) { p.get_asset_details(a); }},
            {"impact_radius",     [](ProjectActions& p, const Args& a) { p.impact_radius(a); }},
            {"health",            [](ProjectActions& p, const Args& a) { p.health(a); }},
            {"repair_fts",        [](ProjectActions& p, const Args& a) { p.repair_fts(a); }},
            {"repair_crg_cache",  [](ProjectActions& p, const Args& a) { p.repair_crg_cache(a); }},
            {"risk_score",        [](ProjectActions& p, const Args& a) { p.risk_score(a); }},
            {"review_hotspots",   [](ProjectActions& p, const Args& a) { p.review_hotspots(a); }},
            {"review_context",    [](ProjectActions& p, const Args& a) { p.review_context(a); }},
            {"detect_changes",    [](ProjectActions& p, const Args& a) { p.detect_changes(a); }},
            {"find_unused",       [](ProjectActions& p, const Args& a) { p.find_unused(a); }},
            {"pre_merge_check",   [](ProjectActions& p, const Args& a) { p.pre_merge_check(a); }},
            {"snapshot",          [](ProjectActions& p, const Args& a) { p.snapshot(a); }},
            {"diff_snapshots",    [](ProjectActions& p, const Args& a) { p.diff_snapshots(a); }},
        };

        auto it = actions.find(args.action);
        if (it == actions.end()) die("Unknown project action: " + args.action);
        it->second(pa, args);

    } else {
        die("Unknown namespace: " + args.ns + " (expected 'source', 'project', or 'context')");
    }

    return 0;
}
