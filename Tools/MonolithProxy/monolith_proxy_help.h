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
        << "  Long-lived native MCP control plane with no Unreal Engine DLL dependency.\n"
        << "  Calls use the editor HTTP endpoint first. If its transport is unavailable,\n"
        << "  four stable tools remain advertised: monolith_query, monolith_discover,\n"
        << "  monolith_status, and monolith_find. Fixed read-only query actions run\n"
        << "  through the sibling monolith_query executable with forced --readonly.\n"
        << "  Default live target: MONOLITH_URL=http://localhost:9316/mcp\n\n"
        << "Environment:\n"
        << "  MONOLITH_URL                         Editor MCP endpoint.\n"
        << "  MONOLITH_OFFLINE_FALLBACK            Set 0 to disable native offline routing.\n"
        << "  MONOLITH_QUERY_EXE                   Override the offline query executable path.\n"
        << "  MONOLITH_EXPECTED_PROJECT_ROOT       Expected host project identity; inferred from the deployed binary by default.\n"
        << "  MONOLITH_SPLIT_EDITOR_QUERY          Set 1 to split editor/query routing.\n"
        << "  MONOLITH_EDITOR_ACTION_ALLOWLIST     CSV allowlist for editor actions.\n"
        << "  MONOLITH_EDITOR_ACTION_DENYLIST      CSV denylist for editor actions.\n"
        << "  MONOLITH_TOOL_LOG_ENABLED            Set 0 to disable daily proxy logs.\n"
        << "  MONOLITH_TOOL_LOG_DIR                Redirect Logs/yyyyMMdd/proxy.jsonl.\n"
        << "  MONOLITH_TOOL_LOG_MAX_FIELD_BYTES    Bound captured log fields.\n"
        << "  MONOLITH_CALL_LOG                    Native C++ proxy call log toggle; 0 disables.\n"
        << "  MONOLITH_PROJECT_ROOT                Native C++ call-log project root override.\n\n"
        << "MCP deployment:\n"
        << "  Run Scripts/onboard_monolith.ps1. It validates Binaries/monolith_proxy.current.json\n"
        << "  and registers the exact immutable monolith_proxy-<source-hash>.exe path. The\n"
        << "  historical monolith_proxy.exe path is compatibility-only and may remain stale\n"
        << "  while older MCP sessions hold its Windows image lock.\n\n"
        << "Offline fallback:\n"
        << "  Enabled by default for a fixed namespace/tool allowlist. Unsupported editor,\n"
        << "  UObject, loaded-asset lifecycle/mutation, world, Slate, and PIE calls remain live-only.\n"
        << "  Child processes use direct CreateProcessW execution, bounded output/time,\n"
        << "  a non-reparse executable held against replacement and re-verified by file\n"
        << "  identity before its suspended process resumes, restricted inherited handles, and an\n"
        << "  immutable trailing --readonly flag. Plain HTTP is loopback-only; a live\n"
        << "  endpoint is accepted only after health and project identity validation.\n";
}

static void print_proxy_version()
{
    json out = {
        {"tool", PROXY_NAME},
        {"version", PROXY_VERSION},
        {"runtime", "native-cpp"},
        {"source_hash", SOURCE_HASH}
    };
    std::cout << out.dump(2) << std::endl;
}
