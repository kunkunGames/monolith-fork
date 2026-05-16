// monolith_query.exe — Standalone offline query tool for Monolith databases.
// Replaces monolith_offline.py with zero Python dependency.
// Links sqlite3 amalgamation directly. No Unreal Engine dependency.
//
// Usage:
//   monolith_query.exe source <action> [params...] [--options]
//   monolith_query.exe project <action> [params...] [--options]

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
                  << "  risk_score <symbol> [--limit=N] [--min-tier=low|medium|high]\n"
                  << "  review_context <symbol> [--direction=in|out|both] [--detail-level=minimal|standard]\n"
                  << "\nProject actions:\n"
                  << "  search <query> [--limit=N]\n"
                  << "  find_by_type <asset_class> [--limit=N] [--offset=N]\n"
                  << "  find_references <asset_path>\n"
                  << "  get_stats\n"
                  << "  get_asset_details <asset_path>\n"
                  << "  impact_radius <asset_path> [--direction=in|out|both] [--max-depth=N] [--max-results=N] [--dependency-type=Hard|Soft]\n"
                  << "  health [--include-counts=false]\n"
                  << "  repair_fts [--target=all|assets|nodes] [--execute]\n"
                  << "  risk_score [asset_path] [--limit=N] [--min-tier=low|medium|high]\n"
                  << "  review_context <asset_path> [--direction=in|out|both] [--detail-level=minimal|standard]\n";
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
        root["status"] = root["warnings"].empty() ? "ok" : "warning";
        root["summary"] = root["warnings"].empty() ? "EngineSource schema, triggers, symbols_fts parity and integrity OK" : std::to_string(root["warnings"].size()) + " health warning(s)";
        add_next(root, {"source.repair_fts", "source.trigger_project_reindex", "source.search_source"});
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
        for (const auto& row : rows) {
            json scored = score_symbol(row);
            if (tier_rank(scored["tier"].get<std::string>()) >= min_rank) items.push_back(scored);
        }
        std::sort(items.begin(), items.end(), [](const json& a, const json& b) { return a["score"].get<double>() > b["score"].get<double>(); });
        root["status"] = "ok";
        root["summary"] = std::to_string(items.size()) + " symbol overload(s) scored (scoring_version=1)";
        root["scoring_version"] = "1";
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
        root["status"] = root["warnings"].empty() ? "ok" : "warning";
        root["summary"] = root["warnings"].empty() ? "ProjectIndex schema, triggers, FTS parity and integrity OK" : std::to_string(root["warnings"].size()) + " health warning(s)";
        add_next(root, {"project.repair_fts", "project.get_stats"});
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
        json reasons = json::array();
        double raw = 0.0;
        auto factor = [&](double c, const std::string& why) {
            if (c > 0.0) { raw += c; reasons.push_back(why); }
        };
        factor(std::min<double>(in.size(), 30) / 30.0 * 0.30, "inbound dependency fan-in: " + std::to_string(in.size()) + " referencer(s)");
        factor(std::min<double>(hard_in, 20) / 20.0 * 0.20, "hard inbound dependencies: " + std::to_string(hard_in));
        factor(std::min<double>(out.size(), 30) / 30.0 * 0.10, "outbound dependencies: " + std::to_string(out.size()));
        factor((class_w - 1) / 4.0 * 0.20, "asset class weight: " + asset.get("asset_class") + " (w=" + std::to_string(class_w) + ")");
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
            }},
        };
    }

    json project_risk_score_json(const std::string& seed_path, int limit, const std::string& min_tier) {
        int cap = clamp_int(limit <= 0 ? 20 : limit, 1, 200);
        json root = {{"input", {{"seed", seed_path}}}, {"limits", {{"limit", cap}, {"min_tier", min_tier.empty() ? "low" : min_tier}}}};
        json items = json::array();
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
            items.push_back(score_asset(seed));
        } else {
            auto candidates = query(db,
                "SELECT target_asset_id, COUNT(*) c FROM dependencies GROUP BY target_asset_id ORDER BY c DESC LIMIT " + std::to_string(cap));
            for (const auto& c : candidates) {
                Row a = asset_by_id(c.get("target_asset_id"));
                if (!a.cols.empty()) items.push_back(score_asset(a));
            }
        }
        int min_rank = tier_rank(min_tier.empty() ? "low" : min_tier);
        json filtered = json::array();
        for (const auto& item : items) if (tier_rank(item["tier"].get<std::string>()) >= min_rank) filtered.push_back(item);
        std::sort(filtered.begin(), filtered.end(), [](const json& a, const json& b) { return a["score"].get<double>() > b["score"].get<double>(); });
        root["status"] = "ok";
        root["summary"] = std::to_string(filtered.size()) + " asset(s) scored (scoring_version=1)";
        root["scoring_version"] = "1";
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

        SourceActions sa;
        sa.open(db_path, !(args.action == "repair_fts" && args.opt_bool("execute", false)));

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
            {"risk_score",          [](SourceActions& s, const Args& a) { s.risk_score(a); }},
            {"review_context",      [](SourceActions& s, const Args& a) { s.review_context(a); }},
        };

        auto it = actions.find(args.action);
        if (it == actions.end()) die("Unknown source action: " + args.action);
        it->second(sa, args);

    } else if (args.ns == "project") {
        std::string db_path = args.opt("project_db");
        if (db_path.empty()) db_path = (fs::path(db_dir) / "ProjectIndex.db").string();

        ProjectActions pa;
        pa.open(db_path, !(args.action == "repair_fts" && args.opt_bool("execute", false)));

        static const std::map<std::string, std::function<void(ProjectActions&, const Args&)>> actions = {
            {"search",            [](ProjectActions& p, const Args& a) { p.search(a); }},
            {"find_by_type",      [](ProjectActions& p, const Args& a) { p.find_by_type(a); }},
            {"find_references",   [](ProjectActions& p, const Args& a) { p.find_references(a); }},
            {"get_stats",         [](ProjectActions& p, const Args& a) { p.get_stats(a); }},
            {"get_asset_details", [](ProjectActions& p, const Args& a) { p.get_asset_details(a); }},
            {"impact_radius",     [](ProjectActions& p, const Args& a) { p.impact_radius(a); }},
            {"health",            [](ProjectActions& p, const Args& a) { p.health(a); }},
            {"repair_fts",        [](ProjectActions& p, const Args& a) { p.repair_fts(a); }},
            {"risk_score",        [](ProjectActions& p, const Args& a) { p.risk_score(a); }},
            {"review_context",    [](ProjectActions& p, const Args& a) { p.review_context(a); }},
        };

        auto it = actions.find(args.action);
        if (it == actions.end()) die("Unknown project action: " + args.action);
        it->second(pa, args);

    } else {
        die("Unknown namespace: " + args.ns + " (expected 'source' or 'project')");
    }

    return 0;
}
