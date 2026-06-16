// monolith_query.exe — Standalone offline query tool for Monolith databases.
// Replaces monolith_offline.py with zero Python dependency.
// Links sqlite3 amalgamation directly. No Unreal Engine dependency.
//
// Usage:
//   monolith_query.exe source <action> [params...] [--options]
//   monolith_query.exe project <action> [params...] [--options]
//   monolith_query.exe bridge <action> [params...] [--options]
//   monolith_query.exe monolith guide [--section=NAME]

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
#include <cctype>
#include <cstdint>
#include <iomanip>
#include <mutex>
#include <stdexcept>
#include <cerrno>

#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <bcrypt.h>
#pragma comment(lib, "bcrypt.lib")
#else
#include <fcntl.h>
#include <signal.h>
#include <unistd.h>
#endif

#include "sqlite3.h"
#include <nlohmann/json.hpp>

namespace fs = std::filesystem;
using json = nlohmann::json;

// ============================================================
// Utility
// ============================================================

struct QueryFatal : public std::runtime_error {
    int code = 1;
    bool already_printed = false;

    QueryFatal(const std::string& msg, int exit_code = 1, bool printed = false)
        : std::runtime_error(msg), code(exit_code), already_printed(printed) {}
};

static void die(const std::string& msg) {
    throw QueryFatal(msg);
}

static std::string getenv_str(const char* name, const char* def = "") {
    const char* value = std::getenv(name);
    return value ? std::string(value) : std::string(def);
}

// Query tool invocation daily log helpers.
#include "monolith_query_tool_log.h"

static int levenshtein(const std::string& a, const std::string& b) {
    if (a == b) return 0;
    if (a.empty()) return (int)b.size();
    if (b.empty()) return (int)a.size();

    std::vector<int> prev(b.size() + 1);
    for (size_t j = 0; j <= b.size(); ++j) prev[j] = (int)j;

    for (size_t i = 1; i <= a.size(); ++i) {
        std::vector<int> cur(b.size() + 1);
        cur[0] = (int)i;
        for (size_t j = 1; j <= b.size(); ++j) {
            int cost = (a[i - 1] == b[j - 1]) ? 0 : 1;
            cur[j] = std::min({prev[j] + 1, cur[j - 1] + 1, prev[j - 1] + cost});
        }
        prev = std::move(cur);
    }
    return prev[b.size()];
}

// Return up to top_n keys ranked by Levenshtein-normalised score descending.
static std::vector<std::string> fuzzy_top(const std::string& needle,
                                          const std::vector<std::string>& keys,
                                          int top_n = 3) {
    if (needle.empty() || keys.empty() || top_n <= 0) return {};

    std::vector<std::pair<float, std::string>> scored;
    scored.reserve(keys.size());
    for (const auto& k : keys) {
        int dist = levenshtein(needle, k);
        int max_len = std::max((int)needle.size(), (int)k.size());
        float worst = max_len > 0 ? (float)max_len : 1.0f;
        float score = 1.0f - ((float)dist / worst);
        scored.emplace_back(score, k);
    }
    // Stable sort descending by score — preserves insertion order on ties.
    std::stable_sort(scored.begin(), scored.end(),
                     [](const auto& x, const auto& y) { return x.first > y.first; });

    std::vector<std::string> out;
    int take = std::min(top_n, (int)scored.size());
    out.reserve(take);
    for (int i = 0; i < take; ++i) out.push_back(scored[i].second);
    return out;
}

static std::string did_you_mean_suffix(const std::string& needle,
                                       const std::vector<std::string>& keys) {
    auto sugg = fuzzy_top(needle, keys, 3);
    if (sugg.empty()) return "";
    std::string s = " did_you_mean: ";
    for (size_t i = 0; i < sugg.size(); ++i) {
        if (i > 0) s += ", ";
        s += sugg[i];
    }
    return s;
}

// Namespaces this OFFLINE tool can serve from on-disk SQLite. The live MCP
// server exposes ~29; the rest are LIVE-ONLY (require a running editor).
static const std::vector<std::string>& offline_namespaces() {
    static const std::vector<std::string> ns = {
        "source", "project", "bridge", "monolith", "cppreflect", "network", "decision", "risk"
    };
    return ns;
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

static std::string lower_copy(std::string value);

class Database {
public:
    sqlite3* db = nullptr;

    Database() = default;
    ~Database() { close(); }

    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;

    void close() noexcept {
        if (!db) return;
        sqlite3* handle = db;
        db = nullptr;
        write_open = false;
        sqlite3_close(handle);
    }

    void open(const std::string& path, bool query_only = true) {
        if (!fs::exists(path))
            die("Database not found: " + path);

        if (query_only) {
            std::string recovery_error;
            if (!recover_rollback_journal(path, recovery_error)) {
                die("Rollback journal exists for database and could not be recovered safely: " + path + "-journal - " + recovery_error);
            }
        }

        close();
        int rc = sqlite3_open_v2(path.c_str(), &db, query_only ? SQLITE_OPEN_READONLY : SQLITE_OPEN_READWRITE, nullptr);
        if (rc != SQLITE_OK)
            die("Failed to open database: " + path + " — " + sqlite3_errmsg(db));

        write_open = !query_only;
        exec("PRAGMA busy_timeout=5000;");
        if (query_only) {
            exec("PRAGMA query_only=ON;");
        } else {
            exec("PRAGMA locking_mode=NORMAL;");
            exec("PRAGMA journal_mode=DELETE;");
            exec("PRAGMA synchronous=NORMAL;");
            exec("PRAGMA query_only=OFF;");
        }
    }

    void open_or_create(const std::string& path) {
        fs::path parent = fs::path(path).parent_path();
        if (!parent.empty())
            fs::create_directories(parent);

        close();
        int rc = sqlite3_open_v2(path.c_str(), &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr);
        if (rc != SQLITE_OK)
            die("Failed to open database: " + path + " — " + sqlite3_errmsg(db));

        write_open = true;
        exec("PRAGMA busy_timeout=5000;");
        exec("PRAGMA locking_mode=NORMAL;");
        exec("PRAGMA journal_mode=DELETE;");
        exec("PRAGMA synchronous=NORMAL;");
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

    std::string scalar(const char* sql) {
        sqlite3_stmt* stmt = nullptr;
        int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr);
        if (rc != SQLITE_OK)
            die("SQL prepare failed: " + sqlite_error(db, rc));
        std::string value;
        rc = sqlite3_step(stmt);
        if (rc == SQLITE_ROW) {
            const unsigned char* text = sqlite3_column_text(stmt, 0);
            if (text) value = reinterpret_cast<const char*>(text);
        } else if (rc != SQLITE_DONE) {
            std::string error = sqlite_error(db, rc);
            sqlite3_finalize(stmt);
            die("SQL step failed: " + error);
        }
        sqlite3_finalize(stmt);
        return value;
    }

private:
    bool write_open = false;

    static bool rollback_journal_exists(const std::string& path) {
        std::error_code ec;
        return fs::exists(path + "-journal", ec);
    }

    static std::string sqlite_error(sqlite3* handle, int rc) {
        const char* msg = handle ? sqlite3_errmsg(handle) : sqlite3_errstr(rc);
        return msg ? std::string(msg) : ("SQLite error " + std::to_string(rc));
    }

    static bool exec_recovery(sqlite3* handle, const char* sql, std::string& error) {
        char* err = nullptr;
        int rc = sqlite3_exec(handle, sql, nullptr, nullptr, &err);
        if (rc == SQLITE_OK) return true;

        error = err ? std::string(err) : sqlite_error(handle, rc);
        if (err) sqlite3_free(err);
        return false;
    }

    static bool readonly_probe_ok(const std::string& path, std::string& error) {
        sqlite3* probe_db = nullptr;
        int rc = sqlite3_open_v2(path.c_str(), &probe_db, SQLITE_OPEN_READONLY, nullptr);
        if (rc != SQLITE_OK) {
            error = sqlite_error(probe_db, rc);
            if (probe_db) sqlite3_close(probe_db);
            return false;
        }

        bool ok = exec_recovery(probe_db, "PRAGMA busy_timeout=5000;", error)
            && exec_recovery(probe_db, "PRAGMA schema_version;", error);

        rc = sqlite3_close(probe_db);
        if (ok && rc != SQLITE_OK) {
            error = sqlite_error(probe_db, rc);
            ok = false;
        }

        return ok;
    }

    static bool needs_writable_recovery(const std::string& error) {
        std::string lower = error;
        std::transform(lower.begin(), lower.end(), lower.begin(),
                       [](unsigned char c){ return static_cast<char>(std::tolower(c)); });
        return lower.find("readonly") != std::string::npos
            || lower.find("read-only") != std::string::npos
            || lower.find("rollback") != std::string::npos;
    }

    static bool recover_rollback_journal(const std::string& path, std::string& error) {
        if (!rollback_journal_exists(path)) return true;

        std::string probe_error;
        if (readonly_probe_ok(path, probe_error)) return true;
        if (!needs_writable_recovery(probe_error)) {
            error = probe_error;
            return false;
        }

        sqlite3* recovery_db = nullptr;
        int rc = sqlite3_open_v2(path.c_str(), &recovery_db, SQLITE_OPEN_READWRITE, nullptr);
        if (rc != SQLITE_OK) {
            error = sqlite_error(recovery_db, rc);
            if (recovery_db) sqlite3_close(recovery_db);
            return false;
        }

        bool ok = exec_recovery(recovery_db, "PRAGMA busy_timeout=5000;", error)
            && exec_recovery(recovery_db, "PRAGMA schema_version;", error);

        rc = sqlite3_close(recovery_db);
        if (ok && rc != SQLITE_OK) {
            error = sqlite_error(recovery_db, rc);
            ok = false;
        }

        return ok;
    }
};

// Simple row type: vector of (column_name, value) pairs.
//
// `cols` holds the TEXT rendering of every column (sqlite3_column_text). For
// REAL/numeric columns we ALSO capture the native double (sqlite3_column_double)
// in `doubles`, because SQLite's text rendering of a double is only ~15
// significant digits — e.g. confidence 0.6499999761581421 renders as the lossy
// "0.649999976158142". get_double() prefers the native double so nlohmann emits
// the 16-digit shortest-round-trip form that the Python sibling / live produce.
struct Row {
    std::map<std::string, std::string> cols;
    std::map<std::string, double> doubles;   // only populated for REAL columns

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
        // Prefer the native double captured from sqlite3_column_double — full
        // precision, no text round-trip loss.
        auto dit = doubles.find(key);
        if (dit != doubles.end()) return dit->second;
        auto it = cols.find(key);
        if (it == cols.end() || it->second.empty()) return def;
        try { return std::stod(it->second); }
        catch (...) { return def; }
    }
};

using Rows = std::vector<Row>;

// A single bind parameter that carries its SQLite storage class. Most RI
// queries bind everything as TEXT (fine: SQLite applies the COLUMN's numeric
// affinity to coerce the TEXT operand for `col <op> ?` comparisons). BUT when
// the left side is a no-affinity expression — e.g. the scalar subquery
// `MAX(c2.last_touched)` in get_release_window_hotspots — SQLite applies NO
// conversion and compares storage classes directly. An INTEGER column value
// always sorts BEFORE a TEXT value, so `INTEGER >= '173...'` is ALWAYS false →
// the prior exe returned 0 rows while the Python sibling / live (which bind an
// int64) returned the real set (BUG 3). Binding such params as INTEGER fixes it.
struct Bind {
    enum class Kind { Text, Int } kind = Kind::Text;
    std::string text;
    int64_t i = 0;

    Bind(const std::string& s) : kind(Kind::Text), text(s) {}
    Bind(const char* s) : kind(Kind::Text), text(s ? s : "") {}
    static Bind Integer(int64_t v) { Bind b(""); b.kind = Kind::Int; b.i = v; return b; }
};

static Rows query_typed(Database& db, const std::string& sql, const std::vector<Bind>& params) {
    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(db.db, sql.c_str(), -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        std::string err = sqlite3_errmsg(db.db);
        std::cerr << "[query error] " << err << "\n  SQL: " << sql.substr(0, 200) << std::endl;
        return {};
    }
    for (int i = 0; i < (int)params.size(); ++i) {
        if (params[i].kind == Bind::Kind::Int)
            sqlite3_bind_int64(stmt, i + 1, params[i].i);
        else
            sqlite3_bind_text(stmt, i + 1, params[i].text.c_str(), -1, SQLITE_TRANSIENT);
    }
    Rows rows;
    int ncols = sqlite3_column_count(stmt);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        Row row;
        for (int c = 0; c < ncols; ++c) {
            const char* name = sqlite3_column_name(stmt, c);
            std::string key = name ? name : "";
            if (sqlite3_column_type(stmt, c) == SQLITE_FLOAT)
                row.doubles[key] = sqlite3_column_double(stmt, c);
            const char* val = (const char*)sqlite3_column_text(stmt, c);
            row.cols[key] = val ? val : "";
        }
        rows.push_back(std::move(row));
    }
    sqlite3_finalize(stmt);
    return rows;
}

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
            std::string key = name ? name : "";
            // Capture native double for REAL columns BEFORE column_text (which
            // mutates the column's type via SQLite's type-coercion rules). This
            // preserves shortest-round-trip precision for confidence/score/etc.
            if (sqlite3_column_type(stmt, c) == SQLITE_FLOAT)
                row.doubles[key] = sqlite3_column_double(stmt, c);
            const char* val = (const char*)sqlite3_column_text(stmt, c);
            row.cols[key] = val ? val : "";
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

static std::string sql_quote(std::string value) {
    for (size_t pos = 0; (pos = value.find('\'', pos)) != std::string::npos; pos += 2)
        value.replace(pos, 1, "''");
    return "'" + value + "'";
}

static std::string trim_copy(std::string value) {
    value.erase(value.begin(), std::find_if(value.begin(), value.end(),
        [](unsigned char ch) { return !std::isspace(ch); }));
    value.erase(std::find_if(value.rbegin(), value.rend(),
        [](unsigned char ch) { return !std::isspace(ch); }).base(), value.end());
    return value;
}

static std::string collapse_ws_copy(const std::string& value) {
    std::string out;
    bool pending_space = false;
    for (unsigned char ch : value) {
        if (std::isspace(ch)) {
            if (!out.empty()) pending_space = true;
            continue;
        }
        if (pending_space) {
            out.push_back(' ');
            pending_space = false;
        }
        out.push_back(static_cast<char>(ch));
    }
    return trim_copy(out);
}

// Query CLI help catalog, renderers, option arity, and DB-free validation.
#include "monolith_query_help.h"

static std::string duplicated_qualified_lookup_name(const std::string& name) {
    size_t pos = name.rfind("::");
    if (pos == std::string::npos) return name;
    std::string owner = name.substr(0, pos);
    return owner.empty() ? name : owner + "::" + name;
}

static std::string short_owner_duplicated_qualified_lookup_name(const std::string& name) {
    size_t pos = name.rfind("::");
    if (pos == std::string::npos) return name;
    std::string owner = name.substr(0, pos);
    std::string method = name.substr(pos + 2);
    size_t owner_pos = owner.rfind("::");
    std::string short_owner = owner_pos == std::string::npos ? owner : owner.substr(owner_pos + 2);
    return owner.empty() || short_owner.empty() ? name : owner + "::" + short_owner + "::" + method;
}

static bool is_ident_char(char ch) {
    return std::isalnum(static_cast<unsigned char>(ch)) || ch == '_';
}

static size_t find_matching_paren(const std::string& text, size_t open) {
    int depth = 0;
    for (size_t i = open; i < text.size(); ++i) {
        if (text[i] == '(') ++depth;
        else if (text[i] == ')') {
            --depth;
            if (depth == 0) return i;
        }
    }
    return std::string::npos;
}

static bool extract_params_at_paren(const std::string& signature, size_t open, std::string& out_params) {
    size_t close = find_matching_paren(signature, open);
    if (close == std::string::npos || close <= open) return false;
    out_params = signature.substr(open + 1, close - open - 1);
    return true;
}

static bool extract_signature_params(const std::string& signature,
                                     const std::string& function_name,
                                     std::string& out_params) {
    if (!function_name.empty()) {
        size_t search_from = 0;
        while (search_from < signature.size()) {
            size_t name_pos = signature.find(function_name, search_from);
            if (name_pos == std::string::npos) break;
            bool left_boundary = name_pos == 0 || !is_ident_char(signature[name_pos - 1]);
            size_t open = name_pos + function_name.size();
            while (open < signature.size() && std::isspace(static_cast<unsigned char>(signature[open]))) ++open;
            if (left_boundary && open < signature.size() && signature[open] == '('
                && extract_params_at_paren(signature, open, out_params)) {
                return true;
            }
            search_from = name_pos + std::max<size_t>(1, function_name.size());
        }
    }
    for (size_t i = 0; i < signature.size(); ++i) {
        if (signature[i] == '(' && extract_params_at_paren(signature, i, out_params)) return true;
    }
    return false;
}

static std::vector<std::string> split_top_level_params(const std::string& params) {
    std::vector<std::string> out;
    std::string cur;
    int angle = 0, paren = 0, bracket = 0;
    for (char ch : params) {
        if (ch == '<') ++angle;
        else if (ch == '>' && angle > 0) --angle;
        else if (ch == '(') ++paren;
        else if (ch == ')' && paren > 0) --paren;
        else if (ch == '[') ++bracket;
        else if (ch == ']' && bracket > 0) --bracket;
        if (ch == ',' && angle == 0 && paren == 0 && bracket == 0) {
            std::string part = trim_copy(cur);
            if (!part.empty()) out.push_back(part);
            cur.clear();
            continue;
        }
        cur.push_back(ch);
    }
    std::string tail = trim_copy(cur);
    if (!tail.empty()) out.push_back(tail);
    return out;
}

static std::string strip_default_param_value(const std::string& param) {
    int angle = 0, paren = 0, bracket = 0;
    for (size_t i = 0; i < param.size(); ++i) {
        char ch = param[i];
        if (ch == '<') ++angle;
        else if (ch == '>' && angle > 0) --angle;
        else if (ch == '(') ++paren;
        else if (ch == ')' && paren > 0) --paren;
        else if (ch == '[') ++bracket;
        else if (ch == ']' && bracket > 0) --bracket;
        else if (ch == '=' && angle == 0 && paren == 0 && bracket == 0) {
            return trim_copy(param.substr(0, i));
        }
    }
    return trim_copy(param);
}

static bool is_trailing_type_qualifier(const std::string& word) {
    std::string lower = lower_copy(word);
    return lower == "const" || lower == "volatile" || lower == "mutable"
        || lower == "final" || lower == "override";
}

static std::string strip_trailing_param_name(std::string param) {
    param = trim_copy(param);
    if (param.size() >= 3 && param.substr(param.size() - 3) == "...") return param;
    if (param.empty() || !is_ident_char(param.back())) return param;
    size_t end = param.size();
    size_t start = end;
    while (start > 0 && is_ident_char(param[start - 1])) --start;
    std::string word = param.substr(start, end - start);
    if (is_trailing_type_qualifier(word)) return param;
    std::string prefix = trim_copy(param.substr(0, start));
    return prefix.empty() ? param : prefix;
}

static std::string strip_leading_elaborated_type_keyword(std::string param) {
    param = trim_copy(param);
    std::string lower = lower_copy(param);
    for (const std::string& keyword : {"enum ", "class ", "struct "}) {
        if (lower.rfind(keyword, 0) == 0) return trim_copy(param.substr(keyword.size()));
    }
    return param;
}

static std::string normalize_override_param(std::string param) {
    param = collapse_ws_copy(strip_leading_elaborated_type_keyword(strip_trailing_param_name(strip_default_param_value(param))));
    for (const auto& repl : std::vector<std::pair<std::string, std::string>>{
        {" &", "&"}, {" *", "*"}, {" &&", "&&"}, {" ,", ","}, {", ", ","}}) {
        size_t pos = 0;
        while ((pos = param.find(repl.first, pos)) != std::string::npos) {
            param.replace(pos, repl.first.size(), repl.second);
            pos += repl.second.size();
        }
    }
    return trim_copy(param);
}

static bool normalize_override_params(const std::string& signature,
                                      const std::string& function_name,
                                      std::string& out_normalized) {
    std::string params;
    if (!extract_signature_params(signature, function_name, params)) return false;
    auto parts = split_top_level_params(params);
    if (parts.size() == 1 && lower_copy(trim_copy(parts[0])) == "void") parts.clear();
    for (auto& part : parts) part = normalize_override_param(part);
    std::ostringstream out;
    for (size_t i = 0; i < parts.size(); ++i) {
        if (i > 0) out << ",";
        out << parts[i];
    }
    out_normalized = out.str();
    return true;
}

static bool override_signatures_match(const std::string& child_signature,
                                      const std::string& parent_signature,
                                      const std::string& child_name,
                                      const std::string& parent_name,
                                      std::string& reason) {
    if (trim_copy(child_signature).empty() || trim_copy(parent_signature).empty()) {
        reason = "signature parameters unavailable but assuming compatible";
        return true;
    }
    std::string child_params, parent_params;
    if (!normalize_override_params(child_signature, child_name, child_params)
        || !normalize_override_params(parent_signature, parent_name, parent_params)) {
        reason = "signature parameters unavailable but assuming compatible";
        return true;
    }
    if (child_params != parent_params) {
        reason = "same name but different normalized parameter list";
        return false;
    }
    reason = child_params.empty()
        ? "same name and zero parameters"
        : "same name and matching normalized parameter list";
    return true;
}

static const char* override_confidence_for_reason(const std::string& reason) {
    return reason.find("assuming compatible") != std::string::npos ? "medium" : "high";
}

static bool is_numeric_string(const std::string& value) {
    if (value.empty()) return false;
    return std::all_of(value.begin(), value.end(),
        [](unsigned char ch) { return std::isdigit(ch); });
}

static bool sensitivity_prefix_allowed(const std::string& token) {
    return token == "save"
        || token == "serialize"
        || token == "archive"
        || token == "auth"
        || token == "login"
        || token == "account"
        || token == "session"
        || token == "purchase"
        || token == "store"
        || token == "entitlement"
        || token == "anticheat"
        || token == "crypto"
        || token == "crypt"
        || token == "encrypt"
        || token == "decrypt"
        || token == "signature"
        || token == "signed"
        || token == "signing"
        || token == "hash"
        || token == "exec"
        || token == "eval"
        || token == "command"
        || token == "file"
        || token == "registry"
        || token == "process"
        || token == "ufunction"
        || token == "server"
        || token == "client"
        || token == "netmulticast"
        || token == "onrep"
        || token == "replication"
        || token == "rpc"
        || token == "network";
}

static bool sensitivity_token_matches(const std::string& candidate,
                                      const std::string& sensitive_token) {
    if (candidate == sensitive_token) return true;
    return sensitivity_prefix_allowed(sensitive_token)
        && candidate.rfind(sensitive_token, 0) == 0;
}

static std::vector<std::string> sensitivity_candidate_tokens(const std::string& text) {
    std::vector<std::string> tokens;
    std::set<std::string> seen;
    auto add_token = [&](std::string token) {
        std::transform(token.begin(), token.end(), token.begin(),
            [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
        if (token.empty() || seen.count(token)) return;
        seen.insert(token);
        tokens.push_back(std::move(token));
    };

    std::string normalized = lower_copy(text);
    for (char& ch : normalized) {
        if (!std::isalnum(static_cast<unsigned char>(ch))) ch = ' ';
    }
    std::istringstream stream(normalized);
    std::string token;
    while (stream >> token) add_token(token);

    std::string current;
    unsigned char previous = 0;
    for (unsigned char ch : text) {
        if (!std::isalnum(ch)) {
            add_token(current);
            current.clear();
            previous = 0;
            continue;
        }
        if (!current.empty()
            && std::isupper(ch)
            && (std::islower(previous) || std::isdigit(previous))) {
            add_token(current);
            current.clear();
        }
        current.push_back(static_cast<char>(ch));
        previous = ch;
    }
    add_token(current);
    return tokens;
}

static bool contains_any_token(const std::string& text,
                               std::initializer_list<const char*> tokens,
                               std::string* matched_token = nullptr) {
    for (const std::string& candidate : sensitivity_candidate_tokens(text)) {
        for (const char* token : tokens) {
            std::string sensitive_token(token);
            if (sensitivity_token_matches(candidate, sensitive_token)) {
                if (matched_token) *matched_token = candidate;
                return true;
            }
        }
    }
    return false;
}

static std::string sensitivity_reason(const std::string& reason,
                                      const std::string& token) {
    return token.empty() ? reason : reason + " (token=" + token + ")";
}

static std::pair<double, std::string> sensitivity_factor(const std::string& text,
                                                         bool source_domain) {
    std::string matched;
    if (source_domain && contains_any_token(text, {"ufunction", "server", "client", "netmulticast", "onrep", "replication", "rpc", "network"}, &matched))
        return {0.15, sensitivity_reason("sensitivity: replication/RPC or network surface", matched)};
    if (!source_domain && contains_any_token(text, {"replication", "network", "rpc", "netmulticast", "onrep", "server", "client"}, &matched))
        return {0.15, sensitivity_reason("sensitivity: replication/RPC or network surface", matched)};
    if (contains_any_token(text, {"save", "serialize", "archive"}, &matched))
        return {0.15, sensitivity_reason("sensitivity: save/serialization surface", matched)};
    if (contains_any_token(text, {"auth", "login", "account", "session"}, &matched))
        return {0.15, sensitivity_reason("sensitivity: auth/account/session surface", matched)};
    if (contains_any_token(text, {"purchase", "iap", "store", "entitlement"}, &matched))
        return {0.15, sensitivity_reason("sensitivity: purchase/store entitlement surface", matched)};
    if (contains_any_token(text, {"anticheat", "anti_cheat", "cheat"}, &matched))
        return {0.15, sensitivity_reason("sensitivity: anticheat surface", matched)};
    if (contains_any_token(text, {"crypt", "crypto", "encrypt", "decrypt", "sign", "signature", "signed", "signing", "hash"}, &matched))
        return {0.15, sensitivity_reason("sensitivity: crypto/signing/hash surface", matched)};
    if (contains_any_token(text, {"exec", "eval", "command"}, &matched))
        return {0.15, sensitivity_reason("sensitivity: exec/eval/command surface", matched)};
    if (contains_any_token(text, {"file", "registry", "process"}, &matched))
        return {0.15, sensitivity_reason("sensitivity: file/registry/process surface", matched)};
    return {0.0, ""};
}

// CRG graph/cache, snapshot, and source review maintenance helpers.
#include "monolith_query_crg.h"

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
        if (v.empty()) return true;
        return parse_bool_literal(v, def);
    }
};

static Args parse_args(int argc, char* argv[]) {
    Args args;
    if (argc < 3) {
        print_top_level_help(std::cerr);
        throw QueryFatal("", 1, true);
    }

    args.ns = argv[1];
    args.action = argv[2];

    for (int i = 3; i < argc; ++i) {
        std::string a = argv[i];
        if (a.substr(0, 2) == "--") {
            std::string key, val;
            auto eq = a.find('=');
            bool had_inline_value = false;
            if (eq != std::string::npos) {
                key = a.substr(2, eq - 2);
                val = a.substr(eq + 1);
                had_inline_value = true;
            } else {
                key = a.substr(2);
                val = "";  // flag-style unless descriptor arity consumes a value below
            }
            // Normalize hyphens to underscores for consistency
            std::replace(key.begin(), key.end(), '-', '_');
            key = lower_copy(key);

            // Space-separated value form: `--key VALUE`. Only consume the
            // following token for descriptor-known value options. Boolean
            // options consume the following token only when it is an explicit
            // bool literal, so `--execute SomeAsset` remains a flag plus a
            // positional instead of eating the asset name.
            if (!had_inline_value && i + 1 < argc) {
                std::string nxt = argv[i + 1];
                CliOptionKind option_kind = cli_option_kind_for(key);
                if (option_kind == CliOptionKind::Value && nxt.substr(0, 2) != "--") {
                    val = nxt;
                    ++i;  // consume the value token
                } else if (option_kind == CliOptionKind::Bool && is_bool_literal(nxt)) {
                    val = lower_copy(nxt);
                    ++i;  // consume the explicit bool token
                }
            }
            args.options[key] = val;
        } else {
            args.positional.push_back(a);
        }
    }

    return args;
}

// Review-context changed-path and line-range helpers.
#include "monolith_query_review_ranges.h"

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

static bool looks_like_source_file_path(const std::string& value) {
    std::string trimmed = trim_copy(value);
    if (trimmed.empty()) return false;

    std::string lower = lower_copy(trimmed);
    if (lower.find('/') != std::string::npos || lower.find('\\') != std::string::npos)
        return true;

    std::string ext = lower_copy(fs::path(trimmed).extension().string());
    return ext == ".h" || ext == ".hpp" || ext == ".hh" ||
           ext == ".cpp" || ext == ".cc" || ext == ".cxx" ||
           ext == ".c" || ext == ".inl" || ext == ".cs" ||
           ext == ".usf" || ext == ".ush";
}

// Bridge asset/source symbol helpers and actions.
#include "monolith_query_bridge.h"

class SourceActions {
    Database db;
    std::string source_db_path;

public:
    void open(const std::string& path, bool query_only = true) {
        source_db_path = path;
        db.open(path, query_only);
    }

    std::string graph_db_path(const Args& args) const {
        std::string explicit_graph = args.opt("graph_db");
        if (!explicit_graph.empty()) return explicit_graph;
        fs::path source_path(source_db_path);
        fs::path dir = source_path.parent_path();
        if (dir.empty()) dir = ".";
        return (dir / "graph.db").string();
    }

