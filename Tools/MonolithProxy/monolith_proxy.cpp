/*
 * Monolith MCP stdio-to-HTTP proxy (C++ / WinHTTP).
 *
 * Sits between Claude Code (stdio JSON-RPC) and Monolith (HTTP on localhost).
 * Handles initialize locally, forwards tool calls to Monolith.
 * Survives editor restarts -- proxy process never dies.
 * Background health poll auto-detects when the editor comes online.
 *
 * Build: see build.bat or CMakeLists.txt
 * Usage (in .mcp.json):
 *   {"mcpServers":{"monolith":{"command":"path/to/monolith_proxy.exe"}}}
 */

// ============================================================================
// Includes
// ============================================================================

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winhttp.h>
#include <bcrypt.h>
#include <io.h>
#include <fcntl.h>
#pragma comment(lib, "bcrypt.lib")

#include <iostream>
#include <string>
#include <sstream>
#include <thread>
#include <mutex>
#include <chrono>
#include <set>
#include <map>
#include <unordered_map>
#include <algorithm>
#include <optional>
#include <cstdlib>
#include <fstream>
#include <vector>
#include <filesystem>
#include <iomanip>
#include <stdexcept>

#include <nlohmann/json.hpp>

using json = nlohmann::json;
namespace fs = std::filesystem;

// ============================================================================
// Constants
// ============================================================================

static const char* PROXY_NAME    = "monolith-proxy";
static const char* PROXY_VERSION = "1.1.1";

static constexpr double TIMEOUT                  = 30.0;
static constexpr double POLL_INTERVAL            = 5.0;
static constexpr double POLL_START_DELAY         = 3.0;
static constexpr double REPEAT_TOOL_CALL_WINDOW  = 3.0;

static const std::set<std::string> SUPPORTED_VERSIONS = {
    "2024-11-05", "2025-03-26", "2025-06-18", "2025-11-25"
};

static const std::set<std::string> EDITOR_BUILD_ACTIONS = {
    "trigger_build", "live_compile"
};

static const std::set<std::string> EDITOR_READ_ACTIONS = {
    "get_build_errors",
    "get_build_status",
    "get_build_summary",
    "search_build_output",
    "get_recent_logs",
    "search_logs",
    "tail_log",
    "get_log_categories",
    "get_log_stats",
    "get_compile_output",
    "get_crash_context",
};

// ============================================================================
// Globals
// ============================================================================

static std::string g_monolith_url;        // e.g. "http://localhost:9316/mcp"
static std::string g_monolith_host;       // e.g. "localhost"
static int         g_monolith_port = 0;   // e.g. 9316
static std::string g_monolith_path_mcp;   // e.g. "/mcp"
static std::string g_monolith_path_health;// e.g. "/health"

static bool g_split_editor_query = false;
static std::set<std::string> g_editor_action_allowlist;
static std::set<std::string> g_editor_action_denylist;

// State tracking
static std::optional<bool> g_monolith_was_up; // nullopt = unknown
static std::mutex g_stdout_lock;
static std::unordered_map<std::string, double> g_recent_tool_calls;

static const std::vector<std::string> CORE_QUERY_TOOLS = {
    "ai_query",
    "animation_query",
    "asset_query",
    "audio_query",
    "blueprint_query",
    "chaos_fracture_query",
    "chooser_query",
    "cloth_query",
    "collection_query",
    "combograph_query",
    "config_query",
    "context_query",
    "dataflow_query",
    "editor_query",
    "gamefeatures_query",
    "gas_query",
    "hlod_query",
    "input_query",
    "interchange_query",
    "level_instance_query",
    "level_query",
    "level_sequence_query",
    "localization_query",
    "logicdriver_query",
    "material_query",
    "mesh_query",
    "metahuman_query",
    "movie_render_query",
    "ndisplay_query",
    "niagara_query",
    "paper2d_query",
    "pcg_query",
    "project_query",
    "slate_query",
    "source_control_query",
    "source_query",
    "ui_query",
    "water_query",
    "world_conditions_query",
};

// ============================================================================
// Logging
// ============================================================================

static void log_msg(const std::string& msg)
{
    std::cerr << "[monolith-proxy] " << msg << std::endl;
}

// ============================================================================
// Utility: environment variable helpers
// ============================================================================

static std::string get_env(const char* name, const char* default_val = "")
{
    const char* val = std::getenv(name);
    return val ? std::string(val) : std::string(default_val);
}

static std::set<std::string> parse_csv_env(const char* name)
{
    std::set<std::string> result;
    std::string raw = get_env(name);
    if (raw.empty()) return result;

    std::istringstream ss(raw);
    std::string part;
    while (std::getline(ss, part, ','))
    {
        // trim whitespace
        size_t start = part.find_first_not_of(" \t\r\n");
        size_t end   = part.find_last_not_of(" \t\r\n");
        if (start == std::string::npos) continue;
        std::string trimmed = part.substr(start, end - start + 1);
        // lowercase
        std::transform(trimmed.begin(), trimmed.end(), trimmed.begin(),
                       [](unsigned char c){ return (char)std::tolower(c); });
        if (!trimmed.empty())
            result.insert(std::move(trimmed));
    }
    return result;
}

// ============================================================================
// Tool invocation daily log
// ============================================================================

static std::mutex g_tool_log_lock;
static uint64_t g_tool_log_sequence = 0;
static std::string g_process_instance_id;
static std::string g_previous_record_id;
static std::chrono::steady_clock::time_point g_previous_record_start;

static bool tool_log_enabled()
{
    return get_env("MONOLITH_TOOL_LOG_ENABLED", "1") != "0";
}

static std::string iso_local_now()
{
    auto now = std::chrono::system_clock::now();
    auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()) % 1000;
    std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm local_tm{};
    localtime_s(&local_tm, &t);

    TIME_ZONE_INFORMATION tzi;
    DWORD tz_result = GetTimeZoneInformation(&tzi);
    long bias_minutes = tzi.Bias;
    if (tz_result == TIME_ZONE_ID_DAYLIGHT) bias_minutes += tzi.DaylightBias;
    else if (tz_result == TIME_ZONE_ID_STANDARD) bias_minutes += tzi.StandardBias;
    long offset = -bias_minutes;
    char sign = offset >= 0 ? '+' : '-';
    offset = std::labs(offset);

    std::ostringstream out;
    out << std::put_time(&local_tm, "%Y-%m-%dT%H:%M:%S")
        << "." << std::setw(3) << std::setfill('0') << millis.count()
        << sign << std::setw(2) << std::setfill('0') << (offset / 60)
        << ":" << std::setw(2) << std::setfill('0') << (offset % 60);
    return out.str();
}

static std::string yyyymmdd_local()
{
    std::time_t t = std::time(nullptr);
    std::tm local_tm{};
    localtime_s(&local_tm, &t);
    std::ostringstream out;
    out << std::put_time(&local_tm, "%Y%m%d");
    return out.str();
}

