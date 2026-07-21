#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <bcrypt.h>

#include "monolith_proxy_offline.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdlib>
#include <cwchar>
#include <exception>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

#include <nlohmann/json.hpp>

#pragma comment(lib, "bcrypt.lib")

using json = nlohmann::json;
namespace fs = std::filesystem;

namespace
{
    constexpr DWORD QueryTimeoutMs = 30000;
    constexpr size_t OutputMaxBytes = 16 * 1024 * 1024;
    constexpr size_t DrainBudgetBytes = 256 * 1024;

    const std::set<std::string> QueryNamespaces = {
        "source",
        "project",
        "bridge",
        "console",
        "cppreflect",
        "network",
        "decision",
        "risk",
    };

    const std::map<std::string, std::string> MonolithTools = {
        {"monolith_status", "status"},
        {"monolith_discover", "discover"},
        {"monolith_find", "find"},
        {"monolith_guide", "guide"},
        {"monolith_get_action_metadata_coverage", "get_action_metadata_coverage"},
    };

    struct Invocation
    {
        std::string query_namespace;
        std::string action;
        json params = json::object();
        json response_shaping = json::object();
    };

    struct ProcessResult
    {
        bool launched = false;
        bool timed_out = false;
        bool stdout_truncated = false;
        bool stderr_truncated = false;
        DWORD exit_code = 0;
        DWORD win32_error = ERROR_SUCCESS;
        double duration_ms = 0.0;
        std::string stdout_text;
        std::string stderr_text;
        std::string failure_message;
    };

    class ScopedKernelHandle
    {
    public:
        ScopedKernelHandle() = default;
        explicit ScopedKernelHandle(HANDLE value) : value_(value) {}
        ~ScopedKernelHandle()
        {
            if (value_ && value_ != INVALID_HANDLE_VALUE)
                CloseHandle(value_);
        }
        ScopedKernelHandle(const ScopedKernelHandle&) = delete;
        ScopedKernelHandle& operator=(const ScopedKernelHandle&) = delete;
        ScopedKernelHandle(ScopedKernelHandle&& other) noexcept
            : value_(other.release()) {}
        ScopedKernelHandle& operator=(ScopedKernelHandle&& other) noexcept
        {
            if (this != &other)
            {
                if (value_ && value_ != INVALID_HANDLE_VALUE)
                    CloseHandle(value_);
                value_ = other.release();
            }
            return *this;
        }
        HANDLE get() const { return value_; }
        bool valid() const { return value_ && value_ != INVALID_HANDLE_VALUE; }
        HANDLE release()
        {
            const HANDLE released = value_;
            value_ = INVALID_HANDLE_VALUE;
            return released;
        }

    private:
        HANDLE value_ = INVALID_HANDLE_VALUE;
    };

    struct QueryBundle
    {
        bool valid = false;
        fs::path query_path;
        fs::path catalog_path;
        std::string error;
        ScopedKernelHandle query_pin;
        ScopedKernelHandle catalog_pin;
    };

    const QueryBundle& GetQueryBundle();

    std::string GetEnv(const char* name)
    {
        const char* value = std::getenv(name);
        return value ? std::string(value) : std::string();
    }

    std::wstring ToWide(const std::string& value)
    {
        if (value.empty()) return {};
        const int size = MultiByteToWideChar(
            CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
            static_cast<int>(value.size()), nullptr, 0);
        if (size <= 0) return {};
        std::wstring wide(size, L'\0');
        if (MultiByteToWideChar(
                CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                static_cast<int>(value.size()), wide.data(), size) <= 0)
        {
            return {};
        }
        return wide;
    }

    bool EndsWith(const std::string& value, const std::string& suffix)
    {
        return value.size() >= suffix.size()
            && value.compare(value.size() - suffix.size(), suffix.size(), suffix) == 0;
    }

    bool IsValidActionName(const std::string& value)
    {
        if (value.empty()) return false;
        return std::all_of(value.begin(), value.end(), [](unsigned char ch)
        {
            return (ch >= 'A' && ch <= 'Z')
                || (ch >= 'a' && ch <= 'z')
                || (ch >= '0' && ch <= '9')
                || ch == '_';
        });
    }

    bool ReadShapingStringArray(
        const json& value,
        const std::string& key,
        json& normalized,
        std::string& error)
    {
        json array_value;
        if (value.is_array())
        {
            array_value = value;
        }
        else if (value.is_string())
        {
            try
            {
                array_value = json::parse(value.get<std::string>());
            }
            catch (...)
            {
                error = "Offline response-shaping parameter '" + key
                    + "' must be an array of strings.";
                return false;
            }
        }
        else
        {
            error = "Offline response-shaping parameter '" + key
                + "' must be an array of strings.";
            return false;
        }

        if (!array_value.is_array())
        {
            error = "Offline response-shaping parameter '" + key
                + "' must be an array of strings.";
            return false;
        }
        for (const json& item : array_value)
        {
            if (!item.is_string())
            {
                error = "Offline response-shaping parameter '" + key
                    + "' must contain only strings.";
                return false;
            }
        }

        normalized = std::move(array_value);
        return true;
    }

    bool ReadShapingBool(
        const json& value,
        const std::string& key,
        json& normalized,
        std::string& error)
    {
        if (value.is_boolean())
        {
            normalized = value;
            return true;
        }
        if (value.is_string())
        {
            std::string text = value.get<std::string>();
            std::transform(
                text.begin(), text.end(), text.begin(),
                [](unsigned char character)
                {
                    return static_cast<char>(std::tolower(character));
                });
            if (text == "true" || text == "false")
            {
                normalized = text == "true";
                return true;
            }
        }

        error = "Offline response-shaping parameter '" + key
            + "' must be a boolean.";
        return false;
    }

    bool ExtractResponseShaping(
        json& params,
        json& response_shaping,
        std::string& error)
    {
        response_shaping = json::object();
        const std::vector<std::string> array_keys = {"_fields", "_omit"};
        for (const std::string& key : array_keys)
        {
            const json* value = nullptr;
            auto value_it = params.find(key);
            if (value_it != params.end())
                value = &*value_it;
            if (value != nullptr)
            {
                json normalized;
                if (!ReadShapingStringArray(*value, key, normalized, error))
                    return false;
                response_shaping[key] = std::move(normalized);
            }
            params.erase(key);
        }

        const std::string compact_key = "_compact_json";
        const json* compact_value = nullptr;
        auto compact_it = params.find(compact_key);
        if (compact_it != params.end())
            compact_value = &*compact_it;
        if (compact_value != nullptr)
        {
            json normalized;
            if (!ReadShapingBool(
                    *compact_value, compact_key, normalized, error))
            {
                return false;
            }
            response_shaping[compact_key] = std::move(normalized);
        }
        params.erase(compact_key);
        return true;
    }

    bool MergeInvocationParams(
        const json& args,
        const std::set<std::string>& reserved_top_level,
        json& merged,
        std::string& error)
    {
        merged = json::object();
        for (auto it = args.begin(); it != args.end(); ++it)
        {
            if (reserved_top_level.count(it.key()) == 0)
                merged[it.key()] = it.value();
        }

        auto params_it = args.find("params");
        if (params_it == args.end())
            return true;
        if (params_it->is_null())
        {
            error = "Offline fallback parameter 'params' must be a JSON object "
                "or a JSON-encoded object string when provided.";
            return false;
        }

        json nested;
        if (params_it->is_object())
        {
            nested = *params_it;
        }
        else if (params_it->is_string())
        {
            const std::string text = params_it->get<std::string>();
            try
            {
                nested = json::parse(text);
            }
            catch (...)
            {
                error = "Offline fallback parameter 'params' must contain a "
                    "valid JSON-encoded object.";
                return false;
            }
            if (!nested.is_object())
            {
                error = "Offline fallback parameter 'params' must decode to a "
                    "JSON object.";
                return false;
            }
        }
        else
        {
            error = "Offline fallback parameter 'params' must be a JSON object "
                "or a JSON-encoded object string when provided.";
            return false;
        }

        for (auto it = nested.begin(); it != nested.end(); ++it)
            merged[it.key()] = it.value();
        return true;
    }