    std::string get_file_path(int file_id) {
        auto rows = query(db, "SELECT path FROM files WHERE id = ?", {std::to_string(file_id)});
        return rows.empty() ? "<unknown>" : rows[0].get("path");
    }

    static bool known_source_path(const std::string& path) {
        return !path.empty() && path != "<unknown>";
    }

    static std::string source_path_status(const std::string& path) {
        return known_source_path(path) ? "known" : "missing";
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
                              "FROM symbols_fts f JOIN symbols s ON s.id = f.rowid "
                              "LEFT JOIN files p ON p.id = s.file_id";
            std::vector<std::string> conditions = {"symbols_fts MATCH ?"};
            std::vector<std::string> params = {fts_q};

            if (!module.empty()) {
                sql += " JOIN modules m ON m.id = p.module_id";
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
            sql += " ORDER BY CASE WHEN p.path IS NULL OR p.path = '' OR p.path = '<unknown>' THEN 1 ELSE 0 END, bm25(symbols_fts) LIMIT " + std::to_string(limit);

            auto rows = query(db, sql, params);
            if (!rows.empty()) {
                out << "=== Symbol Matches ===\n";
                for (auto& r : rows) {
                    std::string raw_fp = get_file_path(r.get_int("file_id"));
                    std::string fp = short_path(raw_fp);
                    out << "  [" << r.get("kind") << "] " << r.get("qualified_name")
                        << " (" << fp << ":" << r.get("line_start") << ")";
                    if (source_path_status(raw_fp) == "missing") out << " [path_status=missing]";
                    out << "\n";
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
        std::string path = args.opt("path", args.opt("file_path"));
        if (args.positional.empty()) {
            if (!path.empty()) {
                Args file_args = args;
                file_args.positional = {path};
                read_file(file_args);
                return;
            }
            die("read_source requires a symbol argument; use read_file or --path for source file paths");
        }
        std::string symbol = args.positional[0];
        if (looks_like_source_file_path(symbol)) {
            Args file_args = args;
            file_args.positional = {symbol};
            read_file(file_args);
            return;
        }
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
        // Coverage-miss hint (SPEC_MonolithToolCallReliabilityBacklog §5.2): a
        // well-formed symbol absent from EngineSource.db is usually an index
        // coverage gap, not a bad name. Steer toward search/reindex, not a blind
        // retry or an editor.run_python fallback.
        if (rows.empty()) die("No symbol found matching '" + symbol + "' in EngineSource.db. "
            "If it exists in the tree this is likely an index coverage gap, not a bad name; confirm with "
            "'source search_source', refresh via 'source.trigger_project_reindex' (live editor), then retry. "
            "Do not fall back to editor.run_python for source reads. [error_class=coverage_miss]");

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
        if (sym_rows.empty()) {
            std::cout << "No symbol found matching '" << symbol << "'. "
                      << "Run source search_source first to discover the indexed symbol name, then retry source find_references.\n"
                      << "match_status=no_symbol\n"
                      << "query=" << symbol << "\n"
                      << "count=0\n"
                      << "next_actions=source.search_source,source.search_crg_graph,source.review_context"
                      << std::endl;
            return;
        }

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
        std::string file_path = args.opt("file_path", args.opt("path"));
        if (file_path.empty() && !args.positional.empty()) file_path = args.positional[0];
        if (file_path.empty()) die("read_file requires a file_path argument");
        int start = args.opt_int("start", args.opt_int("start_line", 1));
        int end = args.opt_int("end", args.opt_int("end_line", 0));
        int line_count = args.opt_int("line_count", args.opt_int("max_lines", 0));

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

        // Coverage-miss hint (SPEC_MonolithToolCallReliabilityBacklog §5.2):
        // absolute on-disk paths resolve above, so a relative path that exists on
        // disk but misses here is an index coverage gap, not a bad path.
        if (resolved.empty()) die("No file found matching '" + file_path + "' in EngineSource.db. "
            "Absolute on-disk paths are read directly; a relative path that exists on disk but is missing here "
            "is likely an index coverage gap. Pass an absolute path, or refresh via 'source.trigger_project_reindex' "
            "(live editor), then retry. Do not fall back to editor.run_python for source reads. [error_class=coverage_miss]");

        if (end <= 0 && line_count > 0) end = start + line_count - 1;
        if (end <= 0) end = start + 199;

        std::cout << "--- " << short_path(resolved) << " (lines " << start << "-" << end << ") ---\n";
        std::cout << read_file_lines(resolved, start, end) << std::endl;
    }

    Rows symbols_by_name(const std::string& symbol, int limit = 5) {
        Rows rows;
        if (symbol.find("::") != std::string::npos) {
            std::string duplicated = duplicated_qualified_lookup_name(symbol);
            std::string short_owner_duplicated = short_owner_duplicated_qualified_lookup_name(symbol);
            rows = query(db,
                "SELECT s.id, s.name, s.qualified_name, s.kind, s.file_id, s.line_start, s.line_end, "
                "s.parent_symbol_id, s.access, s.signature, s.docstring, s.is_ue_macro "
                "FROM symbols s LEFT JOIN files f ON f.id = s.file_id "
                "WHERE (s.qualified_name = ? OR s.name = ? OR s.qualified_name = ? OR s.qualified_name = ?) "
                "ORDER BY CASE WHEN s.qualified_name = ? THEN 0 WHEN s.name = ? THEN 1 WHEN s.qualified_name = ? THEN 2 WHEN s.qualified_name = ? THEN 3 ELSE 4 END, "
                "         CASE WHEN f.path IS NULL OR f.path = '' OR f.path = '<unknown>' THEN 1 ELSE 0 END, "
                "         (s.line_end > s.line_start) DESC, s.id "
                "LIMIT " + std::to_string(limit),
                {symbol, symbol, duplicated, short_owner_duplicated, symbol, symbol, duplicated, short_owner_duplicated});
        } else {
            rows = query(db,
                "SELECT s.id, s.name, s.qualified_name, s.kind, s.file_id, s.line_start, s.line_end, "
                "s.parent_symbol_id, s.access, s.signature, s.docstring, s.is_ue_macro "
                "FROM symbols s LEFT JOIN files f ON f.id = s.file_id "
                "WHERE s.name = ? "
                "ORDER BY CASE WHEN f.path IS NULL OR f.path = '' OR f.path = '<unknown>' THEN 1 ELSE 0 END, "
                "         (s.line_end > s.line_start) DESC, s.id LIMIT " + std::to_string(limit),
                {symbol});
        }
        if (rows.empty()) {
            rows = query(db,
                "SELECT s.id, s.name, s.qualified_name, s.kind, s.file_id, s.line_start, s.line_end, "
                "s.parent_symbol_id, s.access, s.signature, s.docstring, s.is_ue_macro "
                "FROM symbols_fts fts JOIN symbols s ON s.id = fts.rowid LEFT JOIN files f ON f.id = s.file_id "
                "WHERE symbols_fts MATCH ? "
                "ORDER BY CASE WHEN f.path IS NULL OR f.path = '' OR f.path = '<unknown>' THEN 1 ELSE 0 END, "
                "         bm25(symbols_fts), s.id LIMIT " + std::to_string(limit),
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
        std::string file_path = get_file_path(r.get_int("file_id"));
        json o = {
            {"id", r.get_int64("id")},
            {"name", r.get("name")},
            {"qualified_name", r.get("qualified_name")},
            {"kind", r.get("kind")},
            {"file", short_path(file_path)},
            {"path_status", source_path_status(file_path)},
            {"line_start", r.get_int("line_start")},
            {"line_end", r.get_int("line_end")},
        };
        if (depth >= 0) o["depth"] = depth;
        std::string sig = r.get("signature");
        if (!sig.empty()) o["signature"] = sig;
        return o;
    }

    Rows filter_override_rows(const Rows& rows, int limit) {
        Rows out;
        for (auto row : rows) {
            std::string reason;
            if (!override_signatures_match(row.get("child_signature"), row.get("parent_signature"),
                                           row.get("child_name"), row.get("parent_name"), reason)) {
                continue;
            }
            row.cols["confidence"] = override_confidence_for_reason(reason);
            row.cols["reason"] = reason;
            out.push_back(std::move(row));
            if ((int)out.size() >= limit) break;
        }
        return out;
    }

    Rows override_edges_to(const std::string& symbol_id, int limit) {
        if (source_override_edge_cache_ready(db)) {
            return query(db, R"SQL(
SELECT child_fn.id AS child_id,base_fn.id AS parent_id,
       child_fn.name AS child_name,child_fn.qualified_name AS child_qualified_name,
       base_fn.name AS parent_name,base_fn.qualified_name AS parent_qualified_name,
       child_cls.name AS child_class_name,child_cls.qualified_name AS child_class_qualified_name,
       base_cls.name AS parent_class_name,base_cls.qualified_name AS parent_class_qualified_name,
       e.confidence AS confidence,
       e.reason AS reason
FROM source_override_edges e
JOIN symbols child_fn ON child_fn.id = e.child_symbol_id
JOIN symbols base_fn ON base_fn.id = e.parent_symbol_id
JOIN symbols child_cls ON child_cls.id = child_fn.parent_symbol_id
JOIN symbols base_cls ON base_cls.id = base_fn.parent_symbol_id
WHERE e.parent_symbol_id = ?
ORDER BY child_fn.qualified_name
LIMIT ?
)SQL", {symbol_id, std::to_string(clamp_int(limit, 1, 1000))});
        }
        int sql_limit = clamp_int(limit * 4, limit, 1000);
        return filter_override_rows(query(db, R"SQL(
WITH RECURSIVE descendants(base_class_id, child_class_id) AS (
  SELECT parent_id, child_id FROM inheritance
  UNION
  SELECT d.base_class_id, i.child_id
  FROM descendants d
  JOIN inheritance AS i ON i.parent_id = d.child_class_id
)
SELECT child_fn.id AS child_id,base_fn.id AS parent_id,
       child_fn.name AS child_name,child_fn.qualified_name AS child_qualified_name,
       base_fn.name AS parent_name,base_fn.qualified_name AS parent_qualified_name,
       child_cls.name AS child_class_name,child_cls.qualified_name AS child_class_qualified_name,
       base_cls.name AS parent_class_name,base_cls.qualified_name AS parent_class_qualified_name,
       COALESCE(child_fn.signature,'') AS child_signature,
       COALESCE(base_fn.signature,'') AS parent_signature
FROM symbols base_fn
JOIN symbols base_cls ON base_cls.id = base_fn.parent_symbol_id
JOIN descendants AS d ON d.base_class_id = base_cls.id
JOIN symbols child_cls ON child_cls.id = d.child_class_id
JOIN symbols child_fn ON child_fn.parent_symbol_id = child_cls.id
    AND (base_fn.name = child_fn.name
      OR base_fn.name = base_cls.name || '::' || child_fn.name)
    AND child_fn.kind = base_fn.kind
    AND child_fn.id != base_fn.id
WHERE base_fn.id = ? AND base_fn.kind = 'function'
ORDER BY child_fn.qualified_name
LIMIT ?
)SQL", {symbol_id, std::to_string(sql_limit)}), limit);
    }

    Rows override_edges_from(const std::string& symbol_id, int limit) {
        if (source_override_edge_cache_ready(db)) {
            return query(db, R"SQL(
SELECT child_fn.id AS child_id,base_fn.id AS parent_id,
       child_fn.name AS child_name,child_fn.qualified_name AS child_qualified_name,
       base_fn.name AS parent_name,base_fn.qualified_name AS parent_qualified_name,
       child_cls.name AS child_class_name,child_cls.qualified_name AS child_class_qualified_name,
       base_cls.name AS parent_class_name,base_cls.qualified_name AS parent_class_qualified_name,
       e.confidence AS confidence,
       e.reason AS reason
FROM source_override_edges e
JOIN symbols child_fn ON child_fn.id = e.child_symbol_id
JOIN symbols base_fn ON base_fn.id = e.parent_symbol_id
JOIN symbols child_cls ON child_cls.id = child_fn.parent_symbol_id
JOIN symbols base_cls ON base_cls.id = base_fn.parent_symbol_id
WHERE e.child_symbol_id = ?
ORDER BY base_fn.qualified_name
LIMIT ?
)SQL", {symbol_id, std::to_string(clamp_int(limit, 1, 1000))});
        }
        int sql_limit = clamp_int(limit * 4, limit, 1000);
        return filter_override_rows(query(db, R"SQL(
WITH RECURSIVE ancestors(child_class_id, ancestor_class_id) AS (
  SELECT child_id, parent_id FROM inheritance
  UNION
  SELECT a.child_class_id, i.parent_id
  FROM ancestors a
  JOIN inheritance AS i ON i.child_id = a.ancestor_class_id
)
SELECT child_fn.id AS child_id,base_fn.id AS parent_id,
       child_fn.name AS child_name,child_fn.qualified_name AS child_qualified_name,
       base_fn.name AS parent_name,base_fn.qualified_name AS parent_qualified_name,
       child_cls.name AS child_class_name,child_cls.qualified_name AS child_class_qualified_name,
       base_cls.name AS parent_class_name,base_cls.qualified_name AS parent_class_qualified_name,
       COALESCE(child_fn.signature,'') AS child_signature,
       COALESCE(base_fn.signature,'') AS parent_signature
FROM symbols child_fn
JOIN symbols child_cls ON child_cls.id = child_fn.parent_symbol_id
JOIN ancestors AS a ON a.child_class_id = child_cls.id
JOIN symbols base_cls ON base_cls.id = a.ancestor_class_id
JOIN symbols base_fn INDEXED BY idx_symbols_parent_name_kind ON base_fn.parent_symbol_id = base_cls.id
    AND (base_fn.name = child_fn.name
      OR base_fn.name = base_cls.name || '::' || child_fn.name)
    AND base_fn.kind = child_fn.kind
WHERE child_fn.id = ? AND child_fn.kind = 'function'
ORDER BY base_fn.qualified_name
LIMIT ?
)SQL", {symbol_id, std::to_string(sql_limit)}), limit);
    }

    json override_edge_json(const Row& r, int depth = -1) {
        json o = {
            {"from", r.get_int64("child_id")},
            {"to", r.get_int64("parent_id")},
            {"kind", "override"},
            {"confidence", r.get("confidence", "high")},
            {"reason", r.get("reason")},
            {"child", {
                {"id", r.get_int64("child_id")},
                {"name", r.get("child_name")},
                {"qualified_name", r.get("child_qualified_name")},
                {"class", r.get("child_class_qualified_name")},
            }},
            {"parent", {
                {"id", r.get_int64("parent_id")},
                {"name", r.get("parent_name")},
                {"qualified_name", r.get("parent_qualified_name")},
                {"class", r.get("parent_class_qualified_name")},
            }},
        };
        if (depth >= 0) o["depth"] = depth;
        return o;
    }

    json source_health_json(bool include_counts, bool include_deep_checks) {
        bool run_expensive_checks = include_counts || include_deep_checks;
        bool needs_reindex = false;
        bool needs_fts_repair = false;
        bool needs_crg_repair = false;
        bool needs_override_repair = false;
        json root = {
            {"input", {{"include_counts", include_counts}, {"include_deep_checks", include_deep_checks}}},
            {"limits", {{"include_counts", include_counts}, {"include_deep_checks", include_deep_checks}}},
            {"checks", json::array()},
            {"warnings", json::array()},
            {"truncated", false},
        };
        auto check = [&](const std::string& name, bool pass, const std::string& detail) {
            root["checks"].push_back({{"check", name}, {"result", pass ? "ok" : "warning"}, {"detail", detail}});
            if (!pass) root["warnings"].push_back(detail);
        };
        auto info = [&](const std::string& name, const std::string& detail) {
            root["checks"].push_back({{"check", name}, {"result", "info"}, {"detail", detail}});
        };

        for (const char* t : {"modules", "files", "symbols", "inheritance", "references", "includes", "meta"}) {
            bool has = object_exists(db, "table", t);
            if (!has) needs_reindex = true;
            check(std::string("table:") + t, has, has ? std::string("table ") + t + " present" : std::string("missing table ") + t);
        }
        for (const char* f : {"symbols_fts", "source_fts"}) {
            bool has = object_exists(db, "table", f);
            if (!has) {
                if (std::string(f) == "symbols_fts") needs_fts_repair = true;
                else needs_reindex = true;
            }
            check(std::string("fts:") + f, has, has ? std::string("FTS table ") + f + " present" : std::string("missing FTS table ") + f);
        }
        for (const char* tr : {"symbols_ai", "symbols_ad"}) {
            bool has = object_exists(db, "trigger", tr);
            if (!has) needs_fts_repair = true;
            check(std::string("trigger:") + tr, has, has ? std::string("trigger ") + tr + " present" : std::string("missing trigger ") + tr + " (symbols_fts may drift)");
        }

        std::string schema_ver = scalar_str(db, "SELECT value FROM meta WHERE key = 'schema_version';");
        if (schema_ver != "1") needs_reindex = true;
        check("meta:schema_version", schema_ver == "1", schema_ver.empty() ? "meta.schema_version missing" : "schema_version=" + schema_ver + " (expected 1)");

        int64_t sym_cnt = -1;
        int64_t source_fts_cnt = -1;
        if (run_expensive_checks) {
            int64_t orphan_symbols = count_rows(db,
                "SELECT COUNT(*) FROM symbols s "
                "LEFT JOIN files f ON f.id = s.file_id "
                "WHERE f.id IS NULL;");
            if (orphan_symbols != 0) needs_reindex = true;
            check("integrity:orphan_symbols", orphan_symbols == 0,
                  orphan_symbols == 0 ? "no orphan symbol rows" : std::to_string(orphan_symbols) + " orphan symbol row(s); run project/source reindex so prune can remove invalid dependent rows");

            int64_t orphan_refs = count_rows(db,
                "SELECT COUNT(*) FROM \"references\" r "
                "WHERE (r.from_symbol_id != 0 AND r.from_symbol_id NOT IN (SELECT id FROM symbols)) "
                "OR r.to_symbol_id NOT IN (SELECT id FROM symbols);");
            if (orphan_refs != 0) needs_reindex = true;
            check("integrity:orphan_references", orphan_refs == 0,
                  orphan_refs == 0 ? "no orphan reference rows" : std::to_string(orphan_refs) + " orphan reference row(s); rebuild source index rather than repeatedly repairing CRG cache");

            sym_cnt = count_rows(db, "SELECT COUNT(*) FROM symbols;");
            int64_t sym_fts_cnt = count_rows(db, "SELECT COUNT(*) FROM symbols_fts;");
            if (sym_cnt != sym_fts_cnt) needs_fts_repair = true;
            check("fts:symbols_row_parity", sym_cnt == sym_fts_cnt,
                "symbols=" + std::to_string(sym_cnt) + " symbols_fts=" + std::to_string(sym_fts_cnt) + (sym_cnt == sym_fts_cnt ? "" : " (mismatch -> source.repair_fts target=symbols)"));
            source_fts_cnt = count_rows(db, "SELECT COUNT(*) FROM source_fts;");
            info("fts:source_fts_info", "source_fts rows=" + std::to_string(source_fts_cnt) + " (plain fts5; not rebuildable; reindex to repair)");
        } else {
            info("integrity:orphan_symbols", "skipped; pass --include-deep-checks=true or --include-counts=true");
            info("integrity:orphan_references", "skipped; pass --include-deep-checks=true or --include-counts=true");
            info("fts:symbols_row_parity", "skipped; pass --include-deep-checks=true or --include-counts=true");
            info("fts:source_fts_info", "source_fts row count skipped; pass --include-deep-checks=true or --include-counts=true");
        }
        std::string journal_mode = scalar_str(db, "PRAGMA journal_mode;");
        root["schema"] = {{"schema_version", schema_ver}, {"journal_mode", journal_mode}};
        if (lower_copy(journal_mode) == "wal") {
            auto file_size = [](const std::string& path) -> uintmax_t {
                std::error_code ec;
                if (!fs::exists(path, ec)) return 0;
                uintmax_t size = fs::file_size(path, ec);
                return ec ? 0 : size;
            };
            json wal = {
                {"wal_bytes", file_size(source_db_path + "-wal")},
                {"shm_bytes", file_size(source_db_path + "-shm")},
            };
            sqlite3_stmt* stmt = nullptr;
            int rc = sqlite3_prepare_v2(db.db, "PRAGMA wal_checkpoint(NOOP);", -1, &stmt, nullptr);
            if (rc == SQLITE_OK) {
                rc = sqlite3_step(stmt);
                if (rc == SQLITE_ROW) {
                    wal["checkpoint_busy"] = sqlite3_column_int64(stmt, 0);
                    wal["log_frames"] = sqlite3_column_int64(stmt, 1);
                    wal["checkpointed_frames"] = sqlite3_column_int64(stmt, 2);
                }
            } else {
                wal["checkpoint_error"] = sqlite3_errmsg(db.db);
            }
            if (stmt) sqlite3_finalize(stmt);
            root["wal"] = wal;
        }
        if (include_counts) {
            root["row_counts"] = {
                {"symbols", sym_cnt},
                {"references", count_rows(db, "SELECT COUNT(*) FROM \"references\";")},
                {"inheritance", count_rows(db, "SELECT COUNT(*) FROM inheritance;")},
                {"source_fts", source_fts_cnt},
            };
        }
        const size_t warnings_before_crg = root["warnings"].size();
        if (run_expensive_checks) {
            // RX-2: CRG projection-cache health parity (mirrors editor ComputeHealth).
            // valid edges use the symbols-joined ref count so parity matches the
            // editor's dangling-ref-corrected rebuild.
            int64_t valid_refs = count_rows(db,
                "SELECT COUNT(*) FROM \"references\" r "
                "JOIN symbols fs ON fs.id = r.from_symbol_id "
                "JOIN symbols ts ON ts.id = r.to_symbol_id;");
            int64_t inh_cnt = count_rows(db, "SELECT COUNT(*) FROM inheritance;");
            append_crg_health_checks(db, "source", sym_cnt, valid_refs + inh_cnt, root);
        } else {
            bool has_core_crg = object_exists(db, "table", "crg_nodes")
                && object_exists(db, "table", "crg_edges")
                && object_exists(db, "table", "crg_node_metrics")
                && object_exists(db, "table", "crg_meta");
            if (!has_core_crg) needs_crg_repair = true;
            check("crg:tables_core", has_core_crg,
                  has_core_crg ? "core CRG projection tables present" : "one or more core CRG projection tables are missing");
            bool has_override_edges = object_exists(db, "table", "source_override_edges");
            if (!has_override_edges) needs_override_repair = true;
            check("crg:table:source_override_edges", has_override_edges,
                  has_override_edges ? "source_override_edges table present" : "source_override_edges table missing");
            info("crg:row_parity", "skipped; pass --include-deep-checks=true or --include-counts=true");
        }
        if (root["warnings"].size() > warnings_before_crg) {
            needs_crg_repair = true;
        }
        if (run_expensive_checks && !source_override_edge_cache_ready(db)) {
            needs_override_repair = true;
        }
        root["status"] = root["warnings"].empty() ? "ok" : "warning";
        root["check_depth"] = run_expensive_checks ? "deep" : "shallow";
        root["summary"] = root["warnings"].empty()
            ? (run_expensive_checks
                ? "EngineSource schema, triggers, symbols_fts parity, CRG cache and integrity OK"
                : "EngineSource schema, required tables/triggers, and CRG structure OK; deep parity checks skipped")
            : std::to_string(root["warnings"].size()) + " health warning(s)";
        auto next = json::array();
        auto add_next_unique = [&](const std::string& action) {
            for (const auto& existing : next) {
                if (existing.is_string() && existing.get<std::string>() == action) return;
            }
            next.push_back(action);
        };
        if (!run_expensive_checks) add_next_unique("source.health --include-deep-checks=true");
        if (needs_crg_repair) add_next_unique("source.repair_crg_cache");
        if (needs_override_repair) add_next_unique("source.repair_crg_cache --scope=override_edges");
        if (needs_fts_repair) add_next_unique("source.repair_fts --target=symbols");
        if (needs_reindex) {
            add_next_unique("source.trigger_project_reindex");
            add_next_unique("source.trigger_reindex");
        }
        if (root["warnings"].empty()) {
            add_next_unique("source.search_source");
            add_next_unique("source.review_context");
            add_next_unique("source.risk_score");
        } else {
            add_next_unique("source.health");
            add_next_unique("source.search_source");
        }
        root["next_actions"] = next;
        return root;
    }

    void health(const Args& args) {
        print_json(source_health_json(
            args.opt_bool("include_counts", false),
            args.opt_bool("include_deep_checks", false)));
    }

    void trigger_reindex(const Args&) {
        json root = {
            {"status", "live_only"},
            {"offline_supported", false},
            {"action", "source.trigger_reindex"},
            {"summary", "source.trigger_reindex requires a running Unreal Editor/Monolith MCP server; monolith_query.exe is an offline SQLite reader and cannot launch source indexing."},
            {"next_actions", json::array({"source.health", "source.search_source", "Use live MCP source.trigger_reindex when editor-backed reindexing is required"})},
        };
        print_json(root);
    }

    void trigger_project_reindex(const Args&) {
        json root = {
            {"status", "live_only"},
            {"offline_supported", false},
            {"action", "source.trigger_project_reindex"},
            {"summary", "source.trigger_project_reindex requires a running Unreal Editor/Monolith MCP server; monolith_query.exe is an offline SQLite reader and cannot update EngineSource.db."},
            {"next_actions", json::array({"source.health", "source.search_source", "Use live MCP source.trigger_project_reindex after a C++ build or hot reload"})},
        };
        print_json(root);
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

    json build_crg_graph_json(const Args& args) {
        const std::string graph_path = graph_db_path(args);
        const bool execute = args.opt_bool("execute", false);
        const bool force = args.opt_bool("force", false);
        // Cooldown window for the graph.db export rebuild. Default 1800s (30 min):
        // collapses a chained/bursty repair_crg_cache + build_crg_graph loop into
        // at most one graph rebuild per window unless --force. 0 disables the gate.
        const int cooldown_seconds = args.opt_int("cooldown_seconds", 1800);
        const json source_counts = source_graph_build_counts(db);
        json root = {
            {"input", {{"execute", execute}, {"force", force}, {"cooldown_seconds", cooldown_seconds}, {"source_db", source_db_path}, {"graph_db", graph_path}}},
            {"limits", {{"execute", execute}, {"force", force}, {"cooldown_seconds", cooldown_seconds}}},
            {"graph_db", graph_path},
            {"source_db", source_db_path},
            {"build_mode", "atomic_temp_replace"},
            {"plan", json::array({
                "BUILD CRG-compatible nodes/edges/metadata/FTS schema into a same-directory temp graph DB",
                "REPLACE graph nodes from EngineSource files + symbols",
                "REPLACE graph edges from file containment, references and inheritance",
                "REBUILD nodes_fts from nodes",
                "VALIDATE temp graph counts, FTS parity, schema_version=9 and source signature metadata",
                "Atomically replace Saved/graph.db only after validation"
            })},
            {"source_counts", source_counts},
            {"warnings", json::array()},
            {"truncated", false},
        };

        if (!execute) {
            if (fs::exists(graph_path)) {
                Database graph;
                std::string open_error;
                if (!try_open_readonly_database(graph, graph_path, open_error)) {
                    json busy = graph_busy_json("source.build_crg_graph", graph_path, open_error);
                    busy["input"] = root["input"];
                    busy["source_db"] = source_db_path;
                    busy["source_counts"] = source_counts;
                    busy["build_mode"] = root["build_mode"];
                    busy["summary"] = "Dry-run: CRG graph database is busy; no writes were attempted";
                    return busy;
                }
                root["before"] = crg_graph_counts(graph);
            } else {
                root["before"] = json::object();
                root["warnings"].push_back("graph database does not exist yet");
            }
            root["after"] = json::object();
            root["status"] = "ok";
            root["summary"] = "Dry-run: Saved/graph.db would be rebuilt from EngineSource.db. Pass --execute to apply.";
            root["next_actions"] = json::array({"source.build_crg_graph --execute", "source.crg_graph_health", "source.search_source"});
            return root;
        }

        const std::string lock_path = graph_path + ".rebuild.lock";
        root["lock_path"] = lock_path;
        GraphRebuildLock rebuild_lock(lock_path);
        json busy;
        if (!rebuild_lock.acquire(busy)) {
            busy["input"] = root["input"];
            busy["graph_db"] = graph_path;
            busy["source_db"] = source_db_path;
            busy["build_mode"] = root["build_mode"];
            return busy;
        }

        json existing_metadata = json::object();
        if (fs::exists(graph_path)) {
            Database existing_graph;
            std::string open_error;
            if (!try_open_readonly_database(existing_graph, graph_path, open_error)) {
                json busy_graph = graph_busy_json("source.build_crg_graph", graph_path, open_error);
                busy_graph["input"] = root["input"];
                busy_graph["source_db"] = source_db_path;
                busy_graph["lock_path"] = lock_path;
                busy_graph["build_mode"] = root["build_mode"];
                return busy_graph;
            }
            root["before"] = crg_graph_counts(existing_graph);
            if (!force && graph_metadata_matches_source(existing_graph, source_counts, existing_metadata)) {
                root["after"] = root["before"];
                root["metadata"] = existing_metadata;
                root["status"] = "ok";
                root["summary"] = "Saved/graph.db is already current for the EngineSource source signature";
                root["skipped"] = true;
                root["replaced"] = false;
                root["skip_reason"] = "parity_fresh";
                root["next_actions"] = json::array({"source.search_crg_graph", "source.crg_graph_health", "source.review_context"});
                return root;
            }
            root["metadata_before"] = existing_metadata;

            // Cooldown gate (CLAUDE.md §12; SPEC_MonolithToolCallReliabilityBacklog
            // §5.1). graph.db is an export/search cache, NOT the source of truth for
            // risk_score/review_context, so when an immediately preceding
            // repair_crg_cache churns the EngineSource signature we must not rebuild
            // it more than once per cooldown window unless --force. This caps the
            // daily repair+build maintenance-loop wall-time without masking a real
            // change: the first build after the window still rebuilds, and the
            // existing signature check above still fast-skips an unchanged source.
            if (!force && cooldown_seconds > 0) {
                const std::string last_epoch_str =
                    scalar_str(existing_graph, "SELECT value FROM metadata WHERE key = 'last_build_epoch';");
                long long last_epoch = 0;
                if (!last_epoch_str.empty()) {
                    try { last_epoch = std::stoll(last_epoch_str); } catch (...) { last_epoch = 0; }
                }
                const long long now_epoch = static_cast<long long>(std::time(nullptr));
                const long long age = now_epoch - last_epoch;
                if (last_epoch > 0 && age >= 0 && age < static_cast<long long>(cooldown_seconds)) {
                    root["after"] = root["before"];
                    root["metadata"] = existing_metadata;
                    root["status"] = "ok";
                    root["skipped"] = true;
                    root["replaced"] = false;
                    root["skip_reason"] = "cooldown";
                    root["seconds_since_last_build"] = age;
                    root["summary"] = "Saved/graph.db rebuild skipped: last build was "
                        + std::to_string(age) + "s ago, within the " + std::to_string(cooldown_seconds)
                        + "s cooldown (EngineSource signature likely churned via repair_crg_cache). Pass --force to rebuild now.";
                    root["next_actions"] = json::array({"source.search_crg_graph", "source.crg_graph_health", "source.review_context"});
                    return root;
                }
            }
        } else {
            root["before"] = json::object();
            root["warnings"].push_back("graph database does not exist yet");
        }

        bool ok = true;
        std::string error;
        Database graph;
        const std::string temp_path = graph_path + ".rebuild." + std::to_string(current_process_id()) + ".tmp";
        root["temp_graph_db"] = temp_path;
        root["replaced"] = false;
        root["skipped"] = false;

        remove_file_best_effort(temp_path);
        remove_sqlite_sidecars_best_effort(temp_path);

        auto fail = [&](const std::string& label, const std::string& err) {
            ok = false;
            root["warnings"].push_back("CRG graph rebuild failed at " + label + (err.empty() ? "" : ": " + err));
        };

        graph.open_or_create(temp_path);
        if (!exec_sql_ok(graph, "PRAGMA foreign_keys=OFF;", error)) fail("pragma foreign_keys", error);
        if (ok && !exec_sql_ok(graph, "ATTACH DATABASE " + sql_quote(source_db_path) + " AS src;", error)) fail("attach source", error);
        if (ok && !exec_sql_ok(graph, "DROP TABLE IF EXISTS nodes_fts;", error)) fail("drop stale nodes_fts", error);
        if (ok && !ensure_crg_graph_base_tables(graph, error)) fail("create base schema", error);
        if (ok && !exec_sql_ok(graph, "BEGIN;", error)) fail("begin transaction", error);
        if (ok) {
            for (const auto& step : crg_graph_build_sql(source_db_path, source_counts)) {
                if (!exec_sql_ok(graph, step.second, error)) {
                    fail(step.first, error);
                    break;
                }
            }
        }
        if (ok) {
            // Stamp the build time so the cooldown gate above can short-circuit
            // rapid repeat rebuilds (SPEC_MonolithToolCallReliabilityBacklog §5.1).
            const std::string now_epoch = std::to_string(static_cast<long long>(std::time(nullptr)));
            if (!exec_sql_ok(graph,
                    "INSERT OR REPLACE INTO metadata(key,value) VALUES('last_build_epoch'," + sql_quote(now_epoch) + ");",
                    error)) {
                fail("metadata last_build_epoch", error);
            }
        }
        if (ok && !ensure_crg_graph_indexes(graph, error)) fail("create indexes", error);
        if (ok) {
            if (!exec_sql_ok(graph, "COMMIT;", error)) {
                fail("commit", error);
                std::string rollback_error;
                exec_sql_ok(graph, "ROLLBACK;", rollback_error);
            }
        } else {
            std::string rollback_error;
            exec_sql_ok(graph, "ROLLBACK;", rollback_error);
        }
        std::string detach_error;
        exec_sql_ok(graph, "DETACH DATABASE src;", detach_error);

        json temp_counts = ok ? crg_graph_counts(graph) : json::object();
        const std::string schema_version = ok ? scalar_str(graph, "SELECT value FROM metadata WHERE key = 'schema_version';") : "";
        json validation = {
            {"nodes_gt_zero", ok && temp_counts.value("nodes", static_cast<int64_t>(-1)) > 0},
            {"edges_gt_zero", ok && temp_counts.value("edges", static_cast<int64_t>(-1)) > 0},
            {"fts_row_parity", ok && temp_counts.value("nodes", static_cast<int64_t>(-1)) == temp_counts.value("fts_rows", static_cast<int64_t>(-2))},
            {"schema_version", schema_version},
            {"schema_version_ok", ok && schema_version == "9"},
        };
        validation["passed"] = validation["nodes_gt_zero"].get<bool>()
            && validation["edges_gt_zero"].get<bool>()
            && validation["fts_row_parity"].get<bool>()
            && validation["schema_version_ok"].get<bool>();
        root["validation"] = validation;
        root["after"] = temp_counts;

        if (ok && !validation["passed"].get<bool>()) {
            fail("validate temp graph", validation.dump());
        }

        graph.close();
        if (ok) {
            std::string replace_error;
            if (!atomic_replace_file(temp_path, graph_path, replace_error)) {
                fail("atomic replace", replace_error);
            } else {
                root["replaced"] = true;
            }
        }

        if (!ok) {
            remove_file_best_effort(temp_path);
            remove_sqlite_sidecars_best_effort(temp_path);
        }

        root["status"] = ok ? "ok" : "error";
        root["summary"] = ok
            ? "Rebuilt Saved/graph.db atomically from EngineSource files, symbols, references and inheritance"
            : "Saved/graph.db atomic rebuild failed; existing graph database was left untouched";
        root["next_actions"] = ok
            ? json::array({"source.search_crg_graph", "source.crg_graph_health", "source.review_context"})
            : json::array({"source.crg_graph_health", "source.health", "source.search_source"});
        return root;
    }

    void build_crg_graph(const Args& args) {
        print_json(build_crg_graph_json(args));
    }

    void rebuild_crg_graph(const Args& args) {
        print_json(build_crg_graph_json(args));
    }

    void crg_graph_health(const Args& args) {
        print_json(crg_graph_health_json(graph_db_path(args)));
    }

    void search_crg_graph(const Args& args) {
        if (args.positional.empty()) die("search_crg_graph requires a query argument");
        std::string q = args.positional[0];
        int cap = clamp_int(args.opt_int("limit", 20), 1, 200);
        std::string kind = args.opt("kind");
        std::string graph_path = graph_db_path(args);

        json root = {
            {"input", {{"query", q}, {"kind", kind}, {"graph_db", graph_path}}},
            {"limits", {{"limit", cap}}},
            {"graph_db", graph_path},
            {"results", json::array()},
            {"warnings", json::array()},
            {"truncated", false},
        };

        if (!fs::exists(graph_path)) {
            root["status"] = "warning";
            root["summary"] = "CRG graph database is missing";
            root["warnings"].push_back("Saved/graph.db is missing; run source.build_crg_graph --execute");
            root["next_actions"] = json::array({"source.build_crg_graph --execute"});
            print_json(root);
            return;
        }

        Database graph;
        std::string open_error;
        if (!try_open_readonly_database(graph, graph_path, open_error)) {
            json busy = graph_busy_json("source.search_crg_graph", graph_path, open_error);
            busy["input"] = root["input"];
            busy["limits"] = root["limits"];
            busy["results"] = json::array();
            busy["count"] = 0;
            if (!looks_like_graph_busy_error(open_error)) busy["status"] = "warning";
            print_json(busy);
            return;
        }
        bool used_fts = false;
        Rows rows;
        if (object_exists(graph, "table", "nodes_fts")) {
            std::string sql =
                "SELECT n.id,n.kind,n.name,n.qualified_name,n.file_path,n.line_start,n.line_end,"
                "n.language,n.signature,bm25(nodes_fts) AS rank "
                "FROM nodes_fts f JOIN nodes n ON n.id = f.rowid "
                "WHERE nodes_fts MATCH ?";
            std::vector<std::string> params = {escape_fts(q)};
            if (!kind.empty()) {
                sql += " AND lower(n.kind) = lower(?)";
                params.push_back(kind);
            }
            sql += " ORDER BY CASE WHEN n.file_path IS NULL OR n.file_path = '' OR n.file_path = '<unknown>' THEN 1 ELSE 0 END, rank LIMIT " + std::to_string(cap + 1);
            rows = query(graph, sql, params);
            used_fts = !rows.empty();
        }

        if (rows.empty()) {
            std::string sql =
                "SELECT id,kind,name,qualified_name,file_path,line_start,line_end,language,signature,0 AS rank "
                "FROM nodes WHERE (lower(name) LIKE ? OR lower(qualified_name) LIKE ? OR lower(file_path) LIKE ?)";
            std::vector<std::string> params = {"%" + lower_copy(q) + "%", "%" + lower_copy(q) + "%", "%" + lower_copy(q) + "%"};
            if (!kind.empty()) {
                sql += " AND lower(kind) = lower(?)";
                params.push_back(kind);
            }
            sql += " ORDER BY CASE WHEN file_path IS NULL OR file_path = '' OR file_path = '<unknown>' THEN 1 ELSE 0 END, CASE WHEN lower(name)=lower(?) THEN 0 WHEN lower(name) LIKE lower(?) THEN 1 ELSE 2 END, name LIMIT " + std::to_string(cap + 1);
            params.push_back(q);
            params.push_back(q + "%");
            rows = query(graph, sql, params);
        }

        bool truncated = (int)rows.size() > cap;
        if (truncated) rows.pop_back();

        json results = json::array();
        std::set<std::string> seen;
        for (const auto& row : rows) {
            double rank = row.get_double("rank", 0.0);
            std::string file_path = row.get("file_path");
            std::string key = row.get("qualified_name") + "|" + row.get("kind") + "|" + file_path;
            if (!seen.insert(key).second) continue;
            results.push_back({
                {"id", row.get_int64("id")},
                {"kind", row.get("kind")},
                {"name", row.get("name")},
                {"qualified_name", row.get("qualified_name")},
                {"file_path", short_path(file_path)},
                {"path_status", source_path_status(file_path)},
                {"line_start", row.get_int("line_start")},
                {"line_end", row.get_int("line_end")},
                {"language", row.get("language")},
                {"signature", row.get("signature")},
                {"score", used_fts ? std::round((-rank) * 1000000.0) / 1000000.0 : 0.0},
            });
        }

        root["status"] = "ok";
        root["summary"] = std::to_string(results.size()) + " CRG graph node match(es)";
        root["results"] = results;
        root["count"] = results.size();
        root["used_fts"] = used_fts;
        root["truncated"] = truncated;
        root["next_actions"] = json::array({"source.review_context", "source.impact_radius", "source.crg_graph_health"});
        print_json(root);
    }

    json augment_override_risk(json risk, int64_t id) {
        Rows override_children = override_edges_to(std::to_string(id), 1000);
        Rows override_parents = override_edges_from(std::to_string(id), 100);
        if (override_children.empty() && override_parents.empty()) return risk;

        if (!risk.contains("reasons") || !risk["reasons"].is_array()) risk["reasons"] = json::array();
        double score = risk.value("score", 0.0);
        double child_factor = std::min<double>(override_children.size(), 30) / 30.0 * 0.20;
        double parent_factor = std::min<double>(override_parents.size(), 10) / 10.0 * 0.05;
        if (child_factor > 0.0) {
            risk["reasons"].push_back("override fan-out: " + std::to_string(override_children.size()) + " child override method(s)");
        }
        if (parent_factor > 0.0) {
            risk["reasons"].push_back("overrides parent method(s): " + std::to_string(override_parents.size()));
        }
        score = std::max(0.0, std::min(score + child_factor + parent_factor, 1.0));
        risk["score"] = std::round(score * 1000.0) / 1000.0;
        risk["tier"] = tier_for(score);
        if (!risk.contains("raw_counts") || !risk["raw_counts"].is_object()) risk["raw_counts"] = json::object();
        risk["raw_counts"]["override_children"] = override_children.size();
        risk["raw_counts"]["overridden_parents"] = override_parents.size();
        if (risk.contains("cache") && risk["cache"].is_object()) risk["cache"]["override_augmented"] = true;
        return risk;
    }

    json score_symbol(const Row& sym) {
        int64_t id = sym.get_int64("id");
        int64_t callers = count_rows(db, "SELECT COUNT(*) FROM \"references\" WHERE to_symbol_id = " + std::to_string(id) + ";");
        int64_t callees = count_rows(db, "SELECT COUNT(*) FROM \"references\" WHERE from_symbol_id = " + std::to_string(id) + ";");
        int64_t children = count_rows(db, "SELECT COUNT(*) FROM inheritance WHERE parent_id = " + std::to_string(id) + ";");
        int64_t parents = count_rows(db, "SELECT COUNT(*) FROM inheritance WHERE child_id = " + std::to_string(id) + ";");
        int64_t caller_files = count_rows(db, "SELECT COUNT(DISTINCT file_id) FROM \"references\" WHERE to_symbol_id = " + std::to_string(id) + ";");
        Rows override_children = override_edges_to(std::to_string(id), 1000);
        Rows override_parents = override_edges_from(std::to_string(id), 100);
        callers = std::max<int64_t>(callers, 0);
        callees = std::max<int64_t>(callees, 0);
        children = std::max<int64_t>(children, 0);
        parents = std::max<int64_t>(parents, 0);
        caller_files = std::max<int64_t>(caller_files, 0);
        json reasons = json::array();
        double raw = 0.0;
        auto factor = [&](double c, const std::string& why) {
            if (c > 0.0) { raw += c; reasons.push_back(why); }
        };
        factor(std::min<double>(callers, 50) / 50.0 * 0.35, "caller fan-in: " + std::to_string(callers));
        factor(std::min<double>(children, 30) / 30.0 * 0.25, "inheritance descendants (1-hop): " + std::to_string(children));
        factor(std::min<double>(override_children.size(), 30) / 30.0 * 0.20,
               "override fan-out: " + std::to_string(override_children.size()) + " child override method(s)");
        factor(std::min<double>(callees, 50) / 50.0 * 0.10, "callee fan-out: " + std::to_string(callees));
        factor(std::min<double>(override_parents.size(), 10) / 10.0 * 0.05,
               "overrides parent method(s): " + std::to_string(override_parents.size()));
        factor(sym.get_int("is_ue_macro") != 0 ? 0.15 : 0.0, "UE reflection macro symbol (UCLASS/UFUNCTION/UPROPERTY family)");
        factor(caller_files > 1 ? std::min<double>(caller_files, 20) / 20.0 * 0.15 : 0.0,
            "module/file boundary crossing: " + std::to_string(caller_files) + " distinct caller file(s)");
        auto sensitivity = sensitivity_factor(
            sym.get("name") + " " + sym.get("qualified_name") + " " + sym.get("kind") + " " + sym.get("signature"),
            true);
        factor(sensitivity.first, sensitivity.second);
        if (callers == 0 && sym.get("kind").find("function") != std::string::npos)
            reasons.push_back("missing direct callers: function has 0 indexed callers; may be reflection/delegate/Blueprint-invoked");
        double score = std::max(0.0, std::min(raw, 1.0));
        return {
            {"id", id},
            {"name", sym.get("name")},
            {"qualified_name", sym.get("qualified_name")},
            {"kind", sym.get("kind")},
            {"score", std::round(score * 1000.0) / 1000.0},
            {"tier", tier_for(score)},
            {"reasons", reasons},
            {"raw_counts", {
                {"callers", callers},
                {"callees", callees},
                {"descendants", children},
                {"ancestors", parents},
                {"override_children", override_children.size()},
                {"overridden_parents", override_parents.size()},
                {"caller_files", caller_files},
                {"is_ue_macro", sym.get_int("is_ue_macro") != 0},
                {"sensitivity", sensitivity.first},
            }},
            {"cache", {{"status", crg_cache_present(db) ? "miss" : "unavailable"}}},
            {"scoring_version", "3"},
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
                scored = augment_override_risk(cached, row.get_int64("id"));
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
        add_next(root, {"source.review_context", "source.find_overrides", "source.impact_radius"});
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
        if (k != "fan_in" && k != "fan_out" && k != "risk" && k != "large" && k != "override" && k != "all") {
            root["status"] = "error";
            root["summary"] = "Unsupported kind for source.review_hotspots (expected fan_in|fan_out|risk|large|override|all)";
            root["hotspots"] = json::array();
            root["truncated"] = false;
            add_next(root, {"source.review_hotspots", "source.risk_score"});
            return root;
        }

        bool has_crg_cache = crg_cache_present(db);
        bool has_override_cache = has_crg_cache && source_override_edge_cache_ready(db);
        std::string where;
        if (k == "large") {
            where = "WHERE lines >= " + std::to_string(loc_floor) + " ";
        } else if (k == "override") {
            where = "WHERE override_children > 0 OR overridden_parents > 0 ";
        } else {
            where = "WHERE fan_in > 0 OR fan_out > 0 OR descendants > 0 OR risk_score > 0 OR lines >= " + std::to_string(loc_floor) + " ";
        }
        std::string order = "ORDER BY hotspot_score DESC, risk_score DESC, fan_in DESC, lines DESC ";
        if (k == "fan_in") order = "ORDER BY fan_in DESC, risk_score DESC, lines DESC ";
        else if (k == "fan_out") order = "ORDER BY fan_out DESC, risk_score DESC, lines DESC ";
        else if (k == "risk") order = "ORDER BY risk_score DESC, fan_in DESC, lines DESC ";
        else if (k == "large") order = "ORDER BY lines DESC, risk_score DESC, fan_in DESC ";
        else if (k == "override") order = "ORDER BY override_children DESC, overridden_parents DESC, risk_score DESC, fan_in DESC, lines DESC ";
        int fetch_limit = (k == "override")
            ? std::min(2000, std::max(cap + 1, cap * 8 + 50))
            : cap + 1;
        int override_seed_limit = std::min(10000, std::max(5000, cap * 1000));

        std::string sql;
        if (k == "override") {
            if (has_override_cache) {
                sql = "WITH override_symbols AS ("
                      "  SELECT parent_symbol_id AS symbol_id FROM source_override_edges"
                      "  UNION SELECT child_symbol_id AS symbol_id FROM source_override_edges"
                      "), override_children AS ("
                      "  SELECT parent_symbol_id AS symbol_id, COUNT(*) AS override_children FROM source_override_edges GROUP BY parent_symbol_id"
                      "), overridden_parents AS ("
                      "  SELECT child_symbol_id AS symbol_id, COUNT(*) AS overridden_parents FROM source_override_edges GROUP BY child_symbol_id"
                      "), scored AS ("
                      "  SELECT s.id,s.name,s.qualified_name,s.kind,COALESCE(f.path,'') AS file,s.line_start,s.line_end,"
                      "         CASE WHEN s.line_end >= s.line_start THEN s.line_end - s.line_start + 1 ELSE 0 END AS lines,"
                      "         COALESCE(m.fan_in,0) AS fan_in,COALESCE(m.fan_out,0) AS fan_out,"
                      "         COALESCE(m.descendants,0) AS descendants,"
                      "         COALESCE(oc.override_children,0) AS override_children,"
                      "         COALESCE(op.overridden_parents,0) AS overridden_parents,"
                      "         COALESCE(m.risk_score,0.0) AS risk_score,COALESCE(m.risk_tier, 'low') AS risk_tier "
                      "  FROM override_symbols os "
                      "  JOIN symbols s ON s.id=os.symbol_id "
                      "  LEFT JOIN files f ON f.id=s.file_id "
                      "  LEFT JOIN override_children oc ON oc.symbol_id=s.id "
                      "  LEFT JOIN overridden_parents op ON op.symbol_id=s.id "
                      "  LEFT JOIN crg_nodes n ON n.domain='source' AND n.native_table='symbols' AND n.native_id=s.id "
                      "  LEFT JOIN crg_node_metrics m ON m.node_id=n.id"
                      ") "
                      "SELECT *, MAX(risk_score, MIN(override_children,30)/30.0, MIN(overridden_parents,10)/10.0, MIN(lines,500)/500.0) AS hotspot_score "
                      "FROM scored " + where + order + "LIMIT " + std::to_string(fetch_limit) + ";";
            } else {
                std::string risk_expr = "c.estimated_risk";
                std::string tier_expr = "CASE WHEN c.estimated_risk >= 0.66 THEN 'high' WHEN c.estimated_risk >= 0.33 THEN 'medium' ELSE 'low' END";
                sql = "WITH RECURSIVE child_seed AS ("
                      "  SELECT id,name,kind,parent_symbol_id FROM symbols "
                      "  WHERE kind='function' AND signature LIKE '%override%' LIMIT " + std::to_string(override_seed_limit) +
                      "), ancestors(child_class_id, ancestor_class_id) AS ("
                      "  SELECT child_id, parent_id FROM inheritance"
                      "  UNION "
                      "  SELECT a.child_class_id, i.parent_id "
                      "  FROM ancestors a "
                      "  JOIN inheritance AS i ON i.child_id=a.ancestor_class_id"
                      "), override_children AS ("
                      "  SELECT base_fn.id AS symbol_id, COUNT(*) AS override_children "
                      "  FROM child_seed child_fn "
                      "  JOIN symbols child_cls ON child_cls.id=child_fn.parent_symbol_id "
                      "  JOIN ancestors AS a ON a.child_class_id=child_cls.id "
                      "  JOIN symbols base_cls ON base_cls.id=a.ancestor_class_id "
                      "  JOIN symbols base_fn INDEXED BY idx_symbols_parent_name_kind ON base_fn.parent_symbol_id=base_cls.id "
                      "    AND (base_fn.name=child_fn.name "
                      "      OR base_fn.name=base_cls.name || '::' || child_fn.name) "
                      "    AND base_fn.kind=child_fn.kind "
                      "  GROUP BY base_fn.id"
                      "), ref_in AS ("
                      "  SELECT to_symbol_id AS symbol_id, COUNT(*) AS fan_in, COUNT(DISTINCT r.file_id) AS caller_files FROM \"references\" r GROUP BY to_symbol_id"
                      "), ref_out AS ("
                      "  SELECT from_symbol_id AS symbol_id, COUNT(*) AS fan_out FROM \"references\" r GROUP BY from_symbol_id"
                      "), inh_desc AS ("
                      "  SELECT parent_id AS symbol_id, COUNT(*) AS descendants FROM inheritance GROUP BY parent_id"
                      "), counts AS ("
                      "  SELECT s.id,s.name,s.qualified_name,s.kind,COALESCE(f.path,'') AS file,s.line_start,s.line_end,"
                      "         CASE WHEN s.line_end >= s.line_start THEN s.line_end - s.line_start + 1 ELSE 0 END AS lines,"
                      "         COALESCE(ri.fan_in,0) AS fan_in,COALESCE(ro.fan_out,0) AS fan_out,COALESCE(id.descendants,0) AS descendants,"
                      "         oc.override_children,0 AS overridden_parents,COALESCE(ri.caller_files,0) AS caller_files,s.is_ue_macro AS is_ue_macro,"
                      "         MIN(1.0, MIN(COALESCE(ri.fan_in,0),50)/50.0*0.35 + MIN(COALESCE(id.descendants,0),30)/30.0*0.25 + MIN(COALESCE(ro.fan_out,0),50)/50.0*0.10 + CASE WHEN s.is_ue_macro != 0 THEN 0.15 ELSE 0 END + MIN(COALESCE(ri.caller_files,0),20)/20.0*0.15) AS estimated_risk "
                      "  FROM override_children oc JOIN symbols s ON s.id=oc.symbol_id LEFT JOIN files f ON f.id=s.file_id "
                      "  LEFT JOIN ref_in ri ON ri.symbol_id=s.id LEFT JOIN ref_out ro ON ro.symbol_id=s.id LEFT JOIN inh_desc id ON id.symbol_id=s.id"
                      "), scored AS ("
                      "  SELECT c.id,c.name,c.qualified_name,c.kind,c.file,c.line_start,c.line_end,c.lines,c.fan_in,c.fan_out,c.descendants,c.override_children,c.overridden_parents," + risk_expr + " AS risk_score," + tier_expr + " AS risk_tier FROM counts c"
                      ") "
                      "SELECT *, MAX(risk_score, MIN(override_children,30)/30.0, MIN(lines,500)/500.0) AS hotspot_score FROM scored " + where + order + "LIMIT " + std::to_string(fetch_limit) + ";";
            }
        } else if (has_crg_cache) {
            sql = R"SQL(
WITH scored AS (
  SELECT s.id,s.name,s.qualified_name,s.kind,COALESCE(f.path,'') AS file,s.line_start,s.line_end,
         CASE WHEN s.line_end >= s.line_start THEN s.line_end - s.line_start + 1 ELSE 0 END AS lines,
         COALESCE(m.fan_in,0) AS fan_in,COALESCE(m.fan_out,0) AS fan_out,
         COALESCE(m.descendants,0) AS descendants,0 AS override_children,0 AS overridden_parents,
         COALESCE(m.risk_score,0.0) AS risk_score,
         COALESCE(m.risk_tier, 'low') AS risk_tier
  FROM symbols s
  LEFT JOIN files f ON f.id=s.file_id
  LEFT JOIN crg_nodes n ON n.domain='source' AND n.native_table='symbols' AND n.native_id=s.id
  LEFT JOIN crg_node_metrics m ON m.node_id=n.id
)
SELECT *, MAX(risk_score, MIN(fan_in,50)/50.0, MIN(fan_out,50)/50.0, MIN(lines,500)/500.0) AS hotspot_score
FROM scored )SQL" + where + order + "LIMIT " + std::to_string(fetch_limit) + ";";
        } else {
            std::string risk_expr = "c.estimated_risk";
            std::string tier_expr = "CASE WHEN c.estimated_risk >= 0.66 THEN 'high' WHEN c.estimated_risk >= 0.33 THEN 'medium' ELSE 'low' END";
            sql = R"SQL(
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
         0 AS override_children,0 AS overridden_parents,
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
         c.fan_in,c.fan_out,c.descendants,c.override_children,c.overridden_parents,)SQL" + risk_expr + R"SQL( AS risk_score,)SQL" + tier_expr + R"SQL( AS risk_tier
  FROM counts c
)
SELECT *, MAX(risk_score, MIN(fan_in,50)/50.0, MIN(fan_out,50)/50.0, MIN(lines,500)/500.0) AS hotspot_score
FROM scored )SQL" + where + order + "LIMIT " + std::to_string(fetch_limit) + ";";
        }

        Rows rows = query(db, sql);
        bool truncated = false;
        json hotspots = json::array();
        json questions = json::array();
        for (const auto& r : rows) {
            int fan_in = r.get_int("fan_in");
            int fan_out = r.get_int("fan_out");
            int override_children = r.get_int("override_children");
            int overridden_parents = r.get_int("overridden_parents");
            if (k == "override" && !has_override_cache) {
                std::string symbol_id = std::to_string(r.get_int64("id"));
                override_children = static_cast<int>(override_edges_to(symbol_id, 1000).size());
                overridden_parents = static_cast<int>(override_edges_from(symbol_id, 1000).size());
            }
            if (k == "override" && override_children <= 0 && overridden_parents <= 0) {
                continue;
            }
            int lines = r.get_int("lines");
            double risk = r.get_double("risk_score");
            risk = std::min(1.0, risk
                + std::min<double>(override_children, 30) / 30.0 * 0.20
                + std::min<double>(overridden_parents, 10) / 10.0 * 0.05);
            if ((int)hotspots.size() >= cap) {
                truncated = true;
                break;
            }
            std::string primary = k;
            if (primary == "all") {
                double best = risk;
                primary = "risk";
                double in_signal = std::min<double>(fan_in, 50) / 50.0;
                double out_signal = std::min<double>(fan_out, 50) / 50.0;
                double override_signal = std::max(
                    std::min<double>(override_children, 30) / 30.0,
                    std::min<double>(overridden_parents, 10) / 10.0);
                double large_signal = std::min<double>(lines, 500) / 500.0;
                if (in_signal > best) { best = in_signal; primary = "fan_in"; }
                if (out_signal > best) { best = out_signal; primary = "fan_out"; }
                if (override_signal > best) { best = override_signal; primary = "override"; }
                if (large_signal > best) primary = "large";
            }
            json signals = {
                {"fan_in", fan_in},
                {"fan_out", fan_out},
                {"descendants", r.get_int("descendants")},
                {"override_children", override_children},
                {"overridden_parents", overridden_parents},
                {"risk_score", std::round(risk * 1000.0) / 1000.0},
                {"risk_tier", tier_for(risk)},
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
                        : (primary == "override"
                            ? "Which child overrides and parent contracts must be reviewed before changing this method?"
                            : "Which callers and tests cover this hotspot before changing it?")},
                });
            }
        }
        root["status"] = "ok";
        root["summary"] = std::to_string(hotspots.size()) + " source review hotspot(s) ranked by " + k
            + (has_crg_cache ? " using CRG cache when available" : " using native fallback");
        root["cache"] = {
            {"status", has_crg_cache ? "hit" : "miss"},
            {"source", has_override_cache ? "crg_node_metrics + source_override_edges"
                : (has_crg_cache ? "crg_node_metrics + query-time override aggregation" : "query-time references/inheritance/override aggregation")},
        };
        root["hotspots"] = hotspots;
        if (include_questions) root["questions"] = questions;
        root["truncated"] = truncated;
        add_next(root, {"source.review_context", "source.find_overrides", "source.risk_score", "source.impact_radius"});
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
        bool over = wants_kind(kinds, "override");
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
                if (out && over) for (const auto& r : override_edges_from(cur, per_node)) neighbors.push_back({r.get("parent_id"), "override"});
                if (in && over) for (const auto& r : override_edges_to(cur, per_node)) neighbors.push_back({r.get("child_id"), "override"});
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
        add_next(root, {"source.review_context", "source.find_overrides", "source.risk_score", "source.find_callers"});
        return root;
    }

    void impact_radius(const Args& args) {
        std::string symbol = args.opt("symbol");
        if (symbol.empty() && !args.positional.empty()) symbol = args.positional[0];
        if (symbol.empty()) die("impact_radius requires a symbol argument");
        print_json(source_impact_radius_json(symbol, args.opt("edge_kinds", "call|type|inheritance"), args.opt("direction", "both"), args.opt_int("max_depth", 2), args.opt_int("max_results", 200)));
    }

    void find_overrides(const Args& args) {
        std::string symbol = args.opt("symbol");
        if (symbol.empty() && !args.positional.empty()) symbol = args.positional[0];
        if (symbol.empty()) die("find_overrides requires a symbol argument");
        std::string direction = args.opt("direction", "both");
        int max_depth = args.opt_int("max_depth", 2);
        int max_results = args.opt_int("max_results", 200);
        std::string detail = lower_copy(args.opt("detail_level", "minimal"));
        bool standard = detail == "standard" || detail == "full";
        if (!standard && detail != "minimal") die("find_overrides --detail-level must be minimal or standard");
        json root = source_impact_radius_json(symbol, "override", direction, max_depth, max_results);
        root["overrides"] = root.value("impacted_symbols", json::array());
        root["direct_override_edges"] = json::array();
        root["detail_level"] = standard ? "standard" : "minimal";

        int cap = clamp_int(max_results <= 0 ? 200 : max_results, 1, 1000);
        int direct_edge_cap = standard ? cap : std::min(cap, 25);
        int direct_edge_count = 0;
        auto seeds = symbols_by_name(symbol, 5);
        bool in = direction != "out";
        bool out = direction != "in";
        auto add_direct_edge = [&](const Row& edge) {
            ++direct_edge_count;
            if ((int)root["direct_override_edges"].size() < direct_edge_cap) {
                root["direct_override_edges"].push_back(override_edge_json(edge, 1));
            }
        };
        for (const auto& seed : seeds) {
            if (out) {
                for (const auto& edge : override_edges_from(seed.get("id"), cap)) {
                    add_direct_edge(edge);
                }
            }
            if (in) {
                for (const auto& edge : override_edges_to(seed.get("id"), cap)) {
                    add_direct_edge(edge);
                }
            }
        }
        root["direct_override_edge_count"] = direct_edge_count;
        root["direct_override_edges_truncated"] = direct_edge_count > (int)root["direct_override_edges"].size();
        json edges = root.value("edges", json::array());
        int edge_count = edges.size();
        root["edge_count"] = edge_count;
        if (!standard) {
            json edge_sample = json::array();
            for (size_t i = 0; i < edges.size() && i < 25u; ++i) edge_sample.push_back(edges[i]);
            root["edges"] = edge_sample;
            root["edges_truncated"] = edge_count > (int)edge_sample.size();
            root.erase("impacted_symbols");
        } else {
            root["edges_truncated"] = false;
        }
        root["summary"] = std::to_string(root["overrides"].size()) + " override-related symbol(s) within depth "
            + std::to_string(clamp_int(max_depth <= 0 ? 2 : max_depth, 0, 8))
            + " (" + direction + ") of " + symbol;
        add_next(root, {"source.impact_radius", "source.review_context", "source.risk_score"});
        print_json(root);
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
        json cached = try_cached_risk(db, "source", "symbols", seed.get_int64("id"));
        json risk;
        if (!cached.is_null()) {
            risk = augment_override_risk(cached, seed.get_int64("id"));
            risk["id"] = seed.get_int64("id");
            risk["name"] = seed.get("name");
            risk["qualified_name"] = seed.get("qualified_name");
            risk["kind"] = seed.get("kind");
        } else {
            risk = score_symbol(seed);
        }
        root["risk"] = risk;
        root["top_risks"] = json::array();
        for (size_t i = 0; i < risk["reasons"].size() && i < 5; ++i) root["top_risks"].push_back(risk["reasons"][i]);
        json impact = source_impact_radius_json(symbol, "call|type|inheritance|override", direction, max_depth, minimal ? std::min(max_results, 25) : max_results);
        json summary = {{"truncated", impact.value("truncated", false)}};
        summary["impacted_count"] = impact["impacted_symbols"].size();
        summary["top_impacted"] = json::array();
        std::set<std::string> exact_impacted;
        std::set<std::string> known_impacted;
        size_t top_limit = minimal ? 5u : 25u;
        for (size_t i = 0; i < impact["impacted_symbols"].size() && summary["top_impacted"].size() < top_limit; ++i) {
            const json& item = impact["impacted_symbols"][i];
            std::string qualified_name = item.value("qualified_name", "");
            std::string name = item.value("name", "");
            std::string kind = item.value("kind", "");
            std::string file = item.value("file", "");
            std::string status = item.value("path_status", source_path_status(file));
            std::string symbol_key = (qualified_name.empty() ? name : qualified_name) + "|" + kind;
            std::string exact_key = symbol_key + "|" + file;
            bool known = status == "known" || known_source_path(file);
            if (exact_impacted.count(exact_key)) continue;
            if (!known && known_impacted.count(symbol_key)) continue;
            exact_impacted.insert(exact_key);
            if (known) known_impacted.insert(symbol_key);
            summary["top_impacted"].push_back(item);
        }
        root["impact"] = summary;
        root["seed"] = symbol_json(seed, 0);
        std::string seed_file = get_file_path(seed.get_int("file_id"));
        root["context"] = json::array({{{"type", "seed_symbol"}, {"name", seed.get("name")}, {"qualified_name", seed.get("qualified_name")}, {"file", short_path(seed_file)}, {"path_status", source_path_status(seed_file)}, {"line", seed.get_int("line_start")}}});
        if (!minimal) root["details"] = symbol_json(seed, 0);
        root["status"] = "ok";
        root["summary"] = seed.get("name") + " risk=" + risk["tier"].get<std::string>() + " (" + (minimal ? "minimal" : "standard") + "); review the top impacted symbols and high-risk reasons";
        root["truncated"] = impact.value("truncated", false);
        add_next(root, {"source.read_source", "source.impact_radius", "source.find_overrides", "source.get_class_hierarchy"});
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
                if (!cached.is_null()) { any_cache = true; item = augment_override_risk(cached, sid); }
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
        add_next(root, {"source.review_context", "source.find_overrides", "source.impact_radius", "source.risk_score"});
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

        json health = source_health_json(false, true);
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

    // ============================================================
    // Phase 1-3 — LLM C++ authoring ergonomics. Byte-equivalent to the
    // monolith_offline.py SourceErgo methods (parity-gated, plain-text output).
    // ============================================================

private:
    // ASCII-only isalnum, matching Python str.isalnum() over the ASCII subset
    // these paths exercise (identifier chars only).
    static bool is_word_char(char c) {
        unsigned char uc = static_cast<unsigned char>(c);
        return std::isalnum(uc) != 0 || c == '_';
    }

    // strip a trailing "::method" to the class name (mirror of the Python
    // `symbol[:symbol.rfind("::")]` pattern).
    static std::string strip_to_class(const std::string& symbol) {
        auto scope = symbol.rfind("::");
        return (scope != std::string::npos) ? symbol.substr(0, scope) : symbol;
    }

    // strip a leading "Class::" to the method name.
    static std::string strip_to_method(const std::string& symbol) {
        auto scope = symbol.rfind("::");
        return (scope != std::string::npos) ? symbol.substr(scope + 2) : symbol;
    }

    struct IncludeInfo {
        std::string include;
        bool includable = true;
        std::string warning;
    };

    // Mirror of SourceErgo.derive_include_path / FMonolithSourceActions::DeriveIncludePath.
    static IncludeInfo derive_include_path(const std::string& indexed_path,
                                           const std::string& module_name) {
        std::string path = indexed_path;
        std::replace(path.begin(), path.end(), '\\', '/');
        for (const char* root : {"/Public/", "/Classes/", "/Internal/"}) {
            auto idx = path.rfind(root);
            if (idx != std::string::npos) {
                IncludeInfo info;
                info.include = path.substr(idx + std::strlen(root));
                info.includable = true;
                return info;
            }
        }
        auto pidx = path.rfind("/Private/");
        if (pidx != std::string::npos) {
            IncludeInfo info;
            info.include = path.substr(pidx + std::strlen("/Private/"));
            info.includable = false;
            info.warning = "Private header -- not includable outside "
                + (module_name.empty() ? std::string("its module") : module_name)
                + "; same-module include shown";
            return info;
        }
        auto slash = path.rfind('/');
        IncludeInfo info;
        info.include = (slash == std::string::npos) ? path : path.substr(slash + 1);
        info.includable = true;
        return info;
    }

    // Mirror of SourceErgo.resolve_symbol_row. Returns the first matching row or
    // an empty Rows when nothing resolves.
    Rows resolve_symbol_row(const std::string& symbol) {
        std::string lookup = strip_to_class(symbol);
        auto rows = query(db,
            "SELECT id, name, file_id FROM symbols WHERE name = ? ORDER BY (line_end > line_start) DESC",
            {lookup});
        if (rows.empty()) {
            std::string fts_q = escape_fts(lookup);
            rows = query(db,
                "SELECT s.id, s.name, s.file_id FROM symbols_fts f JOIN symbols s ON s.id = f.rowid "
                "WHERE symbols_fts MATCH ? LIMIT 5", {fts_q});
        }
        if (rows.empty()) return {};
        return {rows[0]};
    }

    // Mirror of SourceErgo.compact_declaration / FMonolithSourceActions::CompactDeclaration.
    static std::string compact_declaration(const std::vector<std::string>& lines, size_t start_idx) {
        std::string accum;
        int paren_depth = 0;
        bool saw_open = false;
        size_t limit = std::min(lines.size(), start_idx + 12);
        for (size_t i = start_idx; i < limit; ++i) {
            std::string line = lines[i];
            // rstrip
            while (!line.empty() && std::isspace(static_cast<unsigned char>(line.back())))
                line.pop_back();
            if (!line.empty() && line.back() == '\\') {
                line.pop_back();
                while (!line.empty() && std::isspace(static_cast<unsigned char>(line.back())))
                    line.pop_back();
            }
            bool done = false;
            for (char ch : line) {
                if (ch == '(') {
                    paren_depth += 1;
                    saw_open = true;
                } else if (ch == ')') {
                    paren_depth = std::max(0, paren_depth - 1);
                } else if (paren_depth == 0 && saw_open && (ch == '{' || ch == ';')) {
                    done = true;
                    break;
                } else {
                    accum += ch;
                    continue;
                }
                accum += ch;
            }
            if (done) break;
            accum += ' ';
        }
        // collapse whitespace
        std::string out;
        bool prev_space = false;
        for (char ch : accum) {
            if (ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n') {
                if (!prev_space) out += ' ';
                prev_space = true;
            } else {
                out += ch;
                prev_space = false;
            }
        }
        return trim_copy(out);
    }

    // Read a file fully, splitting into lines with trailing \n and \r stripped.
    // Mirrors the Python `[ln.rstrip("\n").rstrip("\r") for ln in f.readlines()]`
    // pattern used by the ergonomics scanners. Returns false on open failure.
    static bool read_lines_strip_nl(const std::string& path, std::vector<std::string>& out) {
        std::ifstream f(path, std::ios::binary);
        if (!f.is_open()) return false;
        std::string content((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        out.clear();
        std::string line;
        for (char ch : content) {
            if (ch == '\n') {
                while (!line.empty() && line.back() == '\r') line.pop_back();
                out.push_back(line);
                line.clear();
            } else {
                line += ch;
            }
        }
        // trailing partial line (no terminating newline)
        if (!line.empty()) {
            while (!line.empty() && line.back() == '\r') line.pop_back();
            out.push_back(line);
        }
        return true;
    }

public:
    // item 1: get_include_path
    void get_include_path(const Args& args) {
        std::string symbol = args.positional.empty() ? args.opt("symbol") : args.positional[0];
        Rows sym = resolve_symbol_row(symbol);
        if (sym.empty()) {
            std::cerr << "No symbol found matching '" << symbol << "'." << std::endl;
            std::exit(1);
        }

        int file_id = sym[0].get_int("file_id");
        std::string lookup = strip_to_class(symbol);
        auto allrows = query(db,
            "SELECT s.file_id, f.path FROM symbols s JOIN files f ON f.id = s.file_id WHERE s.name = ?",
            {lookup});
        std::string file_path = get_file_path(file_id);
        for (auto& r : allrows) {
            std::string p = r.get("path");
            if (p.size() >= 2 && p.substr(p.size() - 2) == ".h") {
                file_id = r.get_int("file_id");
                file_path = p;
                break;
            }
        }

        auto mrows = query(db,
            "SELECT m.name, m.build_cs_path FROM files f JOIN modules m ON m.id = f.module_id WHERE f.id = ?",
            {std::to_string(file_id)});
        std::string module_name = mrows.empty() ? "" : mrows[0].get("name");
        std::string build_cs = mrows.empty() ? "" : mrows[0].get("build_cs_path");

        IncludeInfo info = derive_include_path(file_path, module_name);

        std::string build_cs_note;
        if (!module_name.empty()) {
            if (!build_cs.empty()) {
                std::string norm = build_cs;
                std::replace(norm.begin(), norm.end(), '\\', '/');
                auto slash = norm.rfind('/');
                std::string base = (slash == std::string::npos) ? norm : norm.substr(slash + 1);
                build_cs_note = "Module '" + module_name + "' -- add to your Build.cs deps (" + base + ")";
            } else {
                build_cs_note = "Module '" + module_name + "' -- add to your Build.cs deps";
            }
        }

        std::vector<std::string> out;
        out.push_back("#include \"" + info.include + "\"");
        if (!module_name.empty()) out.push_back("Module: " + module_name);
        if (!build_cs_note.empty()) out.push_back(build_cs_note);
        if (!info.warning.empty()) out.push_back("WARNING: " + info.warning);

        for (size_t i = 0; i < out.size(); ++i) {
            if (i > 0) std::cout << "\n";
            std::cout << out[i];
        }
        std::cout << "\n";
    }

    // item 2: get_signature
    void get_signature(const Args& args) {
        std::string symbol = args.positional.empty() ? args.opt("symbol") : args.positional[0];
        int limit = args.opt_int("limit", 10);
        std::string method = strip_to_method(symbol);

        // (sig, source, shortpath, line)
        std::vector<std::tuple<std::string, std::string, std::string, int>> overloads;

        auto fnrows = query(db,
            "SELECT signature, file_id, line_start FROM symbols WHERE name = ? AND kind = 'function'",
            {method});
        for (auto& r : fnrows) {
            if ((int)overloads.size() >= limit) break;
            std::string sig = r.get("signature");
            if (sig.empty()) continue;
            if (sig.find('{') != std::string::npos || sig.find('\\') != std::string::npos) continue;
            sig = trim_copy(sig);
            overloads.emplace_back(sig, "column",
                                   short_path(get_file_path(r.get_int("file_id"))),
                                   r.get_int("line_start"));
        }

        if (overloads.empty()) {
            std::string fts_q = escape_fts(symbol);
            auto chunks = query(db,
                "SELECT file_id, line_number, text FROM source_fts WHERE source_fts MATCH ? "
                "ORDER BY bm25(source_fts) LIMIT 50", {fts_q});
            std::set<std::string> seen;
            std::string needle = method + "(";
            for (auto& ch : chunks) {
                if ((int)overloads.size() >= limit) break;
                std::string fp = get_file_path(ch.get_int("file_id"));
                std::vector<std::string> file_lines;
                if (!read_lines_strip_nl(fp, file_lines)) continue;
                int line_number = ch.get_int("line_number");
                int win_start = std::max(0, line_number - 1);
                int win_end = std::min((int)file_lines.size(), win_start + 10);
                for (int i = win_start; i < win_end; ++i) {
                    if ((int)overloads.size() >= limit) break;
                    const std::string& line = file_lines[i];
                    auto didx = line.find(needle);
                    if (didx == std::string::npos) continue;
                    if (didx > 0 && is_word_char(line[didx - 1])) continue;
                    std::string sig = compact_declaration(file_lines, i);
                    if (sig.empty() || sig.find(needle) == std::string::npos) continue;
                    if (seen.count(sig)) continue;
                    seen.insert(sig);
                    overloads.emplace_back(sig, "declaration_read", short_path(fp), i + 1);
                }
            }
        }

        if (overloads.empty()) {
            std::cerr << "No signature found for '" << symbol << "'." << std::endl;
            std::exit(1);
        }

        for (size_t i = 0; i < overloads.size(); ++i) {
            if (i > 0) std::cout << "\n";
            std::cout << std::get<0>(overloads[i]) << "\n  // "
                      << std::get<1>(overloads[i]) << " @ "
                      << std::get<2>(overloads[i]) << ":" << std::get<3>(overloads[i]);
        }
        std::cout << "\n";
    }

    // item 3: check_deprecations
    void check_deprecations(const Args& args) {
        const std::vector<std::string>& symbols = args.positional;
        auto cnt = query(db, "SELECT COUNT(*) as c FROM symbol_deprecations");
        int total = cnt.empty() ? 0 : cnt[0].get_int("c");
        if (total == 0) {
            std::cout << "Deprecation index is empty (schema v2 landed but not yet populated). "
                         "Run source.trigger_reindex to populate it." << "\n";
            return;
        }
        std::vector<std::string> lines;
        for (const auto& name : symbols) {
            auto rows = query(db,
                "SELECT version, message, kind FROM symbol_deprecations WHERE symbol_name = ? LIMIT 1",
                {name});
            if (!rows.empty()) {
                lines.push_back(name + ": DEPRECATED (" + rows[0].get("version") + ") ["
                                + rows[0].get("kind") + "] " + rows[0].get("message"));
            } else {
                lines.push_back(name + ": not deprecated");
            }
        }
        for (size_t i = 0; i < lines.size(); ++i) {
            if (i > 0) std::cout << "\n";
            std::cout << lines[i];
        }
        std::cout << "\n";
    }

private:
    // Mirror of SourceErgo.symbol_exists.
    bool symbol_exists(const std::string& symbol) {
        std::string lookup = strip_to_class(symbol);
        if (!query(db, "SELECT id FROM symbols WHERE name = ? LIMIT 1", {lookup}).empty())
            return true;
        std::string fts_q = escape_fts(lookup);
        if (!query(db, "SELECT s.id FROM symbols_fts f JOIN symbols s ON s.id = f.rowid "
                       "WHERE symbols_fts MATCH ? LIMIT 1", {fts_q}).empty())
            return true;
        std::string method = strip_to_method(symbol);
        std::string needle = method + "(";
        std::string sfts = escape_fts(symbol);
        auto chunks = query(db,
            "SELECT file_id, line_number FROM source_fts WHERE source_fts MATCH ? "
            "ORDER BY bm25(source_fts) LIMIT 25", {sfts});
        for (auto& ch : chunks) {
            std::vector<std::string> fl;
            if (!read_lines_strip_nl(get_file_path(ch.get_int("file_id")), fl)) continue;
            int ln = ch.get_int("line_number");
            int ws = std::max(0, ln - 1);
            int we = std::min((int)fl.size(), ws + 10);
            for (int i = ws; i < we; ++i) {
                auto di = fl[i].find(needle);
                if (di == std::string::npos) continue;
                if (di > 0 && is_word_char(fl[i][di - 1])) continue;
                return true;
            }
        }
        return false;
    }

    // Mirror of SourceErgo.first_signature. Returns (sig, source); sig empty when none.
    std::pair<std::string, std::string> first_signature(const std::string& symbol) {
        std::string method = strip_to_method(symbol);
        auto fnrows = query(db,
            "SELECT signature, file_id, line_start FROM symbols WHERE name = ? AND kind = 'function'",
            {method});
        for (auto& r : fnrows) {
            std::string sig = r.get("signature");
            if (sig.empty() || sig.find('{') != std::string::npos || sig.find('\\') != std::string::npos)
                continue;
            return {trim_copy(sig), "column"};
        }
        std::string fts_q = escape_fts(symbol);
        auto chunks = query(db,
            "SELECT file_id, line_number FROM source_fts WHERE source_fts MATCH ? "
            "ORDER BY bm25(source_fts) LIMIT 50", {fts_q});
        std::string needle = method + "(";
        for (auto& ch : chunks) {
            std::vector<std::string> fl;
            if (!read_lines_strip_nl(get_file_path(ch.get_int("file_id")), fl)) continue;
            int ln = ch.get_int("line_number");
            int ws = std::max(0, ln - 1);
            int we = std::min((int)fl.size(), ws + 10);
            for (int i = ws; i < we; ++i) {
                auto di = fl[i].find(needle);
                if (di == std::string::npos) continue;
                if (di > 0 && is_word_char(fl[i][di - 1])) continue;
                std::string sig = compact_declaration(fl, i);
                if (sig.empty() || sig.find(needle) == std::string::npos) continue;
                return {sig, "declaration_read"};
            }
        }
        return {"", ""};
    }

public:
    // item 4: verify_symbols
    void verify_symbols(const Args& args) {
        const std::vector<std::string>& symbols = args.positional;
        auto cntrows = query(db, "SELECT COUNT(*) as c FROM symbol_deprecations");
        int cnt = cntrows.empty() ? 0 : cntrows[0].get_int("c");
        bool dep_empty = (cnt == 0);

        std::vector<std::string> lines;
        for (const auto& symbol : symbols) {
            if (!symbol_exists(symbol)) {
                lines.push_back(symbol + ": NOT FOUND");
                continue;
            }
            std::string line = symbol + ": exists";

            Rows sym = resolve_symbol_row(symbol);
            std::string include, module_name;
            bool includable = true;
            if (!sym.empty()) {
                std::string lookup = strip_to_class(symbol);
                auto allrows = query(db,
                    "SELECT s.file_id, f.path FROM symbols s JOIN files f ON f.id = s.file_id WHERE s.name = ?",
                    {lookup});
                int file_id = sym[0].get_int("file_id");
                std::string file_path = get_file_path(file_id);
                for (auto& r : allrows) {
                    std::string p = r.get("path");
                    if (p.size() >= 2 && p.substr(p.size() - 2) == ".h") {
                        file_id = r.get_int("file_id");
                        file_path = p;
                        break;
                    }
                }
                auto mrows = query(db,
                    "SELECT m.name FROM files f JOIN modules m ON m.id = f.module_id WHERE f.id = ?",
                    {std::to_string(file_id)});
                module_name = mrows.empty() ? "" : mrows[0].get("name");
                IncludeInfo info = derive_include_path(file_path, module_name);
                include = info.include;
                includable = info.includable;
            }
            if (!include.empty()) {
                line += " | #include \"" + include + "\"";
                if (!includable) line += " (NOT includable)";
            }

            auto sig = first_signature(symbol);
            if (!sig.first.empty()) line += " | " + sig.first;

            if (!dep_empty) {
                std::string method = strip_to_method(symbol);
                auto drow = query(db,
                    "SELECT version FROM symbol_deprecations WHERE symbol_name = ? LIMIT 1", {method});
                if (!drow.empty()) line += " | DEPRECATED";
            }
            lines.push_back(line);
        }
        for (size_t i = 0; i < lines.size(); ++i) {
            if (i > 0) std::cout << "\n";
            std::cout << lines[i];
        }
        std::cout << "\n";
    }

    // item 5: find_example_usage
    void find_example_usage(const Args& args) {
        std::string symbol = args.positional.empty() ? args.opt("symbol") : args.positional[0];
        std::string prefer = lower_copy(args.opt("prefer", "engine"));
        bool prefer_project = (prefer == "project");
        int limit = std::max(1, args.opt_int("limit", 10));
        const int HARD_CAP = 500, FTS_FETCH = 400;

        std::string method = strip_to_method(symbol);
        std::string needle = method + "(";

        // (shortpath, line, context, rank, sort_path)
        std::vector<std::tuple<std::string, int, std::string, int, std::string>> usages;
        std::set<std::string> seen;
        std::string fts_q = escape_fts(symbol);
        auto chunks = query(db,
            "SELECT file_id, line_number FROM source_fts WHERE source_fts MATCH ? "
            "ORDER BY bm25(source_fts) LIMIT ?", {fts_q, std::to_string(FTS_FETCH)});
        for (auto& ch : chunks) {
            int file_id = ch.get_int("file_id");
            std::string fp = get_file_path(file_id);
            std::vector<std::string> fl;
            if (!read_lines_strip_nl(fp, fl)) continue;
            int ln = ch.get_int("line_number");
            int ws = std::max(0, ln - 1);
            int we = std::min((int)fl.size(), ws + 10);
            for (int i = ws; i < we; ++i) {
                auto hi = fl[i].find(needle);
                if (hi == std::string::npos) continue;
                if (hi > 0 && is_word_char(fl[i][hi - 1])) continue;
                std::string key = std::to_string(file_id) + "_" + std::to_string(i + 1);
                if (seen.count(key)) continue;
                seen.insert(key);
                int cs = std::max(0, i - 3);
                int ce = std::min((int)fl.size() - 1, i + 3);
                std::string ctx;
                for (int c = cs; c <= ce; ++c) {
                    if (c > cs) ctx += "\n";
                    char buf[16];
                    std::snprintf(buf, sizeof(buf), "%5d", c + 1);
                    ctx += std::string(buf) + " | " + fl[c];
                }
                std::string norm = fp;
                std::replace(norm.begin(), norm.end(), '\\', '/');
                bool is_engine = norm.find("Engine/Source/") != std::string::npos;
                bool is_runtime = norm.find("/Source/Runtime/") != std::string::npos;
                int rank = (is_engine && is_runtime) ? 0 : (is_engine ? 1 : 2);
                usages.emplace_back(short_path(fp), i + 1, ctx, rank, fp);
            }
        }

        auto rank_key = [&](const std::tuple<std::string, int, std::string, int, std::string>& u) -> int {
            int r = std::get<3>(u);
            if (!prefer_project) return r;
            // {2: 0, 0: 1}.get(rank, 2)
            if (r == 2) return 0;
            if (r == 0) return 1;
            return 2;
        };

        std::stable_sort(usages.begin(), usages.end(),
            [&](const std::tuple<std::string, int, std::string, int, std::string>& a,
                const std::tuple<std::string, int, std::string, int, std::string>& b) {
                int ra = rank_key(a), rb = rank_key(b);
                if (ra != rb) return ra < rb;
                const std::string& spa = std::get<4>(a);
                const std::string& spb = std::get<4>(b);
                if (spa != spb) return spa < spb;
                return std::get<1>(a) < std::get<1>(b);
            });

        int total = (int)usages.size();
        if (total == 0) {
            std::cout << "No call-site examples found for '" << symbol << "'." << "\n";
            return;
        }
        int slice_end = std::min(std::min(limit, total), HARD_CAP);
        // PARITY: emit ONLY the rendered snippets (no total_estimate / next_cursor).
        for (int i = 0; i < slice_end; ++i) {
            if (i > 0) std::cout << "\n\n";
            std::cout << "--- " << std::get<0>(usages[i]) << ":" << std::get<1>(usages[i]) << " ---\n"
                      << std::get<2>(usages[i]);
        }
        std::cout << "\n";
    }

private:
    // Mirror of SourceErgo._derive_module_from_path.
    static std::string derive_module_from_path(const std::string& file_path) {
        std::string p = file_path;
        std::replace(p.begin(), p.end(), '\\', '/');
        auto src = p.rfind("/Source/");
        if (src == std::string::npos) return "";
        std::string after = p.substr(src + std::strlen("/Source/"));
        auto slash = after.find('/');
        return (slash == std::string::npos) ? "" : after.substr(0, slash);
    }

    // Mirror of SourceErgo._strip_block_comments. `in_block` threads the
    // in-block-comment flag across lines.
    static std::string strip_block_comments(const std::string& line, bool& in_block) {
        std::string result;
        std::string work = line;
        while (true) {
            if (in_block) {
                auto end = work.find("*/");
                if (end == std::string::npos) return result;
                work = work.substr(end + 2);
                in_block = false;
            }
            auto open_idx = work.find("/*");
            if (open_idx == std::string::npos) {
                result += work;
                return result;
            }
            result += work.substr(0, open_idx);
            work = work.substr(open_idx + 2);
            in_block = true;
        }
    }

    // Mirror of SourceErgo._strip_line_comment.
    static std::string strip_line_comment(const std::string& line) {
        auto lc = line.find("//");
        return (lc == std::string::npos) ? line : line.substr(0, lc);
    }

    static std::string py_strip(const std::string& s) {
        return trim_copy(s);
    }

    // Lower-cased basename without final extension, mirroring
    // os.path.splitext(os.path.basename(path.replace("\\","/")))[0].
    static std::string header_base_of(const std::string& file_path) {
        std::string p = file_path;
        std::replace(p.begin(), p.end(), '\\', '/');
        auto slash = p.rfind('/');
        std::string base = (slash == std::string::npos) ? p : p.substr(slash + 1);
        auto dot = base.rfind('.');
        if (dot != std::string::npos && dot != 0) base = base.substr(0, dot);
        return base;
    }

    static bool starts_with(const std::string& s, const std::string& prefix) {
        return s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0;
    }

    static bool ends_with(const std::string& s, const std::string& suffix) {
        return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
    }

    // Match the include regex: ^\s*#\s*include\s+["<]([^">]+)[">]. Returns the
    // captured path, or empty when no match.
    static std::string match_include(const std::string& code) {
        size_t i = 0, n = code.size();
        while (i < n && std::isspace(static_cast<unsigned char>(code[i]))) ++i;
        if (i >= n || code[i] != '#') return "";
        ++i;
        while (i < n && std::isspace(static_cast<unsigned char>(code[i]))) ++i;
        const std::string kw = "include";
        if (code.compare(i, kw.size(), kw) != 0) return "";
        i += kw.size();
        // \s+ : require at least one whitespace
        if (i >= n || !std::isspace(static_cast<unsigned char>(code[i]))) return "";
        while (i < n && std::isspace(static_cast<unsigned char>(code[i]))) ++i;
        if (i >= n || (code[i] != '"' && code[i] != '<')) return "";
        ++i;
        std::string captured;
        while (i < n && code[i] != '"' && code[i] != '>') {
            captured += code[i];
            ++i;
        }
        if (i >= n) return "";  // unterminated; regex would not match the closing class
        if (captured.empty()) return "";  // [^">]+ requires at least one char
        return captured;
    }

    // Match the declaration regex:
    // ^\s*(?:class|struct)\s+(?:[A-Z][A-Z0-9_]*_API\s+)?([A-Za-z_][A-Za-z0-9_]*)
    // Returns the captured type name, or empty when no match.
    static std::string match_decl(const std::string& code) {
        size_t i = 0, n = code.size();
        while (i < n && std::isspace(static_cast<unsigned char>(code[i]))) ++i;
        const char* kws[] = {"class", "struct"};
        size_t kwlen = 0;
        bool matched_kw = false;
        for (const char* kw : kws) {
            size_t kl = std::strlen(kw);
            if (code.compare(i, kl, kw) == 0) { kwlen = kl; matched_kw = true; break; }
        }
        if (!matched_kw) return "";
        i += kwlen;
        // \s+ : require at least one whitespace
        if (i >= n || !std::isspace(static_cast<unsigned char>(code[i]))) return "";
        while (i < n && std::isspace(static_cast<unsigned char>(code[i]))) ++i;
        // optional API macro: [A-Z][A-Z0-9_]*_API\s+
        size_t save = i;
        if (i < n && code[i] >= 'A' && code[i] <= 'Z') {
            size_t j = i + 1;
            while (j < n && ((code[j] >= 'A' && code[j] <= 'Z') ||
                             (code[j] >= '0' && code[j] <= '9') || code[j] == '_')) ++j;
            // token must end in "_API" and be followed by whitespace
            std::string token = code.substr(i, j - i);
            if (ends_with(token, "_API") && j < n &&
                std::isspace(static_cast<unsigned char>(code[j]))) {
                i = j;
                while (i < n && std::isspace(static_cast<unsigned char>(code[i]))) ++i;
            } else {
                i = save;
            }
        }
        // ([A-Za-z_][A-Za-z0-9_]*)
        if (i >= n) return "";
        char c0 = code[i];
        if (!((c0 >= 'A' && c0 <= 'Z') || (c0 >= 'a' && c0 <= 'z') || c0 == '_')) return "";
        std::string name;
        name += c0;
        ++i;
        while (i < n && ((code[i] >= 'A' && code[i] <= 'Z') || (code[i] >= 'a' && code[i] <= 'z') ||
                         (code[i] >= '0' && code[i] <= '9') || code[i] == '_')) {
            name += code[i];
            ++i;
        }
        return name;
    }

    // Match \b([A-Z][A-Z0-9_]*_API)\b anywhere in code (api_re.search).
    static bool has_api_macro_token(const std::string& code) {
        size_t n = code.size();
        for (size_t i = 0; i < n; ++i) {
            // \b before [A-Z]: previous char is not a word char
            if (i > 0 && is_word_char(code[i - 1])) continue;
            if (!(code[i] >= 'A' && code[i] <= 'Z')) continue;
            size_t j = i + 1;
            while (j < n && ((code[j] >= 'A' && code[j] <= 'Z') ||
                             (code[j] >= '0' && code[j] <= '9') || code[j] == '_')) ++j;
            std::string token = code.substr(i, j - i);
            if (!ends_with(token, "_API")) continue;
            // \b after: next char is not a word char
            if (j < n && is_word_char(code[j])) continue;
            return true;
        }
        return false;
    }

public:
    // item 7: lint_header
    void lint_header(const Args& args) {
        std::string file_path = args.positional.empty() ? args.opt("file_path") : args.positional[0];
        std::vector<std::string> lines;
        if (!read_lines_strip_nl(file_path, lines)) {
            std::cerr << "Could not read header file: " << file_path << std::endl;
            std::exit(1);
        }

        std::string module = derive_module_from_path(file_path);
        std::string api_macro;
        if (!module.empty()) {
            std::string up = module;
            std::transform(up.begin(), up.end(), up.begin(),
                           [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
            api_macro = up + "_API";
        }

        bool has_gen_body = false;
        bool any_reflected = false;
        int gen_body_line = 0;
        int last_inc_line = 0;
        std::string last_inc_path;
        int gen_h_line = 0;
        std::string gen_h_path;
        // (decl_line, name, has_api)
        std::vector<std::tuple<int, std::string, bool>> reflected;
        bool pending = false;

        // (rule_id, line, message, severity)
        std::vector<std::tuple<std::string, int, std::string, std::string>> findings;
        bool in_block = false;
        for (size_t idx = 0; idx < lines.size(); ++idx) {
            int i = (int)idx;
            std::string code = strip_line_comment(strip_block_comments(lines[idx], in_block));
            std::string trimmed = py_strip(code);
            if (trimmed.empty()) continue;
            std::string inc = match_include(code);
            if (!inc.empty()) {
                last_inc_line = i + 1;
                last_inc_path = inc;
                if (ends_with(inc, ".generated.h")) {
                    gen_h_line = i + 1;
                    gen_h_path = inc;
                }
                continue;
            }
            if (trimmed.find("GENERATED_BODY") != std::string::npos ||
                trimmed.find("GENERATED_UCLASS_BODY") != std::string::npos) {
                has_gen_body = true;
                if (gen_body_line == 0) gen_body_line = i + 1;
            }
            if (starts_with(trimmed, "UCLASS") || starts_with(trimmed, "USTRUCT") ||
                starts_with(trimmed, "UENUM") || starts_with(trimmed, "UINTERFACE")) {
                any_reflected = true;
                if (starts_with(trimmed, "UCLASS")) pending = true;
            }
            if (pending) {
                std::string dm = match_decl(code);
                if (!dm.empty()) {
                    bool has_api = has_api_macro_token(code);
                    reflected.emplace_back(i + 1, dm, has_api);
                    pending = false;
                }
            }
            // Invalid-specifier rule SKIPPED offline (empty vocabulary).
        }

        // (a) missing GENERATED_BODY.
        if (any_reflected && !has_gen_body) {
            findings.emplace_back("missing_generated_body",
                reflected.empty() ? 0 : std::get<0>(reflected[0]),
                "Reflected type (UCLASS/USTRUCT) is missing a GENERATED_BODY() macro.",
                "error");
        }
        // (b) generated.h not last.
        if (gen_h_line != 0 && last_inc_line != 0 && gen_h_line != last_inc_line) {
            findings.emplace_back("generated_h_not_last", gen_h_line,
                "'*.generated.h' must be the LAST #include (an include at line "
                + std::to_string(last_inc_line) + " follows it: \"" + last_inc_path + "\").",
                "error");
        } else if (any_reflected && has_gen_body && gen_h_line == 0) {
            findings.emplace_back("missing_generated_h_include", gen_body_line,
                "Reflected type uses GENERATED_BODY() but no '*.generated.h' include is present (must be last).",
                "error");
        }
        // (d) missing <MODULE>_API.
        std::string header_base = header_base_of(file_path);
        for (auto& rf : reflected) {
            int decl_line = std::get<0>(rf);
            const std::string& name = std::get<1>(rf);
            bool has_api = std::get<2>(rf);
            if (!api_macro.empty() && !has_api) {
                findings.emplace_back("missing_api_macro", decl_line,
                    "UCLASS-declared type '" + name + "' is missing the '" + api_macro
                    + "' export macro (class " + name + " ...).",
                    "warning");
            }
        }
        // (c) generated.h name mismatch.
        if (gen_h_line != 0 && !header_base.empty() && ends_with(gen_h_path, ".generated.h")) {
            std::string norm = gen_h_path;
            std::replace(norm.begin(), norm.end(), '\\', '/');
            auto slash = norm.rfind('/');
            std::string gen_base = (slash == std::string::npos) ? norm : norm.substr(slash + 1);
            gen_base = gen_base.substr(0, gen_base.size() - std::strlen(".generated.h"));
            if (!gen_base.empty() && gen_base != header_base) {
                findings.emplace_back("generated_h_name_mismatch", gen_h_line,
                    "'" + gen_base + ".generated.h' does not match the header file name '"
                    + header_base + ".h' -- the GENERATED_BODY pairing requires \""
                    + header_base + ".generated.h\".",
                    "error");
            }
        }
        // (e) UPROPERTY/UFUNCTION in a non-reflected file.
        if (!any_reflected) {
            bool in_block2 = false;
            for (size_t idx = 0; idx < lines.size(); ++idx) {
                std::string trimmed = py_strip(strip_line_comment(strip_block_comments(lines[idx], in_block2)));
                if (starts_with(trimmed, "UPROPERTY") || starts_with(trimmed, "UFUNCTION")) {
                    findings.emplace_back("reflected_member_in_non_reflected_type", (int)idx + 1,
                        "UPROPERTY/UFUNCTION found but the file declares no reflected type "
                        "(UCLASS/USTRUCT) -- the macro will not be processed by UHT.",
                        "error");
                }
            }
        }

        std::stable_sort(findings.begin(), findings.end(),
            [](const std::tuple<std::string, int, std::string, std::string>& a,
               const std::tuple<std::string, int, std::string, std::string>& b) {
                if (std::get<1>(a) != std::get<1>(b)) return std::get<1>(a) < std::get<1>(b);
                return std::get<0>(a) < std::get<0>(b);
            });

        if (findings.empty()) {
            std::cout << "Clean -- no lint findings." << "\n";
            return;
        }
        for (size_t i = 0; i < findings.size(); ++i) {
            if (i > 0) std::cout << "\n";
            std::cout << "[" << std::get<3>(findings[i]) << "] L" << std::get<1>(findings[i])
                      << " (" << std::get<0>(findings[i]) << "): " << std::get<2>(findings[i]);
        }
        std::cout << "\n";
    }

    // item 9: generate_class_stub
    void generate_class_stub(const Args& args) {
        std::string parent = args.positional.size() > 0 ? args.positional[0] : args.opt("parent");
        std::string class_name = args.positional.size() > 1 ? args.positional[1] : args.opt("class_name");
        std::string module = args.positional.size() > 2 ? args.positional[2] : args.opt("module");
        if (parent.empty() || class_name.empty() || module.empty()) {
            std::cerr << "generate_class_stub requires parent, class_name, and module" << std::endl;
            std::exit(1);
        }

        Rows sym = resolve_symbol_row(parent);
        if (sym.empty()) {
            std::cerr << "Parent class '" << parent << "' not found in the source index. "
                         "Run source.trigger_reindex if it is a project type." << std::endl;
            std::exit(1);
        }

        if (!(parent[0] == 'U' || parent[0] == 'A')) {
            std::cerr << "Parent '" << parent << "' is not a UCLASS-derived type. generate_class_stub v1 "
                         "supports UCLASS-derived parents only (no USTRUCT/UENUM/UINTERFACE)." << std::endl;
            std::exit(1);
        }

        auto allrows = query(db,
            "SELECT s.file_id, f.path FROM symbols s JOIN files f ON f.id = s.file_id WHERE s.name = ?",
            {parent});
        std::string parent_path = get_file_path(sym[0].get_int("file_id"));
        for (auto& r : allrows) {
            std::string p = r.get("path");
            if (p.size() >= 2 && p.substr(p.size() - 2) == ".h") {
                parent_path = p;
                break;
            }
        }
        std::string parent_module = derive_module_from_path(parent_path);
        IncludeInfo parent_info = derive_include_path(parent_path, parent_module);
        std::string parent_include = parent_info.include;

        // Constructor convention.
        auto ctors = query(db,
            "SELECT signature FROM symbols WHERE name = ? AND kind = 'function'", {parent});
        bool saw_any = false, saw_plain = false, saw_obj = false;
        std::string ctor_open = parent + "(";
        for (auto& r : ctors) {
            std::string s = r.get("signature");
            if (s.empty() || s.find(ctor_open) == std::string::npos) continue;
            saw_any = true;
            if (s.find("FObjectInitializer") != std::string::npos) saw_obj = true;
            else saw_plain = true;
        }
        bool needs_obj_init = saw_any && saw_obj && !saw_plain;

        std::string api_macro;
        {
            std::string up = module;
            std::transform(up.begin(), up.end(), up.begin(),
                           [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
            api_macro = up + "_API";
        }

        std::string file_base;
        if (class_name.size() >= 2 && (class_name[0] == 'U' || class_name[0] == 'A') &&
            std::isupper(static_cast<unsigned char>(class_name[1]))) {
            file_base = class_name.substr(1);
        } else {
            file_base = class_name;
        }
        std::string gen_inc = file_base + ".generated.h";

        std::vector<std::string> h;
        h.push_back("#pragma once");
        h.push_back("");
        h.push_back("#include \"CoreMinimal.h\"");
        if (!parent_include.empty()) h.push_back("#include \"" + parent_include + "\"");
        h.push_back("#include \"" + gen_inc + "\"");
        h.push_back("");
        h.push_back("UCLASS()");
        h.push_back("class " + api_macro + " " + class_name + " : public " + parent);
        h.push_back("{");
        h.push_back("\tGENERATED_BODY()");
        h.push_back("");
        h.push_back("public:");
        if (needs_obj_init) {
            h.push_back("\t" + class_name + "(const FObjectInitializer& ObjectInitializer);");
        } else {
            h.push_back("\t" + class_name + "();");
        }
        h.push_back("};");
        h.push_back("");
        std::string header_text;
        for (size_t i = 0; i < h.size(); ++i) {
            if (i > 0) header_text += "\n";
            header_text += h[i];
        }

        std::vector<std::string> c;
        c.push_back("#include \"" + file_base + ".h\"");
        c.push_back("");
        if (needs_obj_init) {
            c.push_back(class_name + "::" + class_name + "(const FObjectInitializer& ObjectInitializer)");
            c.push_back("\t: Super(ObjectInitializer)");
            c.push_back("{");
            c.push_back("}");
        } else {
            c.push_back(class_name + "::" + class_name + "()");
            c.push_back("{");
            c.push_back("}");
        }
        c.push_back("");
        std::string cpp_text;
        for (size_t i = 0; i < c.size(); ++i) {
            if (i > 0) cpp_text += "\n";
            cpp_text += c[i];
        }

        std::cout << "// === " << file_base << ".h ===\n" << header_text
                  << "\n// === " << file_base << ".cpp ===\n" << cpp_text << "\n";
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
        int limit = clamp_int(args.opt_int("limit", 50), 1, 1000);
        bool include_content = args.opt_bool("include_content", true);

        json results = json::array();

        auto add_matches = [&](const std::string& fts_table, const std::string& sql, const std::string& match_source) {
            if (!object_exists(db, "table", fts_table)) return;
            auto rows = query(db,
                sql + " LIMIT " + std::to_string(limit) + ";",
                {q});
            for (auto& r : rows) {
                results.push_back({
                    {"asset_path", r.get("package_path")},
                    {"asset_name", r.get("asset_name")},
                    {"asset_class", r.get("asset_class")},
                    {"module_name", r.get("module_name")},
                    {"match_context", r.get("ctx")},
                    {"match_source", match_source},
                    {"match_table", r.get("match_table")},
                    {"match_field", r.get("match_field")},
                    {"match_object_path", r.get("match_object_path")},
                    {"match_value", r.get("match_value")},
                    {"rank", r.get_double("rank")},
                });
            }
        };

        add_matches(
            "fts_assets",
            "SELECT a.package_path, a.asset_name, a.asset_class, a.module_name, "
            "snippet(fts_assets, -1, '>>>', '<<<', '...', 32) AS ctx, f.rank AS rank, "
            "'assets' AS match_table, 'asset' AS match_field, a.package_path AS match_object_path, a.asset_name AS match_value "
            "FROM fts_assets f JOIN assets a ON a.id = f.rowid "
            "WHERE fts_assets MATCH ? ORDER BY f.rank",
            "asset");

        add_matches(
            "fts_nodes",
            "SELECT a.package_path, a.asset_name, a.asset_class, a.module_name, "
            "snippet(fts_nodes, -1, '>>>', '<<<', '...', 32) AS ctx, f.rank AS rank, "
            "'nodes' AS match_table, n.node_name AS match_field, n.node_name AS match_object_path, n.node_class AS match_value "
            "FROM fts_nodes f JOIN nodes n ON n.id = f.rowid JOIN assets a ON a.id = n.asset_id "
            "WHERE fts_nodes MATCH ? ORDER BY f.rank",
            "node");

        if (include_content) {
            add_matches(
                "fts_variables",
                "SELECT a.package_path, a.asset_name, a.asset_class, a.module_name, "
                "snippet(fts_variables, -1, '>>>', '<<<', '...', 32) AS ctx, f.rank AS rank, "
                "'variables' AS match_table, v.var_name AS match_field, v.var_name AS match_object_path, v.default_value AS match_value "
                "FROM fts_variables f JOIN variables v ON v.id = f.rowid JOIN assets a ON a.id = v.asset_id "
                "WHERE fts_variables MATCH ? ORDER BY f.rank",
                "variable");

            add_matches(
                "fts_parameters",
                "SELECT a.package_path, a.asset_name, a.asset_class, a.module_name, "
                "snippet(fts_parameters, -1, '>>>', '<<<', '...', 32) AS ctx, f.rank AS rank, "
                "'parameters' AS match_table, p.param_name AS match_field, p.param_name AS match_object_path, p.default_value AS match_value "
                "FROM fts_parameters f JOIN parameters p ON p.id = f.rowid JOIN assets a ON a.id = p.asset_id "
                "WHERE fts_parameters MATCH ? ORDER BY f.rank",
                "parameter");

            add_matches(
                "fts_datatable_rows",
                "SELECT a.package_path, a.asset_name, a.asset_class, a.module_name, "
                "snippet(fts_datatable_rows, -1, '>>>', '<<<', '...', 32) AS ctx, f.rank AS rank, "
                "'datatable_rows' AS match_table, 'row_name' AS match_field, r.row_name AS match_object_path, r.row_name AS match_value "
                "FROM fts_datatable_rows f JOIN datatable_rows r ON r.id = f.rowid JOIN assets a ON a.id = r.asset_id "
                "WHERE fts_datatable_rows MATCH ? ORDER BY f.rank",
                "datatable_row");

            add_matches(
                "fts_actors",
                "SELECT a.package_path, a.asset_name, a.asset_class, a.module_name, "
                "snippet(fts_actors, -1, '>>>', '<<<', '...', 32) AS ctx, f.rank AS rank, "
                "'actors' AS match_table, ac.actor_label AS match_field, ac.actor_name AS match_object_path, ac.actor_class AS match_value "
                "FROM fts_actors f JOIN actors ac ON ac.id = f.rowid JOIN assets a ON a.id = ac.asset_id "
                "WHERE fts_actors MATCH ? ORDER BY f.rank",
                "actor");

            add_matches(
                "fts_asset_search_values",
                "SELECT a.package_path, a.asset_name, a.asset_class, a.module_name, "
                "snippet(fts_asset_search_values, -1, '>>>', '<<<', '...', 32) AS ctx, f.rank AS rank, "
                "'asset_search_values' AS match_table, sv.field_name AS match_field, sv.object_path AS match_object_path, sv.value_text AS match_value "
                "FROM fts_asset_search_values f JOIN asset_search_values sv ON sv.id = f.rowid JOIN assets a ON a.id = sv.asset_id "
                "WHERE fts_asset_search_values MATCH ? ORDER BY f.rank",
                "supplemental_value");
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
            "dependencies", "actors", "tags", "tag_references", "configs",
            "cpp_symbols", "datatable_rows", "asset_search_values", "meta"
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
        for (const char* t : {"assets", "nodes", "connections", "variables", "parameters", "dependencies", "actors", "tags", "tag_references", "configs", "cpp_symbols", "datatable_rows", "asset_search_values", "meta"})
            check(std::string("table:") + t, object_exists(db, "table", t), object_exists(db, "table", t) ? std::string("table ") + t + " present" : std::string("missing table ") + t);
        for (const char* f : {"fts_assets", "fts_nodes", "fts_variables", "fts_parameters", "fts_datatable_rows", "fts_actors", "fts_asset_search_values"})
            check(std::string("fts:") + f, object_exists(db, "table", f), object_exists(db, "table", f) ? std::string("FTS table ") + f + " present" : std::string("missing FTS table ") + f);
        for (const char* tr : {
            "fts_assets_ai", "fts_assets_ad", "fts_assets_au",
            "fts_nodes_ai", "fts_nodes_ad", "fts_nodes_au",
            "fts_variables_ai", "fts_variables_ad", "fts_variables_au",
            "fts_parameters_ai", "fts_parameters_ad", "fts_parameters_au",
            "fts_datatable_rows_ai", "fts_datatable_rows_ad", "fts_datatable_rows_au",
            "fts_actors_ai", "fts_actors_ad", "fts_actors_au",
            "fts_asset_search_values_ai", "fts_asset_search_values_ad", "fts_asset_search_values_au"})
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
        int64_t vcnt = count_rows(db, "SELECT COUNT(*) FROM variables;");
        int64_t fvcnt = count_rows(db, "SELECT COUNT(*) FROM fts_variables;");
        int64_t pcnt = count_rows(db, "SELECT COUNT(*) FROM parameters;");
        int64_t fpcnt = count_rows(db, "SELECT COUNT(*) FROM fts_parameters;");
        int64_t dtcnt = count_rows(db, "SELECT COUNT(*) FROM datatable_rows;");
        int64_t fdtcnt = count_rows(db, "SELECT COUNT(*) FROM fts_datatable_rows;");
        int64_t actorcnt = count_rows(db, "SELECT COUNT(*) FROM actors;");
        int64_t factorcnt = count_rows(db, "SELECT COUNT(*) FROM fts_actors;");
        int64_t svcnt = count_rows(db, "SELECT COUNT(*) FROM asset_search_values;");
        int64_t fsvcnt = count_rows(db, "SELECT COUNT(*) FROM fts_asset_search_values;");
        check("fts:assets_row_parity", acnt == facnt, "assets=" + std::to_string(acnt) + " fts_assets=" + std::to_string(facnt) + (acnt == facnt ? "" : " (mismatch -> project.repair_fts)"));
        check("fts:nodes_row_parity", ncnt == fncnt, "nodes=" + std::to_string(ncnt) + " fts_nodes=" + std::to_string(fncnt) + (ncnt == fncnt ? "" : " (mismatch -> project.repair_fts)"));
        check("fts:variables_row_parity", vcnt == fvcnt, "variables=" + std::to_string(vcnt) + " fts_variables=" + std::to_string(fvcnt) + (vcnt == fvcnt ? "" : " (mismatch -> project.repair_fts)"));
        check("fts:parameters_row_parity", pcnt == fpcnt, "parameters=" + std::to_string(pcnt) + " fts_parameters=" + std::to_string(fpcnt) + (pcnt == fpcnt ? "" : " (mismatch -> project.repair_fts)"));
        check("fts:datatable_rows_row_parity", dtcnt == fdtcnt, "datatable_rows=" + std::to_string(dtcnt) + " fts_datatable_rows=" + std::to_string(fdtcnt) + (dtcnt == fdtcnt ? "" : " (mismatch -> project.repair_fts)"));
        check("fts:actors_row_parity", actorcnt == factorcnt, "actors=" + std::to_string(actorcnt) + " fts_actors=" + std::to_string(factorcnt) + (actorcnt == factorcnt ? "" : " (mismatch -> project.repair_fts)"));
        check("fts:asset_search_values_row_parity", svcnt == fsvcnt, "asset_search_values=" + std::to_string(svcnt) + " fts_asset_search_values=" + std::to_string(fsvcnt) + (svcnt == fsvcnt ? "" : " (mismatch -> project.repair_fts)"));
        root["schema"] = {{"schema_version", schema_ver}, {"journal_mode", scalar_str(db, "PRAGMA journal_mode;")}};
        if (include_counts) root["row_counts"] = {
            {"assets", acnt},
            {"nodes", ncnt},
            {"variables", vcnt},
            {"parameters", pcnt},
            {"datatable_rows", dtcnt},
            {"actors", actorcnt},
            {"asset_search_values", svcnt},
            {"dependencies", count_rows(db, "SELECT COUNT(*) FROM dependencies;")}
        };
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
        if (t == "all" || t == "variables") tables.push_back("fts_variables");
        if (t == "all" || t == "parameters") tables.push_back("fts_parameters");
        if (t == "all" || t == "datatable_rows") tables.push_back("fts_datatable_rows");
        if (t == "all" || t == "actors") tables.push_back("fts_actors");
        if (t == "all" || t == "asset_search_values") tables.push_back("fts_asset_search_values");
        if (tables.empty()) {
            root["status"] = "error";
            root["summary"] = "Unknown target '" + t + "' (expected all|assets|nodes|variables|parameters|datatable_rows|actors|asset_search_values)";
            add_next(root, {"project.health"});
            return root;
        }
        std::vector<std::string> missing;
        for (const auto& table : tables) {
            if (!object_exists(db, "table", table)) missing.push_back(table);
        }
        if (!missing.empty()) {
            std::string joined;
            for (const auto& table : missing) {
                if (!joined.empty()) joined += ", ";
                joined += table;
                root["warnings"].push_back("missing FTS table " + table);
            }
            root["status"] = "error";
            root["summary"] = "Missing selected FTS table(s): " + joined + ". Open the editor to run schema migration or run monolith_reindex after updating.";
            root["before"] = json::object();
            root["after"] = json::object();
            add_next(root, {"project.health", "monolith_reindex"});
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
// Reflection Intelligence shared infra — base64, cursor codec, filter hash
//
// FULL OFFLINE PARITY (20 RI actions across cppreflect/network/decision/risk).
// Authoritative reference: Docs/plans/offline-parity-spec.md. Every field NAME,
// TYPE (bool vs int!), ORDER, wrapper shape, ORDER BY, and default filter is
// pinned by that spec; a downstream HARD-GATE parity test byte-diffs this exe's
// output against the Python sibling (Scripts/monolith_offline.py) and the live
// server. KEY ORDER MATTERS — UE FJsonObject serializes in insertion order, so
// all RI JSON below uses nlohmann::ordered_json (insertion-order preserving),
// NOT the default json (which sorts keys alphabetically). The source.* and
// project.* actions above are intentionally left on default json — out of scope.
// ============================================================

using ojson = nlohmann::ordered_json;

static std::string to_lower_copy(std::string s) {
    return lower_copy(s);
}

// PARITY_SPEC_REV — the agreed parity-rev string. Both this exe and the Python
// sibling mirror this literal so an external parity guard can assert exe and py
// report the same rev (and thus implement the same spec snapshot). Bump in BOTH
// implementers whenever offline-parity-spec.md changes shape.
static const char* PARITY_SPEC_REV = "2026-05-29.1";

// SOURCE_HASH — optionally injected by the build (e.g. /DSOURCE_HASH="...") so
// the orchestrator's staleness guard can detect a stale exe vs source. Defaults
// to "dev" when not injected.
#ifndef SOURCE_HASH
#define SOURCE_HASH "dev"
#endif

// ---- Standard base64 (RFC 4648, '+' '/' '=' padding) — matches UE FBase64. ----
static const char kB64Chars[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static std::string base64_encode(const std::string& in) {
    std::string out;
    out.reserve(((in.size() + 2) / 3) * 4);
    size_t i = 0;
    while (i + 2 < in.size()) {
        unsigned n = (unsigned char)in[i] << 16 | (unsigned char)in[i + 1] << 8 | (unsigned char)in[i + 2];
        out += kB64Chars[(n >> 18) & 63];
        out += kB64Chars[(n >> 12) & 63];
        out += kB64Chars[(n >> 6) & 63];
        out += kB64Chars[n & 63];
        i += 3;
    }
    size_t rem = in.size() - i;
    if (rem == 1) {
        unsigned n = (unsigned char)in[i] << 16;
        out += kB64Chars[(n >> 18) & 63];
        out += kB64Chars[(n >> 12) & 63];
        out += "==";
    } else if (rem == 2) {
        unsigned n = (unsigned char)in[i] << 16 | (unsigned char)in[i + 1] << 8;
        out += kB64Chars[(n >> 18) & 63];
        out += kB64Chars[(n >> 12) & 63];
        out += kB64Chars[(n >> 6) & 63];
        out += '=';
    }
    return out;
}

// Returns false on malformed input (used to flag INVALID_CURSOR).
static bool base64_decode(const std::string& in, std::string& out) {
    auto val = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+') return 62;
        if (c == '/') return 63;
        return -1;
    };
    out.clear();
    int buf = 0, bits = 0;
    for (char c : in) {
        if (c == '=') break;
        if (c == '\r' || c == '\n') continue;
        int v = val(c);
        if (v < 0) return false;
        buf = (buf << 6) | v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out += (char)((buf >> bits) & 0xFF);
        }
    }
    return true;
}

// ---- UE-style string + combine hashing (for ComputeFilterHash). ----
//
// The HARD-GATE parity guard byte-diffs `next_cursor`, which embeds the filter
// hash `qh`. So these MUST be a step-for-step port of the live UE hash funcs
// (validated against the live editor in offline-byte-parity-addendum.md §3):
//   GetTypeHash(const FString&) -> FCrc::Strihash_DEPRECATED(Len, *S)  (UnrealString.h.inl:2176)
//   GetTypeHash(double)         -> (uint32)v + ((uint32)(v>>32))*23
//   HashCombine(A, C)           -> full Bob-Jenkins mix (TypeHash.h:36-52)
//
// ROOT-CAUSE FIX (addendum §3): GetTypeHash(FString) is NOT FCrc::StrCrc32
// (reflected CRC-32). It is FCrc::Strihash_DEPRECATED — a CASE-INSENSITIVE hash
// over WIDECHAR using a FORWARD (non-reflected) CRC-32 table (CRCTable_DEPRECATED,
// poly 0x04C11DB7), processing each UTF-16 code unit as lo-byte THEN hi-byte,
// with ToUpper applied per char. The prior StrCrc32 port produced wrong `qh`
// values. ue_hash_combine and ue_hash_double were already correct and are kept.

// FCrc::CRCTable_DEPRECATED[256] (Crc.cpp:40) — the FORWARD CRC-32 table:
//   c = i << 24; repeat 8x: c = (c<<1) ^ (poly if top bit set, else 0).
// Sanity (addendum §3): t[0]=0x00000000, t[1]=0x04C11DB7, t[255]=0xB1F740B4.
static const uint32_t (&strihash_table())[256] {
    static uint32_t t[256];
    static bool init = false;
    if (!init) {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = (i << 24) & 0xFFFFFFFFu;
            for (int k = 0; k < 8; ++k)
                c = (c & 0x80000000u) ? (((c << 1) ^ 0x04C11DB7u) & 0xFFFFFFFFu)
                                      : ((c << 1) & 0xFFFFFFFFu);
            t[i] = c;
        }
        init = true;
    }
    return t;
}

// ASCII ToUpper over a 16-bit code unit (UE FChar::ToUpper for BMP a-z; the RI
// filter inputs — class names, paths, stringified ints, "0"/"1"/"__stale__" —
// are all ASCII, so case-folding only matters in the a-z range here).
static uint16_t widechar_to_upper(uint16_t cu) {
    if (cu >= (uint16_t)'a' && cu <= (uint16_t)'z') return (uint16_t)(cu - 32);
    return cu;
}

// FCrc::Strihash_DEPRECATED(Len, *S) over WIDECHAR (Crc.h:195) ==
// GetTypeHash(const FString&). Case-insensitive (ToUpper per char), each UTF-16
// code unit fed lo-byte THEN hi-byte through the forward table. Empty -> 0.
// ASCII bytes map 1:1 to UTF-16 code units (value == byte), matching the live
// FString contents for all RI filter inputs. Validated qh values in addendum §3:
// Strihash("ALeviathanCharacterBase") chain -> 2954246778; Strihash("Blueprintable")
// -> 1087131954 (== "blueprintable", proving case-insensitivity).
static uint32_t ue_str_crc32(const std::string& s) {
    const auto& t = strihash_table();
    uint32_t h = 0;
    for (unsigned char b : s) {
        uint16_t cu = widechar_to_upper((uint16_t)b);
        uint16_t lo = cu & 0xFF;
        h = ((h >> 8) & 0x00FFFFFFu) ^ t[(h ^ lo) & 0xFFu];
        uint16_t hi = (cu >> 8) & 0xFF;
        h = ((h >> 8) & 0x00FFFFFFu) ^ t[(h ^ hi) & 0xFFu];
    }
    return h & 0xFFFFFFFFu;
}

// UE HashCombine (TypeHash.h:36-52) — the full Bob-Jenkins mixing variant
// (NOT HashCombineFast). Ported step-for-step from the Python sibling's
// _hash_combine(a, c).
static uint32_t ue_hash_combine(uint32_t a, uint32_t c) {
    uint32_t b = 0x9e3779b9u;
    a += b;

    a -= b; a -= c; a ^= (c >> 13);
    b -= c; b -= a; b ^= (a << 8);
    c -= a; c -= b; c ^= (b >> 13);
    a -= b; a -= c; a ^= (c >> 12);
    b -= c; b -= a; b ^= (a << 16);
    c -= a; c -= b; c ^= (b >> 5);
    a -= b; a -= c; a ^= (c >> 3);
    b -= c; b -= a; b ^= (a << 10);
    c -= a; c -= b; c ^= (b >> 15);
    return c;
}

// GetTypeHash(double) -> GetTypeHash(*(uint64*)&d) -> (uint32)v + ((uint32)(v>>32))*23.
static uint32_t ue_hash_double(double d) {
    uint64_t bits;
    std::memcpy(&bits, &d, sizeof(bits));
    return (uint32_t)bits + (uint32_t)(bits >> 32) * 23u;
}

// ComputeFilterHash over an ordered list of string parts (initializer-list form).
static uint32_t compute_filter_hash(const std::vector<std::string>& parts) {
    uint32_t h = 0;
    for (const auto& p : parts)
        h = ue_hash_combine(h, ue_str_crc32(p));
    return h;
}

// ---- Cursor codec. Encodes {qh,p,tc} as UE pretty-printed JSON, then base64. ----
// CRITICAL (addendum §2): the live cursor is FBase64::Encode of the DEFAULT
// TJsonWriterFactory<> output — pretty-print policy = CRLF newlines + single-TAB
// indent + one space after each colon, key order qh,p,tc. The exact decoded
// bytes are:
//     {\r\n\t"qh": <qh>,\r\n\t"p": <p>,\r\n\t"tc": <tc>\r\n}
// nlohmann's dump() cannot reproduce this exact whitespace (no CRLF, no
// space-after-colon-without-newline control), so the byte sequence is HAND-BUILT
// here, then base64-encoded. Integer values are plain (qh/p/tc are integral and
// in-range, so WriteDouble("%.17g") yields the same digits as plain int — see
// addendum §2 "Numbers inside the cursor are also %.17g"). tc may be -1.
// Verified samples (addendum §2): list_uproperties qh=2954246778,p=1,tc=17 ->
//   ew0KCSJxaCI6IDI5NTQyNDY3NzgsDQoJInAiOiAxLA0KCSJ0YyI6IDE3DQp9
static std::string encode_cursor(uint32_t query_hash, int32_t page, int32_t cached_total) {
    std::string body;
    body += "{\r\n\t\"qh\": ";
    body += std::to_string((uint32_t)query_hash);
    body += ",\r\n\t\"p\": ";
    body += std::to_string((int32_t)page);
    body += ",\r\n\t\"tc\": ";
    body += std::to_string((int32_t)cached_total);
    body += "\r\n}";
    return base64_encode(body);
}

// ---- %.17g float-field sentinel mechanism (addendum §1). ----
// Live RI numeric fields are written by UE via SetNumberField -> serialized by
// TJsonPrintPolicy::WriteDouble = FString::Printf(TEXT("%.17g"), Value). nlohmann
// dump() emits shortest-round-trip and offers NO %.17g hook, so for the genuine-
// fractional REAL fields we store a unique SENTINEL STRING in the ojson, then run
// a string pass over the dumped output replacing the QUOTED sentinel with the RAW
// unquoted number. Integer-valued doubles (total_estimate, counts, line numbers,
// unix timestamps) print identically as plain ints, so they are left as-is per
// the addendum ("integer-valued doubles print clean").
//
// REAL columns are stored float32 in the indexer; when read into a C++ double
// they widen. The live server formats THAT widened value. So we MUST float32-
// round-trip (double -> float -> double) before %.17g, e.g. 0.65 -> the float32
// nearest -> widened -> "0.64999997615814209" (== live confidence), NOT the
// double "0.6499999761581421".
//
// The sentinel uses the 0x01 control byte as a fence; 0x01 cannot appear in any
// emitted RI JSON value (all are class/property/path/snippet text + numbers),
// and the quoted form "\x01FLT:<digits>\x01" cannot collide with real data.
static const char kFltSentinelByte = '\x01';

// Produce the sentinel STRING for a genuine-float REAL field. Caller assigns it
// to an ojson string value; finalize_float_sentinels() later substitutes the raw
// number. Formats %.17g of the RAW double exactly as read from
// sqlite3_column_double (NO float32 round-trip): the stored doubles are already
// correct. confidence is float32-origin so still prints 0.64999997615814209,
// while genuine doubles like normalised_churn print 0.65714285714285714.
static std::string flt_sentinel(double real_value) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.17g", real_value);
    std::string s;
    s += kFltSentinelByte;
    s += "FLT:";
    s += buf;
    s += kFltSentinelByte;
    return s;
}

// After ojson::dump(): replace every  "\x01FLT:<digits>\x01"  (a QUOTED sentinel,
// exactly as nlohmann renders a string value) with  <digits>  (raw JSON number,
// no quotes). nlohmann does not escape 0x01 inside strings with the default dump,
// but to be defensive we also handle the  escaped form. Returns the patched
// string ready for stdout.
static std::string finalize_float_sentinels(const std::string& dumped) {
    std::string out = dumped;
    // Two possible renderings of the fence byte by nlohmann: raw 0x01 or .
    const std::string fences[] = { std::string(1, kFltSentinelByte), "\\u0001" };
    for (const std::string& fence : fences) {
        const std::string open = "\"" + fence + "FLT:";   // opening quote + fence + tag
        const std::string close = fence + "\"";           // fence + closing quote
        size_t pos = 0;
        while ((pos = out.find(open, pos)) != std::string::npos) {
            size_t digits_start = pos + open.size();
            size_t close_pos = out.find(close, digits_start);
            if (close_pos == std::string::npos) break;     // malformed; leave as-is
            std::string digits = out.substr(digits_start, close_pos - digits_start);
            out.replace(pos, (close_pos + close.size()) - pos, digits);
            pos += digits.size();
        }
    }
    return out;
}

// Emit an RI ojson with the float-sentinel finalize pass applied. ALL RI actions
// route their final output through this so the %.17g substitution is uniform.
static void emit_ri(const ojson& out) {
    std::cout << finalize_float_sentinels(out.dump(2)) << std::endl;
}

struct CursorState {
    uint32_t query_hash = 0;
    int32_t page = 0;
    int32_t cached_total = -1;
};

// Returns true on a valid cursor; false sets out_reason to the spec message.
static bool decode_cursor(const std::string& cursor, CursorState& out, std::string& out_reason) {
    std::string jsonStr;
    if (!base64_decode(cursor, jsonStr)) {
        out_reason = "Cursor decode failed; restart pagination without `cursor`.";
        return false;
    }
    ojson o = ojson::parse(jsonStr, nullptr, false);
    if (o.is_discarded() || !o.is_object()) {
        out_reason = "Cursor decode failed; restart pagination without `cursor`.";
        return false;
    }
    if (!o.contains("qh") || !o.contains("p") || !o.contains("tc") ||
        !o["qh"].is_number() || !o["p"].is_number() || !o["tc"].is_number()) {
        out_reason = "Cursor decode failed; restart pagination without `cursor`.";
        return false;
    }
    double qh = o["qh"].get<double>();
    double p = o["p"].get<double>();
    double tc = o["tc"].get<double>();
    if (p < 0 || qh < 0 || qh > 4294967295.0) {
        out_reason = "Cursor decode failed; restart pagination without `cursor`.";
        return false;
    }
    out.query_hash = (uint32_t)qh;
    out.page = (int32_t)p;
    out.cached_total = (int32_t)tc;
    return true;
}

// Emit the INVALID_CURSOR error envelope and return.
static void emit_invalid_cursor(const std::string& reason) {
    ojson out;
    out["success"] = false;
    out["error"] = reason;
    out["error_code"] = "INVALID_CURSOR";
    std::cout << out.dump(2) << std::endl;
}

// Emit a generic missing-required / bad-param error.
static void emit_param_error(const std::string& message) {
    ojson out;
    out["success"] = false;
    out["error"] = message;
    std::cout << out.dump(2) << std::endl;
}

static void emit_invalid_params_error(const std::string& message) {
    ojson out;
    out["success"] = false;
    out["error"] = message;
    out["error_code"] = "INVALID_PARAMS";
    std::cout << out.dump(2) << std::endl;
}

// Emit a missing-table error per spec §0.7.
static void emit_missing_table(const std::string& table) {
    ojson out;
    out["success"] = false;
    out["error"] = table + " not in EngineSource.db. Build the project + rebuild_reflection_index in-editor.";
    std::cout << out.dump(2) << std::endl;
}

// Clamp limit per spec §0.3: default 50, [1,200].
static int clamp_limit(const Args& args) {
    int limit = args.opt_int("limit", 50);
    if (limit < 1) limit = 1;
    if (limit > 200) limit = 200;
    return limit;
}

static bool validate_cppreflect_limit(const Args& args, int& limit) {
    auto it = args.options.find("limit");
    if (it == args.options.end() || it->second.empty()) {
        limit = 50;
        return true;
    }

    try {
        std::size_t parsed = 0;
        limit = std::stoi(it->second, &parsed);
        if (parsed != it->second.size()) {
            emit_invalid_params_error("`limit` must be a number.");
            return false;
        }
    } catch (...) {
        emit_invalid_params_error("`limit` must be a number.");
        return false;
    }

    constexpr int HARD_CAP = 200;
    if (limit < 1 || limit > HARD_CAP) {
        emit_invalid_params_error("`limit` (" + std::to_string(limit) +
                                  ") exceeds the allowed bounds [1, " +
                                  std::to_string(HARD_CAP) + "].");
        return false;
    }
    return true;
}

// COUNT(*) helper.
static int64_t scalar_count(Database& db, const std::string& sql, const std::vector<std::string>& params) {
    auto rows = query(db, sql, params);
    return rows.empty() ? 0 : rows[0].get_int64(rows[0].cols.begin()->first);
}

// Does a table exist?
static bool table_exists(Database& db, const std::string& name) {
    auto rows = query(db,
        "SELECT name FROM sqlite_master WHERE type='table' AND name = ? LIMIT 1", {name});
    return !rows.empty();
}

// ============================================================
// Reflection Intelligence actions (read-only, EngineSource.db reflect_* tables)
//
// Implements all 20 RI actions to Docs/plans/offline-parity-spec.md. The live
// F*QueryAdapter.cpp handlers + *Schema.cpp CREATE TABLE statements are the
// upstream source of truth; the spec distilled them. Offline output is shaped to
// be byte-identical to the Python sibling and the live server.
// ============================================================

class ReflectionActions {
    Database db;

    // Resolve pagination page from an optional cursor. On a cursor present:
    // decode + validate filter hash; on mismatch/decode-fail emit INVALID_CURSOR
    // and signal abort via return false. On no cursor: page 0. CachedTotal is
    // carried out for actions that forward it (unused for total-less actions).
    bool resolve_page(const Args& args, uint32_t expected_hash,
                      int32_t& out_page, int32_t& out_cached_total, bool& has_cursor) {
        std::string cursor = args.opt("cursor");
        has_cursor = !cursor.empty();
        if (!has_cursor) { out_page = 0; out_cached_total = -1; return true; }
        CursorState st;
        std::string reason;
        if (!decode_cursor(cursor, st, reason)) { emit_invalid_cursor(reason); return false; }
        if (st.query_hash != expected_hash) {
            emit_invalid_cursor("Cursor filter mismatch; restart pagination without `cursor`.");
            return false;
        }
        out_page = st.page;
        out_cached_total = st.cached_total;
        return true;
    }

    // Per-function source-line join (list_ufunctions). Returns 0 on miss.
    int lookup_function_source_line(const std::string& function_name, const std::string& owning_class) {
        if (function_name.empty()) return 0;
        if (!owning_class.empty()) {
            auto rows = query(db,
                "SELECT s.line_start FROM symbols s JOIN symbols p ON p.id = s.parent_symbol_id "
                "WHERE s.name = ? AND s.kind IN ('function','method') "
                "AND p.name = ? AND p.kind IN ('class','struct') LIMIT 1",
                {function_name, owning_class});
            return rows.empty() ? 0 : rows[0].get_int("line_start");
        }
        auto rows = query(db,
            "SELECT line_start FROM symbols WHERE name = ? AND kind IN ('function','method') LIMIT 1",
            {function_name});
        return rows.empty() ? 0 : rows[0].get_int("line_start");
    }

    // Class source-line auto-join (get_uclass). Returns 0 on miss.
    int lookup_class_source_line(const std::string& class_name) {
        if (class_name.empty()) return 0;
        auto rows = query(db,
            "SELECT s.line_start FROM symbols s WHERE s.name = ? "
            "AND s.kind IN ('class','struct') LIMIT 1", {class_name});
        return rows.empty() ? 0 : rows[0].get_int("line_start");
    }

    // Class source-path auto-join (get_uclass). Returns "" on miss.
    std::string lookup_class_source_path(const std::string& class_name) {
        if (class_name.empty()) return "";
        auto rows = query(db,
            "SELECT f.path FROM symbols s JOIN files f ON f.id = s.file_id "
            "WHERE s.name = ? AND s.kind IN ('class','struct') LIMIT 1", {class_name});
        return rows.empty() ? "" : rows[0].get("path");
    }

public:
    void open(const std::string& path) { db.open(path); }

    // ========================================================
    // NAMESPACE: cppreflect (6)
    // ========================================================

    // --- cppreflect get_uclass ---  (NOT paginated)
    void get_uclass(const Args& args) {
        std::string class_name = args.opt("class_name");
        if (class_name.empty() && !args.positional.empty()) class_name = args.positional[0];
        if (class_name.empty()) { emit_param_error("`class_name` is required."); return; }
        std::string module = args.opt("module");
        if (module.empty()) module = args.opt("module_name");

        if (!table_exists(db, "reflect_uclasses")) { emit_missing_table("reflect_uclasses"); return; }

        std::string sql = "SELECT class_name, module_name, parent_class, source_path, source_line, flags "
                          "FROM reflect_uclasses WHERE class_name = ?";
        std::vector<std::string> params = {class_name};
        if (!module.empty()) { sql += " AND module_name = ?"; params.push_back(module); }
        sql += " LIMIT 1";

        auto rows = query(db, sql, params);
        if (rows.empty()) {
            ojson out;
            out["success"] = true;
            out["uclass"] = nullptr;   // live miss shape: { uclass: null }
            std::cout << out.dump(2) << std::endl;
            return;
        }

        auto& r = rows[0];
        std::string cn = r.get("class_name");
        std::string mn = r.get("module_name");

        int src_line = r.get_int("source_line");
        std::string src_path = r.get("source_path");
        if (src_line == 0) src_line = lookup_class_source_line(cn);
        if (src_path.empty()) src_path = lookup_class_source_path(cn);

        ojson uclass;
        uclass["class_name"] = cn;
        uclass["module_name"] = mn;
        uclass["parent_class"] = r.get("parent_class");
        uclass["source_path"] = src_path;
        uclass["source_line"] = src_line;
        uclass["flags"] = r.get("flags");

        // Parent chain: iterative, cap 16.
        ojson chain = ojson::array();
        {
            std::string cur = r.get("parent_class");
            std::string last_appended;
            int guard = 0;
            while (!cur.empty() && guard < 16) {
                chain.push_back(cur);
                last_appended = cur;
                auto prow = query(db,
                    "SELECT parent_class FROM reflect_uclasses WHERE class_name = ? LIMIT 1", {cur});
                if (prow.empty()) break;            // engine class outside index → stop
                std::string next = prow[0].get("parent_class");
                if (next == last_appended) break;   // cycle guard
                cur = next;
                ++guard;
                if (cur.empty()) break;
            }
        }
        uclass["parent_chain"] = chain;

        // UPROPERTYs (module filter only when module passed).
        {
            std::string psql = "SELECT property_name, property_type, cpp_module, blueprint_visibility, specifiers "
                               "FROM reflect_uproperties WHERE owning_class = ?";
            std::vector<std::string> pp = {cn};
            if (!module.empty()) { psql += " AND cpp_module = ?"; pp.push_back(module); }
            psql += " ORDER BY property_name";
            auto props = query(db, psql, pp);
            ojson jprops = ojson::array();
            for (auto& p : props) {
                ojson o;
                o["property_name"] = p.get("property_name");
                o["property_type"] = p.get("property_type");
                o["cpp_module"] = p.get("cpp_module");
                o["blueprint_visibility"] = p.get("blueprint_visibility");
                o["specifiers"] = p.get("specifiers");
                jprops.push_back(o);
            }
            uclass["uproperties"] = jprops;
        }

        // UFUNCTIONs (module filter only when module passed). No source join here.
        {
            std::string fsql = "SELECT function_name, return_type, blueprint_callable, cpp_module, specifiers "
                               "FROM reflect_ufunctions WHERE owning_class = ?";
            std::vector<std::string> fp = {cn};
            if (!module.empty()) { fsql += " AND cpp_module = ?"; fp.push_back(module); }
            fsql += " ORDER BY function_name";
            auto funcs = query(db, fsql, fp);
            ojson jfuncs = ojson::array();
            for (auto& f : funcs) {
                ojson o;
                o["function_name"] = f.get("function_name");
                o["return_type"] = f.get("return_type");
                o["blueprint_callable"] = f.get_int("blueprint_callable") != 0;
                o["cpp_module"] = f.get("cpp_module");
                o["specifiers"] = f.get("specifiers");
                jfuncs.push_back(o);
            }
            uclass["ufunctions"] = jfuncs;
        }

        ojson out;
        out["success"] = true;
        out["uclass"] = uclass;
        std::cout << out.dump(2) << std::endl;
    }

    // --- cppreflect list_uproperties ---  (paginated, total_estimate)
    void list_uproperties(const Args& args) {
        if (!table_exists(db, "reflect_uproperties")) { emit_missing_table("reflect_uproperties"); return; }
        // Precedence: --class_name flag wins, else the positional arg (mirrors
        // get_uclass and the Python sibling: Args::opt then positional[0]).
        // The prior exe read ONLY opt("class_name"), so a positional class name
        // (`list_uproperties ALeviathanCharacterBase`) was dropped and the query
        // scanned the ENTIRE table (BUG 2).
        std::string class_name = args.opt("class_name");
        if (class_name.empty() && !args.positional.empty()) class_name = args.positional[0];
        bool bp_only = args.opt_bool("blueprint_visible_only", false);
        int limit = 50;
        if (!validate_cppreflect_limit(args, limit)) return;

        uint32_t qh = compute_filter_hash({class_name, bp_only ? "1" : "0"});
        int32_t page = 0, cached_total = -1; bool has_cursor = false;
        if (!resolve_page(args, qh, page, cached_total, has_cursor)) return;

        std::string where = " WHERE 1=1";
        std::vector<std::string> binds;
        if (!class_name.empty()) { where += " AND owning_class = ?"; binds.push_back(class_name); }
        if (bp_only) where += " AND blueprint_visibility IS NOT NULL AND blueprint_visibility <> ''";

        std::string sql = "SELECT owning_class, property_name, property_type, cpp_module, "
                          "blueprint_visibility, specifiers FROM reflect_uproperties" + where +
                          " ORDER BY owning_class, property_name LIMIT ? OFFSET ?";
        std::vector<std::string> params = binds;
        params.push_back(std::to_string(limit));
        params.push_back(std::to_string(page * limit));
        auto rows = query(db, sql, params);

        ojson arr = ojson::array();
        for (auto& r : rows) {
            ojson o;
            o["owning_class"] = r.get("owning_class");
            o["property_name"] = r.get("property_name");
            o["property_type"] = r.get("property_type");
            o["cpp_module"] = r.get("cpp_module");
            o["blueprint_visibility"] = r.get("blueprint_visibility");
            o["specifiers"] = r.get("specifiers");
            arr.push_back(o);
        }

        ojson out;
        out["success"] = true;
        out["uproperties"] = arr;
        int64_t total = -1;
        if (!has_cursor) {
            total = scalar_count(db, "SELECT COUNT(*) FROM reflect_uproperties" + where, binds);
            out["total_estimate"] = total;
        }
        if ((int)rows.size() == limit)
            out["next_cursor"] = encode_cursor(qh, page + 1, has_cursor ? cached_total : (int32_t)total);
        std::cout << out.dump(2) << std::endl;
    }

    // --- cppreflect list_ufunctions ---  (paginated, total_estimate, source join)
    void list_ufunctions(const Args& args) {
        if (!table_exists(db, "reflect_ufunctions")) { emit_missing_table("reflect_ufunctions"); return; }
        // Precedence: --class_name flag wins, else the positional arg (mirrors
        // get_uclass and the Python sibling: Args::opt then positional[0]). See
        // list_uproperties for the BUG 2 root-cause note.
        std::string class_name = args.opt("class_name");
        if (class_name.empty() && !args.positional.empty()) class_name = args.positional[0];
        bool bp_only = args.opt_bool("blueprint_callable_only", false);
        int limit = 50;
        if (!validate_cppreflect_limit(args, limit)) return;

        uint32_t qh = compute_filter_hash({class_name, bp_only ? "1" : "0"});
        int32_t page = 0, cached_total = -1; bool has_cursor = false;
        if (!resolve_page(args, qh, page, cached_total, has_cursor)) return;

        std::string where = " WHERE 1=1";
        std::vector<std::string> binds;
        if (!class_name.empty()) { where += " AND owning_class = ?"; binds.push_back(class_name); }
        if (bp_only) where += " AND blueprint_callable = 1";

        std::string sql = "SELECT owning_class, function_name, return_type, blueprint_callable, "
                          "cpp_module, specifiers, source_line FROM reflect_ufunctions" + where +
                          " ORDER BY owning_class, function_name LIMIT ? OFFSET ?";
        std::vector<std::string> params = binds;
        params.push_back(std::to_string(limit));
        params.push_back(std::to_string(page * limit));
        auto rows = query(db, sql, params);

        ojson arr = ojson::array();
        for (auto& r : rows) {
            int sl = r.get_int("source_line");
            if (sl == 0) sl = lookup_function_source_line(r.get("function_name"), r.get("owning_class"));
            ojson o;
            o["owning_class"] = r.get("owning_class");
            o["function_name"] = r.get("function_name");
            o["return_type"] = r.get("return_type");
            o["blueprint_callable"] = r.get_int("blueprint_callable") != 0;
            o["cpp_module"] = r.get("cpp_module");
            o["specifiers"] = r.get("specifiers");
            o["source_line"] = sl;
            arr.push_back(o);
        }

        ojson out;
        out["success"] = true;
        out["ufunctions"] = arr;
        int64_t total = -1;
        if (!has_cursor) {
            total = scalar_count(db, "SELECT COUNT(*) FROM reflect_ufunctions" + where, binds);
            out["total_estimate"] = total;
        }
        if ((int)rows.size() == limit)
            out["next_cursor"] = encode_cursor(qh, page + 1, has_cursor ? cached_total : (int32_t)total);
        std::cout << out.dump(2) << std::endl;
    }

    // --- cppreflect find_interface_impls ---  (NOT paginated)
    void find_interface_impls(const Args& args) {
        std::string iface = args.opt("interface_name");
        if (iface.empty() && !args.positional.empty()) iface = args.positional[0];
        if (iface.empty()) { emit_param_error("`interface_name` is required."); return; }
        if (!table_exists(db, "reflect_uinterface_impls")) { emit_missing_table("reflect_uinterface_impls"); return; }

        auto rows = query(db,
            "SELECT impl.implementing_class, impl.cpp_module, cls.source_path "
            "FROM reflect_uinterface_impls impl "
            "LEFT JOIN reflect_uclasses cls ON cls.class_name = impl.implementing_class "
            "AND cls.module_name = impl.cpp_module "
            "WHERE impl.interface_name = ? "
            "ORDER BY impl.cpp_module, impl.implementing_class", {iface});

        ojson arr = ojson::array();
        for (auto& r : rows) {
            ojson o;
            o["implementing_class"] = r.get("implementing_class");
            o["cpp_module"] = r.get("cpp_module");
            o["source_path"] = r.get("source_path");
            arr.push_back(o);
        }
        ojson out;
        out["success"] = true;
        out["interface_name"] = iface;
        out["implementers"] = arr;
        std::cout << out.dump(2) << std::endl;
    }

    // Build the token universe (token,count) for known_tokens / list_class_specifiers.
    std::vector<std::pair<std::string, int>> build_token_universe() {
        std::map<std::string, int> counts;
        auto rows = query(db,
            "SELECT flags FROM reflect_uclasses WHERE flags IS NOT NULL AND flags <> ''");
        for (auto& r : rows) {
            std::string flags = r.get("flags");
            std::set<std::string> seen_this_row;
            std::string token;
            std::istringstream ss(flags);
            // split on ':' culling empty, trimming each
            size_t start = 0;
            while (start <= flags.size()) {
                size_t pos = flags.find(':', start);
                std::string tok = (pos == std::string::npos)
                    ? flags.substr(start) : flags.substr(start, pos - start);
                tok = trim_copy(tok);
                if (!tok.empty()) seen_this_row.insert(tok);
                if (pos == std::string::npos) break;
                start = pos + 1;
            }
            for (const auto& t : seen_this_row) counts[t]++;
        }
        std::vector<std::pair<std::string, int>> out(counts.begin(), counts.end());
        std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) {
            if (a.second != b.second) return a.second > b.second;  // count desc
            return a.first < b.first;                              // token asc
        });
        return out;
    }

    // --- cppreflect find_class_specifier ---  (paginated, NO total)
    void find_class_specifier(const Args& args) {
        std::string spec = args.opt("specifier_name");
        if (spec.empty() && !args.positional.empty()) spec = args.positional[0];
        if (spec.empty()) { emit_param_error("`specifier_name` is required."); return; }
        if (!table_exists(db, "reflect_uclasses")) { emit_missing_table("reflect_uclasses"); return; }

        std::string spec_lower = to_lower_copy(spec);

        // DROPPED tokens: no SQL, immediate note + known_tokens.
        if (spec_lower == "minimalapi" || spec_lower == "notblueprintable") {
            auto universe = build_token_universe();
            ojson known = ojson::array();
            for (auto& kv : universe) {
                ojson o; o["token"] = kv.first; o["count"] = kv.second; known.push_back(o);
            }
            ojson out;
            out["success"] = true;
            out["specifier_name"] = spec;
            out["uclasses"] = ojson::array();
            out["token_stored"] = false;
            out["note"] = "\"" + spec + "\" is a C++ UCLASS specifier that UHT does not store in the "
                          "metadata-key vocabulary (the `flags` column), so it can never match. Call "
                          "list_class_specifiers to see the tokens that are queryable.";
            out["known_tokens"] = known;
            std::cout << out.dump(2) << std::endl;
            return;
        }

        int limit = 50;
        if (!validate_cppreflect_limit(args, limit)) return;
        uint32_t qh = compute_filter_hash({spec});
        int32_t page = 0, cached_total = -1; bool has_cursor = false;
        if (!resolve_page(args, qh, page, cached_total, has_cursor)) return;

        // ALIAS.
        std::string effective = spec;
        if (spec_lower == "blueprintable") effective = "IsBlueprintBase";

        std::string sql =
            "SELECT class_name, module_name, parent_class, source_path, flags FROM reflect_uclasses "
            "WHERE flags = ? COLLATE NOCASE OR flags LIKE ? OR flags LIKE ? OR flags LIKE ? "
            "ORDER BY module_name, class_name LIMIT ? OFFSET ?";
        std::vector<std::string> params = {
            effective,
            effective + ":%",
            "%:" + effective + ":%",
            "%:" + effective,
            std::to_string(limit),
            std::to_string(page * limit)
        };
        auto rows = query(db, sql, params);

        ojson arr = ojson::array();
        for (auto& r : rows) {
            ojson o;
            o["class_name"] = r.get("class_name");
            o["module_name"] = r.get("module_name");
            o["parent_class"] = r.get("parent_class");
            o["source_path"] = r.get("source_path");
            o["flags"] = r.get("flags");
            arr.push_back(o);
        }

        ojson out;
        out["success"] = true;
        out["specifier_name"] = spec;
        if (effective != spec) out["effective_token"] = effective;
        out["uclasses"] = arr;
        if (rows.empty() && !has_cursor) {
            auto universe = build_token_universe();
            ojson known = ojson::array();
            for (auto& kv : universe) {
                ojson o; o["token"] = kv.first; o["count"] = kv.second; known.push_back(o);
            }
            out["known_tokens"] = known;
        }
        if ((int)rows.size() == limit)
            out["next_cursor"] = encode_cursor(qh, page + 1, -1);  // tc always -1
        std::cout << out.dump(2) << std::endl;
    }

    // --- cppreflect list_class_specifiers ---  (NOT paginated)
    void list_class_specifiers(const Args&) {
        if (!table_exists(db, "reflect_uclasses")) { emit_missing_table("reflect_uclasses"); return; }
        auto universe = build_token_universe();
        ojson arr = ojson::array();
        for (auto& kv : universe) {
            ojson o; o["token"] = kv.first; o["count"] = kv.second; arr.push_back(o);
        }
        ojson out;
        out["success"] = true;
        out["specifiers"] = arr;
        out["distinct_count"] = (int)universe.size();
        std::cout << out.dump(2) << std::endl;
    }

    // ========================================================
    // NAMESPACE: network (4)
    // ========================================================

    // --- network list_replicated_classes ---  (paginated, total_estimate)
    void list_replicated_classes(const Args& args) {
        if (!table_exists(db, "reflect_replicated_properties")) {
            emit_missing_table("reflect_replicated_properties"); return;
        }
        int limit = clamp_limit(args);
        uint32_t qh = compute_filter_hash({});   // empty parts → 0
        int32_t page = 0, cached_total = -1; bool has_cursor = false;
        if (!resolve_page(args, qh, page, cached_total, has_cursor)) return;

        auto rows = query(db,
            "SELECT owning_class, cpp_module, COUNT(*) AS prop_count "
            "FROM reflect_replicated_properties GROUP BY owning_class, cpp_module "
            "ORDER BY cpp_module, owning_class LIMIT ? OFFSET ?",
            {std::to_string(limit), std::to_string(page * limit)});

        ojson arr = ojson::array();
        for (auto& r : rows) {
            ojson o;
            o["owning_class"] = r.get("owning_class");
            o["cpp_module"] = r.get("cpp_module");
            o["replicated_property_count"] = r.get_int("prop_count");
            arr.push_back(o);
        }

        ojson out;
        out["success"] = true;
        out["classes"] = arr;
        int64_t total = -1;
        if (!has_cursor) {
            total = scalar_count(db,
                "SELECT COUNT(*) FROM (SELECT 1 FROM reflect_replicated_properties "
                "GROUP BY owning_class, cpp_module)", {});
            out["total_estimate"] = total;
        }
        if ((int)rows.size() == limit)
            out["next_cursor"] = encode_cursor(qh, page + 1, has_cursor ? cached_total : (int32_t)total);
        std::cout << out.dump(2) << std::endl;
    }

    // --- network list_rpc_functions ---  (paginated, NO total, post-fetch filter)
    void list_rpc_functions(const Args& args) {
        if (!table_exists(db, "reflect_ufunctions")) { emit_missing_table("reflect_ufunctions"); return; }
        std::string class_name = args.opt("class_name");
        std::string rpc_kind = args.opt("rpc_kind");
        int limit = clamp_limit(args);

        uint32_t qh = compute_filter_hash({class_name, rpc_kind});
        int32_t page = 0, cached_total = -1; bool has_cursor = false;
        if (!resolve_page(args, qh, page, cached_total, has_cursor)) return;

        std::string where = " WHERE (specifiers LIKE '%Server%' OR specifiers LIKE '%Client%' "
                            "OR specifiers LIKE '%NetMulticast%')";
        std::vector<std::string> binds;
        if (!class_name.empty()) { where += " AND owning_class = ?"; binds.push_back(class_name); }

        std::string sql = "SELECT owning_class, function_name, cpp_module, blueprint_callable, specifiers "
                          "FROM reflect_ufunctions" + where +
                          " ORDER BY cpp_module, owning_class, function_name LIMIT ? OFFSET ?";
        std::vector<std::string> params = binds;
        params.push_back(std::to_string(limit));
        params.push_back(std::to_string(page * limit));
        auto rows = query(db, sql, params);

        std::string rpc_kind_lower = to_lower_copy(rpc_kind);
        ojson arr = ojson::array();
        int emitted = 0;
        for (auto& r : rows) {
            std::string specs = r.get("specifiers");
            std::string kind;
            if (specs.find("Server") != std::string::npos) kind = "Server";
            else if (specs.find("Client") != std::string::npos) kind = "Client";
            else if (specs.find("NetMulticast") != std::string::npos) kind = "Multicast";
            else kind = "";

            if (!rpc_kind.empty() && to_lower_copy(kind) != rpc_kind_lower) continue;

            ojson o;
            o["owning_class"] = r.get("owning_class");
            o["function_name"] = r.get("function_name");
            o["cpp_module"] = r.get("cpp_module");
            o["rpc_kind"] = kind;
            o["specifiers"] = specs;
            o["blueprint_callable"] = r.get_int("blueprint_callable") != 0;
            arr.push_back(o);
            ++emitted;
        }

        ojson out;
        out["success"] = true;
        out["rpcs"] = arr;
        if (emitted == limit)
            out["next_cursor"] = encode_cursor(qh, page + 1, -1);
        std::cout << out.dump(2) << std::endl;
    }

    // --- network list_onrep_handlers ---  (paginated, NO total)
    void list_onrep_handlers(const Args& args) {
        if (!table_exists(db, "reflect_ufunctions")) { emit_missing_table("reflect_ufunctions"); return; }
        std::string class_name = args.opt("class_name");
        int limit = clamp_limit(args);

        uint32_t qh = compute_filter_hash({class_name});
        int32_t page = 0, cached_total = -1; bool has_cursor = false;
        if (!resolve_page(args, qh, page, cached_total, has_cursor)) return;

        std::string where = " WHERE function_name LIKE 'OnRep_%'";
        std::vector<std::string> binds;
        if (!class_name.empty()) { where += " AND owning_class = ?"; binds.push_back(class_name); }

        std::string sql = "SELECT owning_class, function_name, cpp_module FROM reflect_ufunctions" + where +
                          " ORDER BY cpp_module, owning_class, function_name LIMIT ? OFFSET ?";
        std::vector<std::string> params = binds;
        params.push_back(std::to_string(limit));
        params.push_back(std::to_string(page * limit));
        auto rows = query(db, sql, params);

        ojson arr = ojson::array();
        for (auto& r : rows) {
            ojson o;
            o["owning_class"] = r.get("owning_class");
            o["function_name"] = r.get("function_name");
            o["cpp_module"] = r.get("cpp_module");
            arr.push_back(o);
        }

        ojson out;
        out["success"] = true;
        out["handlers"] = arr;
        if ((int)rows.size() == limit)
            out["next_cursor"] = encode_cursor(qh, page + 1, -1);
        std::cout << out.dump(2) << std::endl;
    }

    // --- network audit_unbalanced_onreps ---  (paginated, NO total)
    void audit_unbalanced_onreps(const Args& args) {
        if (!table_exists(db, "reflect_replicated_properties")) {
            emit_missing_table("reflect_replicated_properties"); return;
        }
        int limit = clamp_limit(args);
        uint32_t qh = compute_filter_hash({});
        int32_t page = 0, cached_total = -1; bool has_cursor = false;
        if (!resolve_page(args, qh, page, cached_total, has_cursor)) return;

        auto rows = query(db,
            "SELECT rp.owning_class, rp.property_name, rp.cpp_module, rp.rep_notify_func "
            "FROM reflect_replicated_properties rp "
            "LEFT JOIN reflect_ufunctions uf ON uf.owning_class = rp.owning_class "
            "AND uf.function_name = rp.rep_notify_func AND uf.cpp_module = rp.cpp_module "
            "WHERE rp.rep_kind = 'ReplicatedUsing' "
            "AND rp.rep_notify_func IS NOT NULL AND rp.rep_notify_func <> '' "
            "AND uf.function_name IS NULL "
            "ORDER BY rp.cpp_module, rp.owning_class, rp.property_name LIMIT ? OFFSET ?",
            {std::to_string(limit), std::to_string(page * limit)});

        ojson arr = ojson::array();
        for (auto& r : rows) {
            ojson o;
            o["owning_class"] = r.get("owning_class");
            o["property_name"] = r.get("property_name");
            o["cpp_module"] = r.get("cpp_module");
            o["missing_function"] = r.get("rep_notify_func");
            o["violation"] = "UPROPERTY(ReplicatedUsing) references an OnRep_ UFUNCTION that does not "
                             "exist on this class.";
            arr.push_back(o);
        }

        ojson out;
        out["success"] = true;
        out["violations"] = arr;
        if ((int)rows.size() == limit)
            out["next_cursor"] = encode_cursor(qh, page + 1, -1);
        std::cout << out.dump(2) << std::endl;
    }

    // ========================================================
    // NAMESPACE: decision (5)
    // ========================================================

    // Shared RowToJson for decision_records (8 cols → 8 keys, ordered).
    ojson decision_row_to_json(const Row& r) {
        ojson o;
        o["decision_id"] = r.get("decision_id");
        o["title"] = r.get("title");
        o["status"] = r.get("status");
        o["source_path"] = r.get("source_path");
        o["source_line"] = r.get_int("source_line");
        // confidence is a genuine REAL (float32 upstream) -> %.17g via sentinel.
        o["confidence"] = flt_sentinel(r.get_double("confidence"));
        o["rationale"] = r.get("rationale");
        o["source_mtime"] = r.get_int64("source_mtime");
        return o;
    }

    static const char* decision_columns() {
        return "decision_id, title, status, source_path, source_line, confidence, rationale, source_mtime";
    }

    // 3-arg decision filter hash form (path_filter, min_confidence/double, status-or-marker).
    uint32_t decision_filter_hash(const std::string& path_filter, double conf_or_age, const std::string& status) {
        uint32_t h = ue_str_crc32(path_filter);
        h = ue_hash_combine(h, ue_hash_double(conf_or_age));
        h = ue_hash_combine(h, ue_str_crc32(status));
        return h;
    }

    // --- decision list_decisions ---  (paginated, total_estimate, default conf 0.6)
    void list_decisions(const Args& args) {
        if (!table_exists(db, "decision_records")) { emit_missing_table("decision_records"); return; }
        std::string path_filter = args.opt("path_filter");
        double min_conf = 0.6;
        {
            auto it = args.options.find("min_confidence");
            if (it != args.options.end() && !it->second.empty()) {
                try { min_conf = std::stod(it->second); } catch (...) {}
            }
        }
        std::string status = args.opt("status");
        int limit = clamp_limit(args);

        uint32_t qh = decision_filter_hash(path_filter, min_conf, status);
        int32_t page = 0, cached_total = -1; bool has_cursor = false;
        if (!resolve_page(args, qh, page, cached_total, has_cursor)) return;

        std::string where = " WHERE confidence >= ?";
        std::vector<std::string> binds;
        // bind min_confidence as text; SQLite coerces for the numeric comparison.
        {
            std::ostringstream ss; ss << min_conf; binds.push_back(ss.str());
        }
        if (!path_filter.empty()) { where += " AND source_path LIKE ?"; binds.push_back("%" + path_filter + "%"); }
        if (!status.empty()) { where += " AND status = ?"; binds.push_back(status); }

        std::string sql = std::string("SELECT ") + decision_columns() + " FROM decision_records" + where +
                          " ORDER BY source_path, source_line LIMIT ? OFFSET ?";
        std::vector<std::string> params = binds;
        params.push_back(std::to_string(limit));
        params.push_back(std::to_string(page * limit));
        auto rows = query(db, sql, params);

        ojson arr = ojson::array();
        for (auto& r : rows) arr.push_back(decision_row_to_json(r));

        ojson out;
        out["success"] = true;
        out["decisions"] = arr;
        int64_t total = -1;
        if (!has_cursor) {
            total = scalar_count(db, "SELECT COUNT(*) FROM decision_records" + where, binds);
            out["total_estimate"] = total;
        }
        if ((int)rows.size() == limit)
            out["next_cursor"] = encode_cursor(qh, page + 1, has_cursor ? cached_total : (int32_t)total);
        emit_ri(out);   // %.17g float-sentinel finalize (confidence)
    }

    // --- decision get_decision ---  (NOT paginated)
    void get_decision(const Args& args) {
        std::string did = args.opt("decision_id");
        if (did.empty() && !args.positional.empty()) did = args.positional[0];
        if (did.empty()) { emit_param_error("`decision_id` is required."); return; }
        if (!table_exists(db, "decision_records")) { emit_missing_table("decision_records"); return; }

        auto rows = query(db,
            std::string("SELECT ") + decision_columns() + " FROM decision_records WHERE decision_id = ? LIMIT 1",
            {did});
        ojson out;
        out["success"] = true;
        out["decision"] = rows.empty() ? ojson(nullptr) : decision_row_to_json(rows[0]);
        emit_ri(out);   // %.17g float-sentinel finalize (confidence)
    }

    // --- decision list_stale ---  (paginated, NO total, cutoff_unix)
    void list_stale(const Args& args) {
        if (!table_exists(db, "decision_records")) { emit_missing_table("decision_records"); return; }
        // max_age_days required + positive.
        auto it = args.options.find("max_age_days");
        std::string raw = (it != args.options.end()) ? it->second : "";
        if (raw.empty() && !args.positional.empty()) raw = args.positional[0];
        int max_age_days = 0;
        try { max_age_days = std::stoi(raw); } catch (...) { max_age_days = 0; }
        if (max_age_days <= 0) { emit_param_error("`max_age_days` must be positive."); return; }

        std::string path_filter = args.opt("path_filter");
        int limit = clamp_limit(args);

        int64_t now_unix = (int64_t)std::time(nullptr);
        int64_t cutoff_unix = now_unix - (int64_t)max_age_days * 86400;

        uint32_t qh = decision_filter_hash(path_filter, (double)max_age_days, "__stale__");
        int32_t page = 0, cached_total = -1; bool has_cursor = false;
        if (!resolve_page(args, qh, page, cached_total, has_cursor)) return;

        std::string where = " WHERE source_mtime > 0 AND source_mtime < ?";
        std::vector<std::string> binds = {std::to_string(cutoff_unix)};
        if (!path_filter.empty()) { where += " AND source_path LIKE ?"; binds.push_back("%" + path_filter + "%"); }

        std::string sql = std::string("SELECT ") + decision_columns() + " FROM decision_records" + where +
                          " ORDER BY source_mtime ASC LIMIT ? OFFSET ?";
        std::vector<std::string> params = binds;
        params.push_back(std::to_string(limit));
        params.push_back(std::to_string(page * limit));
        auto rows = query(db, sql, params);

        ojson arr = ojson::array();
        for (auto& r : rows) arr.push_back(decision_row_to_json(r));

        ojson out;
        out["success"] = true;
        out["stale_decisions"] = arr;
        out["cutoff_unix"] = cutoff_unix;
        if ((int)rows.size() == limit)
            out["next_cursor"] = encode_cursor(qh, page + 1, -1);
        emit_ri(out);   // %.17g float-sentinel finalize (confidence)
    }

    // --- decision find_supersession_chain ---  (NOT paginated, BFS)
    void find_supersession_chain(const Args& args) {
        std::string start = args.opt("decision_id");
        if (start.empty() && !args.positional.empty()) start = args.positional[0];
        if (start.empty()) { emit_param_error("`decision_id` is required."); return; }
        if (!table_exists(db, "decision_supersedes")) { emit_missing_table("decision_supersedes"); return; }

        int depth = args.opt_int("depth", 10);
        if (depth < 1) depth = 1;
        if (depth > 50) depth = 50;

        std::set<std::string> visited;
        visited.insert(start);
        std::vector<std::string> frontier = {start};
        ojson chain = ojson::array();
        int cur_depth = 0;
        bool truncated = false;

        while (!frontier.empty()) {
            if (cur_depth == depth) { truncated = true; break; }
            std::vector<std::string> next;
            for (const auto& from : frontier) {
                auto rows = query(db,
                    "SELECT to_decision_id FROM decision_supersedes WHERE from_decision_id = ?", {from});
                for (auto& r : rows) {
                    std::string to = r.get("to_decision_id");
                    if (visited.count(to)) continue;
                    visited.insert(to);
                    ojson edge;
                    edge["from"] = from;
                    edge["to"] = to;
                    edge["depth"] = cur_depth + 1;
                    chain.push_back(edge);
                    next.push_back(to);
                }
            }
            frontier = std::move(next);
            ++cur_depth;
        }

        ojson out;
        out["success"] = true;
        out["start"] = start;
        out["chain"] = chain;
        out["truncated"] = truncated;
        std::cout << out.dump(2) << std::endl;
    }

    // --- decision find_referent_decisions ---  (NOT paginated)
    void find_referent_decisions(const Args& args) {
        std::string did = args.opt("decision_id");
        if (did.empty() && !args.positional.empty()) did = args.positional[0];
        if (did.empty()) { emit_param_error("`decision_id` is required."); return; }
        if (!table_exists(db, "decision_supersedes")) { emit_missing_table("decision_supersedes"); return; }

        auto rows = query(db,
            "SELECT r.decision_id, r.title, r.status, r.source_path, r.source_line, "
            "r.confidence, r.rationale, r.source_mtime "
            "FROM decision_supersedes s JOIN decision_records r ON r.decision_id = s.from_decision_id "
            "WHERE s.to_decision_id = ? ORDER BY r.source_path, r.source_line", {did});

        ojson arr = ojson::array();
        for (auto& r : rows) arr.push_back(decision_row_to_json(r));

        ojson out;
        out["success"] = true;
        out["decision_id"] = did;
        out["referent_decisions"] = arr;
        emit_ri(out);   // %.17g float-sentinel finalize (confidence)
    }

    // ========================================================
    // NAMESPACE: risk (5)
    // ========================================================

    // CanonPath: backslash → forward-slash ONLY (no lowercasing).
    static std::string canon_path(const std::string& p) {
        std::string out = p;
        std::replace(out.begin(), out.end(), '\\', '/');
        return out;
    }

    // --- risk get_hotspot_score ---  (NOT paginated)
    void get_hotspot_score(const Args& args) {
        std::string fp = args.opt("file_path");
        if (fp.empty() && !args.positional.empty()) fp = args.positional[0];
        if (fp.empty()) { emit_param_error("`file_path` is required."); return; }
        if (!table_exists(db, "risk_hotspot_scores")) { emit_missing_table("risk_hotspot_scores"); return; }

        std::string cp = canon_path(fp);
        auto rows = query(db,
            "SELECT file_path, churn, complexity_proxy, normalised_churn, normalised_complexity, score "
            "FROM risk_hotspot_scores WHERE file_path = ? LIMIT 1", {cp});

        ojson out;
        out["success"] = true;
        if (rows.empty()) {
            out["hotspot"] = nullptr;
        } else {
            auto& r = rows[0];
            ojson h;
            h["file_path"] = r.get("file_path");
            h["churn"] = r.get_int("churn");
            h["complexity_proxy"] = r.get_int("complexity_proxy");
            // normalised_churn / normalised_complexity / score are genuine REAL
            // (float32 upstream) -> %.17g via sentinel.
            h["normalised_churn"] = flt_sentinel(r.get_double("normalised_churn"));
            h["normalised_complexity"] = flt_sentinel(r.get_double("normalised_complexity"));
            h["score"] = flt_sentinel(r.get_double("score"));
            out["hotspot"] = h;
        }
        emit_ri(out);   // %.17g float-sentinel finalize (score/normalised_*)
    }

    // --- risk get_cochange_pairs ---  (paginated, total_estimate)
    void get_cochange_pairs(const Args& args) {
        std::string fp = args.opt("file_path");
        if (fp.empty() && !args.positional.empty()) fp = args.positional[0];
        if (fp.empty()) { emit_param_error("`file_path` is required."); return; }
        if (!table_exists(db, "git_cochange_pairs")) { emit_missing_table("git_cochange_pairs"); return; }

        std::string cp = canon_path(fp);
        int limit = clamp_limit(args);
        uint32_t qh = compute_filter_hash({cp});
        int32_t page = 0, cached_total = -1; bool has_cursor = false;
        if (!resolve_page(args, qh, page, cached_total, has_cursor)) return;

        auto rows = query(db,
            "SELECT repo_tag, CASE WHEN file_a = ? THEN file_b ELSE file_a END AS partner, count "
            "FROM git_cochange_pairs WHERE file_a = ? OR file_b = ? "
            "ORDER BY count DESC, partner ASC LIMIT ? OFFSET ?",
            {cp, cp, cp, std::to_string(limit), std::to_string(page * limit)});

        ojson arr = ojson::array();
        for (auto& r : rows) {
            ojson o;
            o["repo_tag"] = r.get("repo_tag");
            o["partner"] = r.get("partner");
            o["count"] = r.get_int("count");
            arr.push_back(o);
        }

        ojson out;
        out["success"] = true;
        out["file_path"] = cp;
        out["partners"] = arr;
        int64_t total = -1;
        if (!has_cursor) {
            total = scalar_count(db,
                "SELECT COUNT(*) FROM git_cochange_pairs WHERE file_a = ? OR file_b = ?", {cp, cp});
            out["total_estimate"] = total;
        }
        if ((int)rows.size() == limit)
            out["next_cursor"] = encode_cursor(qh, page + 1, has_cursor ? cached_total : (int32_t)total);
        std::cout << out.dump(2) << std::endl;
    }

    // --- risk get_file_churn ---  (NOT paginated)
    void get_file_churn(const Args& args) {
        std::string fp = args.opt("file_path");
        if (fp.empty() && !args.positional.empty()) fp = args.positional[0];
        if (fp.empty()) { emit_param_error("`file_path` is required."); return; }
        if (!table_exists(db, "git_file_churn")) { emit_missing_table("git_file_churn"); return; }

        std::string cp = canon_path(fp);
        std::string repo_tag = args.opt("repo_tag");

        std::string sql = "SELECT repo_tag, commit_count, last_touched FROM git_file_churn WHERE file_path = ?";
        std::vector<std::string> binds = {cp};
        if (!repo_tag.empty()) { sql += " AND repo_tag = ?"; binds.push_back(repo_tag); }
        auto rows = query(db, sql, binds);

        ojson arr = ojson::array();
        for (auto& r : rows) {
            ojson o;
            o["repo_tag"] = r.get("repo_tag");
            o["commit_count"] = r.get_int("commit_count");
            o["last_touched_unix"] = r.get_int64("last_touched");
            arr.push_back(o);
        }

        ojson out;
        out["success"] = true;
        out["file_path"] = cp;
        out["churn_by_repo"] = arr;
        std::cout << out.dump(2) << std::endl;
    }

    // --- risk get_release_window_hotspots ---  (paginated, NO total, since_unix)
    void get_release_window_hotspots(const Args& args) {
        if (!table_exists(db, "risk_hotspot_scores")) { emit_missing_table("risk_hotspot_scores"); return; }
        int64_t now_unix = (int64_t)std::time(nullptr);
        // Spec §risk.get_release_window_hotspots default window = now - 30 days.
        // int64 arithmetic throughout to match the Python sibling exactly.
        int64_t since = now_unix - (int64_t)30 * 86400;
        {
            auto it = args.options.find("since_unix");
            if (it != args.options.end() && !it->second.empty()) {
                try { since = std::stoll(it->second); } catch (...) {}
            }
        }
        int limit = clamp_limit(args);

        // FilterHash part: stringified int value used.
        uint32_t qh = compute_filter_hash({std::to_string(since)});
        int32_t page = 0, cached_total = -1; bool has_cursor = false;
        if (!resolve_page(args, qh, page, cached_total, has_cursor)) return;

        // BUG 3: `since` MUST bind as INTEGER. The WHERE-side is a scalar
        // subquery aggregate (MAX) with NO column affinity, so SQLite will not
        // coerce a TEXT-bound param to numeric — it would compare storage
        // classes (INTEGER always < TEXT) and match nothing. The live adapter
        // (FRiskQueryAdapter::HandleGetReleaseWindowHotspots) and the Python
        // sibling both bind an int64 here; query_typed mirrors that.
        auto rows = query_typed(db,
            "SELECT h.file_path, h.score, h.churn, h.complexity_proxy, "
            "(SELECT MAX(c.last_touched) FROM git_file_churn c WHERE c.file_path = h.file_path) AS last_touched "
            "FROM risk_hotspot_scores h "
            "WHERE (SELECT MAX(c2.last_touched) FROM git_file_churn c2 WHERE c2.file_path = h.file_path) >= ? "
            "ORDER BY h.score DESC LIMIT ? OFFSET ?",
            {Bind::Integer(since), Bind::Integer(limit), Bind::Integer((int64_t)page * limit)});

        ojson arr = ojson::array();
        for (auto& r : rows) {
            ojson o;
            o["file_path"] = r.get("file_path");
            // score is a genuine REAL (float32 upstream) -> %.17g via sentinel.
            o["score"] = flt_sentinel(r.get_double("score"));
            o["churn"] = r.get_int("churn");
            o["complexity_proxy"] = r.get_int("complexity_proxy");
            o["last_touched_unix"] = r.get_int64("last_touched");
            arr.push_back(o);
        }

        ojson out;
        out["success"] = true;
        out["since_unix"] = since;
        out["hotspots"] = arr;
        if ((int)rows.size() == limit)
            out["next_cursor"] = encode_cursor(qh, page + 1, -1);
        emit_ri(out);   // %.17g float-sentinel finalize (score)
    }

    // --- risk list_conditional_gates ---  (paginated, NO total)
    void list_conditional_gates(const Args& args) {
        if (!table_exists(db, "reflect_conditional_gates")) {
            emit_missing_table("reflect_conditional_gates"); return;
        }
        std::string macro_filter = args.opt("macro_filter");
        std::string path_filter = args.opt("path_filter");
        int limit = clamp_limit(args);

        uint32_t qh = compute_filter_hash({macro_filter, path_filter});
        int32_t page = 0, cached_total = -1; bool has_cursor = false;
        if (!resolve_page(args, qh, page, cached_total, has_cursor)) return;

        std::string where = " WHERE 1=1";
        std::vector<std::string> binds;
        if (!macro_filter.empty()) { where += " AND macro_name LIKE ?"; binds.push_back("%" + macro_filter + "%"); }
        if (!path_filter.empty()) { where += " AND source_path LIKE ?"; binds.push_back("%" + path_filter + "%"); }

        std::string sql = "SELECT id, source_path, source_line, macro_name, gate_kind, context_snippet "
                          "FROM reflect_conditional_gates" + where +
                          " ORDER BY source_path, source_line LIMIT ? OFFSET ?";
        std::vector<std::string> params = binds;
        params.push_back(std::to_string(limit));
        params.push_back(std::to_string(page * limit));
        auto rows = query(db, sql, params);

        ojson arr = ojson::array();
        for (auto& r : rows) {
            ojson o;
            o["id"] = r.get_int("id");
            o["source_path"] = r.get("source_path");
            o["source_line"] = r.get_int("source_line");
            o["macro_name"] = r.get("macro_name");
            o["gate_kind"] = r.get("gate_kind");
            o["context_snippet"] = r.get("context_snippet");
            arr.push_back(o);
        }

        ojson out;
        out["success"] = true;
        out["gates"] = arr;
        if ((int)rows.size() == limit)
            out["next_cursor"] = encode_cursor(qh, page + 1, -1);
        std::cout << out.dump(2) << std::endl;
    }
};

// ============================================================
// DB path resolution
// ============================================================

// Directory containing the running executable, or empty on failure.

static fs::path get_exe_dir() {
    fs::path exe_path;
#ifdef _WIN32
    char buf[MAX_PATH];
    DWORD len = GetModuleFileNameA(nullptr, buf, MAX_PATH);
    if (len > 0 && len < MAX_PATH)
        exe_path = buf;
#endif
    return exe_path.empty() ? fs::path() : exe_path.parent_path();
}

static std::string resolve_db_dir() {
    // Default: ../../Saved/ relative to exe location
    // (exe at Plugins/Monolith/Tools/MonolithQuery/ or Plugins/Monolith/Binaries/)
    auto exe_dir = get_exe_dir();
    if (exe_dir.empty()) {
        // Fallback: try current directory
        return ".";
    }

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

static bool is_db_file_path(const fs::path& path) {
    return lower_copy(path.extension().string()) == ".db";
}

static std::string parent_dir_or_dot(const fs::path& path) {
    fs::path parent = path.parent_path();
    return parent.empty() ? std::string(".") : parent.string();
}

static std::string resolve_db_dir_from_arg(const std::string& db_arg) {
    if (db_arg.empty()) return resolve_db_dir();

    fs::path db_path(db_arg);
    if (is_db_file_path(db_path)) return parent_dir_or_dot(db_path);
    return db_path.string();
}

static std::string resolve_namespace_db_path(const std::string& explicit_db,
                                             const std::string& db_arg,
                                             const std::string& db_dir,
                                             const std::string& default_filename) {
    if (!explicit_db.empty()) return explicit_db;

    if (!db_arg.empty()) {
        fs::path db_path(db_arg);
        if (is_db_file_path(db_path)) return db_path.string();
    }

    return (fs::path(db_dir) / default_filename).string();
}

static std::string resolve_bridge_db_path(const std::string& explicit_db,
                                          const std::string& db_arg,
                                          const std::string& db_dir,
                                          const std::string& default_filename) {
    if (!explicit_db.empty()) return explicit_db;

    if (!db_arg.empty()) {
        fs::path db_path(db_arg);
        if (lower_copy(db_path.filename().string()) == lower_copy(default_filename))
            return db_path.string();
    }

    return (fs::path(db_dir) / default_filename).string();
}

// Resolve the Monolith plugin root (the dir containing Monolith.uplugin) by
// probing the same exe-relative layouts resolve_db_dir() uses. Returns empty
// if not found.
//   exe at Plugins/Monolith/Tools/MonolithQuery/ -> root is ../../
//   exe at Plugins/Monolith/Tools/MonolithQuery/build/ -> root is ../../../
//   exe at Plugins/Monolith/Binaries/            -> root is ../
//   exe at Plugins/Monolith/ (plugin root)       -> root is ./
static fs::path resolve_plugin_root() {
    auto exe_dir = get_exe_dir();
    if (exe_dir.empty())
        return fs::path();

    const fs::path candidates[] = {
        exe_dir / ".." / ".." / "..", // Tools/MonolithQuery/build/
        exe_dir / ".." / "..",   // Tools/MonolithQuery/
        exe_dir / "..",          // Binaries/
        exe_dir                  // plugin root
    };
    for (const auto& c : candidates) {
        if (fs::exists(c / "Monolith.uplugin"))
            return fs::canonical(c);
    }
    return fs::path();
}

// Read "VersionName" from Monolith.uplugin via a plain string scan (no JSON
// dependency for this single field). Returns empty on any failure — the guide
// is still served; the version line is simply omitted.
static std::string parse_uplugin_version(const fs::path& plugin_root) {
    if (plugin_root.empty())
        return "";

    std::ifstream f((plugin_root / "Monolith.uplugin").string());
    if (!f.is_open())
        return "";

    std::stringstream ss;
    ss << f.rdbuf();
    std::string contents = ss.str();

    // Find "VersionName", then the value inside the next pair of quotes after
    // the following colon. Tolerant of arbitrary whitespace.
    auto key = contents.find("\"VersionName\"");
    if (key == std::string::npos)
        return "";

    auto colon = contents.find(':', key);
    if (colon == std::string::npos)
        return "";

    auto open_quote = contents.find('"', colon);
    if (open_quote == std::string::npos)
        return "";

    auto close_quote = contents.find('"', open_quote + 1);
    if (close_quote == std::string::npos)
        return "";

    return contents.substr(open_quote + 1, close_quote - open_quote - 1);
}

// ============================================================
// Monolith namespace — offline guide server
// ============================================================
//
// Mirrors the in-editor FMonolithGuideTool but WITHOUT the live registry
// overlay (offline has no registry — it just serves the markdown). The H2
// section split mirrors FMonolithGuideTool::SplitSections: a line is a section
// header iff it starts with "## " (after trimming) but NOT "### "; the key is
// the trimmed, lowercased remainder. Blank lines inside bodies are preserved;
// trailing whitespace per body is trimmed. Content before the first H2 (the H1
// title + intro) is intentionally dropped.

// Canonical section order — must match GetCanonicalSectionOrder() in
// MonolithGuideTool.cpp so offline and in-editor agree on index ordering.
static const std::vector<std::string>& get_canonical_section_order() {
    static const std::vector<std::string> order = {
        "onboarding", "recipes", "decisions", "errors", "skills_map", "gotchas"
    };
    return order;
}

static std::string join_canonical_section_names() {
    std::string out;
    const auto& order = get_canonical_section_order();
    for (size_t i = 0; i < order.size(); ++i) {
        if (i > 0) out += ", ";
        out += order[i];
    }
    return out;
}

// Trim leading whitespace.
static std::string guide_ltrim_copy(const std::string& s) {
    size_t i = 0;
    while (i < s.size() && std::isspace((unsigned char)s[i])) ++i;
    return s.substr(i);
}

// Trim trailing whitespace (mirrors FString::TrimEnd for body cleanup).
static std::string guide_rtrim_copy(const std::string& s) {
    size_t end = s.size();
    while (end > 0 && std::isspace((unsigned char)s[end - 1])) --end;
    return s.substr(0, end);
}

static std::string guide_trim_copy(const std::string& s) {
    return guide_rtrim_copy(guide_ltrim_copy(s));
}

static std::string guide_lower_copy(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    return s;
}

// Split markdown into H2-keyed sections, preserving body order in `ordered`.
static void split_sections(const std::string& markdown,
                           std::map<std::string, std::string>& out_sections,
                           std::vector<std::string>& out_ordered) {
    out_sections.clear();
    out_ordered.clear();

    std::string current_name;
    std::string current_body;
    bool in_section = false;

    auto flush_current = [&]() {
        if (!current_name.empty()) {
            out_sections[current_name] = guide_rtrim_copy(current_body);
            out_ordered.push_back(current_name);
        }
    };

    std::istringstream iss(markdown);
    std::string line;
    while (std::getline(iss, line)) {
        // Strip a trailing CR so CRLF files split cleanly.
        if (!line.empty() && line.back() == '\r')
            line.pop_back();

        std::string trimmed = guide_trim_copy(line);
        // H2 == "## " but NOT "### " (H3) and NOT "#" (H1 title).
        if (trimmed.rfind("## ", 0) == 0 && trimmed.rfind("### ", 0) != 0) {
            flush_current();
            current_name = guide_lower_copy(guide_trim_copy(trimmed.substr(3)));
            current_body.clear();
            in_section = true;
            continue;
        }

        if (in_section) {
            current_body += line;
            current_body += "\n";
        }
        // Content before the first H2 is intentionally dropped.
    }

    flush_current();
}

class MonolithActions {
public:
    // --- guide ---
    void print_guide(const Args& args) {
        fs::path plugin_root = resolve_plugin_root();
        fs::path guide_path = plugin_root.empty()
            ? fs::path("MONOLITH_GUIDE.md")
            : (plugin_root / "Docs" / "MONOLITH_GUIDE.md");

        std::ifstream f(guide_path.string());
        if (!f.is_open()) {
            std::cout << "Guide markdown not found at " << guide_path.string()
                      << "\nRestore Docs/MONOLITH_GUIDE.md (it ships in the release zip) "
                         "or pull the latest Monolith release, then retry." << std::endl;
            return;
        }

        std::stringstream ss;
        ss << f.rdbuf();
        std::string markdown = ss.str();

        std::map<std::string, std::string> sections;
        std::vector<std::string> ordered;
        split_sections(markdown, sections, ordered);

        const std::vector<std::string>& canonical = get_canonical_section_order();
        std::string version = parse_uplugin_version(plugin_root);

        // ----- Section-keyed dispatch -----
        std::string requested = guide_lower_copy(guide_trim_copy(args.opt("section")));
        if (!requested.empty()) {
            auto it = sections.find(requested);
            if (it == sections.end()) {
                std::cout << "Unknown section '" << requested << "'. Valid sections: "
                          << join_canonical_section_names() << "." << std::endl;
                return;
            }
            std::cout << "## " << requested << "\n\n" << it->second << std::endl;
            return;
        }

        // ----- No-arg dispatch: index + all section bodies (no live overlay) -----
        std::cout << "=== Monolith Guide";
        if (!version.empty())
            std::cout << " (v" << version << ")";
        std::cout << " ===\n\n";

        std::cout << "Sections (pass --section=NAME to print just one):\n";
        for (const auto& name : canonical) {
            if (sections.count(name))
                std::cout << "  - " << name << "\n";
        }
        std::cout << "\n";

        for (const auto& name : canonical) {
            auto it = sections.find(name);
            if (it != sections.end()) {
                std::cout << "## " << name << "\n\n" << it->second << "\n\n";
            }
        }
        std::cout << std::flush;
    }
};

// ============================================================
// Main
// ============================================================

int main(int argc, char* argv[]) {
    // --version / -v: print the build stamp before the 3-arg usage gate fires.
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--version" || a == "-v") {
            fs::path plugin_root = resolve_plugin_root();
            std::string version = parse_uplugin_version(plugin_root);
            ojson out;
            out["tool"] = "monolith_query";
            out["plugin_version"] = version;
            out["parity_spec_rev"] = PARITY_SPEC_REV;
            out["source_hash"] = SOURCE_HASH;
            std::cout << out.dump(2) << std::endl;
            return 0;
        }
    }

    bool handled_help = false;
    int help_exit_code = maybe_handle_help_or_catalog_check(argc, argv, handled_help);
    if (handled_help) return help_exit_code;

    const std::string start_time = iso_local_now();
    const auto start_clock = std::chrono::steady_clock::now();
    Args args;
    bool parsed_args = false;
    int exit_code = 0;
    std::string fatal_error;
    json db_paths = json::array();
    double parse_args_ms = 0.0;
    double db_resolve_ms = 0.0;
    double db_open_ms = 0.0;
    double action_exec_ms = 0.0;

    std::ostringstream captured_stdout;
    std::ostringstream captured_stderr;
    std::streambuf* old_stdout = std::cout.rdbuf(captured_stdout.rdbuf());
    std::streambuf* old_stderr = std::cerr.rdbuf(captured_stderr.rdbuf());

    try {
        auto phase_start = std::chrono::steady_clock::now();
        args = parse_args(argc, argv);
        parsed_args = true;
        parse_args_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - phase_start).count();

        validate_help_catalog_or_die();
        validate_known_command_or_die(args.ns, args.action);

        phase_start = std::chrono::steady_clock::now();
        const std::string db_arg = args.opt("db");
        const std::string db_dir = resolve_db_dir_from_arg(db_arg);
        db_resolve_ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - phase_start).count();

        if (args.ns == "source") {
            phase_start = std::chrono::steady_clock::now();
            std::string db_path = resolve_namespace_db_path(
                args.opt("source_db"), db_arg, db_dir, "EngineSource.db");
            db_paths.push_back(db_path);
            db_resolve_ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - phase_start).count();

            bool write_action = args.opt_bool("execute", false)
                && (args.action == "repair_fts" || args.action == "snapshot" || args.action == "repair_crg_cache");
            SourceActions sa;
            phase_start = std::chrono::steady_clock::now();
            sa.open(db_path, !write_action);
            db_open_ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - phase_start).count();

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
                {"find_overrides",      [](SourceActions& s, const Args& a) { s.find_overrides(a); }},
                {"health",              [](SourceActions& s, const Args& a) { s.health(a); }},
                {"trigger_reindex",     [](SourceActions& s, const Args& a) { s.trigger_reindex(a); }},
                {"trigger_project_reindex", [](SourceActions& s, const Args& a) { s.trigger_project_reindex(a); }},
                {"repair_fts",          [](SourceActions& s, const Args& a) { s.repair_fts(a); }},
                {"repair_crg_cache",    [](SourceActions& s, const Args& a) { s.repair_crg_cache(a); }},
                {"build_crg_graph",     [](SourceActions& s, const Args& a) { s.build_crg_graph(a); }},
                {"rebuild_crg_graph",   [](SourceActions& s, const Args& a) { s.rebuild_crg_graph(a); }},
                {"repair_crg_graph",    [](SourceActions& s, const Args& a) { s.rebuild_crg_graph(a); }},
                {"search_crg_graph",    [](SourceActions& s, const Args& a) { s.search_crg_graph(a); }},
                {"crg_graph_health",    [](SourceActions& s, const Args& a) { s.crg_graph_health(a); }},
                {"risk_score",          [](SourceActions& s, const Args& a) { s.risk_score(a); }},
                {"review_hotspots",     [](SourceActions& s, const Args& a) { s.review_hotspots(a); }},
                {"review_context",      [](SourceActions& s, const Args& a) { s.review_context(a); }},
                {"detect_changes",      [](SourceActions& s, const Args& a) { s.detect_changes(a); }},
                {"find_unused",         [](SourceActions& s, const Args& a) { s.find_unused(a); }},
                {"pre_merge_check",     [](SourceActions& s, const Args& a) { s.pre_merge_check(a); }},
                {"snapshot",            [](SourceActions& s, const Args& a) { s.snapshot(a); }},
                {"diff_snapshots",      [](SourceActions& s, const Args& a) { s.diff_snapshots(a); }},
                {"get_include_path",    [](SourceActions& s, const Args& a) { s.get_include_path(a); }},
                {"get_signature",       [](SourceActions& s, const Args& a) { s.get_signature(a); }},
                {"check_deprecations",  [](SourceActions& s, const Args& a) { s.check_deprecations(a); }},
                {"verify_symbols",      [](SourceActions& s, const Args& a) { s.verify_symbols(a); }},
                {"find_example_usage",  [](SourceActions& s, const Args& a) { s.find_example_usage(a); }},
                {"lint_header",         [](SourceActions& s, const Args& a) { s.lint_header(a); }},
                {"generate_class_stub", [](SourceActions& s, const Args& a) { s.generate_class_stub(a); }},
            };

            auto it = actions.find(args.action);
            if (it == actions.end()) die("Unknown source action: " + args.action);
            phase_start = std::chrono::steady_clock::now();
            it->second(sa, args);
            action_exec_ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - phase_start).count();

        } else if (args.ns == "bridge") {
            phase_start = std::chrono::steady_clock::now();
            std::string project_db = resolve_bridge_db_path(
                args.opt("project_db"), db_arg, db_dir, "ProjectIndex.db");
            std::string source_db = resolve_bridge_db_path(
                args.opt("source_db"), db_arg, db_dir, "EngineSource.db");
            db_paths.push_back(project_db);
            db_paths.push_back(source_db);
            db_resolve_ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - phase_start).count();

            BridgeActions ba;
            phase_start = std::chrono::steady_clock::now();
            ba.open(project_db, source_db);
            db_open_ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - phase_start).count();

            static const std::map<std::string, std::function<void(BridgeActions&, const Args&)>> actions = {
                {"search_asset_symbols", [](BridgeActions& b, const Args& a) { b.search_asset_symbols(a); }},
            };

            auto it = actions.find(args.action);
            if (it == actions.end()) die("Unknown bridge action: " + args.action);
            phase_start = std::chrono::steady_clock::now();
            it->second(ba, args);
            action_exec_ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - phase_start).count();

        } else if (args.ns == "project") {
            phase_start = std::chrono::steady_clock::now();
            std::string db_path = resolve_namespace_db_path(
                args.opt("project_db"), db_arg, db_dir, "ProjectIndex.db");
            db_paths.push_back(db_path);
            db_resolve_ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - phase_start).count();