static fs::path executable_dir()
{
    char buf[MAX_PATH];
    DWORD len = GetModuleFileNameA(nullptr, buf, MAX_PATH);
    if (len > 0 && len < MAX_PATH)
        return fs::path(buf).parent_path();
    return fs::current_path();
}

static fs::path proxy_log_root()
{
    std::string override_dir = get_env("MONOLITH_TOOL_LOG_DIR");
    if (!override_dir.empty()) return fs::path(override_dir);

    fs::path cur = executable_dir();
    for (int i = 0; i < 8 && !cur.empty(); ++i)
    {
        if (fs::exists(cur / "Monolith.uplugin")) return cur / "Logs";
        cur = cur.parent_path();
    }
    return fs::current_path() / "Logs";
}

static std::string hash_text(const std::string& text)
{
#ifdef _WIN32
    auto to_hex = [](const std::vector<unsigned char>& bytes)
    {
        std::ostringstream out;
        out << std::hex << std::setfill('0');
        for (unsigned char byte : bytes)
            out << std::setw(2) << static_cast<int>(byte);
        return out.str();
    };

    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    DWORD bytes_written = 0;
    DWORD hash_len = 0;
    std::vector<unsigned char> digest;

    NTSTATUS status = BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0);
    if (status >= 0)
        status = BCryptGetProperty(alg, BCRYPT_HASH_LENGTH,
            reinterpret_cast<PUCHAR>(&hash_len), sizeof(hash_len), &bytes_written, 0);
    if (status >= 0)
    {
        digest.resize(hash_len);
        status = BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0);
    }
    if (status >= 0 && !text.empty())
        status = BCryptHashData(hash,
            reinterpret_cast<PUCHAR>(const_cast<char*>(text.data())),
            static_cast<ULONG>(text.size()), 0);
    if (status >= 0)
        status = BCryptFinishHash(hash, digest.data(), hash_len, 0);
    if (hash) BCryptDestroyHash(hash);
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    if (status >= 0)
        return "sha256:" + to_hex(digest);
#endif

    std::hash<std::string> hasher;
    uint64_t a = hasher(text);
    uint64_t b = hasher("monolith:" + text);
    std::ostringstream out;
    out << "hash:" << std::hex << std::setw(16) << std::setfill('0') << a
        << std::setw(16) << std::setfill('0') << b;
    return out.str();
}

static std::string log_id(const std::string& prefix, const std::string& seed)
{
    std::string digest = hash_text(seed);
    size_t pos = digest.find(':');
    if (pos != std::string::npos) digest = digest.substr(pos + 1);
    if (digest.size() > 32) digest.resize(32);
    return prefix + "-" + digest;
}

static std::string process_instance_id()
{
    if (g_process_instance_id.empty())
        g_process_instance_id = log_id("proc", "proxy-cpp:" + std::to_string(GetCurrentProcessId()) + ":" + iso_local_now());
    return g_process_instance_id;
}

static std::pair<std::string, std::string> tool_namespace_action(const std::string& tool_name, const json& args)
{
    const std::string core_prefix = "monolith_";
    const std::string query_suffix = "_query";
    if (tool_name.rfind(core_prefix, 0) == 0)
        return {"monolith", tool_name.substr(core_prefix.size())};
    if (tool_name.size() > query_suffix.size() &&
        tool_name.compare(tool_name.size() - query_suffix.size(), query_suffix.size(), query_suffix) == 0)
        return {tool_name.substr(0, tool_name.size() - query_suffix.size()), args.value("action", "")};
    return {"", ""};
}

static std::string namespace_source_for_tool(const std::string& tool_name, const std::string& forwarded_name)
{
    if (tool_name != forwarded_name) return "alias_rewrite";
    if (tool_name.rfind("monolith_", 0) == 0) return "core_tool";
    if (tool_name.size() > 6 && tool_name.compare(tool_name.size() - 6, 6, "_query") == 0) return "domain_query";
    return "unknown";
}

static std::pair<std::string, std::string> infer_intent_for_tool(const std::string& ns, const std::string& action, const std::string& outcome)
{
    std::string act = action;
    std::transform(act.begin(), act.end(), act.begin(), [](unsigned char c){ return (char)std::tolower(c); });
    if (ns == "monolith" && (act == "find" || act == "discover")) return {"schema_discovery", "high"};
    if (act.find("health") != std::string::npos || act.find("status") != std::string::npos ||
        act.find("check") != std::string::npos || act.find("validate") != std::string::npos || act.find("test") != std::string::npos)
        return {"verification", "medium"};
    if (ns == "source" || act.find("source") != std::string::npos || act.find("symbol") != std::string::npos ||
        act.find("reference") != std::string::npos || act.find("caller") != std::string::npos || act.find("callee") != std::string::npos)
        return {"source_lookup", "medium"};
    if (ns == "project" || ns == "asset" || act.find("asset") != std::string::npos) return {"asset_search", "medium"};
    if (ns == "editor" && (act.find("build") != std::string::npos || act.find("compile") != std::string::npos ||
        act.find("log") != std::string::npos || act.find("crash") != std::string::npos))
        return {"build_diagnostics", "medium"};
    if (act.find("repair") != std::string::npos || act.find("reindex") != std::string::npos ||
        act.find("rebuild") != std::string::npos || act.find("snapshot") != std::string::npos)
        return {"maintenance", "medium"};
    if (outcome != "success") return {"error_recovery", "low"};
    if (act.rfind("create", 0) == 0 || act.rfind("set", 0) == 0 || act.rfind("add", 0) == 0 ||
        act.rfind("remove", 0) == 0 || act.rfind("delete", 0) == 0 || act.rfind("import", 0) == 0)
        return {"mutation", "medium"};
    return {"unknown", "low"};
}

static std::string workflow_step_for_intent(const std::string& intent, const std::string& outcome)
{
    if (outcome != "success") return "recover";
    if (intent == "schema_discovery") return "discover";
    if (intent == "verification") return "verify";
    if (intent == "maintenance") return "maintenance";
    if (intent == "mutation") return "execute";
    if (intent == "source_lookup" || intent == "asset_search" || intent == "build_diagnostics") return "inspect";
    return "unknown";
}

static bool is_sensitive_key(const std::string& key)
{
    std::string lower = key;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c){ return (char)std::tolower(c); });
    static const std::vector<std::string> fragments = {
        "authorization", "bearer", "token", "api_key", "apikey",
        "password", "passwd", "secret", "cookie", "private_key", "session_id"
    };
    for (const auto& fragment : fragments)
    {
        if (lower.find(fragment) != std::string::npos) return true;
    }
    return false;
}