    bool ResolveInvocation(
        const std::string& tool_name,
        const json& args,
        Invocation& invocation,
        std::string& error)
    {
        if (tool_name == "monolith_query")
        {
            if (!args.is_object())
            {
                error = "Offline fallback requires the tool arguments to be an object.";
                return true;
            }
            auto namespace_it = args.find("namespace");
            auto action_it = args.find("action");
            if (namespace_it == args.end() || !namespace_it->is_string()
                || namespace_it->get<std::string>().empty())
            {
                error = "Offline fallback requires a non-empty 'namespace' string.";
                return true;
            }
            if (action_it == args.end() || !action_it->is_string()
                || action_it->get<std::string>().empty())
            {
                error = "Offline fallback requires a non-empty 'action' string.";
                return true;
            }
            invocation.query_namespace = namespace_it->get<std::string>();
            invocation.action = action_it->get<std::string>();
            if (QueryNamespaces.count(invocation.query_namespace) == 0
                && invocation.query_namespace != "monolith")
            {
                error = "Offline fallback does not serve namespace '"
                    + invocation.query_namespace
                    + "'; restore the live editor endpoint for this namespace.";
                return true;
            }
            if (invocation.action.find('\0') != std::string::npos
                || !IsValidActionName(invocation.action))
            {
                error = "Offline fallback action names must match [A-Za-z0-9_]+.";
                return true;
            }
            if (!MergeInvocationParams(
                    args, {"namespace", "action", "params"},
                    invocation.params, error))
            {
                return true;
            }
            ExtractResponseShaping(
                invocation.params, invocation.response_shaping, error);
            return true;
        }

        const std::string query_suffix = "_query";
        if (EndsWith(tool_name, query_suffix))
        {
            const std::string query_namespace =
                tool_name.substr(0, tool_name.size() - query_suffix.size());
            if (!MonolithToolSupportsOfflineFallback(tool_name))
                return false;

            invocation.query_namespace = query_namespace;
            if (!args.is_object())
            {
                error = "Offline fallback requires the tool arguments to be an object.";
                return true;
            }

            auto action_it = args.find("action");
            if (action_it == args.end() || !action_it->is_string()
                || action_it->get<std::string>().empty())
            {
                error = "Offline fallback requires a non-empty 'action' string.";
                return true;
            }
            invocation.action = action_it->get<std::string>();
            if (invocation.action.find('\0') != std::string::npos)
            {
                error = "Offline fallback action strings cannot contain NUL bytes.";
                return true;
            }
            if (!IsValidActionName(invocation.action))
            {
                error = "Offline fallback action names must match [A-Za-z0-9_]+.";
                return true;
            }

            if (!MergeInvocationParams(
                    args, {"action", "params"}, invocation.params, error))
            {
                return true;
            }
            ExtractResponseShaping(
                invocation.params, invocation.response_shaping, error);
            return true;
        }

        auto monolith_it = MonolithTools.find(tool_name);
        if (monolith_it == MonolithTools.end())
            return false;
        if (!args.is_object())
        {
            error = "Offline fallback requires the tool arguments to be an object.";
            return true;
        }

        invocation.query_namespace = "monolith";
        invocation.action = monolith_it->second;
        if (!MergeInvocationParams(
                args, {"params"}, invocation.params, error))
        {
            return true;
        }
        ExtractResponseShaping(
            invocation.params, invocation.response_shaping, error);
        return true;
    }

    bool ToSnakeOptionKey(
        const std::string& input,
        std::string& output,
        std::string& error)
    {
        output.clear();
        if (input.empty())
        {
            error = "Offline fallback parameter names cannot be empty.";
            return false;
        }

        for (size_t index = 0; index < input.size(); ++index)
        {
            const unsigned char character =
                static_cast<unsigned char>(input[index]);
            if (character == '-')
            {
                output.push_back('_');
            }
            else if (character >= 'A' && character <= 'Z')
            {
                if (index > 0 && !output.empty() && output.back() != '_')
                {
                    const unsigned char previous =
                        static_cast<unsigned char>(input[index - 1]);
                    if ((previous >= 'a' && previous <= 'z')
                        || (previous >= '0' && previous <= '9'))
                    {
                        output.push_back('_');
                    }
                }
                output.push_back(static_cast<char>(std::tolower(character)));
            }
            else if ((character >= 'a' && character <= 'z')
                || (character >= '0' && character <= '9')
                || character == '_')
            {
                output.push_back(static_cast<char>(character));
            }
            else
            {
                error = "Offline fallback parameter '" + input
                    + "' contains unsupported characters.";
                return false;
            }
        }
        return !output.empty();
    }

    bool ToCliValue(
        const json& value,
        std::string& output,
        std::string& error)
    {
        if (value.is_string())
        {
            output = value.get<std::string>();
            if (output.find('\0') != std::string::npos)
            {
                error = "Offline fallback string parameters cannot contain NUL bytes.";
                return false;
            }
            return true;
        }
        if (value.is_boolean() || value.is_number())
        {
            output = value.dump(-1);
            return true;
        }
        if (value.is_array())
        {
            bool all_strings = !value.empty();
            for (const auto& item : value)
                all_strings = all_strings && item.is_string();

            if (all_strings)
            {
                for (const auto& item_value : value)
                {
                    const std::string item = item_value.get<std::string>();
                    if (item.find('\0') != std::string::npos)
                    {
                        error = "Offline fallback string-array parameters cannot contain NUL bytes.";
                        return false;
                    }
                }
                // Preserve array element boundaries instead of flattening to CSV.
                // C++ symbols and paths may legitimately contain commas.
                output = value.dump(-1);
                return true;
            }
        }
        if (value.is_object() || value.is_array())
        {
            output = value.dump(-1);
            return true;
        }

        error = "Offline fallback null parameters are omitted, and all other values "
            "must be JSON scalars, arrays, or objects.";
        return false;
    }

