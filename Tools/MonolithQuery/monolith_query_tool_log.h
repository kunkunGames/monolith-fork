#pragma once

// Query tool invocation daily log helpers.

static bool tool_log_enabled() {
    return getenv_str("MONOLITH_TOOL_LOG_ENABLED", "1") != "0";
}

static std::string iso_local_now() {
    auto now = std::chrono::system_clock::now();
    auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()) % 1000;
    std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm local_tm{};
#ifdef _WIN32
    localtime_s(&local_tm, &t);
#else
    localtime_r(&t, &local_tm);
#endif
    std::ostringstream out;
    out << std::put_time(&local_tm, "%Y-%m-%dT%H:%M:%S")
        << "." << std::setw(3) << std::setfill('0') << millis.count();
#ifdef _WIN32
    long bias_minutes = 0;
    TIME_ZONE_INFORMATION tzi;
    DWORD tz_result = GetTimeZoneInformation(&tzi);
    bias_minutes = tzi.Bias;
    if (tz_result == TIME_ZONE_ID_DAYLIGHT) bias_minutes += tzi.DaylightBias;
    else if (tz_result == TIME_ZONE_ID_STANDARD) bias_minutes += tzi.StandardBias;
    long offset = -bias_minutes;
    char sign = offset >= 0 ? '+' : '-';
    offset = std::labs(offset);
    out << sign << std::setw(2) << std::setfill('0') << (offset / 60)
        << ":" << std::setw(2) << std::setfill('0') << (offset % 60);
#endif
    return out.str();
}

static std::string yyyymmdd_local() {
    std::time_t t = std::time(nullptr);
    std::tm local_tm{};
#ifdef _WIN32
    localtime_s(&local_tm, &t);
#else
    localtime_r(&t, &local_tm);
#endif
    std::ostringstream out;
    out << std::put_time(&local_tm, "%Y%m%d");
    return out.str();
}

static std::string hash_text(const std::string& text) {
#ifdef _WIN32
    auto to_hex = [](const std::vector<unsigned char>& bytes) {
        std::ostringstream out;
        out << std::hex << std::setfill('0');
        for (unsigned char byte : bytes) {
            out << std::setw(2) << static_cast<int>(byte);
        }
        return out.str();
    };

    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    DWORD bytes_written = 0;
    DWORD hash_len = 0;
    std::vector<unsigned char> digest;

    NTSTATUS status = BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0);
    if (status >= 0) {
        status = BCryptGetProperty(alg, BCRYPT_HASH_LENGTH,
            reinterpret_cast<PUCHAR>(&hash_len), sizeof(hash_len), &bytes_written, 0);
    }
    if (status >= 0) {
        digest.resize(hash_len);
        status = BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0);
    }
    if (status >= 0 && !text.empty()) {
        status = BCryptHashData(hash,
            reinterpret_cast<PUCHAR>(const_cast<char*>(text.data())),
            static_cast<ULONG>(text.size()), 0);
    }
    if (status >= 0) {
        status = BCryptFinishHash(hash, digest.data(), hash_len, 0);
    }
    if (hash) BCryptDestroyHash(hash);
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    if (status >= 0) {
        return "sha256:" + to_hex(digest);
    }
#endif

    // Non-Windows fallback keeps grouping deterministic if BCrypt is unavailable.
    std::hash<std::string> hasher;
    uint64_t a = hasher(text);
    uint64_t b = hasher("monolith:" + text);
    std::ostringstream out;
    out << "hash:" << std::hex << std::setw(16) << std::setfill('0') << a
        << std::setw(16) << std::setfill('0') << b;
    return out.str();
}

static std::string log_id(const std::string& prefix, const std::string& seed) {
    std::string digest = hash_text(seed);
    size_t pos = digest.find(':');
    if (pos != std::string::npos) digest = digest.substr(pos + 1);
    if (digest.size() > 32) digest.resize(32);
    return prefix + "-" + digest;
}

static void prune_empty_json(json& value);

static std::string process_instance_id() {
    static const std::string id = log_id("proc", "query:" + std::to_string(
#ifdef _WIN32
        static_cast<int>(GetCurrentProcessId())
#else
        0
#endif
    ) + ":" + iso_local_now());
    return id;
}