static json redact_json(const json& value)
{
    if (value.is_object())
    {
        json out = json::object();
        for (auto it = value.begin(); it != value.end(); ++it)
            out[it.key()] = is_sensitive_key(it.key()) ? json("[REDACTED]") : redact_json(it.value());
        return out;
    }
    if (value.is_array())
    {
        json out = json::array();
        for (const auto& item : value) out.push_back(redact_json(item));
        return out;
    }
    if (value.is_string())
    {
        std::string text = value.get<std::string>();
        if (!text.empty() && (text.front() == '{' || text.front() == '['))
        {
            try { return redact_json(json::parse(text)); } catch (...) {}
        }
    }
    return value;
}

static size_t max_log_field_bytes()
{
    const std::string raw = get_env("MONOLITH_TOOL_LOG_MAX_FIELD_BYTES");
    if (raw.empty()) return 256 * 1024;
    try
    {
        const size_t parsed = static_cast<size_t>(std::stoull(raw));
        return std::max<size_t>(1024, std::min<size_t>(parsed, 16 * 1024 * 1024));
    }
    catch (...)
    {
        return 256 * 1024;
    }
}

static json bounded_json(const json& value, bool& truncated, size_t& original_bytes, std::string& hash)
{
    const size_t MaxBytes = max_log_field_bytes();
    std::string text = value.dump();
    original_bytes = text.size();
    if (text.size() <= MaxBytes)
    {
        truncated = false;
        hash.clear();
        return value;
    }
    truncated = true;
    hash = hash_text(text);
    return json{
        {"truncated", true},
        {"original_bytes", original_bytes},
        {"sha256", hash},
        {"preview", text.substr(0, MaxBytes)}
    };
}

static void prune_empty_json(json& value);

static bool is_empty_log_value(const json& value)
{
    return value.is_null()
        || (value.is_string() && value.get<std::string>().empty())
        || (value.is_array() && value.empty())
        || (value.is_object() && value.empty());
}

static void prune_empty_json(json& value)
{
    if (value.is_object())
    {
        for (auto it = value.begin(); it != value.end();)
        {
            prune_empty_json(it.value());
            if (is_empty_log_value(it.value()))
                it = value.erase(it);
            else
                ++it;
        }
    }
    else if (value.is_array())
    {
        for (auto& item : value) prune_empty_json(item);
        value.erase(std::remove_if(value.begin(), value.end(), is_empty_log_value), value.end());
    }
}

static json summarize_response(const json& response_json, size_t result_bytes, bool truncated)
{
    json summary = {
        {"result_bytes", result_bytes},
        {"truncated", truncated}
    };
    if (response_json.is_object())
    {
        if (response_json.contains("error"))
            summary["result_shape"] = "error";
        else if (response_json.contains("result") && response_json["result"].is_object() && response_json["result"].contains("content") && response_json["result"]["content"].is_array())
            summary["result_shape"] = "mcp_content";
        else if (response_json.contains("result") && response_json["result"].is_object())
            summary["result_shape"] = "object";
        else if (response_json.contains("result") && response_json["result"].is_array())
            summary["result_shape"] = "list";
        else if (!response_json.contains("result") || response_json["result"].is_null())
            summary["result_shape"] = "empty";
        else
            summary["result_shape"] = "text";

        json top_keys = json::array();
        for (auto it = response_json.begin(); it != response_json.end(); ++it) top_keys.push_back(it.key());
        summary["response_top_keys"] = top_keys;

        if (response_json.contains("error"))
        {
            const json& err = response_json["error"];
            if (err.is_object())
            {
                if (err.contains("code")) summary["jsonrpc_error_code"] = err["code"];
                summary["jsonrpc_error_message"] = err.value("message", "").substr(0, 240);
            }
            else
            {
                summary["jsonrpc_error_message"] = err.dump().substr(0, 240);
            }
        }

        if (response_json.contains("result"))
        {
            const json& result = response_json["result"];
            if (result.is_object())
            {
                json result_keys = json::array();
                for (auto it = result.begin(); it != result.end(); ++it) result_keys.push_back(it.key());
                summary["result_top_keys"] = result_keys;
                if (result.contains("isError")) summary["is_error"] = result.value("isError", false);
                if (result.contains("content") && result["content"].is_array()) summary["content_count"] = result["content"].size();
                if (result.contains("tools") && result["tools"].is_array()) summary["tools_count"] = result["tools"].size();
                if (result.contains("resources") && result["resources"].is_array()) summary["resources_count"] = result["resources"].size();
            }
            else
            {
                summary["result_type"] = result.type_name();
            }
        }
    }
    else
    {
        summary["response_type"] = response_json.type_name();
    }
    prune_empty_json(summary);
    return summary;
}

#ifdef _WIN32
static HANDLE acquire_log_lock_file(const fs::path& lock_path)
{
    const auto start = std::chrono::steady_clock::now();
    for (;;)
    {
        HANDLE lock_handle = CreateFileA(
            lock_path.string().c_str(),
            GENERIC_WRITE,
            0,
            nullptr,
            CREATE_NEW,
            FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE,
            nullptr);
        if (lock_handle != INVALID_HANDLE_VALUE)
            return lock_handle;

        const DWORD error = GetLastError();
        if (error != ERROR_FILE_EXISTS && error != ERROR_ALREADY_EXISTS && error != ERROR_SHARING_VIOLATION && error != ERROR_ACCESS_DENIED)
            throw std::runtime_error("could not create log lock file");

        WIN32_FILE_ATTRIBUTE_DATA attr{};
        if (GetFileAttributesExA(lock_path.string().c_str(), GetFileExInfoStandard, &attr))
        {
            FILETIME now_ft{};
            GetSystemTimeAsFileTime(&now_ft);
            ULARGE_INTEGER now{};
            now.LowPart = now_ft.dwLowDateTime;
            now.HighPart = now_ft.dwHighDateTime;
            ULARGE_INTEGER modified{};
            modified.LowPart = attr.ftLastWriteTime.dwLowDateTime;
            modified.HighPart = attr.ftLastWriteTime.dwHighDateTime;
            if (now.QuadPart > modified.QuadPart && now.QuadPart - modified.QuadPart > 30ULL * 1000ULL * 1000ULL * 10ULL)
                DeleteFileA(lock_path.string().c_str());
        }
        else if (GetLastError() == ERROR_FILE_NOT_FOUND)
        {
            continue;
        }

        if (std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count() > 5000)
            throw std::runtime_error("timed out acquiring tool log lock");
        Sleep(25);
    }
}
#endif

