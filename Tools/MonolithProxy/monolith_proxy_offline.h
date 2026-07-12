#pragma once

#include <string>

struct MonolithOfflineFallbackResult
{
    bool handled = false;
    std::string response;
    double duration_ms = 0.0;
    std::string backend;
    std::string query_namespace;
    std::string action;
};

bool MonolithToolSupportsOfflineFallback(const std::string& tool_name);

// True only when the concrete namespace/action is a bounded read that the
// bundled offline catalog can execute. This is intentionally narrower than
// MonolithToolSupportsOfflineFallback: live-only/long-running actions may still
// receive offline recovery guidance, but must never use the short live timeout.
bool MonolithInvocationCanUseOfflineFastPath(
    const std::string& tool_name,
    const std::string& arguments_json);

// Mutating or long-running catalogued actions keep a longer completed-call
// replay guard because a timed-out HTTP request may still be executing in UE.
bool MonolithInvocationRequiresStrongRepeatProtection(
    const std::string& tool_name,
    const std::string& arguments_json);

// Load and freeze the immutable Query/catalog bundle selected at proxy startup.
// Environment overrides are also frozen once and remain subject to regular-file
// and reparse-point checks.
bool InitializeMonolithOfflineBundle(std::string& error);

MonolithOfflineFallbackResult TryMonolithOfflineFallback(
    bool enabled,
    const std::string& tool_name,
    const std::string& arguments_json,
    const std::string& id_json,
    const std::string& trace_id,
    const std::string& parent_span_id);
