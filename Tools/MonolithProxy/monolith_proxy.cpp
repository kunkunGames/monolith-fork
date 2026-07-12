/*
 * Monolith MCP stdio-to-HTTP proxy (C++ / WinHTTP).
 *
 * Sits between Claude Code (stdio JSON-RPC) and Monolith (HTTP on localhost).
 * Handles initialize locally, forwards tool calls to Monolith.
 * Survives editor restarts -- proxy process never dies.
 * Background health poll auto-detects when the editor comes online.
 *
 * Build: see build.bat or CMakeLists.txt
 * Usage: run Scripts/onboard_monolith.ps1 so the client config receives the
 * immutable image selected by Binaries/monolith_proxy.current.json.
 */

// ============================================================================
// Includes
// ============================================================================

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <windows.h>
#include <iphlpapi.h>
#include <winhttp.h>
#include <bcrypt.h>
#include <intrin.h>
#include <io.h>
#include <fcntl.h>
#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "iphlpapi.lib")

#include <iostream>
#include <string>
#include <sstream>
#include <thread>
#include <mutex>
#include <chrono>
#include <ctime>
#include <set>
#include <map>
#include <unordered_map>
#include <algorithm>
#include <optional>
#include <cstdlib>
#include <cstdio>
#include <fstream>
#include <vector>
#include <filesystem>
#include <iomanip>
#include <stdexcept>
#include <atomic>
#include <cmath>

#include <nlohmann/json.hpp>

#include "monolith_proxy_offline.h"

#pragma comment(lib, "bcrypt.lib")

using json = nlohmann::json;
namespace fs = std::filesystem;

#ifndef SOURCE_HASH
#define SOURCE_HASH "dev"
#endif

// ============================================================================
// Constants
// ============================================================================

static const char* PROXY_NAME    = "monolith-proxy";
static const char* PROXY_VERSION = "1.1.4";

static constexpr double TIMEOUT                  = 30.0;
static constexpr double TOOLS_LIST_TIMEOUT       = 0.75;
static constexpr double READ_FALLBACK_LIVE_TIMEOUT = 3.0;
static constexpr DWORD REQUEST_HEALTH_TIMEOUT_MS = 250;
static constexpr double POLL_INTERVAL            = 5.0;
static constexpr double POLL_START_DELAY         = 3.0;
static constexpr double REPEAT_TOOL_CALL_WINDOW  = 3.0;
static constexpr double RISKY_REPEAT_TOOL_CALL_WINDOW = 60.0;
static constexpr size_t MAX_RECENT_TOOL_CALLS    = 1024;
static constexpr size_t MAX_LIVE_RESPONSE_BYTES = 16 * 1024 * 1024;

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
static bool        g_monolith_secure = false;
static bool        g_monolith_loopback = true;
static std::string g_expected_project_root;

static bool g_split_editor_query = false;
static bool g_offline_fallback_enabled = true;
static std::set<std::string> g_editor_action_allowlist;
static std::set<std::string> g_editor_action_denylist;

// State tracking
static std::optional<bool> g_monolith_was_up; // nullopt = unknown
static std::mutex g_monolith_state_lock;
static std::mutex g_identity_state_lock;
static int64_t g_identity_pid = -1;
static bool g_identity_matches = false;
static unsigned g_transport_failure_count = 0;
static std::chrono::steady_clock::time_point g_transport_retry_after{};
static std::atomic<bool> g_stop_health_poll{false};
static std::mutex g_stdout_lock;
struct RecentToolCall
{
    bool in_flight = true;
    double completed_at = 0.0;
    double repeat_window_seconds = REPEAT_TOOL_CALL_WINDOW;
};
static std::unordered_map<std::string, RecentToolCall> g_recent_tool_calls;
static std::mutex g_recent_tool_calls_lock;

// Call-log state (Phase 4 / survivor F)
//
// NOTE: Saved/Logs/MonolithCalls.jsonl is project-root-relative and excluded
// from crash zip generation by UE's crash reporter (Saved/Logs/ tail capture
// only includes editor logs, not arbitrary jsonl). If a crash collector pattern
// elsewhere DOES sweep Saved/Logs/*, the user should add MonolithCalls.jsonl to
// the exclusion list. Single-user local dev tool; no phone-home.
static bool      g_call_log_enabled = false;     // resolved once at startup
static HANDLE    g_call_log_handle  = INVALID_HANDLE_VALUE;
static std::mutex g_call_log_lock;

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

// Proxy tool invocation daily log helpers.
#include "monolith_proxy_tool_log.h"

static void parse_monolith_url(const std::string& url)
{
    g_monolith_url = url;

    // Parse the only supported transport schemes explicitly. Treating an
    // https URL as plain WinHTTP previously downgraded it to cleartext.
    std::string rest = url;
    if (rest.rfind("http://", 0) == 0)
    {
        g_monolith_secure = false;
        rest = rest.substr(7);
    }
    else if (rest.rfind("https://", 0) == 0)
    {
        g_monolith_secure = true;
        rest = rest.substr(8);
    }
    else
    {
        throw std::invalid_argument("MONOLITH_URL must use http:// or https://");
    }

    // Split host:port/path
    auto slash_pos = rest.find('/');
    std::string host_port = (slash_pos != std::string::npos) ? rest.substr(0, slash_pos) : rest;
    g_monolith_path_mcp = (slash_pos != std::string::npos) ? rest.substr(slash_pos) : "/mcp";
    if (host_port.empty() || host_port.find('@') != std::string::npos)
        throw std::invalid_argument("MONOLITH_URL must contain a host and cannot contain userinfo");

    std::string port_text;
    if (host_port.front() == '[')
    {
        const size_t close = host_port.find(']');
        if (close == std::string::npos)
            throw std::invalid_argument("MONOLITH_URL has an invalid IPv6 host");
        g_monolith_host = host_port.substr(1, close - 1);
        if (close + 1 < host_port.size())
        {
            if (host_port[close + 1] != ':')
                throw std::invalid_argument("MONOLITH_URL has invalid text after the IPv6 host");
            port_text = host_port.substr(close + 2);
        }
    }
    else
    {
        const size_t colon_pos = host_port.rfind(':');
        if (colon_pos != std::string::npos)
        {
            g_monolith_host = host_port.substr(0, colon_pos);
            port_text = host_port.substr(colon_pos + 1);
        }
        else
        {
            g_monolith_host = host_port;
        }
    }
    g_monolith_port = g_monolith_secure ? 443 : 80;
    if (!port_text.empty())
    {
        size_t consumed = 0;
        const long parsed = std::stol(port_text, &consumed);
        if (consumed != port_text.size() || parsed < 1 || parsed > 65535)
            throw std::invalid_argument("MONOLITH_URL port must be within 1..65535");
        g_monolith_port = static_cast<int>(parsed);
    }

    std::string lower_host = g_monolith_host;
    std::transform(lower_host.begin(), lower_host.end(), lower_host.begin(),
                   [](unsigned char c){ return static_cast<char>(std::tolower(c)); });
    g_monolith_loopback = lower_host == "localhost" || lower_host == "::1"
        || lower_host.rfind("127.", 0) == 0;
    if (!g_monolith_secure && !g_monolith_loopback)
        throw std::invalid_argument("Plain HTTP MONOLITH_URL is restricted to loopback hosts; use HTTPS for remote endpoints");

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
// JSONL call log (Phase 4 / survivor F)
//
// One line per upstream HTTP roundtrip:
//   {"ts":"2026-05-27T18:14:56Z","namespace":"editor","action":"get_build_errors",
//    "params_hash":"<40-char-sha1-hex>","duration_ms":42.5,"ok":true,
//    "error_code":null,"result_bytes":1834}
//
// Path: <project-root>/Saved/Logs/MonolithCalls.jsonl
// Opt-out: env var MONOLITH_CALL_LOG=0
// Append semantics on Win32: CreateFile w/ FILE_APPEND_DATA — OS guarantees
// atomic end-of-file positioning for writes < 4KB. Never seek before write.
// ============================================================================

static std::string sha1_hex(const std::string& data)
{
    BCRYPT_ALG_HANDLE hAlg = nullptr;
    if (BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_SHA1_ALGORITHM, nullptr, 0) != 0)
        return std::string();

    DWORD hashObjSize = 0;
    DWORD cb = 0;
    if (BCryptGetProperty(hAlg, BCRYPT_OBJECT_LENGTH,
            reinterpret_cast<PUCHAR>(&hashObjSize), sizeof(DWORD), &cb, 0) != 0)
    {
        BCryptCloseAlgorithmProvider(hAlg, 0);
        return std::string();
    }

    std::vector<UCHAR> hashObj(hashObjSize);
    BCRYPT_HASH_HANDLE hHash = nullptr;
    if (BCryptCreateHash(hAlg, &hHash, hashObj.data(), hashObjSize, nullptr, 0, 0) != 0)
    {
        BCryptCloseAlgorithmProvider(hAlg, 0);
        return std::string();
    }

    if (BCryptHashData(hHash,
            reinterpret_cast<PUCHAR>(const_cast<char*>(data.data())),
            static_cast<ULONG>(data.size()), 0) != 0)
    {
        BCryptDestroyHash(hHash);
        BCryptCloseAlgorithmProvider(hAlg, 0);
        return std::string();
    }

    UCHAR digest[20] = {0};
    if (BCryptFinishHash(hHash, digest, sizeof(digest), 0) != 0)
    {
        BCryptDestroyHash(hHash);
        BCryptCloseAlgorithmProvider(hAlg, 0);
        return std::string();
    }

    BCryptDestroyHash(hHash);
    BCryptCloseAlgorithmProvider(hAlg, 0);

    static const char* kHex = "0123456789abcdef";
    std::string out(40, '0');
    for (size_t i = 0; i < 20; ++i)
    {
        out[i * 2]     = kHex[(digest[i] >> 4) & 0x0F];
        out[i * 2 + 1] = kHex[digest[i]        & 0x0F];
    }
    return out;
}