static void append_proxy_log(const json& record)
{
    if (!tool_log_enabled()) return;
    try
    {
        std::lock_guard<std::mutex> guard(g_tool_log_lock);
        json cleaned = record;
        prune_empty_json(cleaned);
        fs::path path = proxy_log_root() / yyyymmdd_local() / "proxy.jsonl";
        fs::create_directories(path.parent_path());
#ifdef _WIN32
        const fs::path lock_path = path.string() + ".lock";
        HANDLE lock_handle = acquire_log_lock_file(lock_path);
        try
        {
            std::ofstream out(path, std::ios::app | std::ios::binary);
            out << cleaned.dump() << "\n";
            if (!out) throw std::runtime_error("could not append daily log");
        }
        catch (...)
        {
            CloseHandle(lock_handle);
            throw;
        }
        CloseHandle(lock_handle);
#else
        std::ofstream out(path, std::ios::app | std::ios::binary);
        out << cleaned.dump() << "\n";
        if (!out) throw std::runtime_error("could not append daily log");
#endif
    }
    catch (const std::exception& e)
    {
        log_msg(std::string("Tool daily log failed: ") + e.what());
    }
}

static std::string retry_signature_for(const std::string& tool_name, const json& args)
{
    json payload;
    payload["tool"] = tool_name;
    payload["arguments"] = redact_json(args);
    return hash_text(payload.dump());
}

static void log_proxy_tools_call(
    const json& msg,
    const std::string& tool_name,
    const std::string& forwarded_name,
    const json& args,
    const std::string& response,
    const std::string& start_time,
    const std::chrono::steady_clock::time_point& start_clock,
    bool repeated,
    const std::string& retry_signature,
    const std::string& trace_id,
    const std::string& span_id,
    const json& phase_timing_input)
{
    if (!tool_log_enabled()) return;

    const auto log_prepare_start = std::chrono::steady_clock::now();
    const auto end_clock = std::chrono::steady_clock::now();
    const double duration_ms = std::chrono::duration<double, std::milli>(end_clock - start_clock).count();
    json response_json = response;
    try { response_json = json::parse(response); } catch (...) {}

    bool args_truncated = false;
    bool response_truncated = false;
    size_t arg_bytes = 0;
    size_t result_bytes = 0;
    std::string arg_hash;
    std::string result_hash;
    json bounded_args = bounded_json(redact_json(args), args_truncated, arg_bytes, arg_hash);
    json bounded_response = bounded_json(redact_json(response_json), response_truncated, result_bytes, result_hash);

    std::string outcome = "success";
    std::string error_class;
    json tags = json::array();
    int error_code = 0;
    if (response_json.is_object() && response_json.contains("error"))
    {
        outcome = "jsonrpc_error";
        const json& err = response_json["error"];
        if (err.is_object() && err.contains("code") && err["code"].is_number_integer())
            error_code = err["code"].get<int>();
        std::string message = err.is_object() ? err.value("message", "") : err.dump();
        std::string lower = message;
        std::transform(lower.begin(), lower.end(), lower.begin(),
                       [](unsigned char c){ return (char)std::tolower(c); });
        if (lower.find("unknown action") != std::string::npos || lower.find("method not found") != std::string::npos)
        {
            error_class = "unknown_action";
            tags.push_back("missing_action");
        }
        else if (lower.find("missing required") != std::string::npos || lower.find("invalid param") != std::string::npos)
        {
            error_class = "missing_param";
            tags.push_back("schema_confusing");
        }
        else
        {
            error_class = "jsonrpc_error";
        }
    }
    else if (response_json.is_object() && response_json.contains("result") && response_json["result"].value("isError", false))
    {
        outcome = "tool_error";
        std::string lower = response_json["result"].dump();
        std::transform(lower.begin(), lower.end(), lower.begin(),
                       [](unsigned char c){ return (char)std::tolower(c); });
        if (lower.find("not available") != std::string::npos || lower.find("not running") != std::string::npos)
        {
            outcome = "editor_unavailable";
            error_class = "editor_unavailable";
            tags.push_back("editor_unavailable");
        }
        else if (lower.find("blocked") != std::string::npos)
        {
            error_class = "profile_blocked";
            tags.push_back("profile_blocked");
        }
        else
        {
            error_class = "tool_error";
        }
    }

    if (repeated) tags.push_back("repeated_call");
    if (duration_ms > 5000.0) tags.push_back("slow_action");
    if (args_truncated || response_truncated) tags.push_back("large_result");
    json return_summary = summarize_response(response_json, result_bytes, args_truncated || response_truncated);

    json redaction = {
        {"argument_bytes", arg_bytes},
        {"result_bytes", result_bytes}
    };
    if (args_truncated || response_truncated) redaction["truncated"] = true;
    if (!arg_hash.empty()) redaction["argument_sha256"] = arg_hash;
    if (!result_hash.empty()) redaction["result_sha256"] = result_hash;

    json agent_signal = {
        {"outcome", outcome},
        {"hints_returned", 0}
    };
    if (error_code != 0) agent_signal["error_code"] = error_code;
    if (!error_class.empty()) agent_signal["error_class"] = error_class;
    if (repeated) agent_signal["repeat_within_window"] = true;
    if (!tags.empty()) agent_signal["improvement_tags"] = tags;

    auto [context_namespace, context_action] = tool_namespace_action(forwarded_name, args);
    auto [intent, confidence] = infer_intent_for_tool(context_namespace, context_action, outcome);
    json routing_context = {
        {"decision_source", repeated ? "fallback" : "direct"},
        {"namespace_source", namespace_source_for_tool(tool_name, forwarded_name)},
        {"inferred_intent", intent},
        {"intent_confidence", confidence}
    };
    json workflow = {
        {"step", workflow_step_for_intent(intent, outcome)}
    };

    json return_record = {
        {"response", bounded_response}
    };
    json response_id = response_json.is_object() ? response_json.value("id", json(nullptr)) : json(nullptr);
    if (response_id != msg.value("id", json(nullptr))) return_record["jsonrpc_id"] = response_id;

    uint64_t sequence = 0;
    std::string record_id;
    std::string previous_record_id;
    double time_since_previous_ms = 0.0;
    bool has_previous_record = false;
    const std::string proc_instance_id = process_instance_id();
    {
        std::lock_guard<std::mutex> guard(g_tool_log_lock);
        sequence = ++g_tool_log_sequence;
        record_id = log_id("rec", proc_instance_id + ":proxy:" + std::to_string(sequence) + ":" + trace_id + ":" + span_id + ":" + start_time);
        previous_record_id = g_previous_record_id;
        has_previous_record = !previous_record_id.empty();
        if (has_previous_record)
            time_since_previous_ms = std::chrono::duration<double, std::milli>(start_clock - g_previous_record_start).count();
        g_previous_record_id = record_id;
        g_previous_record_start = start_clock;
    }
    json phase_timing = phase_timing_input.is_object() ? phase_timing_input : json::object();
    phase_timing["log_prepare_ms"] = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - log_prepare_start).count();

    json record = {
        {"format_version", 3},
        {"surface", "proxy"},
        {"record_id", record_id},
        {"sequence", sequence},
        {"trace_id", trace_id},
        {"span_id", span_id},
        {"session_key", "stateless"},
        {"process_instance_id", proc_instance_id},
        {"call_index", sequence},
        {"previous_record_id", previous_record_id},
        {"time_since_previous_ms", has_previous_record ? json(time_since_previous_ms) : json(nullptr)},
        {"start_time", start_time},
        {"end_time", iso_local_now()},
        {"duration_ms", duration_ms},
        {"pid", static_cast<int>(GetCurrentProcessId())},
        {"thread_id", "main"},
        {"status", outcome == "success" ? "success" : "error"},
        {"client", {
            {"proxy_runtime", "cpp"},
            {"proxy_version", PROXY_VERSION}
        }},
        {"routing_context", routing_context},
        {"workflow", workflow},
        {"phase_timing", phase_timing},
        {"call", {
            {"jsonrpc_id", msg.value("id", json(nullptr))},
            {"tool_name_original", tool_name},
            {"tool_name_forwarded", forwarded_name},
            {"arguments", bounded_args},
            {"retry_signature", retry_signature}
        }},
        {"return", return_record},
        {"return_summary", return_summary},
        {"redaction", redaction},
        {"agent_signal", agent_signal}
    };
    append_proxy_log(record);
}

