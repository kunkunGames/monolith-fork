#pragma once

// Proxy tool invocation daily log helpers.

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