static std::string iso8601_utc_now()
{
    // Second-precision ISO-8601 UTC, e.g. "2026-05-27T18:14:56Z".
    std::time_t t = std::time(nullptr);
    std::tm tm_buf;
    gmtime_s(&tm_buf, &t);
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02dZ",
        tm_buf.tm_year + 1900, tm_buf.tm_mon + 1, tm_buf.tm_mday,
        tm_buf.tm_hour, tm_buf.tm_min, tm_buf.tm_sec);
    return std::string(buf);
}

// Resolve <project-root>/Saved/Logs/MonolithCalls.jsonl
// Priority:
//   1. MONOLITH_PROJECT_ROOT env var (explicit override)
//   2. Current working directory (proxy CWD is the project root when launched
//      by Claude Code's MCP config)
static std::string resolve_call_log_path()
{
    std::string root = get_env("MONOLITH_PROJECT_ROOT");
    if (root.empty())
    {
        char cwd_buf[MAX_PATH];
        DWORD n = GetCurrentDirectoryA(MAX_PATH, cwd_buf);
        if (n > 0 && n < MAX_PATH)
            root = std::string(cwd_buf, n);
        else
            root = ".";
    }

    // Strip any trailing slash so we can append uniformly
    while (!root.empty() && (root.back() == '\\' || root.back() == '/'))
        root.pop_back();

    std::string saved   = root + "\\Saved";
    std::string logsdir = saved + "\\Logs";

    CreateDirectoryA(saved.c_str(), nullptr);    // OK if already exists
    CreateDirectoryA(logsdir.c_str(), nullptr);

    return logsdir + "\\MonolithCalls.jsonl";
}

static void init_call_log()
{
    // Default-enabled; only "0" disables. Read once at startup, cache the bool.
    g_call_log_enabled = get_env("MONOLITH_CALL_LOG", "1") != "0";
    if (!g_call_log_enabled)
    {
        log_msg("Call log disabled (MONOLITH_CALL_LOG=0)");
        return;
    }

    std::string path = resolve_call_log_path();
    g_call_log_handle = CreateFileA(
        path.c_str(),
        FILE_APPEND_DATA | FILE_WRITE_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);

    if (g_call_log_handle == INVALID_HANDLE_VALUE)
    {
        log_msg("Failed to open call log at " + path + " -- logging disabled");
        g_call_log_enabled = false;
        return;
    }

    log_msg("Call log: " + path);
}

// Canonicalised JSON over a params-style object: sorted keys, no whitespace.
// Matches Python json.dumps(sort_keys=True, separators=(",",":")).
static std::string canonical_json(const json& value)
{
    // nlohmann::json's underlying object is std::map (alphabetically sorted by
    // key); dump(-1) emits compact form (no spaces). For nested objects, the
    // library walks std::map-order recursively. This matches the sorted-keys
    // contract for hashing purposes.
    if (value.is_null())
        return "{}";
    return value.dump(-1);
}

// Extract (namespace, action) from a JSON-RPC message.
//   - tools/call w/ name matching "*_query": namespace = prefix, action = args.action
//   - tools/call w/ name matching "monolith_*": namespace = "monolith", action = suffix
//   - tools/call other: namespace = name, action = ""
//   - non-tools/call (initialize, ping, tools/list): namespace = method, action = ""
static void extract_namespace_action(const json& msg, std::string& ns, std::string& action)
{
    std::string method = msg.value("method", "");
    if (method != "tools/call")
    {
        ns = method;
        action = "";
        return;
    }

    auto params_it = msg.find("params");
    if (params_it == msg.end() || !params_it->is_object())
    {
        ns = "tools/call";
        action = "";
        return;
    }

    std::string name = params_it->value("name", "");
    const std::string query_suffix = "_query";
    if (name.size() > query_suffix.size() &&
        name.compare(name.size() - query_suffix.size(), query_suffix.size(), query_suffix) == 0)
    {
        ns = name.substr(0, name.size() - query_suffix.size());
        json args = params_it->value("arguments", json::object());
        if (args.is_object())
            action = args.value("action", "");
        else
            action = "";
        return;
    }

    const std::string monolith_prefix = "monolith_";
    if (name.rfind(monolith_prefix, 0) == 0)
    {
        ns = "monolith";
        action = name.substr(monolith_prefix.size());
        return;
    }

    ns = name;
    action = "";
}

// Extract params dict for hashing. For tools/call, that's params.arguments;
// for other methods, that's the whole params object.
static json extract_params_for_hash(const json& msg)
{
    std::string method = msg.value("method", "");
    auto params_it = msg.find("params");
    if (params_it == msg.end() || !params_it->is_object())
        return json::object();

    if (method == "tools/call")
    {
        json args = params_it->value("arguments", json::object());
        if (!args.is_object())
            return json::object();
        return args;
    }
    return *params_it;
}