// ============================================================================
// Utility: URL parsing
// ============================================================================

static void parse_monolith_url(const std::string& url)
{
    g_monolith_url = url;

    // Strip "http://"
    std::string rest = url;
    if (rest.rfind("http://", 0) == 0)
        rest = rest.substr(7);
    else if (rest.rfind("https://", 0) == 0)
        rest = rest.substr(8);

    // Split host:port/path
    auto slash_pos = rest.find('/');
    std::string host_port = (slash_pos != std::string::npos) ? rest.substr(0, slash_pos) : rest;
    g_monolith_path_mcp = (slash_pos != std::string::npos) ? rest.substr(slash_pos) : "/mcp";

    auto colon_pos = host_port.find(':');
    if (colon_pos != std::string::npos)
    {
        g_monolith_host = host_port.substr(0, colon_pos);
        g_monolith_port = std::stoi(host_port.substr(colon_pos + 1));
    }
    else
    {
        g_monolith_host = host_port;
        g_monolith_port = 80;
    }

    // Derive health path: replace trailing /mcp with /health
    g_monolith_path_health = g_monolith_path_mcp;
    auto mcp_pos = g_monolith_path_health.rfind("/mcp");
    if (mcp_pos != std::string::npos)
        g_monolith_path_health = g_monolith_path_health.substr(0, mcp_pos) + "/health";
    else
        g_monolith_path_health = "/health";
}

// ============================================================================
// Time helper
// ============================================================================

static double now_seconds()
{
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

// ============================================================================
// WinHTTP client
// ============================================================================

static std::wstring to_wide(const std::string& s)
{
    if (s.empty()) return {};
    int sz = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring ws(sz, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &ws[0], sz);
    return ws;
}

// POST JSON to Monolith. Returns response body or empty string on failure.
static std::string post_monolith(const std::string& body, double timeout_sec = TIMEOUT)
{
    HINTERNET hSession = WinHttpOpen(
        L"MonolithProxy/1.0",
        WINHTTP_ACCESS_TYPE_NO_PROXY,
        WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS,
        0);
    if (!hSession) return {};

    std::wstring whost = to_wide(g_monolith_host);
    HINTERNET hConnect = WinHttpConnect(hSession, whost.c_str(), (INTERNET_PORT)g_monolith_port, 0);
    if (!hConnect) { WinHttpCloseHandle(hSession); return {}; }

    std::wstring wpath = to_wide(g_monolith_path_mcp);
    HINTERNET hRequest = WinHttpOpenRequest(
        hConnect, L"POST", wpath.c_str(),
        nullptr, WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES, 0);
    if (!hRequest) { WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession); return {}; }

    // Set timeouts (milliseconds)
    DWORD timeout_ms = (DWORD)(timeout_sec * 1000);
    WinHttpSetTimeouts(hRequest, timeout_ms, timeout_ms, timeout_ms, timeout_ms);

    // Send
    const wchar_t* hdrs = L"Content-Type: application/json";
    BOOL ok = WinHttpSendRequest(
        hRequest, hdrs, (DWORD)-1,
        (LPVOID)body.c_str(), (DWORD)body.size(),
        (DWORD)body.size(), 0);

    if (!ok || !WinHttpReceiveResponse(hRequest, nullptr))
    {
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return {};
    }

    // Read response
    std::string response;
    DWORD bytesAvailable = 0;
    while (WinHttpQueryDataAvailable(hRequest, &bytesAvailable) && bytesAvailable > 0)
    {
        std::string chunk(bytesAvailable, '\0');
        DWORD bytesRead = 0;
        WinHttpReadData(hRequest, &chunk[0], bytesAvailable, &bytesRead);
        response.append(chunk.c_str(), bytesRead);
    }

    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);

    return response;
}

// GET health endpoint. Returns true if 200 OK.
static bool check_monolith_up()
{
    HINTERNET hSession = WinHttpOpen(
        L"MonolithProxy/1.0",
        WINHTTP_ACCESS_TYPE_NO_PROXY,
        WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS,
        0);
    if (!hSession) return false;

    std::wstring whost = to_wide(g_monolith_host);
    HINTERNET hConnect = WinHttpConnect(hSession, whost.c_str(), (INTERNET_PORT)g_monolith_port, 0);
    if (!hConnect) { WinHttpCloseHandle(hSession); return false; }

    std::wstring wpath = to_wide(g_monolith_path_health);
    HINTERNET hRequest = WinHttpOpenRequest(
        hConnect, L"GET", wpath.c_str(),
        nullptr, WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES, 0);
    if (!hRequest) { WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession); return false; }

    // 3-second timeout for health check
    DWORD timeout_ms = 3000;
    WinHttpSetTimeouts(hRequest, timeout_ms, timeout_ms, timeout_ms, timeout_ms);

    BOOL ok = WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                  WINHTTP_NO_REQUEST_DATA, 0, 0, 0);
    if (!ok || !WinHttpReceiveResponse(hRequest, nullptr))
    {
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return false;
    }

    DWORD statusCode = 0;
    DWORD statusSize = sizeof(statusCode);
    WinHttpQueryHeaders(hRequest,
        WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
        WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &statusSize, WINHTTP_NO_HEADER_INDEX);

    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);

    return statusCode == 200;
}

// ============================================================================
// JSON-RPC helpers
// ============================================================================

static std::string make_result(const json& id, const json& result)
{
    json resp;
    resp["jsonrpc"] = "2.0";
    resp["id"] = id;
    resp["result"] = result;
    return resp.dump();
}

static std::string make_tool_error(const json& id, const std::string& message)
{
    json resp;
    resp["jsonrpc"] = "2.0";
    resp["id"] = id;
    resp["result"] = {
        {"content", json::array({{{"type", "text"}, {"text", message}}})},
        {"isError", true}
    };
    return resp.dump();
}