            bool write_action = args.opt_bool("execute", false)
                && (args.action == "repair_fts" || args.action == "snapshot" || args.action == "repair_crg_cache");
            ProjectActions pa;
            phase_start = std::chrono::steady_clock::now();
            pa.open(db_path, !write_action);
            db_open_ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - phase_start).count();

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
            phase_start = std::chrono::steady_clock::now();
            it->second(pa, args);
            action_exec_ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - phase_start).count();

        } else if (args.ns == "monolith") {
            MonolithActions ma;

            static const std::map<std::string, std::function<void(MonolithActions&, const Args&)>> actions = {
                {"guide", [](MonolithActions& m, const Args& a) { m.print_guide(a); }},
            };

            auto it = actions.find(args.action);
            if (it == actions.end()) die("Unknown monolith action: " + args.action + " (expected 'guide')");
            phase_start = std::chrono::steady_clock::now();
            it->second(ma, args);
            action_exec_ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - phase_start).count();

        } else if (args.ns == "cppreflect" || args.ns == "network" ||
                   args.ns == "decision" || args.ns == "risk") {
            phase_start = std::chrono::steady_clock::now();
            std::string db_path = resolve_namespace_db_path(
                args.opt("source_db"), db_arg, db_dir, "EngineSource.db");
            db_paths.push_back(db_path);
            db_resolve_ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - phase_start).count();

            ReflectionActions ra;
            phase_start = std::chrono::steady_clock::now();
            ra.open(db_path);
            db_open_ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - phase_start).count();

            using RIFn = std::function<void(ReflectionActions&, const Args&)>;
            static const std::map<std::string, std::map<std::string, RIFn>> ns_actions = {
                {"cppreflect", {
                    {"get_uclass",             [](ReflectionActions& r, const Args& a){ r.get_uclass(a); }},
                    {"list_uproperties",       [](ReflectionActions& r, const Args& a){ r.list_uproperties(a); }},
                    {"list_ufunctions",        [](ReflectionActions& r, const Args& a){ r.list_ufunctions(a); }},
                    {"find_interface_impls",   [](ReflectionActions& r, const Args& a){ r.find_interface_impls(a); }},
                    {"find_class_specifier",   [](ReflectionActions& r, const Args& a){ r.find_class_specifier(a); }},
                    {"list_class_specifiers",  [](ReflectionActions& r, const Args& a){ r.list_class_specifiers(a); }},
                }},
                {"network", {
                    {"list_replicated_classes", [](ReflectionActions& r, const Args& a){ r.list_replicated_classes(a); }},
                    {"list_rpc_functions",      [](ReflectionActions& r, const Args& a){ r.list_rpc_functions(a); }},
                    {"list_onrep_handlers",     [](ReflectionActions& r, const Args& a){ r.list_onrep_handlers(a); }},
                    {"audit_unbalanced_onreps", [](ReflectionActions& r, const Args& a){ r.audit_unbalanced_onreps(a); }},
                }},
                {"decision", {
                    {"list_decisions",          [](ReflectionActions& r, const Args& a){ r.list_decisions(a); }},
                    {"get_decision",            [](ReflectionActions& r, const Args& a){ r.get_decision(a); }},
                    {"list_stale",              [](ReflectionActions& r, const Args& a){ r.list_stale(a); }},
                    {"find_supersession_chain", [](ReflectionActions& r, const Args& a){ r.find_supersession_chain(a); }},
                    {"find_referent_decisions", [](ReflectionActions& r, const Args& a){ r.find_referent_decisions(a); }},
                }},
                {"risk", {
                    {"get_hotspot_score",           [](ReflectionActions& r, const Args& a){ r.get_hotspot_score(a); }},
                    {"get_cochange_pairs",          [](ReflectionActions& r, const Args& a){ r.get_cochange_pairs(a); }},
                    {"get_file_churn",              [](ReflectionActions& r, const Args& a){ r.get_file_churn(a); }},
                    {"get_release_window_hotspots", [](ReflectionActions& r, const Args& a){ r.get_release_window_hotspots(a); }},
                    {"list_conditional_gates",      [](ReflectionActions& r, const Args& a){ r.list_conditional_gates(a); }},
                }},
            };

            const auto& table = ns_actions.at(args.ns);
            auto it = table.find(args.action);
            if (it == table.end()) {
                std::vector<std::string> known;
                for (const auto& kv : table) known.push_back(kv.first);
                auto sugg = fuzzy_top(args.action, known, 3);
                std::string suffix;
                if (!sugg.empty()) {
                    suffix = " Did you mean: ";
                    for (size_t i = 0; i < sugg.size(); ++i) {
                        if (i) suffix += ", ";
                        suffix += sugg[i];
                    }
                    suffix += "?";
                }
                ojson out;
                out["success"] = false;
                out["error"] = "Unknown action: " + args.ns + "." + args.action +
                               " \xE2\x80\x94 call monolith_discover(\"" + args.ns +
                               "\") to enumerate valid actions in this namespace." + suffix;
                std::cout << out.dump(2) << std::endl;
                throw QueryFatal("", 0, true);
            }

            phase_start = std::chrono::steady_clock::now();
            it->second(ra, args);
            action_exec_ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - phase_start).count();

        } else {
            const auto& ns = offline_namespaces();
            std::string ns_list;
            for (size_t i = 0; i < ns.size(); ++i) {
                if (i > 0) ns_list += ", ";
                ns_list += "'" + ns[i] + "'";
            }
            std::cerr << "ERROR: Unknown namespace: " << args.ns
                      << " (offline-servable: " << ns_list << ")"
                      << did_you_mean_suffix(args.ns, ns) << std::endl;
            std::cerr << "NOTE: this offline tool serves only namespaces backed by on-disk SQLite. "
                         "The live MCP server exposes ~29 namespaces; the rest are LIVE-ONLY "
                         "(require a running editor + UObject reflection)." << std::endl;
            throw QueryFatal("", 2, true);
        }
    } catch (const QueryFatal& e) {
        exit_code = e.code;
        fatal_error = e.what();
        if (!e.already_printed && !fatal_error.empty()) {
            std::cerr << "ERROR: " << fatal_error << std::endl;
        }
    } catch (const std::exception& e) {
        exit_code = 1;
        fatal_error = e.what();
        std::cerr << "ERROR: " << fatal_error << std::endl;
    }

    const auto stdout_capture_start = std::chrono::steady_clock::now();
    std::cout.rdbuf(old_stdout);
    std::cerr.rdbuf(old_stderr);
    const std::string stdout_text = captured_stdout.str();
    const std::string stderr_text = captured_stderr.str();
    std::cout << stdout_text;
    std::cerr << stderr_text;
    const double stdout_capture_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - stdout_capture_start).count();

    if (tool_log_enabled()) {
        const auto log_prepare_start = std::chrono::steady_clock::now();
        const auto end_clock = std::chrono::steady_clock::now();
        const double duration_ms = std::chrono::duration<double, std::milli>(end_clock - start_clock).count();

        json options = json::object();
        json positional = json::array();
        if (parsed_args) {
            for (const auto& pair : args.options) options[pair.first] = pair.second;
            for (const auto& value : args.positional) positional.push_back(value);
        }

        bool stdout_truncated = false;
        bool stderr_truncated = false;
        size_t stdout_bytes = 0;
        size_t stderr_bytes = 0;
        std::string stdout_hash;
        std::string stderr_hash;
        json raw_stdout = redact_json(stdout_text);
        json raw_stderr = redact_json(stderr_text);
        json bounded_stdout = bounded_json(raw_stdout, stdout_truncated, stdout_bytes, stdout_hash);
        json bounded_stderr = bounded_json(raw_stderr, stderr_truncated, stderr_bytes, stderr_hash);

        const std::string signal_text = fatal_error.empty() ? stderr_text : fatal_error;
        std::string outcome = exit_code == 0 ? "success" : "tool_error";
        std::string error_class;
        json tags = json::array();
        std::string lower = signal_text;
        std::transform(lower.begin(), lower.end(), lower.begin(),
                       [](unsigned char c){ return static_cast<char>(std::tolower(c)); });
        if (exit_code != 0) {
            if (lower.find("unknown") != std::string::npos && lower.find("action") != std::string::npos) {
                error_class = "unknown_action";
                tags.push_back("missing_action");
            } else if (lower.find("requires") != std::string::npos || lower.find("invalid") != std::string::npos) {
                error_class = "missing_param";
                tags.push_back("schema_confusing");
            } else {
                error_class = "tool_error";
            }
        }
        if (stdout_bytes > max_log_field_bytes() || stderr_bytes > max_log_field_bytes()) {
            tags.push_back("large_result");
        }
        if (duration_ms > 5000.0) {
            tags.push_back("slow_action");
        }

        std::string trace_id = getenv_str("MONOLITH_TRACE_ID");
        if (trace_id.empty()) {
            trace_id = log_id("trace", start_time + ":query:" + argv_json(argc, argv).dump());
        }
        const std::string parent_span_id = getenv_str("MONOLITH_PARENT_SPAN_ID");
        const std::string span_id = log_id("span", trace_id + ":query:" + start_time + ":" + argv_json(argc, argv).dump());
        const std::string proc_instance_id = process_instance_id();
        const std::string record_id = log_id("rec", proc_instance_id + ":query:1:" + trace_id + ":" + span_id + ":" + start_time);
        const bool truncated = stdout_truncated || stderr_truncated;

        json call = {
            {"argv", argv_json(argc, argv)},
            {"namespace", parsed_args ? args.ns : ""},
            {"action", parsed_args ? args.action : ""},
            {"options", redact_json(options)},
            {"positional", redact_json(positional)},
            {"db_path", db_paths},
            {"retry_signature", hash_text((parsed_args ? args.ns + ":" + args.action : "usage") + argv_json(argc, argv).dump())}
        };

        json redaction = {
            {"stdout_bytes", stdout_bytes},
            {"stderr_bytes", stderr_bytes}
        };
        if (truncated) redaction["truncated"] = true;
        if (!stdout_hash.empty()) redaction["stdout_sha256"] = stdout_hash;
        if (!stderr_hash.empty()) redaction["stderr_sha256"] = stderr_hash;

        json agent_signal = {
            {"outcome", outcome},
            {"hints_returned", 0}
        };
        if (exit_code != 0) agent_signal["error_code"] = exit_code;
        if (!error_class.empty()) agent_signal["error_class"] = error_class;
        if (!tags.empty()) agent_signal["improvement_tags"] = tags;

        const std::string ns_for_context = parsed_args ? args.ns : "";
        const std::string action_for_context = parsed_args ? args.action : "";
        const std::string intent = infer_intent(ns_for_context, action_for_context, exit_code);
        json routing_context = {
            {"decision_source", parent_span_id.empty() ? "direct" : "child_query"},
            {"namespace_source", parent_span_id.empty() && getenv_str("MONOLITH_TRACE_ID").empty() ? "offline_cli" : "child_process"},
            {"inferred_intent", intent},
            {"intent_confidence", intent == "unknown" ? "low" : "medium"}
        };
        json workflow = {
            {"step", workflow_step_for_intent(intent, exit_code)}
        };
        json phase_timing = json::object();
        if (parse_args_ms > 0.0) phase_timing["parse_args_ms"] = parse_args_ms;
        if (db_resolve_ms > 0.0) phase_timing["db_resolve_ms"] = db_resolve_ms;
        if (db_open_ms > 0.0) phase_timing["db_open_ms"] = db_open_ms;
        if (action_exec_ms > 0.0) phase_timing["action_exec_ms"] = action_exec_ms;
        if (stdout_capture_ms > 0.0) phase_timing["stdout_capture_ms"] = stdout_capture_ms;
        phase_timing["log_prepare_ms"] = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - log_prepare_start).count();

        json record = {
            {"format_version", 3},
            {"surface", "query"},
            {"record_id", record_id},
            {"sequence", 1},
            {"trace_id", trace_id},
            {"span_id", span_id},
            {"parent_span_id", parent_span_id},
            {"process_instance_id", proc_instance_id},
            {"start_time", start_time},
            {"end_time", iso_local_now()},
            {"duration_ms", duration_ms},
#ifdef _WIN32
            {"pid", static_cast<int>(GetCurrentProcessId())},
#else
            {"pid", 0},
#endif
            {"thread_id", "main"},
            {"status", exit_code == 0 ? "success" : "error"},
            {"client", {
                {"name", "monolith_query"}
            }},
            {"routing_context", routing_context},
            {"workflow", workflow},
            {"phase_timing", phase_timing},
            {"call", call},
            {"return", {
                {"exit_code", exit_code},
                {"stdout", bounded_stdout},
                {"stderr", bounded_stderr},
                {"fatal_error", fatal_error}
            }},
            {"return_summary", summarize_query_return(
                raw_stdout,
                raw_stderr,
                exit_code,
                stdout_bytes,
                stderr_bytes,
                truncated,
                fatal_error)},
            {"redaction", redaction},
            {"agent_signal", agent_signal},
            {"environment", make_query_environment(ns_for_context, db_paths)}
        };
        append_query_log(record);
    }

    return exit_code;
}