static int64_t current_process_id() {
#ifdef _WIN32
    return static_cast<int64_t>(GetCurrentProcessId());
#else
    return static_cast<int64_t>(getpid());
#endif
}

static bool process_is_running(int64_t pid) {
    if (pid <= 0) return false;
#ifdef _WIN32
    HANDLE handle = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(pid));
    if (!handle) {
        DWORD error = GetLastError();
        return error == ERROR_ACCESS_DENIED;
    }
    DWORD wait_result = WaitForSingleObject(handle, 0);
    CloseHandle(handle);
    return wait_result == WAIT_TIMEOUT;
#else
    if (kill(static_cast<pid_t>(pid), 0) == 0) return true;
    return errno == EPERM;
#endif
}

static std::string infer_intent(const std::string& ns, const std::string& action, int exit_code) {
    std::string act = action;
    std::transform(act.begin(), act.end(), act.begin(),
                   [](unsigned char c){ return static_cast<char>(std::tolower(c)); });
    if (act.find("health") != std::string::npos || act.find("status") != std::string::npos
        || act.find("check") != std::string::npos || act.find("validate") != std::string::npos) {
        return "verification";
    }
    if (ns == "source" || act.find("source") != std::string::npos || act.find("symbol") != std::string::npos
        || act.find("reference") != std::string::npos || act.find("caller") != std::string::npos || act.find("callee") != std::string::npos) {
        return "source_lookup";
    }
    if (ns == "project" || act.find("asset") != std::string::npos || act.find("reference") != std::string::npos) {
        return "asset_search";
    }
    if (act.find("repair") != std::string::npos || act.find("rebuild") != std::string::npos
        || act.find("build") != std::string::npos || act.find("snapshot") != std::string::npos) {
        return "maintenance";
    }
    if (exit_code != 0) {
        return "error_recovery";
    }
    return "unknown";
}

static std::string workflow_step_for_intent(const std::string& intent, int exit_code) {
    if (exit_code != 0) return "recover";
    if (intent == "verification") return "verify";
    if (intent == "maintenance") return "maintenance";
    if (intent == "source_lookup" || intent == "asset_search") return "inspect";
    return "unknown";
}

static std::string result_shape_for_query(const json& stdout_value, const json& stderr_value, int exit_code) {
    if (exit_code != 0) return "error";
    if (stdout_value.is_null() && stderr_value.is_null()) return "empty";
    if (stdout_value.is_object()) return "object";
    if (stdout_value.is_array()) return "list";
    if (stdout_value.is_string()) return stdout_value.get<std::string>().empty() ? "empty" : "text";
    return "mixed";
}

static json make_query_environment(const std::string& ns, const json& db_paths) {
    json env = json::object();
    std::string version = getenv_str("MONOLITH_VERSION");
    if (!version.empty()) env["plugin_version"] = version;
    if (!db_paths.empty()) env["project_name_hash"] = hash_text(db_paths.dump());
    std::string index_health = "unknown";
    if ((ns == "source" || ns == "project" || ns == "bridge") && db_paths.is_array()) {
        bool all_exist = !db_paths.empty();
        for (const auto& path_value : db_paths) {
            if (!path_value.is_string() || !fs::exists(path_value.get<std::string>())) {
                all_exist = false;
                break;
            }
        }
        index_health = all_exist ? "ok" : "missing";
    }
    env["index_health"] = index_health;
    prune_empty_json(env);
    return env;
}

static bool is_sensitive_key(const std::string& key) {
    std::string lower = key;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c){ return static_cast<char>(std::tolower(c)); });
    static const std::vector<std::string> fragments = {
        "authorization", "bearer", "token", "api_key", "apikey",
        "password", "passwd", "secret", "cookie", "private_key", "session_id"
    };
    for (const auto& fragment : fragments) {
        if (lower.find(fragment) != std::string::npos) return true;
    }
    return false;
}