static std::string make_jsonrpc_error(const json& id, int code, const std::string& message)
{
    json resp;
    resp["jsonrpc"] = "2.0";
    resp["id"] = id;
    resp["error"] = {{"code", code}, {"message", message}};
    return resp.dump();
}

// ============================================================================
// Stable tools/list fallback
// ============================================================================

static std::string sanitize_cache_part(std::string value)
{
    for (char& c : value)
    {
        const bool ok =
            (c >= 'a' && c <= 'z') ||
            (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') ||
            c == '-' || c == '_';
        if (!ok)
            c = '_';
    }
    return value;
}

static std::string tools_cache_path()
{
    std::string base = get_env("LOCALAPPDATA");
    if (base.empty())
        base = get_env("TEMP", ".");

    std::string dir = base + "\\Monolith";
    CreateDirectoryA(dir.c_str(), nullptr);

    return dir + "\\monolith_proxy_tools_" +
        sanitize_cache_part(g_monolith_host) + "_" +
        std::to_string(g_monolith_port) + ".json";
}

static json make_query_tool_schema()
{
    return {
        {"type", "object"},
        {"properties", {
            {"action", {
                {"type", "string"},
                {"description", "The action to execute. Use monolith_discover first when the editor is available."}
            }},
            {"params", {
                {"type", "object"},
                {"description", "Parameters for the selected action."}
            }}
        }},
        {"required", json::array({"action"})}
    };
}

static json make_empty_object_schema()
{
    return {
        {"type", "object"},
        {"properties", json::object()}
    };
}

static json make_tool(const std::string& name, const std::string& description, const json& schema)
{
    return {
        {"name", name},
        {"description", description},
        {"inputSchema", schema}
    };
}

static json make_seed_tools()
{
    json tools = json::array();

    for (const std::string& name : CORE_QUERY_TOOLS)
    {
        std::string domain = name;
        const std::string suffix = "_query";
        if (domain.size() > suffix.size() &&
            domain.compare(domain.size() - suffix.size(), suffix.size(), suffix) == 0)
        {
            domain.resize(domain.size() - suffix.size());
        }

        tools.push_back(make_tool(
            name,
            "Query the " + domain + " domain. The editor may be offline at session start; retry after Monolith is healthy.",
            make_query_tool_schema()));
    }

    tools.push_back(make_tool(
        "monolith_discover",
        "List available tool namespaces and their actions. Pass namespace and optional category to filter.",
        {
            {"type", "object"},
            {"properties", {
                {"namespace", {
                    {"type", "string"},
                    {"description", "Optional: filter to a specific namespace"}
                }},
                {"category", {
                    {"type", "string"},
                    {"description", "Optional: filter actions within the namespace by category"}
                }}
            }}
        }));

    tools.push_back(make_tool(
        "monolith_status",
        "Get Monolith server health: version, uptime, port, registered action count, and module status.",
        make_empty_object_schema()));

    tools.push_back(make_tool(
        "monolith_update",
        "Check for or install Monolith updates from GitHub Releases.",
        {
            {"type", "object"},
            {"properties", {
                {"action", {
                    {"type", "string"},
                    {"description", "'check' to compare versions, 'install' to download and stage update"},
                    {"default", "check"}
                }}
            }}
        }));

    tools.push_back(make_tool(
        "monolith_reindex",
        "Re-index the Monolith project database. Requires the editor-side Monolith server.",
        make_empty_object_schema()));

    return tools;
}

static void write_tools_cache(const std::string& response)
{
    try
    {
        json payload = json::parse(response);
        auto result_it = payload.find("result");
        if (result_it == payload.end() || !result_it->is_object())
            return;

        auto tools_it = result_it->find("tools");
        if (tools_it == result_it->end() || !tools_it->is_array() || tools_it->empty())
            return;

        std::ofstream out(tools_cache_path(), std::ios::binary | std::ios::trunc);
        if (out)
            out << tools_it->dump();
    }
    catch (const std::exception& e)
    {
        log_msg(std::string("Failed to write tools/list cache: ") + e.what());
    }
}

static std::optional<json> read_tools_cache()
{
    try
    {
        std::ifstream in(tools_cache_path(), std::ios::binary);
        if (!in)
            return std::nullopt;

        json tools;
        in >> tools;
        if (!tools.is_array() || tools.empty())
            return std::nullopt;

        return tools;
    }
    catch (const std::exception& e)
    {
        log_msg(std::string("Failed to read tools/list cache: ") + e.what());
        return std::nullopt;
    }
}

static std::string make_fallback_tools_list_response(const json& msg)
{
    if (auto cached = read_tools_cache())
    {
        log_msg("Monolith down during tools/list -- returning cached tools");
        return make_result(msg.value("id", json()), {{"tools", cached.value()}});
    }

    log_msg("Monolith down during tools/list -- returning seed tools");
    return make_result(msg.value("id", json()), {{"tools", make_seed_tools()}});
}

// ============================================================================
// stdout writing (thread-safe)
// ============================================================================

static void write_stdout(const std::string& msg)
{
    std::lock_guard<std::mutex> lock(g_stdout_lock);
    std::cout << msg << "\n";
    std::cout.flush();
}

// ============================================================================
// Dedup tracking
// ============================================================================

static std::string tool_signature(const json& msg)
{
    auto params_it = msg.find("params");
    if (params_it == msg.end() || !params_it->is_object())
        return {};

    auto name_it = params_it->find("name");
    if (name_it == params_it->end() || !name_it->is_string() || name_it->get<std::string>().empty())
        return {};

    // Build signature object using json (std::map-backed, sorts keys alphabetically)
    // This matches Python's json.dumps(sort_keys=True, separators=(",",":"))
    json sig;
    sig["name"] = *name_it;
    sig["arguments"] = params_it->value("arguments", json::object());

    // dump(-1) = compact, no spaces — matches Python separators=(",",":")
    return sig.dump(-1);
}

static bool is_repeated_tool_call(const json& msg)
{
    std::string sig = tool_signature(msg);
    if (sig.empty()) return false;

    auto it = g_recent_tool_calls.find(sig);
    if (it == g_recent_tool_calls.end()) return false;

    return (now_seconds() - it->second) < REPEAT_TOOL_CALL_WINDOW;
}

static void record_tool_call(const json& msg)
{
    std::string sig = tool_signature(msg);
    if (!sig.empty())
        g_recent_tool_calls[sig] = now_seconds();
}

// ============================================================================
// State check + health poll
// ============================================================================

static bool send_list_changed()
{
    try
    {
        json notification;
        notification["jsonrpc"] = "2.0";
        notification["method"] = "notifications/tools/list_changed";
        write_stdout(notification.dump());
        return true;
    }
    catch (...)
    {
        return false;
    }
}

static void check_monolith_state_change()
{
    bool is_up = check_monolith_up();

    if (g_monolith_was_up.has_value() && is_up != g_monolith_was_up.value())
    {
        const char* direction = is_up ? "online" : "offline";
        log_msg(std::string("Monolith went ") + direction + " -- sending tools/list_changed");
        send_list_changed();
    }

    g_monolith_was_up = is_up;
}

static void health_poll_thread()
{
    // Initial delay
    std::this_thread::sleep_for(
        std::chrono::milliseconds((int)(POLL_START_DELAY * 1000)));
    log_msg("Health poll started (interval=" + std::to_string((int)POLL_INTERVAL) + "s)");

    while (true)
    {
        try
        {
            check_monolith_state_change();
        }
        catch (...)
        {
            log_msg("Health poll error");
        }

        std::this_thread::sleep_for(
            std::chrono::milliseconds((int)(POLL_INTERVAL * 1000)));
    }
}

// ============================================================================
// Handlers
// ============================================================================

static std::string handle_initialize(const json& msg)
{
    std::string client_version = "2025-11-25";
    auto params_it = msg.find("params");
    if (params_it != msg.end() && params_it->is_object())
    {
        auto pv_it = params_it->find("protocolVersion");
        if (pv_it != params_it->end() && pv_it->is_string())
            client_version = pv_it->get<std::string>();
    }

    std::string version = (SUPPORTED_VERSIONS.count(client_version) > 0)
        ? client_version : "2025-11-25";

    json result;
    result["protocolVersion"] = version;
    result["capabilities"] = {{"tools", {{"listChanged", true}}}};
    result["serverInfo"] = {{"name", PROXY_NAME}, {"version", PROXY_VERSION}};
    result["instructions"] =
        "Monolith MCP proxy. Tools are forwarded to the Unreal Editor. "
        "If tools return errors about the editor not running, wait and retry.";

    return make_result(msg.value("id", json()), result);
}

static std::string handle_ping(const json& msg)
{
    return make_result(msg.value("id", json()), json::object());
}

static std::string handle_tools_list(const json& msg)
{
    std::string resp = post_monolith(msg.dump());

    if (!resp.empty())
    {
        if (g_split_editor_query)
        {
            try
            {
                json payload = json::parse(resp);
                auto result_it = payload.find("result");
                if (result_it != payload.end() && result_it->is_object())
                {
                    auto tools_it = result_it->find("tools");
                    if (tools_it != result_it->end() && tools_it->is_array())
                    {
                        json rewritten_tools = json::array();
                        for (auto& tool : *tools_it)
                        {
                            if (tool.is_object() && tool.value("name", "") == "editor_query")
                            {
                                // Create read tool
                                json read_tool = tool;
                                read_tool["name"] = "editor_read_query";
                                read_tool["description"] =
                                    "Read-only Unreal editor diagnostics and log access. "
                                    "Use for build status, build errors, build summary, compile output, crash context, "
                                    "and recent log queries. Never use this tool to trigger a build.";

                                // Create build tool
                                json build_tool = tool;
                                build_tool["name"] = "editor_build_query";
                                build_tool["description"] =
                                    "Mutating Unreal editor build actions only. "
                                    "Use only when the user explicitly asks to trigger a full build or a Live Coding compile.";

                                rewritten_tools.push_back(std::move(read_tool));
                                rewritten_tools.push_back(std::move(build_tool));
                                continue;
                            }
                            rewritten_tools.push_back(tool);
                        }
                        (*result_it)["tools"] = std::move(rewritten_tools);
                        resp = payload.dump();
                    }
                }
            }
            catch (const std::exception& e)
            {
                log_msg(std::string("Failed to rewrite tools/list response: ") + e.what());
            }
        }
        write_tools_cache(resp);
        return resp;
    }

    return make_fallback_tools_list_response(msg);
}

static std::string handle_tools_call(const json& msg)
{
    const std::string log_start_time = iso_local_now();
    const auto log_start_clock = std::chrono::steady_clock::now();
    const auto parse_start_clock = std::chrono::steady_clock::now();
    json id = msg.value("id", json());

    // Extract params (copy so we can modify)
    json params = msg.value("params", json::object());
    std::string tool_name = params.value("name", "unknown");
    std::string forwarded_name = tool_name;
    json args = params.value("arguments", json::object());
    if (args.is_null()) args = json::object();
    const double parse_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - parse_start_clock).count();
    const auto dedup_start_clock = std::chrono::steady_clock::now();
    const std::string retry_signature = retry_signature_for(tool_name, args);
    const std::string trace_id = log_id("trace", log_start_time + ":" + std::to_string(GetCurrentProcessId()) + ":main:" + retry_signature);
    const std::string span_id = log_id("span", trace_id + ":proxy:" + id.dump() + ":" + log_start_time);
    const double dedup_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - dedup_start_clock).count();
    json phase_timing = {
        {"parse_ms", parse_ms},
        {"dedup_ms", dedup_ms}
    };
    bool repeated_for_log = false;
    auto finish = [&](const std::string& response) -> std::string
    {
        log_proxy_tools_call(
            msg,
            tool_name,
            forwarded_name,
            args,
            response,
            log_start_time,
            log_start_clock,
            repeated_for_log,
            retry_signature,
            trace_id,
            span_id,
            phase_timing);
        return response;
    };

    // --- Split editor_query handling ---
    if (tool_name == "editor_read_query" || tool_name == "editor_build_query")
    {
        // Validate action arg exists
        std::string action;
        auto action_it = args.find("action");
        if (action_it == args.end() || !action_it->is_string() || action_it->get<std::string>().empty())
        {
            return finish(make_tool_error(id,
                "Tool '" + tool_name + "' requires an 'action' string argument."));
        }
        action = action_it->get<std::string>();

        // Normalize
        std::string normalized = action;
        // trim
        size_t s = normalized.find_first_not_of(" \t\r\n");
        size_t e = normalized.find_last_not_of(" \t\r\n");
        if (s != std::string::npos) normalized = normalized.substr(s, e - s + 1);
        // lowercase
        std::transform(normalized.begin(), normalized.end(), normalized.begin(),
                       [](unsigned char c){ return (char)std::tolower(c); });

        if (tool_name == "editor_read_query" && EDITOR_BUILD_ACTIONS.count(normalized))
        {
            return finish(make_tool_error(id,
                "Tool '" + tool_name + "' is read-only. Use the build-capable editor-open preset if you intentionally want '" + action + "'."));
        }
        if (tool_name == "editor_build_query" && !EDITOR_BUILD_ACTIONS.count(normalized))
        {
            // Build sorted action list string
            std::string actions_str;
            for (auto it = EDITOR_BUILD_ACTIONS.begin(); it != EDITOR_BUILD_ACTIONS.end(); ++it)
            {
                if (!actions_str.empty()) actions_str += ", ";
                actions_str += *it;
            }
            return finish(make_tool_error(id,
                "Tool '" + tool_name + "' only supports build actions (" + actions_str + "). "
                "Use 'editor_read_query' for diagnostics and logs."));
        }

        // Remap to editor_query for forwarding
        forwarded_name = "editor_query";
        params["name"] = forwarded_name;
    }
    else if (tool_name == "editor_query")
    {
        auto action_it = args.find("action");
        if (action_it != args.end() && action_it->is_string() && !action_it->get<std::string>().empty())
        {
            std::string action = action_it->get<std::string>();
            std::string normalized = action;
            size_t s = normalized.find_first_not_of(" \t\r\n");
            size_t e = normalized.find_last_not_of(" \t\r\n");
            if (s != std::string::npos) normalized = normalized.substr(s, e - s + 1);
            std::transform(normalized.begin(), normalized.end(), normalized.begin(),
                           [](unsigned char c){ return (char)std::tolower(c); });

            if (g_split_editor_query && EDITOR_BUILD_ACTIONS.count(normalized))
            {
                return finish(make_tool_error(id,
                    "Generic 'editor_query' is not available in split-editor mode for build actions. "
                    "Use 'editor_build_query' from the build-capable preset for '" + action + "'."));
            }
        }
    }

    // --- Dedup check ---
    // Build the message we'll actually forward (with possibly rewritten params)
    const auto rewrite_start_clock = std::chrono::steady_clock::now();
    json forwarded_msg = msg;
    forwarded_msg["params"] = params;
    forwarded_msg["_monolith_trace_id"] = trace_id;
    forwarded_msg["_monolith_parent_span_id"] = span_id;
    forwarded_msg["_monolith_session_key"] = "stateless";
    auto [context_namespace, context_action] = tool_namespace_action(forwarded_name, args);
    auto [intent, confidence] = infer_intent_for_tool(context_namespace, context_action, "unknown");
    forwarded_msg["_monolith_routing_context"] = {
        {"decision_source", "direct"},
        {"namespace_source", namespace_source_for_tool(tool_name, forwarded_name)},
        {"inferred_intent", intent},
        {"intent_confidence", confidence}
    };
    phase_timing["rewrite_ms"] = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - rewrite_start_clock).count();

    if (is_repeated_tool_call(forwarded_msg))
    {
        repeated_for_log = true;
        return finish(make_tool_error(id,
            "Tool '" + tool_name + "' with the same arguments was just called. "
            "Reuse the previous result and answer the user instead of repeating the same call."));
    }

    // --- Allowlist/denylist check ---
    if (forwarded_name == "editor_query")
    {
        auto action_it = args.find("action");
        if (action_it != args.end() && action_it->is_string() && !action_it->get<std::string>().empty())
        {
            std::string action = action_it->get<std::string>();
            std::string normalized = action;
            size_t s = normalized.find_first_not_of(" \t\r\n");
            size_t e = normalized.find_last_not_of(" \t\r\n");
            if (s != std::string::npos) normalized = normalized.substr(s, e - s + 1);
            std::transform(normalized.begin(), normalized.end(), normalized.begin(),
                           [](unsigned char c){ return (char)std::tolower(c); });

            if (!g_editor_action_allowlist.empty() && !g_editor_action_allowlist.count(normalized))
            {
                return finish(make_tool_error(id,
                    "Monolith editor action '" + action + "' is blocked by this preset. "
                    "Switch to the build-capable editor-open preset if you want mutating editor actions."));
            }
            if (g_editor_action_denylist.count(normalized))
            {
                return finish(make_tool_error(id,
                    "Monolith editor action '" + action + "' is blocked by this preset. "
                    "Use the build-capable editor-open preset when you intentionally want compile or build actions."));
            }
        }
    }

    // --- Record and forward ---
    record_tool_call(forwarded_msg);

    const auto http_start_clock = std::chrono::steady_clock::now();
    std::string resp = post_monolith(forwarded_msg.dump());
    phase_timing["http_roundtrip_ms"] = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - http_start_clock).count();
    if (!resp.empty())
        return finish(resp);

    const auto fallback_start_clock = std::chrono::steady_clock::now();
    std::string unavailable = make_tool_error(id,
        "Monolith MCP is not available (Unreal Editor not running). "
        "Tool '" + tool_name + "' cannot execute. Start the editor and try again.");
    phase_timing["fallback_ms"] = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - fallback_start_clock).count();
    return finish(unavailable);
}