// Inspect a forwarded HTTP response to extract (ok, error_code, result_bytes).
//   - ok = true iff response is valid JSON-RPC AND has no top-level "error"
//   - error_code = response.error.code if present, else null
//   - result_bytes = length of serialised result payload (or full response if no result)
static void inspect_response(const std::string& resp, bool& ok,
    std::optional<int>& error_code, size_t& result_bytes)
{
    ok = false;
    error_code.reset();
    result_bytes = 0;

    if (resp.empty())
        return;

    try
    {
        json parsed = json::parse(resp);
        auto err_it = parsed.find("error");
        if (err_it != parsed.end() && err_it->is_object())
        {
            ok = false;
            auto code_it = err_it->find("code");
            if (code_it != err_it->end() && code_it->is_number_integer())
                error_code = code_it->get<int>();
        }
        else
        {
            ok = true;
        }

        auto result_it = parsed.find("result");
        if (result_it != parsed.end())
            result_bytes = result_it->dump(-1).size();
        else
            result_bytes = resp.size();
    }
    catch (...)
    {
        // Unparseable -- treat as failure with no error_code; record full body size
        ok = false;
        result_bytes = resp.size();
    }
}

static void write_call_log_line(const json& msg, const std::string& resp, double duration_ms)
{
    if (!g_call_log_enabled || g_call_log_handle == INVALID_HANDLE_VALUE)
        return;

    try
    {
        std::string ns;
        std::string action;
        extract_namespace_action(msg, ns, action);

        json params_for_hash = extract_params_for_hash(msg);
        std::string canonical = canonical_json(params_for_hash);
        std::string params_hash = sha1_hex(canonical);

        bool ok = false;
        std::optional<int> error_code;
        size_t result_bytes = 0;
        inspect_response(resp, ok, error_code, result_bytes);

        json line;
        line["ts"]           = iso8601_utc_now();
        line["namespace"]    = ns;
        line["action"]       = action;
        line["params_hash"]  = params_hash;
        line["duration_ms"]  = duration_ms;
        line["ok"]           = ok;
        if (error_code.has_value())
            line["error_code"] = error_code.value();
        else
            line["error_code"] = nullptr;
        line["result_bytes"] = static_cast<int64_t>(result_bytes);

        std::string serialised = line.dump(-1);
        serialised.push_back('\n');

        // FILE_APPEND_DATA guarantees the kernel positions writes at EOF
        // atomically; single WriteFile call keeps the line indivisible up to
        // PIPE_BUF / 4KB. Mutex guards our handle from cross-thread races
        // (defence in depth -- only the dispatcher main thread writes today).
        std::lock_guard<std::mutex> lock(g_call_log_lock);
        DWORD written = 0;
        WriteFile(g_call_log_handle,
            serialised.data(),
            static_cast<DWORD>(serialised.size()),
            &written,
            nullptr);
    }
    catch (const std::exception& e)
    {
        log_msg(std::string("Call-log write failed: ") + e.what());
    }
    catch (...)
    {
        // never let logging crash the proxy
    }
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

static std::wstring winhttp_server_name()
{
    // WinHttpConnect requires brackets around a literal IPv6 server name even
    // though the URL parser stores the canonical address without brackets.
    if (g_monolith_host.find(':') != std::string::npos)
        return to_wide("[" + g_monolith_host + "]");
    return to_wide(g_monolith_host);
}

static bool set_http_deadline(
    HINTERNET request,
    const std::chrono::steady_clock::time_point& deadline)
{
    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
        deadline - std::chrono::steady_clock::now()).count();
    if (remaining <= 0) return false;
    const DWORD timeout_ms = static_cast<DWORD>(
        std::min<long long>(MAXDWORD, std::max<long long>(1, remaining)));
    return WinHttpSetTimeouts(
        request, timeout_ms, timeout_ms, timeout_ms, timeout_ms) == TRUE;
}

// POST JSON to Monolith. Returns response body or empty string on failure.
// Both body size and total wall-clock work are bounded; WinHTTP's native
// timeout is phase/receive based and is not a total deadline on its own.
static std::string post_monolith(const std::string& body, double timeout_sec = TIMEOUT)
{
    if (body.size() > MAX_LIVE_RESPONSE_BYTES || timeout_sec <= 0.0)
        return {};
    const auto deadline = std::chrono::steady_clock::now()
        + std::chrono::milliseconds(
            static_cast<long long>(timeout_sec * 1000.0));

    HINTERNET hSession = WinHttpOpen(
        L"MonolithProxy/1.0",
        WINHTTP_ACCESS_TYPE_NO_PROXY,
        WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS,
        0);
    if (!hSession) return {};

    std::wstring whost = winhttp_server_name();
    HINTERNET hConnect = WinHttpConnect(hSession, whost.c_str(), (INTERNET_PORT)g_monolith_port, 0);
    if (!hConnect) { WinHttpCloseHandle(hSession); return {}; }

    std::wstring wpath = to_wide(g_monolith_path_mcp);
    HINTERNET hRequest = WinHttpOpenRequest(
        hConnect, L"POST", wpath.c_str(),
        nullptr, WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES,
        g_monolith_secure ? WINHTTP_FLAG_SECURE : 0);
    if (!hRequest) { WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession); return {}; }

    if (!set_http_deadline(hRequest, deadline))
    {
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return {};
    }

    // Send
    const wchar_t* hdrs = L"Content-Type: application/json";
    BOOL ok = WinHttpSendRequest(
        hRequest, hdrs, (DWORD)-1,
        (LPVOID)body.c_str(), (DWORD)body.size(),
        (DWORD)body.size(), 0);

    if (!ok || !set_http_deadline(hRequest, deadline)
        || !WinHttpReceiveResponse(hRequest, nullptr))
    {
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return {};
    }

    DWORD status_code = 0;
    DWORD status_size = sizeof(status_code);
    if (!WinHttpQueryHeaders(
            hRequest,
            WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX,
            &status_code,
            &status_size,
            WINHTTP_NO_HEADER_INDEX)
        || status_code < 200 || status_code >= 300)
    {
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return {};
    }

    DWORD content_length = 0;
    DWORD content_length_size = sizeof(content_length);
    const bool has_content_length = WinHttpQueryHeaders(
            hRequest,
            WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX,
            &content_length,
            &content_length_size,
            WINHTTP_NO_HEADER_INDEX) == TRUE;
    if (has_content_length && content_length > MAX_LIVE_RESPONSE_BYTES)
    {
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return {};
    }

    // Read response
    std::string response;
    bool read_failed = false;
    while (true)
    {
        DWORD bytesAvailable = 0;
        if (!set_http_deadline(hRequest, deadline)
            || !WinHttpQueryDataAvailable(hRequest, &bytesAvailable))
        {
            read_failed = true;
            break;
        }
        if (bytesAvailable == 0) break;
        if (bytesAvailable > MAX_LIVE_RESPONSE_BYTES - response.size())
        {
            read_failed = true;
            break;
        }
        std::string chunk(bytesAvailable, '\0');
        DWORD bytesRead = 0;
        if (!set_http_deadline(hRequest, deadline)
            || !WinHttpReadData(hRequest, &chunk[0], bytesAvailable, &bytesRead)
            || bytesRead == 0)
        {
            read_failed = true;
            break;
        }
        response.append(chunk.data(), bytesRead);
    }

    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);

    if (read_failed
        || (has_content_length && response.size() != content_length))
    {
        return {};
    }
    return response;
}