    bool BuildCliArguments(
        const Invocation& invocation,
        std::vector<std::string>& arguments,
        std::string& parameter_types_json,
        std::string& error)
    {
        arguments = {invocation.query_namespace, invocation.action};
        json parameter_types = json::object();
        std::set<std::string> normalized_keys;
        for (auto it = invocation.params.begin(); it != invocation.params.end(); ++it)
        {
            if (it.value().is_null())
            {
                error = "Offline fallback parameter '" + it.key()
                    + "' must be non-null.";
                return false;
            }
            std::string key;
            if (!ToSnakeOptionKey(it.key(), key, error))
                return false;
            static const std::set<std::string> ReservedProxyOptions = {
                "readonly",
                "no_log",
                "db",
                "source_db",
                "project_db",
                "snapshot",
            };
            if (ReservedProxyOptions.count(key) > 0)
            {
                error = "Offline fallback parameter '" + it.key()
                    + "' attempts to override a proxy-controlled query path or safety option. "
                      "Use monolith_query.exe directly for an explicitly selected copied database.";
                return false;
            }
            if (!normalized_keys.insert(key).second)
            {
                error = "Offline fallback parameters collide after snake_case "
                    "normalization: " + key;
                return false;
            }
            if (it.value().is_string()) parameter_types[key] = "string";
            else if (it.value().is_boolean()) parameter_types[key] = "boolean";
            else if (it.value().is_number_integer() || it.value().is_number_unsigned()) parameter_types[key] = "integer";
            else if (it.value().is_number()) parameter_types[key] = "number";
            else if (it.value().is_array()) parameter_types[key] = "array";
            else if (it.value().is_object()) parameter_types[key] = "object";
            else parameter_types[key] = "unknown";

            std::string value;
            if (!ToCliValue(it.value(), value, error))
                return false;
            arguments.push_back("--" + key + "=" + value);
        }

        auto append_trusted_database = [&](
            const char* environment_name,
            const char* option_name) -> bool
        {
            const std::string configured = GetEnv(environment_name);
            if (configured.empty()) return true;
            if (configured.find('\0') != std::string::npos)
            {
                error = std::string(environment_name) + " contains a NUL byte.";
                return false;
            }
            const fs::path requested = fs::u8path(configured);
            std::error_code path_error;
            if (!fs::is_regular_file(requested, path_error) || path_error)
            {
                error = std::string(environment_name)
                    + " must identify an existing regular database file.";
                return false;
            }
#ifdef _WIN32
            const DWORD attributes = GetFileAttributesW(requested.c_str());
            if (attributes == INVALID_FILE_ATTRIBUTES
                || (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
            {
                error = std::string(environment_name)
                    + " must not identify a reparse point.";
                return false;
            }
#endif
            const fs::path canonical = fs::weakly_canonical(requested, path_error);
            if (path_error || canonical.empty())
            {
                error = std::string("Could not canonicalize ") + environment_name + ".";
                return false;
            }
            const std::string key(option_name);
            arguments.push_back("--" + key + "=" + canonical.u8string());
            parameter_types[key] = "string";
            return true;
        };

        const std::string& query_namespace = invocation.query_namespace;
        const bool uses_source_db = query_namespace == "source"
            || query_namespace == "console"
            || query_namespace == "cppreflect"
            || query_namespace == "network"
            || query_namespace == "decision"
            || query_namespace == "risk"
            || query_namespace == "bridge";
        const bool uses_project_db = query_namespace == "project"
            || query_namespace == "bridge";
        if (uses_source_db
            && !append_trusted_database(
                "MONOLITH_OFFLINE_SOURCE_DB", "source_db"))
            return false;
        if (uses_project_db
            && !append_trusted_database(
                "MONOLITH_OFFLINE_PROJECT_DB", "project_db"))
            return false;
        parameter_types_json = parameter_types.dump(-1);
        return true;
    }

    fs::path ResolveQueryExecutable()
    {
        const QueryBundle& bundle = GetQueryBundle();
        return bundle.valid ? bundle.query_path : fs::path{};
    }

    std::string Win32ErrorMessage(DWORD error);

    bool PinQueryExecutable(
        const fs::path& requested,
        ScopedKernelHandle& pinned,
        fs::path& final_path,
        DWORD& win32_error,
        std::string& error)
    {
        ScopedKernelHandle candidate(CreateFileW(
            requested.c_str(),
            GENERIC_READ | FILE_EXECUTE,
            FILE_SHARE_READ,
            nullptr,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT,
            nullptr));
        if (!candidate.valid())
        {
            win32_error = GetLastError();
            error = "Offline query executable could not be opened safely: "
                + requested.u8string() + " (" + Win32ErrorMessage(win32_error) + ")";
            return false;
        }

        FILE_ATTRIBUTE_TAG_INFO tag_info{};
        if (!GetFileInformationByHandleEx(
                candidate.get(), FileAttributeTagInfo,
                &tag_info, sizeof(tag_info)))
        {
            win32_error = GetLastError();
            error = "Could not inspect offline query executable attributes: "
                + Win32ErrorMessage(win32_error);
            return false;
        }
        if ((tag_info.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0
            || (tag_info.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
        {
            win32_error = ERROR_REPARSE_TAG_INVALID;
            error = "Offline query executable must be a regular, non-reparse file.";
            return false;
        }

        std::vector<wchar_t> buffer(32768, L'\0');
        const DWORD length = GetFinalPathNameByHandleW(
            candidate.get(), buffer.data(), static_cast<DWORD>(buffer.size()),
            FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
        if (length == 0 || length >= buffer.size())
        {
            win32_error = length == 0 ? GetLastError() : ERROR_INSUFFICIENT_BUFFER;
            error = "Could not resolve pinned offline query executable path: "
                + Win32ErrorMessage(win32_error);
            return false;
        }

        std::wstring normalized(buffer.data(), length);
        // CreateProcessW accepts extended paths, but a child launched with a
        // `\\?\` image path observes that prefix through GetModuleFileNameW.
        // Query's executable-relative Saved/*.db discovery deliberately uses
        // normal DOS path semantics, so convert the already-resolved final path
        // back to its equivalent DOS/UNC spelling while the file handle remains
        // pinned against replacement.
        if (normalized.rfind(L"\\\\?\\UNC\\", 0) == 0)
            normalized = L"\\\\" + normalized.substr(8);
        else if (normalized.rfind(L"\\\\?\\", 0) == 0)
            normalized = normalized.substr(4);
        final_path = fs::path(std::move(normalized));
        pinned = std::move(candidate);
        return true;
    }

    bool PinRegularDataFile(
        const fs::path& requested,
        const std::string& label,
        ScopedKernelHandle& pinned,
        fs::path& final_path,
        std::string& error)
    {
        ScopedKernelHandle candidate(CreateFileW(
            requested.c_str(),
            GENERIC_READ,
            FILE_SHARE_READ,
            nullptr,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT,
            nullptr));
        if (!candidate.valid())
        {
            error = label + " could not be opened safely: "
                + requested.u8string() + " (Win32 "
                + std::to_string(GetLastError()) + ")";
            return false;
        }

        FILE_ATTRIBUTE_TAG_INFO tag_info{};
        if (!GetFileInformationByHandleEx(
                candidate.get(), FileAttributeTagInfo,
                &tag_info, sizeof(tag_info)))
        {
            error = "Could not inspect " + label + " attributes (Win32 "
                + std::to_string(GetLastError()) + ").";
            return false;
        }
        if ((tag_info.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0
            || (tag_info.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
        {
            error = label + " must be a regular, non-reparse file.";
            return false;
        }

        std::vector<wchar_t> buffer(32768, L'\0');
        const DWORD length = GetFinalPathNameByHandleW(
            candidate.get(), buffer.data(), static_cast<DWORD>(buffer.size()),
            FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
        if (length == 0 || length >= buffer.size())
        {
            error = "Could not resolve pinned " + label + " path (Win32 "
                + std::to_string(length == 0 ? GetLastError() : ERROR_INSUFFICIENT_BUFFER)
                + ").";
            return false;
        }
        std::wstring normalized(buffer.data(), length);
        if (normalized.rfind(L"\\\\?\\UNC\\", 0) == 0)
            normalized = L"\\\\" + normalized.substr(8);
        else if (normalized.rfind(L"\\\\?\\", 0) == 0)
            normalized = normalized.substr(4);
        final_path = fs::path(std::move(normalized));
        pinned = std::move(candidate);
        return true;
    }

    bool ReadPinnedBytes(
        HANDLE file,
        size_t max_bytes,
        const std::string& label,
        std::vector<unsigned char>& bytes,
        std::string& error)
    {
        LARGE_INTEGER size{};
        if (!GetFileSizeEx(file, &size) || size.QuadPart < 0
            || static_cast<uint64_t>(size.QuadPart) > max_bytes)
        {
            error = label + " size is unavailable or exceeds "
                + std::to_string(max_bytes) + " bytes.";
            return false;
        }
        LARGE_INTEGER zero{};
        if (!SetFilePointerEx(file, zero, nullptr, FILE_BEGIN))
        {
            error = "Could not rewind " + label + ".";
            return false;
        }
        bytes.assign(static_cast<size_t>(size.QuadPart), 0);
        size_t offset = 0;
        while (offset < bytes.size())
        {
            const DWORD requested = static_cast<DWORD>(
                std::min<size_t>(bytes.size() - offset, 1024 * 1024));
            DWORD read = 0;
            if (!ReadFile(file, bytes.data() + offset, requested, &read, nullptr)
                || read == 0)
            {
                error = "Could not read the complete pinned " + label + ".";
                return false;
            }
            offset += read;
        }
        SetFilePointerEx(file, zero, nullptr, FILE_BEGIN);
        return true;
    }

    bool Sha256Bytes(
        const std::vector<unsigned char>& bytes,
        std::string& hex,
        std::string& error)
    {
        BCRYPT_ALG_HANDLE algorithm = nullptr;
        BCRYPT_HASH_HANDLE hash = nullptr;
        std::vector<unsigned char> object;
        std::vector<unsigned char> digest;
        auto cleanup = [&]()
        {
            if (hash) BCryptDestroyHash(hash);
            if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
        };
        if (BCryptOpenAlgorithmProvider(
                &algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0)
        {
            error = "Could not open the native SHA-256 provider.";
            cleanup();
            return false;
        }
        DWORD object_size = 0;
        DWORD digest_size = 0;
        DWORD copied = 0;
        if (BCryptGetProperty(
                algorithm, BCRYPT_OBJECT_LENGTH,
                reinterpret_cast<PUCHAR>(&object_size), sizeof(object_size),
                &copied, 0) < 0
            || BCryptGetProperty(
                algorithm, BCRYPT_HASH_LENGTH,
                reinterpret_cast<PUCHAR>(&digest_size), sizeof(digest_size),
                &copied, 0) < 0)
        {
            error = "Could not inspect native SHA-256 provider sizes.";
            cleanup();
            return false;
        }
        object.resize(object_size);
        digest.resize(digest_size);
        if (BCryptCreateHash(
                algorithm, &hash, object.data(), object_size,
                nullptr, 0, 0) < 0)
        {
            error = "Could not create native SHA-256 state.";
            cleanup();
            return false;
        }
        size_t offset = 0;
        while (offset < bytes.size())
        {
            const ULONG chunk = static_cast<ULONG>(
                std::min<size_t>(bytes.size() - offset, 1024 * 1024));
            if (BCryptHashData(
                    hash,
                    const_cast<PUCHAR>(bytes.data() + offset), chunk, 0) < 0)
            {
                error = "Could not update native SHA-256 state.";
                cleanup();
                return false;
            }
            offset += chunk;
        }
        if (BCryptFinishHash(
                hash, digest.data(), static_cast<ULONG>(digest.size()), 0) < 0)
        {
            error = "Could not finish native SHA-256 state.";
            cleanup();
            return false;
        }
        static const char* digits = "0123456789abcdef";
        hex.clear();
        hex.reserve(digest.size() * 2);
        for (unsigned char byte : digest)
        {
            hex.push_back(digits[byte >> 4]);
            hex.push_back(digits[byte & 0x0f]);
        }
        cleanup();
        return true;
    }

    bool IsLowerHex(const std::string& value, size_t length)
    {
        return value.size() == length
            && std::all_of(value.begin(), value.end(), [](unsigned char ch)
            {
                return (ch >= '0' && ch <= '9')
                    || (ch >= 'a' && ch <= 'f');
            });
    }

    std::string CanonicalPathKey(const fs::path& path)
    {
        std::error_code path_error;
        std::string key = fs::weakly_canonical(path, path_error)
            .lexically_normal().u8string();
        if (path_error) return {};
        std::replace(key.begin(), key.end(), '\\', '/');
        std::transform(key.begin(), key.end(), key.begin(),
            [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
        return key;
    }

    fs::path ProxyExecutableDirectory()
    {
        std::vector<wchar_t> path_buffer(32768, L'\0');
        const DWORD length = GetModuleFileNameW(
            nullptr, path_buffer.data(),
            static_cast<DWORD>(path_buffer.size()));
        if (length == 0 || length >= path_buffer.size()) return {};
        return fs::path(std::wstring(path_buffer.data(), length)).parent_path();
    }

    bool IsExactLeaf(const std::string& value)
    {
        if (value.empty()) return false;
        const fs::path leaf = fs::u8path(value);
        return leaf.filename() == leaf && leaf.parent_path().empty();
    }

    QueryBundle LoadQueryBundle()
    {
        QueryBundle bundle;
        const fs::path binaries_root = ProxyExecutableDirectory();
        if (binaries_root.empty())
        {
            bundle.error = "Could not resolve the native proxy executable directory.";
            return bundle;
        }

        const std::string query_override = GetEnv("MONOLITH_QUERY_EXE");
        const std::string catalog_override = GetEnv("MONOLITH_CATALOG_SNAPSHOT");
        if (!query_override.empty() || !catalog_override.empty())
        {
            const fs::path requested_query = !query_override.empty()
                ? fs::u8path(query_override)
                : binaries_root / L"monolith_query.exe";
            const fs::path plugin_root = binaries_root.parent_path();
            const fs::path requested_catalog = !catalog_override.empty()
                ? fs::u8path(catalog_override)
                : plugin_root / L"Tools" / L"MonolithQuery" / L"Generated"
                    / L"monolith_catalog_snapshot.json";
            DWORD win32_error = ERROR_SUCCESS;
            if (!PinQueryExecutable(
                    requested_query, bundle.query_pin, bundle.query_path,
                    win32_error, bundle.error)
                || !PinRegularDataFile(
                    requested_catalog, "Offline catalog override",
                    bundle.catalog_pin, bundle.catalog_path, bundle.error))
            {
                return bundle;
            }
            bundle.valid = true;
            return bundle;
        }

        const fs::path manifest_path = binaries_root
            / L"monolith_query.current.json";
        ScopedKernelHandle manifest_pin;
        fs::path final_manifest;
        if (!PinRegularDataFile(
                manifest_path, "Query bundle manifest", manifest_pin,
                final_manifest, bundle.error))
        {
            return bundle;
        }
        std::vector<unsigned char> manifest_bytes;
        if (!ReadPinnedBytes(
                manifest_pin.get(), 64 * 1024, "Query bundle manifest",
                manifest_bytes, bundle.error))
        {
            return bundle;
        }

        json manifest;
        try
        {
            manifest = json::parse(manifest_bytes.begin(), manifest_bytes.end());
        }
        catch (const std::exception& exception)
        {
            bundle.error = std::string("Query bundle manifest is invalid JSON: ")
                + exception.what();
            return bundle;
        }
        static const std::set<std::string> ExpectedFields = {
            "schema_version", "tool", "runtime", "file", "plugin_version",
            "parity_spec_rev", "source_hash", "sha256", "catalog_file",
            "catalog_source_hash", "catalog_sha256",
        };
        if (!manifest.is_object() || manifest.size() != ExpectedFields.size())
        {
            bundle.error = "Query bundle manifest has an unexpected field set.";
            return bundle;
        }
        for (auto it = manifest.begin(); it != manifest.end(); ++it)
        {
            if (!ExpectedFields.count(it.key()))
            {
                bundle.error = "Query bundle manifest contains an unknown field.";
                return bundle;
            }
        }
        auto required_string = [&](const char* field, std::string& out) -> bool
        {
            auto found = manifest.find(field);
            if (found == manifest.end() || !found->is_string()
                || found->get_ref<const std::string&>().empty())
            {
                bundle.error = std::string("Query bundle manifest field '")
                    + field + "' must be a non-empty string.";
                return false;
            }
            out = found->get<std::string>();
            return true;
        };
        if (!manifest.contains("schema_version")
            || !manifest["schema_version"].is_number_integer()
            || manifest["schema_version"].get<int>() != 1
            || manifest.value("tool", "") != "monolith_query"
            || manifest.value("runtime", "") != "native-cpp")
        {
            bundle.error = "Query bundle manifest schema/tool/runtime identity is invalid.";
            return bundle;
        }

        std::string query_file;
        std::string query_source_hash;
        std::string query_sha;
        std::string catalog_file;
        std::string catalog_source_hash;
        std::string catalog_sha;
        std::string plugin_version;
        std::string parity_spec_rev;
        if (!required_string("file", query_file)
            || !required_string("source_hash", query_source_hash)
            || !required_string("sha256", query_sha)
            || !required_string("catalog_file", catalog_file)
            || !required_string("catalog_source_hash", catalog_source_hash)
            || !required_string("catalog_sha256", catalog_sha)
            || !required_string("plugin_version", plugin_version)
            || !required_string("parity_spec_rev", parity_spec_rev))
        {
            return bundle;
        }
        if (!IsLowerHex(query_source_hash, 16)
            || !IsLowerHex(query_sha, 64)
            || !IsLowerHex(catalog_source_hash, 64)
            || !IsLowerHex(catalog_sha, 64)
            || !IsExactLeaf(query_file)
            || !IsExactLeaf(catalog_file)
            || query_file != "monolith_query-" + query_source_hash + ".exe"
            || catalog_file != "monolith_catalog-" + catalog_source_hash + ".json")
        {
            bundle.error = "Query bundle manifest filenames or hashes are invalid.";
            return bundle;
        }

        DWORD win32_error = ERROR_SUCCESS;
        if (!PinQueryExecutable(
                binaries_root / fs::u8path(query_file), bundle.query_pin,
                bundle.query_path, win32_error, bundle.error)
            || !PinRegularDataFile(
                binaries_root / fs::u8path(catalog_file),
                "Immutable offline catalog", bundle.catalog_pin,
                bundle.catalog_path, bundle.error))
        {
            return bundle;
        }
        const std::string root_key = CanonicalPathKey(binaries_root);
        if (root_key.empty()
            || CanonicalPathKey(bundle.query_path.parent_path()) != root_key
            || CanonicalPathKey(bundle.catalog_path.parent_path()) != root_key)
        {
            bundle.error = "Query bundle files escaped the proxy Binaries directory.";
            return bundle;
        }

        std::vector<unsigned char> query_bytes;
        std::vector<unsigned char> catalog_bytes;
        std::string actual_query_sha;
        std::string actual_catalog_sha;
        if (!ReadPinnedBytes(
                bundle.query_pin.get(), 128 * 1024 * 1024,
                "Immutable Query executable", query_bytes, bundle.error)
            || !ReadPinnedBytes(
                bundle.catalog_pin.get(), 64 * 1024 * 1024,
                "Immutable offline catalog", catalog_bytes, bundle.error)
            || !Sha256Bytes(query_bytes, actual_query_sha, bundle.error)
            || !Sha256Bytes(catalog_bytes, actual_catalog_sha, bundle.error))
        {
            return bundle;
        }
        if (actual_query_sha != query_sha || actual_catalog_sha != catalog_sha)
        {
            bundle.error = "Query bundle file SHA-256 does not match the manifest.";
            return bundle;
        }
        try
        {
            const json catalog = json::parse(
                catalog_bytes.begin(), catalog_bytes.end());
            if (!catalog.is_object()
                || catalog.value("source_hash", "") != catalog_source_hash
                || !catalog.contains("actions")
                || !catalog["actions"].is_array())
            {
                bundle.error = "Immutable offline catalog semantic identity is invalid.";
                return bundle;
            }
        }
        catch (const std::exception& exception)
        {
            bundle.error = std::string("Immutable offline catalog is invalid JSON: ")
                + exception.what();
            return bundle;
        }
        bundle.valid = true;
        return bundle;
    }

    const QueryBundle& GetQueryBundle()
    {
        static std::once_flag once;
        static QueryBundle bundle;
        std::call_once(once, []() { bundle = LoadQueryBundle(); });
        return bundle;
    }

    bool ChildImageMatchesPinnedExecutable(
        HANDLE process,
        HANDLE pinned_executable,
        DWORD& win32_error,
        std::string& error)
    {
        std::vector<wchar_t> image_path(32768, L'\0');
        DWORD image_path_size = static_cast<DWORD>(image_path.size());
        if (!QueryFullProcessImageNameW(
                process, 0, image_path.data(), &image_path_size))
        {
            win32_error = GetLastError();
            error = "Could not verify suspended offline query image path: "
                + Win32ErrorMessage(win32_error);
            return false;
        }

        ScopedKernelHandle child_image(CreateFileW(
            image_path.data(),
            GENERIC_READ,
            FILE_SHARE_READ,
            nullptr,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT,
            nullptr));
        if (!child_image.valid())
        {
            win32_error = GetLastError();
            error = "Could not open suspended offline query image for identity verification: "
                + Win32ErrorMessage(win32_error);
            return false;
        }

        FILE_ATTRIBUTE_TAG_INFO tag_info{};
        if (!GetFileInformationByHandleEx(
                child_image.get(), FileAttributeTagInfo,
                &tag_info, sizeof(tag_info)))
        {
            win32_error = GetLastError();
            error = "Could not inspect suspended offline query image attributes: "
                + Win32ErrorMessage(win32_error);
            return false;
        }
        if ((tag_info.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
        {
            win32_error = ERROR_REPARSE_TAG_INVALID;
            error = "Suspended offline query image resolved through a reparse point.";
            return false;
        }

        BY_HANDLE_FILE_INFORMATION expected{};
        BY_HANDLE_FILE_INFORMATION actual{};
        if (!GetFileInformationByHandle(pinned_executable, &expected)
            || !GetFileInformationByHandle(child_image.get(), &actual))
        {
            win32_error = GetLastError();
            error = "Could not compare offline query image file identity: "
                + Win32ErrorMessage(win32_error);
            return false;
        }
        const bool matches = expected.dwVolumeSerialNumber == actual.dwVolumeSerialNumber
            && expected.nFileIndexHigh == actual.nFileIndexHigh
            && expected.nFileIndexLow == actual.nFileIndexLow;
        if (!matches)
        {
            win32_error = ERROR_INVALID_DATA;
            error = "Suspended offline query image did not match the pinned executable file identity.";
        }
        return matches;
    }

    std::wstring QuoteWindowsArgument(const std::wstring& argument)
    {
        const bool needs_quotes = argument.empty()
            || argument.find_first_of(L" \t\n\v\"") != std::wstring::npos;
        if (!needs_quotes)
            return argument;

        std::wstring quoted(1, L'\"');
        size_t backslashes = 0;
        for (wchar_t character : argument)
        {
            if (character == L'\\')
            {
                ++backslashes;
                continue;
            }
            if (character == L'\"')
            {
                quoted.append(backslashes * 2 + 1, L'\\');
                quoted.push_back(L'\"');
                backslashes = 0;
                continue;
            }
            quoted.append(backslashes, L'\\');
            backslashes = 0;
            quoted.push_back(character);
        }
        quoted.append(backslashes * 2, L'\\');
        quoted.push_back(L'\"');
        return quoted;
    }

    bool EnvironmentEntryMatches(
        const std::wstring& entry,
        const wchar_t* variable_name)
    {
        size_t equals = entry.find(L'=');
        if (equals == 0)
            equals = entry.find(L'=', 1);
        if (equals == std::wstring::npos)
            return false;
        return _wcsicmp(entry.substr(0, equals).c_str(), variable_name) == 0;
    }

    std::vector<wchar_t> BuildEnvironment(
        const std::string& trace_id,
        const std::string& parent_span_id,
        const std::string& parameter_types_json)
    {
        std::vector<std::wstring> entries;
        LPWCH raw_environment = GetEnvironmentStringsW();
        if (raw_environment)
        {
            for (const wchar_t* cursor = raw_environment; *cursor != L'\0';)
            {
                std::wstring entry(cursor);
                cursor += entry.size() + 1;
                if (!EnvironmentEntryMatches(entry, L"MONOLITH_TRACE_ID")
                    && !EnvironmentEntryMatches(
                        entry, L"MONOLITH_PARENT_SPAN_ID")
                    && !EnvironmentEntryMatches(
                        entry, L"MONOLITH_MCP_PARAM_TYPES_JSON"))
                {
                    entries.push_back(std::move(entry));
                }
            }
            FreeEnvironmentStringsW(raw_environment);
        }

        entries.push_back(L"MONOLITH_TRACE_ID=" + ToWide(trace_id));
        entries.push_back(
            L"MONOLITH_PARENT_SPAN_ID=" + ToWide(parent_span_id));
        entries.push_back(
            L"MONOLITH_MCP_PARAM_TYPES_JSON=" + ToWide(parameter_types_json));
        std::sort(
            entries.begin(), entries.end(),
            [](const std::wstring& left, const std::wstring& right)
            {
                return _wcsicmp(left.c_str(), right.c_str()) < 0;
            });

        std::vector<wchar_t> block;
        for (const std::wstring& entry : entries)
        {
            block.insert(block.end(), entry.begin(), entry.end());
            block.push_back(L'\0');
        }
        block.push_back(L'\0');
        return block;
    }

    std::string Win32ErrorMessage(DWORD error)
    {
        LPSTR buffer = nullptr;
        const DWORD length = FormatMessageA(
            FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM
                | FORMAT_MESSAGE_IGNORE_INSERTS,
            nullptr,
            error,
            MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
            reinterpret_cast<LPSTR>(&buffer),
            0,
            nullptr);
        std::string message = length > 0 && buffer
            ? std::string(buffer, length)
            : "Win32 error " + std::to_string(error);
        if (buffer) LocalFree(buffer);
        while (!message.empty()
            && (message.back() == '\r' || message.back() == '\n'))
        {
            message.pop_back();
        }
        return message;
    }

    void DrainPipe(HANDLE pipe, std::string& output, bool& truncated)
    {
        if (!pipe || pipe == INVALID_HANDLE_VALUE)
            return;
        size_t drained = 0;
        while (drained < DrainBudgetBytes)
        {
            DWORD available = 0;
            if (!PeekNamedPipe(
                    pipe, nullptr, 0, nullptr, &available, nullptr)
                || available == 0)
            {
                return;
            }

            char buffer[8192];
            const DWORD requested = std::min<DWORD>(
                std::min<DWORD>(
                    available, static_cast<DWORD>(sizeof(buffer))),
                static_cast<DWORD>(DrainBudgetBytes - drained));
            DWORD bytes_read = 0;
            if (!ReadFile(pipe, buffer, requested, &bytes_read, nullptr)
                || bytes_read == 0)
            {
                return;
            }
            drained += bytes_read;

            if (output.size() < OutputMaxBytes)
            {
                const size_t remaining = OutputMaxBytes - output.size();
                const size_t accepted =
                    std::min<size_t>(remaining, bytes_read);
                output.append(buffer, accepted);
                if (accepted < bytes_read)
                    truncated = true;
            }
            else
            {
                truncated = true;
            }
        }
    }

    ProcessResult RunQuery(
        const std::vector<std::string>& cli_arguments,
        const std::string& trace_id,
        const std::string& parent_span_id,
        const std::string& parameter_types_json)
    {
        ProcessResult result;
        const auto start_clock = std::chrono::steady_clock::now();
        auto finish_duration = [&]()
        {
            result.duration_ms = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - start_clock).count();
        };

        const fs::path requested_executable = ResolveQueryExecutable();
        ScopedKernelHandle pinned_executable;
        fs::path executable;
        if (!PinQueryExecutable(
                requested_executable,
                pinned_executable,
                executable,
                result.win32_error,
                result.failure_message))
        {
            finish_duration();
            return result;
        }

        std::vector<std::wstring> wide_arguments = {executable.wstring()};
        for (const std::string& argument : cli_arguments)
        {
            const std::wstring wide_argument = ToWide(argument);
            if (!argument.empty() && wide_argument.empty())
            {
                result.win32_error = ERROR_NO_UNICODE_TRANSLATION;
                result.failure_message =
                    "Offline query argument is not valid UTF-8.";
                finish_duration();
                return result;
            }
            wide_arguments.push_back(wide_argument);
        }
        // Keep the immutable safety flag last as defense in depth: the query
        // parser scans global booleans across argv and the last value wins.
        wide_arguments.push_back(L"--readonly");

        std::wstring command_line;
        for (const std::wstring& argument : wide_arguments)
        {
            if (!command_line.empty()) command_line.push_back(L' ');
            command_line += QuoteWindowsArgument(argument);
        }
        if (command_line.size() >= 32767)
        {
            result.win32_error = ERROR_FILENAME_EXCED_RANGE;
            result.failure_message =
                "Offline query command line exceeds the Windows limit.";
            finish_duration();
            return result;
        }

        SECURITY_ATTRIBUTES security{};
        security.nLength = sizeof(security);
        security.bInheritHandle = TRUE;

        HANDLE stdout_read = nullptr;
        HANDLE stdout_write = nullptr;
        HANDLE stderr_read = nullptr;
        HANDLE stderr_write = nullptr;
        HANDLE null_input = INVALID_HANDLE_VALUE;
        auto close_handle = [](HANDLE& handle)
        {
            if (handle && handle != INVALID_HANDLE_VALUE)
                CloseHandle(handle);
            handle = nullptr;
        };
        auto cleanup_pipes = [&]()
        {
            close_handle(stdout_read);
            close_handle(stdout_write);
            close_handle(stderr_read);
            close_handle(stderr_write);
            if (null_input != INVALID_HANDLE_VALUE)
            {
                CloseHandle(null_input);
                null_input = INVALID_HANDLE_VALUE;
            }
        };

        if (!CreatePipe(&stdout_read, &stdout_write, &security, 0)
            || !SetHandleInformation(stdout_read, HANDLE_FLAG_INHERIT, 0)
            || !CreatePipe(&stderr_read, &stderr_write, &security, 0)
            || !SetHandleInformation(stderr_read, HANDLE_FLAG_INHERIT, 0))
        {
            result.win32_error = GetLastError();
            result.failure_message = "Failed to create offline query pipes: "
                + Win32ErrorMessage(result.win32_error);
            cleanup_pipes();
            finish_duration();
            return result;
        }

        null_input = CreateFileW(
            L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
            &security, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (null_input == INVALID_HANDLE_VALUE)
        {
            result.win32_error = GetLastError();
            result.failure_message =
                "Failed to open NUL for offline query stdin: "
                + Win32ErrorMessage(result.win32_error);
            cleanup_pipes();
            finish_duration();
            return result;
        }

        STARTUPINFOEXW startup{};
        startup.StartupInfo.cb = sizeof(startup);
        startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
        startup.StartupInfo.hStdInput = null_input;
        startup.StartupInfo.hStdOutput = stdout_write;
        startup.StartupInfo.hStdError = stderr_write;

        SIZE_T attribute_bytes = 0;
        InitializeProcThreadAttributeList(
            nullptr, 1, 0, &attribute_bytes);
        startup.lpAttributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(
            HeapAlloc(GetProcessHeap(), 0, attribute_bytes));
        if (!startup.lpAttributeList)
        {
            result.win32_error = ERROR_NOT_ENOUGH_MEMORY;
            result.failure_message =
                "Failed to allocate offline query process attributes.";
            cleanup_pipes();
            finish_duration();
            return result;
        }
        auto cleanup_attributes = [&]()
        {
            if (startup.lpAttributeList)
            {
                DeleteProcThreadAttributeList(startup.lpAttributeList);
                HeapFree(GetProcessHeap(), 0, startup.lpAttributeList);
                startup.lpAttributeList = nullptr;
            }
        };
        if (!InitializeProcThreadAttributeList(
                startup.lpAttributeList, 1, 0, &attribute_bytes))
        {
            result.win32_error = GetLastError();
            result.failure_message =
                "Failed to initialize offline query process attributes: "
                + Win32ErrorMessage(result.win32_error);
            HeapFree(GetProcessHeap(), 0, startup.lpAttributeList);
            startup.lpAttributeList = nullptr;
            cleanup_pipes();
            finish_duration();
            return result;
        }

        HANDLE inherited_handles[] = {null_input, stdout_write, stderr_write};
        if (!UpdateProcThreadAttribute(
                startup.lpAttributeList,
                0,
                PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                inherited_handles,
                sizeof(inherited_handles),
                nullptr,
                nullptr))
        {
            result.win32_error = GetLastError();
            result.failure_message =
                "Failed to restrict offline query inherited handles: "
                + Win32ErrorMessage(result.win32_error);
            cleanup_attributes();
            cleanup_pipes();
            finish_duration();
            return result;
        }

        PROCESS_INFORMATION process{};
        std::vector<wchar_t> mutable_command(
            command_line.begin(), command_line.end());
        mutable_command.push_back(L'\0');
        std::vector<wchar_t> environment =
            BuildEnvironment(trace_id, parent_span_id, parameter_types_json);

        const BOOL created = CreateProcessW(
            executable.c_str(),
            mutable_command.data(),
            nullptr,
            nullptr,
            TRUE,
            CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT | CREATE_SUSPENDED
                | EXTENDED_STARTUPINFO_PRESENT,
            environment.data(),
            nullptr,
            &startup.StartupInfo,
            &process);
        cleanup_attributes();
        if (!created)
        {
            result.win32_error = GetLastError();
            result.failure_message = "Failed to start offline query: "
                + Win32ErrorMessage(result.win32_error);
            cleanup_pipes();
            finish_duration();
            return result;
        }

        if (!ChildImageMatchesPinnedExecutable(
                process.hProcess,
                pinned_executable.get(),
                result.win32_error,
                result.failure_message))
        {
            TerminateProcess(process.hProcess, result.win32_error);
            WaitForSingleObject(process.hProcess, 5000);
            CloseHandle(process.hThread);
            CloseHandle(process.hProcess);
            cleanup_pipes();
            finish_duration();
            return result;
        }
        if (ResumeThread(process.hThread) == static_cast<DWORD>(-1))
        {
            result.win32_error = GetLastError();
            result.failure_message = "Could not resume verified offline query process: "
                + Win32ErrorMessage(result.win32_error);
            TerminateProcess(process.hProcess, result.win32_error);
            WaitForSingleObject(process.hProcess, 5000);
            CloseHandle(process.hThread);
            CloseHandle(process.hProcess);
            cleanup_pipes();
            finish_duration();
            return result;
        }

        result.launched = true;
        close_handle(stdout_write);
        close_handle(stderr_write);
        CloseHandle(null_input);
        null_input = INVALID_HANDLE_VALUE;

        const auto deadline = start_clock
            + std::chrono::milliseconds(QueryTimeoutMs);
        for (;;)
        {
            DrainPipe(
                stdout_read, result.stdout_text, result.stdout_truncated);
            DrainPipe(
                stderr_read, result.stderr_text, result.stderr_truncated);

            const DWORD wait_result = WaitForSingleObject(process.hProcess, 20);
            if (wait_result == WAIT_OBJECT_0)
                break;
            if (wait_result == WAIT_FAILED)
            {
                result.win32_error = GetLastError();
                result.failure_message = "Waiting for offline query failed: "
                    + Win32ErrorMessage(result.win32_error);
                TerminateProcess(process.hProcess, result.win32_error);
                WaitForSingleObject(process.hProcess, 5000);
                break;
            }
            if (std::chrono::steady_clock::now() >= deadline)
            {
                result.timed_out = true;
                result.failure_message = "Offline query timed out after "
                    + std::to_string(QueryTimeoutMs) + " ms.";
                TerminateProcess(process.hProcess, ERROR_TIMEOUT);
                WaitForSingleObject(process.hProcess, 5000);
                break;
            }
        }

        DrainPipe(stdout_read, result.stdout_text, result.stdout_truncated);
        DrainPipe(stderr_read, result.stderr_text, result.stderr_truncated);
        if (!GetExitCodeProcess(process.hProcess, &result.exit_code))
        {
            result.win32_error = GetLastError();
            if (result.failure_message.empty())
            {
                result.failure_message =
                    "Failed to read offline query exit code: "
                    + Win32ErrorMessage(result.win32_error);
            }
        }

        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
        cleanup_pipes();
        finish_duration();
        return result;
    }

    bool IsCompactEmpty(const json& value)
    {
        return value.is_null()
            || (value.is_string() && value.get_ref<const std::string&>().empty())
            || (value.is_array() && value.empty())
            || (value.is_object() && value.empty());
    }

    void ApplyResponseShaping(
        json& structured,
        const json& shaping,
        std::vector<std::string>& warnings)
    {
        if (!structured.is_object() || !shaping.is_object())
            return;

        std::set<std::string> fields;
        std::set<std::string> omit;
        auto fields_it = shaping.find("_fields");
        if (fields_it != shaping.end() && fields_it->is_array())
        {
            for (const json& item : *fields_it)
                fields.insert(item.get<std::string>());
        }
        auto omit_it = shaping.find("_omit");
        if (omit_it != shaping.end() && omit_it->is_array())
        {
            for (const json& item : *omit_it)
                omit.insert(item.get<std::string>());
        }

        const bool has_fields = !fields.empty();
        const bool has_omit = !omit.empty();
        if (has_fields && has_omit)
        {
            warnings.push_back(
                "`_fields` and `_omit` are mutually exclusive; honoring "
                "`_fields`, ignoring `_omit`.");
        }

        if (has_fields)
        {
            for (auto it = structured.begin(); it != structured.end();)
            {
                if (fields.count(it.key()) == 0)
                    it = structured.erase(it);
                else
                    ++it;
            }
        }
        else if (has_omit)
        {
            for (const std::string& key : omit)
                structured.erase(key);
        }

        const bool compact = shaping.value("_compact_json", false);
        if (compact)
        {
            for (auto it = structured.begin(); it != structured.end();)
            {
                if (IsCompactEmpty(it.value()))
                    it = structured.erase(it);
                else
                    ++it;
            }
        }
    }

    void AppendWarnings(json& structured, const std::vector<std::string>& warnings)
    {
        if (!structured.is_object() || warnings.empty())
            return;

        json existing = json::array();
        auto warnings_it = structured.find("warnings");
        if (warnings_it != structured.end() && warnings_it->is_array())
            existing = *warnings_it;
        for (const std::string& warning : warnings)
            existing.push_back(warning);
        structured["warnings"] = std::move(existing);
    }

    std::string MakeResponse(
        const json& id,
        const Invocation& invocation,
        const ProcessResult& process_result)
    {
        json process = {
            {"launched", process_result.launched},
            {"timed_out", process_result.timed_out},
            {"exit_code", process_result.exit_code},
            {"stdout_truncated", process_result.stdout_truncated},
            {"stderr_truncated", process_result.stderr_truncated},
        };
        if (process_result.win32_error != ERROR_SUCCESS)
            process["win32_error"] = process_result.win32_error;

        const json offline_metadata = {
            {"offline_fallback", true},
            {"routing_context", {
                {"decision_source", "offline_fallback"},
                {"backend", "monolith_query"},
                {"namespace", invocation.query_namespace},
                {"action", invocation.action},
            }},
            {"phase_timing", {
                {"query_ms", process_result.duration_ms},
                {"timeout_ms", QueryTimeoutMs},
            }},
            {"process", process},
        };

        json structured;
        bool parsed_json = false;
        if (!process_result.stdout_text.empty())
        {
            try
            {
                structured = json::parse(process_result.stdout_text);
                parsed_json = true;
            }
            catch (...) {}
        }
        if (!parsed_json || !structured.is_object())
        {
            json wrapped = json::object();
            if (parsed_json)
                wrapped["result"] = structured;
            else if (!process_result.stdout_text.empty())
                wrapped["output"] = process_result.stdout_text;
            structured = std::move(wrapped);
        }

        const size_t first_non_whitespace = process_result.stdout_text.find_first_not_of(
            " \t\r\n");
        const bool looks_like_json_object = first_non_whitespace != std::string::npos
            && process_result.stdout_text[first_non_whitespace] == '{';
        std::string output_contract_error;
        if (process_result.stdout_truncated)
        {
            output_contract_error =
                "Offline query stdout exceeded the 16 MiB capture limit.";
        }
        else if (process_result.stdout_text.empty()
            && process_result.launched
            && !process_result.timed_out
            && process_result.exit_code == 0
            && process_result.failure_message.empty())
        {
            output_contract_error = "Offline query produced empty stdout.";
        }
        else if (!parsed_json && looks_like_json_object)
        {
            output_contract_error =
                "Offline query stdout was not valid JSON.";
        }

        const bool payload_reports_failure = structured.contains("success")
            && structured["success"].is_boolean()
            && !structured["success"].get<bool>();
        bool payload_reports_error = false;
        auto error_it = structured.find("error");
        if (error_it != structured.end() && !error_it->is_null())
        {
            payload_reports_error = !error_it->is_string()
                || !error_it->get<std::string>().empty();
        }
        const bool is_error = !process_result.launched
            || process_result.timed_out
            || process_result.exit_code != 0
            || !process_result.failure_message.empty()
            || !output_contract_error.empty()
            || payload_reports_failure
            || payload_reports_error;
        if (is_error && !structured.contains("error"))
        {
            if (!process_result.failure_message.empty())
                structured["error"] = process_result.failure_message;
            else if (process_result.exit_code != 0
                && !process_result.stderr_text.empty())
            {
                structured["error"] = process_result.stderr_text;
            }
            else if (!output_contract_error.empty())
                structured["error"] = output_contract_error;
            else if (!process_result.stderr_text.empty())
                structured["error"] = process_result.stderr_text;
            else
                structured["error"] = "Offline query failed.";
        }
        if (is_error && !process_result.stderr_text.empty())
            structured["stderr"] = process_result.stderr_text;

        structured["_monolith"] = offline_metadata;
        if (!is_error)
        {
            std::vector<std::string> shaping_warnings;
            ApplyResponseShaping(
                structured, invocation.response_shaping, shaping_warnings);
            AppendWarnings(structured, shaping_warnings);
        }
        const json tool_result = {
            {"content", json::array({{
                {"type", "text"},
                {"text", structured.dump(2, ' ', false, json::error_handler_t::replace)},
            }})},
            {"structuredContent", structured},
            {"isError", is_error},
            {"_meta", {
                {"monolith", {
                    {"transport", "offline_fallback"},
                    {"backend", "monolith_query"},
                }},
            }},
        };
        const json response = {
            {"jsonrpc", "2.0"},
            {"id", id},
            {"result", tool_result},
        };
        return response.dump(-1, ' ', false, json::error_handler_t::replace);
    }

    fs::path ResolveOfflineCatalogPath()
    {
        const QueryBundle& bundle = GetQueryBundle();
        return bundle.valid ? bundle.catalog_path : fs::path{};
    }

    const std::set<std::string>& OfflineFastPathActions()
    {
        static std::once_flag once;
        static std::set<std::string> actions;
        std::call_once(once, []()
        {
            try
            {
                const fs::path catalog_path = ResolveOfflineCatalogPath();
                std::ifstream input(catalog_path, std::ios::binary);
                if (!input) return;
                json snapshot;
                input >> snapshot;
                auto rows = snapshot.find("actions");
                if (rows == snapshot.end() || !rows->is_array()) return;
                for (const json& row : *rows)
                {
                    if (!row.is_object()
                        || !row.value("available_offline", false)
                        || row.value("requires_live_editor", true)
                        || row.value("mutates_assets", true)
                        || row.value("long_running", true))
                    {
                        continue;
                    }
                    const std::string full_name = row.value("full_name", "");
                    if (!full_name.empty()) actions.insert(full_name);
                }
            }
            catch (...)
            {
                // Missing/invalid catalog is fail-closed: calls take the strong
                // live path and can still receive fallback guidance afterward.
            }
        });
        return actions;
    }

    const std::set<std::string>& ShortRepeatProtectionActions()
    {
        static std::once_flag once;
        static std::set<std::string> actions;
        std::call_once(once, []()
        {
            try
            {
                const fs::path catalog_path = ResolveOfflineCatalogPath();
                std::ifstream input(catalog_path, std::ios::binary);
                if (!input) return;
                json snapshot;
                input >> snapshot;
                auto rows = snapshot.find("actions");
                if (rows == snapshot.end() || !rows->is_array()) return;
                for (const json& row : *rows)
                {
                    if (!row.is_object()
                        || row.value("mutates_assets", true)
                        || row.value("long_running", true))
                    {
                        continue;
                    }
                    const std::string full_name = row.value("full_name", "");
                    if (!full_name.empty()) actions.insert(full_name);
                }
            }
            catch (...)
            {
                // Missing metadata leaves the set empty, so every invocation
                // receives strong repeat protection below.
            }
        });
        return actions;
    }

    bool InvocationCanUseOfflineFastPath(
        const std::string& tool_name,
        const json& arguments)
    {
        Invocation invocation;
        std::string error;
        if (!ResolveInvocation(tool_name, arguments, invocation, error)
            || !error.empty())
        {
            return false;
        }

        if (invocation.query_namespace == "monolith")
        {
            // A schema request is explicitly asking for the authoritative live
            // action contract.  The offline catalog can only return degraded
            // routing metadata, so a healthy editor must receive the normal
            // request budget instead of the short read-fallback probe budget.
            // Other discovery projections remain eligible for the fast path so
            // an unresponsive editor does not stall ordinary offline browsing.
            if (invocation.action == "discover")
            {
                const auto mode_it = invocation.params.find("mode");
                if (mode_it != invocation.params.end()
                    && mode_it->is_string()
                    && mode_it->get<std::string>() == "schema")
                {
                    return false;
                }
            }

            static const std::set<std::string> SafeCatalogActions = {
                "status",
                "discover",
                "find",
                "guide",
                "get_action_metadata_coverage",
            };
            return SafeCatalogActions.count(invocation.action) > 0;
        }
        return OfflineFastPathActions().count(
            invocation.query_namespace + "." + invocation.action) > 0;
    }
}

bool InitializeMonolithOfflineBundle(std::string& error)
{
    const QueryBundle& bundle = GetQueryBundle();
    error = bundle.error;
    return bundle.valid;
}

bool MonolithToolSupportsOfflineFallback(const std::string& tool_name)
{
    if (tool_name == "monolith_query")
        return true;
    const std::string query_suffix = "_query";
    if (EndsWith(tool_name, query_suffix))
    {
        const std::string query_namespace =
            tool_name.substr(0, tool_name.size() - query_suffix.size());
        return QueryNamespaces.count(query_namespace) > 0;
    }
    return MonolithTools.count(tool_name) > 0;
}

bool MonolithInvocationCanUseOfflineFastPath(
    const std::string& tool_name,
    const std::string& arguments_json)
{
    try
    {
        const json arguments = json::parse(arguments_json);
        return arguments.is_object()
            && InvocationCanUseOfflineFastPath(tool_name, arguments);
    }
    catch (...)
    {
        return false;
    }
}

bool MonolithInvocationRequiresStrongRepeatProtection(
    const std::string& tool_name,
    const std::string& arguments_json)
{
    try
    {
        const json arguments = json::parse(arguments_json);
        Invocation invocation;
        std::string error;
        if (!arguments.is_object()
            || !ResolveInvocation(tool_name, arguments, invocation, error)
            || !error.empty())
        {
            return true;
        }
        return ShortRepeatProtectionActions().count(
            invocation.query_namespace + "." + invocation.action) == 0;
    }
    catch (...)
    {
        return true;
    }
}

MonolithOfflineFallbackResult TryMonolithOfflineFallback(
    bool enabled,
    const std::string& tool_name,
    const std::string& arguments_json,
    const std::string& id_json,
    const std::string& trace_id,
    const std::string& parent_span_id)
{
    MonolithOfflineFallbackResult result;
    if (!enabled)
        return result;

    json arguments;
    json id;
    try
    {
        arguments = json::parse(arguments_json);
        id = json::parse(id_json);
    }
    catch (...)
    {
        return result;
    }

    Invocation invocation;
    std::string error;
    if (!ResolveInvocation(tool_name, arguments, invocation, error))
        return result;

    result.handled = true;
    result.backend = "monolith_query";
    result.query_namespace = invocation.query_namespace;
    result.action = invocation.action;

    ProcessResult process_result;
    if (!error.empty())
    {
        process_result.win32_error = ERROR_INVALID_PARAMETER;
        process_result.failure_message = error;
    }
    else
    {
        std::vector<std::string> cli_arguments;
        std::string parameter_types_json;
        if (!BuildCliArguments(
                invocation, cli_arguments, parameter_types_json, error))
        {
            process_result.win32_error = ERROR_INVALID_PARAMETER;
            process_result.failure_message = error;
        }
        else
        {
            try
            {
                if (invocation.query_namespace == "monolith")
                {
                    const fs::path catalog_path = ResolveOfflineCatalogPath();
                    if (catalog_path.empty())
                    {
                        throw std::runtime_error(
                            "Immutable offline catalog bundle is unavailable.");
                    }
                    cli_arguments.push_back(
                        "--snapshot=" + catalog_path.u8string());
                    json parameter_types = json::parse(parameter_types_json);
                    parameter_types["snapshot"] = "string";
                    parameter_types_json = parameter_types.dump(-1);
                }
                process_result = RunQuery(
                    cli_arguments, trace_id, parent_span_id,
                    parameter_types_json);
            }
            catch (const std::exception& exception)
            {
                process_result.win32_error = ERROR_UNHANDLED_EXCEPTION;
                process_result.failure_message =
                    "Offline query fallback failed before completion: "
                    + std::string(exception.what());
            }
            catch (...)
            {
                process_result.win32_error = ERROR_UNHANDLED_EXCEPTION;
                process_result.failure_message =
                    "Offline query fallback failed before completion.";
            }
        }
    }

    result.duration_ms = process_result.duration_ms;
    try
    {
        result.response = MakeResponse(id, invocation, process_result);
    }
    catch (const std::exception& error)
    {
        ProcessResult safe_error = process_result;
        safe_error.stdout_text.clear();
        safe_error.stderr_text.clear();
        safe_error.failure_message =
            std::string("Offline query response serialization failed: ") + error.what();
        safe_error.win32_error = ERROR_UNHANDLED_EXCEPTION;
        result.response = MakeResponse(id, invocation, safe_error);
    }
    return result;
}