static json redact_json(const json& value) {
    if (value.is_object()) {
        json out = json::object();
        for (auto it = value.begin(); it != value.end(); ++it) {
            out[it.key()] = is_sensitive_key(it.key()) ? json("[REDACTED]") : redact_json(it.value());
        }
        return out;
    }
    if (value.is_array()) {
        json out = json::array();
        for (const auto& item : value) out.push_back(redact_json(item));
        return out;
    }
    if (value.is_string()) {
        const std::string text = value.get<std::string>();
        if (!text.empty() && (text.front() == '{' || text.front() == '[')) {
            try {
                return redact_json(json::parse(text));
            } catch (...) {
            }
        }
    }
    return value;
}

static size_t max_log_field_bytes() {
    const std::string raw = getenv_str("MONOLITH_TOOL_LOG_MAX_FIELD_BYTES");
    if (raw.empty()) return 256 * 1024;
    try {
        const size_t parsed = static_cast<size_t>(std::stoull(raw));
        return std::max<size_t>(1024, std::min<size_t>(parsed, 16 * 1024 * 1024));
    } catch (...) {
        return 256 * 1024;
    }
}

static json bounded_json(const json& value, bool& truncated, size_t& original_bytes, std::string& hash) {
    const size_t MaxBytes = max_log_field_bytes();
    const std::string text = value.dump();
    original_bytes = text.size();
    if (text.size() <= MaxBytes) {
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

static bool is_empty_log_value(const json& value) {
    return value.is_null()
        || (value.is_string() && value.get<std::string>().empty())
        || (value.is_array() && value.empty())
        || (value.is_object() && value.empty());
}

static void prune_empty_json(json& value) {
    if (value.is_object()) {
        for (auto it = value.begin(); it != value.end();) {
            prune_empty_json(it.value());
            if (is_empty_log_value(it.value())) it = value.erase(it);
            else ++it;
        }
    } else if (value.is_array()) {
        for (auto& item : value) prune_empty_json(item);
        value.erase(std::remove_if(value.begin(), value.end(), is_empty_log_value), value.end());
    }
}

static json summarize_query_return(
    const json& stdout_value,
    const json& stderr_value,
    int exit_code,
    size_t stdout_bytes,
    size_t stderr_bytes,
    bool truncated,
    const std::string& fatal_error) {
    json summary = {
        {"exit_code", exit_code},
        {"result_shape", result_shape_for_query(stdout_value, stderr_value, exit_code)},
        {"stdout_bytes", stdout_bytes},
        {"stderr_bytes", stderr_bytes},
        {"result_bytes", stdout_bytes + stderr_bytes},
        {"truncated", truncated}
    };
    if (!fatal_error.empty()) summary["fatal_error"] = fatal_error.substr(0, 240);
    if (stdout_value.is_object()) {
        json top_keys = json::array();
        for (auto it = stdout_value.begin(); it != stdout_value.end(); ++it) top_keys.push_back(it.key());
        summary["stdout_top_keys"] = top_keys;
        if (stdout_value.contains("status")) summary["stdout_status"] = stdout_value["status"];
        if (stdout_value.contains("summary")) summary["summary"] = stdout_value["summary"];
        if (stdout_value.contains("results") && stdout_value["results"].is_array()) summary["results_count"] = stdout_value["results"].size();
        if (stdout_value.contains("items") && stdout_value["items"].is_array()) summary["items_count"] = stdout_value["items"].size();
        if (stdout_value.contains("warnings") && stdout_value["warnings"].is_array()) summary["warnings_count"] = stdout_value["warnings"].size();
        if (stdout_value.contains("errors") && stdout_value["errors"].is_array()) summary["errors_count"] = stdout_value["errors"].size();
    } else if (!stdout_value.is_null()) {
        summary["stdout_type"] = stdout_value.type_name();
    }
    if (stderr_value.is_string() && !stderr_value.get<std::string>().empty()) {
        summary["stderr_preview"] = stderr_value.get<std::string>().substr(0, 240);
    } else if (!stderr_value.is_null()) {
        summary["stderr_type"] = stderr_value.type_name();
    }
    prune_empty_json(summary);
    return summary;
}

static fs::path executable_dir_for_logs() {
#ifdef _WIN32
    char buf[MAX_PATH];
    DWORD len = GetModuleFileNameA(nullptr, buf, MAX_PATH);
    if (len > 0 && len < MAX_PATH) return fs::path(buf).parent_path();
#endif
    return fs::current_path();
}

static fs::path query_log_root() {
    std::string override_dir = getenv_str("MONOLITH_TOOL_LOG_DIR");
    if (!override_dir.empty()) return fs::path(override_dir);

    fs::path cur = executable_dir_for_logs();
    for (int i = 0; i < 8 && !cur.empty(); ++i) {
        if (fs::exists(cur / "Monolith.uplugin")) return cur / "Logs";
        cur = cur.parent_path();
    }
    return fs::current_path() / "Logs";
}

#ifdef _WIN32
static HANDLE acquire_log_lock_file(const fs::path& lock_path) {
    const auto start = std::chrono::steady_clock::now();
    for (;;) {
        HANDLE lock_handle = CreateFileA(
            lock_path.string().c_str(),
            GENERIC_WRITE,
            0,
            nullptr,
            CREATE_NEW,
            FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE,
            nullptr);
        if (lock_handle != INVALID_HANDLE_VALUE) {
            return lock_handle;
        }

        const DWORD error = GetLastError();
        if (error != ERROR_FILE_EXISTS && error != ERROR_ALREADY_EXISTS
            && error != ERROR_SHARING_VIOLATION && error != ERROR_ACCESS_DENIED) {
            throw std::runtime_error("could not create log lock file");
        }

        WIN32_FILE_ATTRIBUTE_DATA attr{};
        if (GetFileAttributesExA(lock_path.string().c_str(), GetFileExInfoStandard, &attr)) {
            FILETIME now_ft{};
            GetSystemTimeAsFileTime(&now_ft);
            ULARGE_INTEGER now{};
            now.LowPart = now_ft.dwLowDateTime;
            now.HighPart = now_ft.dwHighDateTime;
            ULARGE_INTEGER modified{};
            modified.LowPart = attr.ftLastWriteTime.dwLowDateTime;
            modified.HighPart = attr.ftLastWriteTime.dwHighDateTime;
            if (now.QuadPart > modified.QuadPart
                && now.QuadPart - modified.QuadPart > 30ULL * 1000ULL * 1000ULL * 10ULL) {
                DeleteFileA(lock_path.string().c_str());
            }
        } else if (GetLastError() == ERROR_FILE_NOT_FOUND) {
            continue;
        }

        if (std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count() > 5000) {
            throw std::runtime_error("timed out acquiring tool log lock");
        }
        Sleep(25);
    }
}
#endif

static void append_query_log(const json& record) {
    if (!tool_log_enabled()) return;
    static std::mutex log_mutex;
    try {
        std::lock_guard<std::mutex> guard(log_mutex);
        json cleaned = record;
        prune_empty_json(cleaned);
        fs::path path = query_log_root() / yyyymmdd_local() / "query.jsonl";
        fs::create_directories(path.parent_path());
#ifdef _WIN32
        const fs::path lock_path = path.string() + ".lock";
        HANDLE lock_handle = acquire_log_lock_file(lock_path);
        try {
            std::ofstream out(path, std::ios::app | std::ios::binary);
            out << cleaned.dump() << "\n";
            if (!out) throw std::runtime_error("could not append daily log");
        } catch (...) {
            CloseHandle(lock_handle);
            throw;
        }
        CloseHandle(lock_handle);
#else
        std::ofstream out(path, std::ios::app | std::ios::binary);
        out << cleaned.dump() << "\n";
        if (!out) throw std::runtime_error("could not append daily log");
#endif
    } catch (const std::exception& e) {
        std::cerr << "[monolith-query] daily log failed: " << e.what() << std::endl;
    }
}

static json argv_json(int argc, char* argv[]) {
    json out = json::array();
    for (int i = 1; i < argc; ++i) out.push_back(argv[i] ? argv[i] : "");
    return redact_json(out);
}

// ============================================================
// Fuzzy match — ports MonolithFuzzyMatchDetail::ScoreFuzzyMatches
// (Source/MonolithCore/Private/MonolithFuzzyMatch.cpp). The live version uses
// Algo::LevenshteinDistance (a UE dependency this standalone tool cannot link),
// so the distance is reimplemented inline. Score = 1 - dist/maxLen, top-N
// descending, stable sort on ties — behaviourally identical to the live matcher
// and the Python port in monolith_offline.py.
// ============================================================