static bool canonical_path_key(const fs::path& path, std::string& out)
{
    std::error_code error;
    fs::path canonical = fs::weakly_canonical(path, error);
    if (error || canonical.empty()) return false;
    out = canonical.lexically_normal().u8string();
    std::replace(out.begin(), out.end(), '\\', '/');
#ifdef _WIN32
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c){ return static_cast<char>(std::tolower(c)); });
#endif
    while (out.size() > 1 && out.back() == '/') out.pop_back();
    return true;
}

static std::string resolve_expected_project_root()
{
    const std::string configured = get_env("MONOLITH_EXPECTED_PROJECT_ROOT");
    fs::path candidate;
    if (!configured.empty())
    {
        candidate = fs::u8path(configured);
    }
    else
    {
        const fs::path plugin_root = executable_dir().parent_path();
        candidate = plugin_root.parent_path().parent_path();
    }
    std::string canonical;
    if (!canonical_path_key(candidate, canonical))
        throw std::runtime_error("Could not resolve expected Monolith host project root: " + candidate.u8string());
    return canonical;
}

static bool check_monolith_identity(int64_t health_pid, DWORD timeout_ms)
{
    json request = {
        {"jsonrpc", "2.0"},
        {"id", "proxy-project-identity"},
        {"method", "tools/call"},
        {"params", {
            {"name", "monolith_status"},
            {"arguments", json::object()},
        }},
    };
    const double timeout_seconds = std::max(0.25, timeout_ms / 1000.0);
    const std::string response_text = post_monolith(request.dump(), timeout_seconds);
    bool matches = false;
    try
    {
        const json response = json::parse(response_text);
        const json& result = response.at("result");
        if (!result.value("isError", true)
            && result.contains("structuredContent")
            && result["structuredContent"].is_object())
        {
            const json& status = result["structuredContent"];
            std::string reported_root;
            if (status.contains("recovery_plan")
                && status["recovery_plan"].is_object()
                && status["recovery_plan"].contains("editor_candidate_status")
                && status["recovery_plan"]["editor_candidate_status"].is_object())
            {
                const json& candidate = status["recovery_plan"]
                    ["editor_candidate_status"];
                reported_root = candidate.value("host_project_root", "");
                const auto pid_it = candidate.find("pid");
                const bool reported_pid_matches = pid_it != candidate.end()
                    && pid_it->is_number()
                    && std::isfinite(pid_it->get<double>())
                    && std::floor(pid_it->get<double>()) == pid_it->get<double>()
                    && pid_it->get<double>() == static_cast<double>(health_pid);
                const bool reports_editor_process =
                    candidate.value("status", "") == "current_editor_process";
                const bool reports_non_commandlet = candidate.contains("commandlet")
                    && candidate["commandlet"].is_boolean()
                    && !candidate["commandlet"].get<bool>();
                if (!reported_pid_matches || !reports_editor_process
                    || !reports_non_commandlet)
                {
                    reported_root.clear();
                }
            }
            std::string canonical_reported;
            matches = !reported_root.empty()
                && canonical_path_key(fs::u8path(reported_root), canonical_reported)
                && canonical_reported == g_expected_project_root;
        }
    }
    catch (...)
    {
        matches = false;
    }

    {
        std::lock_guard<std::mutex> lock(g_identity_state_lock);
        g_identity_pid = health_pid;
        g_identity_matches = matches;
    }
    if (!matches)
        log_msg("Rejected live endpoint: monolith_status editor/project identity did not match " + g_expected_project_root);
    return matches;
}

static unsigned short host_port_from_tcp_row(DWORD network_port)
{
    return _byteswap_ushort(static_cast<unsigned short>(network_port));
}

struct ListenerOwnerCheck
{
    bool saw_expected = false;
    bool saw_other = false;
};

static bool collect_tcp_listener_owners(
    ULONG address_family,
    DWORD expected_pid,
    ListenerOwnerCheck& check)
{
    ULONG bytes = 0;
    const DWORD sizing_result = GetExtendedTcpTable(
        nullptr,
        &bytes,
        FALSE,
        address_family,
        TCP_TABLE_OWNER_PID_LISTENER,
        0);
    if (sizing_result != ERROR_INSUFFICIENT_BUFFER || bytes == 0)
        return false;

    std::vector<unsigned char> storage(bytes);
    const DWORD table_result = GetExtendedTcpTable(
        storage.data(),
        &bytes,
        FALSE,
        address_family,
        TCP_TABLE_OWNER_PID_LISTENER,
        0);
    if (table_result != NO_ERROR)
        return false;

    if (address_family == AF_INET)
    {
        const auto* table = reinterpret_cast<const MIB_TCPTABLE_OWNER_PID*>(storage.data());
        for (DWORD index = 0; index < table->dwNumEntries; ++index)
        {
            const auto& row = table->table[index];
            if (host_port_from_tcp_row(row.dwLocalPort)
                != static_cast<unsigned short>(g_monolith_port))
                continue;
            if (row.dwOwningPid == expected_pid)
                check.saw_expected = true;
            else
                check.saw_other = true;
        }
        return true;
    }

    if (address_family == AF_INET6)
    {
        // Some supported Windows SDK target baselines omit the public IPv6
        // OWNER_PID typedef even though GetExtendedTcpTable supports the
        // documented layout. Keep the compatible layout local and bounded.
        struct Tcp6RowOwnerPidLayout
        {
            UCHAR local_addr[16];
            DWORD local_scope_id;
            DWORD local_port;
            UCHAR remote_addr[16];
            DWORD remote_scope_id;
            DWORD remote_port;
            DWORD state;
            DWORD owning_pid;
        };
        struct Tcp6TableOwnerPidLayout
        {
            DWORD entry_count;
            Tcp6RowOwnerPidLayout rows[1];
        };
        const auto* table = reinterpret_cast<const Tcp6TableOwnerPidLayout*>(storage.data());
        for (DWORD index = 0; index < table->entry_count; ++index)
        {
            const auto& row = table->rows[index];
            if (host_port_from_tcp_row(row.local_port)
                != static_cast<unsigned short>(g_monolith_port))
                continue;
            if (row.owning_pid == expected_pid)
                check.saw_expected = true;
            else
                check.saw_other = true;
        }
        return true;
    }
    return false;
}

static bool listener_owned_by_health_pid(int64_t health_pid)
{
    // Remote endpoints are permitted only over HTTPS. Their listener process
    // is not present in the local TCP owner table, so TLS plus the project
    // identity response is the applicable boundary there.
    if (!g_monolith_loopback)
        return true;
    if (health_pid <= 0
        || static_cast<uint64_t>(health_pid) > static_cast<uint64_t>(MAXDWORD))
        return false;
    const DWORD expected_pid = static_cast<DWORD>(health_pid);
    ListenerOwnerCheck check;
    const bool ipv4_ok = collect_tcp_listener_owners(AF_INET, expected_pid, check);
    const bool ipv6_ok = collect_tcp_listener_owners(AF_INET6, expected_pid, check);
    return ipv4_ok && ipv6_ok && check.saw_expected && !check.saw_other;
}

