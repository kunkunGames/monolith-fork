#pragma once

#include "CoreMinimal.h"
#include "MonolithToolRegistry.h"

/**
 * Core discovery and status tool implementations.
 * These are registered under the "monolith" namespace.
 */
class FMonolithCoreTools
{
public:
	/** Register all core tools with the registry */
	static void RegisterAll();

	// --- Action Handlers ---

	/** monolith_find — Fuzzy task-to-action search across the live registry */
	static FMonolithActionResult HandleFind(const TSharedPtr<FJsonObject>& Params);

	/** monolith_discover — List available namespaces and their actions */
	static FMonolithActionResult HandleDiscover(const TSharedPtr<FJsonObject>& Params);

	/** monolith_status — Server health, version, index status */
	static FMonolithActionResult HandleStatus(const TSharedPtr<FJsonObject>& Params);

	/** monolith_update — Check or install updates */
	static FMonolithActionResult HandleUpdate(const TSharedPtr<FJsonObject>& Params);

	/** monolith_reindex — Trigger full project re-index */
	static FMonolithActionResult HandleReindex(const TSharedPtr<FJsonObject>& Params);

	/** monolith_get_mcp_server_status — Return MCP transport/server status */
	static FMonolithActionResult HandleGetMcpServerStatus(const TSharedPtr<FJsonObject>& Params);

	/** monolith_list_mcp_sessions — Report MCP session tracking availability */
	static FMonolithActionResult HandleListMcpSessions(const TSharedPtr<FJsonObject>& Params);

	/** monolith_terminate_mcp_session — Report MCP session termination availability */
	static FMonolithActionResult HandleTerminateMcpSession(const TSharedPtr<FJsonObject>& Params);

	/** monolith_set_mcp_compatibility_options — Set safe MCP compatibility options */
	static FMonolithActionResult HandleSetMcpCompatibilityOptions(const TSharedPtr<FJsonObject>& Params);

	/** monolith_get_mcp_discovery_state — Return current discovery snapshot status */
	static FMonolithActionResult HandleGetMcpDiscoveryState(const TSharedPtr<FJsonObject>& Params);

	/** monolith_list_domains — Return cheap profile-filtered domain metadata */
	static FMonolithActionResult HandleListDomains(const TSharedPtr<FJsonObject>& Params);

	/** monolith_describe_domain — Return one domain's profile-filtered actions and schemas */
	static FMonolithActionResult HandleDescribeDomain(const TSharedPtr<FJsonObject>& Params);

	/** monolith_load_domain — Mark a domain loaded for metadata/discovery scope without changing tools/list */
	static FMonolithActionResult HandleLoadDomain(const TSharedPtr<FJsonObject>& Params);

	/** monolith_get_loaded_domains — Return process/profile-scoped loaded domain state */
	static FMonolithActionResult HandleGetLoadedDomains(const TSharedPtr<FJsonObject>& Params);

	/** monolith_get_onboarding_state — Return local onboarding progress */
	static FMonolithActionResult HandleGetOnboardingState(const TSharedPtr<FJsonObject>& Params);

	/** monolith_set_onboarding_state — Complete, skip, reopen, or reset onboarding steps */
	static FMonolithActionResult HandleSetOnboardingState(const TSharedPtr<FJsonObject>& Params);

	/** monolith_get_readiness_status — Read-only Monolith readiness checks */
	static FMonolithActionResult HandleGetReadinessStatus(const TSharedPtr<FJsonObject>& Params);

	/** monolith_get_readiness_help — Help text for readiness components */
	static FMonolithActionResult HandleGetReadinessHelp(const TSharedPtr<FJsonObject>& Params);

	/** monolith_get_notification_settings — Return local notification preferences */
	static FMonolithActionResult HandleGetNotificationSettings(const TSharedPtr<FJsonObject>& Params);

	/** monolith_set_notification_settings — Persist local notification preferences */
	static FMonolithActionResult HandleSetNotificationSettings(const TSharedPtr<FJsonObject>& Params);

	/** monolith_test_notification — Trigger a harmless notification test */
	static FMonolithActionResult HandleTestNotification(const TSharedPtr<FJsonObject>& Params);
};
