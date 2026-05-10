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
#include <io.h>
#include <fcntl.h>

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

#include <nlohmann/json.hpp>

using json = nlohmann::json;

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
    "blueprint_query",
    "material_query",
    "animation_query",
    "niagara_query",
    "editor_query",
    "config_query",
    "project_query",
    "source_query",
    "ui_query",
    "mesh_query",
    "gas_query",
    "combograph_query",
    "ai_query",
    "logicdriver_query",
    "audio_query",
    "level_sequence_query",
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
    json id = msg.value("id", json());

    // Extract params (copy so we can modify)
    json params = msg.value("params", json::object());
    std::string tool_name = params.value("name", "unknown");
    std::string forwarded_name = tool_name;
    json args = params.value("arguments", json::object());
    if (args.is_null()) args = json::object();

    // --- Split editor_query handling ---
    if (tool_name == "editor_read_query" || tool_name == "editor_build_query")
    {
        // Validate action arg exists
        std::string action;
        auto action_it = args.find("action");
        if (action_it == args.end() || !action_it->is_string() || action_it->get<std::string>().empty())
        {
            return make_tool_error(id,
                "Tool '" + tool_name + "' requires an 'action' string argument.");
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
            return make_tool_error(id,
                "Tool '" + tool_name + "' is read-only. Use the build-capable editor-open preset if you intentionally want '" + action + "'.");
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
            return make_tool_error(id,
                "Tool '" + tool_name + "' only supports build actions (" + actions_str + "). "
                "Use 'editor_read_query' for diagnostics and logs.");
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
                return make_tool_error(id,
                    "Generic 'editor_query' is not available in split-editor mode for build actions. "
                    "Use 'editor_build_query' from the build-capable preset for '" + action + "'.");
            }
        }
    }

    // --- Dedup check ---
    // Build the message we'll actually forward (with possibly rewritten params)
    json forwarded_msg = msg;
    forwarded_msg["params"] = params;

    if (is_repeated_tool_call(forwarded_msg))
    {
        return make_tool_error(id,
            "Tool '" + tool_name + "' with the same arguments was just called. "
            "Reuse the previous result and answer the user instead of repeating the same call.");
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
                return make_tool_error(id,
                    "Monolith editor action '" + action + "' is blocked by this preset. "
                    "Switch to the build-capable editor-open preset if you want mutating editor actions.");
            }
            if (g_editor_action_denylist.count(normalized))
            {
                return make_tool_error(id,
                    "Monolith editor action '" + action + "' is blocked by this preset. "
                    "Use the build-capable editor-open preset when you intentionally want compile or build actions.");
            }
        }
    }

    // --- Record and forward ---
    record_tool_call(forwarded_msg);

    std::string resp = post_monolith(forwarded_msg.dump());
    if (!resp.empty())
        return resp;

    return make_tool_error(id,
        "Monolith MCP is not available (Unreal Editor not running). "
        "Tool '" + tool_name + "' cannot execute. Start the editor and try again.");
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