// GET health endpoint. A 200 alone is insufficient: validate the Monolith
// health schema, then bind the listener PID to this proxy's host project.
static bool check_monolith_up(DWORD timeout_ms = 3000)
{
    const auto deadline = std::chrono::steady_clock::now()
        + std::chrono::milliseconds(timeout_ms);
    HINTERNET hSession = WinHttpOpen(
        L"MonolithProxy/1.0",
        WINHTTP_ACCESS_TYPE_NO_PROXY,
        WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS,
        0);
    if (!hSession) return false;

    std::wstring whost = winhttp_server_name();
    HINTERNET hConnect = WinHttpConnect(hSession, whost.c_str(), (INTERNET_PORT)g_monolith_port, 0);
    if (!hConnect) { WinHttpCloseHandle(hSession); return false; }

    std::wstring wpath = to_wide(g_monolith_path_health);
    HINTERNET hRequest = WinHttpOpenRequest(
        hConnect, L"GET", wpath.c_str(),
        nullptr, WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES,
        g_monolith_secure ? WINHTTP_FLAG_SECURE : 0);
    if (!hRequest) { WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession); return false; }

    if (!set_http_deadline(hRequest, deadline))
    {
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return false;
    }

    BOOL ok = WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                  WINHTTP_NO_REQUEST_DATA, 0, 0, 0);
    if (!ok || !set_http_deadline(hRequest, deadline)
        || !WinHttpReceiveResponse(hRequest, nullptr))
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

    DWORD content_length = 0;
    DWORD content_length_size = sizeof(content_length);
    const bool has_content_length = WinHttpQueryHeaders(
            hRequest,
            WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX,
            &content_length,
            &content_length_size,
            WINHTTP_NO_HEADER_INDEX) == TRUE;
    if (has_content_length && content_length >= 64 * 1024)
    {
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return false;
    }

    std::string response;
    bool read_failed = false;
    while (true)
    {
        DWORD bytes_available = 0;
        if (!set_http_deadline(hRequest, deadline)
            || !WinHttpQueryDataAvailable(hRequest, &bytes_available))
        {
            read_failed = true;
            break;
        }
        if (bytes_available == 0) break;
        const size_t remaining = 64 * 1024 - response.size();
        if (remaining == 0 || bytes_available >= remaining)
        {
            read_failed = true;
            break;
        }
        std::string chunk(std::min<size_t>(bytes_available, remaining), '\0');
        DWORD bytes_read = 0;
        if (!set_http_deadline(hRequest, deadline)
            || !WinHttpReadData(
                hRequest, chunk.data(), static_cast<DWORD>(chunk.size()),
                &bytes_read))
        {
            read_failed = true;
            break;
        }
        if (bytes_read == 0)
        {
            read_failed = true;
            break;
        }
        response.append(chunk.data(), bytes_read);
    }

    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);

    if (read_failed || statusCode != 200 || response.empty()
        || (has_content_length && response.size() != content_length)
        || response.size() >= 64 * 1024)
        return false;
    try
    {
        const json health = json::parse(response);
        if (!health.is_object() || health.value("status", "") != "ok")
            return false;
        if (!health.contains("pid")
            || (!health["pid"].is_number_integer()
                && !health["pid"].is_number_unsigned()))
            return false;
        const int64_t pid = health["pid"].get<int64_t>();
        if (pid <= 0 || !listener_owned_by_health_pid(pid))
            return false;
        if (!health.contains("port")
            || (!health["port"].is_number_integer()
                && !health["port"].is_number_unsigned())
            || health["port"].get<int64_t>() != g_monolith_port)
            return false;
        if (!health.contains("version")
            || !health["version"].is_string()
            || health["version"].get<std::string>().empty())
            return false;
        if (!health.contains("uptime_seconds")
            || !health["uptime_seconds"].is_number()
            || health["uptime_seconds"].get<double>() < 0.0)
            return false;
        if (!health.contains("tools_registered")
            || (!health["tools_registered"].is_number_integer()
                && !health["tools_registered"].is_number_unsigned())
            || health["tools_registered"].get<int64_t>() <= 0)
            return false;
        if (!health.contains("mcp_transport")
            || !health["mcp_transport"].is_object()
            || health["mcp_transport"].value("primary_route", "") != "/mcp")
            return false;
        if (!check_monolith_identity(pid, timeout_ms))
            return false;
        // Bind both HTTP exchanges to the same listener owner. A process that
        // releases the port between health and status is not a valid endpoint.
        return listener_owned_by_health_pid(pid);
    }
    catch (...)
    {
        return false;
    }
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

