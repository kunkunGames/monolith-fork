#pragma once

static bool is_proxy_help_arg(const std::string& arg)
{
    return arg == "--help" || arg == "-h" || arg == "help";
}

static bool is_proxy_version_arg(const std::string& arg)
{
    return arg == "--version" || arg == "-v" || arg == "version";
}

static void print_proxy_help()
{
    std::cout
        << "Usage:\n"
        << "  monolith_proxy --help\n"
        << "  monolith_proxy --version\n"
        << "  monolith_proxy\n\n"
        << "Role:\n"
        << "  Stdio-to-HTTP MCP bridge for the editor-hosted Monolith server.\n"
        << "  Default target: MONOLITH_URL=http://localhost:9316/mcp\n\n"
        << "Environment:\n"
        << "  MONOLITH_URL                         Editor MCP endpoint.\n"
        << "  MONOLITH_SPLIT_EDITOR_QUERY          Set 1 to split editor/query routing.\n"
        << "  MONOLITH_EDITOR_ACTION_ALLOWLIST     CSV allowlist for editor actions.\n"
        << "  MONOLITH_EDITOR_ACTION_DENYLIST      CSV denylist for editor actions.\n"
        << "  MONOLITH_TOOL_LOG_ENABLED            Set 0 to disable daily proxy logs.\n"
        << "  MONOLITH_TOOL_LOG_DIR                Redirect Logs/yyyyMMdd/proxy.jsonl.\n"
        << "  MONOLITH_TOOL_LOG_MAX_FIELD_BYTES    Bound captured log fields.\n"
        << "  MONOLITH_CALL_LOG                    Native C++ proxy call log toggle; 0 disables.\n"
        << "  MONOLITH_PROJECT_ROOT                Native C++ call-log project root override.\n\n"
        << "MCP config example:\n"
        << "  {\"mcpServers\":{\"monolith\":{\"command\":\"<project-root>/Plugins/Monolith/Binaries/monolith_proxy.exe\"}}}\n\n"
        << "Offline fallback:\n"
        << "  Use Binaries/monolith_query (or .exe on Windows) for read-only source/project/bridge queries when the editor or MCP server is unavailable.\n";
}

static void print_proxy_version()
{
    json out = {
        {"tool", PROXY_NAME},
        {"version", PROXY_VERSION},
        {"runtime", "native-cpp"}
    };
    std::cout << out.dump(2) << std::endl;
}