// ============================================================================
// Main loop
// ============================================================================

int main()
{
    // Binary-safe stdin/stdout on Windows
    _setmode(_fileno(stdin),  _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);

    // Parse configuration from environment
    std::string url = get_env("MONOLITH_URL", "http://localhost:9316/mcp");
    parse_monolith_url(url);

    g_split_editor_query   = get_env("MONOLITH_SPLIT_EDITOR_QUERY", "0") == "1";
    g_editor_action_allowlist = parse_csv_env("MONOLITH_EDITOR_ACTION_ALLOWLIST");
    g_editor_action_denylist  = parse_csv_env("MONOLITH_EDITOR_ACTION_DENYLIST");

    log_msg(std::string("Started. Forwarding to ") + g_monolith_url);

    // Start background health poll thread (detached = daemon)
    std::thread poller(health_poll_thread);
    poller.detach();

    // Main stdin read loop
    std::string line;
    while (std::getline(std::cin, line))
    {
        // Trim
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n'))
            line.pop_back();
        if (line.empty())
            continue;

        // Parse JSON
        json msg;
        try
        {
            msg = json::parse(line);
        }
        catch (const json::parse_error& e)
        {
            log_msg(std::string("Bad JSON: ") + e.what());
            continue;
        }

        std::string method = msg.value("method", "");
        bool has_id = msg.contains("id");
        std::string response;

        if (method == "initialize")
        {
            response = handle_initialize(msg);
            log_msg("Initialized");
        }
        else if (method == "notifications/initialized" || method == "initialized")
        {
            // Notification -- no response. Check if Monolith is up.
            check_monolith_state_change();
        }
        else if (method == "ping")
        {
            response = handle_ping(msg);
        }
        else if (method == "tools/list")
        {
            check_monolith_state_change();
            response = handle_tools_list(msg);
        }
        else if (method == "tools/call")
        {
            response = handle_tools_call(msg);
        }
        else
        {
            // Forward unknown methods to Monolith
            std::string resp = post_monolith(msg.dump());
            if (!resp.empty())
            {
                response = resp;
            }
            else if (has_id)
            {
                response = make_jsonrpc_error(msg["id"], -32601,
                    "Method not found: " + method);
            }
            // else: notification with no id, silently drop
        }

        if (!response.empty())
            write_stdout(response);
    }

    // EOF on stdin -- clean exit
    log_msg("stdin closed, exiting");
    return 0;
}