static bool is_valid_jsonrpc_response(
    const std::string& response,
    const json& expected_id,
    std::string& error)
{
    if (response.empty())
    {
        error = "empty response";
        return false;
    }
    try
    {
        const json payload = json::parse(response);
        if (!payload.is_object())
        {
            error = "response is not a JSON object";
            return false;
        }
        auto version_it = payload.find("jsonrpc");
        if (version_it == payload.end() || !version_it->is_string()
            || version_it->get_ref<const std::string&>() != "2.0")
        {
            error = "response is missing jsonrpc=2.0";
            return false;
        }
        auto id_it = payload.find("id");
        if (id_it == payload.end() || *id_it != expected_id)
        {
            error = "response id does not match the request";
            return false;
        }
        const bool has_result = payload.contains("result");
        const bool has_error = payload.contains("error");
        if (has_result == has_error)
        {
            error = "response must contain exactly one of result or error";
            return false;
        }
        if (has_error && !payload["error"].is_object())
        {
            error = "response error member is not an object";
            return false;
        }
        return true;
    }
    catch (const std::exception& parse_error)
    {
        error = std::string("response is not valid JSON: ") + parse_error.what();
        return false;
    }
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

static json make_tool(const std::string& name, const std::string& description, const json& schema)
{
    json complete_schema = schema;
    if (!complete_schema.is_object())
        complete_schema = json::object();
    complete_schema["type"] = "object";
    json& properties = complete_schema["properties"];
    if (!properties.is_object())
        properties = json::object();
    properties["_fields"] = {
        {"type", "array"},
        {"items", {{"type", "string"}}},
        {"description", "Optional top-level whitelist — return only these top-level fields of the response. Mutually exclusive with _omit."},
    };
    properties["_omit"] = {
        {"type", "array"},
        {"items", {{"type", "string"}}},
        {"description", "Optional top-level blacklist — remove these top-level fields from the response. Mutually exclusive with _fields."},
    };
    properties["_compact_json"] = {
        {"type", "boolean"},
        {"description", "Optional — when true, drop top-level fields whose value is null, empty string, empty array, or empty object."},
    };
    return {
        {"name", name},
        {"description", description},
        {"inputSchema", std::move(complete_schema)}
    };
}

static json make_seed_tools()
{
    json tools = json::array();

    const std::string catalog_availability = g_offline_fallback_enabled
        ? " Uses the bundled offline catalog when the editor transport is unavailable."
        : " Requires the live editor transport because offline fallback is disabled.";

    tools.push_back(make_tool(
        "monolith_query",
        g_offline_fallback_enabled
            ? "Execute one namespaced Monolith action. Fixed read-only namespaces route to the bundled offline Query process when the editor transport is unavailable; all other actions require the live editor."
            : "Execute one namespaced Monolith action through the live editor transport.",
        {
            {"type", "object"},
            {"properties", {
                {"namespace", {
                    {"type", "string"},
                    {"description", "Target namespace, for example source or project."}
                }},
                {"action", {
                    {"type", "string"},
                    {"description", "Action name within the target namespace."}
                }},
                {"params", {
                    {"type", "object"},
                    {"description", "Parameters for the selected action."}
                }}
            }},
            {"required", json::array({"namespace", "action"})}
        }));

    tools.push_back(make_tool(
        "monolith_discover",
        "List available tool namespaces/actions or inspect one action contract."
            + catalog_availability,
        {
            {"type", "object"},
            {"properties", {
                {"namespace", {
                    {"type", "string"},
                    {"description", "Optional: filter to a specific namespace"}
                }},
                {"action", {
                    {"type", "string"},
                    {"description", "Optional: filter to one exact action within the namespace"}
                }},
                {"category", {
                    {"type", "string"},
                    {"description", "Optional: filter actions within the namespace by category"}
                }},
                {"mode", {
                    {"type", "string"},
                    {"enum", json::array({"summary", "actions", "schema"})}
                }},
                {"limit", {{"type", "integer"}, {"minimum", 0}, {"maximum", 1000}}},
                {"offset", {{"type", "integer"}, {"minimum", 0}}},
                {"filter", {{"type", "string"}}},
                {"detail", {{"type", "boolean"}}},
                {"verbose", {{"type", "boolean"}}},
                {"planning_detail", {
                    {"type", "string"},
                    {"enum", json::array({"compact", "full"})}
                }},
                {"schema_detail", {
                    {"type", "string"},
                    {"enum", json::array({"compact", "full"})}
                }},
                {"if_version", {{"type", "string"}}},
            }}
        }));

    tools.push_back(make_tool(
        "monolith_status",
        g_offline_fallback_enabled
            ? "Get live Monolith server health when the editor is available, or offline catalog snapshot status and recovery guidance when it is not."
            : "Get live Monolith server health. Offline fallback is disabled for this process.",
        {
            {"type", "object"},
            {"properties", {
                {"mcp_url", {{"type", "string"}}}
            }}
        }));

    tools.push_back(make_tool(
        "monolith_find",
        "Find candidate Monolith actions for a task." + catalog_availability,
        {
            {"type", "object"},
            {"properties", {
                {"query", {{"type", "string"}}},
                {"namespace", {{"type", "string"}}},
                {"limit", {{"type", "integer"}, {"minimum", 1}, {"maximum", 50}}},
                {"include_schema", {{"type", "boolean"}}},
                {"planning_detail", {
                    {"type", "string"},
                    {"enum", json::array({"compact", "full"})}
                }},
                {"offset", {{"type", "integer"}, {"minimum", 0}}},
                {"cursor", {{"type", "string"}}},
                {"fields", {
                    {"oneOf", json::array({
                        json{{"type", "string"}},
                        json{{"type", "array"}, {"items", {{"type", "string"}}}}
                    })}
                }}
            }},
            {"required", json::array({"query"})}
        }));

    return tools;
}

static bool is_valid_tools_list_response(
    const std::string& response,
    std::string& error)
{
    try
    {
        const json payload = json::parse(response);
        if (!payload.contains("result") || !payload["result"].is_object()
            || !payload["result"].contains("tools")
            || !payload["result"]["tools"].is_array())
        {
            error = "tools/list result must contain a tools array";
            return false;
        }
        std::set<std::string> names;
        for (const json& tool : payload["result"]["tools"])
        {
            if (!tool.is_object() || !tool.contains("name")
                || !tool["name"].is_string()
                || tool["name"].get_ref<const std::string&>().empty()
                || !tool.contains("inputSchema")
                || !tool["inputSchema"].is_object())
            {
                error = "tools/list contains an invalid tool descriptor";
                return false;
            }
            if (!names.insert(tool["name"].get<std::string>()).second)
            {
                error = "tools/list contains duplicate tool names";
                return false;
            }
        }
        return true;
    }
    catch (const std::exception& parse_error)
    {
        error = std::string("tools/list response is not valid JSON: ")
            + parse_error.what();
        return false;
    }
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
    // Do not advertise a stale live/profile cache while the editor is down.
    // Older caches exposed dozens of tools that could not execute and expanded
    // cold-start prompt metadata by roughly an order of magnitude. The stable
    // control plane is deliberately four tools; live tools return after a
    // validated endpoint transition and tools/list_changed notification.
    log_msg("Monolith down during tools/list -- returning four stable control-plane tools");
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

static void prune_recent_tool_calls(double now)
{
    for (auto it = g_recent_tool_calls.begin(); it != g_recent_tool_calls.end();)
    {
        const RecentToolCall& call = it->second;
        if (!call.in_flight
            && now - call.completed_at >= call.repeat_window_seconds)
        {
            it = g_recent_tool_calls.erase(it);
        }
        else
        {
            ++it;
        }
    }

    while (g_recent_tool_calls.size() >= MAX_RECENT_TOOL_CALLS)
    {
        auto oldest = g_recent_tool_calls.end();
        for (auto it = g_recent_tool_calls.begin(); it != g_recent_tool_calls.end(); ++it)
        {
            if (it->second.in_flight) continue;
            if (oldest == g_recent_tool_calls.end()
                || it->second.completed_at < oldest->second.completed_at)
            {
                oldest = it;
            }
        }
        if (oldest == g_recent_tool_calls.end()) break;
        g_recent_tool_calls.erase(oldest);
    }
}

static bool begin_tool_call(
    const json& msg,
    double repeat_window_seconds,
    std::string& signature)
{
    signature = tool_signature(msg);
    if (signature.empty()) return true;

    const double now = now_seconds();
    std::lock_guard<std::mutex> lock(g_recent_tool_calls_lock);
    prune_recent_tool_calls(now);
    auto existing = g_recent_tool_calls.find(signature);
    if (existing != g_recent_tool_calls.end())
    {
        const RecentToolCall& call = existing->second;
        if (call.in_flight
            || now - call.completed_at < call.repeat_window_seconds)
        {
            return false;
        }
        g_recent_tool_calls.erase(existing);
    }

    if (g_recent_tool_calls.size() >= MAX_RECENT_TOOL_CALLS)
        return false;
    g_recent_tool_calls.emplace(signature, RecentToolCall{
        true, 0.0, repeat_window_seconds});
    return true;
}

static void complete_tool_call(const std::string& signature)
{
    if (signature.empty()) return;
    std::lock_guard<std::mutex> lock(g_recent_tool_calls_lock);
    auto existing = g_recent_tool_calls.find(signature);
    if (existing == g_recent_tool_calls.end()) return;
    existing->second.in_flight = false;
    existing->second.completed_at = now_seconds();
}

struct ToolCallCompletionGuard
{
    bool& active;
    const std::string& signature;

    ~ToolCallCompletionGuard()
    {
        if (active)
        {
            complete_tool_call(signature);
            active = false;
        }
    }
};

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

static bool transport_circuit_open();

static void check_monolith_state_change()
{
    // A valid health/status pair is not proof that a previously wedged action
    // route is ready. Honor the transport-failure cooldown; only an actual
    // successful MCP request resets its exponential backoff.
    if (transport_circuit_open())
        return;
    const bool is_up = check_monolith_up();
    bool should_notify = false;
    {
        std::lock_guard<std::mutex> lock(g_monolith_state_lock);
        const bool had_state = g_monolith_was_up.has_value();
        should_notify = (had_state && is_up != g_monolith_was_up.value())
            || (!had_state && is_up);
        g_monolith_was_up = is_up;
    }

    // Notification/logging may acquire independent locks and perform I/O; keep
    // it outside the state mutex so the poller cannot block request dispatch.
    if (should_notify)
    {
        const char* direction = is_up ? "online" : "offline";
        log_msg(std::string("Monolith went ") + direction + " -- sending tools/list_changed");
        send_list_changed();
    }
}

static bool monolith_known_up()
{
    std::lock_guard<std::mutex> lock(g_monolith_state_lock);
    return g_monolith_was_up.value_or(false);
}

static bool transport_circuit_open()
{
    std::lock_guard<std::mutex> lock(g_monolith_state_lock);
    return g_transport_retry_after != std::chrono::steady_clock::time_point{}
        && std::chrono::steady_clock::now() < g_transport_retry_after;
}

static void mark_monolith_down_after_transport_failure()
{
    bool should_notify = false;
    {
        std::lock_guard<std::mutex> lock(g_monolith_state_lock);
        should_notify = g_monolith_was_up.value_or(false);
        g_monolith_was_up = false;
        ++g_transport_failure_count;
        const unsigned exponent = std::min<unsigned>(g_transport_failure_count - 1, 3);
        const unsigned cooldown_seconds = std::min<unsigned>(60, 10u << exponent);
        g_transport_retry_after = std::chrono::steady_clock::now()
            + std::chrono::seconds(cooldown_seconds);
    }
    {
        std::lock_guard<std::mutex> lock(g_identity_state_lock);
        g_identity_pid = -1;
        g_identity_matches = false;
    }
    if (should_notify)
    {
        log_msg("Live transport failed after a known-up probe; opening the offline circuit immediately");
        send_list_changed();
    }
}

static void mark_monolith_transport_success()
{
    bool should_notify = false;
    {
        std::lock_guard<std::mutex> lock(g_monolith_state_lock);
        should_notify = !g_monolith_was_up.value_or(false);
        g_monolith_was_up = true;
        g_transport_failure_count = 0;
        g_transport_retry_after = {};
    }
    if (should_notify)
    {
        log_msg("Validated live MCP request succeeded; closing the offline circuit");
        send_list_changed();
    }
}

static bool sleep_until_poll_or_stop(double seconds)
{
    const auto deadline = std::chrono::steady_clock::now()
        + std::chrono::milliseconds(static_cast<int>(seconds * 1000));
    while (!g_stop_health_poll.load() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    return !g_stop_health_poll.load();
}

static void health_poll_thread()
{
    // Initial delay
    if (!sleep_until_poll_or_stop(POLL_START_DELAY))
        return;
    log_msg("Health poll started (interval=" + std::to_string((int)POLL_INTERVAL) + "s)");

    while (!g_stop_health_poll.load())
    {
        try
        {
            check_monolith_state_change();
        }
        catch (...)
        {
            log_msg("Health poll error");
        }

        if (!sleep_until_poll_or_stop(POLL_INTERVAL))
            break;
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
        "Monolith MCP proxy for Unreal Engine. Tools forward to the Unreal Editor.\n"
        "\n"
        "ROUTING:\n"
        "  monolith_find(query)                            — find the right action\n"
        "  monolith_discover()                             — list all namespaces\n"
        "  monolith_discover(namespace)                    — list actions in a namespace\n"
        "  monolith_discover(namespace, action, 'schema')  — fetch exact param schema\n"
        "  monolith_query({namespace, action, params})     — execute any action\n"
        "\n"
        "SKILL LOADING: domain skills live in Skills/<namespace>/SKILL.md and document\n"
        "available actions and params for that namespace.\n"
        "\n"
        + std::string(g_offline_fallback_enabled
            ? "EDITOR OFFLINE: when editor transport is down, the advertised surface contracts to monolith_query, monolith_discover, monolith_status, and monolith_find.\n"
              "Fixed read-only query actions run through the co-located offline monolith_query binary.\n"
            : "EDITOR OFFLINE: offline fallback is disabled for this proxy process, so editor transport failures remain unavailable errors.\n")
        +
        "Before calling a domain action, check its schema instead of guessing. "
        "monolith_discover() lists namespaces and monolith_discover(namespace='<namespace>', "
        "mode='actions') lists actions; monolith_discover(namespace='<namespace>', action='<action>', mode='schema') fetches the exact live schema. "
        "Offline schema mode returns explicitly degraded catalog guidance rather than fabricating a live JSON schema. "
        "If an editor-only tool returns a transport-unavailable error, run Scripts/recover_mcp.ps1, wait for the configured endpoint, and retry.";

    return make_result(msg.value("id", json()), result);
}

static std::string handle_ping(const json& msg)
{
    return make_result(msg.value("id", json()), json::object());
}

static std::string handle_tools_list(const json& msg)
{
    double t0 = now_seconds();
    // Cold client startup must never wait on a half-started editor. The first
    // list is cache+seed; a successful background/initialized health probe
    // announces list_changed, after which a tightly bounded live refresh is
    // allowed.
    std::string resp;
    bool live_attempted = false;
    if (monolith_known_up() && !transport_circuit_open()
        && check_monolith_up(REQUEST_HEALTH_TIMEOUT_MS))
    {
        live_attempted = true;
        resp = post_monolith(msg.dump(), TOOLS_LIST_TIMEOUT);
    }
    double duration_ms = (now_seconds() - t0) * 1000.0;
    write_call_log_line(msg, resp, duration_ms);

    std::string response_validation_error;
    if (!resp.empty()
        && !is_valid_jsonrpc_response(
            resp, msg.value("id", json()), response_validation_error))
    {
        log_msg("Discarding invalid live tools/list response: "
            + response_validation_error);
        resp.clear();
    }
    if (!resp.empty()
        && !is_valid_tools_list_response(resp, response_validation_error))
    {
        log_msg("Discarding live tools/list response with an invalid method contract: "
            + response_validation_error);
        resp.clear();
    }
    if (live_attempted && resp.empty())
        log_msg("Live tools/list missed its bounded metadata budget; using the stable cold surface without opening the action circuit");

    if (!resp.empty())
    {
        mark_monolith_transport_success();
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
    if (!params.is_object())
        return make_jsonrpc_error(id, -32602,
            "tools/call params must be an object.");
    auto name_it = params.find("name");
    if (name_it == params.end() || !name_it->is_string()
        || name_it->get_ref<const std::string&>().empty())
        return make_jsonrpc_error(id, -32602,
            "tools/call params.name must be a non-empty string.");
    std::string tool_name = name_it->get<std::string>();
    std::string forwarded_name = tool_name;
    json args = params.value("arguments", json::object());
    if (args.is_null()) args = json::object();
    if (!args.is_object())
        return make_jsonrpc_error(id, -32602,
            "tools/call params.arguments must be an object or null.");
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
    bool dedup_started = false;
    std::string dedup_signature;
    ToolCallCompletionGuard completion_guard{dedup_started, dedup_signature};
    auto finish = [&](const std::string& response) -> std::string
    {
        if (dedup_started)
        {
            complete_tool_call(dedup_signature);
            dedup_started = false;
        }
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

    const double repeat_window =
        MonolithInvocationRequiresStrongRepeatProtection(
            forwarded_name, args.dump(-1))
        ? RISKY_REPEAT_TOOL_CALL_WINDOW
        : REPEAT_TOOL_CALL_WINDOW;
    if (!begin_tool_call(forwarded_msg, repeat_window, dedup_signature))
    {
        repeated_for_log = true;
        return finish(make_tool_error(id,
            "Tool '" + tool_name + "' with the same arguments was just called. "
            "Reuse the previous result and answer the user instead of repeating the same call."));
    }
    dedup_started = true;

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

    const auto http_start_clock = std::chrono::steady_clock::now();
    // A process may still own port/editor state while its MCP endpoint is
    // booting or wedged. Cold/offline-capable reads do not wait for an unknown
    // backend; after health is known-up they get a short live attempt before
    // the fixed read-only fallback. Live-only tools retain the normal timeout.
    std::string resp;
    const bool offline_fast_path = g_offline_fallback_enabled
        && MonolithInvocationCanUseOfflineFastPath(tool_name, args.dump(-1));
    bool live_attempted = false;
    if (!transport_circuit_open() && offline_fast_path)
    {
        if (monolith_known_up()
            && check_monolith_up(REQUEST_HEALTH_TIMEOUT_MS))
        {
            live_attempted = true;
            resp = post_monolith(
                forwarded_msg.dump(), READ_FALLBACK_LIVE_TIMEOUT);
        }
    }
    else if (!transport_circuit_open()
        && check_monolith_up(REQUEST_HEALTH_TIMEOUT_MS))
    {
        live_attempted = true;
        resp = post_monolith(forwarded_msg.dump());
    }
    phase_timing["http_roundtrip_ms"] = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - http_start_clock).count();
    std::string response_validation_error;
    if (!resp.empty()
        && !is_valid_jsonrpc_response(resp, id, response_validation_error))
    {
        log_msg("Discarding invalid live tools/call response and considering "
            "the read-only fallback: " + response_validation_error);
        resp.clear();
    }
    if (live_attempted && resp.empty())
        mark_monolith_down_after_transport_failure();
    if (!resp.empty())
    {
        mark_monolith_transport_success();
        return finish(resp);
    }

    const auto fallback_start_clock = std::chrono::steady_clock::now();
    MonolithOfflineFallbackResult offline = TryMonolithOfflineFallback(
        g_offline_fallback_enabled,
        tool_name,
        args.dump(-1),
        id.dump(-1),
        trace_id,
        span_id);
    if (offline.handled)
    {
        phase_timing["offline_fallback_ms"] = offline.duration_ms;
        phase_timing["fallback_ms"] =
            std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - fallback_start_clock).count();
        log_msg("Editor backend unavailable; routed '" + tool_name
            + "' to " + offline.backend + " " + offline.query_namespace
            + "." + offline.action);
        return finish(offline.response);
    }

    std::string unavailable = make_tool_error(id,
        "Live Monolith editor transport is unavailable; the editor may be stopped, restarting, or endpoint-unhealthy. "
        "Tool '" + tool_name + "' cannot execute on the current route. Restore the configured MCP endpoint and retry.");
    phase_timing["fallback_ms"] = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - fallback_start_clock).count();
    return finish(unavailable);
}

// ============================================================================
// Main loop
// ============================================================================



// Proxy CLI help/version helpers.
#include "monolith_proxy_help.h"

int main(int argc, char* argv[])
{
    for (int i = 1; i < argc; ++i)
    {
        std::string arg = argv[i];
        if (is_proxy_help_arg(arg))
        {
            print_proxy_help();
            return 0;
        }
        if (is_proxy_version_arg(arg))
        {
            print_proxy_version();
            return 0;
        }
    }

    // Binary-safe stdin/stdout on Windows
    _setmode(_fileno(stdin),  _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);

    // Parse configuration from environment
    std::string url = get_env("MONOLITH_URL", "http://localhost:9316/mcp");
    try
    {
        parse_monolith_url(url);
        g_expected_project_root = resolve_expected_project_root();
    }
    catch (const std::exception& config_error)
    {
        log_msg(std::string("Invalid startup configuration: ") + config_error.what());
        return 2;
    }

    g_split_editor_query   = get_env("MONOLITH_SPLIT_EDITOR_QUERY", "0") == "1";
    g_offline_fallback_enabled = get_env("MONOLITH_OFFLINE_FALLBACK", "1") != "0";
    g_editor_action_allowlist = parse_csv_env("MONOLITH_EDITOR_ACTION_ALLOWLIST");
    g_editor_action_denylist  = parse_csv_env("MONOLITH_EDITOR_ACTION_DENYLIST");

    if (g_offline_fallback_enabled)
    {
        std::string bundle_error;
        if (!InitializeMonolithOfflineBundle(bundle_error))
        {
            // Treat bundle validity as part of effective offline readiness.
            // Keeping the request flag true here would let the offline fast
            // path intercept a healthy live endpoint with an unusable bundle.
            g_offline_fallback_enabled = false;
            log_msg("Offline Query/catalog bundle is unavailable; live routing remains enabled: "
                + bundle_error);
        }
    }

    log_msg(std::string("Started. Forwarding to ") + g_monolith_url);
    log_msg(std::string("Offline fallback ")
        + (g_offline_fallback_enabled ? "enabled" : "disabled"));

    init_call_log();

    // Keep ownership of the poller so it cannot race process teardown after
    // stdin closes.
    std::thread poller(health_poll_thread);

    // Main stdin read loop
    std::string line;
    while (std::getline(std::cin, line))
    {
        // Trim
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n'))
            line.pop_back();
        if (line.empty())
            continue;

        // Parse JSON. Malformed input is a request-level error, not a reason to
        // terminate the long-lived control plane or silently drop the caller.
        json msg;
        try
        {
            msg = json::parse(line);
        }
        catch (const json::parse_error& e)
        {
            log_msg(std::string("Bad JSON: ") + e.what());
            write_stdout(make_jsonrpc_error(json(), -32700, "Parse error."));
            continue;
        }

        if (!msg.is_object())
        {
            write_stdout(make_jsonrpc_error(json(), -32600,
                "Invalid Request: JSON-RPC message must be an object."));
            continue;
        }

        bool has_id = msg.contains("id");
        json request_id = has_id ? msg["id"] : json();
        auto method_it = msg.find("method");
        if (method_it == msg.end() || !method_it->is_string()
            || method_it->get_ref<const std::string&>().empty())
        {
            write_stdout(make_jsonrpc_error(request_id, -32600,
                "Invalid Request: method must be a non-empty string."));
            continue;
        }
        std::string method = method_it->get<std::string>();
        std::string response;

        try
        {
            if (method == "initialize")
            {
                response = handle_initialize(msg);
                log_msg("Initialized");
            }
            else if (method == "notifications/initialized" || method == "initialized")
            {
                // Notification -- no response. The background poll owns live
                // state transitions so client initialization never blocks on
                // a half-started editor endpoint.
            }
            else if (method == "ping")
            {
                response = handle_ping(msg);
            }
            else if (method == "tools/list")
            {
                response = handle_tools_list(msg);
            }
            else if (method == "tools/call")
            {
                response = handle_tools_call(msg);
            }
            else
            {
                // The proxy advertises only tools. Answer the standard empty
                // optional surfaces locally and reject everything else without
                // a network roundtrip. Forwarding unknown methods caused 4-30s
                // shutdown/startup stalls against half-started editors and
                // violated the capabilities returned by initialize.
                if (!has_id)
                    continue;
                if (method == "resources/list")
                    response = make_result(request_id, {{"resources", json::array()}});
                else if (method == "resources/templates/list")
                    response = make_result(request_id, {{"resourceTemplates", json::array()}});
                else if (method == "prompts/list")
                    response = make_result(request_id, {{"prompts", json::array()}});
                else
                    response = make_jsonrpc_error(request_id, -32601,
                        "Method not found: " + method);
            }
        }
        catch (const std::exception& error)
        {
            log_msg("Request handler exception for '" + method + "': " + error.what());
            if (has_id)
                response = make_jsonrpc_error(request_id, -32603,
                    "Internal error while handling request; the proxy remains available.");
        }

        if (!response.empty())
            write_stdout(response);
    }

    // EOF on stdin -- clean exit
    g_stop_health_poll.store(true);
    if (poller.joinable())
        poller.join();
    log_msg("stdin closed, exiting");
    return 0;
}
